// usdGen imaging — engine bridge: commit-thread gate + stats-ring drain.
//
// E-2: UsdGenSession::Commit is commit-thread-only (03 §1.1). The gate flags
// the first commit thread per session and TF_CODING_ERRORs any later commit
// from a different thread. DrainStats forwards the engine's ring buffer of
// per-commit timings (usdGen::UsdGenStats::ring, 03 §7) to an optional hook
// exactly once per entry.
//
// Plan: plan/06-imaging.md §3.1 (per-session member of UsdGenImagingSession).
#ifndef USDGEN_IMAGING_ENGINE_BRIDGE_H
#define USDGEN_IMAGING_ENGINE_BRIDGE_H

#include "usdGen/stats.h"

#include "pxr/usd/sdf/path.h"
#include "pxr/pxr.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

PXR_NAMESPACE_OPEN_SCOPE

namespace usdGenImaging {

class UsdGenEngineBridge
{
public:
    using TimingHook = std::function<void(usdGen::UsdGenCommitTiming const &)>;

    /// Records the thread allowed to run engine commits (E-2).
    void RegisterCommitThread(std::thread::id tid);

    /// False until the first commit registers a thread.
    bool HasCommitThread() const;
    bool IsCommitThread() const;

    /// Call before every engine Commit; TF_CODING_ERRORs off-thread commits.
    void GateCommit(SdfPath const &descriptionPath, char const *where);

    /// Installs the timing sink; pass an empty function to remove it.
    void SetTimingHook(TimingHook hook);

    /// Forwards every ring entry not seen yet, oldest first.
    void DrainStats(usdGen::UsdGenStats const &stats);

private:
    std::thread::id _commitThread = std::thread::id();
    std::atomic<bool> _hasCommitThread{false};

    TimingHook _timingHook;
    std::mutex _hookMutex;
    size_t _seenEntries = 0;
    bool _firstDrain = true;
};

}  // namespace usdGenImaging

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // USDGEN_IMAGING_ENGINE_BRIDGE_H
