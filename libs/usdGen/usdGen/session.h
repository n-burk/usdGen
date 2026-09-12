#ifndef USDGEN_SESSION_H
#define USDGEN_SESSION_H

#include "usdGen/cudaExecution.h"
#include "usdGen/executionPipeline.h"
#include "usdGen/generationStore.h"
#include "usdGen/graph.h"
#include "usdGen/stats.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// Value-only dirty input delivered to the command owner. Structural dirt
/// replaces all finer-grained entries; all other entries are OR-merged.
struct UsdGenPendingDirty
{
    bool structural = false;
    bool surfaceTopology = false;
    std::map<UsdGenNodeId, uint32_t> nodeBits;
    std::map<UsdGenSurfaceId, uint32_t> surfaceBits;

    bool Any() const {
        return structural || surfaceTopology || !nodeBits.empty() ||
               !surfaceBits.empty();
    }
};

/// Detached public graph information. It deliberately contains no compiled
/// graph/operator alias: the cooker may mutate or replace its graph safely.
struct UsdGenSessionGraphSnapshot
{
    int nodeCount = 0;
    UsdGenGraphDesc desc;
    std::shared_ptr<const UsdGenCudaExecutionPlan> cudaPlan;
    std::unordered_map<SdfPath, UsdGenNodeId, SdfPath::Hash> nodeIds;

    int NodeCount() const noexcept { return nodeCount; }
    UsdGenNodeId NodeIdForPath(SdfPath const& path) const {
        auto found = nodeIds.find(path);
        return found == nodeIds.end() ? kUsdGenInvalidNode : found->second;
    }
    UsdGenGraphDesc const& Desc() const noexcept { return desc; }
    std::shared_ptr<const UsdGenCudaExecutionPlan> const& CudaPlan() const noexcept {
        return cudaPlan;
    }
};

/// Atomically published immutable result bundle. Readers never enter either
/// command or cook ownership domain.
/// A failed current attempt publishes new diagnostics/compiled graph metadata
/// alongside the retained last-good generation and an empty dirty report.
struct UsdGenSessionSnapshot
{
    UsdGenGenerationConstPtr generation;
    UsdGenDirtyReport report;
    UsdGenDiagnostics diagnostics;
    UsdGenStats stats;
    std::shared_ptr<const UsdGenGraphRoutingSnapshot> routing;
    UsdGenSessionGraphSnapshot graphInfo;
    std::unordered_map<UsdGenNodeId, UsdGenNodeStats> nodeStats;
    std::vector<UsdGenCudaBindingStats> cudaBindings;
};

/// One atomic command-owner commit request. Optional input changes are applied
/// immediately before capturing the publication baseline and submitting work,
/// so they cannot interleave with a separately posted descriptor/context
/// command.
struct UsdGenSessionCommitRequest
{
    double frame = 0.0;
    UsdGenCommitReason reason = UsdGenCommitReason::NoticeBatchEnd;
    std::shared_ptr<const UsdGenGraphDesc> desc;
    std::optional<UsdGenContext> context;
    // Captured by an upstream owner when it must relay the original caller's
    // CUDA device. Absent means capture at this Session API boundary.
    std::optional<int> callerDevice;
};

class UsdGenSession
{
public:
    using SnapshotPtr = std::shared_ptr<const UsdGenSessionSnapshot>;
    using Completion = std::function<void(
        SnapshotPtr, UsdGenExecutionPipeline::Outcome)>;
    using CommitRequest = UsdGenSessionCommitRequest;

    explicit UsdGenSession(int threadLimit = 0);
    // External ownership boundary: stop/join submitters first. Accepted work
    // receives terminal callbacks before Session state is destroyed.
    ~UsdGenSession();
    UsdGenSession(UsdGenSession const&) = delete;
    UsdGenSession& operator=(UsdGenSession const&) = delete;

    // External synchronous adapters. They cooperatively invoke the short
    // command owner and must not be called from a pipeline callback.
    void SetGraphDesc(UsdGenGraphDesc const&);
    void SetContext(UsdGenContext);
    void SetDevicePublicationEnabled(bool);

    // Primary callback-safe mutation API: enqueue a command and return.
    void PostGraphDesc(UsdGenGraphDesc);
    void PostContext(UsdGenContext);
    void PostDevicePublicationEnabled(bool);
    void PostDirty(UsdGenPendingDirty);

    bool BeginDeviceEdit(UsdGenGenerationConstPtr const&, uint64_t*,
                         std::string* = nullptr);
    bool StageDeviceRevision(UsdGenGenerationConstPtr const&,
                             std::shared_ptr<const UsdGenDeviceGeneration>,
                             uint64_t, std::string* = nullptr);
    bool EndDeviceEdit(uint64_t, UsdGenGenerationConstPtr const&, bool = true);
    void AccumulateDirty(UsdGenPendingDirty&&);
    bool NeedsCommit() const noexcept;
    /// Captures CUDA's thread-local current device, or -2 on CPU/failure.
    static int CaptureCallerDevice() noexcept;

    /// Asynchronously accept a command snapshot and schedule private cooking.
    /// The completion runs on the command owner and may enqueue follow-up
    /// commands, but must not make synchronous Session calls.
    bool CommitAsync(CommitRequest, Completion = {});
    bool CommitAsync(double, UsdGenCommitReason, Completion = {});
    /// Compatibility boundary that cooperatively waits for one CommitAsync.
    SnapshotPtr CommitSnapshot(CommitRequest);
    SnapshotPtr CommitSnapshot(double, UsdGenCommitReason);
    UsdGenGenerationConstPtr Commit(double frame, UsdGenCommitReason reason) {
        auto snapshot = CommitSnapshot(frame, reason);
        return snapshot ? snapshot->generation : Generation();
    }

    SnapshotPtr Snapshot() const noexcept;
    // Returned by value; this copy can allocate and is not noexcept.
    UsdGenSessionGraphSnapshot Graph() const {
        auto snapshot = Snapshot();
        return snapshot ? snapshot->graphInfo : UsdGenSessionGraphSnapshot{};
    }
    UsdGenGenerationConstPtr Generation() const noexcept {
        auto snapshot = Snapshot();
        return snapshot ? snapshot->generation : nullptr;
    }
    UsdGenDirtyReport LastReport() const {
        auto snapshot = Snapshot();
        return snapshot ? snapshot->report : UsdGenDirtyReport{};
    }
    UsdGenDiagnostics LastDiagnostics() const {
        auto snapshot = Snapshot();
        return snapshot ? snapshot->diagnostics : UsdGenDiagnostics{};
    }
    UsdGenStats Stats() const {
        auto snapshot = Snapshot();
        return snapshot ? snapshot->stats : UsdGenStats{};
    }
    std::vector<UsdGenCudaBindingStats> CudaBindingStats() const;
    std::optional<UsdGenNodeStats> NodeStats(UsdGenNodeId) const;

    void InvalidateAllValues();
    void BeginDensityDrag();
    void EndDensityDrag();

private:
    struct State;
    std::unique_ptr<State> _state;
    std::shared_ptr<const UsdGenSessionSnapshot> _snapshot;
};

} // namespace usdGen

#endif // USDGEN_SESSION_H
