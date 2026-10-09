// usdGen engine — scheduler: private tbb::task_arena + commit driver (03 §5.3/§5.4).
//
// I8: every parallel region runs in a PRIVATE task_arena sized at the measured
// 8-thread knee (EV-001/EV-008), never the process-default 20-worker arena.
// USDGEN_THREAD_LIMIT (env) pins it explicitly; otherwise one-time calibration
// (03 §5.3) picks the smallest concurrency within 5 % of best.
// Loops are tbb::parallel_for, never pxr work:: (WorkHasConcurrency() reads
// the process-global PXR_WORK_THREAD_LIMIT and would serialise under
// PXR_WORK_THREAD_LIMIT=1, 03 §5.3 caveat). An external caller enters the
// private arena. A thread that is already a TBB worker stays in its current
// arena: joining this one from another worker deadlocks once the market is
// saturated.
#ifndef USDGEN_SCHEDULER_H
#define USDGEN_SCHEDULER_H

#include "usdGen/graph.h"
#include "usdGen/op.h"
#include "usdGen/types.h"

#include <tbb/task_arena.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// Per-node timing/counters for one Run (03 §9.2; feeds UsdGenNodeStats).
struct UsdGenNodeRunStats
{
    UsdGenNodeId id = 0;
    double captureMs = 0.0;
    double evalMs = 0.0;
    uint64_t chunksEvaluated = 0;
};

struct UsdGenRunResult
{
    UsdGenCurveBuffer const *terminalOutput = nullptr;  // owned by the graph
    TfSpan<const UsdGenTileView> tiles;
    bool superseded = false;   // a newer request arrived; the published generation is unchanged
    bool topologyChanged = false; // re-partition happened; every tile re-interleaves
    UsdGenDiagnostics diagnostics; // node diagnostics for this run
    std::vector<UsdGenNodeRunStats> nodeStats;  // one entry per node with work
};

/// The work dispatcher handed to Capture() for intra-node parallelism (03 §8.1).
class UsdGenWorkDispatcher
{
public:
    explicit UsdGenWorkDispatcher(tbb::task_arena *arena) : _arena(arena) {}
    /// Run a per-index body 0..count-1 inside the arena (capture phase).
    void ParallelFor(size_t count, void (*body)(size_t, void *), void *payload);
private:
    tbb::task_arena *_arena;
};

class UsdGenScheduler
{
public:
    using TileCompleted = std::function<void(UsdGenTileView const&,
                                             UsdGenCurveBuffer const&)>;
    /// threadLimit: 0 == env USDGEN_THREAD_LIMIT, else one-shot calibration.
    explicit UsdGenScheduler(int threadLimit = 0);
    ~UsdGenScheduler();

    UsdGenScheduler(const UsdGenScheduler &) = delete;
    UsdGenScheduler &operator=(const UsdGenScheduler &) = delete;

    int ThreadLimit() const noexcept;
    tbb::task_arena const &Arena() const noexcept { return _arena; }
    UsdGenWorkDispatcher MakeWorkDispatcher() const;

    /// The 8-step commit of 03 §5.4 (steps 1-6 engine-side; step 7 diff and
    /// step 8 notices are the session/imaging side). Evaluates every node in
    /// topological order with its dirty chunks, runs the reference lane first,
    /// interleaves dirty tiles, and checks for supersession between nodes.
    UsdGenRunResult Run(UsdGenGraph &graph, UsdGenEvalContext const &evalCtx,
                        uint64_t generationRequested,
                        TileCompleted tileCompleted = {});

private:
    tbb::task_arena _arena;
    int _threadLimit;
};

/// One-shot process calibration (03 §5.3): sweeps concurrencies {2,4,8,16,
/// min(20,physical)} over a fixed synthetic styler pass (64 chunks ≈ 2 MB)
/// and returns the smallest concurrency within 5 % of best, clamped [2,physical].
int CalibrateThreads();

}  // namespace usdGen

#endif  // USDGEN_SCHEDULER_H
