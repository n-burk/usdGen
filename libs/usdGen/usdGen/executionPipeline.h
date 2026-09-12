#ifndef USDGEN_EXECUTION_PIPELINE_H
#define USDGEN_EXECUTION_PIPELINE_H

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>

namespace usdGen {

// Shared private arena. Independent description pipelines use the same worker
// budget; ownership of a description never serializes unrelated descriptions.
class UsdGenExecutionRuntime {
public:
    explicit UsdGenExecutionRuntime(int concurrency = 8);
    ~UsdGenExecutionRuntime();
    UsdGenExecutionRuntime(UsdGenExecutionRuntime const&) = delete;
    UsdGenExecutionRuntime& operator=(UsdGenExecutionRuntime const&) = delete;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend class UsdGenExecutionPipeline;
};

// Asynchronous command -> work -> publication graph. The short command owner
// keeps accepting requests while a separate work node evaluates. Only work for
// the same pipeline is ordered (e.g. persistent RBF workspace ownership).
// Submission is not a synchronous acceptance acknowledgement. Requests become
// current when accepted by the command owner, in monotonically increasing
// ticket order; out-of-order older arrivals and completions are superseded.
class UsdGenExecutionPipeline {
public:
    enum class Outcome { Published, Superseded, Failed };
    struct Cancellation {
        uint64_t epoch = 0;
        bool Superseded() const noexcept;
    private:
        std::shared_ptr<const std::atomic<uint64_t>> accepted;
        std::shared_ptr<const std::atomic<bool>> closing;
        friend class UsdGenExecutionPipeline;
    };
    using Publish = std::function<void()>;
    // Work captures immutable input values. Return a short publication action
    // owning the completed output; it executes only for the accepted epoch.
    // Work may inspect Cancellation between dependent tasks. CUDA work must
    // explicitly bind its device and retain buffers until device completion.
    using Work = std::function<Publish(Cancellation const&)>;
    using Completion = std::function<void(uint64_t, Outcome, std::exception_ptr)>;

    explicit UsdGenExecutionPipeline(UsdGenExecutionRuntime& runtime);
    // Destruction is an external ownership boundary, never a graph callback.
    // Violating this precondition terminates instead of self-deadlocking.
    ~UsdGenExecutionPipeline();
    UsdGenExecutionPipeline(UsdGenExecutionPipeline const&) = delete;
    UsdGenExecutionPipeline& operator=(UsdGenExecutionPipeline const&) = delete;
    // Allocation/dispatch failures may throw; reserved epoch numbers are never
    // reused, so callers must allow gaps. A framework dispatch exception from
    // Drain is fatal to this pipeline; retain the last published snapshot and
    // replace the pipeline rather than assuming queued callbacks completed.
    uint64_t Submit(Work work, Completion completion = {});
    // Non-coalesced short mutation of owner state. Capture input values, not
    // caller-owned mutable references. Commands may enqueue work/commands but
    // must not wait for them. They do not themselves change the work epoch.
    bool PostCommand(std::function<void()> command);
    uint64_t CancelPending();
    uint64_t AcceptedEpoch() const noexcept;
    uint64_t CallbackFailures() const noexcept;
    // External boundary only, one draining caller. Never invoke from a graph
    // callback/work item; callback re-entry uses Submit, which never waits.
    // Before destruction the owner must stop/join external submitters.
    void Drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace usdGen
#endif
