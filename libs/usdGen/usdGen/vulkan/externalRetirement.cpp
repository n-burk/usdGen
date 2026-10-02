// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT
#include "externalRetirement.h"
#include "completionService.h"

#include <array>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace usdGen::vulkan {
namespace {

class ExternalRetirementQueue final {
public:
    bool Track(std::shared_ptr<VulkanExternalRetirementDomain> const& domain) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_) return false;
            for (auto& entry : domains_) {
                if (!entry.expired()) continue;
                if (!StartWorker()) return false;
                entry = domain;
                return true;
            }
        } catch (...) {}
        return false;
    }

    bool RegisterAtExit() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return !closing_ && std::atexit(CloseVulkanExternalRetirement) == 0;
    }

    bool Enqueue(std::function<void()> callback) noexcept {
        if (!callback) return false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_ || outstanding_ == capacity_) return false;
            if (!StartWorker()) return false;
            callbacks_.push_back(std::move(callback));
            ++outstanding_;
            ready_.notify_one();
            return true;
        } catch (...) { return false; }
    }

    void Close() noexcept {
        // Multiple external shutdown callers must not concurrently join the
        // same std::thread. Callback admission uses the separate queue lock.
        std::lock_guard<std::mutex> closeLock(closeMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) std::terminate();
            worker_.join();
        }
    }

private:
    // mutex_ is held. Track starts the worker even without ready callbacks,
    // so Close can always quiesce a live service whose consumer is still held.
    bool StartWorker() noexcept {
        if (worker_.joinable()) return true;
        if (std::atexit(CloseVulkanExternalRetirement) != 0) return false;
        try { worker_ = std::thread([this] { Run(); }); return true; }
        catch (...) { return false; }
    }

    void Run() noexcept {
        for (;;) {
            std::function<void()> callback;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return closing_ || !callbacks_.empty(); });
                if (callbacks_.empty()) break;
                callback = std::move(callbacks_.front());
                callbacks_.pop_front();
            }
            try { callback(); } catch (...) {}
            // Native captures are destroyed before returning admission or
            // allowing Close's join to report this callback complete.
            callback = {};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --outstanding_;
            }
        }
        // Admission is closed and accepted callbacks have finished. Registry
        // slots are now immutable; take each weak lifetime without holding
        // the queue lock across a native join. A deferred provider retains
        // its record and every unproved resource through its domain cycle.
        for (auto const& entry : domains_)
            if (auto domain = entry.lock()) domain->Quiesce();
    }

    static constexpr size_t capacity_ = 1024;
    std::mutex mutex_, closeMutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> callbacks_;
    std::array<std::weak_ptr<VulkanExternalRetirementDomain>, 1024> domains_;
    std::thread worker_;
    size_t outstanding_ = 0;
    bool closing_ = false;
};

ExternalRetirementQueue* Queue() noexcept {
    // The queue/gate survives static destruction. Only its worker is joined;
    // late domain proofs must still see closed admission, never stale storage.
    static auto* const queue = []() noexcept {
        try { return new ExternalRetirementQueue; }
        catch (...) { return static_cast<ExternalRetirementQueue*>(nullptr); }
    }();
    return queue;
}
} // namespace

void VulkanExternalRetirementDomain::Quiesce() noexcept {
    auto lock = Lock();
    if (IsQuiesced()) return;
    // Close ingress and finish owner registration before signaling the
    // service's host-control semaphore. Held consumers are not awaited.
    // Shutdown closes ingress before its potentially throwing graph wait;
    // even a dispatch failure must still stop the native service waiter.
    try { owner_->Shutdown(); } catch (...) {}
    if (service_->CloseAndJoin()) quiesced_.store(true, std::memory_order_release);
}

std::shared_ptr<VulkanExternalRetirementDomain> TrackVulkanExternalRetirementDomain(
    std::shared_ptr<UsdGenExecutionPipeline> owner,
    std::shared_ptr<VulkanCompletionService> service) noexcept {
    if (!owner || !service || service->queueOwner() != owner.get()) return {};
    try {
        auto domain = std::make_shared<VulkanExternalRetirementDomain>();
        domain->owner_ = std::move(owner);
        domain->service_ = std::move(service);
        auto* queue = Queue();
        return queue && queue->Track(domain) ? domain : nullptr;
    } catch (...) { return {}; }
}

bool RegisterVulkanExternalRetirementAtExit() noexcept {
    auto* queue = Queue();
    return queue && queue->RegisterAtExit();
}

bool EnqueueVulkanExternalRetirement(std::function<void()> callback) noexcept {
    auto* queue = Queue();
    return queue && queue->Enqueue(std::move(callback));
}

void CloseVulkanExternalRetirement() noexcept {
    if (auto* queue = Queue()) queue->Close();
}

} // namespace usdGen::vulkan
