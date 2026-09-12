#include "usdGen/cudaExecutionQueue.h"
#include "usdGen/gpu/generation.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

using namespace usdGen;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (false)

static UsdGenGraphDesc Desc(float translation = 1.f) {
    UsdGenGraphDesc d;
    d.description = SdfPath("/Groom/Hair"); d.executionBackend = UsdGenExecutionBackend::Cuda;
    UsdGenSurfaceDesc s; s.path = SdfPath("/Scalp");
    s.restPoints = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    s.points = s.restPoints; for (auto& p : s.points) p[0] += translation;
    s.faceVertexCounts = {3,3,3}; s.faceVertexIndices = {0,1,2, 0,1,3, 1,2,4};
    d.surfaces.push_back(s);
    UsdGenCurveSetDesc c; c.path = SdfPath("/Curves"); c.role = UsdGenRole::Curves;
    c.curveRole = TfToken("hair"); c.curveVertexCounts = {2};
    c.points = {{.2f,.2f,0},{.2f,.4f,0}}; c.rest = c.points;
    c.curveId = {42}; c.skinPrim = {0}; c.skinPrimUv = {{.2f,.2f}};
    d.curveSets.push_back(c);
    UsdGenNodeDesc source; source.path = SdfPath("/Groom/Hair/Ops/Source");
    source.type = TfToken("UsdGenCurveSource"); source.curves = {c.path}; source.surfaces = {s.path};
    UsdGenNodeDesc deform; deform.path = SdfPath("/Groom/Hair/Ops/Deform");
    deform.type = TfToken("UsdGenDeform"); deform.inputs = {source.path}; deform.surfaces = {s.path};
    deform.mode = TfToken("rbf"); deform.readPhase = TfToken("final");
    deform.params.push_back({TfToken("rbfSamples"), VtValue(5), false});
    UsdGenExpressionDesc e; e.path = SdfPath("/Blend"); e.source = "$frame == 1 ? 0.25 : 1";
    const expr::ValueShape shape{expr::ScalarType::Float32,1,1,1,1,false};
    e.outputs.push_back({TfToken("result"), TfToken("float"), shape}); d.expressions.push_back(e);
    UsdGenExpressionBinding b; b.expression = e.path; b.destination = TfToken("blend");
    b.domain = expr::Domain::Point; b.nativeType = TfToken("float"); b.destinationShape = shape;
    b.literal = VtValue(1.f); deform.expressionBindings.push_back(b);
    d.nodes = {source,deform}; d.terminal = deform.path;
    return d;
}

static bool Root(std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
                 float x, cudaStream_t stream) {
    auto lease = gpu::AcquireGeometry(generation,stream);
    if (!lease || lease.Geometry().pointCount != 2) return false;
    float3 point{};
    if (cudaMemcpyAsync(&point,lease.Geometry().points.data,sizeof(point),cudaMemcpyDeviceToHost,stream) != cudaSuccess)
        return false;
    return cudaStreamSynchronize(stream) == cudaSuccess && std::fabs(point.x-x) < 2e-3f &&
        std::fabs(point.y-.2f) < 2e-3f && std::fabs(point.z) < 2e-3f;
}

int main() {
    UsdGenDiagnostics diagnostics;
    auto desc = Desc(); auto plan = CompileCudaGraph(desc,&diagnostics);
    CHECK(plan && !diagnostics.HasErrors());
    // Plan owns the authored snapshot; later edits to the caller's descriptor
    // cannot change its literal/pose inputs or compiled expression programs.
    desc.surfaces[0].points[0][0] = 100;
    desc.expressions[0].source = "0";
    UsdGenExecutionRuntime runtime(4);
    cudaStream_t reader = nullptr;
    CHECK(cudaStreamCreateWithFlags(&reader,cudaStreamNonBlocking) == cudaSuccess);
    std::shared_ptr<const UsdGenCudaQueueSnapshot> retained;
    {
        UsdGenCudaExecutionQueue a(runtime,-1,&diagnostics), b(runtime,-1,&diagnostics);
        CHECK(a.Valid() && b.Valid() && !a.Snapshot() && !b.Snapshot());
        std::atomic<int> completed{0}, failed{0};
        auto completion = [&](uint64_t, UsdGenExecutionPipeline::Outcome outcome, UsdGenDiagnostics const& d) {
            ++completed; if (outcome == UsdGenExecutionPipeline::Outcome::Failed || d.HasErrors()) ++failed;
        };
        auto firstA = a.Submit(plan,1,completion), firstB = b.Submit(plan,2,completion);
        CHECK(firstA && firstB);
        a.Drain(); b.Drain();
        auto a1 = a.Snapshot(), b1 = b.Snapshot();
        CHECK(completed == 2 && failed == 0 && a1 && b1);
        CHECK(a1->epoch == firstA && b1->epoch == firstB);
        CHECK(Root(a1->generation,.45f,reader) && Root(b1->generation,1.2f,reader));
        CHECK(a1->bindings.size() == 1 && b1->bindings.size() == 1 &&
            a1->bindings[0].identity != b1->bindings[0].identity &&
            a1->bindings[0].bindCount == 1 && b1->bindings[0].bindCount == 1);
        retained = a1;

        auto posed = CompileCudaGraph(Desc(2),&diagnostics); CHECK(posed);
        CHECK(a.Submit(posed,2,completion)); a.Drain();
        auto a2 = a.Snapshot(); CHECK(a2 != a1 && Root(a2->generation,2.2f,reader));
        CHECK(a2->bindings[0].identity == a1->bindings[0].identity &&
            a2->bindings[0].bindCount == 1 && a2->bindings[0].solveCount == 2);
        CHECK(Root(a1->generation,.45f,reader) && Root(b1->generation,1.2f,reader));

        // Execution failure cannot replace the last good published snapshot.
        bool rejected = false;
        CHECK(!a.Submit(posed,std::numeric_limits<double>::quiet_NaN()));
        auto invalidValues = Desc();
        invalidValues.expressions[0].source = "$P[0] / ($P[0] - $P[0])";
        auto invalidPlan = CompileCudaGraph(invalidValues,&diagnostics); CHECK(invalidPlan);
        CHECK(a.Submit(invalidPlan,2,
            [&](uint64_t, auto outcome, auto const& d) {
                rejected = outcome == UsdGenExecutionPipeline::Outcome::Failed && d.HasErrors();
            }));
        a.Drain(); CHECK(rejected && a.Snapshot() == a2);
        CHECK(a.Submit(plan,2,completion)); a.Drain();
        CHECK(Root(a.Snapshot()->generation,1.2f,reader) &&
            a.Snapshot()->bindings[0].identity == a1->bindings[0].identity);

        // Multiple external producers enqueue immutable requests. The final
        // accepted request wins without a per-plan or per-cache mutex.
        std::vector<std::thread> producers;
        for (int thread=0;thread<4;++thread) producers.emplace_back([&] {
            for (int i=0;i<4;++i) a.Submit(plan,2,completion);
        });
        for (auto& thread : producers) thread.join();
        auto latest = a.Submit(posed,3,completion); CHECK(latest);
        a.Drain(); CHECK(a.Snapshot()->epoch == latest && Root(a.Snapshot()->generation,2.2f,reader));
        CHECK(completed == 21 && failed == 0);
        auto snapshot = a.Snapshot(); CHECK(a.CancelPending()); a.Drain();
        CHECK(a.Snapshot() == snapshot);

        // Completion callbacks may enqueue another cook, never wait on the
        // owner task. Request reports remain paired with their generations.
        CHECK(b.Submit(plan,1,[&](uint64_t, auto outcome, auto const&) {
            if (outcome == UsdGenExecutionPipeline::Outcome::Published)
                b.Submit(posed,2,completion);
        }));
        b.Drain(); CHECK(Root(b.Snapshot()->generation,2.2f,reader));
    }
    // Shutdown retires accepted/queued requests while their callbacks and
    // publication storage remain alive. No external Drain is needed here.
    std::atomic<int> retired{0};
    {
        UsdGenCudaExecutionQueue closing(runtime,-1,&diagnostics); CHECK(closing.Valid());
        for (int i=0;i<32;++i)
            CHECK(closing.Submit(plan,2,[&](uint64_t, auto, auto const&) { ++retired; }));
    }
    CHECK(retired == 32);
    plan.reset();
    CHECK(Root(retained->generation,.45f,reader)); // queue/workspace/plan gone
    retained.reset();
    CHECK(cudaStreamDestroy(reader) == cudaSuccess);
    return 0;
}
