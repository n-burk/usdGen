#ifndef USDGEN_EXECUTION_RUNTIME_INTERNAL_H
#define USDGEN_EXECUTION_RUNTIME_INTERNAL_H

#include "usdGen/executionPipeline.h"
#include <algorithm>
#include <tbb/task_arena.h>
#include <thread>

namespace usdGen {
// Shared callback-scope guard across pipeline and ready-task graphs.  It is
// intentionally internal: synchronous waits from either framework are invalid.
extern thread_local bool usdGenExecutionGraphActive;

// Pin the process-wide TBB worker market before any 1-slot cleanup arena is
// created. oneTBB sizes the market from the first initialized arena; the
// retirement drain arena is concurrency-one, and if it wins that race the
// imaging runtime's later request for 8 workers is ignored. Pipeline
// wait_for_all then deadlocks (observed in CI as TBB "workers limited to 1"
// plus 60s ctest timeouts after the test body had already printed PASS).
// The market arena is leaked: tearing it down at DSO unload waits for
// workers that process shutdown may already have stopped.
inline void EnsureUsdGenTbbMarket() {
    static tbb::task_arena* const market = [] {
        int n = 8;
        if (unsigned const hw = std::thread::hardware_concurrency()) {
            n = std::max(n, static_cast<int>(hw));
        }
        auto* arena = new tbb::task_arena(n, 0);
        arena->initialize();
        return arena;
    }();
    (void)market;
}

bool UsdGenProcessShutdownInProgress() noexcept;

// Private shared definition: both pipeline and task dispatchers must attach
// their flow graphs to the same arena and retain it past wrapper destruction.
struct UsdGenExecutionRuntime::Impl {
    // The runtime is fully asynchronous: all advertised slots must be usable
    // by TBB workers.  Reserving the default master slot would leave a
    // concurrency-one arena with no worker to service enqueued flow-graph
    // work unless a caller later enters it through Drain().
    explicit Impl(int concurrency) : arena(concurrency, 0) {
        EnsureUsdGenTbbMarket();
        arena.initialize();
    }
    tbb::task_arena arena;
};
}
#endif
