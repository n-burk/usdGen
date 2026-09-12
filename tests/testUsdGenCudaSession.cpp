#include "usdGen/session.h"
#include "usdGen/gpu/generation.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
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

static expr::ValueShape ScalarShape(expr::ScalarType scalar) {
    return {scalar, 1, 1, 1, 1, false};
}

static void AddCudaExpression(UsdGenGraphDesc* desc, char const* path,
                              char const* source, bool boolean = false) {
    UsdGenExpressionDesc expression;
    expression.path = SdfPath(path);
    expression.source = source;
    expression.outputs.push_back({TfToken("result"),
                                  TfToken(boolean ? "bool" : "float"),
                                  ScalarShape(boolean ? expr::ScalarType::Bool
                                                      : expr::ScalarType::Float32)});
    desc->expressions.push_back(std::move(expression));
}

static void BindCudaExpression(UsdGenNodeDesc* node, char const* expression,
                               char const* destination, expr::Domain domain,
                               bool boolean, VtValue literal) {
    UsdGenExpressionBinding binding;
    binding.expression = SdfPath(expression);
    binding.destination = TfToken(destination);
    binding.nativeType = TfToken(boolean ? "bool" : "float");
    binding.destinationShape = ScalarShape(boolean ? expr::ScalarType::Bool
                                                   : expr::ScalarType::Float32);
    binding.domain = domain;
    binding.literal = std::move(literal);
    node->expressionBindings.push_back(std::move(binding));
}

static UsdGenGraphDesc WidthChainDesc() {
    auto desc = SourceDesc();
    desc.timeCodesPerSecond = 24.0;

    UsdGenNodeDesc first;
    first.path = desc.description.AppendChild(TfToken("Ops")).AppendChild(TfToken("WidthLiteral"));
    first.type = TfToken("UsdGenWidth");
    first.inputs.push_back(desc.nodes.front().path);
    first.params.push_back({TfToken("width"), VtValue(0.1f), false});

    UsdGenNodeDesc second;
    second.path = desc.description.AppendChild(TfToken("Ops")).AppendChild(TfToken("WidthExpr"));
    second.type = TfToken("UsdGenWidth");
    second.inputs.push_back(first.path);
    second.params.push_back({TfToken("width"), VtValue(0.5f), false});
    desc.terminal = second.path;
    desc.nodes.push_back(std::move(first));
    desc.nodes.push_back(std::move(second));

    // The point field deliberately uses both commit frame and seconds.  The
    // usdGen: prefix is part of the authored transport contract and is
    // normalized by the CUDA executor before it reaches the primitive API.
    AddCudaExpression(&desc, "/Groom/Description/Expressions/pointWidth",
                      "$frame * 0.1 + $time");
    AddCudaExpression(&desc, "/Groom/Description/Expressions/replaceFirst",
                      "$primIndex == 0", true);
    AddCudaExpression(&desc, "/Groom/Description/Expressions/enabled",
                      "$frame < 3", true);
    BindCudaExpression(&desc.nodes[2],
                       "/Groom/Description/Expressions/pointWidth",
                       "usdGen:width", expr::Domain::Point, false, VtValue(0.5f));
    BindCudaExpression(&desc.nodes[2],
                       "/Groom/Description/Expressions/replaceFirst",
                       "replace", expr::Domain::Primitive, true, VtValue(false));
    BindCudaExpression(&desc.nodes[2],
                       "/Groom/Description/Expressions/enabled",
                       "enabled", expr::Domain::Groom, true, VtValue(true));
    return desc;
}

struct AsyncCommitResult {
    std::atomic<bool> done{false};
    bool accepted = false;
    UsdGenSession::SnapshotPtr snapshot;
    UsdGenExecutionPipeline::Outcome outcome =
        UsdGenExecutionPipeline::Outcome::Superseded;
};

static std::shared_ptr<AsyncCommitResult>
SubmitRequest(UsdGenSession& session, UsdGenSession::CommitRequest request)
{
    auto result = std::make_shared<AsyncCommitResult>();
    result->accepted = session.CommitAsync(std::move(request), [result](
        UsdGenSession::SnapshotPtr snapshot,
        UsdGenExecutionPipeline::Outcome outcome) {
            result->snapshot = std::move(snapshot);
            result->outcome = outcome;
            result->done.store(true, std::memory_order_release);
        });
    return result;
}

static bool WaitFor(AsyncCommitResult const& result)
{
    auto const deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(10);
    while (!result.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return result.done.load(std::memory_order_acquire);
}

static bool HasDiagnostic(UsdGenSession::SnapshotPtr const& snapshot,
                          char const* text)
{
    if (!snapshot) return false;
    for (std::string const& error : snapshot->diagnostics.errors)
        if (error.find(text) != std::string::npos) return true;
    return false;
}

int main() {
    cudaStream_t consumer = nullptr;
    CHECK(cudaStreamCreateWithFlags(&consumer, cudaStreamNonBlocking) == cudaSuccess);
    // Source resampling happens after the GPU source upload: literal targets
    // change topology without changing the authored host curve set.
    {
        UsdGenSession session;
        auto uniform = SourceDesc();
        uniform.nodes.front().params.push_back({TfToken("resampleTo"), VtValue(4), false});
        session.SetGraphDesc(uniform);
        session.SetDevicePublicationEnabled(true);
        auto four = session.Commit(1, UsdGenCommitReason::SetTime);
        CHECK(four && four->device);
        auto lease = gpu::AcquireGeometry(four->device, consumer);
        CHECK(lease && lease.Geometry().curveCount == 2 && lease.Geometry().pointCount == 8);
        std::vector<float3> points(8);
        std::vector<uint32_t> offsets(3);
        CHECK(cudaMemcpyAsync(points.data(), lease.Geometry().points.data, points.size() * sizeof(float3), cudaMemcpyDeviceToHost, consumer) == cudaSuccess);
        CHECK(cudaMemcpyAsync(offsets.data(), lease.Geometry().curveOffsets.data, offsets.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost, consumer) == cudaSuccess);
        CHECK(cudaStreamSynchronize(consumer) == cudaSuccess);
        CHECK(offsets == std::vector<uint32_t>({0,4,8}) &&
              std::fabs(points[0].x - 20.0f) < 1e-4f && std::fabs(points[1].x - 20.666666f) < 1e-4f &&
              std::fabs(points[2].x - 22.0f) < 1e-4f && std::fabs(points[3].x - 24.0f) < 1e-4f &&
              std::fabs(points[4].x - 10.0f) < 1e-4f && std::fabs(points[5].x - 10.333334f) < 1e-4f &&
              std::fabs(points[6].x - 10.666666f) < 1e-4f && std::fabs(points[7].x - 11.0f) < 1e-4f);
        auto const lastGood = four;
        auto invalid = uniform;
        invalid.nodes.front().params.front().value = VtValue(1);
        session.SetGraphDesc(invalid);
        CHECK(session.Commit(2, UsdGenCommitReason::SetTime) == lastGood && session.LastDiagnostics().HasErrors());
        invalid.nodes.front().params.front().value = VtValue(-2);
        session.SetGraphDesc(invalid);
        CHECK(session.Commit(3, UsdGenCommitReason::SetTime) == lastGood && session.LastDiagnostics().HasErrors());
        auto ragged = uniform;
        ragged.nodes.front().params.front().value = VtValue(0);
        session.SetGraphDesc(ragged);
        auto zero = session.Commit(4, UsdGenCommitReason::SetTime);
        CHECK(zero && zero != lastGood);
        auto zeroLease = gpu::AcquireGeometry(zero->device, consumer);
        CHECK(zeroLease && zeroLease.Geometry().pointCount == 5);
    }
    // Groom scalar Int32 controls are evaluated per commit, before source
    // resampling; an invalid evaluated value must retain the last publication.
    {
        UsdGenSession session;
        auto desc = SourceDesc();
        UsdGenExpressionDesc expression;
        expression.path = SdfPath("/Groom/Description/Expressions/resample");
        expression.source = "$frame > 1 ? 4 : 0";
        expression.outputs.push_back({TfToken("result"), TfToken("int"), ScalarShape(expr::ScalarType::Int32)});
        desc.expressions.push_back(expression);
        UsdGenExpressionBinding binding;
        binding.expression = expression.path;
        binding.destination = TfToken("resampleTo");
        binding.nativeType = TfToken("int");
        binding.destinationShape = ScalarShape(expr::ScalarType::Int32);
        binding.domain = expr::Domain::Groom;
        binding.literal = VtValue(0);
        // This literal would be invalid without the connected groom result;
        // frame 2 evaluates to four and must win before source validation.
        desc.nodes.front().params.push_back({TfToken("resampleTo"), VtValue(1), false});
        desc.nodes.front().expressionBindings.push_back(binding);
        session.SetGraphDesc(desc);
        session.SetDevicePublicationEnabled(true);
        auto ragged = session.Commit(1, UsdGenCommitReason::SetTime);
        CHECK(ragged && ragged->device && ragged->device->Geometry().pointCount == 5);
        auto uniform = session.Commit(2, UsdGenCommitReason::SetTime);
        CHECK(uniform && uniform != ragged && uniform->device->Geometry().pointCount == 8);
        desc.expressions[0].source = "1";
        session.SetGraphDesc(desc);
        CHECK(session.Commit(3, UsdGenCommitReason::SetTime) == uniform && session.LastDiagnostics().HasErrors());
        desc.expressions[0].source = "-2";
        session.SetGraphDesc(desc);
        CHECK(session.Commit(4, UsdGenCommitReason::SetTime) == uniform && session.LastDiagnostics().HasErrors());
    }
    {
        UsdGenSession session;
        auto desc = SourceDesc();
        desc.curveSets[0].points[0][2] = 5.0f; // independent authored rest remains z=0
        UsdGenExpressionDesc expression;
        expression.path = SdfPath("/Groom/Description/Expressions/useRest");
        expression.source = "$frame < 2";
        expression.outputs.push_back({TfToken("result"), TfToken("bool"), ScalarShape(expr::ScalarType::Bool)});
        desc.expressions.push_back(expression);
        UsdGenExpressionBinding binding;
        binding.expression = expression.path; binding.destination = TfToken("useRest");
        binding.nativeType = TfToken("bool"); binding.destinationShape = ScalarShape(expr::ScalarType::Bool);
        binding.domain = expr::Domain::Groom; binding.literal = VtValue(false);
        desc.nodes.front().params.push_back({TfToken("resampleTo"), VtValue(4), false});
        desc.nodes.front().expressionBindings.push_back(binding);
        {
            UsdGenSession literalSession; literalSession.SetDevicePublicationEnabled(true);
            literalSession.SetGraphDesc(desc);
            auto literalRest = literalSession.Commit(1, UsdGenCommitReason::SetTime);
            auto literalCurrent = literalSession.Commit(2, UsdGenCommitReason::SetTime);
            CHECK(literalRest && literalCurrent && literalRest->device && literalCurrent->device &&
                  literalRest != literalCurrent && literalRest->device->Geometry().pointCount == 8 &&
                  literalCurrent->device->Geometry().pointCount == 8 &&
                  !literalRest->device->Geometry().alreadyDeformed &&
                  literalCurrent->device->Geometry().alreadyDeformed);
        }
        UsdGenExpressionDesc resampleExpression;
        resampleExpression.path = SdfPath("/Groom/Description/Expressions/useRestResample");
        resampleExpression.source = "$frame < 2 ? 3 : 2";
        resampleExpression.outputs.push_back({TfToken("result"), TfToken("int"), ScalarShape(expr::ScalarType::Int32)});
        desc.expressions.push_back(resampleExpression);
        UsdGenExpressionBinding resampleBinding;
        resampleBinding.expression = resampleExpression.path; resampleBinding.destination = TfToken("resampleTo");
        resampleBinding.nativeType = TfToken("int"); resampleBinding.destinationShape = ScalarShape(expr::ScalarType::Int32);
        resampleBinding.domain = expr::Domain::Groom; resampleBinding.literal = VtValue(4);
        desc.nodes.front().expressionBindings.push_back(resampleBinding);
        session.SetGraphDesc(desc); session.SetDevicePublicationEnabled(true);
        auto rootFor91 = [](gpu::CudaGeometryLease const& lease, bool rest = false) {
            auto geometry = lease.Geometry();
            auto const* channel = rest ? geometry.restPoints.data : geometry.points.data;
            if (!lease || !geometry.stableIds.data || !geometry.curveOffsets.data || !channel)
                return float3{NAN, NAN, NAN};
            std::vector<uint64_t> ids(geometry.curveCount);
            std::vector<uint32_t> offsets(geometry.curveCount + 1);
            if (cudaMemcpy(ids.data(), geometry.stableIds.data, ids.size() * sizeof(uint64_t), cudaMemcpyDeviceToHost) != cudaSuccess ||
                cudaMemcpy(offsets.data(), geometry.curveOffsets.data, offsets.size() * sizeof(uint32_t), cudaMemcpyDeviceToHost) != cudaSuccess)
                return float3{NAN, NAN, NAN};
            for (size_t curve = 0; curve < ids.size(); ++curve) if (ids[curve] == 91) {
                if (offsets[curve] >= offsets[curve + 1] || offsets[curve] >= geometry.pointCount)
                    return float3{NAN, NAN, NAN};
                float3 point{};
                if (cudaMemcpy(&point, channel + offsets[curve], sizeof(point), cudaMemcpyDeviceToHost) != cudaSuccess)
                    return float3{NAN, NAN, NAN};
                return point;
            }
            return float3{NAN, NAN, NAN};
        };
        auto restGeneration = session.Commit(1, UsdGenCommitReason::SetTime);
        CHECK(restGeneration && restGeneration->device && !restGeneration->device->Geometry().alreadyDeformed &&
              restGeneration->device->Geometry().pointCount == 6);
        auto restLease = gpu::AcquireGeometry(restGeneration->device, nullptr);
        CHECK(restLease);
        float3 restPoint = rootFor91(restLease);
        CHECK(std::fabs(restPoint.z - 5.0f) < 1e-6f);
        CHECK(std::fabs(rootFor91(restLease, true).z) < 1e-6f);
        auto currentGeneration = session.Commit(2, UsdGenCommitReason::SetTime);
        CHECK(currentGeneration && currentGeneration != restGeneration && currentGeneration->device &&
              currentGeneration->device->Geometry().alreadyDeformed && currentGeneration->device->Geometry().pointCount == 4);
        auto currentLease = gpu::AcquireGeometry(currentGeneration->device, nullptr);
        CHECK(currentLease);
        float3 currentPoint = rootFor91(currentLease);
        CHECK(std::fabs(currentPoint.z - 5.0f) < 1e-6f);
        CHECK(std::fabs(rootFor91(currentLease, true).z) < 1e-6f);
        auto badType = desc;
        badType.nodes.front().expressionBindings[0].nativeType = TfToken("float");
        badType.nodes.front().expressionBindings[0].destinationShape = ScalarShape(expr::ScalarType::Float32);
        session.SetGraphDesc(badType);
        CHECK(session.Commit(3, UsdGenCommitReason::SetTime) == currentGeneration && session.LastDiagnostics().HasErrors());
        for (expr::Domain domain : {expr::Domain::Primitive, expr::Domain::Point}) {
            auto badDomain = desc;
            badDomain.nodes.front().expressionBindings[0].domain = domain;
            session.SetGraphDesc(badDomain);
            CHECK(session.Commit(3, UsdGenCommitReason::SetTime) == currentGeneration && session.LastDiagnostics().HasErrors());
        }
    }
    // The renderer selection belongs to the immutable request, not a
    // separately posted mutable side channel.  Start CUDA-enabled, reject two
    // false/absent requests against the retained device generation, then
    // recover and prove an absent request preserves the recovered true mode.
    {
        UsdGenSession session;
        auto desc = std::make_shared<const UsdGenGraphDesc>(SourceDesc());
        UsdGenSession::CommitRequest enable;
        enable.frame = 101.0;
        enable.reason = UsdGenCommitReason::SetTime;
        enable.desc = desc;
        enable.devicePublication = true;
        auto first = SubmitRequest(session, std::move(enable));
        CHECK(first->accepted && WaitFor(*first));
        CHECK(first->outcome == UsdGenExecutionPipeline::Outcome::Published &&
              first->snapshot && first->snapshot->generation &&
              first->snapshot->generation->device &&
              first->snapshot->generation->frame == 101.0);
        auto const retained = first->snapshot->generation;

        UsdGenSession::CommitRequest disable;
        disable.frame = 102.0;
        disable.reason = UsdGenCommitReason::SetTime;
        disable.devicePublication = false;
        auto rejected = SubmitRequest(session, std::move(disable));
        CHECK(rejected->accepted && WaitFor(*rejected));
        CHECK(rejected->outcome == UsdGenExecutionPipeline::Outcome::Failed &&
              rejected->snapshot && rejected->snapshot->generation == retained &&
              HasDiagnostic(rejected->snapshot, "device-aware consumer"));

        UsdGenSession::CommitRequest absentFalse;
        absentFalse.frame = 103.0;
        absentFalse.reason = UsdGenCommitReason::SetTime;
        auto stillRejected = SubmitRequest(session, std::move(absentFalse));
        CHECK(stillRejected->accepted && WaitFor(*stillRejected));
        CHECK(stillRejected->outcome == UsdGenExecutionPipeline::Outcome::Failed &&
              stillRejected->snapshot &&
              stillRejected->snapshot->generation == retained &&
              HasDiagnostic(stillRejected->snapshot, "device-aware consumer"));

        UsdGenSession::CommitRequest recover;
        recover.frame = 104.0;
        recover.reason = UsdGenCommitReason::SetTime;
        recover.devicePublication = true;
        auto recovered = SubmitRequest(session, std::move(recover));
        CHECK(recovered->accepted && WaitFor(*recovered));
        CHECK(recovered->outcome == UsdGenExecutionPipeline::Outcome::Published &&
              recovered->snapshot && recovered->snapshot->generation &&
              recovered->snapshot->generation->device &&
              recovered->snapshot->generation != retained &&
              recovered->snapshot->generation->frame == 104.0);

        UsdGenSession::CommitRequest absentTrue;
        absentTrue.frame = 105.0;
        absentTrue.reason = UsdGenCommitReason::SetTime;
        auto stillEnabled = SubmitRequest(session, std::move(absentTrue));
        CHECK(stillEnabled->accepted && WaitFor(*stillEnabled));
        CHECK(stillEnabled->outcome == UsdGenExecutionPipeline::Outcome::Published &&
              stillEnabled->snapshot && stillEnabled->snapshot->generation &&
              stillEnabled->snapshot->generation->device &&
              stillEnabled->snapshot->generation != recovered->snapshot->generation &&
              stillEnabled->snapshot->generation->frame == 105.0);
    }
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

    // A compiled Source -> Width -> Width chain is reusable across frames.
    // Keep the first width generation leased while later commits replace the
    // session's published generation, proving that its borrowed device data
    // remains valid until the consumer releases it.
    gpu::CudaGeometryLease retainedWidths;
    std::weak_ptr<const UsdGenDeviceOwner> widthOwner;
    {
        UsdGenSession session;
        auto desc = WidthChainDesc();
        session.SetGraphDesc(desc);
        session.SetDevicePublicationEnabled(true);
        auto first = session.Commit(1, UsdGenCommitReason::SetTime);
        if (!first) for (auto const& error : session.LastDiagnostics().errors)
            std::fprintf(stderr, "%s\n", error.c_str());
        CHECK(first && first->device && first->id == 0 && first->frame == 1);
        auto const* plan = session.Graph().CudaPlan().get();
        CHECK(plan != nullptr);
        retainedWidths = gpu::AcquireGeometry(first->device, consumer);
        CHECK(retainedWidths && retainedWidths.Geometry().pointCount == 5);
        widthOwner = first->device->Owner();
        auto firstView = retainedWidths.Geometry();
        std::vector<float> widths(5);
        auto readWidths = [&](gpu::DeviceCurveGeometryView view) {
            if (cudaMemcpyAsync(widths.data(), view.widths.data,
                                widths.size() * sizeof(float),
                                cudaMemcpyDeviceToHost, consumer) != cudaSuccess)
                return false;
            return cudaStreamSynchronize(consumer) == cudaSuccess;
        };

        // WidthLiteral supplies a literal width of .1. WidthExpr then uses a
        // typed point field, primitive replace, and groom enabled field.
        CHECK(readWidths(firstView));
        float const pointFrame1 = 0.1f + 1.0f / 24.0f;
        CHECK(std::fabs(widths[0] - pointFrame1) < 2e-5f);
        CHECK(std::fabs(widths[1] - pointFrame1) < 2e-5f);
        CHECK(std::fabs(widths[2] - pointFrame1) < 2e-5f);
        CHECK(std::fabs(widths[3] - 0.1f * pointFrame1) < 2e-5f);
        CHECK(std::fabs(widths[4] - 0.1f * pointFrame1) < 2e-5f);

        auto second = session.Commit(2, UsdGenCommitReason::SetTime);
        CHECK(second && second->device && second->id == 1 && second->frame == 2);
        CHECK(session.Graph().CudaPlan().get() == plan);
        // Primitive 0 has replace=true; primitive 1 has replace=false. The
        // latter multiplies WidthLiteral's .1 by the point target.
        auto secondLease = gpu::AcquireGeometry(second->device, consumer);
        CHECK(secondLease);
        CHECK(readWidths(secondLease.Geometry()));
        float const pointFrame2 = 0.2f + 2.0f / 24.0f;
        CHECK(std::fabs(widths[0] - pointFrame2) < 2e-5f);
        CHECK(std::fabs(widths[1] - pointFrame2) < 2e-5f);
        CHECK(std::fabs(widths[2] - pointFrame2) < 2e-5f);
        CHECK(std::fabs(widths[3] - 0.1f * pointFrame2) < 2e-5f);
        CHECK(std::fabs(widths[4] - 0.1f * pointFrame2) < 2e-5f);
        CHECK(!widthOwner.expired());
        first.reset();
        CHECK(readWidths(firstView));
        CHECK(std::fabs(widths[0] - pointFrame1) < 2e-5f);
        CHECK(std::fabs(widths[3] - 0.1f * pointFrame1) < 2e-5f);

        auto third = session.Commit(3, UsdGenCommitReason::SetTime);
        CHECK(third && third->device && third->id == 2 && third->frame == 3);
        // enabled is false at frame 3, so WidthExpr is an exact passthrough
        // and the preceding literal Width remains .1 everywhere.
        auto thirdLease = gpu::AcquireGeometry(third->device, consumer);
        CHECK(thirdLease);
        CHECK(readWidths(thirdLease.Geometry()));
        for (float width : widths) CHECK(width == 0.1f);

        // A malformed program must leave the last-good generation and plan
        // published. A runtime non-finite result must do the same.
        auto lastGood = third;
        auto malformed = desc;
        malformed.expressions[0].source = "$frame +";
        session.SetGraphDesc(malformed);
        CHECK(session.Commit(4, UsdGenCommitReason::SetTime) == lastGood);
        CHECK(session.LastDiagnostics().HasErrors() && session.NeedsCommit());
        CHECK(session.Graph().CudaPlan().get() == plan);

        auto runtimeBad = desc;
        runtimeBad.expressions[0].source = "$frame / ($frame - 4)";
        session.SetGraphDesc(runtimeBad);
        CHECK(session.Commit(4, UsdGenCommitReason::SetTime) == lastGood);
        CHECK(session.LastDiagnostics().HasErrors() && session.NeedsCommit());
        // Compilation succeeded, so Graph()/CudaPlan() may now describe the
        // attempted program even though publication correctly retained the
        // last-good generation.

        session.SetGraphDesc(desc);
        auto recovered = session.Commit(5, UsdGenCommitReason::SetTime);
        CHECK(recovered && recovered->device && recovered != lastGood && recovered->frame == 5);

        // Empty authored tiles still produce a valid empty Source -> Width
        // generation. The point binding is intentionally retained: empty
        // domains must not force a null field dereference or a stale publish.
        auto empty = desc;
        auto& curves = empty.curveSets[0];
        curves.curveVertexCounts.clear();
        curves.points.clear();
        curves.rest.clear();
        curves.curveId.clear();
        curves.skinPrim.clear();
        curves.skinPrimUv.clear();
        session.SetGraphDesc(empty);
        auto emptyGeneration = session.Commit(6, UsdGenCommitReason::SetTime);
        for (auto const& error : session.LastDiagnostics().errors)
            std::fprintf(stderr, "%s\n", error.c_str());
        CHECK(emptyGeneration && emptyGeneration->device && emptyGeneration != recovered);
        CHECK(!session.LastDiagnostics().HasErrors());
        auto emptyLease = gpu::AcquireGeometry(emptyGeneration->device, consumer);
        CHECK(emptyLease && emptyLease.Geometry().curveCount == 0 &&
              emptyLease.Geometry().pointCount == 0 &&
              emptyLease.Geometry().curveOffsets.size == 1 &&
              emptyLease.Geometry().widths.size == 0);
    }
    CHECK(retainedWidths && !widthOwner.expired());
    retainedWidths = {};
    CHECK(widthOwner.expired());
    CHECK(cudaStreamDestroy(consumer) == cudaSuccess);
    std::puts("testUsdGenCudaSession: PASS");
    return 0;
}
