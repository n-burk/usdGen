#include "usdGen/cudaExecution.h"
#include "usdGen/cudaExecutionQueue.h"
#include "usdGen/executionResources.h"
#include "usdGen/executionRetirement.h"
#include "usdGen/gpu/generation.h"
#include "usdGen/gpu/deviceResources.h"
#include "usdGen/imagePayload.h"

#include <cuda_runtime_api.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace usdGen;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (false)

static size_t ResourceKindBytes(UsdGenExecutionResourceSnapshot const& snapshot,
                                UsdGenExecutionResourceKind kind) {
    return snapshot.byKind[static_cast<size_t>(kind)];
}

static bool ParseLiteralRbfPeak(UsdGenDiagnostics const& diagnostics,
                                uint64_t* peak) {
    if (!peak || diagnostics.errors.size() != 1 || !diagnostics.warnings.empty())
        return false;
    std::string const marker = "literal-RBF memory reservation of ";
    auto const begin = diagnostics.errors.front().find(marker);
    if (begin == std::string::npos) return false;
    auto const number = begin + marker.size();
    auto const end = diagnostics.errors.front().find(" bytes", number);
    if (end == std::string::npos || end == number) return false;
    try {
        size_t consumed = 0;
        auto const value = std::stoull(diagnostics.errors.front().substr(
            number, end - number), &consumed);
        if (consumed != end - number || value == 0) return false;
        *peak = static_cast<uint64_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

UsdGenGraphDesc RbfDesc() {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/RbfAtomic/Hair");
    desc.executionBackend = UsdGenExecutionBackend::Cuda;
    UsdGenSurfaceDesc surface;
    surface.path = SdfPath("/RbfAtomic/Scalp");
    surface.restPoints = {{0,0,0}, {1,0,0}, {0,1,0}, {0,0,1}, {1,1,1}};
    surface.points = surface.restPoints;
    for (auto& point : surface.points) point[0] += .25f;
    surface.faceVertexCounts = {3,3,3};
    surface.faceVertexIndices = {0,1,2, 0,1,3, 1,2,4};
    desc.surfaces.push_back(surface);
    UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/RbfAtomic/Curves");
    curves.role = UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.curveVertexCounts = {2};
    curves.points = {{.2f,.2f,0}, {.2f,.4f,0}};
    curves.rest = curves.points;
    curves.curveId = {42};
    curves.skinPrim = {0};
    curves.skinPrimUv = {{.2f,.2f}};
    desc.curveSets.push_back(curves);
    UsdGenNodeDesc source;
    source.path = SdfPath("/RbfAtomic/Source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.surfaces = {surface.path};
    UsdGenNodeDesc deform;
    deform.path = SdfPath("/RbfAtomic/Deform");
    deform.type = TfToken("UsdGenDeform");
    deform.inputs = {source.path};
    deform.surfaces = {surface.path};
    deform.params.push_back({TfToken("rbfSamples"), VtValue(5), false});
    desc.nodes = {source, deform};
    desc.terminal = deform.path;
    return desc;
}

UsdGenGraphDesc ChangedRestDesc() {
    auto desc = RbfDesc();
    desc.surfaces.front().restPoints.back()[2] += .25f;
    desc.surfaces.front().points.back()[2] += .25f;
    return desc;
}

UsdGenGraphDesc ChangedBudgetDesc() {
    auto desc = ChangedRestDesc();
    desc.nodes.back().params.front().value = VtValue(4);
    return desc;
}

bool SameStats(std::vector<UsdGenCudaBindingStats> const& a,
               std::vector<UsdGenCudaBindingStats> const& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i != a.size(); ++i)
        if (a[i].path != b[i].path || a[i].identity != b[i].identity ||
            a[i].bindCount != b[i].bindCount ||
            a[i].solveCount != b[i].solveCount ||
            a[i].sampleCount != b[i].sampleCount) return false;
    return true;
}

bool SameStatsAccounting(std::vector<UsdGenCudaBindingStats> const& a,
                         std::vector<UsdGenCudaBindingStats> const& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i != a.size(); ++i)
        if (a[i].path != b[i].path || a[i].bindCount != b[i].bindCount ||
            a[i].solveCount != b[i].solveCount ||
            a[i].sampleCount != b[i].sampleCount) return false;
    return true;
}

bool SameGeneration(std::shared_ptr<const UsdGenDeviceGeneration> const& a,
                    std::shared_ptr<const UsdGenDeviceGeneration> const& b) {
    if (!a || !b || a->Channels().size() != b->Channels().size()) return false;
    auto const& am = a->Geometry();
    auto const& bm = b->Geometry();
    // Generation/version identities may differ across independent owners;
    // topology shape, channel contracts and tile ranges must not.
    if (am.curveCount != bm.curveCount || am.pointCount != bm.pointCount ||
        am.curveTopology.type != bm.curveTopology.type ||
        am.curveTopology.basis != bm.curveTopology.basis ||
        am.curveTopology.wrap != bm.curveTopology.wrap ||
        am.tiles.size() != bm.tiles.size()) return false;
    for (size_t i = 0; i != am.tiles.size(); ++i) {
        auto const& at = am.tiles[i]; auto const& bt = bm.tiles[i];
        if (at.tile != bt.tile || at.firstCurve != bt.firstCurve || at.curveCount != bt.curveCount ||
            at.firstPoint != bt.firstPoint || at.pointCount != bt.pointCount ||
            at.boundsValid != bt.boundsValid) return false;
        if (at.boundsValid) for (size_t lane = 0; lane != 3; ++lane)
            if (!(std::fabs(at.extentMin[lane] - bt.extentMin[lane]) <= 1e-5f) ||
                !(std::fabs(at.extentMax[lane] - bt.extentMax[lane]) <= 1e-5f)) return false;
    }
    for (auto const& ac : a->Channels()) {
        auto bc = std::find_if(b->Channels().begin(), b->Channels().end(),
            [&](auto const& channel) { return channel.name == ac.name; });
        if (bc == b->Channels().end() || ac.type != bc->type || ac.domain != bc->domain ||
            ac.elementCount != bc->elementCount || ac.arity != bc->arity ||
            ac.strideBytes != bc->strideBytes || ac.readOnly != bc->readOnly ||
            ac.semantic != bc->semantic) return false;
    }
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
        return false;
    bool same = false;
    {
        auto aa = gpu::AcquireGeometry(a, stream);
        auto bb = gpu::AcquireGeometry(b, stream);
        if (aa && bb && aa.Geometry().pointCount == bb.Geometry().pointCount &&
            aa.Geometry().curveCount == bb.Geometry().curveCount) {
            std::vector<float3> ap(aa.Geometry().pointCount), bp(bb.Geometry().pointCount);
            std::vector<float> aw(aa.Geometry().pointCount), bw(bb.Geometry().pointCount);
            auto const pointBytes = ap.size() * sizeof(float3);
            auto const widthBytes = aw.size() * sizeof(float);
            bool copied = true;
            if (pointBytes)
                copied = cudaMemcpyAsync(ap.data(), aa.Geometry().points.data, pointBytes,
                                         cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
                         cudaMemcpyAsync(bp.data(), bb.Geometry().points.data, pointBytes,
                                         cudaMemcpyDeviceToHost, stream) == cudaSuccess;
            if (copied && widthBytes)
                copied = cudaMemcpyAsync(aw.data(), aa.Geometry().widths.data, widthBytes,
                                         cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
                         cudaMemcpyAsync(bw.data(), bb.Geometry().widths.data, widthBytes,
                                         cudaMemcpyDeviceToHost, stream) == cudaSuccess;
            same = copied && cudaStreamSynchronize(stream) == cudaSuccess;
            for (size_t i = 0; same && i != ap.size(); ++i)
                same = std::fabs(ap[i].x - bp[i].x) < 1e-5f &&
                       std::fabs(ap[i].y - bp[i].y) < 1e-5f &&
                       std::fabs(ap[i].z - bp[i].z) < 1e-5f &&
                       std::fabs(aw[i] - bw[i]) < 1e-5f;
            auto equalView = [&](auto av, auto bv) {
                if (av.size != bv.size) return false;
                size_t const bytes = av.size * sizeof(*av.data);
                if (!bytes) return true;
                if (!av.data || !bv.data) return false;
                std::vector<unsigned char> left(bytes), right(bytes);
                return cudaMemcpyAsync(left.data(), av.data, bytes, cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
                    cudaMemcpyAsync(right.data(), bv.data, bytes, cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
                    cudaStreamSynchronize(stream) == cudaSuccess && left == right;
            };
            same = same && equalView(aa.Geometry().restPoints, bb.Geometry().restPoints) &&
                equalView(aa.Geometry().curveOffsets, bb.Geometry().curveOffsets) &&
                equalView(aa.Geometry().stableIds, bb.Geometry().stableIds) &&
                equalView(aa.HairT(), bb.HairT()) && equalView(aa.RootPrim(), bb.RootPrim()) &&
                equalView(aa.RootUV(), bb.RootUV()) && equalView(aa.RootT(), bb.RootT()) &&
                equalView(aa.RootB(), bb.RootB()) && equalView(aa.RootN(), bb.RootN()) &&
                a->Geometry().alreadyDeformed == b->Geometry().alreadyDeformed;
            for (auto const& channel : a->Channels()) {
                if (!same || channel.semantic != UsdGenDeviceChannelSemantic::Generic) continue;
                auto left = gpu::AcquireNamedChannel(a, channel.name, stream);
                auto right = gpu::AcquireNamedChannel(b, channel.name, stream);
                same = left && right && left.Metadata()->type == right.Metadata()->type &&
                    left.Metadata()->domain == right.Metadata()->domain &&
                    left.Metadata()->arity == right.Metadata()->arity && equalView(left.Bytes(), right.Bytes());
            }
        }
    }
    return cudaStreamDestroy(stream) == cudaSuccess && same;
}

struct Attempts {
    uint64_t rbfAccept = 0, rbfRollback = 0;
    uint64_t surfaceAccept = 0, surfaceRollback = 0;
};

Attempts GetAttempts() {
    return {cudaRbfFieldAcceptAttemptCountForTesting(),
            cudaRbfFieldRollbackAttemptCountForTesting(),
            cudaRbfSurfaceAcceptAttemptCountForTesting(),
            cudaRbfSurfaceRollbackAttemptCountForTesting()};
}

enum class Resolution { AcceptPreflight, RollbackPreflight,
                        AcceptCommit, RollbackCommit };
enum class Side { Rbf, Surface };

bool FailureCase(std::shared_ptr<const UsdGenCudaExecutionPlan> const& plan,
                 Resolution resolution, Side side) {
    auto require = [&](bool condition, char const* stage) {
        if (!condition)
            std::fprintf(stderr, "transaction case failed at %s (resolution=%d side=%d)\n",
                         stage, int(resolution), int(side));
        return condition;
    };
    UsdGenExecutionRuntime runtime(4);
    UsdGenDiagnostics diagnostics;
    UsdGenCudaExecutionQueue queue(runtime, -1, &diagnostics);
    if (!require(queue.Valid(), "queue creation")) return false;
    std::atomic<int> initialCalls{0};
    std::atomic<bool> initialPublished{false};
    if (!require(queue.Submit(plan, 1.0, [&](uint64_t, auto outcome, auto const&) {
            initialPublished.store(
                outcome == UsdGenExecutionPipeline::Outcome::Published,
                std::memory_order_release);
            initialCalls.fetch_add(1, std::memory_order_release);
        }), "initial submit")) return false;
    queue.Drain();
    auto const lastGood = queue.Snapshot();
    if (!require(initialCalls.load(std::memory_order_acquire) == 1 &&
        initialPublished.load(std::memory_order_acquire) && lastGood &&
        lastGood->generation && lastGood->bindings.size() == 1 &&
        lastGood->bindings[0].identity,
        "initial publication")) return false;

    auto const acceptedStats = lastGood->bindings;
    auto const before = GetAttempts();
    auto const quarantined = cudaFinalizationRelayQuarantinedCountForTesting();
    bool const rollback = resolution == Resolution::RollbackPreflight ||
                          resolution == Resolution::RollbackCommit;
    bool const preflight = resolution == Resolution::AcceptPreflight ||
                           resolution == Resolution::RollbackPreflight;
    if (rollback) failNextCudaFinalizationRelayAllocationForTesting();
    if (side == Side::Rbf) {
        if (preflight) failNextCudaRbfFieldResolvePreflightForTesting();
        else failNextCudaRbfFieldResolveCommitForTesting();
    } else {
        if (preflight) failNextCudaRbfSurfaceResolvePreflightForTesting();
        else failNextCudaRbfSurfaceResolveCommitForTesting();
    }

    std::atomic<int> failureCalls{0};
    std::atomic<bool> failed{false};
    if (!require(queue.Submit(plan, 2.0, [&](uint64_t, auto outcome, auto const&) {
            failed.store(outcome == UsdGenExecutionPipeline::Outcome::Failed,
                         std::memory_order_release);
            failureCalls.fetch_add(1, std::memory_order_release);
        }), "fault submit")) return false;
    queue.Drain();
    auto const after = GetAttempts();
    bool const failureOutcomeOk = failureCalls.load(std::memory_order_acquire) == 1 &&
        failed.load(std::memory_order_acquire) && queue.Snapshot() == lastGood &&
        SameStats(queue.Snapshot()->bindings, acceptedStats) &&
        cudaFinalizationRelayQuarantinedCountForTesting() == quarantined + 1;
    if (!failureOutcomeOk)
        std::fprintf(stderr,
            "  calls=%d failed=%d sameSnapshot=%d sameStats=%d quarantine=%zu->%zu\n",
            failureCalls.load(std::memory_order_acquire),
            int(failed.load(std::memory_order_acquire)),
            int(queue.Snapshot() == lastGood),
            int(queue.Snapshot() && SameStats(queue.Snapshot()->bindings, acceptedStats)),
            quarantined, cudaFinalizationRelayQuarantinedCountForTesting());
    if (!require(failureOutcomeOk,
        "failure outcome/retention/quarantine")) return false;

    if (preflight) {
        if (!require(after.rbfAccept == before.rbfAccept &&
            after.rbfRollback == before.rbfRollback &&
            after.surfaceAccept == before.surfaceAccept &&
            after.surfaceRollback == before.surfaceRollback,
            "preflight made no concrete calls")) return false;
    } else if (rollback) {
        if (!require(after.rbfRollback == before.rbfRollback + 1 &&
            after.surfaceRollback == before.surfaceRollback + 1 &&
            after.rbfAccept == before.rbfAccept &&
            after.surfaceAccept == before.surfaceAccept,
            "rollback attempted both resources")) return false;
    } else {
        if (!require(after.rbfAccept == before.rbfAccept + 1 &&
            after.surfaceAccept == before.surfaceAccept + 1 &&
            after.rbfRollback == before.rbfRollback &&
            after.surfaceRollback == before.surfaceRollback,
            "accept attempted both resources")) return false;
    }

    // Resolution failure poisons the workspace. A later request reports one
    // failure and cannot replace the exact last-good queue snapshot.
    std::atomic<int> retryCalls{0};
    std::atomic<bool> retryFailed{false};
    if (!require(queue.Submit(plan, 3.0, [&](uint64_t, auto outcome, auto const&) {
            retryFailed.store(outcome == UsdGenExecutionPipeline::Outcome::Failed,
                              std::memory_order_release);
            retryCalls.fetch_add(1, std::memory_order_release);
        }), "poisoned retry submit")) return false;
    queue.Drain();
    return require(retryCalls.load(std::memory_order_acquire) == 1 &&
        retryFailed.load(std::memory_order_acquire) &&
        queue.Snapshot() == lastGood &&
        SameStats(queue.Snapshot()->bindings, acceptedStats),
        "poisoned retry outcome");
}

int SynchronousTransactions(
    std::shared_ptr<const UsdGenCudaExecutionPlan> const& plan) {
    UsdGenDiagnostics diagnostics;
    auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(workspace);
    int resourceDevice = -1;
    CHECK(cudaGetDevice(&resourceDevice) == cudaSuccess);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, resourceDevice});
    CHECK(resources);
    auto const workspaceBaseline = resources->Snapshot();
    auto first = ExecuteCudaGraph(*plan, *workspace, 20.0, 200, &diagnostics);
    CHECK(first && !diagnostics.HasErrors());
    auto const firstStats = GetCudaBindingStats(*workspace);
    CHECK(firstStats.size() == 1 && firstStats.front().identity &&
          firstStats.front().bindCount == 1 && firstStats.front().solveCount == 1 &&
          firstStats.front().sampleCount == 5);
    auto const firstAcceptedResources = resources->Snapshot();
    CHECK(firstAcceptedResources.usedBytes > workspaceBaseline.usedBytes &&
          ResourceKindBytes(firstAcceptedResources,
              UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(workspaceBaseline,
                  UsdGenExecutionResourceKind::Pending));

    // The literal cold Source->RBF plan is admitted with one precharged
    // execution ticket. Saturate the baseline once to learn the exact bound
    // from the production diagnostic, then prove a peak-minus-one filler
    // rejects before source/device work or cache mutation.
    auto const rbfBaseline = resources->Snapshot();
    CHECK(rbfBaseline.usableBytes >= rbfBaseline.usedBytes);
    auto const rbfAvailable = rbfBaseline.usableBytes - rbfBaseline.usedBytes;
    auto fullFiller = resources->TryReserve(
        rbfAvailable, UsdGenExecutionResourceKind::Active);
    CHECK(fullFiller);
    auto const fullSaturated = resources->Snapshot();
    UsdGenDiagnostics fullRejectDiagnostics;
    auto fullReject = ExecuteCudaGraph(*plan, *workspace, 20.5, 2001,
                                       &fullRejectDiagnostics, first);
    uint64_t rbfPeak = 0;
    CHECK(!fullReject && ParseLiteralRbfPeak(fullRejectDiagnostics, &rbfPeak));
    auto const afterFullReject = resources->Snapshot();
    CHECK(afterFullReject.usedBytes == fullSaturated.usedBytes &&
          ResourceKindBytes(afterFullReject,
              UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(fullSaturated,
                  UsdGenExecutionResourceKind::Pending) &&
          SameStats(GetCudaBindingStats(*workspace), firstStats));
    fullFiller->Release();
    CHECK(resources->Snapshot().usedBytes == rbfBaseline.usedBytes &&
          rbfPeak <= rbfAvailable);

    auto peakFiller = resources->TryReserve(
        rbfAvailable - static_cast<size_t>(rbfPeak) + 1,
        UsdGenExecutionResourceKind::Active);
    CHECK(peakFiller);
    auto const peakSaturated = resources->Snapshot();
    UsdGenDiagnostics peakRejectDiagnostics;
    auto peakReject = ExecuteCudaGraph(*plan, *workspace, 20.75, 2002,
                                       &peakRejectDiagnostics, first);
    uint64_t repeatedPeak = 0;
    CHECK(!peakReject && ParseLiteralRbfPeak(peakRejectDiagnostics, &repeatedPeak) &&
          repeatedPeak == rbfPeak);
    auto const afterPeakReject = resources->Snapshot();
    CHECK(afterPeakReject.usedBytes == peakSaturated.usedBytes &&
          ResourceKindBytes(afterPeakReject,
              UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(peakSaturated,
                  UsdGenExecutionResourceKind::Pending) &&
          SameStats(GetCudaBindingStats(*workspace), firstStats));
    peakFiller->Release();
    CHECK(resources->Snapshot().usedBytes == rbfBaseline.usedBytes);

    // Releasing the filler admits the warm update. Its cache identity remains
    // stable, the output owner is replaced privately, and Pending is closed
    // before the synchronous call returns.
    UsdGenDiagnostics warmDiagnostics;
    auto warm = ExecuteCudaGraph(*plan, *workspace, 20.75, 2003,
                                 &warmDiagnostics, first);
    auto const warmStats = GetCudaBindingStats(*workspace);
    CHECK(warm && !warmDiagnostics.HasErrors() && warm->Owner() &&
          warm->Owner() != first->Owner() && warmStats.size() == 1 &&
          warmStats.front().identity == firstStats.front().identity &&
          warmStats.front().solveCount == firstStats.front().solveCount + 1 &&
          ResourceKindBytes(resources->Snapshot(),
              UsdGenExecutionResourceKind::Pending) == 0 &&
          resources->Snapshot().usedBytes > rbfBaseline.usedBytes &&
          resources->Snapshot().usedBytes <= rbfBaseline.usedBytes + rbfPeak);

    // The asynchronous job ticket uses the same cold bound and is acquired
    // before source submission. A peak-minus-one filler therefore rejects
    // without constructing a cache candidate or changing Pending.
    auto asyncWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(asyncWorkspace);
    auto const asyncBaseline = resources->Snapshot();
    CHECK(asyncBaseline.usableBytes >= asyncBaseline.usedBytes + rbfPeak);
    auto asyncFiller = resources->TryReserve(
        asyncBaseline.usableBytes - asyncBaseline.usedBytes -
            static_cast<size_t>(rbfPeak) + 1,
        UsdGenExecutionResourceKind::Active);
    CHECK(asyncFiller);
    auto const asyncSaturated = resources->Snapshot();
    UsdGenDiagnostics asyncRejectDiagnostics;
    auto asyncRejected = CreateCudaExecutionJob(
        plan, *asyncWorkspace, 20.75, 2004, &asyncRejectDiagnostics, warm);
    uint64_t asyncPeak = 0;
    CHECK(!asyncRejected && ParseLiteralRbfPeak(asyncRejectDiagnostics, &asyncPeak) &&
          asyncPeak == rbfPeak && GetCudaBindingStats(*asyncWorkspace).empty());
    auto const afterAsyncReject = resources->Snapshot();
    CHECK(afterAsyncReject.usedBytes == asyncSaturated.usedBytes &&
          ResourceKindBytes(afterAsyncReject,
              UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(asyncSaturated,
                  UsdGenExecutionResourceKind::Pending));
    asyncFiller->Release();
    CHECK(resources->Snapshot().usedBytes == asyncBaseline.usedBytes);
    UsdGenDiagnostics asyncAdmissionDiagnostics;
    auto asyncAdmitted = CreateCudaExecutionJob(
        plan, *asyncWorkspace, 20.75, 2005, &asyncAdmissionDiagnostics, warm);
    CHECK(asyncAdmitted);
    auto const asyncHeld = resources->Snapshot();
    CHECK(ResourceKindBytes(asyncHeld, UsdGenExecutionResourceKind::Pending) >
          ResourceKindBytes(asyncBaseline, UsdGenExecutionResourceKind::Pending));
    CHECK(ExecuteCudaJobSource(*asyncAdmitted));
    for (size_t i = 0; i != CudaExecutionJobOperatorCount(*asyncAdmitted); ++i)
        CHECK(ExecuteCudaJobOperator(*asyncAdmitted, i));
    auto asyncGeneration = FinalizeCudaExecutionJob(*asyncAdmitted);
    CHECK(asyncGeneration && !asyncAdmissionDiagnostics.HasErrors());
    auto const afterAsyncAdmission = resources->Snapshot();
    CHECK(ResourceKindBytes(afterAsyncAdmission,
              UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(asyncBaseline,
                  UsdGenExecutionResourceKind::Pending) &&
          GetCudaBindingStats(*asyncWorkspace).size() == 1);
    asyncAdmitted.reset();
    // The compatibility entry point publishes the same geometry and binding
    // accounting as the queued path for the same graph and frame.
    UsdGenExecutionRuntime runtime(4);
    UsdGenDiagnostics asyncDiagnostics;
    UsdGenCudaExecutionQueue queue(runtime, -1, &asyncDiagnostics);
    CHECK(queue.Valid() && queue.Submit(plan, 20.0));
    queue.Drain();
    auto const async = queue.Snapshot();
    CHECK(async && async->generation && SameGeneration(first, async->generation) &&
          SameStatsAccounting(firstStats, async->bindings));

    // A failure after the fresh field and surface solves committed must roll
    // both back. The caller's exact prior publication and the workspace's
    // accepted cache identity/counters remain unchanged.
    auto const firstPointer = first;
    auto const beforeFailureAttempts = GetAttempts();
    auto const beforeFailureResources = resources->Snapshot();
    failNextCudaSynchronousFinalizationForTesting();
    UsdGenDiagnostics failedDiagnostics;
    auto failed = ExecuteCudaGraph(*plan, *workspace, 21.0, 201,
                                   &failedDiagnostics, firstPointer);
    auto const afterFailureAttempts = GetAttempts();
    auto const afterFailureResources = resources->Snapshot();
    if (afterFailureResources.usedBytes != beforeFailureResources.usedBytes ||
        ResourceKindBytes(afterFailureResources, UsdGenExecutionResourceKind::Pending) !=
            ResourceKindBytes(beforeFailureResources, UsdGenExecutionResourceKind::Pending)) {
        std::fprintf(stderr,
            "  rollback resources used=%zu->%zu pending=%zu->%zu cache=%zu->%zu scratch=%zu->%zu active=%zu->%zu\n",
            beforeFailureResources.usedBytes, afterFailureResources.usedBytes,
            ResourceKindBytes(beforeFailureResources, UsdGenExecutionResourceKind::Pending),
            ResourceKindBytes(afterFailureResources, UsdGenExecutionResourceKind::Pending),
            ResourceKindBytes(beforeFailureResources, UsdGenExecutionResourceKind::Cache),
            ResourceKindBytes(afterFailureResources, UsdGenExecutionResourceKind::Cache),
            ResourceKindBytes(beforeFailureResources, UsdGenExecutionResourceKind::Scratch),
            ResourceKindBytes(afterFailureResources, UsdGenExecutionResourceKind::Scratch),
            ResourceKindBytes(beforeFailureResources, UsdGenExecutionResourceKind::Active),
            ResourceKindBytes(afterFailureResources, UsdGenExecutionResourceKind::Active));
    }
    if (failed || firstPointer != first || !SameGeneration(firstPointer, first) ||
        !SameStats(GetCudaBindingStats(*workspace), warmStats) ||
        afterFailureAttempts.rbfAccept != beforeFailureAttempts.rbfAccept ||
        afterFailureAttempts.surfaceAccept != beforeFailureAttempts.surfaceAccept ||
        afterFailureAttempts.rbfRollback != beforeFailureAttempts.rbfRollback + 1 ||
        afterFailureAttempts.surfaceRollback != beforeFailureAttempts.surfaceRollback + 1) {
        std::fprintf(stderr,
            "  rollback state result=%d pointer=%d generation=%d stats=%d attempts rbfA=%llu->%llu surfaceA=%llu->%llu rbfR=%llu->%llu surfaceR=%llu->%llu\n",
            int(bool(failed)), int(firstPointer == first), int(SameGeneration(firstPointer, first)),
            int(SameStats(GetCudaBindingStats(*workspace), firstStats)),
            static_cast<unsigned long long>(beforeFailureAttempts.rbfAccept),
            static_cast<unsigned long long>(afterFailureAttempts.rbfAccept),
            static_cast<unsigned long long>(beforeFailureAttempts.surfaceAccept),
            static_cast<unsigned long long>(afterFailureAttempts.surfaceAccept),
            static_cast<unsigned long long>(beforeFailureAttempts.rbfRollback),
            static_cast<unsigned long long>(afterFailureAttempts.rbfRollback),
            static_cast<unsigned long long>(beforeFailureAttempts.surfaceRollback),
            static_cast<unsigned long long>(afterFailureAttempts.surfaceRollback));
    }
    CHECK(!failed && firstPointer == first && SameGeneration(firstPointer, first) &&
          SameStats(GetCudaBindingStats(*workspace), warmStats) &&
          afterFailureResources.usedBytes == beforeFailureResources.usedBytes &&
          ResourceKindBytes(afterFailureResources,
              UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(beforeFailureResources,
                  UsdGenExecutionResourceKind::Pending) &&
          afterFailureAttempts.rbfAccept == beforeFailureAttempts.rbfAccept &&
          afterFailureAttempts.surfaceAccept == beforeFailureAttempts.surfaceAccept &&
          afterFailureAttempts.rbfRollback == beforeFailureAttempts.rbfRollback + 1 &&
          afterFailureAttempts.surfaceRollback == beforeFailureAttempts.surfaceRollback + 1);

    UsdGenDiagnostics retryDiagnostics;
    auto retry = ExecuteCudaGraph(*plan, *workspace, 21.0, 202,
                                  &retryDiagnostics, firstPointer);
    auto const retryStats = GetCudaBindingStats(*workspace);
    CHECK(retry && !retryDiagnostics.HasErrors() && retryStats.size() == 1 &&
          retryStats.front().identity == warmStats.front().identity &&
          retryStats.front().bindCount == warmStats.front().bindCount &&
          retryStats.front().solveCount == warmStats.front().solveCount + 1 &&
          retryStats.front().sampleCount == warmStats.front().sampleCount);

    // A rejected changed-rest transaction cannot replace the accepted key.
    // Its clean retry installs a fresh identity and resets that binding's
    // counters, exactly as the queued transaction does.
    UsdGenDiagnostics changedRestCompileDiagnostics;
    auto changedRestPlan = CompileCudaGraph(ChangedRestDesc(),
                                            &changedRestCompileDiagnostics);
    CHECK(changedRestPlan && !changedRestCompileDiagnostics.HasErrors());
    failNextCudaSynchronousFinalizationForTesting();
    UsdGenDiagnostics changedRestFailureDiagnostics;
    CHECK(!ExecuteCudaGraph(*changedRestPlan, *workspace, 22.0, 203,
                            &changedRestFailureDiagnostics, retry) &&
          SameStats(GetCudaBindingStats(*workspace), retryStats));
    UsdGenDiagnostics changedRestRetryDiagnostics;
    auto changedRestRetry = ExecuteCudaGraph(*changedRestPlan, *workspace, 22.0, 204,
                                             &changedRestRetryDiagnostics, retry);
    auto const changedRestStats = GetCudaBindingStats(*workspace);
    CHECK(changedRestRetry && changedRestStats.size() == 1 &&
          changedRestStats.front().identity != retryStats.front().identity &&
          changedRestStats.front().bindCount == 1 &&
          changedRestStats.front().solveCount == 1 &&
          changedRestStats.front().sampleCount == 5);

    // A budget change is likewise part of the cache key. Failed finalization
    // retains the old identity; retry accepts the four-sample binding once.
    UsdGenDiagnostics budgetCompileDiagnostics;
    auto budgetPlan = CompileCudaGraph(ChangedBudgetDesc(), &budgetCompileDiagnostics);
    CHECK(budgetPlan && !budgetCompileDiagnostics.HasErrors());
    failNextCudaSynchronousFinalizationForTesting();
    UsdGenDiagnostics budgetFailureDiagnostics;
    CHECK(!ExecuteCudaGraph(*budgetPlan, *workspace, 23.0, 205,
                            &budgetFailureDiagnostics, changedRestRetry) &&
          SameStats(GetCudaBindingStats(*workspace), changedRestStats));
    UsdGenDiagnostics budgetRetryDiagnostics;
    auto budgetRetry = ExecuteCudaGraph(*budgetPlan, *workspace, 23.0, 206,
                                        &budgetRetryDiagnostics, changedRestRetry);
    auto const budgetStats = GetCudaBindingStats(*workspace);
    if (!budgetRetry || budgetStats.size() != 1 ||
        budgetStats.front().identity == changedRestStats.front().identity ||
        budgetStats.front().sampleCount != 4 ||
        budgetStats.front().bindCount != changedRestStats.front().bindCount + 1 ||
        budgetStats.front().solveCount != changedRestStats.front().solveCount + 1) {
        std::fprintf(stderr,
            "  budget retry result=%d errors=%zu stats=%zu old[id=%llu bind=%llu solve=%llu samples=%zu]",
            int(bool(budgetRetry)), budgetRetryDiagnostics.errors.size(), budgetStats.size(),
            static_cast<unsigned long long>(changedRestStats.front().identity),
            static_cast<unsigned long long>(changedRestStats.front().bindCount),
            static_cast<unsigned long long>(changedRestStats.front().solveCount),
            changedRestStats.front().sampleCount);
        if (!budgetStats.empty()) std::fprintf(stderr,
            " new[id=%llu bind=%llu solve=%llu samples=%zu]",
            static_cast<unsigned long long>(budgetStats.front().identity),
            static_cast<unsigned long long>(budgetStats.front().bindCount),
            static_cast<unsigned long long>(budgetStats.front().solveCount),
            budgetStats.front().sampleCount);
        if (!budgetRetryDiagnostics.errors.empty())
            std::fprintf(stderr, " error=%s", budgetRetryDiagnostics.errors.front().c_str());
        std::fprintf(stderr, "\n");
    }
    CHECK(budgetRetry && budgetStats.size() == 1 &&
          budgetStats.front().identity != changedRestStats.front().identity &&
          budgetStats.front().sampleCount == 4 &&
          budgetStats.front().bindCount == changedRestStats.front().bindCount + 1 &&
          budgetStats.front().solveCount == changedRestStats.front().solveCount + 1);

    // Accepted cache state is owned by the workspace, not by a completed
    // execution job. Published geometry remains charged by its generation,
    // while destroying the workspace releases the cache's child permits.
    auto cacheWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(cacheWorkspace);
    auto const cacheBaseline = resources->Snapshot();
    auto cacheGeneration = ExecuteCudaGraph(*plan, *cacheWorkspace, 24.0, 207,
                                            &diagnostics);
    CHECK(cacheGeneration && !diagnostics.HasErrors());
    auto const cacheHeld = resources->Snapshot();
    CHECK(cacheHeld.usedBytes > cacheBaseline.usedBytes &&
          ResourceKindBytes(cacheHeld, UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(cacheBaseline, UsdGenExecutionResourceKind::Pending));
    cacheWorkspace.reset();
    auto const cacheReleased = resources->Snapshot();
    CHECK(cacheReleased.usedBytes < cacheHeld.usedBytes &&
          ResourceKindBytes(cacheReleased, UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(cacheBaseline, UsdGenExecutionResourceKind::Pending));
    cacheGeneration.reset();

    // Once fresh solve work was submitted but its completion cannot be
    // proven, rollback is unsafe. The workspace is quarantined: its accepted
    // stats and retained generation remain readable and every retry fails.
    auto poisonedWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(poisonedWorkspace);
    UsdGenDiagnostics poisonBaseDiagnostics;
    auto poisonBase = ExecuteCudaGraph(*plan, *poisonedWorkspace, 30.0, 300,
                                       &poisonBaseDiagnostics);
    CHECK(poisonBase);
    auto const poisonStats = GetCudaBindingStats(*poisonedWorkspace);
    failNextCudaRbfSolveAfterInputSubmitForTesting();
    UsdGenDiagnostics poisonDiagnostics;
    CHECK(!ExecuteCudaGraph(*plan, *poisonedWorkspace, 31.0, 301,
                            &poisonDiagnostics, poisonBase) &&
          SameStats(GetCudaBindingStats(*poisonedWorkspace), poisonStats) &&
          SameGeneration(poisonBase, poisonBase));
    UsdGenDiagnostics poisonedRetryDiagnostics;
    CHECK(!ExecuteCudaGraph(*plan, *poisonedWorkspace, 32.0, 302,
                            &poisonedRetryDiagnostics, poisonBase) &&
          SameStats(GetCudaBindingStats(*poisonedWorkspace), poisonStats));
    return 0;
}

} // namespace

static int ValueDagTransactions() {
    auto width = [](char const* path, SdfPath input, float factor) {
        UsdGenNodeDesc node;
        node.path = SdfPath(path); node.type = TfToken("UsdGenWidth"); node.inputs = {input};
        node.params = {{TfToken("width"), VtValue(factor), false},
                       {TfToken("replace"), VtValue(false), false}};
        return node;
    };
    for (int variant = 0; variant != 8; ++variant) {
        std::fprintf(stderr, "RBF value DAG variant %d\n", variant);
        auto dag = RbfDesc();
        auto& curves = dag.curveSets.front();
        curves.curveVertexCounts = {2,3}; curves.curveId = {42,7};
        curves.points = {{.2f,.2f,0},{.2f,.4f,0},{.3f,.2f,0},{.3f,.7f,0},{.3f,1.2f,0}};
        curves.rest = curves.points; curves.skinPrim = {0,0};
        curves.skinPrimUv = {{.2f,.2f},{.3f,.2f}};
        UsdGenAuthoredPlaneDesc named;
        named.name = TfToken("rbfPointTag"); named.floatValues = {42,43,7,8,9};
        curves.authoredPlanes = {named};
        if (variant == 7) {
            curves.curveVertexCounts.clear(); curves.curveId.clear();
            curves.points.clear(); curves.rest.clear(); curves.skinPrim.clear(); curves.skinPrimUv.clear();
            curves.authoredPlanes.front().floatValues.clear();
        }
        auto source = dag.nodes.front();
        auto left = dag.nodes.back(); left.path = SdfPath("/RbfAtomic/LeftDeform");
        auto right = left; right.path = SdfPath("/RbfAtomic/RightDeform");
        auto sibling = width("/RbfAtomic/SiblingWidth", source.path, 3.f);
        auto reference = dag;
        reference.nodes = {source, left}; reference.terminal = left.path;
        dag.nodes = {source, left, sibling}; dag.terminal = left.path;
        if (variant == 1) {
            dag.terminal = source.path;
            reference.nodes = {source}; reference.terminal = source.path;
        } else if (variant == 2) {
            dag.terminal = sibling.path;
            reference.nodes = {source, sibling}; reference.terminal = sibling.path;
        } else if (variant == 3 || variant == 4 || variant == 6) {
            auto predecessor = width("/RbfAtomic/Predecessor", source.path, 2.f);
            if (variant != 3) {
                predecessor.type = TfToken("UsdGenLength");
                predecessor.params = variant == 6
                    ? std::vector<UsdGenParamValue>{{TfToken("length:mode"), VtValue(TfToken("cull")), false},
                                                  {TfToken("cullThreshold"), VtValue(.3f), false}}
                    : std::vector<UsdGenParamValue>{{TfToken("length:mode"), VtValue(TfToken("scale")), false},
                                                  {TfToken("length:value"), VtValue(.5f), false}};
            }
            left.inputs = {predecessor.path};
            dag.nodes = {source, predecessor, left, sibling};
            reference.nodes = {source, predecessor, left};
        } else if (variant == 5) {
            auto wl = width("/RbfAtomic/LeftWidth", left.path, 2.f);
            auto wr = width("/RbfAtomic/RightWidth", right.path, 3.f);
            UsdGenNodeDesc blend;
            blend.path = SdfPath("/RbfAtomic/Blend"); blend.type = TfToken("UsdGenWidthBlend");
            blend.inputs = {wl.path, wr.path};
            blend.params.push_back({TfToken("widthBlend:weight"), VtValue(.25f), false});
            dag.nodes = {source, left, right, wl, wr, blend}; dag.terminal = blend.path;
            auto expectedWidth = width("/RbfAtomic/ExpectedWidth", left.path, 2.25f);
            reference.nodes = {source, left, expectedWidth}; reference.terminal = expectedWidth.path;
        }
        UsdGenDiagnostics diagnostics;
        auto plan = CompileCudaGraph(dag, &diagnostics);
        if (!plan) for (auto const& error : diagnostics.errors) std::fprintf(stderr, "%s\n", error.c_str());
        CHECK(plan && !diagnostics.HasErrors());
        auto referencePlan = CompileCudaGraph(reference, &diagnostics); CHECK(referencePlan);
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        auto referenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace && referenceWorkspace);
        auto expected = ExecuteCudaGraph(*referencePlan, *referenceWorkspace, 1.0, 1000, &diagnostics);
        auto first = ExecuteCudaGraph(*plan, *workspace, 1.0, 1001, &diagnostics);
        CHECK(expected && first && !diagnostics.HasErrors() && SameGeneration(expected, first));
        if (variant == 7) {
            CHECK(first->Geometry().pointCount == 0 && first->Geometry().curveCount == 0 &&
                  first->Geometry().alreadyDeformed);
            UsdGenExecutionRuntime runtime(4);
            UsdGenCudaExecutionQueue queue(runtime, -1, &diagnostics);
            CHECK(queue.Valid() && queue.Submit(plan, 1.0)); queue.Drain();
            auto snapshot = queue.Snapshot();
            CHECK(snapshot && snapshot->generation && SameGeneration(expected, snapshot->generation));
            continue;
        }
        auto stats = GetCudaBindingStats(*workspace);
        CHECK(stats.size() == (variant == 5 ? 2 : 1));
        auto attempts = GetAttempts();
        failNextCudaSynchronousFinalizationForTesting();
        UsdGenDiagnostics failedDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 2.0, 1002, &failedDiagnostics, first));
        CHECK(failedDiagnostics.HasErrors() && SameStats(stats, GetCudaBindingStats(*workspace)) &&
              SameGeneration(expected, first));
        auto rejected = GetAttempts();
        if (rejected.rbfRollback != attempts.rbfRollback + stats.size() ||
            rejected.surfaceRollback != attempts.surfaceRollback + stats.size()) {
            std::fprintf(stderr, "rollback delta field=%llu surface=%llu expected=%zu\n",
                static_cast<unsigned long long>(rejected.rbfRollback - attempts.rbfRollback),
                static_cast<unsigned long long>(rejected.surfaceRollback - attempts.surfaceRollback), stats.size());
            for (auto const& error : failedDiagnostics.errors) std::fprintf(stderr, "%s\n", error.c_str());
        }
        CHECK(rejected.rbfAccept == attempts.rbfAccept && rejected.surfaceAccept == attempts.surfaceAccept);
        CHECK(rejected.rbfRollback == attempts.rbfRollback + stats.size() &&
              rejected.surfaceRollback == attempts.surfaceRollback + stats.size());
        auto retry = ExecuteCudaGraph(*plan, *workspace, 3.0, 1003, &diagnostics, first);
        CHECK(retry && SameGeneration(expected, retry) && SameGeneration(expected, first));
        auto after = GetCudaBindingStats(*workspace);
        CHECK(after.size() == stats.size());
        for (size_t i = 0; i != after.size(); ++i)
            CHECK(after[i].identity == stats[i].identity && after[i].solveCount == stats[i].solveCount + 1);
        UsdGenExecutionRuntime runtime(4);
        UsdGenCudaExecutionQueue queue(runtime, -1, &diagnostics);
        CHECK(queue.Valid() && queue.Submit(plan, 1.0)); queue.Drain();
        auto async = queue.Snapshot();
        CHECK(async && async->generation && async->bindings.size() == stats.size() &&
              SameGeneration(expected, async->generation));
        if (variant == 5) {
            auto beforeAsyncFailure = GetAttempts();
            failNextCudaFinalizationRelayAllocationForTesting();
            std::atomic<unsigned> callbacks{0};
            std::atomic<bool> failed{false};
            CHECK(queue.Submit(plan, 2.0, [&](uint64_t, auto outcome, auto const&) {
                ++callbacks;
                failed = outcome == UsdGenExecutionPipeline::Outcome::Failed;
            }));
            queue.Drain();
            CHECK(callbacks == 1 && failed && queue.Snapshot() == async &&
                  SameGeneration(expected, async->generation));
            auto failedAttempts = GetAttempts();
            CHECK(failedAttempts.rbfAccept == beforeAsyncFailure.rbfAccept &&
                  failedAttempts.surfaceAccept == beforeAsyncFailure.surfaceAccept &&
                  failedAttempts.rbfRollback == beforeAsyncFailure.rbfRollback + 2 &&
                  failedAttempts.surfaceRollback == beforeAsyncFailure.surfaceRollback + 2);
            CHECK(queue.Submit(plan, 3.0)); queue.Drain();
            CHECK(queue.Snapshot() != async && queue.Snapshot()->generation &&
                  SameGeneration(expected, queue.Snapshot()->generation) &&
                  SameGeneration(expected, async->generation));
        }
    }
    return 0;
}

// Exercise the aggregate ticket through both native async relays and the
// compatibility executor. Fill the pool to the reported boundary rather than
// relying on an unconstrained retry, which can hide a missing child charge.
static int LiteralRbfChainAdmission() {
    int device = -1;
    CHECK(cudaGetDevice(&device) == cudaSuccess);
    auto initializePool = gpu::TryReserveCudaExecutionBytes(0, UsdGenExecutionResourceKind::Active);
    CHECK(initializePool);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, device});
    CHECK(resources);
    auto drainRetirement = [device] {
        if (auto service = FindUsdGenExecutionRetirementService(
                {UsdGenExecutionResourceBackend::Cuda, device})) service->Drain();
    };
    auto const outerBaseline = resources->Snapshot();
    uint64_t singlePeak = 0;
    for (int variant = 0; variant != 4; ++variant) {
        {
        std::fprintf(stderr, "RBF aggregate admission variant %d\n", variant);
        auto desc = RbfDesc();
        auto& curves = desc.curveSets.front();
        curves.curveVertexCounts = {2, 3}; curves.curveId = {42, 7};
        curves.points = {{.2f,.2f,0},{.2f,.4f,0},
                         {.3f,.2f,0},{.3f,.7f,0},{.3f,1.2f,0}};
        curves.rest = curves.points; curves.skinPrim = {0,0};
        curves.skinPrimUv = {{.2f,.2f},{.3f,.2f}};
        UsdGenAuthoredPlaneDesc point, primitive, groom;
        point.name = TfToken("pointTag"); point.arity = 4;
        point.floatValues.resize(20);
        for (size_t i = 0; i != point.floatValues.size(); ++i)
            point.floatValues[i] = float(i) + .25f;
        primitive.name = TfToken("primitiveTag"); primitive.arity = 2;
        primitive.domain = UsdGenAuthoredPlaneDomain::Primitive;
        primitive.type = UsdGenAuthoredPlaneType::Int32;
        primitive.intValues = {42, -42, 7, -7};
        groom.name = TfToken("groomTag"); groom.arity = 4;
        groom.domain = UsdGenAuthoredPlaneDomain::Groom;
        groom.floatValues = {1, -2, 3, -4};
        curves.authoredPlanes = {point, primitive, groom};
        constexpr uint64_t namedBytes = (20 + 4 + 4) * sizeof(uint32_t);
        auto source = desc.nodes.front();
        auto deform = desc.nodes.back();
        desc.nodes = {source};
        auto addWidth = [&](char const* path, SdfPath input) {
            UsdGenNodeDesc width;
            width.path = SdfPath(path); width.type = TfToken("UsdGenWidth");
            width.inputs = {input};
            width.params = {{TfToken("width"), VtValue(.125f), false}};
            desc.nodes.push_back(width);
            return width.path;
        };
        unsigned widthCount = 0;
        if (variant == 1 || variant == 3) {
            deform.inputs = {addWidth("/RbfAtomic/Prefix", source.path)};
            ++widthCount;
        }
        desc.nodes.push_back(deform);
        desc.terminal = deform.path;
        if (variant == 2 || variant == 3) {
            desc.terminal = addWidth("/RbfAtomic/Tail", desc.terminal);
            desc.terminal = addWidth("/RbfAtomic/Tail2", desc.terminal);
            widthCount += 2;
        }
        UsdGenDiagnostics diagnostics;
        auto plan = CompileCudaGraph(desc, &diagnostics);
        CHECK(plan && !diagnostics.HasErrors());
        auto metadata = GetCudaExecutionPlanMetadata(*plan);
        CHECK(metadata && metadata->MemoryEstimate().runtimeRefinementAvailable &&
              !metadata->MemoryEstimate().memoryAvailable &&
              !metadata->MemoryEstimate().conservativeUpperBound);
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        auto const baseline = resources->Snapshot();
        auto filler = resources->TryReserve(baseline.usableBytes - baseline.usedBytes,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const saturated = resources->Snapshot();
        auto const beforeAttempts = GetAttempts();
        UsdGenDiagnostics rejectDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 1, 1, &rejectDiagnostics));
        uint64_t peak = 0;
        CHECK(ParseLiteralRbfPeak(rejectDiagnostics, &peak));
        auto const afterAttempts = GetAttempts();
        CHECK(afterAttempts.rbfAccept == beforeAttempts.rbfAccept &&
              afterAttempts.rbfRollback == beforeAttempts.rbfRollback &&
              afterAttempts.surfaceAccept == beforeAttempts.surfaceAccept &&
              afterAttempts.surfaceRollback == beforeAttempts.surfaceRollback &&
              GetCudaBindingStats(*workspace).empty() &&
              resources->Snapshot().byKind == saturated.byKind);
        // Named-channel accounting must be explicit, not absorbed accidentally
        // by cold-cache slack. The same descriptor without planes differs by
        // exactly their upload bytes.
        auto plainDesc = desc;
        plainDesc.curveSets.front().authoredPlanes.clear();
        auto plainPlan = CompileCudaGraph(plainDesc, &diagnostics);
        CHECK(plainPlan && !diagnostics.HasErrors());
        UsdGenDiagnostics plainReject;
        CHECK(!ExecuteCudaGraph(*plainPlan, *workspace, 1, 2, &plainReject));
        uint64_t plainPeak = 0;
        CHECK(ParseLiteralRbfPeak(plainReject, &plainPeak) && peak == plainPeak + namedBytes);
        if (!variant) singlePeak = peak;
        CHECK(peak == singlePeak + widthCount * (8 * curves.points.size() + 2 * 257 * 4 + 8));
        filler->Release();
        if (resources->Snapshot().byKind != baseline.byKind) {
            auto now = resources->Snapshot();
            for (size_t kind = 0; kind != now.byKind.size(); ++kind)
                std::fprintf(stderr, "aggregate baseline kind %zu: %zu -> %zu\n",
                    kind, baseline.byKind[kind], now.byKind[kind]);
        }
        CHECK(resources->Snapshot().byKind == baseline.byKind);
        // An independently owned reference makes retention checks meaningful:
        // comparing an old generation with itself cannot detect mutation.
        auto referenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(referenceWorkspace);
        auto expected = ExecuteCudaGraph(*plan, *referenceWorkspace, 1, 3, &diagnostics);
        CHECK(expected && !diagnostics.HasErrors());
        std::shared_ptr<const UsdGenDeviceGeneration> first;
        for (int warm = 0; warm != 2; ++warm) {
            drainRetirement();
            auto const before = resources->Snapshot();
            CHECK(before.usableBytes - before.usedBytes >= peak);
            filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak + 1,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto const full = resources->Snapshot();
            auto const stats = GetCudaBindingStats(*workspace);
            auto const attempts = GetAttempts();
            UsdGenDiagnostics belowDiagnostics;
            CHECK(!ExecuteCudaGraph(*plan, *workspace, 2 + warm, 4 + warm,
                                     &belowDiagnostics, first));
            uint64_t belowPeak = 0;
            auto const rejected = GetAttempts();
            CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == peak &&
                  resources->Snapshot().byKind == full.byKind &&
                  SameStats(stats, GetCudaBindingStats(*workspace)) &&
                  attempts.rbfAccept == rejected.rbfAccept &&
                  attempts.rbfRollback == rejected.rbfRollback &&
                  attempts.surfaceAccept == rejected.surfaceAccept &&
                  attempts.surfaceRollback == rejected.surfaceRollback);
            filler->Release();
            filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto result = ExecuteCudaGraph(*plan, *workspace, 2 + warm, 6 + warm,
                                           &diagnostics, first);
            if (!result) for (auto const& error : diagnostics.errors)
                std::fprintf(stderr, "exact-budget direct: %s\n", error.c_str());
            CHECK(result && !diagnostics.HasErrors() && SameGeneration(expected, result));
            CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
                  ResourceKindBytes(before, UsdGenExecutionResourceKind::Pending));
            if (first) CHECK(first->Owner() != result->Owner() && SameGeneration(expected, first));
            else first = result;
            filler->Release();
        }
        // Rejection after native work must roll back the candidate and leave
        // every retained plane and accepted cache identity untouched.
        {
            drainRetirement();
            auto const before = resources->Snapshot();
            auto const stats = GetCudaBindingStats(*workspace);
            auto const attempts = GetAttempts();
            CHECK(before.usableBytes - before.usedBytes >= peak);
            filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto const filled = resources->Snapshot();
            failNextCudaSynchronousFinalizationForTesting();
            UsdGenDiagnostics rollbackDiagnostics;
            CHECK(!ExecuteCudaGraph(*plan, *workspace, 3.5, 8, &rollbackDiagnostics, first));
            drainRetirement();
            auto const rejected = GetAttempts();
            // A clean rollback may discard previously cached scratch. It
            // must not grow any charged class or alter immutable data/cache.
            auto const rolledBack = resources->Snapshot();
            for (size_t kind = 0; kind != filled.byKind.size(); ++kind)
                CHECK(rolledBack.byKind[kind] <= filled.byKind[kind]);
            CHECK(rollbackDiagnostics.HasErrors() &&
                  SameStats(stats, GetCudaBindingStats(*workspace)) &&
                  SameGeneration(expected, first) &&
                  rejected.rbfAccept == attempts.rbfAccept &&
                  rejected.surfaceAccept == attempts.surfaceAccept &&
                  rejected.rbfRollback == attempts.rbfRollback + 1 &&
                  rejected.surfaceRollback == attempts.surfaceRollback + 1);
            filler->Release();
            auto const retryBaseline = resources->Snapshot();
            CHECK(retryBaseline.usableBytes - retryBaseline.usedBytes >= peak);
            filler = resources->TryReserve(retryBaseline.usableBytes - retryBaseline.usedBytes - peak,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto retry = ExecuteCudaGraph(*plan, *workspace, 3.5, 9, &diagnostics, first);
            CHECK(retry && SameGeneration(expected, retry) && SameGeneration(expected, first));
            filler->Release();
        }
        // Retain explicit native jobs across pressure snapshots. Queue::Drain
        // proves publication but need not retire the final callback's local
        // job owner; that later release would change an exact-byte baseline.
        auto asyncWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(asyncWorkspace);
        std::vector<std::shared_ptr<UsdGenCudaExecutionJob>> retainedJobs;
        std::vector<std::shared_ptr<const UsdGenDeviceGeneration>> retainedGenerations;
        auto runStage = [](auto launch) {
            auto promise = std::make_shared<std::promise<bool>>();
            auto future = promise->get_future();
            if (!launch([promise](bool success) { promise->set_value(success); })) return false;
            bool const success = future.get();
            waitForCudaNativeRelayRetirementForTesting();
            return success;
        };
        auto finalize = [](auto job) {
            auto promise = std::make_shared<std::promise<std::shared_ptr<const UsdGenDeviceGeneration>>>();
            auto future = promise->get_future();
            if (!FinalizeCudaExecutionJobAsync(job, [promise](auto result) { promise->set_value(result); }))
                return std::shared_ptr<const UsdGenDeviceGeneration>{};
            auto result = future.get();
            waitForCudaNativeRelayRetirementForTesting();
            return result;
        };
        for (int warm = 0; warm != 2; ++warm) {
            drainRetirement();
            auto const before = resources->Snapshot();
            CHECK(before.usableBytes - before.usedBytes >= peak);
            auto lastGood = retainedGenerations.empty() ? first : retainedGenerations.back();
            filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak + 1,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto const saturatedAsync = resources->Snapshot();
            auto const attempts = GetAttempts();
            UsdGenDiagnostics belowDiagnostics;
            CHECK(!CreateCudaExecutionJob(plan, *asyncWorkspace, 4 + warm, 10 + warm,
                                          &belowDiagnostics, lastGood));
            uint64_t belowPeak = 0;
            auto const rejectedAttempts = GetAttempts();
            CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == peak &&
                  resources->Snapshot().byKind == saturatedAsync.byKind &&
                  attempts.rbfAccept == rejectedAttempts.rbfAccept &&
                  attempts.rbfRollback == rejectedAttempts.rbfRollback &&
                  attempts.surfaceAccept == rejectedAttempts.surfaceAccept &&
                  attempts.surfaceRollback == rejectedAttempts.surfaceRollback);
            filler->Release();
            filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto job = CreateCudaExecutionJob(plan, *asyncWorkspace, 4 + warm, 12 + warm,
                                              &diagnostics, lastGood);
            CHECK(job);
            retainedJobs.push_back(job);
            CHECK(runStage([&](auto done) { return ExecuteCudaJobSourceAsync(job, done); }));
            for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i)
                CHECK(runStage([&](auto done) { return ExecuteCudaJobOperatorAsync(job, i, done); }));
            auto accepted = finalize(job);
            CHECK(accepted && accepted != lastGood &&
                  SameGeneration(expected, accepted) && SameGeneration(expected, first) &&
                  SameGeneration(expected, lastGood));
            retainedGenerations.push_back(accepted);
            CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
                  ResourceKindBytes(before, UsdGenExecutionResourceKind::Pending));
            filler->Release();
        }
        {
            auto lastGood = retainedGenerations.back();
            drainRetirement();
            auto const before = resources->Snapshot();
            auto const attempts = GetAttempts();
            CHECK(lastGood && before.usableBytes - before.usedBytes >= peak);
            filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto const stats = GetCudaBindingStats(*asyncWorkspace);
            auto job = CreateCudaExecutionJob(plan, *asyncWorkspace, 6, 14, &diagnostics, lastGood);
            CHECK(job);
            retainedJobs.push_back(job);
            CHECK(runStage([&](auto done) { return ExecuteCudaJobSourceAsync(job, done); }));
            for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i)
                CHECK(runStage([&](auto done) { return ExecuteCudaJobOperatorAsync(job, i, done); }));
            failNextCudaFinalizationRelayAllocationForTesting();
            CHECK(!finalize(job));
            auto const rejected = GetAttempts();
            CHECK(SameStats(stats, GetCudaBindingStats(*asyncWorkspace)) &&
                  SameGeneration(expected, lastGood) && SameGeneration(expected, first) &&
                  rejected.rbfAccept == attempts.rbfAccept &&
                  rejected.surfaceAccept == attempts.surfaceAccept &&
                  rejected.rbfRollback == attempts.rbfRollback + 1 &&
                  rejected.surfaceRollback == attempts.surfaceRollback + 1);
            filler->Release();
            auto const retryBaseline = resources->Snapshot();
            filler = resources->TryReserve(retryBaseline.usableBytes - retryBaseline.usedBytes - peak,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            UsdGenDiagnostics retryDiagnostics;
            auto retry = CreateCudaExecutionJob(plan, *asyncWorkspace, 6, 15, &retryDiagnostics, lastGood);
            CHECK(retry);
            retainedJobs.push_back(retry);
            CHECK(runStage([&](auto done) { return ExecuteCudaJobSourceAsync(retry, done); }));
            for (size_t i = 0; i != CudaExecutionJobOperatorCount(*retry); ++i)
                CHECK(runStage([&](auto done) { return ExecuteCudaJobOperatorAsync(retry, i, done); }));
            auto accepted = finalize(retry);
            CHECK(accepted && !retryDiagnostics.HasErrors() &&
                  SameGeneration(expected, accepted) && SameGeneration(expected, lastGood));
            retainedGenerations.push_back(accepted);
            filler->Release();
        }
        }
        // All callback stacks were joined before releasing the fixture's
        // retained owners. Join the normal deferred generation/consumer
        // retirement service too, then require exact per-kind recovery.
        drainRetirement();
        CHECK(resources->Snapshot().byKind == outerBaseline.byKind);
    }
    CHECK(resources->Snapshot().byKind == outerBaseline.byKind);
    return 0;
}

// Fixed-cardinality source-rooted RBF value DAG: Source feeds two independent
// literal sibling Deforms, each followed by a literal Width, joined by a
// WidthBlend terminal. Equal siblings prove admission + parity; the unequal
// variant proves the non-width join fails closed.
static UsdGenGraphDesc RbfLengthDagDesc() {
    auto desc = RbfDesc();
    auto& curves = desc.curveSets.front();
    curves.curveVertexCounts = {2, 3}; curves.curveId = {42, 7};
    curves.points = {{.2f,.2f,0},{.2f,.4f,0},
                     {.3f,.2f,0},{.3f,.7f,0},{.3f,1.2f,0}};
    curves.rest = curves.points; curves.skinPrim = {0,0};
    curves.skinPrimUv = {{.2f,.2f},{.3f,.2f}};
    UsdGenAuthoredPlaneDesc point;
    point.name = TfToken("pointTag"); point.arity = 4;
    point.floatValues.resize(20);
    for (size_t i = 0; i != point.floatValues.size(); ++i)
        point.floatValues[i] = float(i) + .25f;
    curves.authoredPlanes = {point};
    desc.surfaces.front().points[2][1] += .5f;
    auto source = desc.nodes.front();
    UsdGenNodeDesc length;
    length.path = SdfPath("/RbfAtomic/Length");
    length.type = TfToken("UsdGenLength");
    length.inputs = {source.path};
    length.params = {{TfToken("length:mode"), VtValue(TfToken("scale")), false},
                     {TfToken("length:value"), VtValue(.5f), false}};
    auto deform = [&](char const* path, SdfPath input) {
        UsdGenNodeDesc node;
        node.path = SdfPath(path);
        node.type = TfToken("UsdGenDeform");
        node.inputs = {input};
        node.surfaces = {desc.surfaces.front().path};
        node.mode = TfToken("rbf");
        node.readPhase = TfToken("final");
        node.params = {{TfToken("rbfSamples"), VtValue(5), false}};
        return node;
    };
    auto width = [](char const* path, SdfPath input, float factor) {
        UsdGenNodeDesc node;
        node.path = SdfPath(path);
        node.type = TfToken("UsdGenWidth");
        node.inputs = {input};
        node.params = {{TfToken("width"), VtValue(factor), false},
                       {TfToken("replace"), VtValue(false), false}};
        return node;
    };
    auto left = deform("/RbfAtomic/LeftDeform", length.path);
    auto right = deform("/RbfAtomic/RightDeform", length.path);
    auto wl = width("/RbfAtomic/LeftWidth", left.path, 2.f);
    auto wr = width("/RbfAtomic/RightWidth", right.path, 3.f);
    UsdGenNodeDesc blend;
    blend.path = SdfPath("/RbfAtomic/Blend");
    blend.type = TfToken("UsdGenWidthBlend");
    blend.inputs = {wl.path, wr.path};
    blend.blend = .25f;
    desc.nodes = {source, length, left, right, wl, wr, blend};
    desc.terminal = blend.path;
    return desc;
}

static UsdGenGraphDesc RbfGrowDagDesc() {
    auto desc = RbfLengthDagDesc();
    UsdGenNodeDesc grow;
    grow.path = SdfPath("/RbfAtomic/Grow");
    grow.type = TfToken("UsdGenGrow");
    grow.inputs = {desc.nodes.front().path};
    grow.seed = 19;
    grow.params = {{TfToken("segments"), VtValue(5), false},
                   {TfToken("length"), VtValue(2.f), false},
                   {TfToken("lengthRandom"), VtValue(GfVec2f(.5f,1.5f)), false}};
    desc.nodes[1] = grow;
    for (size_t i = 2; i != 4; ++i) desc.nodes[i].inputs = {grow.path};
    return desc;
}

static int ProveTopologyDagAdmission(
    std::shared_ptr<UsdGenExecutionResourcePool> const& resources, int device,
    UsdGenGraphDesc desc, char const* tag) {
    auto drainRetirement = [device] {
        if (auto service = FindUsdGenExecutionRetirementService(
                {UsdGenExecutionResourceBackend::Cuda, device})) service->Drain();
    };
    auto const outerBaseline = resources->Snapshot();
    UsdGenDiagnostics diagnostics;
    auto plan = CompileCudaGraph(desc, &diagnostics);
    if (!plan || diagnostics.HasErrors())
        for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "%s dag: %s\n", tag, error.c_str());
    CHECK(plan && !diagnostics.HasErrors());
    auto metadata = GetCudaExecutionPlanMetadata(*plan);
    CHECK(metadata && metadata->Shape() == UsdGenExecutionPlanShape::SourceRootedValueDag &&
          metadata->MemoryEstimate().runtimeRefinementAvailable &&
          !metadata->MemoryEstimate().memoryAvailable &&
          !metadata->MemoryEstimate().conservativeUpperBound);
    {
    auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    auto referenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(workspace && referenceWorkspace);
    auto expected = ExecuteCudaGraph(*plan, *referenceWorkspace, 1, 3, &diagnostics);
    if (!expected) for (auto const& error : diagnostics.errors)
        std::fprintf(stderr, "%s dag reference: %s\n", tag, error.c_str());
    CHECK(expected && !diagnostics.HasErrors());
    uint64_t peak = 0;
    {
        drainRetirement();
        auto const baseline = resources->Snapshot();
        auto filler = resources->TryReserve(baseline.usableBytes - baseline.usedBytes,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const saturated = resources->Snapshot();
        auto const beforeAttempts = GetAttempts();
        UsdGenDiagnostics rejectDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 1, 1, &rejectDiagnostics));
        auto const afterAttempts = GetAttempts();
        CHECK(ParseLiteralRbfPeak(rejectDiagnostics, &peak) &&
              afterAttempts.rbfAccept == beforeAttempts.rbfAccept &&
              afterAttempts.rbfRollback == beforeAttempts.rbfRollback &&
              afterAttempts.surfaceAccept == beforeAttempts.surfaceAccept &&
              afterAttempts.surfaceRollback == beforeAttempts.surfaceRollback &&
              GetCudaBindingStats(*workspace).empty() &&
              resources->Snapshot().byKind == saturated.byKind);
        filler->Release();
        CHECK(resources->Snapshot().byKind == baseline.byKind);
    }
    std::shared_ptr<const UsdGenDeviceGeneration> first;
    for (int warm = 0; warm != 2; ++warm) {
        drainRetirement();
        auto const before = resources->Snapshot();
        CHECK(before.usableBytes - before.usedBytes >= peak);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak + 1,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const full = resources->Snapshot();
        auto const stats = GetCudaBindingStats(*workspace);
        auto const attempts = GetAttempts();
        UsdGenDiagnostics belowDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 2 + warm, 4 + warm,
                                &belowDiagnostics, first));
        uint64_t belowPeak = 0;
        auto const rejected = GetAttempts();
        CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == peak &&
              resources->Snapshot().byKind == full.byKind &&
              SameStats(stats, GetCudaBindingStats(*workspace)) &&
              attempts.rbfAccept == rejected.rbfAccept &&
              attempts.rbfRollback == rejected.rbfRollback &&
              attempts.surfaceAccept == rejected.surfaceAccept &&
              attempts.surfaceRollback == rejected.surfaceRollback);
        filler->Release();
        filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                       UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto result = ExecuteCudaGraph(*plan, *workspace, 2 + warm, 6 + warm,
                                       &diagnostics, first);
        if (!result) for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "%s dag exact-budget direct: %s\n", tag, error.c_str());
        CHECK(result && !diagnostics.HasErrors() && SameGeneration(expected, result));
        CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(before, UsdGenExecutionResourceKind::Pending));
        if (first) CHECK(first->Owner() != result->Owner() && SameGeneration(expected, first));
        else first = result;
        filler->Release();
    }
    {
        drainRetirement();
        auto const before = resources->Snapshot();
        auto const stats = GetCudaBindingStats(*workspace);
        auto const attempts = GetAttempts();
        CHECK(stats.size() == 2 && before.usableBytes - before.usedBytes >= peak);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const filled = resources->Snapshot();
        failNextCudaSynchronousFinalizationForTesting();
        UsdGenDiagnostics rollbackDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 3.5, 8, &rollbackDiagnostics, first));
        drainRetirement();
        auto const rejected = GetAttempts();
        auto const rolledBack = resources->Snapshot();
        for (size_t kind = 0; kind != filled.byKind.size(); ++kind)
            CHECK(rolledBack.byKind[kind] <= filled.byKind[kind]);
        CHECK(rollbackDiagnostics.HasErrors() &&
              SameStats(stats, GetCudaBindingStats(*workspace)) &&
              SameGeneration(expected, first) &&
              rejected.rbfAccept == attempts.rbfAccept &&
              rejected.surfaceAccept == attempts.surfaceAccept &&
              rejected.rbfRollback == attempts.rbfRollback + 2 &&
              rejected.surfaceRollback == attempts.surfaceRollback + 2);
        filler->Release();
        auto const retryBaseline = resources->Snapshot();
        CHECK(retryBaseline.usableBytes - retryBaseline.usedBytes >= peak);
        filler = resources->TryReserve(retryBaseline.usableBytes - retryBaseline.usedBytes - peak,
                                       UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto retry = ExecuteCudaGraph(*plan, *workspace, 3.5, 9, &diagnostics, first);
        CHECK(retry && SameGeneration(expected, retry) && SameGeneration(expected, first));
        filler->Release();
    }
    {
        drainRetirement();
        auto const before = resources->Snapshot();
        CHECK(before.usableBytes - before.usedBytes >= peak);
        auto asyncWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(asyncWorkspace);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto job = CreateCudaExecutionJob(plan, *asyncWorkspace, 4, 12,
                                          &diagnostics, first);
        CHECK(job);
        auto sourcePromise = std::make_shared<std::promise<bool>>();
        auto sourceFuture = sourcePromise->get_future();
        CHECK(ExecuteCudaJobSourceAsync(job, [sourcePromise](bool ok) {
            sourcePromise->set_value(ok);
        }) && sourceFuture.get());
        for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i) {
            auto opPromise = std::make_shared<std::promise<bool>>();
            auto opFuture = opPromise->get_future();
            bool const launched = ExecuteCudaJobOperatorAsync(job, i, [opPromise](bool ok) {
                opPromise->set_value(ok);
            });
            bool const ok = launched && opFuture.get();
            if (!ok) std::fprintf(stderr, "%s dag async operator %zu/%zu launched=%d\n",
                tag, i, CudaExecutionJobOperatorCount(*job), launched);
            CHECK(ok);
        }
        auto finalPromise = std::make_shared<std::promise<std::shared_ptr<const UsdGenDeviceGeneration>>>();
        auto finalFuture = finalPromise->get_future();
        CHECK(FinalizeCudaExecutionJobAsync(job, [finalPromise](auto result) {
            finalPromise->set_value(result);
        }));
        auto accepted = finalFuture.get();
        waitForCudaNativeRelayRetirementForTesting();
        CHECK(accepted && accepted != first && SameGeneration(expected, accepted));
        filler->Release();
    }
    }
    drainRetirement();
    {
        auto now = resources->Snapshot();
        if (now.byKind != outerBaseline.byKind)
            for (size_t kind = 0; kind != now.byKind.size(); ++kind)
                std::fprintf(stderr, "%s outer kind %zu: %zu -> %zu\n",
                    tag, kind, outerBaseline.byKind[kind], now.byKind[kind]);
    }
    CHECK(resources->Snapshot().byKind == outerBaseline.byKind);
    return 0;
}

static int LiteralRbfTopologyDagAdmission() {
    int device = -1;
    CHECK(cudaGetDevice(&device) == cudaSuccess);
    auto initializePool = gpu::TryReserveCudaExecutionBytes(0, UsdGenExecutionResourceKind::Active);
    CHECK(initializePool);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, device});
    CHECK(resources);
    CHECK(ProveTopologyDagAdmission(resources, device, RbfLengthDagDesc(), "length") == 0);
    CHECK(ProveTopologyDagAdmission(resources, device, RbfGrowDagDesc(), "grow") == 0);
    return 0;
}

static UsdGenGraphDesc RbfDagDesc(bool unequalRight);

static int LiteralRbfMultiGroomAdmission() {
    int device = -1;
    CHECK(cudaGetDevice(&device) == cudaSuccess);
    auto initializePool = gpu::TryReserveCudaExecutionBytes(0, UsdGenExecutionResourceKind::Active);
    CHECK(initializePool);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, device});
    CHECK(resources);
    auto drainRetirement = [device] {
        if (auto service = FindUsdGenExecutionRetirementService(
                {UsdGenExecutionResourceBackend::Cuda, device})) service->Drain();
    };
    auto const outerBaseline = resources->Snapshot();
    // Two independent grooms share one device pool. Identical descriptions
    // must admit identical peaks through independent workspaces, contend
    // gracefully under pressure, and never observe each other's caches.
    UsdGenDiagnostics diagnostics;
    auto descA = RbfDagDesc(false);
    auto descB = RbfDagDesc(false);
    auto planA = CompileCudaGraph(descA, &diagnostics);
    auto planB = CompileCudaGraph(descB, &diagnostics);
    CHECK(planA && planB && !diagnostics.HasErrors());
    auto workspaceA = CreateCudaExecutionWorkspace(-1, &diagnostics);
    auto workspaceB = CreateCudaExecutionWorkspace(-1, &diagnostics);
    auto referenceA = CreateCudaExecutionWorkspace(-1, &diagnostics);
    auto referenceB = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(workspaceA && workspaceB && referenceA && referenceB);
    auto expectedA = ExecuteCudaGraph(*planA, *referenceA, 1, 1, &diagnostics);
    auto expectedB = ExecuteCudaGraph(*planB, *referenceB, 1, 2, &diagnostics);
    CHECK(expectedA && expectedB && !diagnostics.HasErrors() &&
          SameGeneration(expectedA, expectedB));
    uint64_t peakA = 0, peakB = 0;
    {
        drainRetirement();
        auto const baseline = resources->Snapshot();
        auto filler = resources->TryReserve(baseline.usableBytes - baseline.usedBytes,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const saturated = resources->Snapshot();
        UsdGenDiagnostics rejectA, rejectB;
        CHECK(!ExecuteCudaGraph(*planA, *workspaceA, 1, 3, &rejectA));
        CHECK(!ExecuteCudaGraph(*planB, *workspaceB, 1, 4, &rejectB));
        CHECK(ParseLiteralRbfPeak(rejectA, &peakA) &&
              ParseLiteralRbfPeak(rejectB, &peakB) && peakA == peakB &&
              peakA != 0 && resources->Snapshot().byKind == saturated.byKind);
        filler->Release();
        CHECK(resources->Snapshot().byKind == baseline.byKind);
    }
    std::shared_ptr<const UsdGenDeviceGeneration> firstA, firstB;
    {
        drainRetirement();
        auto const before = resources->Snapshot();
        CHECK(before.usableBytes - before.usedBytes >= peakA + peakB);
        auto filler = resources->TryReserve(
            before.usableBytes - before.usedBytes - peakA - peakB,
            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto resultA = ExecuteCudaGraph(*planA, *workspaceA, 2, 5, &diagnostics, firstA);
        auto resultB = ExecuteCudaGraph(*planB, *workspaceB, 2, 6, &diagnostics, firstB);
        if ((!resultA || !resultB) && diagnostics.HasErrors())
            for (auto const& error : diagnostics.errors)
                std::fprintf(stderr, "multi-groom exact: %s\n", error.c_str());
        CHECK(resultA && resultB && !diagnostics.HasErrors() &&
              SameGeneration(expectedA, resultA) && SameGeneration(expectedB, resultB) &&
              resultA->Owner() != resultB->Owner());
        CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(before, UsdGenExecutionResourceKind::Pending));
        firstA = resultA;
        firstB = resultB;
        filler->Release();
    }
    {
        // Contention through held reservations: an async job for groom A
        // holds its exact peak while groom B's creation must fail below
        // budget, then succeed after A releases. Neither publication may
        // move, and no attempt counter may advance on the rejection.
        drainRetirement();
        auto const before = resources->Snapshot();
        CHECK(before.usableBytes - before.usedBytes >= peakA + peakB);
        auto filler = resources->TryReserve(
            before.usableBytes - before.usedBytes - peakA - peakB + 1,
            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto asyncA = CreateCudaExecutionWorkspace(-1, &diagnostics);
        auto asyncB = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(asyncA && asyncB);
        std::vector<std::shared_ptr<UsdGenCudaExecutionJob>> retainedJobs;
        auto jobA = CreateCudaExecutionJob(planA, *asyncA, 3, 7,
                                           &diagnostics, firstA);
        CHECK(jobA);
        retainedJobs.push_back(jobA);
        auto const full = resources->Snapshot();
        auto const attempts = GetAttempts();
        UsdGenDiagnostics belowDiagnostics;
        CHECK(!CreateCudaExecutionJob(planB, *asyncB, 3, 8,
                                      &belowDiagnostics, firstB));
        uint64_t belowPeak = 0;
        auto const rejected = GetAttempts();
        CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == peakB &&
              resources->Snapshot().byKind == full.byKind &&
              attempts.rbfAccept == rejected.rbfAccept &&
              attempts.rbfRollback == rejected.rbfRollback &&
              attempts.surfaceAccept == rejected.surfaceAccept &&
              attempts.surfaceRollback == rejected.surfaceRollback);
        auto runStage = [](auto job, auto launch) {
            auto promise = std::make_shared<std::promise<bool>>();
            auto future = promise->get_future();
            if (!launch(job, [promise](bool success) { promise->set_value(success); }))
                return false;
            return future.get();
        };
        CHECK(runStage(jobA, [](auto job, auto done) {
            return ExecuteCudaJobSourceAsync(job, done);
        }));
        for (size_t i = 0; i != CudaExecutionJobOperatorCount(*jobA); ++i)
            CHECK(runStage(jobA, [i](auto job, auto done) {
                return ExecuteCudaJobOperatorAsync(job, i, done);
            }));
        auto finalPromise = std::make_shared<std::promise<std::shared_ptr<const UsdGenDeviceGeneration>>>();
        auto finalFuture = finalPromise->get_future();
        CHECK(FinalizeCudaExecutionJobAsync(jobA, [finalPromise](auto result) {
            finalPromise->set_value(std::move(result));
        }));
        auto acceptedA = finalFuture.get();
        waitForCudaNativeRelayRetirementForTesting();
        CHECK(acceptedA && SameGeneration(expectedA, acceptedA));
        filler->Release();
        retainedJobs.clear();
        drainRetirement();
        auto const retryBaseline = resources->Snapshot();
        CHECK(retryBaseline.usableBytes - retryBaseline.usedBytes >= peakB);
        filler = resources->TryReserve(
            retryBaseline.usableBytes - retryBaseline.usedBytes - peakB,
            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto retryB = ExecuteCudaGraph(*planB, *workspaceB, 3, 9, &diagnostics, firstB);
        CHECK(retryB && SameGeneration(expectedB, retryB));
        filler->Release();
        firstA = acceptedA;
        firstB = retryB;
    }
    firstA.reset();
    firstB.reset();
    expectedA.reset();
    expectedB.reset();
    workspaceA.reset();
    workspaceB.reset();
    referenceA.reset();
    referenceB.reset();
    drainRetirement();
    CHECK(resources->Snapshot().byKind == outerBaseline.byKind);
    return 0;
}

static UsdGenGraphDesc RbfParamMapDagDesc() {
    auto desc = RbfDagDesc(false);
    UsdGenExpressionDesc widthExpression;
    widthExpression.path = SdfPath("/RbfAtomic/Expressions/dagWidth");
    widthExpression.source = "$value";
    widthExpression.outputs.push_back({TfToken("result"), TfToken("float"),
                                       {expr::ScalarType::Float32, 1, 1, 1, 1, false}});
    desc.expressions.push_back(widthExpression);
    UsdGenExpressionBinding widthBinding;
    widthBinding.expression = widthExpression.path;
    widthBinding.destination = TfToken("width");
    widthBinding.domain = expr::Domain::Groom;
    widthBinding.nativeType = TfToken("float");
    widthBinding.destinationShape = {expr::ScalarType::Float32, 1, 1, 1, 1, false};
    widthBinding.literal = VtValue(1.f);
    desc.nodes[3].expressionBindings.push_back(widthBinding);
    UsdGenMapDesc map;
    map.path = SdfPath("/RbfAtomic/Mask");
    map.type = TfToken("UsdGenImageMap");
    map.textureGeneration = 1;
    map.imagePayload = ImagePayload::Create(1, 1, 1, std::vector<float>{.5f},
        UsdGenImageRowOrientation::BottomUp);
    desc.maps.push_back(std::move(map));
    desc.nodes[4].mapBindings = {{SdfPath("/RbfAtomic/Mask"),
        UsdGenMapBindingPurpose::MaskSource, TfToken("usdGen:mask:source")}};
    return desc;
}

static int LiteralRbfParamMapDagAdmission() {
    int device = -1;
    CHECK(cudaGetDevice(&device) == cudaSuccess);
    auto initializePool = gpu::TryReserveCudaExecutionBytes(0, UsdGenExecutionResourceKind::Active);
    CHECK(initializePool);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, device});
    CHECK(resources);
    auto drainRetirement = [device] {
        if (auto service = FindUsdGenExecutionRetirementService(
                {UsdGenExecutionResourceBackend::Cuda, device})) service->Drain();
    };
    auto const outerBaseline = resources->Snapshot();
    UsdGenDiagnostics diagnostics;
    auto desc = RbfParamMapDagDesc();
    auto plan = CompileCudaGraph(desc, &diagnostics);
    if (!plan || diagnostics.HasErrors())
        for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "parammap dag: %s\n", error.c_str());
    CHECK(plan && !diagnostics.HasErrors());
    auto metadata = GetCudaExecutionPlanMetadata(*plan);
    CHECK(metadata && metadata->Shape() == UsdGenExecutionPlanShape::SourceRootedValueDag &&
          metadata->MemoryEstimate().runtimeRefinementAvailable &&
          !metadata->MemoryEstimate().memoryAvailable &&
          !metadata->MemoryEstimate().conservativeUpperBound);
    {
    auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    auto referenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(workspace && referenceWorkspace);
    auto expected = ExecuteCudaGraph(*plan, *referenceWorkspace, 1, 3, &diagnostics);
    if (!expected) for (auto const& error : diagnostics.errors)
        std::fprintf(stderr, "parammap dag reference: %s\n", error.c_str());
    CHECK(expected && !diagnostics.HasErrors());
    uint64_t peak = 0;
    {
        drainRetirement();
        auto const baseline = resources->Snapshot();
        auto filler = resources->TryReserve(baseline.usableBytes - baseline.usedBytes,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const saturated = resources->Snapshot();
        auto const beforeAttempts = GetAttempts();
        UsdGenDiagnostics rejectDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 1, 1, &rejectDiagnostics));
        auto const afterAttempts = GetAttempts();
        CHECK(ParseLiteralRbfPeak(rejectDiagnostics, &peak) &&
              afterAttempts.rbfAccept == beforeAttempts.rbfAccept &&
              afterAttempts.rbfRollback == beforeAttempts.rbfRollback &&
              afterAttempts.surfaceAccept == beforeAttempts.surfaceAccept &&
              afterAttempts.surfaceRollback == beforeAttempts.surfaceRollback &&
              GetCudaBindingStats(*workspace).empty() &&
              resources->Snapshot().byKind == saturated.byKind);
        filler->Release();
        CHECK(resources->Snapshot().byKind == baseline.byKind);
    }
    std::shared_ptr<const UsdGenDeviceGeneration> first;
    for (int warm = 0; warm != 2; ++warm) {
        drainRetirement();
        auto const before = resources->Snapshot();
        CHECK(before.usableBytes - before.usedBytes >= peak);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak + 1,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const full = resources->Snapshot();
        auto const stats = GetCudaBindingStats(*workspace);
        auto const attempts = GetAttempts();
        UsdGenDiagnostics belowDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 2 + warm, 4 + warm,
                                &belowDiagnostics, first));
        uint64_t belowPeak = 0;
        auto const rejected = GetAttempts();
        CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == peak &&
              resources->Snapshot().byKind == full.byKind &&
              SameStats(stats, GetCudaBindingStats(*workspace)) &&
              attempts.rbfAccept == rejected.rbfAccept &&
              attempts.rbfRollback == rejected.rbfRollback &&
              attempts.surfaceAccept == rejected.surfaceAccept &&
              attempts.surfaceRollback == rejected.surfaceRollback);
        filler->Release();
        filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                       UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto result = ExecuteCudaGraph(*plan, *workspace, 2 + warm, 6 + warm,
                                       &diagnostics, first);
        if (!result) for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "parammap dag exact-budget direct: %s\n", error.c_str());
        CHECK(result && !diagnostics.HasErrors() && SameGeneration(expected, result));
        CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(before, UsdGenExecutionResourceKind::Pending));
        if (first) CHECK(first->Owner() != result->Owner() && SameGeneration(expected, first));
        else first = result;
        filler->Release();
    }
    {
        drainRetirement();
        auto const before = resources->Snapshot();
        auto const stats = GetCudaBindingStats(*workspace);
        auto const attempts = GetAttempts();
        CHECK(stats.size() == 2 && before.usableBytes - before.usedBytes >= peak);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const filled = resources->Snapshot();
        failNextCudaSynchronousFinalizationForTesting();
        UsdGenDiagnostics rollbackDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 3.5, 8, &rollbackDiagnostics, first));
        drainRetirement();
        auto const rejected = GetAttempts();
        auto const rolledBack = resources->Snapshot();
        for (size_t kind = 0; kind != filled.byKind.size(); ++kind)
            CHECK(rolledBack.byKind[kind] <= filled.byKind[kind]);
        CHECK(rollbackDiagnostics.HasErrors() &&
              SameStats(stats, GetCudaBindingStats(*workspace)) &&
              SameGeneration(expected, first) &&
              rejected.rbfAccept == attempts.rbfAccept &&
              rejected.surfaceAccept == attempts.surfaceAccept &&
              rejected.rbfRollback == attempts.rbfRollback + 2 &&
              rejected.surfaceRollback == attempts.surfaceRollback + 2);
        filler->Release();
        auto const retryBaseline = resources->Snapshot();
        CHECK(retryBaseline.usableBytes - retryBaseline.usedBytes >= peak);
        filler = resources->TryReserve(retryBaseline.usableBytes - retryBaseline.usedBytes - peak,
                                       UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto retry = ExecuteCudaGraph(*plan, *workspace, 3.5, 9, &diagnostics, first);
        CHECK(retry && SameGeneration(expected, retry) && SameGeneration(expected, first));
        filler->Release();
    }
    {
        auto asyncWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(asyncWorkspace);
        auto job = CreateCudaExecutionJob(plan, *asyncWorkspace, 4, 12,
                                          &diagnostics, first);
        CHECK(job);
        auto sourcePromise = std::make_shared<std::promise<bool>>();
        auto sourceFuture = sourcePromise->get_future();
        CHECK(ExecuteCudaJobSourceAsync(job, [sourcePromise](bool ok) {
            sourcePromise->set_value(ok);
        }) && sourceFuture.get());
        for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i) {
            auto opPromise = std::make_shared<std::promise<bool>>();
            auto opFuture = opPromise->get_future();
            bool const launched = ExecuteCudaJobOperatorAsync(job, i, [opPromise](bool ok) {
                opPromise->set_value(ok);
            });
            bool const ok = launched && opFuture.get();
            if (!ok) std::fprintf(stderr, "parammap dag async operator %zu/%zu launched=%d\n",
                i, CudaExecutionJobOperatorCount(*job), launched);
            CHECK(ok);
        }
        auto finalPromise = std::make_shared<std::promise<std::shared_ptr<const UsdGenDeviceGeneration>>>();
        auto finalFuture = finalPromise->get_future();
        CHECK(FinalizeCudaExecutionJobAsync(job, [finalPromise](auto result) {
            finalPromise->set_value(result);
        }));
        auto accepted = finalFuture.get();
        waitForCudaNativeRelayRetirementForTesting();
        CHECK(accepted && accepted != first && SameGeneration(expected, accepted));
    }
    }
    drainRetirement();
    {
        auto now = resources->Snapshot();
        if (now.byKind != outerBaseline.byKind)
            for (size_t kind = 0; kind != now.byKind.size(); ++kind)
                std::fprintf(stderr, "parammap outer kind %zu: %zu -> %zu\n",
                    kind, outerBaseline.byKind[kind], now.byKind[kind]);
    }
    CHECK(resources->Snapshot().byKind == outerBaseline.byKind);
    return 0;
}

static bool DiagnosticContains(UsdGenDiagnostics const& diagnostics, char const* text) {
    for (auto const& error : diagnostics.errors)
        if (error.find(text) != std::string::npos) return true;
    return false;
}

static bool ReadWidths(std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
                       std::vector<float>* widths) {
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
        return false;
    bool ok = false;
    {
        auto lease = gpu::AcquireGeometry(generation, stream);
        if (lease) {
            auto const geometry = lease.Geometry();
            widths->resize(geometry.widths.size);
            ok = (widths->empty() || cudaMemcpyAsync(widths->data(),
                    geometry.widths.data, widths->size() * sizeof(float),
                    cudaMemcpyDeviceToHost, stream) == cudaSuccess) &&
                cudaStreamSynchronize(stream) == cudaSuccess;
        }
    }
    ok = cudaStreamSynchronize(stream) == cudaSuccess && ok;
    cudaStreamDestroy(stream);
    return ok;
}

struct ReleaseWidthBranchGate {
    bool armed = true;
    ~ReleaseWidthBranchGate() {
        if (armed) releaseCudaOperatorAsyncWidthBranchGateForTesting();
    }
    void Release() {
        if (armed) releaseCudaOperatorAsyncWidthBranchGateForTesting();
        armed = false;
    }
};

// Two independent WidthBlends over disjoint Width branches. Both blends
// already take branch streams; this test witnesses their hardware overlap
// without changing scheduler behavior.
static UsdGenGraphDesc BlendOverlapDesc(SdfPath const& terminal) {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/BlendOverlap");
    desc.executionBackend = UsdGenExecutionBackend::Cuda;
    UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/BlendOverlap/Curves");
    curves.role = UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.curveVertexCounts = {2, 3};
    curves.curveId = {42, 7};
    curves.points = {{.2f,.2f,0},{.2f,.4f,0},
                     {.3f,.2f,0},{.3f,.7f,0},{.3f,1.2f,0}};
    curves.rest = curves.points;
    curves.skinPrim = {0, 0};
    curves.skinPrimUv = {{.2f,.2f},{.3f,.2f}};
    desc.curveSets.push_back(curves);
    UsdGenSurfaceDesc scalp;
    scalp.path = SdfPath("/BlendOverlap/Scalp");
    scalp.restPoints = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    scalp.points = scalp.restPoints;
    scalp.faceVertexCounts = {3,3,3};
    scalp.faceVertexIndices = {0,1,2, 0,1,3, 1,2,4};
    desc.surfaces.push_back(scalp);
    UsdGenNodeDesc source;
    source.path = SdfPath("/BlendOverlap/Source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.surfaces = {scalp.path};
    auto width = [](char const* path, SdfPath input, float factor) {
        UsdGenNodeDesc node;
        node.path = SdfPath(path);
        node.type = TfToken("UsdGenWidth");
        node.inputs = {input};
        node.params = {{TfToken("width"), VtValue(factor), false},
                       {TfToken("replace"), VtValue(true), false}};
        return node;
    };
    auto wl1 = width("/BlendOverlap/LeftWidth1", source.path, 2.f);
    auto wr1 = width("/BlendOverlap/RightWidth1", source.path, 3.f);
    auto wl2 = width("/BlendOverlap/LeftWidth2", source.path, 4.f);
    auto wr2 = width("/BlendOverlap/RightWidth2", source.path, 5.f);
    UsdGenNodeDesc b1, b2;
    b1.path = SdfPath("/BlendOverlap/Blend1");
    b1.type = TfToken("UsdGenWidthBlend");
    b1.inputs = {wl1.path, wr1.path};
    b1.blend = .25f;
    b2.path = SdfPath("/BlendOverlap/Blend2");
    b2.type = TfToken("UsdGenWidthBlend");
    b2.inputs = {wl2.path, wr2.path};
    b2.blend = .5f;
    desc.nodes = {source, wl1, wr1, wl2, wr2, b1, b2};
    desc.terminal = terminal;
    return desc;
}

static int BlendOverlapWitness() {
    int device = -1;
    CHECK(cudaGetDevice(&device) == cudaSuccess);
    auto initializePool = gpu::TryReserveCudaExecutionBytes(0, UsdGenExecutionResourceKind::Active);
    CHECK(initializePool);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, device});
    CHECK(resources);
    auto drainRetirement = [device] {
        if (auto service = FindUsdGenExecutionRetirementService(
                {UsdGenExecutionResourceBackend::Cuda, device})) service->Drain();
    };
    auto const outerBaseline = resources->Snapshot();
    UsdGenDiagnostics diagnostics;
    // Absolute values first: blend arithmetic is exact for these factors.
    auto descB1 = BlendOverlapDesc(SdfPath("/BlendOverlap/Blend1"));
    auto planB1 = CompileCudaGraph(descB1, &diagnostics);
    CHECK(planB1 && !diagnostics.HasErrors());
    auto descB2 = BlendOverlapDesc(SdfPath("/BlendOverlap/Blend2"));
    auto planB2 = CompileCudaGraph(descB2, &diagnostics);
    CHECK(planB2 && !diagnostics.HasErrors());
    auto referenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(referenceWorkspace);
    auto expectedB1 = ExecuteCudaGraph(*planB1, *referenceWorkspace, 1, 1, &diagnostics);
    auto expectedB2 = ExecuteCudaGraph(*planB2, *referenceWorkspace, 1, 2, &diagnostics);
    if ((!expectedB1 || !expectedB2) || diagnostics.HasErrors())
        for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "blend ref: %s\n", error.c_str());
    CHECK(expectedB1 && expectedB2 && !diagnostics.HasErrors());
    {
        std::vector<float> widths;
        CHECK(ReadWidths(expectedB1, &widths) && widths.size() == 5 &&
              std::all_of(widths.begin(), widths.end(),
                  [](float value) { return std::fabs(value - 2.25f) < 1e-6f; }));
        CHECK(ReadWidths(expectedB2, &widths) && widths.size() == 5 &&
              std::all_of(widths.begin(), widths.end(),
                  [](float value) { return std::fabs(value - 4.5f) < 1e-6f; }));
    }
    {
        // Witness run: source plus four Widths complete first; the two
        // blends rendezvous on the branch gate, then their device probes
        // must overlap where the device allows concurrent kernels.
        auto asyncWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(asyncWorkspace);
        auto job = CreateCudaExecutionJob(planB1, *asyncWorkspace, 2, 3,
                                          &diagnostics, nullptr);
        CHECK(job);
        std::vector<size_t> blends;
        for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i) {
            if (CudaExecutionJobOperatorType(*job, i) == TfToken("UsdGenWidthBlend"))
                blends.push_back(i);
        }
        CHECK(blends.size() == 2);
        auto runStage = [](auto launch) {
            auto promise = std::make_shared<std::promise<bool>>();
            auto future = promise->get_future();
            if (!launch([promise](bool success) { promise->set_value(success); }))
                return false;
            return future.get();
        };
        CHECK(runStage([&](auto done) {
            return ExecuteCudaJobSourceAsync(job, done);
        }));
        for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i) {
            if (i == blends[0] || i == blends[1]) continue;
            CHECK(runStage([&](auto done) {
                return ExecuteCudaJobOperatorAsync(job, i, done);
            }));
        }
        drainRetirement();
        armCudaOperatorAsyncWidthBranchGateForTesting(2);
        ReleaseWidthBranchGate releaseGate;
        armCudaOperatorAsyncWidthDeviceOverlapWitnessForTesting(2, 200000000);
        struct LaunchResult { bool launched = false; bool ok = false; };
        auto launchBlend = [&](size_t index) {
            LaunchResult result;
            auto promise = std::make_shared<std::promise<bool>>();
            auto future = promise->get_future();
            result.launched = ExecuteCudaJobOperatorAsync(job, index,
                [promise](bool success) { promise->set_value(success); });
            if (result.launched) result.ok = future.get();
            return result;
        };
        LaunchResult results[2];
        std::thread first([&] { results[0] = launchBlend(blends[0]); });
        std::thread second([&] { results[1] = launchBlend(blends[1]); });
        bool rendezvous = false;
        try {
            waitCudaOperatorAsyncWidthBranchGateForTesting();
            rendezvous = true;
        } catch (std::exception const& error) {
            std::fprintf(stderr, "blend rendezvous failed: %s\n", error.what());
        }
        releaseGate.Release();
        first.join();
        second.join();
        CHECK(rendezvous && results[0].launched && results[0].ok &&
              results[1].launched && results[1].ok);
        auto promise = std::make_shared<std::promise<std::shared_ptr<const UsdGenDeviceGeneration>>>();
        auto future = promise->get_future();
        CHECK(FinalizeCudaExecutionJobAsync(job, [promise](auto result) {
            promise->set_value(std::move(result));
        }));
        auto accepted = future.get();
        waitForCudaNativeRelayRetirementForTesting();
        CHECK(accepted && SameGeneration(expectedB1, accepted));
        auto const overlap = cudaOperatorAsyncWidthDeviceOverlapWitnessSnapshotForTesting();
        CHECK(overlap.expected == 2 && overlap.claims == 2 && overlap.arrivals == 2 &&
              overlap.status != UsdGenExecutionOverlapWitnessStatus::Error &&
              overlap.taskIds[0] != overlap.taskIds[1] &&
              overlap.laneIds[0] != overlap.laneIds[1]);
        int currentDevice = -1;
        cudaDeviceProp properties{};
        bool const canOverlap =
            cudaGetDevice(&currentDevice) == cudaSuccess &&
            cudaGetDeviceProperties(&properties, currentDevice) == cudaSuccess &&
            properties.concurrentKernels != 0;
        if (canOverlap)
            CHECK(overlap.status == UsdGenExecutionOverlapWitnessStatus::Observed &&
                  overlap.maxActive >= 2);
        else
            CHECK(overlap.maxActive <= 2);
        CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(outerBaseline, UsdGenExecutionResourceKind::Pending));
        disarmCudaOperatorAsyncWidthDeviceOverlapWitnessForTesting();
    }
    // Reference generations and their workspace (including its blend graph
    // cache) hold pool permits until released; drop them before recovery.
    expectedB1.reset();
    expectedB2.reset();
    referenceWorkspace.reset();
    drainRetirement();
    CHECK(resources->Snapshot().byKind == outerBaseline.byKind);
    return 0;
}

static UsdGenGraphDesc RbfDagDesc(bool unequalRight = false) {
    auto desc = RbfDesc();
    // A non-affine bump: approximations with different sample budgets must
    // differ, so the unequal sibling variant observes a real join mismatch.
    // A pure translation lives in the RBF polynomial term and solves exactly
    // at every budget, which cannot discriminate the join.
    desc.surfaces.front().points[2][1] += .5f;
    auto& curves = desc.curveSets.front();
    curves.curveVertexCounts = {2, 3}; curves.curveId = {42, 7};
    curves.points = {{.2f,.2f,0},{.2f,.4f,0},
                     {.3f,.2f,0},{.3f,.7f,0},{.3f,1.2f,0}};
    curves.rest = curves.points; curves.skinPrim = {0,0};
    curves.skinPrimUv = {{.2f,.2f},{.3f,.2f}};
    UsdGenAuthoredPlaneDesc point;
    point.name = TfToken("pointTag"); point.arity = 4;
    point.floatValues.resize(20);
    for (size_t i = 0; i != point.floatValues.size(); ++i)
        point.floatValues[i] = float(i) + .25f;
    curves.authoredPlanes = {point};
    auto source = desc.nodes.front();
    auto deform = desc.nodes.back();
    auto left = deform; left.path = SdfPath("/RbfAtomic/LeftDeform");
    auto right = deform; right.path = SdfPath("/RbfAtomic/RightDeform");
    if (unequalRight) {
        // A different sample budget solves different weights, so the right
        // branch animates different points from the same rest snapshot. The
        // join must observe unequal non-width input and fail closed.
        right.params.front().value = VtValue(4);
    }
    auto width = [](char const* path, SdfPath input, float factor) {
        UsdGenNodeDesc node;
        node.path = SdfPath(path); node.type = TfToken("UsdGenWidth");
        node.inputs = {input};
        node.params = {{TfToken("width"), VtValue(factor), false},
                       {TfToken("replace"), VtValue(false), false}};
        return node;
    };
    auto wl = width("/RbfAtomic/LeftWidth", left.path, 2.f);
    auto wr = width("/RbfAtomic/RightWidth", right.path, 3.f);
    UsdGenNodeDesc blend;
    blend.path = SdfPath("/RbfAtomic/Blend"); blend.type = TfToken("UsdGenWidthBlend");
    blend.inputs = {wl.path, wr.path}; blend.blend = .25f;
    desc.nodes = {source, left, right, wl, wr, blend};
    desc.terminal = blend.path;
    return desc;
}

static UsdGenGraphDesc RbfRefDagDesc() {
    auto desc = RbfDagDesc();
    auto& curves = desc.curveSets.front();
    curves.role = UsdGenRole::Reference;
    curves.curveRole = TfToken("guide");
    curves.authoredPlanes.clear();
    UsdGenNodeDesc source;
    source.path = SdfPath("/RbfAtomic/ReferenceSource");
    source.type = TfToken("UsdGenReferenceSource");
    source.references = {curves.path};
    desc.nodes[0] = source;
    for (size_t i = 1; i != desc.nodes.size(); ++i)
        if (!desc.nodes[i].inputs.empty() && desc.nodes[i].inputs.front() == SdfPath("/RbfAtomic/Source"))
            desc.nodes[i].inputs.front() = source.path;
    return desc;
}

static UsdGenGraphDesc RbfScatterDagDesc(bool unequalRight = false) {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/ScatterRbf");
    desc.executionBackend = UsdGenExecutionBackend::Cuda;
    desc.defaultWidth = .025f;
    UsdGenSurfaceDesc scalp;
    scalp.path = SdfPath("/ScatterRbf/Scalp");
    scalp.restPoints = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    scalp.points = scalp.restPoints;
    for (auto& point : scalp.points) point[0] += .25f;
    // A non-affine posed delta: approximations with different sample budgets
    // must differ, so the unequal sibling variant observes a real mismatch.
    // Vertex UVs stay tri-barycentric (x+y<=1): capture interpolates this
    // attribute and the binder validates the result per face.
    scalp.points[2][1] += .5f;
    scalp.faceVertexCounts = {3,3,3};
    scalp.faceVertexIndices = {0,1,2, 0,1,3, 1,2,4};
    scalp.uv = {{0,0},{1,0},{0,1},{0,0},{1,0}};
    desc.surfaces = {scalp};
    UsdGenNodeDesc scatter;
    scatter.path = SdfPath("/ScatterRbf/Scatter");
    scatter.type = TfToken("UsdGenScatter");
    scatter.seed = 41;
    scatter.surfaces = {scalp.path};
    scatter.params = {{TfToken("density"), VtValue(80.f), false}};
    UsdGenNodeDesc grow;
    grow.path = SdfPath("/ScatterRbf/Grow");
    grow.type = TfToken("UsdGenGrow");
    grow.inputs = {scatter.path};
    grow.seed = 19;
    grow.params = {{TfToken("segments"), VtValue(5), false},
                   {TfToken("length"), VtValue(2.f), false},
                   {TfToken("lengthRandom"), VtValue(GfVec2f(.5f,1.5f)), false}};
    auto deform = [&](char const* path, SdfPath input, int samples) {
        UsdGenNodeDesc node;
        node.path = SdfPath(path);
        node.type = TfToken("UsdGenDeform");
        node.inputs = {input};
        node.surfaces = {scalp.path};
        node.mode = TfToken("rbf");
        node.readPhase = TfToken("final");
        node.params = {{TfToken("rbfSamples"), VtValue(samples), false}};
        return node;
    };
    auto width = [](char const* path, SdfPath input, float factor) {
        UsdGenNodeDesc node;
        node.path = SdfPath(path);
        node.type = TfToken("UsdGenWidth");
        node.inputs = {input};
        node.params = {{TfToken("width"), VtValue(factor), false},
                       {TfToken("replace"), VtValue(false), false}};
        return node;
    };
    auto left = deform("/ScatterRbf/LeftDeform", grow.path, 5);
    auto right = deform("/ScatterRbf/RightDeform", grow.path, unequalRight ? 4 : 5);
    auto wl = width("/ScatterRbf/LeftWidth", left.path, 2.f);
    auto wr = width("/ScatterRbf/RightWidth", right.path, 3.f);
    UsdGenNodeDesc blend;
    blend.path = SdfPath("/ScatterRbf/Blend");
    blend.type = TfToken("UsdGenWidthBlend");
    blend.inputs = {wl.path, wr.path};
    blend.blend = .25f;
    desc.nodes = {scatter, grow, left, right, wl, wr, blend};
    desc.terminal = blend.path;
    return desc;
}

static int LiteralRbfScatterDagAdmission() {
    int device = -1;
    CHECK(cudaGetDevice(&device) == cudaSuccess);
    auto initializePool = gpu::TryReserveCudaExecutionBytes(0, UsdGenExecutionResourceKind::Active);
    CHECK(initializePool);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, device});
    CHECK(resources);
    auto drainRetirement = [device] {
        if (auto service = FindUsdGenExecutionRetirementService(
                {UsdGenExecutionResourceBackend::Cuda, device})) service->Drain();
    };
    auto const outerBaseline = resources->Snapshot();
    UsdGenDiagnostics diagnostics;
    auto desc = RbfScatterDagDesc();
    auto plan = CompileCudaGraph(desc, &diagnostics);
    if (!plan || diagnostics.HasErrors())
        for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "scatter dag: %s\n", error.c_str());
    CHECK(plan && !diagnostics.HasErrors());
    auto metadata = GetCudaExecutionPlanMetadata(*plan);
    CHECK(metadata && metadata->Shape() == UsdGenExecutionPlanShape::SourceRootedValueDag &&
          metadata->MemoryEstimate().runtimeRefinementAvailable &&
          !metadata->MemoryEstimate().memoryAvailable &&
          !metadata->MemoryEstimate().conservativeUpperBound);
    // Workspaces, jobs and generations below are scoped: Grow output, blend
    // graph cache and published owners hold pool permits until their owners
    // die, so the outer recovery check runs only after every scope closes.
    {
    auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    auto referenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(workspace && referenceWorkspace);
    auto expected = ExecuteCudaGraph(*plan, *referenceWorkspace, 1, 3, &diagnostics);
    if (!expected) for (auto const& error : diagnostics.errors)
        std::fprintf(stderr, "scatter dag reference: %s\n", error.c_str());
    CHECK(expected && !diagnostics.HasErrors());
    CHECK(expected->Geometry().curveCount != 0 && expected->Geometry().pointCount != 0);
    uint64_t peak = 0;
    {
        drainRetirement();
        auto const baseline = resources->Snapshot();
        auto filler = resources->TryReserve(baseline.usableBytes - baseline.usedBytes,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const saturated = resources->Snapshot();
        auto const beforeAttempts = GetAttempts();
        UsdGenDiagnostics rejectDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 1, 1, &rejectDiagnostics));
        auto const afterAttempts = GetAttempts();
        CHECK(ParseLiteralRbfPeak(rejectDiagnostics, &peak) &&
              afterAttempts.rbfAccept == beforeAttempts.rbfAccept &&
              afterAttempts.rbfRollback == beforeAttempts.rbfRollback &&
              afterAttempts.surfaceAccept == beforeAttempts.surfaceAccept &&
              afterAttempts.surfaceRollback == beforeAttempts.surfaceRollback &&
              GetCudaBindingStats(*workspace).empty() &&
              resources->Snapshot().byKind == saturated.byKind);
        filler->Release();
        CHECK(resources->Snapshot().byKind == baseline.byKind);
    }
    std::shared_ptr<const UsdGenDeviceGeneration> first;
    for (int warm = 0; warm != 2; ++warm) {
        drainRetirement();
        auto const before = resources->Snapshot();
        CHECK(before.usableBytes - before.usedBytes >= peak);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak + 1,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const full = resources->Snapshot();
        auto const stats = GetCudaBindingStats(*workspace);
        auto const attempts = GetAttempts();
        UsdGenDiagnostics belowDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 2 + warm, 4 + warm,
                                &belowDiagnostics, first));
        uint64_t belowPeak = 0;
        auto const rejected = GetAttempts();
        CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == peak &&
              resources->Snapshot().byKind == full.byKind &&
              SameStats(stats, GetCudaBindingStats(*workspace)) &&
              attempts.rbfAccept == rejected.rbfAccept &&
              attempts.rbfRollback == rejected.rbfRollback &&
              attempts.surfaceAccept == rejected.surfaceAccept &&
              attempts.surfaceRollback == rejected.surfaceRollback);
        filler->Release();
        filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                       UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto result = ExecuteCudaGraph(*plan, *workspace, 2 + warm, 6 + warm,
                                       &diagnostics, first);
        if (!result) for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "scatter dag exact-budget direct: %s\n", error.c_str());
        CHECK(result && !diagnostics.HasErrors() && SameGeneration(expected, result));
        CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(before, UsdGenExecutionResourceKind::Pending));
        if (first) CHECK(first->Owner() != result->Owner() && SameGeneration(expected, first));
        else first = result;
        filler->Release();
    }
    {
        drainRetirement();
        auto const before = resources->Snapshot();
        auto const stats = GetCudaBindingStats(*workspace);
        auto const attempts = GetAttempts();
        CHECK(stats.size() == 2 && before.usableBytes - before.usedBytes >= peak);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const filled = resources->Snapshot();
        failNextCudaSynchronousFinalizationForTesting();
        UsdGenDiagnostics rollbackDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 3.5, 8, &rollbackDiagnostics, first));
        drainRetirement();
        auto const rejected = GetAttempts();
        auto const rolledBack = resources->Snapshot();
        for (size_t kind = 0; kind != filled.byKind.size(); ++kind)
            CHECK(rolledBack.byKind[kind] <= filled.byKind[kind]);
        CHECK(rollbackDiagnostics.HasErrors() &&
              SameStats(stats, GetCudaBindingStats(*workspace)) &&
              SameGeneration(expected, first) &&
              rejected.rbfAccept == attempts.rbfAccept &&
              rejected.surfaceAccept == attempts.surfaceAccept &&
              rejected.rbfRollback == attempts.rbfRollback + 2 &&
              rejected.surfaceRollback == attempts.surfaceRollback + 2);
        filler->Release();
        auto const retryBaseline = resources->Snapshot();
        CHECK(retryBaseline.usableBytes - retryBaseline.usedBytes >= peak);
        filler = resources->TryReserve(retryBaseline.usableBytes - retryBaseline.usedBytes - peak,
                                       UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto retry = ExecuteCudaGraph(*plan, *workspace, 3.5, 9, &diagnostics, first);
        CHECK(retry && SameGeneration(expected, retry) && SameGeneration(expected, first));
        filler->Release();
    }
    {
        auto asyncWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(asyncWorkspace);
        auto job = CreateCudaExecutionJob(plan, *asyncWorkspace, 4, 12,
                                          &diagnostics, first);
        CHECK(job);
        auto sourcePromise = std::make_shared<std::promise<bool>>();
        auto sourceFuture = sourcePromise->get_future();
        CHECK(ExecuteCudaJobSourceAsync(job, [sourcePromise](bool ok) {
            sourcePromise->set_value(ok);
        }) && sourceFuture.get());
        for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i) {
            auto opPromise = std::make_shared<std::promise<bool>>();
            auto opFuture = opPromise->get_future();
            bool const launched = ExecuteCudaJobOperatorAsync(job, i, [opPromise](bool ok) {
                opPromise->set_value(ok);
            });
            bool const ok = launched && opFuture.get();
            if (!ok) std::fprintf(stderr, "scatter dag async operator %zu/%zu launched=%d\n",
                i, CudaExecutionJobOperatorCount(*job), launched);
            CHECK(ok);
        }
        auto finalPromise = std::make_shared<std::promise<std::shared_ptr<const UsdGenDeviceGeneration>>>();
        auto finalFuture = finalPromise->get_future();
        CHECK(FinalizeCudaExecutionJobAsync(job, [finalPromise](auto result) {
            finalPromise->set_value(result);
        }));
        auto accepted = finalFuture.get();
        waitForCudaNativeRelayRetirementForTesting();
        CHECK(accepted && accepted != first && SameGeneration(expected, accepted));
    }
    } // end scoped scatter battery: workspaces, jobs and generations released
    {
        drainRetirement();
        auto unequalDesc = RbfScatterDagDesc(true);
        auto unequalPlan = CompileCudaGraph(unequalDesc, &diagnostics);
        CHECK(unequalPlan && !diagnostics.HasErrors());
        auto unequalWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(unequalWorkspace);
        auto const before = resources->Snapshot();
        auto const attempts = GetAttempts();
        auto const stats = GetCudaBindingStats(*unequalWorkspace);
        UsdGenDiagnostics unequalDiagnostics;
        auto unequal = ExecuteCudaGraph(*unequalPlan, *unequalWorkspace, 5, 20,
                                        &unequalDiagnostics, nullptr);
        auto const after = GetAttempts();
        CHECK(!unequal && unequalDiagnostics.HasErrors() &&
              DiagnosticContains(unequalDiagnostics, "differ outside Width") &&
              SameStats(stats, GetCudaBindingStats(*unequalWorkspace)) &&
              after.rbfAccept == attempts.rbfAccept &&
              after.surfaceAccept == attempts.surfaceAccept);
        drainRetirement();
        auto const settled = resources->Snapshot();
        for (size_t kind = 0; kind != before.byKind.size(); ++kind)
            CHECK(settled.byKind[kind] <= before.byKind[kind]);
    }
    drainRetirement();
    {
        auto now = resources->Snapshot();
        if (now.byKind != outerBaseline.byKind)
            for (size_t kind = 0; kind != now.byKind.size(); ++kind)
                std::fprintf(stderr, "scatter outer kind %zu: %zu -> %zu\n",
                    kind, outerBaseline.byKind[kind], now.byKind[kind]);
    }
    CHECK(resources->Snapshot().byKind == outerBaseline.byKind);
    return 0;
}

// Aggregate admission extended beyond linear chains: the fixed-cardinality
// source-rooted Deform/Width/WidthBlend value DAG above must reserve one
// exact peak (source/named/publication once, each Deform cache/candidate and
// evaluator separately, every Width/WidthBlend output plus proof status) and
// prove exact-budget admit, below-budget reject, atomic multi-branch
// rollback, equal/unequal sibling joins and cold/warm direct/async parity.
static int LiteralRbfDagAdmission() {
    int device = -1;
    CHECK(cudaGetDevice(&device) == cudaSuccess);
    auto initializePool = gpu::TryReserveCudaExecutionBytes(0, UsdGenExecutionResourceKind::Active);
    CHECK(initializePool);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, device});
    CHECK(resources);
    auto drainRetirement = [device] {
        if (auto service = FindUsdGenExecutionRetirementService(
                {UsdGenExecutionResourceBackend::Cuda, device})) service->Drain();
    };
    auto const outerBaseline = resources->Snapshot();
    // A second rest-to-animated Deform along one lineage is not a branch and
    // must stay rejected; the DAG extension must not relax that check.
    {
        auto chained = RbfDagDesc();
        chained.nodes[2].inputs = {chained.nodes[1].path};
        UsdGenDiagnostics chainDiagnostics;
        CHECK(!CompileCudaGraph(chained, &chainDiagnostics) &&
              DiagnosticContains(chainDiagnostics, "apply surface motion twice"));
    }
    // Expression-driven and mapped Widths refine through the aggregate
    // recipe with full proofs (see LiteralRbfParamMapDagAdmission). A
    // non-rbfSamples Deform expression remains a supported literal shape.
    {
        auto deformExpressed = RbfDagDesc();
        UsdGenExpressionDesc blendExpression;
        blendExpression.path = SdfPath("/RbfAtomic/Expressions/dagBlend");
        blendExpression.source = "$value";
        blendExpression.outputs.push_back({TfToken("result"), TfToken("float"),
                                           {expr::ScalarType::Float32, 1, 1, 1, 1, false}});
        deformExpressed.expressions.push_back(blendExpression);
        UsdGenExpressionBinding blendBinding;
        blendBinding.expression = blendExpression.path;
        blendBinding.destination = TfToken("blend");
        blendBinding.domain = expr::Domain::Groom;
        blendBinding.nativeType = TfToken("float");
        blendBinding.destinationShape = {expr::ScalarType::Float32, 1, 1, 1, 1, false};
        blendBinding.literal = VtValue(1.f);
        deformExpressed.nodes[1].expressionBindings.push_back(blendBinding);
        UsdGenDiagnostics deformExpressedDiagnostics;
        auto deformExpressedPlan = CompileCudaGraph(deformExpressed, &deformExpressedDiagnostics);
        CHECK(deformExpressedPlan && !deformExpressedDiagnostics.HasErrors());
        auto deformExpressedMetadata = GetCudaExecutionPlanMetadata(*deformExpressedPlan);
        CHECK(deformExpressedMetadata &&
              deformExpressedMetadata->MemoryEstimate().runtimeRefinementAvailable);
    }
    // Workspaces, jobs and generations below are scoped: the blend graph
    // cache and published owners hold pool permits until their owners die,
    // so the outer recovery check runs only after every scope has closed.
    {
    UsdGenDiagnostics diagnostics;
    auto desc = RbfDagDesc();
    auto plan = CompileCudaGraph(desc, &diagnostics);
    CHECK(plan && !diagnostics.HasErrors());
    auto metadata = GetCudaExecutionPlanMetadata(*plan);
    CHECK(metadata && metadata->Shape() == UsdGenExecutionPlanShape::SourceRootedValueDag &&
          metadata->MemoryEstimate().runtimeRefinementAvailable &&
          !metadata->MemoryEstimate().memoryAvailable &&
          !metadata->MemoryEstimate().conservativeUpperBound);
    auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(workspace);
    // Independent reference proves retained COW channels, not self-equality.
    auto referenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
    CHECK(referenceWorkspace);
    auto referenceDesc = RbfDesc();
    referenceDesc.surfaces.front().points[2][1] += .5f;
    {
        auto& curves = referenceDesc.curveSets.front();
        curves.curveVertexCounts = {2, 3}; curves.curveId = {42, 7};
        curves.points = {{.2f,.2f,0},{.2f,.4f,0},
                         {.3f,.2f,0},{.3f,.7f,0},{.3f,1.2f,0}};
        curves.rest = curves.points; curves.skinPrim = {0,0};
        curves.skinPrimUv = {{.2f,.2f},{.3f,.2f}};
        UsdGenAuthoredPlaneDesc point;
        point.name = TfToken("pointTag"); point.arity = 4;
        point.floatValues.resize(20);
        for (size_t i = 0; i != point.floatValues.size(); ++i)
            point.floatValues[i] = float(i) + .25f;
        curves.authoredPlanes = {point};
    }
    auto referenceSource = referenceDesc.nodes.front();
    auto referenceLeft = referenceDesc.nodes.back();
    referenceLeft.path = SdfPath("/RbfAtomic/LeftDeform");
    UsdGenNodeDesc expectedWidth;
    expectedWidth.path = SdfPath("/RbfAtomic/ExpectedWidth");
    expectedWidth.type = TfToken("UsdGenWidth");
    expectedWidth.inputs = {referenceLeft.path};
    expectedWidth.params = {{TfToken("width"), VtValue(2.25f), false},
                            {TfToken("replace"), VtValue(false), false}};
    referenceDesc.nodes = {referenceSource, referenceLeft, expectedWidth};
    referenceDesc.terminal = expectedWidth.path;
    auto referencePlan = CompileCudaGraph(referenceDesc, &diagnostics);
    CHECK(referencePlan && !diagnostics.HasErrors());
    auto expected = ExecuteCudaGraph(*referencePlan, *referenceWorkspace, 1, 3, &diagnostics);
    CHECK(expected && !diagnostics.HasErrors());
    uint64_t peak = 0;
    {
        auto const baseline = resources->Snapshot();
        auto filler = resources->TryReserve(baseline.usableBytes - baseline.usedBytes,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const saturated = resources->Snapshot();
        auto const beforeAttempts = GetAttempts();
        UsdGenDiagnostics rejectDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 1, 1, &rejectDiagnostics));
        auto const afterAttempts = GetAttempts();
        CHECK(ParseLiteralRbfPeak(rejectDiagnostics, &peak) &&
              afterAttempts.rbfAccept == beforeAttempts.rbfAccept &&
              afterAttempts.rbfRollback == beforeAttempts.rbfRollback &&
              afterAttempts.surfaceAccept == beforeAttempts.surfaceAccept &&
              afterAttempts.surfaceRollback == beforeAttempts.surfaceRollback &&
              GetCudaBindingStats(*workspace).empty() &&
              resources->Snapshot().byKind == saturated.byKind);
        filler->Release();
        CHECK(resources->Snapshot().byKind == baseline.byKind);
    }
    std::shared_ptr<const UsdGenDeviceGeneration> first;
    for (int warm = 0; warm != 2; ++warm) {
        drainRetirement();
        auto const before = resources->Snapshot();
        CHECK(before.usableBytes - before.usedBytes >= peak);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak + 1,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const full = resources->Snapshot();
        auto const stats = GetCudaBindingStats(*workspace);
        auto const attempts = GetAttempts();
        UsdGenDiagnostics belowDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 2 + warm, 4 + warm,
                                &belowDiagnostics, first));
        uint64_t belowPeak = 0;
        auto const rejected = GetAttempts();
        CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == peak &&
              resources->Snapshot().byKind == full.byKind &&
              SameStats(stats, GetCudaBindingStats(*workspace)) &&
              attempts.rbfAccept == rejected.rbfAccept &&
              attempts.rbfRollback == rejected.rbfRollback &&
              attempts.surfaceAccept == rejected.surfaceAccept &&
              attempts.surfaceRollback == rejected.surfaceRollback);
        filler->Release();
        filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                       UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto result = ExecuteCudaGraph(*plan, *workspace, 2 + warm, 6 + warm,
                                       &diagnostics, first);
        if (!result) for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "dag exact-budget direct: %s\n", error.c_str());
        CHECK(result && !diagnostics.HasErrors() && SameGeneration(expected, result));
        CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
              ResourceKindBytes(before, UsdGenExecutionResourceKind::Pending));
        if (first) CHECK(first->Owner() != result->Owner() && SameGeneration(expected, first));
        else first = result;
        filler->Release();
    }
    // Both sibling caches must roll back atomically when finalization fails.
    {
        drainRetirement();
        auto const before = resources->Snapshot();
        auto const stats = GetCudaBindingStats(*workspace);
        auto const attempts = GetAttempts();
        CHECK(stats.size() == 2 && before.usableBytes - before.usedBytes >= peak);
        auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                            UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto const filled = resources->Snapshot();
        failNextCudaSynchronousFinalizationForTesting();
        UsdGenDiagnostics rollbackDiagnostics;
        CHECK(!ExecuteCudaGraph(*plan, *workspace, 3.5, 8, &rollbackDiagnostics, first));
        drainRetirement();
        auto const rejected = GetAttempts();
        auto const rolledBack = resources->Snapshot();
        for (size_t kind = 0; kind != filled.byKind.size(); ++kind)
            CHECK(rolledBack.byKind[kind] <= filled.byKind[kind]);
        CHECK(rollbackDiagnostics.HasErrors() &&
              SameStats(stats, GetCudaBindingStats(*workspace)) &&
              SameGeneration(expected, first) &&
              rejected.rbfAccept == attempts.rbfAccept &&
              rejected.surfaceAccept == attempts.surfaceAccept &&
              rejected.rbfRollback == attempts.rbfRollback + 2 &&
              rejected.surfaceRollback == attempts.surfaceRollback + 2);
        filler->Release();
        auto const retryBaseline = resources->Snapshot();
        CHECK(retryBaseline.usableBytes - retryBaseline.usedBytes >= peak);
        filler = resources->TryReserve(retryBaseline.usableBytes - retryBaseline.usedBytes - peak,
                                       UsdGenExecutionResourceKind::Active);
        CHECK(filler);
        auto retry = ExecuteCudaGraph(*plan, *workspace, 3.5, 9, &diagnostics, first);
        CHECK(retry && SameGeneration(expected, retry) && SameGeneration(expected, first));
        filler->Release();
    }
    // Native async path: same peak, same parity, same rollback contract.
    {
        auto asyncWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(asyncWorkspace);
        std::vector<std::shared_ptr<UsdGenCudaExecutionJob>> retainedJobs;
        std::vector<std::shared_ptr<const UsdGenDeviceGeneration>> retainedGenerations;
        auto runStage = [](auto launch) {
            auto promise = std::make_shared<std::promise<bool>>();
            auto future = promise->get_future();
            if (!launch([promise](bool success) { promise->set_value(success); })) return false;
            bool const success = future.get();
            waitForCudaNativeRelayRetirementForTesting();
            return success;
        };
        auto finalize = [](auto job) {
            auto promise = std::make_shared<std::promise<std::shared_ptr<const UsdGenDeviceGeneration>>>();
            auto future = promise->get_future();
            if (!FinalizeCudaExecutionJobAsync(job, [promise](auto result) { promise->set_value(result); }))
                return std::shared_ptr<const UsdGenDeviceGeneration>{};
            auto result = future.get();
            waitForCudaNativeRelayRetirementForTesting();
            return result;
        };
        for (int warm = 0; warm != 2; ++warm) {
            drainRetirement();
            auto const before = resources->Snapshot();
            CHECK(before.usableBytes - before.usedBytes >= peak);
            auto lastGood = retainedGenerations.empty() ? first : retainedGenerations.back();
            auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak + 1,
                                                UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto const saturatedAsync = resources->Snapshot();
            auto const attempts = GetAttempts();
            UsdGenDiagnostics belowDiagnostics;
            CHECK(!CreateCudaExecutionJob(plan, *asyncWorkspace, 4 + warm, 10 + warm,
                                          &belowDiagnostics, lastGood));
            uint64_t belowPeak = 0;
            auto const rejectedAttempts = GetAttempts();
            CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == peak &&
                  resources->Snapshot().byKind == saturatedAsync.byKind &&
                  attempts.rbfAccept == rejectedAttempts.rbfAccept &&
                  attempts.rbfRollback == rejectedAttempts.rbfRollback &&
                  attempts.surfaceAccept == rejectedAttempts.surfaceAccept &&
                  attempts.surfaceRollback == rejectedAttempts.surfaceRollback);
            filler->Release();
            filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto job = CreateCudaExecutionJob(plan, *asyncWorkspace, 4 + warm, 12 + warm,
                                              &diagnostics, lastGood);
            CHECK(job);
            retainedJobs.push_back(job);
            CHECK(runStage([&](auto done) { return ExecuteCudaJobSourceAsync(job, done); }));
            for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i)
                CHECK(runStage([&](auto done) { return ExecuteCudaJobOperatorAsync(job, i, done); }));
            auto accepted = finalize(job);
            CHECK(accepted && accepted != lastGood &&
                  SameGeneration(expected, accepted) && SameGeneration(expected, first) &&
                  SameGeneration(expected, lastGood));
            retainedGenerations.push_back(accepted);
            CHECK(ResourceKindBytes(resources->Snapshot(), UsdGenExecutionResourceKind::Pending) ==
                  ResourceKindBytes(before, UsdGenExecutionResourceKind::Pending));
            filler->Release();
        }
        {
            auto lastGood = retainedGenerations.back();
            drainRetirement();
            auto const before = resources->Snapshot();
            auto const attempts = GetAttempts();
            CHECK(lastGood && before.usableBytes - before.usedBytes >= peak);
            auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - peak,
                                                UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto const stats = GetCudaBindingStats(*asyncWorkspace);
            auto job = CreateCudaExecutionJob(plan, *asyncWorkspace, 6, 14, &diagnostics, lastGood);
            CHECK(job);
            retainedJobs.push_back(job);
            CHECK(runStage([&](auto done) { return ExecuteCudaJobSourceAsync(job, done); }));
            for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i)
                CHECK(runStage([&](auto done) { return ExecuteCudaJobOperatorAsync(job, i, done); }));
            failNextCudaFinalizationRelayAllocationForTesting();
            CHECK(!finalize(job));
            auto const rejected = GetAttempts();
            CHECK(SameStats(stats, GetCudaBindingStats(*asyncWorkspace)) &&
                  SameGeneration(expected, lastGood) && SameGeneration(expected, first) &&
                  rejected.rbfAccept == attempts.rbfAccept &&
                  rejected.surfaceAccept == attempts.surfaceAccept &&
                  rejected.rbfRollback == attempts.rbfRollback + 2 &&
                  rejected.surfaceRollback == attempts.surfaceRollback + 2);
            filler->Release();
            auto retry = CreateCudaExecutionJob(plan, *asyncWorkspace, 6, 15, &diagnostics, lastGood);
            CHECK(retry);
            retainedJobs.push_back(retry);
            CHECK(runStage([&](auto done) { return ExecuteCudaJobSourceAsync(retry, done); }));
            for (size_t i = 0; i != CudaExecutionJobOperatorCount(*retry); ++i)
                CHECK(runStage([&](auto done) { return ExecuteCudaJobOperatorAsync(retry, i, done); }));
            auto accepted = finalize(retry);
            CHECK(accepted && SameGeneration(expected, accepted) && SameGeneration(expected, lastGood));
            retainedGenerations.push_back(accepted);
        }
    }
    } // end scoped V0 battery: workspaces, jobs and generations released here
    // Unequal siblings must fail closed at the join, accept nothing, leak nothing.
    {
        UsdGenDiagnostics diagnostics;
        drainRetirement();
        auto unequalDesc = RbfDagDesc(true);
        auto unequalPlan = CompileCudaGraph(unequalDesc, &diagnostics);
        if (!unequalPlan || diagnostics.HasErrors())
            for (auto const& error : diagnostics.errors)
                std::fprintf(stderr, "unequal dag: %s\n", error.c_str());
        CHECK(unequalPlan && !diagnostics.HasErrors());
        auto unequalWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(unequalWorkspace);
        auto const before = resources->Snapshot();
        auto const attempts = GetAttempts();
        auto const stats = GetCudaBindingStats(*unequalWorkspace);
        UsdGenDiagnostics unequalDiagnostics;
        auto unequal = ExecuteCudaGraph(*unequalPlan, *unequalWorkspace, 5, 20,
                                        &unequalDiagnostics, nullptr);
        auto const after = GetAttempts();
        if (unequal) std::fprintf(stderr, "unequal dag: unexpectedly admitted\n");
        for (auto const& error : unequalDiagnostics.errors)
            std::fprintf(stderr, "unequal dag: %s\n", error.c_str());
        std::fprintf(stderr, "unequal dag: stats %zu->%zu accept %llu->%llu\n",
            stats.size(), GetCudaBindingStats(*unequalWorkspace).size(),
            static_cast<unsigned long long>(attempts.rbfAccept),
            static_cast<unsigned long long>(after.rbfAccept));
        CHECK(!unequal && unequalDiagnostics.HasErrors() &&
              DiagnosticContains(unequalDiagnostics, "differ outside Width") &&
              SameStats(stats, GetCudaBindingStats(*unequalWorkspace)) &&
              after.rbfAccept == attempts.rbfAccept &&
              after.surfaceAccept == attempts.surfaceAccept);
        drainRetirement();
        auto const settled = resources->Snapshot();
        for (size_t kind = 0; kind != before.byKind.size(); ++kind)
            CHECK(settled.byKind[kind] <= before.byKind[kind]);
    }
    // Reference-rooted Deform DAGs lower as a third reference shape with the
    // same literal rules as authored-source RBF DAGs.
    {
        UsdGenDiagnostics diagnostics;
        auto refDesc = RbfRefDagDesc();
        auto refPlan = CompileCudaGraph(refDesc, &diagnostics);
        CHECK(refPlan && !diagnostics.HasErrors());
        auto refMetadata = GetCudaExecutionPlanMetadata(*refPlan);
        CHECK(refMetadata &&
              refMetadata->Shape() == UsdGenExecutionPlanShape::SourceRootedValueDag &&
              refMetadata->MemoryEstimate().runtimeRefinementAvailable &&
              !refMetadata->MemoryEstimate().memoryAvailable &&
              !refMetadata->MemoryEstimate().conservativeUpperBound);
        auto refWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        auto refReferenceWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(refWorkspace && refReferenceWorkspace);
        auto refExpected = ExecuteCudaGraph(*refPlan, *refReferenceWorkspace, 11, 30,
                                            &diagnostics);
        if (!refExpected) for (auto const& error : diagnostics.errors)
            std::fprintf(stderr, "ref dag reference: %s\n", error.c_str());
        CHECK(refExpected && !diagnostics.HasErrors());
        uint64_t refPeak = 0;
        {
            drainRetirement();
            auto const baseline = resources->Snapshot();
            auto filler = resources->TryReserve(baseline.usableBytes - baseline.usedBytes,
                                                UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto const saturated = resources->Snapshot();
            auto const beforeAttempts = GetAttempts();
            UsdGenDiagnostics rejectDiagnostics;
            CHECK(!ExecuteCudaGraph(*refPlan, *refWorkspace, 11, 31, &rejectDiagnostics));
            auto const afterAttempts = GetAttempts();
            CHECK(ParseLiteralRbfPeak(rejectDiagnostics, &refPeak) &&
                  afterAttempts.rbfAccept == beforeAttempts.rbfAccept &&
                  afterAttempts.rbfRollback == beforeAttempts.rbfRollback &&
                  afterAttempts.surfaceAccept == beforeAttempts.surfaceAccept &&
                  afterAttempts.surfaceRollback == beforeAttempts.surfaceRollback &&
                  GetCudaBindingStats(*refWorkspace).empty() &&
                  resources->Snapshot().byKind == saturated.byKind);
            filler->Release();
            CHECK(resources->Snapshot().byKind == baseline.byKind);
        }
        std::shared_ptr<const UsdGenDeviceGeneration> refFirst;
        for (int warm = 0; warm != 2; ++warm) {
            drainRetirement();
            auto const before = resources->Snapshot();
            CHECK(before.usableBytes - before.usedBytes >= refPeak);
            auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - refPeak + 1,
                                                UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto const full = resources->Snapshot();
            UsdGenDiagnostics belowDiagnostics;
            CHECK(!ExecuteCudaGraph(*refPlan, *refWorkspace, 12 + warm, 32 + warm,
                                    &belowDiagnostics, refFirst));
            uint64_t belowPeak = 0;
            CHECK(ParseLiteralRbfPeak(belowDiagnostics, &belowPeak) && belowPeak == refPeak &&
                  resources->Snapshot().byKind == full.byKind);
            filler->Release();
            filler = resources->TryReserve(before.usableBytes - before.usedBytes - refPeak,
                                           UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            auto result = ExecuteCudaGraph(*refPlan, *refWorkspace, 12 + warm, 34 + warm,
                                           &diagnostics, refFirst);
            if (!result) for (auto const& error : diagnostics.errors)
                std::fprintf(stderr, "ref dag exact-budget direct: %s\n", error.c_str());
            CHECK(result && !diagnostics.HasErrors() && SameGeneration(refExpected, result));
            if (refFirst) CHECK(refFirst->Owner() != result->Owner());
            else refFirst = result;
            filler->Release();
        }
        {
            drainRetirement();
            auto const before = resources->Snapshot();
            auto const stats = GetCudaBindingStats(*refWorkspace);
            auto const attempts = GetAttempts();
            CHECK(stats.size() == 2 && before.usableBytes - before.usedBytes >= refPeak);
            auto filler = resources->TryReserve(before.usableBytes - before.usedBytes - refPeak,
                                                UsdGenExecutionResourceKind::Active);
            CHECK(filler);
            failNextCudaSynchronousFinalizationForTesting();
            UsdGenDiagnostics rollbackDiagnostics;
            CHECK(!ExecuteCudaGraph(*refPlan, *refWorkspace, 13.5, 40, &rollbackDiagnostics,
                                    refFirst));
            drainRetirement();
            auto const rejected = GetAttempts();
            CHECK(rollbackDiagnostics.HasErrors() &&
                  SameStats(stats, GetCudaBindingStats(*refWorkspace)) &&
                  SameGeneration(refExpected, refFirst) &&
                  rejected.rbfAccept == attempts.rbfAccept &&
                  rejected.surfaceAccept == attempts.surfaceAccept &&
                  rejected.rbfRollback == attempts.rbfRollback + 2 &&
                  rejected.surfaceRollback == attempts.surfaceRollback + 2);
            filler->Release();
            auto asyncWorkspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
            CHECK(asyncWorkspace);
            auto job = CreateCudaExecutionJob(refPlan, *asyncWorkspace, 14, 41,
                                              &diagnostics, refFirst);
            CHECK(job);
            auto sourcePromise = std::make_shared<std::promise<bool>>();
            auto sourceFuture = sourcePromise->get_future();
            CHECK(ExecuteCudaJobSourceAsync(job, [sourcePromise](bool ok) {
                sourcePromise->set_value(ok);
            }) && sourceFuture.get());
            for (size_t i = 0; i != CudaExecutionJobOperatorCount(*job); ++i) {
                auto opPromise = std::make_shared<std::promise<bool>>();
                auto opFuture = opPromise->get_future();
                bool const launched = ExecuteCudaJobOperatorAsync(job, i, [opPromise](bool ok) {
                    opPromise->set_value(ok);
                });
                bool const ok = launched && opFuture.get();
                if (!ok) std::fprintf(stderr, "ref dag async operator %zu/%zu launched=%d\n",
                    i, CudaExecutionJobOperatorCount(*job), launched);
                CHECK(ok);
            }
            auto finalPromise = std::make_shared<std::promise<std::shared_ptr<const UsdGenDeviceGeneration>>>();
            auto finalFuture = finalPromise->get_future();
            CHECK(FinalizeCudaExecutionJobAsync(job, [finalPromise](auto result) {
                finalPromise->set_value(result);
            }));
            auto accepted = finalFuture.get();
            waitForCudaNativeRelayRetirementForTesting();
            CHECK(accepted && accepted != refFirst && SameGeneration(refExpected, accepted));
        }
    } // end scoped reference block
    drainRetirement();
    {
        auto now = resources->Snapshot();
        if (now.byKind != outerBaseline.byKind)
            for (size_t kind = 0; kind != now.byKind.size(); ++kind)
                std::fprintf(stderr, "dag outer kind %zu: %zu -> %zu\n",
                    kind, outerBaseline.byKind[kind], now.byKind[kind]);
    }
    CHECK(resources->Snapshot().byKind == outerBaseline.byKind);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--value-dag") return ValueDagTransactions();
    if (argc == 2 && std::string(argv[1]) == "--aggregate-admission") {
        UsdGenDiagnostics diagnostics;
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        return LiteralRbfChainAdmission();
    }
    if (argc == 2 && std::string(argv[1]) == "--aggregate-dag-admission") {
        UsdGenDiagnostics diagnostics;
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        return LiteralRbfDagAdmission();
    }
    if (argc == 2 && std::string(argv[1]) == "--aggregate-scatter-dag-admission") {
        UsdGenDiagnostics diagnostics;
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        return LiteralRbfScatterDagAdmission();
    }
    if (argc == 2 && std::string(argv[1]) == "--aggregate-topology-dag-admission") {
        UsdGenDiagnostics diagnostics;
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        return LiteralRbfTopologyDagAdmission();
    }
    if (argc == 2 && std::string(argv[1]) == "--aggregate-param-map-admission") {
        UsdGenDiagnostics diagnostics;
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        return LiteralRbfParamMapDagAdmission();
    }
    if (argc == 2 && std::string(argv[1]) == "--blend-overlap") {
        UsdGenDiagnostics diagnostics;
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        return BlendOverlapWitness();
    }
    if (argc == 2 && std::string(argv[1]) == "--multi-groom-admission") {
        UsdGenDiagnostics diagnostics;
        auto workspace = CreateCudaExecutionWorkspace(-1, &diagnostics);
        CHECK(workspace);
        return LiteralRbfMultiGroomAdmission();
    }
    CHECK(argc == 1);
    UsdGenDiagnostics diagnostics;
    auto plan = CompileCudaGraph(RbfDesc(), &diagnostics);
    CHECK(plan && !diagnostics.HasErrors());
    auto metadata = GetCudaExecutionPlanMetadata(*plan);
    CHECK(metadata && !metadata->MemoryEstimate().memoryAvailable &&
          !metadata->MemoryEstimate().conservativeUpperBound &&
          metadata->MemoryEstimate().runtimeRefinementAvailable);
    CHECK(FailureCase(plan, Resolution::AcceptPreflight, Side::Rbf));
    CHECK(FailureCase(plan, Resolution::AcceptPreflight, Side::Surface));
    CHECK(FailureCase(plan, Resolution::RollbackPreflight, Side::Rbf));
    CHECK(FailureCase(plan, Resolution::RollbackPreflight, Side::Surface));
    CHECK(FailureCase(plan, Resolution::AcceptCommit, Side::Rbf));
    CHECK(FailureCase(plan, Resolution::AcceptCommit, Side::Surface));
    CHECK(FailureCase(plan, Resolution::RollbackCommit, Side::Rbf));
    CHECK(FailureCase(plan, Resolution::RollbackCommit, Side::Surface));

    UsdGenExecutionRuntime runtime(4);
    UsdGenCudaExecutionQueue queue(runtime, -1, &diagnostics);
    CHECK(queue.Valid() && queue.Submit(plan, 10.0));
    queue.Drain();
    auto const first = queue.Snapshot();
    CHECK(first && first->generation && first->bindings.size() == 1);
    auto const before = GetAttempts();
    CHECK(queue.Submit(plan, 11.0));
    queue.Drain();
    auto const second = queue.Snapshot();
    auto const after = GetAttempts();
    CHECK(second && second != first && second->generation != first->generation &&
          second->bindings.size() == 1 &&
          second->bindings[0].identity == first->bindings[0].identity &&
          second->bindings[0].bindCount == first->bindings[0].bindCount &&
          second->bindings[0].solveCount == first->bindings[0].solveCount + 1 &&
          after.rbfAccept == before.rbfAccept + 1 &&
          after.surfaceAccept == before.surfaceAccept + 1 &&
          after.rbfRollback == before.rbfRollback &&
          after.surfaceRollback == before.surfaceRollback);
    CHECK(SynchronousTransactions(plan) == 0);
    return 0;
}
