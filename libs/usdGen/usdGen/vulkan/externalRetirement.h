// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT
#ifndef USDGEN_VULKAN_EXTERNAL_RETIREMENT_H
#define USDGEN_VULKAN_EXTERNAL_RETIREMENT_H

#include <functional>
#include <atomic>
#include <memory>
#include <mutex>

namespace usdGen { class UsdGenExecutionPipeline; }

namespace usdGen::vulkan {
class VulkanCompletionService;

// Retained by the provider and its deferred retirement, weakly tracked by the
// process queue. The lifecycle lock serializes ordinary provider closure with
// final quiescence; neither path runs on an execution-graph worker.
class VulkanExternalRetirementDomain final {
public:
    std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(mutex_); }
    void Quiesce() noexcept;
    bool IsQuiesced() const noexcept { return quiesced_.load(std::memory_order_acquire); }
private:
    std::shared_ptr<UsdGenExecutionPipeline> owner_;
    std::shared_ptr<VulkanCompletionService> service_;
    std::mutex mutex_;
    std::atomic<bool> quiesced_{false};
    friend std::shared_ptr<VulkanExternalRetirementDomain>
    TrackVulkanExternalRetirementDomain(std::shared_ptr<UsdGenExecutionPipeline>,
                                       std::shared_ptr<VulkanCompletionService>) noexcept;
};

// Register after creating the completion service, before publishing a native
// provider. A null result rejects admission; callers must close that service.
std::shared_ptr<VulkanExternalRetirementDomain> TrackVulkanExternalRetirementDomain(
    std::shared_ptr<UsdGenExecutionPipeline>, std::shared_ptr<VulkanCompletionService>) noexcept;

// Rearm once per provider Shutdown, after lazy backend initialization. Later
// backend exit handlers must not unload code before accepted native cleanup.
bool RegisterVulkanExternalRetirementAtExit() noexcept;

// Runs accepted close callbacks on a process-owned native thread, outside any
// execution graph. Admission is bounded and permanently closes at shutdown.
// A rejected callback must remain retained by its native domain.
bool EnqueueVulkanExternalRetirement(std::function<void()> callback) noexcept;

// External lifecycle boundary, also registered with atexit after first use.
// Joins every accepted callback, then stops and joins every tracked domain's
// owner and completion waiter on the same native worker. Unproved ownership
// remains retained without waiting for consumers. Subsequent admissions fail closed. Never
// call this from a retirement callback or while holding a callback's lock.
void CloseVulkanExternalRetirement() noexcept;

} // namespace usdGen::vulkan
#endif
