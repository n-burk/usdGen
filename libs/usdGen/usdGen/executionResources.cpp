#include "usdGen/executionResources.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <tbb/concurrent_unordered_map.h>
#include <utility>

namespace usdGen {
namespace {
constexpr size_t KindIndex(UsdGenExecutionResourceKind kind) noexcept {
    return static_cast<size_t>(kind);
}
constexpr size_t KindCount = KindIndex(UsdGenExecutionResourceKind::Count);
constexpr bool IsAllocationKind(UsdGenExecutionResourceKind kind) noexcept {
    return KindIndex(kind) < KindIndex(UsdGenExecutionResourceKind::Pending);
}
struct DeviceHash {
    size_t operator()(UsdGenExecutionResourceDevice const& key) const noexcept {
        return std::hash<int>{}(key.device) ^
            (std::hash<unsigned>{}(static_cast<unsigned>(key.backend)) +
             size_t{0x9e3779b9} + (std::hash<int>{}(key.device) << 6) +
             (std::hash<int>{}(key.device) >> 2));
    }
};
struct DeviceEqual {
    bool operator()(UsdGenExecutionResourceDevice const& a,
                    UsdGenExecutionResourceDevice const& b) const noexcept {
        return a.backend == b.backend && a.device == b.device;
    }
};
using DevicePools = tbb::concurrent_unordered_map<UsdGenExecutionResourceDevice,
    std::shared_ptr<UsdGenExecutionResourcePool>, DeviceHash, DeviceEqual>;
// Deliberately process-lifetime: CUDA allocations can outlive shutdown while
// their contexts are being torn down, and a quarantined charge must not vanish.
DevicePools& Pools() { static auto* pools = new DevicePools; return *pools; }
}

struct UsdGenExecutionResourcePermit::State {
    State(size_t limit, size_t headroom) : limitBytes(limit), headroomBytes(headroom) {}
    size_t const limitBytes;
    size_t const headroomBytes;
    size_t const usableBytes = limitBytes - headroomBytes;
    std::atomic<size_t> usedBytes{0};
    std::array<std::atomic<size_t>, KindCount> byKind{};
};

struct UsdGenExecutionMemoryReservationState {
    UsdGenExecutionMemoryReservationState(
        std::shared_ptr<UsdGenExecutionResourcePermit::State> pool,
        size_t initialBytes) noexcept
        : poolState(std::move(pool)), remainingBytes(initialBytes) {}

    // This lock makes Consume and Release one conservation transaction:
    // remaining pending bytes plus transferred permits always equal the
    // original pool charge until a permit or the reservation is released.
    std::mutex mutex;
    std::shared_ptr<UsdGenExecutionResourcePermit::State> poolState;
    size_t remainingBytes = 0;
    bool released = false;
};

UsdGenExecutionResourcePermit::UsdGenExecutionResourcePermit(
    std::shared_ptr<State> state, size_t bytes, UsdGenExecutionResourceKind kind,
    std::shared_ptr<UsdGenExecutionMemoryReservationState> reservationCredit) noexcept
    : state_(std::move(state)), reservationCredit_(std::move(reservationCredit)),
      bytes_(bytes), kind_(kind) {}
UsdGenExecutionResourcePermit::~UsdGenExecutionResourcePermit() { Release(); }
UsdGenExecutionResourcePermit::UsdGenExecutionResourcePermit(
    UsdGenExecutionResourcePermit&& other) noexcept
    : state_(std::move(other.state_)),
      reservationCredit_(std::move(other.reservationCredit_)),
      bytes_(other.bytes_), kind_(other.kind_) {
    other.bytes_ = 0;
}
UsdGenExecutionResourcePermit& UsdGenExecutionResourcePermit::operator=(
    UsdGenExecutionResourcePermit&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = std::move(other.state_);
        reservationCredit_ = std::move(other.reservationCredit_);
        bytes_ = other.bytes_;
        kind_ = other.kind_;
        other.bytes_ = 0;
    }
    return *this;
}
void UsdGenExecutionResourcePermit::Reclassify(UsdGenExecutionResourceKind kind) noexcept {
    // Pending is reservation-owned ledger state, never an allocation permit
    // lifetime class.
    if (!state_ || !IsAllocationKind(kind) || kind == kind_) return;
    state_->byKind[KindIndex(kind_)].fetch_sub(bytes_, std::memory_order_relaxed);
    state_->byKind[KindIndex(kind)].fetch_add(bytes_, std::memory_order_relaxed);
    kind_ = kind;
}
void UsdGenExecutionResourcePermit::Release() noexcept {
    if (!state_) return;
    // One reservation mutex linearizes child release against reservation
    // closure. A live reservation recovers the credit; a closed one lets the
    // child release its still-exact global charge normally.
    if (auto const& credit = reservationCredit_) {
        std::lock_guard<std::mutex> lock(credit->mutex);
        if (!credit->released) {
            state_->byKind[KindIndex(kind_)].fetch_sub(
                bytes_, std::memory_order_relaxed);
            state_->byKind[KindIndex(UsdGenExecutionResourceKind::Pending)].fetch_add(
                bytes_, std::memory_order_relaxed);
            credit->remainingBytes += bytes_;
            reservationCredit_.reset();
            state_.reset();
            bytes_ = 0;
            return;
        }
    }
    state_->byKind[KindIndex(kind_)].fetch_sub(bytes_, std::memory_order_relaxed);
    state_->usedBytes.fetch_sub(bytes_, std::memory_order_release);
    reservationCredit_.reset();
    state_.reset();
    bytes_ = 0;
}
void UsdGenExecutionResourcePermit::Abandon() noexcept {
    // Counters intentionally remain charged. The registry keeps State alive.
    // In particular, a reserved child must not recycle credit after its
    // backend lifetime becomes unproven.
    reservationCredit_.reset();
    state_.reset();
    bytes_ = 0;
}

UsdGenExecutionMemoryReservation::UsdGenExecutionMemoryReservation(
    std::shared_ptr<UsdGenExecutionMemoryReservationState> state) noexcept
    : state_(std::move(state)) {}
UsdGenExecutionMemoryReservation::~UsdGenExecutionMemoryReservation() { Release(); }
UsdGenExecutionMemoryReservation::UsdGenExecutionMemoryReservation(
    UsdGenExecutionMemoryReservation&& other) noexcept
    : state_(std::move(other.state_)) {}
UsdGenExecutionMemoryReservation& UsdGenExecutionMemoryReservation::operator=(
    UsdGenExecutionMemoryReservation&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = std::move(other.state_);
    }
    return *this;
}
UsdGenExecutionMemoryReservation::operator bool() const noexcept {
    auto const& state = state_;
    if (!state) return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    return !state->released;
}
size_t UsdGenExecutionMemoryReservation::RemainingBytes() const noexcept {
    auto const& state = state_;
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->released ? 0 : state->remainingBytes;
}
UsdGenExecutionResourceKind UsdGenExecutionMemoryReservation::PendingKind() const noexcept {
    return UsdGenExecutionResourceKind::Pending;
}
std::optional<UsdGenExecutionResourcePermit>
UsdGenExecutionMemoryReservation::Consume(
    size_t bytes, UsdGenExecutionResourceKind targetKind) noexcept {
    auto const& state = state_;
    if (!state || !IsAllocationKind(targetKind)) return std::nullopt;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->released || bytes > state->remainingBytes) return std::nullopt;
    auto const& pool = state->poolState;
    if (!pool) return std::nullopt;
    state->remainingBytes -= bytes;
    // Snapshot category totals may transiently observe one side of this
    // transfer, but usedBytes is intentionally untouched throughout.
    pool->byKind[KindIndex(UsdGenExecutionResourceKind::Pending)].fetch_sub(
        bytes, std::memory_order_relaxed);
    pool->byKind[KindIndex(targetKind)].fetch_add(
        bytes, std::memory_order_relaxed);
    return UsdGenExecutionResourcePermit(pool, bytes, targetKind, state);
}
void UsdGenExecutionMemoryReservation::Release() noexcept {
    auto const& state = state_;
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->released) return;
    state->released = true;
    size_t const remaining = state->remainingBytes;
    state->remainingBytes = 0;
    auto const& pool = state->poolState;
    if (!pool) return;
    pool->byKind[KindIndex(UsdGenExecutionResourceKind::Pending)].fetch_sub(
        remaining, std::memory_order_relaxed);
    pool->usedBytes.fetch_sub(remaining, std::memory_order_release);
}

UsdGenExecutionResourcePool::UsdGenExecutionResourcePool(size_t limitBytes,
                                                           size_t headroomBytes)
    : state_(std::make_shared<UsdGenExecutionResourcePermit::State>(
          limitBytes, headroomBytes > limitBytes ? limitBytes : headroomBytes)) {}
UsdGenExecutionResourcePool::~UsdGenExecutionResourcePool() = default;
std::optional<UsdGenExecutionResourcePermit>
UsdGenExecutionResourcePool::TryReserve(size_t bytes,
                                        UsdGenExecutionResourceKind kind) const noexcept {
    auto const& state = state_;
    if (!state || !IsAllocationKind(kind) || bytes > state->usableBytes) return std::nullopt;
    size_t used = state->usedBytes.load(std::memory_order_acquire);
    for (;;) {
        // `used <= usable` is an invariant. The subtraction form cannot
        // overflow and makes a malformed/corrupt counter fail closed.
        if (used > state->usableBytes || bytes > state->usableBytes - used)
            return std::nullopt;
        if (state->usedBytes.compare_exchange_weak(used, used + bytes,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            state->byKind[KindIndex(kind)].fetch_add(bytes, std::memory_order_relaxed);
            return UsdGenExecutionResourcePermit(state, bytes, kind);
        }
    }
}
std::optional<UsdGenExecutionMemoryReservation>
UsdGenExecutionResourcePool::TryReserveMemory(
    size_t bytes) const noexcept {
    auto const& state = state_;
    if (!state || bytes > state->usableBytes)
        return std::nullopt;
    std::shared_ptr<UsdGenExecutionMemoryReservationState> reservation;
    try {
        reservation = std::make_shared<UsdGenExecutionMemoryReservationState>(
            state, bytes);
    } catch (...) {
        return std::nullopt;
    }
    size_t used = state->usedBytes.load(std::memory_order_acquire);
    for (;;) {
        if (used > state->usableBytes || bytes > state->usableBytes - used)
            return std::nullopt;
        if (state->usedBytes.compare_exchange_weak(used, used + bytes,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            state->byKind[KindIndex(UsdGenExecutionResourceKind::Pending)].fetch_add(
                bytes, std::memory_order_relaxed);
            return UsdGenExecutionMemoryReservation(std::move(reservation));
        }
    }
}
UsdGenExecutionResourceSnapshot UsdGenExecutionResourcePool::Snapshot() const noexcept {
    UsdGenExecutionResourceSnapshot result;
    auto const& state = state_;
    if (!state) return result;
    result.limitBytes = state->limitBytes;
    result.headroomBytes = state->headroomBytes;
    result.usableBytes = state->usableBytes;
    result.usedBytes = state->usedBytes.load(std::memory_order_acquire);
    for (size_t i = 0; i != KindCount; ++i)
        result.byKind[i] = state->byKind[i].load(std::memory_order_acquire);
    return result;
}
std::shared_ptr<UsdGenExecutionResourcePool> GetOrCreateUsdGenExecutionResourcePool(
    UsdGenExecutionResourceDevice key, UsdGenExecutionResourceConfig config,
    bool requireMatchingConfig) noexcept {
    try {
        if (key.device < 0) return {};
        auto& pools = Pools();
        auto found = pools.find(key);
        if (found != pools.end()) {
            auto const snapshot = found->second->Snapshot();
            if (requireMatchingConfig &&
                (snapshot.limitBytes != config.limitBytes ||
                 snapshot.headroomBytes != (config.headroomBytes > config.limitBytes ? config.limitBytes : config.headroomBytes)))
                return {};
            return found->second;
        }
        auto candidate = std::make_shared<UsdGenExecutionResourcePool>(
            config.limitBytes, config.headroomBytes);
        auto inserted = pools.insert({key, candidate});
        if (!inserted.second && requireMatchingConfig) {
            auto const snapshot = inserted.first->second->Snapshot();
            if (snapshot.limitBytes != config.limitBytes ||
                snapshot.headroomBytes != (config.headroomBytes > config.limitBytes ? config.limitBytes : config.headroomBytes))
                return {};
        }
        return inserted.first->second;
    } catch (...) {
        return {};
    }
}
std::shared_ptr<UsdGenExecutionResourcePool> FindUsdGenExecutionResourcePool(
    UsdGenExecutionResourceDevice key) noexcept {
    try {
        auto& pools = Pools();
        auto found = pools.find(key);
        return found == pools.end() ? std::shared_ptr<UsdGenExecutionResourcePool>{} : found->second;
    } catch (...) { return {}; }
}

} // namespace usdGen
