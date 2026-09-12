#include "usdGen/executionPipeline.h"

#include <tbb/flow_graph.h>
#include <tbb/task_arena.h>
#include <stdexcept>
#include <utility>

namespace usdGen {
namespace {
thread_local bool executingPipeline = false;
struct ExecutionScope {
    bool previous = executingPipeline;
    ExecutionScope() { executingPipeline = true; }
    ~ExecutionScope() { executingPipeline = previous; }
};
}

struct UsdGenExecutionRuntime::Impl {
    explicit Impl(int concurrency) : arena(concurrency) { arena.initialize(); }
    tbb::task_arena arena;
};
UsdGenExecutionRuntime::UsdGenExecutionRuntime(int concurrency) {
    if (concurrency < 1) throw std::invalid_argument("execution concurrency must be positive");
    impl_ = std::make_shared<Impl>(concurrency);
}
UsdGenExecutionRuntime::~UsdGenExecutionRuntime() = default;

struct UsdGenExecutionPipeline::Impl {
    struct Message {
        enum class Kind { Request, Cancel, Finished, Command } kind = Kind::Request;
        uint64_t epoch = 0;
        Work work;
        Publish publish;
        Completion completion;
        std::exception_ptr error;
    };
    using Node = tbb::flow::function_node<Message, tbb::flow::continue_msg>;
    std::shared_ptr<UsdGenExecutionRuntime::Impl> runtime;
    std::shared_ptr<std::atomic<uint64_t>> accepted =
        std::make_shared<std::atomic<uint64_t>>(0);
    std::atomic<uint64_t> requested{0}, callbackFailures{0};
    std::shared_ptr<std::atomic<bool>> closing = std::make_shared<std::atomic<bool>>(false);
    tbb::flow::graph graph;
    Node owner;
    Node worker;

    explicit Impl(std::shared_ptr<UsdGenExecutionRuntime::Impl> runtime_)
        : runtime(std::move(runtime_)),
          owner(graph, tbb::flow::serial, [this](Message message) {
              ExecutionScope scope;
              Accept(std::move(message));
              return tbb::flow::continue_msg{};
          }),
          worker(graph, tbb::flow::serial, [this](Message message) {
              ExecutionScope scope;
              Cancellation cancel;
              cancel.epoch = message.epoch;
              cancel.accepted = accepted;
              cancel.closing = closing;
              if (!cancel.Superseded()) {
                  try { message.publish = message.work(cancel); }
                  catch (...) { message.error = std::current_exception(); }
              }
              message.work = {};
              message.kind = Message::Kind::Finished;
              owner.try_put(std::move(message));
              return tbb::flow::continue_msg{};
          }) {}

    uint64_t Reserve() {
        auto value = requested.load(std::memory_order_relaxed);
        do {
            if (value == UINT64_MAX || closing->load(std::memory_order_acquire)) return 0;
        } while (!requested.compare_exchange_weak(value, value + 1,
                     std::memory_order_relaxed, std::memory_order_relaxed));
        return value + 1;
    }
    void Notify(Message const& message, Outcome outcome) noexcept {
        if (!message.completion) return;
        try { message.completion(message.epoch, outcome, message.error); }
        catch (...) { callbackFailures.fetch_add(1, std::memory_order_relaxed); }
    }
    void Accept(Message message) {
        if (closing->load(std::memory_order_acquire)) {
            Notify(message, Outcome::Superseded);
            return;
        }
        if (message.kind == Message::Kind::Command) {
            try { message.publish(); }
            catch (...) { callbackFailures.fetch_add(1, std::memory_order_relaxed); }
            return;
        }
        const auto current = accepted->load(std::memory_order_relaxed);
        if (message.kind != Message::Kind::Finished) {
            if (message.epoch <= current) {
                Notify(message, Outcome::Superseded);
                return;
            }
            accepted->store(message.epoch, std::memory_order_release);
            if (message.kind == Message::Kind::Request) worker.try_put(std::move(message));
            return;
        }
        if (message.epoch != current) {
            Notify(message, Outcome::Superseded);
            return;
        }
        if (!message.error && !message.publish)
            message.error = std::make_exception_ptr(std::runtime_error("work returned no publication"));
        if (!message.error && message.publish) {
            try { message.publish(); }
            catch (...) { message.error = std::current_exception(); }
        }
        Notify(message, message.error ? Outcome::Failed : Outcome::Published);
    }
};

bool UsdGenExecutionPipeline::Cancellation::Superseded() const noexcept {
    return !accepted || !closing || closing->load(std::memory_order_acquire) ||
        accepted->load(std::memory_order_acquire) != epoch;
}
UsdGenExecutionPipeline::UsdGenExecutionPipeline(UsdGenExecutionRuntime& runtime) {
    // The graph attaches to the arena in which it is constructed, as required
    // by the installed TBB flow-graph implementation.
    runtime.impl_->arena.execute([&] { impl_ = std::make_unique<Impl>(runtime.impl_); });
}
UsdGenExecutionPipeline::~UsdGenExecutionPipeline() {
    if (executingPipeline) std::terminate();
    impl_->closing->store(true, std::memory_order_release);
    impl_->graph.wait_for_all();
}
uint64_t UsdGenExecutionPipeline::Submit(Work work, Completion completion) {
    if (!work) return 0;
    auto epoch = impl_->Reserve();
    if (!epoch) return 0;
    Impl::Message message;
    message.epoch = epoch;
    message.work = std::move(work);
    message.completion = std::move(completion);
    return impl_->owner.try_put(std::move(message)) ? epoch : 0;
}
uint64_t UsdGenExecutionPipeline::CancelPending() {
    auto epoch = impl_->Reserve();
    if (!epoch) return 0;
    Impl::Message message;
    message.kind = Impl::Message::Kind::Cancel;
    message.epoch = epoch;
    return impl_->owner.try_put(message) ? epoch : 0;
}
bool UsdGenExecutionPipeline::PostCommand(std::function<void()> command) {
    if (!command || impl_->closing->load(std::memory_order_acquire)) return false;
    Impl::Message message;
    message.kind = Impl::Message::Kind::Command;
    message.publish = std::move(command);
    return impl_->owner.try_put(std::move(message));
}
uint64_t UsdGenExecutionPipeline::AcceptedEpoch() const noexcept {
    return impl_->accepted->load(std::memory_order_acquire);
}
uint64_t UsdGenExecutionPipeline::CallbackFailures() const noexcept {
    return impl_->callbackFailures.load(std::memory_order_acquire);
}
void UsdGenExecutionPipeline::Drain() {
    if (executingPipeline) throw std::logic_error("graph work must not synchronously drain a pipeline");
    impl_->graph.wait_for_all();
}

} // namespace usdGen
