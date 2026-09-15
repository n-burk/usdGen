#ifndef USDGEN_TEST_VULKAN_NATIVE_H
#define USDGEN_TEST_VULKAN_NATIVE_H

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

// Native lifetime is real ownership, not a dummy token. Quarantined buffers
// retain this object, including the fence/command pool needed by pending work.
struct NativeOwner {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    int physicalIndex = -1;
    ~NativeOwner() {
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (commands) vkDestroyCommandPool(device, commands, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

static std::shared_ptr<NativeOwner> CreateNative(bool* unavailable,
    std::vector<char const*> const& extensions = {}, void const* enabledFeatures = nullptr) {
    *unavailable = false;
    auto native = std::make_shared<NativeOwner>();
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "usdGen Vulkan resource proof";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo create{}; create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app;
    if (vkCreateInstance(&create, nullptr, &native->instance) != VK_SUCCESS) return {};
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(native->instance, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(native->instance, &count, devices.data()) != VK_SUCCESS) return {};
    for (uint32_t index = 0; index < count && !native->physical; ++index) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[index], &props);
        if (props.vendorID != 0x10de || props.apiVersion < VK_API_VERSION_1_2) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &families, nullptr);
        std::vector<VkQueueFamilyProperties> info(families);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &families, info.data());
        for (uint32_t f = 0; f < families; ++f) {
            if (!info[f].queueCount || !(info[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            native->physical = devices[index]; native->physicalIndex = int(index);
            native->family = f;
            std::printf("Vulkan resource device: %s\n", props.deviceName);
            break;
        }
    }
    if (!native->physical) { *unavailable = true; return {}; }
    float priority = 1.f;
    VkDeviceQueueCreateInfo queue{}; queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = native->family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device{}; device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device.pNext = enabledFeatures;
    device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = uint32_t(extensions.size());
    device.ppEnabledExtensionNames = extensions.data();
    if (vkCreateDevice(native->physical, &device, nullptr, &native->device) != VK_SUCCESS) return {};
    vkGetDeviceQueue(native->device, native->family, 0, &native->queue);
    VkCommandPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = native->family;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(native->device, &pool, nullptr, &native->commands) != VK_SUCCESS) return {};
    VkFenceCreateInfo fence{}; fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(native->device, &fence, nullptr, &native->fence) != VK_SUCCESS) return {};
    return native;
}

#endif
