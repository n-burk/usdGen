#ifndef USDGEN_VULKAN_DEVICE_CONTEXT_H
#define USDGEN_VULKAN_DEVICE_CONTEXT_H

#include "usdGen/executionResources.h"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <array>

namespace usdGen::vulkan {

// A caller-selected explicit Vulkan device.  This foundation deliberately
// does not enumerate, create, or advertise an execution backend.
// The factory supplies a Vulkan 1.2+ instance and a matching physical device,
// logical device and compute queue. Vulkan cannot reverse-query the physical
// device from VkDevice, so their association is a trusted factory contract.
class DeviceContext {
public:
    struct CreateInfo {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue computeQueue = VK_NULL_HANDLE;
        uint32_t computeQueueFamily = UINT32_MAX;
        int physicalIndex = -1;
        // Factory-assigned durable identity, not an enumeration ordinal.
        int resourceDeviceId = -1;
        // Factory-owned instance/device lifetime; raw handles alone do not own it.
        std::shared_ptr<const void> nativeLifetime;
        std::string gpuLabel;
        UsdGenExecutionResourceConfig resources;
        // Trusted creation contract: the factory enabled this feature on this
        // actual VkDevice. Physical support alone is not sufficient evidence.
        bool timelineSemaphoreEnabled = false;
        // Trusted creation contract (FP64 parity): the factory enabled
        // VkPhysicalDeviceFeatures::shaderFloat64 on this actual VkDevice.
        // Physical support alone is not sufficient evidence; pipelines that
        // emit Float64 SPIR-V must reject contexts without this flag.
        bool shaderFloat64Enabled = false;
    };

    static std::shared_ptr<DeviceContext> Create(CreateInfo const&, VkResult* result = nullptr);
    ~DeviceContext() = default;
    DeviceContext(DeviceContext const&) = delete;
    DeviceContext& operator=(DeviceContext const&) = delete;

    VkInstance instance() const noexcept { return instance_; }
    VkPhysicalDevice physicalDevice() const noexcept { return physicalDevice_; }
    VkDevice device() const noexcept { return device_; }
    VkQueue computeQueue() const noexcept { return computeQueue_; }
    uint32_t computeQueueFamily() const noexcept { return computeQueueFamily_; }
    int physicalIndex() const noexcept { return physicalIndex_; }
    int resourceDeviceId() const noexcept { return resourceDeviceId_; }
    std::array<uint8_t, VK_UUID_SIZE> const& deviceUUID() const noexcept { return deviceUUID_; }
    std::string const& gpuLabel() const noexcept { return gpuLabel_; }
    std::shared_ptr<UsdGenExecutionResourcePool> const& resources() const noexcept { return resources_; }
    bool timelineSemaphoreEnabled() const noexcept { return timelineSemaphoreEnabled_; }
    bool shaderFloat64Enabled() const noexcept { return shaderFloat64Enabled_; }

private:
    explicit DeviceContext(CreateInfo const&, std::shared_ptr<UsdGenExecutionResourcePool>);
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    uint32_t computeQueueFamily_ = UINT32_MAX;
    int physicalIndex_ = -1;
    int resourceDeviceId_ = -1;
    std::array<uint8_t, VK_UUID_SIZE> deviceUUID_{};
    std::string gpuLabel_;
    std::shared_ptr<const void> nativeLifetime_;
    std::shared_ptr<UsdGenExecutionResourcePool> resources_;
    bool timelineSemaphoreEnabled_ = false;
    bool shaderFloat64Enabled_ = false;
};
} // namespace usdGen::vulkan
#endif
