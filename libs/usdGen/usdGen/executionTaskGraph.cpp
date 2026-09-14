#include "usdGen/executionTaskGraph.h"
#include "usdGen/executionRuntimeInternal.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace usdGen {
namespace {
thread_local bool executingTaskGraph = false;
struct TaskGraphScope {
    bool previous = executingTaskGraph;
    bool previousAny = usdGenExecutionGraphActive;
    TaskGraphScope() {
        executingTaskGraph = true;
        usdGenExecutionGraphActive = true;
    }
    ~TaskGraphScope() {
        executingTaskGraph = previous;
        usdGenExecutionGraphActive = previousAny;
    }
};
struct RegistryKey {
    std::string backend;
    int device = -1;
    bool operator==(RegistryKey const& other) const {
        return device == other.device && backend == other.backend;
    }
};
struct RegistryHash {
    size_t operator()(RegistryKey const& key) const {
        return std::hash<std::string>{}(key.backend) ^
            (std::hash<int>{}(key.device) << 1);
    }
};
}

struct UsdGenExecutionTaskGraph::Impl : std::enable_shared_from_this<Impl> {
    struct State {
        Job job;
        std::vector<uint32_t> remaining;
        std::vector<std::vector<uint32_t>> children;
        std::vector<bool> started;
        uint32_t running = 0;
        uint32_t settled = 0;
        uint32_t edgeCount = 0;
        bool cancelled = false;
        bool completed = false;
        uint64_t lastServed = 0;
        std::exception_ptr error;
        Publish publish;
    };
    struct Run {
        std::shared_ptr<State> state;
        uint32_t task = 0;
    };
    struct DeferredCompletion {
        Completion callback;
        Publish publish;
        std::exception_ptr error;
    };

    explicit Impl(Limits value) : limits(value) {
        jobs.reserve(limits.maxJobs);
        runQueue.resize(limits.maxActiveTasks);
        workers.reserve(limits.maxActiveTasks);
        try {
            for (uint32_t i = 0; i != limits.maxActiveTasks; ++i)
                workers.emplace_back([this] { WorkerLoop(); });
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopWorkers = true;
            }
            workerReady.notify_all();
            for (auto& worker : workers) if (worker.joinable()) worker.join();
            throw;
        }
    }
    ~Impl() = default;

    Limits limits;
    mutable std::mutex mutex;
    std::condition_variable workerReady;
    std::condition_variable drained;
    std::vector<std::thread> workers;
    std::vector<Run> runQueue;
    uint32_t runHead = 0;
    uint32_t runSize = 0;
    std::vector<std::shared_ptr<State>> jobs;
    uint32_t cursor = 0;
    uint32_t active = 0;
    uint32_t callbacksActive = 0;
    uint64_t dispatchTick = 0;
    uint32_t interactiveStreak = 0;
    bool stopWorkers = false;
    std::atomic<bool> closing{false};
    std::atomic<bool> shutdownStarted{false};
    std::atomic<uint32_t> admittedJobs{0};
    std::atomic<uint32_t> admittedTasks{0};
    std::atomic<uint32_t> admittedEdges{0};

    bool IdleLocked() const noexcept {
        return jobs.empty() && runSize == 0 && active == 0 &&
            callbacksActive == 0;
    }

    void DispatchLocked() {
        while (active < limits.maxActiveTasks && !jobs.empty()) {
            bool launched = false;
            auto const count = static_cast<uint32_t>(jobs.size());
            bool hasInteractive = false;
            bool hasBackground = false;
            bool agedBackground = false;
            for (auto const& state : jobs) if (!state->cancelled) {
                bool ready = false;
                for (uint32_t i = 0; i != state->remaining.size(); ++i)
                    if (!state->started[i] && state->remaining[i] == 0) {
                        ready = true;
                        break;
                    }
                if (!ready) continue;
                if (state->job.requestClass == RequestClass::Interactive)
                    hasInteractive = true;
                else {
                    hasBackground = true;
                    agedBackground |= dispatchTick - state->lastServed >=
                        limits.backgroundAgingDispatches;
                }
            }
            bool const chooseBackground = hasBackground && (!hasInteractive ||
                (limits.interactiveBurst &&
                 interactiveStreak >= limits.interactiveBurst) ||
                (agedBackground && interactiveStreak > 0));
            for (uint32_t probe = 0; probe != count; ++probe) {
                cursor %= count;
                auto state = jobs[cursor];
                cursor = (cursor + 1) % count;
                if (state->cancelled) continue;
                bool const background =
                    state->job.requestClass == RequestClass::Background;
                if (background != chooseBackground &&
                    (chooseBackground ? hasBackground : hasInteractive))
                    continue;
                for (uint32_t i = 0; i != state->remaining.size(); ++i) {
                    if (state->started[i] || state->remaining[i] != 0) continue;
                    state->started[i] = true;
                    ++state->running;
                    ++active;
                    uint32_t const tail =
                        (runHead + runSize) % limits.maxActiveTasks;
                    runQueue[tail] = {state, i};
                    ++runSize;
                    ++dispatchTick;
                    state->lastServed = dispatchTick;
                    if (state->job.requestClass == RequestClass::Interactive) {
                        if (interactiveStreak < limits.interactiveBurst)
                            ++interactiveStreak;
                    } else interactiveStreak = 0;
                    launched = true;
                    workerReady.notify_one();
                    break;
                }
                if (launched) break;
            }
            if (!launched) break;
        }
    }

    void CompleteLocked(std::shared_ptr<State> const& state,
                        std::optional<DeferredCompletion>* callback) {
        if (state->completed || state->running != 0) return;
        state->completed = true;
        if (!state->error && !state->publish)
            state->error = std::make_exception_ptr(
                std::runtime_error("final task returned no publication"));
        DeferredCompletion deferred{
            std::move(state->job.completion), std::move(state->publish),
            state->error};
        uint32_t const taskCount =
            static_cast<uint32_t>(state->job.tasks.size());
        jobs.erase(std::remove(jobs.begin(), jobs.end(), state), jobs.end());
        std::vector<Task>{}.swap(state->job.tasks);
        state->job.cancellation = {};
        std::vector<uint32_t>{}.swap(state->remaining);
        std::vector<std::vector<uint32_t>>{}.swap(state->children);
        std::vector<bool>{}.swap(state->started);
        state->error = {};
        admittedEdges.fetch_sub(state->edgeCount, std::memory_order_acq_rel);
        admittedTasks.fetch_sub(taskCount, std::memory_order_acq_rel);
        admittedJobs.fetch_sub(1, std::memory_order_acq_rel);
        if (deferred.callback) {
            ++callbacksActive;
            callback->emplace(std::move(deferred));
        }
    }

    void Invoke(std::optional<DeferredCompletion> deferred) noexcept {
        if (!deferred) return;
        {
            TaskGraphScope scope;
            try {
                deferred->callback(std::move(deferred->publish),
                                   deferred->error);
            } catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            --callbacksActive;
            if (IdleLocked()) drained.notify_all();
        }
    }

    void Finish(std::shared_ptr<State> state, uint32_t task,
                Publish publish, std::exception_ptr error) noexcept {
        std::optional<DeferredCompletion> callback;
        try {
            std::lock_guard<std::mutex> lock(mutex);
            if (!state || state->completed) return;
            --state->running;
            --active;
            ++state->settled;
            if (error) {
                state->cancelled = true;
                if (!state->error) state->error = std::move(error);
            } else if (task == state->job.finalTask && publish) {
                state->publish = std::move(publish);
            }
            if (!state->cancelled)
                for (auto child : state->children[task])
                    --state->remaining[child];
            if (state->cancelled)
                for (uint32_t i = 0; i != state->started.size(); ++i)
                    if (!state->started[i]) {
                        state->started[i] = true;
                        ++state->settled;
                    }
            if (state->settled == state->started.size())
                CompleteLocked(state, &callback);
            DispatchLocked();
            if (IdleLocked()) drained.notify_all();
        } catch (...) { std::terminate(); }
        Invoke(std::move(callback));
    }

    void WorkerLoop() noexcept {
        for (;;) {
            Run run;
            {
                std::unique_lock<std::mutex> lock(mutex);
                workerReady.wait(lock, [this] {
                    return stopWorkers || runSize != 0;
                });
                if (stopWorkers && runSize == 0) return;
                run = std::move(runQueue[runHead]);
                runQueue[runHead] = {};
                runHead = (runHead + 1) % limits.maxActiveTasks;
                --runSize;
            }
            TaskGraphScope scope;
            struct Once {
                std::atomic<bool> done{false};
                std::atomic<unsigned> flags{0};
                Publish publish;
                std::exception_ptr error;
            };
            try {
                auto once = std::make_shared<Once>();
                auto self = shared_from_this();
                auto relay = [self, state = run.state, task = run.task,
                              once]() mutable {
                    self->Finish(std::move(state), task,
                                 std::move(once->publish), once->error);
                };
                auto finish = [once, relay](Publish publish,
                                            std::exception_ptr error) mutable {
                    if (once->done.exchange(true, std::memory_order_acq_rel)) return;
                    once->publish = std::move(publish);
                    once->error = std::move(error);
                    if (once->flags.fetch_or(1, std::memory_order_acq_rel) & 2)
                        relay();
                };
                if (closing.load(std::memory_order_acquire) ||
                    (run.state->job.cancellation.epoch &&
                     run.state->job.cancellation.Superseded()))
                    finish({}, std::make_exception_ptr(
                        std::runtime_error("task graph cancelled")));
                else
                    run.state->job.tasks[run.task].run(
                        run.state->job.cancellation, finish);
                if (once->flags.fetch_or(2, std::memory_order_acq_rel) & 1)
                    relay();
            } catch (...) {
                Finish(std::move(run.state), run.task, {},
                       std::current_exception());
            }
        }
    }

    bool Add(std::shared_ptr<State> state) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closing.load(std::memory_order_acquire)) return false;
        state->lastServed = dispatchTick;
        jobs.push_back(std::move(state));
        DispatchLocked();
        return true;
    }

    void Drain() {
        std::unique_lock<std::mutex> lock(mutex);
        drained.wait(lock, [this] { return IdleLocked(); });
    }

    void Shutdown() noexcept {
        if (shutdownStarted.exchange(true, std::memory_order_acq_rel)) return;
        closing.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto const& state : jobs) {
                state->cancelled = true;
                if (!state->error)
                    state->error = std::make_exception_ptr(
                        std::runtime_error("task graph shutdown"));
                for (uint32_t i = 0; i != state->started.size(); ++i)
                    if (!state->started[i]) {
                        state->started[i] = true;
                        ++state->settled;
                    }
            }
        }
        // Retire queued-only jobs one at a time.  This keeps teardown bounded
        // by the admitted job storage and invokes user callbacks without the
        // dispatcher lock or a separately allocated callback batch.
        for (;;) {
            std::optional<DeferredCompletion> callback;
            bool retired = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto found = std::find_if(jobs.begin(), jobs.end(),
                    [](auto const& state) {
                        return state->cancelled && !state->completed &&
                            state->running == 0 &&
                            state->settled == state->started.size();
                    });
                if (found != jobs.end()) {
                    CompleteLocked(*found, &callback);
                    retired = true;
                }
                if (IdleLocked()) drained.notify_all();
            }
            Invoke(std::move(callback));
            if (!retired) break;
        }
        Drain();
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopWorkers = true;
        }
        workerReady.notify_all();
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }
};

namespace {
using Registry = std::unordered_map<RegistryKey,
    std::shared_ptr<UsdGenExecutionTaskGraph>, RegistryHash>;
Registry& GetRegistry() {
    static auto* registry = new Registry;
    return *registry;
}
std::mutex& GetRegistryMutex() {
    static auto* mutex = new std::mutex;
    return *mutex;
}
bool SameLimits(UsdGenExecutionTaskGraph::Limits const& a,
                UsdGenExecutionTaskGraph::Limits const& b) {
    return a.maxJobs == b.maxJobs && a.maxTasks == b.maxTasks &&
        a.maxDependencyEdges == b.maxDependencyEdges &&
        a.maxActiveTasks == b.maxActiveTasks &&
        a.interactiveBurst == b.interactiveBurst &&
        a.backgroundAgingDispatches == b.backgroundAgingDispatches;
}
}

UsdGenExecutionTaskGraph::UsdGenExecutionTaskGraph(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
UsdGenExecutionTaskGraph::~UsdGenExecutionTaskGraph() {
    if (executingTaskGraph) std::terminate();
    Shutdown();
}

std::shared_ptr<UsdGenExecutionTaskGraph>
UsdGenExecutionTaskGraph::GetOrCreate(
    UsdGenExecutionRuntime& runtime, std::string backend, int device,
    Limits limits) {
    (void)runtime;
    if (backend.empty() || device < 0 || !limits.maxJobs ||
        !limits.maxTasks || !limits.maxActiveTasks ||
        !limits.interactiveBurst) return {};
    RegistryKey key{std::move(backend), device};
    std::lock_guard<std::mutex> lock(GetRegistryMutex());
    auto& registry = GetRegistry();
    if (auto found = registry.find(key); found != registry.end()) {
        auto const& graph = found->second;
        return SameLimits(graph->impl_->limits, limits) ? graph :
            std::shared_ptr<UsdGenExecutionTaskGraph>{};
    }
    auto graph = std::shared_ptr<UsdGenExecutionTaskGraph>(
        new UsdGenExecutionTaskGraph(std::make_shared<Impl>(limits)));
    registry.emplace(std::move(key), graph);
    return graph;
}

bool UsdGenExecutionTaskGraph::Submit(Job job) {
    if (!impl_ || impl_->closing.load(std::memory_order_acquire) ||
        job.tasks.empty() || job.tasks.size() > UINT32_MAX ||
        job.tasks.size() > impl_->limits.maxTasks) return false;
    auto const count = static_cast<uint32_t>(job.tasks.size());
    if (job.requestClass != RequestClass::Interactive &&
        job.requestClass != RequestClass::Background) return false;
    if (job.finalTask == UINT32_MAX) job.finalTask = count - 1;
    if (job.finalTask >= count) return false;
    uint32_t edgeCount = 0;
    for (uint32_t i = 0; i != count; ++i) {
        if (!job.tasks[i].run) return false;
        for (auto dependency : job.tasks[i].dependencies) {
            if (dependency >= count || dependency == i ||
                edgeCount == impl_->limits.maxDependencyEdges) return false;
            ++edgeCount;
        }
    }
    if (edgeCount > impl_->limits.maxDependencyEdges) return false;
    auto jobs = impl_->admittedJobs.load(std::memory_order_relaxed);
    do {
        if (jobs >= impl_->limits.maxJobs) return false;
    } while (!impl_->admittedJobs.compare_exchange_weak(
        jobs, jobs + 1, std::memory_order_acq_rel,
        std::memory_order_relaxed));
    auto tasks = impl_->admittedTasks.load(std::memory_order_relaxed);
    do {
        if (count > impl_->limits.maxTasks - tasks) {
            impl_->admittedJobs.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
    } while (!impl_->admittedTasks.compare_exchange_weak(
        tasks, tasks + count, std::memory_order_acq_rel,
        std::memory_order_relaxed));
    auto edges = impl_->admittedEdges.load(std::memory_order_relaxed);
    do {
        if (edgeCount > impl_->limits.maxDependencyEdges - edges) {
            impl_->admittedTasks.fetch_sub(count, std::memory_order_acq_rel);
            impl_->admittedJobs.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
    } while (!impl_->admittedEdges.compare_exchange_weak(
        edges, edges + edgeCount, std::memory_order_acq_rel,
        std::memory_order_relaxed));
    struct Reservation {
        Impl* impl;
        uint32_t tasks;
        uint32_t edges;
        bool transferred = false;
        ~Reservation() {
            if (transferred) return;
            impl->admittedEdges.fetch_sub(edges, std::memory_order_acq_rel);
            impl->admittedTasks.fetch_sub(tasks, std::memory_order_acq_rel);
            impl->admittedJobs.fetch_sub(1, std::memory_order_acq_rel);
        }
    } reservation{impl_.get(), count, edgeCount};
    std::vector<uint32_t> indegree(count);
    std::vector<std::vector<uint32_t>> children(count);
    for (uint32_t i = 0; i != count; ++i)
        for (auto dependency : job.tasks[i].dependencies) {
            ++indegree[i];
            children[dependency].push_back(i);
        }
    auto test = indegree;
    std::vector<uint32_t> ready;
    for (uint32_t i = 0; i != count; ++i)
        if (!test[i]) ready.push_back(i);
    uint32_t seen = 0;
    for (size_t position = 0; position != ready.size(); ++position) {
        ++seen;
        for (auto child : children[ready[position]])
            if (!--test[child]) ready.push_back(child);
    }
    if (seen != count) return false;
    std::vector<bool> ancestor(count);
    std::vector<uint32_t> stack{job.finalTask};
    while (!stack.empty()) {
        auto const index = stack.back();
        stack.pop_back();
        if (ancestor[index]) continue;
        ancestor[index] = true;
        for (auto dependency : job.tasks[index].dependencies)
            stack.push_back(dependency);
    }
    for (bool used : ancestor) if (!used) return false;
    auto state = std::make_shared<Impl::State>();
    state->job = std::move(job);
    state->remaining = std::move(indegree);
    state->children = std::move(children);
    state->started.resize(count);
    state->edgeCount = edgeCount;
    if (!impl_->Add(std::move(state))) return false;
    reservation.transferred = true;
    return true;
}

void UsdGenExecutionTaskGraph::Drain() {
    if (usdGenExecutionGraphActive)
        throw std::logic_error(
            "graph work must not synchronously drain task dispatcher");
    if (impl_) impl_->Drain();
}

UsdGenExecutionTaskGraph::AdmissionUsage
UsdGenExecutionTaskGraph::GetAdmissionUsage() const noexcept {
    if (!impl_) return {};
    return {impl_->admittedJobs.load(std::memory_order_acquire),
            impl_->admittedTasks.load(std::memory_order_acquire),
            impl_->admittedEdges.load(std::memory_order_acquire)};
}

void UsdGenExecutionTaskGraph::Shutdown() {
    if (usdGenExecutionGraphActive) std::terminate();
    if (impl_) impl_->Shutdown();
}
}
