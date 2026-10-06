// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

#include "usdGen/workerPool.h"
#include "usdGen/tbbFastCores.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#endif

namespace usdGen {

namespace {

// Spin budget before a parked worker blocks: bench gaps (~0.5ms),
// interactive frame gaps (16ms), and scheduling hiccups never sleep, so
// the wake path stays out of every hot loop; a truly idle pool still
// blocks instead of burning cores. USDGEN_POOL_SPIN_US overrides (0
// forces the sleep path; the pool test uses it).
long SpinBudgetUs()
{
    char const *env = std::getenv("USDGEN_POOL_SPIN_US");
    if (env && *env) return std::atol(env);
    return 50000;
}

void CpuRelax()
{
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

} // namespace

struct UsdGenWorkerPool::State {
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake;
    // Dispatch generation: even values are idle. The master publishes
    // body/payload/count/chunk, then release-stores an odd generation and
    // wakes; workers acquire-load it. finished counts workers done with
    // the current odd generation; observed counts workers that parked on
    // the following even marker. The master joins finished, bumps even,
    // then joins observed before the next dispatch, so no worker can be
    // a whole cycle behind when a new payload publishes (a descheduled
    // sleeper delays the next dispatch; it can never miss one).
    std::atomic<unsigned> epoch{0};
    std::atomic<unsigned> finished{0};
    std::atomic<unsigned> observed{0};
    std::atomic<bool> stop{false};
    void (*body)(size_t, void *) = nullptr;
    void *payload = nullptr;
    size_t count = 0;
    // Dynamic queue: workers claim fixed chunks, so a stacked or slow
    // worker takes fewer chunks instead of setting the wall the way a
    // static split would.
    std::atomic<size_t> next{0};
    size_t chunk = 1;
    std::mutex errorMutex;
    std::exception_ptr firstError;
    // One fast CPU per worker where the topology qualifies, else empty
    // (workers float). Read once: affinity is process-stable in practice.
    std::vector<int> pinCpus;
    int workers = 0;

    void WorkerMain(int id)
    {
#if defined(__linux__) && !defined(__ANDROID__)
        // One worker per fast CPU: pinning the whole set lets the
        // scheduler stack workers on a few cores for longer than a
        // sub-millisecond region lasts. The target always intersects the
        // thread's current affinity, so taskset, cgroups, and narrower
        // pre-existing pins are respected, never widened; when the
        // 1:1 CPU falls outside the current set the worker takes the
        // intersected set instead.
        if (!pinCpus.empty()) {
            cpu_set_t current, target;
            CPU_ZERO(&current);
            CPU_ZERO(&target);
            if (pthread_getaffinity_np(pthread_self(), sizeof(current),
                                       &current) == 0) {
                if (id < int(pinCpus.size()) &&
                    CPU_ISSET(pinCpus[size_t(id)], &current)) {
                    CPU_SET(pinCpus[size_t(id)], &target);
                } else {
                    for (int cpu : pinCpus)
                        if (CPU_ISSET(cpu, &current)) CPU_SET(cpu, &target);
                }
                if (CPU_COUNT(&target) > 0 && !CPU_EQUAL(&target, &current))
                    pthread_setaffinity_np(pthread_self(), sizeof(target),
                                           &target);
            }
        }
#else
        (void)id;
#endif
        unsigned seen = 0;
        for (;;) {
            // Spin phase: a dispatch landing here costs no mutex. The
            // clock is sampled sparingly (a vDSO read per iteration
            // would double the spin's core burn while siblings still
            // compute).
            long const spinUs = SpinBudgetUs();
            auto const spinUntil =
                std::chrono::steady_clock::now() + std::chrono::microseconds(spinUs);
            for (unsigned spin = 0;; ++spin) {
                unsigned const e = epoch.load(std::memory_order_acquire);
                if (e != seen) break;
                if (stop.load(std::memory_order_relaxed)) return;
                if (spinUs <= 0) break;
                if ((spin & 255) == 0 &&
                    std::chrono::steady_clock::now() >= spinUntil)
                    break;
                CpuRelax();
            }
            unsigned e = epoch.load(std::memory_order_acquire);
            if (e == seen && !stop.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lock(mutex);
                if (epoch.load(std::memory_order_acquire) == seen &&
                    !stop.load(std::memory_order_relaxed)) {
                    wake.wait(lock, [&] {
                        return epoch.load(std::memory_order_acquire) != seen ||
                            stop.load(std::memory_order_relaxed);
                    });
                }
                if (stop.load(std::memory_order_relaxed)) return;
                e = epoch.load(std::memory_order_acquire);
            }
            if (stop.load(std::memory_order_relaxed)) return;
            if (e == seen) continue;  // defensive: re-park on a stale load
            seen = e;
            if (!(e & 1)) {
                // The master's all-done marker: acknowledge it so the
                // next dispatch knows every worker parked here.
                observed.fetch_add(1, std::memory_order_release);
                continue;
            }
            size_t const n = count;
            void (*runBody)(size_t, void *) = body;
            void *runPayload = payload;
            size_t const step = chunk;
            try {
                for (;;) {
                    size_t const lo =
                        next.fetch_add(step, std::memory_order_relaxed);
                    if (lo >= n) break;
                    size_t const hi = std::min(lo + step, n);
                    for (size_t i = lo; i < hi; ++i) runBody(i, runPayload);
                }
            } catch (...) {
                std::lock_guard<std::mutex> errors(errorMutex);
                if (!firstError) firstError = std::current_exception();
            }
            finished.fetch_add(1, std::memory_order_release);
        }
    }
};

UsdGenWorkerPool::UsdGenWorkerPool(int workers) : _workers(workers)
{
    if (workers <= 1) {
        _workers = workers < 1 ? 1 : workers;
        return;  // inline path; no threads, no state
    }
    _state = new State();
    _state->workers = workers;
    _state->pinCpus = FastCoreList(workers);
    _state->threads.reserve(size_t(workers));
    for (int i = 0; i < workers; ++i)
        _state->threads.emplace_back(&State::WorkerMain, _state, i);
}

UsdGenWorkerPool::~UsdGenWorkerPool()
{
    if (!_state) return;
    _state->stop.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->epoch.fetch_add(1, std::memory_order_release);
    }
    _state->wake.notify_all();
    for (std::thread &t : _state->threads) t.join();
    delete _state;
}

void UsdGenWorkerPool::ParallelFor(size_t count, void (*body)(size_t, void *),
                                   void *payload)
{
    if (count == 0 || !body) return;
    if (!_state) {
        for (size_t i = 0; i < count; ++i) body(i, payload);
        return;
    }
    {
        std::lock_guard<std::mutex> errors(_state->errorMutex);
        _state->firstError = nullptr;
    }
    // Publish the dispatch before the release-store hands it over.
    // Claim chunks of ~4 indices: a stacked worker's straggler bound is
    // one chunk, and the fetch_add traffic is one op per chunk.
    _state->body = body;
    _state->payload = payload;
    _state->count = count;
    _state->chunk = std::max<size_t>(
        1, std::min<size_t>(4, (count + size_t(_state->workers) - 1) /
                                   size_t(_state->workers)));
    _state->next.store(0, std::memory_order_relaxed);
    _state->finished.store(0, std::memory_order_relaxed);
    _state->observed.store(0, std::memory_order_relaxed);
    _state->epoch.fetch_add(1, std::memory_order_release);  // idle even -> work odd
    _state->wake.notify_all();  // no-op for spinning workers, wakes sleepers
    while (_state->finished.load(std::memory_order_acquire) !=
           unsigned(_state->workers))
        CpuRelax();
    _state->epoch.fetch_add(1, std::memory_order_release);  // work odd -> idle even
    // Sleepers parked between their finished++ and this bump need the
    // kick to observe the marker; without it the observed join below
    // deadlocks against a worker that never wakes.
    _state->wake.notify_all();
    while (_state->observed.load(std::memory_order_acquire) !=
           unsigned(_state->workers))
        CpuRelax();
    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> errors(_state->errorMutex);
        error = _state->firstError;
        _state->firstError = nullptr;
    }
    if (error) std::rethrow_exception(error);
}

} // namespace usdGen
