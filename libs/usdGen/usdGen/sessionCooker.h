#ifndef USDGEN_SESSION_COOKER_H
#define USDGEN_SESSION_COOKER_H

#include "usdGen/session.h"
#include "usdGen/compiler.h"
#include "usdGen/scheduler.h"

#include <memory>

namespace usdGen {

// Private serial-work-owner state.  It is deliberately not a publication
// object: all values it returns are copied into a candidate for Session's
// command owner to accept or discard.
class UsdGenSessionCooker {
public:
    explicit UsdGenSessionCooker(int threadLimit = 0);
    ~UsdGenSessionCooker();
    UsdGenSessionCooker(UsdGenSessionCooker const&) = delete;
    UsdGenSessionCooker& operator=(UsdGenSessionCooker const&) = delete;

    /// Called solely by the serial work owner. `desc` is the exact immutable
    /// command snapshot, never an alias to command-owner mutable input.
    UsdGenGenerationConstPtr Cook(std::shared_ptr<const UsdGenGraphDesc> desc,
        UsdGenContext context, bool devicePublicationEnabled,
        UsdGenPendingDirty pending, double frame, UsdGenCommitReason reason,
        UsdGenGenerationConstPtr previous, UsdGenStats publishedStats,
        bool invalidateValues, uint64_t previousPublishedWorkerEpoch,
        uint64_t workEpoch, int callerDevice);
    UsdGenDirtyReport Report() const { return _lastReport; }
    UsdGenDiagnostics Diagnostics() const { return _lastDiagnostics; }
    UsdGenStats Stats() const { return _stats; }
    std::vector<UsdGenCudaBindingStats> CudaBindingStats() const;
    std::unordered_map<UsdGenNodeId, UsdGenNodeStats> NodeStats() const;
    std::shared_ptr<const UsdGenGraphRoutingSnapshot> Routing() const;
    UsdGenGraphDesc GraphDesc() const;
    std::shared_ptr<const UsdGenCudaExecutionPlan> CudaPlan() const;
    int NodeCount() const noexcept { return _graph.NodeCount(); }
    UsdGenNodeId NodeIdForPath(SdfPath const& path) const { return _graph.NodeIdForPath(path); }
private:
    UsdGenCompiler _compiler;
    UsdGenDirtyReport _lastReport;
    UsdGenDiagnostics _lastDiagnostics;
    UsdGenGraphDesc _desc;
    std::shared_ptr<const UsdGenGraphDesc> _descIdentity;
    UsdGenGraph _graph;
    std::unique_ptr<UsdGenCudaExecutionWorkspace> _cudaWorkspace;
    SdfPath _cudaWorkspaceDescription;
    std::shared_ptr<const std::vector<UsdGenCudaBindingStats>> _cudaBindingStats =
        std::make_shared<const std::vector<UsdGenCudaBindingStats>>();
    UsdGenScheduler _scheduler;
    UsdGenGenerationStore _store;
    UsdGenStats _stats;
    UsdGenContext _context = UsdGenContext::Interactive;
    bool _devicePublicationEnabled = false;
    std::unordered_map<UsdGenNodeId, UsdGenNodeRunStats> _lastNodeStats;
    // The graph has mutable skip/capture state. If a completed worker did not
    // become the publication baseline, its state must not suppress a later
    // cook relative to that baseline.
    uint64_t _lastCookedEpoch = 0;
    UsdGenTilePublication _BuildTilePublication(UsdGenTileView const&, UsdGenRunResult const&,
                                                UsdGenGenerationConstPtr const&);
};
} // namespace usdGen
#endif
