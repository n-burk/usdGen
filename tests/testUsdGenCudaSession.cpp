#include "usdGen/session.h"
#include "usdGen/gpu/generation.h"

#include <cstdio>
#include <vector>

using namespace usdGen;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (false)

static UsdGenGraphDesc SourceDesc() {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/Groom/Description");
    desc.executionBackend = UsdGenExecutionBackend::Cuda;
    desc.defaultWidth = .025f;
    UsdGenNodeDesc node;
    node.path = desc.description.AppendChild(TfToken("Ops")).AppendChild(TfToken("Source"));
    node.type = TfToken("UsdGenCurveSource");
    node.curves = {SdfPath("/Source")};
    node.surfaces = {SdfPath("/Scalp")};
    UsdGenSurfaceDesc scalp;
    scalp.path = SdfPath("/Scalp");
    scalp.faceVertexCounts = VtIntArray(9,3);
    scalp.faceVertexIndices = VtIntArray(27,0);
    scalp.restPoints = {GfVec3f(0),GfVec3f(1,0,0),GfVec3f(0,1,0)};
    scalp.points = scalp.restPoints;
    for (size_t i = 0; i < 27; ++i) scalp.faceVertexIndices[i] = static_cast<int>(i % 3);
    desc.surfaces.push_back(scalp);
    desc.terminal = node.path;
    desc.nodes.push_back(node);
    UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/Source"); curves.role = UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.curveVertexCounts = {2,3};
    curves.points = {GfVec3f(10,0,0),GfVec3f(11,0,0),GfVec3f(20,0,0),GfVec3f(21,0,0),GfVec3f(24,0,0)};
    curves.rest = curves.points;
    curves.curveId = {91,37};
    curves.skinPrim = {4,8}; curves.skinPrimUv = {GfVec2f(.1f,.2f),GfVec2f(.3f,.4f)};
    desc.curveSets.push_back(curves);
    return desc;
}

int main() {
    cudaStream_t consumer = nullptr;
    CHECK(cudaStreamCreateWithFlags(&consumer, cudaStreamNonBlocking) == cudaSuccess);
    gpu::CudaGeometryLease retained;
    std::weak_ptr<const UsdGenDeviceOwner> oldOwner;
    {
        UsdGenSession session;
        auto desc = SourceDesc();
        session.SetGraphDesc(desc);
        CHECK(!session.Commit(1, UsdGenCommitReason::SetTime));
        CHECK(session.LastDiagnostics().HasErrors() && session.NeedsCommit());
        session.SetDevicePublicationEnabled(true);
        auto first = session.Commit(1, UsdGenCommitReason::SetTime);
        if (!first) for (auto const& error : session.LastDiagnostics().errors)
            std::fprintf(stderr, "%s\n", error.c_str());
        CHECK(first && first->device && first->id == 0 && first->frame == 1);
        CHECK(!first->device->Geometry().alreadyDeformed);
        CHECK(first->tiles.empty() && first->guides.empty() && first->instancers.empty());
        CHECK(!session.LastDiagnostics().HasErrors() && !session.LastDiagnostics().warnings.empty());
        CHECK(session.Graph().Desc().executionBackend == UsdGenExecutionBackend::Cuda);
        retained = gpu::AcquireGeometry(first->device, consumer);
        CHECK(retained && retained.Geometry().pointCount == 5);
        oldOwner = first->device->Owner();
        auto copy = retained;
        retained = {};
        CHECK(copy && !oldOwner.expired());
        retained = std::move(copy);
        CHECK(!copy && copy.Geometry().points.data == nullptr);
        std::vector<float3> points(5);
        std::vector<float> widths(5);
        std::vector<uint32_t> offsets(3);
        std::vector<uint64_t> ids(2);
        auto view = retained.Geometry();
        CHECK(cudaMemcpyAsync(points.data(),view.points.data,5*sizeof(float3),cudaMemcpyDeviceToHost,consumer) == cudaSuccess);
        CHECK(cudaMemcpyAsync(widths.data(),view.widths.data,5*sizeof(float),cudaMemcpyDeviceToHost,consumer) == cudaSuccess);
        CHECK(cudaMemcpyAsync(offsets.data(),view.curveOffsets.data,3*sizeof(uint32_t),cudaMemcpyDeviceToHost,consumer) == cudaSuccess);
        CHECK(cudaMemcpyAsync(ids.data(),view.stableIds.data,2*sizeof(uint64_t),cudaMemcpyDeviceToHost,consumer) == cudaSuccess);
        CHECK(cudaStreamSynchronize(consumer) == cudaSuccess);
        CHECK(points[0].x == 20 && points[2].x == 24 && points[3].x == 10);
        CHECK(widths == std::vector<float>(5,.025f));
        CHECK(offsets == std::vector<uint32_t>({0,3,5}) && ids == std::vector<uint64_t>({37,91}));

        desc.curveSets[0].points[2] = GfVec3f(50,0,0);
        session.SetGraphDesc(desc);
        auto second = session.Commit(2, UsdGenCommitReason::SetTime);
        CHECK(second && second->device && second->id == 1 && second->device != first->device);
        first.reset();
        CHECK(!oldOwner.expired());
        // A new source allocation cannot mutate a retained prior generation.
        CHECK(cudaMemcpyAsync(points.data(),view.points.data,5*sizeof(float3),cudaMemcpyDeviceToHost,consumer) == cudaSuccess);
        CHECK(cudaStreamSynchronize(consumer) == cudaSuccess && points[0].x == 20);
        {
            auto next = gpu::AcquireGeometry(second->device, consumer);
            CHECK(next && next.Geometry().points.data != view.points.data);
            CHECK(cudaMemcpyAsync(points.data(),next.Geometry().points.data,5*sizeof(float3),cudaMemcpyDeviceToHost,consumer) == cudaSuccess);
            CHECK(cudaStreamSynchronize(consumer) == cudaSuccess && points[0].x == 50);
        }
        auto malformed = desc;
        malformed.curveSets[0].skinPrim[0] = 900;
        session.SetGraphDesc(malformed);
        CHECK(session.Commit(3, UsdGenCommitReason::SetTime) == second);
        CHECK(session.LastDiagnostics().HasErrors());
        malformed = desc;
        malformed.curveSets[0].restFromCurrentPoints = true;
        session.SetGraphDesc(malformed);
        CHECK(session.Commit(3, UsdGenCommitReason::SetTime) == second);
        CHECK(session.LastDiagnostics().HasErrors());
        malformed = desc;
        malformed.curveSets[0].points.pop_back();
        session.SetGraphDesc(malformed);
        CHECK(session.Commit(3, UsdGenCommitReason::SetTime) == second);
        CHECK(session.LastDiagnostics().HasErrors());
        CHECK(session.Commit(4, UsdGenCommitReason::SetTime) == second);
        auto linear = desc;
        linear.curveSets[0].type = TfToken("linear");
        session.SetGraphDesc(linear);
        CHECK(session.Commit(4, UsdGenCommitReason::SetTime) == second);
        CHECK(session.LastDiagnostics().HasErrors());
        auto varying = desc;
        varying.curveSets[0].widths = {.2f};
        varying.curveSets[0].widthsInterpolation = TfToken("varying");
        session.SetGraphDesc(varying);
        CHECK(session.Commit(4, UsdGenCommitReason::SetTime) == second);
        CHECK(session.LastDiagnostics().HasErrors());
        auto unsupported = desc;
        unsupported.nodes[0].params.push_back({TfToken("useRest"), VtValue(std::string("false")), false});
        session.SetGraphDesc(unsupported);
        CHECK(session.Commit(5, UsdGenCommitReason::SetTime) == second);
        CHECK(session.LastDiagnostics().HasErrors());
        unsupported = desc;
        unsupported.nodes[0].type = TfToken("UsdGenNoise");
        session.SetGraphDesc(unsupported);
        CHECK(session.Commit(5, UsdGenCommitReason::SetTime) == second);
        CHECK(session.LastDiagnostics().HasErrors());
        session.SetGraphDesc(desc);
        session.SetDevicePublicationEnabled(false);
        CHECK(session.Commit(6, UsdGenCommitReason::SetTime) == second);
        CHECK(session.LastDiagnostics().HasErrors());
        auto cache = desc;
        cache.nodes[0].params.push_back({TfToken("useRest"), VtValue(false), false});
        cache.curveSets[0].rest.clear();
        session.SetGraphDesc(cache);
        session.SetDevicePublicationEnabled(true);
        auto cacheGeneration = session.Commit(7, UsdGenCommitReason::SetTime);
        CHECK(cacheGeneration && cacheGeneration->id == 2 &&
              cacheGeneration->device->Geometry().alreadyDeformed);
        auto empty = desc;
        auto& emptyCurves = empty.curveSets[0];
        emptyCurves.curveVertexCounts.clear(); emptyCurves.points.clear();
        emptyCurves.rest.clear(); emptyCurves.curveId.clear();
        emptyCurves.skinPrim.clear(); emptyCurves.skinPrimUv.clear();
        session.SetGraphDesc(empty);
        auto emptyGeneration = session.Commit(8, UsdGenCommitReason::SetTime);
        CHECK(emptyGeneration && emptyGeneration->id == 3 && emptyGeneration->device);
        auto emptyLease = gpu::AcquireGeometry(emptyGeneration->device, consumer);
        CHECK(emptyLease && emptyLease.Geometry().curveCount == 0 &&
              emptyLease.Geometry().pointCount == 0 && emptyLease.Geometry().curveOffsets.size == 1);
    }
    CHECK(retained && !oldOwner.expired());
    retained = {};
    CHECK(oldOwner.expired());
    CHECK(cudaStreamDestroy(consumer) == cudaSuccess);
    std::puts("testUsdGenCudaSession: PASS");
    return 0;
}
