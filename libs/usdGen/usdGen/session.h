// usdGen engine — session (03-execution-engine.md §5.4/§5.6/§9.1).
//
// The session is the engine's front door: imaging routes dirties in via
// UsdGenPendingDirty, the session owns the compiler/graph/scheduler/store,
// and Commit() executes the 8-step commit on one thread under one
// non-recursive mutex. GetPrim on the imaging side only atomic_loads the
// published generation (I7) — nothing cooks there.
#ifndef USDGEN_SESSION_H
#define USDGEN_SESSION_H

#include "usdGen/compiler.h"
#include "usdGen/generationStore.h"
#include "usdGen/graph.h"
#include "usdGen/scheduler.h"
#include "usdGen/stats.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

#include <optional>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// Pure-value dirty payload produced by the imaging-side UsdGenDirtyRouter
/// (03 §5.1: "the engine consumes UsdGenPendingDirty, a pure value type").
/// Node ids and surface ids refer to the CURRENTLY compiled graph; a
/// structural change resets the pending set to UsdGenDirtyStructural on
/// every node instead.
struct UsdGenPendingDirty
{
    bool structural = false;  // graph digest input changed -> full recompile
    bool surfaceTopology = false;  // any bound surface resynced
    std::map<UsdGenNodeId, uint32_t> nodeBits;      // OR-accumulated UsdGenDirtyBits
    std::map<UsdGenSurfaceId, uint32_t> surfaceBits;
    bool Any() const {
        return structural || surfaceTopology || !nodeBits.empty() || !surfaceBits.empty();
    }
};

class UsdGenSession
{
public:
    UsdGenSession(int threadLimit = 0);
    ~UsdGenSession();

    UsdGenSession(const UsdGenSession &) = delete;
    UsdGenSession &operator=(const UsdGenSession &) = delete;

    // ---- input (imaging side, notice/app thread) --------------------------------
    /// Replace the graph description; marks the session structural-dirty so the
    /// next commit recompiles. The session copies the desc (03 §2.2).
    void SetGraphDesc(UsdGenGraphDesc const &desc);
    void SetContext(UsdGenContext context);
    /// Explicit consumer capability. Stock Hydra publication has no CUDA
    /// interop bridge, so it leaves this disabled. Device tools may opt in.
    void SetDevicePublicationEnabled(bool enabled);
    /// Reserve the session's single active device stroke against a clean
    /// snapshot. Tokens prevent independent tool bridges replacing each other.
    bool BeginDeviceEdit(UsdGenGenerationConstPtr const& expected, uint64_t* token,
                         std::string* reason = nullptr);
    /// Stage an immutable device point revision against the exact currently
    /// published snapshot. Commit(LiveOverride) publishes it without recooking
    /// operators. Scene/time changes supersede staged edits. No stage write.
    bool StageDeviceRevision(UsdGenGenerationConstPtr const& expected,
                            std::shared_ptr<const UsdGenDeviceGeneration> revision,
                            uint64_t token,
                            std::string* reason = nullptr);
    /// Release the stroke reservation. Return false if its expected snapshot
    /// has been superseded or has pending graph work. Cancellation may retain
    /// its staged restoration; normal release discards an unpublished move.
    bool EndDeviceEdit(uint64_t token, UsdGenGenerationConstPtr const& expected,
                       bool discardStaged = true);
    /// OR-accumulate routed dirties; thread-safe (lock-free fast path when clean).
    void AccumulateDirty(UsdGenPendingDirty &&pending);
    bool NeedsCommit() const noexcept;

    // ---- commit (03 §5.4) ----------------------------------------------------------
    /// Runs the 8-step commit under _commitMutex (commit thread only).
    /// Returns the published generation, or the PREVIOUS generation unchanged
    /// when superseded mid-run (03 §5.6 — and leaves _dirty set).
    UsdGenGenerationConstPtr Commit(double frame, UsdGenCommitReason reason);
    /// The currently compiled graph (M1Imaging contract: UsdGenDirtyRouter::Rebuild
    /// needs it after each commit). Empty before the first successful compile.
    UsdGenGraph const &Graph() const noexcept { return _graph; }

    UsdGenGenerationConstPtr Generation() const noexcept;

    /// The report of the last commit (consumed by the imaging notice
    /// emitter, 06 §5.1). Valid after a Commit that published.
    UsdGenDirtyReport const &LastReport() const noexcept { return _lastReport; }
    /// Bench hook (E-1/E-2): invalidate every node's evaluation signature so
    /// the next commit re-runs every dirty chunk regardless of no-op skips.
    void InvalidateAllValues();

    // ---- density drag publication mode (02 §2.3.1, S28) ----------------------------
    void BeginDensityDrag();
    void EndDensityDrag();

    // ---- diagnostics -----------------------------------------------------------------
    UsdGenStats const &Stats() const noexcept { return _stats; }
    /// Commit-thread diagnostic snapshot, including rejected compiles/runs.
    /// Like LastReport, consume after Commit under the caller's serialization.
    UsdGenDiagnostics const &LastDiagnostics() const noexcept { return _lastDiagnostics; }
    std::optional<UsdGenNodeStats> NodeStats(UsdGenNodeId id) const;

private:
    std::mutex _commitMutex;   // non-recursive; held only by Commit
    std::atomic<bool> _dirty{false};
    std::atomic<uint64_t> _generationRequested{0};
    UsdGenPendingDirty _pending;
    std::mutex _pendingMutex;

    UsdGenCompiler _compiler;
    UsdGenDirtyReport _lastReport;
    UsdGenDiagnostics _lastDiagnostics;
    UsdGenGraphDesc _desc;          // copied by SetGraphDesc (03 §2.2)
    UsdGenGraph _graph;
    UsdGenScheduler _scheduler;
    UsdGenGenerationStore _store;
    UsdGenStats _stats;
    UsdGenContext _context = UsdGenContext::Interactive;
    bool _densityDrag = false;
    bool _devicePublicationEnabled = false;
    std::shared_ptr<const UsdGenDeviceGeneration> _stagedDeviceRevision;
    UsdGenGenerationConstPtr _stagedDeviceExpected;
    uint64_t _activeDeviceEdit = 0, _nextDeviceEditToken = 0;
    // Last commit's per-node run stats (03 §9.2), keyed by node id.
    std::unordered_map<UsdGenNodeId, UsdGenNodeRunStats> _lastNodeStats;
    // Gathers one tile's publication from the terminal buffer (03 §6.3).
    UsdGenTilePublication _BuildTilePublication(UsdGenTileView const &tv,
                                                UsdGenRunResult const &result,
                                                UsdGenGenerationConstPtr const &prev);
};

}  // namespace usdGen

#endif  // USDGEN_SESSION_H
