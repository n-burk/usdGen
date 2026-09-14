#ifndef USDGEN_EXECUTION_RUNTIME_INTERNAL_H
#define USDGEN_EXECUTION_RUNTIME_INTERNAL_H

#include "usdGen/executionPipeline.h"
#include <tbb/task_arena.h>

namespace usdGen {
// Shared callback-scope guard across pipeline and ready-task graphs.  It is
// intentionally internal: synchronous waits from either framework are invalid.
extern thread_local bool usdGenExecutionGraphActive;
// Private shared definition: both pipeline and task dispatchers must attach
// their flow graphs to the same arena and retain it past wrapper destruction.
struct UsdGenExecutionRuntime::Impl {
    // The runtime is fully asynchronous: all advertised slots must be usable
    // by TBB workers.  Reserving the default master slot would leave a
    // concurrency-one arena with no worker to service enqueued flow-graph
    // work unless a caller later enters it through Drain().
    explicit Impl(int concurrency) : arena(concurrency, 0) { arena.initialize(); }
    tbb::task_arena arena;
};
}
#endif
