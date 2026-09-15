#include "usdGen/executionRetirement.h"

#include <atomic>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <tbb/concurrent_unordered_map.h>
#include <tbb/flow_graph.h>
#include <tbb/task_arena.h>
#include <utility>
#include <vector>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace usdGen {
namespace {
enum SlotFlags : uint32_t {
    Reserved = 1u << 0,
    Installing = 1u << 1,
    Retired = 1u << 2,
    Success = 1u << 3,
    Failure = 1u << 4,
    Scheduled = 1u << 5,
    Quarantined = 1u << 6,
    Accounted = 1u << 7,
    Spent = 1u << 8,
};
constexpr uint64_t _Pack(uint32_t generation, uint32_t flags) noexcept {
    return (uint64_t(generation) << 32) | flags;
}
constexpr uint32_t _Generation(uint64_t state) noexcept { return uint32_t(state >> 32); }
constexpr uint32_t _Flags(uint64_t state) noexcept { return uint32_t(state); }

constexpr size_t _BackendIndex(UsdGenExecutionResourceBackend backend) noexcept {
    switch (backend) {
    case UsdGenExecutionResourceBackend::Cuda: return 0;
    case UsdGenExecutionResourceBackend::Metal: return 1;
    case UsdGenExecutionResourceBackend::Vulkan: return 2;
    }
    return 3;
}
std::array<std::atomic<bool>, 3>& _BackendOpen() {
    static auto* open = [] {
        auto* result = new std::array<std::atomic<bool>, 3>;
        for (auto& value : *result) value.store(true, std::memory_order_relaxed);
        return result;
    }();
    return *open;
}
bool _BackendIsOpen(UsdGenExecutionResourceBackend backend) noexcept {
    size_t const index = _BackendIndex(backend);
    return index < _BackendOpen().size() &&
        _BackendOpen()[index].load(std::memory_order_acquire);
}

// A single guard covers every service: invoking Drain from a cleanup worker,
// including a different device service, would wait on work that cannot make
// progress until the current task returns.
thread_local bool _retirementCleanupActive = false;
std::shared_ptr<UsdGenExecutionRetirementInstallTestGate>& _InstallTestGate() {
    static auto* gate = new std::shared_ptr<UsdGenExecutionRetirementInstallTestGate>;
    return *gate;
}

struct DeviceHash {
    size_t operator()(UsdGenExecutionResourceDevice const& key) const noexcept {
        return std::hash<int>{}(key.device) ^
            (std::hash<unsigned>{}(unsigned(key.backend)) + size_t{0x9e3779b9} +
             (std::hash<int>{}(key.device) << 6) +
             (std::hash<int>{}(key.device) >> 2));
    }
};
struct DeviceEqual {
    bool operator()(UsdGenExecutionResourceDevice const& a,
                    UsdGenExecutionResourceDevice const& b) const noexcept {
        return a.backend == b.backend && a.device == b.device;
    }
};
} // namespace

struct UsdGenExecutionRetirementSignal::State
    : std::enable_shared_from_this<UsdGenExecutionRetirementSignal::State> {
    struct Slot {
        // One CAS value prevents a late callback from validating generation N
        // then mutating flags for generation N+1.
        std::atomic<uint64_t> state{0};
        std::atomic<UsdGenExecutionRetirementPayload*> payload{nullptr};
    };

    explicit State(size_t capacity, UsdGenExecutionResourceBackend backend_)
        : backend(backend_), arena(1, 0) {
        slots.reserve(capacity);
        for (size_t i = 0; i != capacity; ++i) slots.emplace_back(new Slot);
        arena.initialize();
        // flow::graph captures the current arena at construction. It must be
        // created while entered into the dedicated cleanup arena.
        arena.execute([this] { drainGraph.reset(new tbb::flow::graph); });
    }

    bool ClaimDrain(Slot& slot, uint32_t generation) noexcept {
        uint64_t old = slot.state.load(std::memory_order_acquire);
        for (;;) {
            if (_Generation(old) != generation || (_Flags(old) & Accounted)) return false;
            if (slot.state.compare_exchange_weak(
                    old, _Pack(generation, _Flags(old) | Accounted),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
    }
    void ReleaseDrain(Slot& slot, uint32_t generation) noexcept {
        if (ClaimDrain(slot, generation)) drainGraph->release_wait();
    }

    // Exactly one caller makes a terminal claim. The object is deliberately
    // retained in its fixed slot; Quarantine must not release callback data.
    void Quarantine(Slot& slot, uint32_t generation, bool callPayload) noexcept {
        uint64_t old = slot.state.load(std::memory_order_acquire);
        for (;;) {
            if (_Generation(old) != generation || (_Flags(old) & Quarantined)) return;
            if (slot.state.compare_exchange_weak(
                    old, _Pack(generation, _Flags(old) | Quarantined),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                break;
        }
        if (callPayload) {
            if (auto* payload = slot.payload.load(std::memory_order_acquire))
                payload->Quarantine();
        }
        ReleaseDrain(slot, generation);
    }

    // Closing must never steal the drain credit from an already scheduled
    // Process: it may have crossed the backend gate before the close
    // linearized, and Drain must wait for that cleanup to finish.  This CAS
    // claims only an unscheduled retired payload.
    void QuarantineUnscheduled(Slot& slot, uint32_t generation) noexcept {
        uint64_t old = slot.state.load(std::memory_order_acquire);
        for (;;) {
            if (_Generation(old) != generation) return;
            uint32_t const flags = _Flags(old);
            if (!(flags & Retired) || (flags & (Scheduled | Quarantined))) return;
            if (slot.state.compare_exchange_weak(
                    old, _Pack(generation, flags | Quarantined),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                ReleaseDrain(slot, generation);
                return;
            }
        }
    }

    void Process(size_t index, uint32_t generation) noexcept {
        _retirementCleanupActive = true;
        struct Reset { ~Reset() { _retirementCleanupActive = false; } } reset;
        Slot& slot = *slots[index];
        uint64_t state = slot.state.load(std::memory_order_acquire);
        uint32_t flags = _Flags(state);
        if (_Generation(state) != generation ||
            (flags & (Reserved | Retired | Scheduled)) !=
                (Reserved | Retired | Scheduled) ||
            (flags & Quarantined))
            return;

        // A Process which observes close before crossing this gate performs no
        // payload virtual call. A pre-close observer retains its Drain credit,
        // so QuiesceBackend cannot return until that cleanup has finished.
        if (backendQuiesced.load(std::memory_order_acquire) ||
            !_BackendIsOpen(backend)) {
            Quarantine(slot, generation, false);
            return;
        }
        auto* payload = slot.payload.load(std::memory_order_acquire);
        if ((flags & Failure) || !payload || !payload->IsComplete()) {
            Quarantine(slot, generation, true);
            return;
        }
        if (!payload->Destroy()) {
            Quarantine(slot, generation, true);
            return;
        }

        delete slot.payload.exchange(nullptr, std::memory_order_acq_rel);
        bool const releaseDrain = ClaimDrain(slot, generation);
        // Advance before exposing the empty slot. A late signal for this
        // ticket can no longer mark a free slot as signalled.
        if (generation == UINT32_MAX) {
            slot.state.store(_Pack(generation, Spent), std::memory_order_release);
        } else {
            slot.state.store(_Pack(generation + 1, 0), std::memory_order_release);
        }
        // Capacity is visible before a Drain waiter is released.
        if (releaseDrain) drainGraph->release_wait();
    }

    bool ClaimAndDispatch(size_t index, uint32_t generation) noexcept {
        Slot& slot = *slots[index];
        if (backendQuiesced.load(std::memory_order_acquire) ||
            !_BackendIsOpen(backend)) {
            QuarantineUnscheduled(slot, generation);
            return true;
        }
        uint64_t old = slot.state.load(std::memory_order_acquire);
        for (;;) {
            if (_Generation(old) != generation) return false;
            uint32_t flags = _Flags(old);
            if ((flags & (Reserved | Retired)) != (Reserved | Retired) ||
                !(flags & (Success | Failure)) ||
                (flags & (Scheduled | Quarantined)))
                return true;
            if (slot.state.compare_exchange_weak(
                    old, _Pack(generation, flags | Scheduled),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                break;
        }
        try {
            auto self = shared_from_this();
            arena.enqueue([self, index, generation] { self->Process(index, generation); });
            return true;
        } catch (...) {
            // TBB dispatch failure is terminal but is never an inline backend
            // cleanup path. The payload and all permits remain retained.
            Quarantine(slot, generation, false);
            return false;
        }
    }

    void Signal(size_t index, uint32_t generation, bool success) noexcept {
        if (index >= slots.size()) return;
        Slot& slot = *slots[index];
        uint64_t old = slot.state.load(std::memory_order_acquire);
        for (;;) {
            if (_Generation(old) != generation) return;
            uint32_t flags = _Flags(old);
            // The first terminal signal is authoritative. A duplicate or an
            // opposite late status cannot retroactively change cleanup.
            if (!(flags & Reserved) ||
                (flags & (Success | Failure | Quarantined | Spent))) return;
            uint32_t nextFlags = flags | (success ? Success : Failure);
            if (slot.state.compare_exchange_weak(
                    old, _Pack(generation, nextFlags),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                if ((nextFlags & (Reserved | Retired)) == (Reserved | Retired))
                    ClaimAndDispatch(index, generation);
                return;
            }
        }
    }

    tbb::task_arena arena;
    std::unique_ptr<tbb::flow::graph> drainGraph;
    std::vector<std::unique_ptr<Slot>> slots;
    std::atomic<bool> accepting{true};
    std::atomic<bool> backendQuiesced{false};
    UsdGenExecutionResourceBackend const backend;
};

struct UsdGenExecutionRetirementService::Impl
    : UsdGenExecutionRetirementSignal::State {
    using UsdGenExecutionRetirementSignal::State::State;
};

UsdGenExecutionRetirementSignal::UsdGenExecutionRetirementSignal(
    std::shared_ptr<State> state, size_t slot, uint64_t generation) noexcept
    : state_(std::move(state)), slot_(slot), generation_(generation) {}
void UsdGenExecutionRetirementSignal::SignalSuccess() const noexcept {
    if (state_) state_->Signal(slot_, uint32_t(generation_), true);
}
void UsdGenExecutionRetirementSignal::SignalFailure() const noexcept {
    if (state_) state_->Signal(slot_, uint32_t(generation_), false);
}

UsdGenExecutionRetirementTicket::UsdGenExecutionRetirementTicket(
    std::shared_ptr<UsdGenExecutionRetirementSignal::State> state, size_t slot,
    uint64_t generation) noexcept
    : state_(std::move(state)), slot_(slot), generation_(generation) {}
UsdGenExecutionRetirementTicket::~UsdGenExecutionRetirementTicket() { Reset(); }
UsdGenExecutionRetirementTicket::UsdGenExecutionRetirementTicket(
    UsdGenExecutionRetirementTicket&& other) noexcept
    : state_(std::move(other.state_)), slot_(other.slot_), generation_(other.generation_) {}
UsdGenExecutionRetirementTicket& UsdGenExecutionRetirementTicket::operator=(
    UsdGenExecutionRetirementTicket&& other) noexcept {
    if (this != &other) {
        Reset(); state_ = std::move(other.state_); slot_ = other.slot_; generation_ = other.generation_;
    }
    return *this;
}
void UsdGenExecutionRetirementTicket::Reset() noexcept {
    if (!state_ || slot_ >= state_->slots.size()) { state_.reset(); return; }
    auto& slot = *state_->slots[slot_];
    uint64_t old = slot.state.load(std::memory_order_acquire);
    while (_Generation(old) == uint32_t(generation_) && !(_Flags(old) & Retired)) {
        uint32_t const nextGeneration = uint32_t(generation_) == UINT32_MAX
            ? uint32_t(generation_) : uint32_t(generation_) + 1;
        uint32_t const nextFlags = uint32_t(generation_) == UINT32_MAX
            ? uint32_t(Spent) : uint32_t{0};
        if (slot.state.compare_exchange_weak(old, _Pack(nextGeneration, nextFlags),
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) break;
    }
    state_.reset();
}
UsdGenExecutionRetirementSignal UsdGenExecutionRetirementTicket::MakeSignal() const noexcept {
    return {state_, slot_, generation_};
}
bool UsdGenExecutionRetirementTicket::Retire(
    std::unique_ptr<UsdGenExecutionRetirementPayload> payload) noexcept {
    if (!payload || !state_ || slot_ >= state_->slots.size()) return false;
    auto state = std::move(state_);
    auto& slot = *state->slots[slot_];
    uint32_t const generation = uint32_t(generation_);

    // Reserve before exposing the installing state. A concurrent backend
    // quiesce can then safely wait for this installation gap without a
    // polling protocol; losing the claim releases the unused credit below.
    state->drainGraph->reserve_wait();
    if (auto gate = std::atomic_load_explicit(&_InstallTestGate(), std::memory_order_acquire);
        gate && !gate->claimed.exchange(true, std::memory_order_acq_rel)) {
        gate->entered.store(true, std::memory_order_release);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!gate->release.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                gate->timedOut.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
        }
    }
    // Claim the one ticket-owned installation phase before exposing the raw
    // payload. Signal may race and set its bit, but cannot dispatch before
    // Retired is published below.
    uint64_t old = slot.state.load(std::memory_order_acquire);
    for (;;) {
        if (_Generation(old) != generation || !(_Flags(old) & Reserved) ||
            (_Flags(old) & (Installing | Retired))) {
            state->drainGraph->release_wait();
            return false;
        }
        if (slot.state.compare_exchange_weak(old, _Pack(generation, _Flags(old) | Installing),
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) break;
    }
    slot.payload.store(payload.release(), std::memory_order_release);

    // Preserve a concurrently recorded signal and retry CAS; do not reserve a
    // second wait credit or turn a successful signal into quarantine.
    old = slot.state.load(std::memory_order_acquire);
    for (;;) {
        uint32_t flags = _Flags(old);
        if (_Generation(old) != generation || !(flags & Installing)) {
            // Impossible for a valid ticket; preserve the payload forever.
            state->Quarantine(slot, generation, false);
            return false;
        }
        uint32_t next = (flags | Retired) & ~Installing;
        if (slot.state.compare_exchange_weak(old, _Pack(generation, next),
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            if (state->backendQuiesced.load(std::memory_order_acquire) ||
                !_BackendIsOpen(state->backend))
                state->QuarantineUnscheduled(slot, generation);
            else if (next & (Success | Failure)) state->ClaimAndDispatch(slot_, generation);
            return true;
        }
    }
}

UsdGenExecutionRetirementService::UsdGenExecutionRetirementService(
    size_t capacity, UsdGenExecutionResourceBackend backend)
    : impl_(std::make_shared<Impl>(capacity, backend)) {}
UsdGenExecutionRetirementService::~UsdGenExecutionRetirementService() = default;
std::optional<UsdGenExecutionRetirementTicket>
UsdGenExecutionRetirementService::TryReserve() noexcept {
    auto state = impl_;
    if (!state || !state->accepting.load(std::memory_order_acquire) ||
        state->backendQuiesced.load(std::memory_order_acquire) ||
        !_BackendIsOpen(state->backend)) return {};
    for (size_t i = 0; i != state->slots.size(); ++i) {
        auto& slot = *state->slots[i];
        uint64_t old = slot.state.load(std::memory_order_acquire);
        for (;;) {
            if (_Flags(old) != 0 || _Generation(old) == UINT32_MAX) break;
            uint32_t generation = _Generation(old) + 1;
            if (slot.state.compare_exchange_weak(old, _Pack(generation, Reserved),
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                // A close which linearized before this successful recheck wins.
                if (state->accepting.load(std::memory_order_acquire) &&
                    !state->backendQuiesced.load(std::memory_order_acquire) &&
                    _BackendIsOpen(state->backend))
                    return UsdGenExecutionRetirementTicket(state, i, generation);
                uint64_t expected = _Pack(generation, Reserved);
                slot.state.compare_exchange_strong(expected, _Pack(generation, 0),
                                                   std::memory_order_acq_rel);
                return {};
            }
        }
    }
    return {};
}
namespace {
// On Windows a std::atexit handler or static destructor that lives in a DLL
// runs from the loader's process shutdown, after ExitProcess has terminated
// every other thread. The cleanup arena's worker is gone by then, so a drain
// wait issued there can never be serviced and spins for as long as the process
// is allowed to live (observed under ctest as a 100% CPU timeout after the
// test body had finished). ntdll exposes that loader state; when it is set
// the wait is skipped, which changes nothing observable: no thread remains
// that could run or observe the cleanup, and the OS reclaims the mapping.
// Linux runs these handlers inside exit() with the workers alive, so the wait
// stays in place there.
bool _ProcessShutdownInProgress() noexcept {
#if defined(_WIN32)
    using Fn = BOOLEAN(NTAPI*)();
    static Fn const fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress")));
    return fn && fn();
#else
    return false;
#endif
}
}

void UsdGenExecutionRetirementService::Shutdown() noexcept {
    if (impl_) impl_->accepting.store(false, std::memory_order_release);
}
void UsdGenExecutionRetirementService::QuiesceBackend() {
    auto state = impl_;
    if (!state) return;
    if (_retirementCleanupActive) std::terminate();
    state->accepting.store(false, std::memory_order_release);
    state->backendQuiesced.store(true, std::memory_order_release);
    // Only unscheduled retired slots are claimed here. Scheduled work owns
    // its Retire reserve_wait until Process observes the close or completes.
    for (auto const& entry : state->slots) {
        auto& slot = *entry;
        uint64_t const observed = slot.state.load(std::memory_order_acquire);
        state->QuarantineUnscheduled(slot, _Generation(observed));
    }
    if (!_ProcessShutdownInProgress()) state->drainGraph->wait_for_all();
}
bool UsdGenExecutionRetirementService::BackendQuiesced() const noexcept {
    auto state = impl_;
    return state && state->backendQuiesced.load(std::memory_order_acquire);
}
void UsdGenExecutionRetirementService::Drain() {
    if (_retirementCleanupActive) std::terminate();
    if (impl_ && impl_->drainGraph && !_ProcessShutdownInProgress())
        impl_->drainGraph->wait_for_all();
}

namespace {
using Services = tbb::concurrent_unordered_map<UsdGenExecutionResourceDevice,
    std::shared_ptr<UsdGenExecutionRetirementService>, DeviceHash, DeviceEqual,
    // This process-lifetime registry intentionally survives shutdown so late
    // backend callbacks retain their payload state. Use std::allocator here
    // to make per-device registry metadata visible to leak instrumentation;
    // it does not alter service, slot, or payload ownership.
    std::allocator<std::pair<UsdGenExecutionResourceDevice const,
        std::shared_ptr<UsdGenExecutionRetirementService>>>>;
Services& _Services() { static auto* services = new Services; return *services; }
}
std::shared_ptr<UsdGenExecutionRetirementService> GetOrCreateUsdGenExecutionRetirementService(
    UsdGenExecutionResourceDevice key, UsdGenExecutionRetirementConfig config,
    bool requireMatchingConfig) noexcept {
    try {
        if (key.device < 0 || config.capacity == 0 || !_BackendIsOpen(key.backend)) return {};
        auto& services = _Services();
        auto found = services.find(key);
        if (found != services.end()) {
            if (requireMatchingConfig && found->second->impl_->slots.size() != config.capacity) return {};
            return _BackendIsOpen(key.backend) ? found->second : std::shared_ptr<UsdGenExecutionRetirementService>{};
        }
        auto candidate = std::shared_ptr<UsdGenExecutionRetirementService>(
            new UsdGenExecutionRetirementService(config.capacity, key.backend));
        auto inserted = services.insert({key, candidate});
        if (!inserted.second && requireMatchingConfig &&
            inserted.first->second->impl_->slots.size() != config.capacity) return {};
        if (!_BackendIsOpen(key.backend)) {
            // This raced a backend-wide close. No ticket escaped this call,
            // and the shared backend flag prevents Process dispatch; do not
            // add a second concurrent graph wait to the lifecycle closer.
            inserted.first->second->Shutdown();
            return {};
        }
        return inserted.first->second;
    } catch (...) { return {}; }
}
std::shared_ptr<UsdGenExecutionRetirementService> FindUsdGenExecutionRetirementService(
    UsdGenExecutionResourceDevice key) noexcept {
    try { auto it = _Services().find(key); return it == _Services().end() ? std::shared_ptr<UsdGenExecutionRetirementService>{} : it->second; }
    catch (...) { return {}; }
}
bool IsUsdGenExecutionRetirementBackendOpen(
    UsdGenExecutionResourceBackend backend) noexcept {
    return _BackendIsOpen(backend);
}
void QuiesceUsdGenExecutionRetirementBackend(
    UsdGenExecutionResourceBackend backend) noexcept {
    size_t const index = _BackendIndex(backend);
    if (index >= _BackendOpen().size()) return;
    _BackendOpen()[index].store(false, std::memory_order_release);
    // The registry is process-lifetime, so its service identities remain
    // stable while late static owners transfer payloads into fixed slots.
    try {
        for (auto const& entry : _Services())
            if (entry.first.backend == backend) entry.second->QuiesceBackend();
    } catch (...) { std::terminate(); }
}
void SetUsdGenExecutionRetirementInstallTestGate(
    std::shared_ptr<UsdGenExecutionRetirementInstallTestGate> gate) noexcept {
    std::atomic_store_explicit(&_InstallTestGate(), std::move(gate),
                               std::memory_order_release);
}
} // namespace usdGen
