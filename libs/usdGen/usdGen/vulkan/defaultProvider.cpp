// Production default Vulkan Session provider.
//
// Owns a complete Vulkan device domain (instance, logical device, pipelines,
// completion service, plan executor) so plain Sessions route Vulkan without
// test-only injection. Device selection accepts discrete or integrated GPUs
// with Vulkan 1.2+ and timeline semaphores; software rasterizers (llvmpipe)
// and other non-GPU devices are rejected so a missing GPU reports as
// unavailable instead of silently executing on the CPU.
#include "defaultProvider.h"

#include "completionService.h"
#include "deformPipeline.h"
#include "deviceContext.h"
#include "deviceGenerationAdapter.h"
#include "executionPlan.h"
#include "lengthCompactionPipeline.h"
#include "lengthScalePipeline.h"
#include "noisePipeline.h"
#include "nonWidthComparePipeline.h"
#include "planExecutor.h"
#include "sessionProvider.h"
#include "widthBlendPipeline.h"
#include "widthPipeline.h"
#include "usdGen/executionCache.h"
#include "usdGen/executionTaskGraph.h"
#include "usdGen/sessionDeviceProvider.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace usdGen::vulkan {
namespace {

void SetReason(std::string* reason, char const* message) noexcept {
    if (!reason) return;
    try { *reason = message; } catch (...) {}
}

// Raw Vulkan lifetime: instance, logical device and compute queue. Queued
// work must be idle before destruction; the wrapper Shutdown drains first.
struct DefaultNative {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    int physicalIndex = -1;
    ~DefaultNative() {
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

bool LoadSpirvFile(char const* path, std::vector<uint32_t>* words) {
    if (!path || !*path || !words) return false;
    std::ifstream file(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(file)), {});
    if (raw.empty() || raw.size() % sizeof(uint32_t) != 0) return false;
    words->resize(raw.size() / sizeof(uint32_t));
    std::memcpy(words->data(), raw.data(), raw.size());
    return true;
}

// Installed shaders live in share/usdGen/vulkan; build-tree shaders live in
// <build>/vulkan. USDGEN_VULKAN_SHADER_DIR overrides both (tests).
std::string ShaderDir() {
    if (char const* env = std::getenv("USDGEN_VULKAN_SHADER_DIR"))
        if (*env) return env;
    static char const* candidates[] = {
        "vulkan",
        "share/usdGen/vulkan",
        "/usr/local/share/usdGen/vulkan",
        "/usr/share/usdGen/vulkan",
    };
    for (char const* dir : candidates) {
        std::string probe = std::string(dir) + "/width.spv";
        std::ifstream file(probe, std::ios::binary);
        if (file.good()) return dir;
    }
    return "vulkan";
}

bool LoadShader(std::string const& dir, char const* name,
                std::vector<uint32_t>* words) {
    return LoadSpirvFile((dir + "/" + name).c_str(), words);
}

int DeviceScore(VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
    default: return 0;
    }
}

struct DevicePick {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    int index = -1;
    uint32_t family = UINT32_MAX;
    bool shaderFloat64 = false;
    int score = 0;
};

// Enumerates instance devices and picks the best GPU with a compute queue.
// Returns false when no suitable device exists (host has no GPU).
bool PickDevice(VkInstance instance, DevicePick* pick) noexcept {
    if (!instance || !pick) return false;
    try {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) !=
                VK_SUCCESS ||
            !count)
            return false;
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) !=
            VK_SUCCESS)
            return false;
    DevicePick best;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_2) continue;
        int const score = DeviceScore(props.deviceType);
        if (!score || score < best.score) continue;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
        timeline.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &timeline;
        vkGetPhysicalDeviceFeatures2(devices[i], &features);
        if (!timeline.timelineSemaphore) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, nullptr);
        std::vector<VkQueueFamilyProperties> info(families);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families,
                                                 info.data());
        uint32_t chosen = UINT32_MAX;
        for (uint32_t f = 0; f < families; ++f) {
            if (!info[f].queueCount ||
                !(info[f].queueFlags & VK_QUEUE_COMPUTE_BIT))
                continue;
            // Prefer a dedicated compute queue over a graphics-capable one.
            if (chosen == UINT32_MAX ||
                ((info[chosen].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                 !(info[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)))
                chosen = f;
        }
        if (chosen == UINT32_MAX) continue;
        best.physical = devices[i];
        best.index = int(i);
        best.family = chosen;
        best.shaderFloat64 = features.features.shaderFloat64 == VK_TRUE;
        best.score = score;
    }
        if (!best.physical) return false;
        *pick = best;
        return true;
    } catch (...) {
        return false;
    }
}

std::shared_ptr<DefaultNative> CreateNative(DevicePick const& pick,
                                            bool enableFloat64,
                                            std::string* reason) {
    auto native = std::make_shared<DefaultNative>();
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "usdGen Vulkan default provider";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    if (vkCreateInstance(&instanceInfo, nullptr, &native->instance) !=
        VK_SUCCESS) {
        SetReason(reason, "Vulkan default provider cannot create an instance");
        return {};
    }
    // Re-enumerate on the owned instance: the probe instance is gone.
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(native->instance, &count, nullptr) !=
            VK_SUCCESS ||
        uint32_t(pick.index) >= count) {
        SetReason(reason, "Vulkan default provider lost its physical device");
        return {};
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(native->instance, &count, devices.data()) !=
        VK_SUCCESS) {
        SetReason(reason, "Vulkan default provider lost its physical device");
        return {};
    }
    native->physical = devices[size_t(pick.index)];
    native->physicalIndex = pick.index;
    native->family = pick.family;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    timeline.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures core{};
    core.shaderFloat64 = enableFloat64 ? VK_TRUE : VK_FALSE;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = native->family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &timeline;
    deviceInfo.pEnabledFeatures = &core;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    if (vkCreateDevice(native->physical, &deviceInfo, nullptr,
                       &native->device) != VK_SUCCESS) {
        SetReason(reason, "Vulkan default provider cannot create a logical device");
        return {};
    }
    vkGetDeviceQueue(native->device, native->family, 0, &native->queue);
    if (!native->queue) {
        SetReason(reason, "Vulkan default provider has no compute queue");
        return {};
    }
    return native;
}

// Delegating wrapper: holds every native lifetime (instance/device, runtime,
// pipelines, executor) behind the neutral provider interface.
class DefaultVulkanSessionProvider final
    : public UsdGenSessionDeviceProvider,
      public std::enable_shared_from_this<DefaultVulkanSessionProvider> {
public:
    static std::shared_ptr<DefaultVulkanSessionProvider> Create(
        std::string* reason);
    ~DefaultVulkanSessionProvider() override { Shutdown(); }

    UsdGenSessionDevicePlanHandle Compile(
        UsdGenGraphDesc const& desc,
        UsdGenDiagnostics* diagnostics) const override {
        return inner_ ? inner_->Compile(desc, diagnostics) : nullptr;
    }
    UsdGenSessionDeviceIdentity Identity() const noexcept override {
        return identity_;
    }
    bool NotifyContextLost(
        UsdGenSessionDeviceIdentity const& lost) noexcept override {
        return inner_ ? inner_->NotifyContextLost(lost) : false;
    }
    bool Submit(UsdGenSessionDeviceRequest request) override {
        return inner_ ? inner_->Submit(std::move(request)) : false;
    }
    void Shutdown() noexcept override {
        bool expected = false;
        if (!closing_.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel))
            return;
        try {
            if (inner_) inner_->Shutdown();
            if (executor_) executor_->Shutdown();
            if (queueOwner_) queueOwner_->Drain();
            if (domain_) (void)domain_->Close();
            if (completion_) completion_->CloseAndJoin();
        } catch (...) {}
    }

private:
    DefaultVulkanSessionProvider() = default;
    std::shared_ptr<DefaultNative> native_;
    std::shared_ptr<UsdGenExecutionRuntime> runtime_;
    std::shared_ptr<UsdGenExecutionPipeline> queueOwner_;
    std::shared_ptr<VulkanCompletionService> completion_;
    std::shared_ptr<VulkanGenerationAdapterDomain> domain_;
    std::shared_ptr<UsdGenExecutionTaskGraph> preparer_;
    std::shared_ptr<DeviceContext> context_;
    std::shared_ptr<VulkanPlanExecutor> executor_;
    std::shared_ptr<VulkanSessionProvider> inner_;
    UsdGenSessionDeviceIdentity identity_{};
    std::atomic<bool> closing_{false};
};

std::shared_ptr<DefaultVulkanSessionProvider>
DefaultVulkanSessionProvider::Create(std::string* reason) {
    // Probe first on a transient instance so a GPU-less host fails before
    // allocating any long-lived state.
    VkInstance probe = VK_NULL_HANDLE;
    {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "usdGen Vulkan default probe";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        if (vkCreateInstance(&info, nullptr, &probe) != VK_SUCCESS) {
            SetReason(reason, "no Vulkan compute device is available");
            return {};
        }
    }
    DevicePick pick;
    bool const found = PickDevice(probe, &pick);
    vkDestroyInstance(probe, nullptr);
    if (!found) {
        SetReason(reason, "no Vulkan compute device is available");
        return {};
    }
    // Load shaders before touching the device: a missing width module fails
    // with no Vulkan state at all (safe on any thread), and optional
    // modules only narrow later pipeline admission.
    std::string const dir = ShaderDir();
    std::vector<uint32_t> widthSpv;
    if (!LoadShader(dir, "width.spv", &widthSpv)) {
        SetReason(reason, "Vulkan default provider cannot load width.spv");
        return {};
    }
    std::vector<uint32_t> scale, set, cut, reparam, minimum, literal, envelope;
    bool const hasScale = LoadShader(dir, "lengthScale.spv", &scale);
    bool const hasSet = LoadShader(dir, "lengthSet.spv", &set);
    bool const hasCut = LoadShader(dir, "lengthCutExtend.spv", &cut);
    bool const hasReparam = LoadShader(dir, "lengthReparam.spv", &reparam);
    bool const hasMinimum = LoadShader(dir, "lengthMinimum.spv", &minimum);
    bool const hasLiteral = LoadShader(dir, "lengthLiteralV1.spv", &literal);
    bool const hasEnvelope = LoadShader(dir, "lengthEnvelopeV1.spv", &envelope);
    std::vector<uint32_t> blendSpv, compareSpv, compactSpv;
    bool const hasBlend = LoadShader(dir, "widthBlend.spv", &blendSpv);
    bool const hasCompare = LoadShader(dir, "nonWidthCompare.spv", &compareSpv);
    bool const hasCompact = LoadShader(dir, "lengthCompaction.spv", &compactSpv);
    std::vector<uint32_t> noiseValidateSpv, noiseGenerateSpv;
    bool const hasNoise =
        LoadShader(dir, "noiseValidate.spv", &noiseValidateSpv) &&
        LoadShader(dir, "noiseGenerate.spv", &noiseGenerateSpv);
    std::vector<uint32_t> deformEvalSpv, deformApplySpv;
    bool const hasDeform =
        LoadShader(dir, "deformEvaluate.spv", &deformEvalSpv) &&
        LoadShader(dir, "deformApply.spv", &deformApplySpv);
    auto self = std::shared_ptr<DefaultVulkanSessionProvider>(
        new DefaultVulkanSessionProvider);
    self->native_ = CreateNative(pick, pick.shaderFloat64, reason);
    if (!self->native_) return {};
    struct Lifetime { std::shared_ptr<DefaultNative> native; };
    auto lifetime = std::make_shared<Lifetime>();
    lifetime->native = self->native_;
    DeviceContext::CreateInfo contextInfo;
    contextInfo.instance = self->native_->instance;
    contextInfo.physicalDevice = self->native_->physical;
    contextInfo.device = self->native_->device;
    contextInfo.computeQueue = self->native_->queue;
    contextInfo.computeQueueFamily = self->native_->family;
    contextInfo.physicalIndex = self->native_->physicalIndex;
    contextInfo.resourceDeviceId = self->native_->physicalIndex;
    contextInfo.nativeLifetime = lifetime;
    contextInfo.gpuLabel = "default";
    contextInfo.resources = {64u * 1024u * 1024u, 4u * 1024u * 1024u};
    contextInfo.timelineSemaphoreEnabled = true;
    contextInfo.shaderFloat64Enabled = pick.shaderFloat64;
    VkResult status = VK_SUCCESS;
    self->context_ = DeviceContext::Create(contextInfo, &status);
    if (!self->context_ || status != VK_SUCCESS) {
        SetReason(reason, "Vulkan default provider cannot create a device context");
        return {};
    }
    // Pipelines need only the device context; build them before the
    // completion waiter exists so a pipeline failure destroys plain Vk
    // objects inline (safe on any thread) instead of needing the reaper.
    auto width = WidthPipeline::Create(self->context_, widthSpv, &status);
    if (!width) {
        SetReason(reason, "Vulkan default provider cannot create the width pipeline");
        return {};
    }
    // Length scale/set/cut/reparam/minimum/literal/envelope chain: install
    // the largest contiguous combination present; missing shaders only
    // narrow admission for graphs needing those Length forms.
    std::shared_ptr<LengthScalePipeline> length;
    if (hasScale && hasSet && hasCut && hasReparam && hasMinimum && hasLiteral &&
        hasEnvelope)
        length = LengthScalePipeline::CreateWithEnvelopeV1(
            self->context_, scale, set, cut, reparam, minimum, literal,
            envelope);
    else if (hasScale && hasSet && hasCut && hasReparam && hasMinimum &&
             hasLiteral)
        length = LengthScalePipeline::CreateWithLiteralV1(
            self->context_, scale, set, cut, reparam, minimum, literal);
    else if (hasScale && hasSet && hasCut && hasReparam && hasMinimum)
        length = LengthScalePipeline::CreateWithMinimum(
            self->context_, scale, set, cut, reparam, minimum);
    else if (hasScale && hasSet && hasCut && hasReparam)
        length = LengthScalePipeline::CreateWithReparam(self->context_, scale,
                                                        set, cut, reparam);
    else if (hasScale && hasSet && hasCut)
        length = LengthScalePipeline::CreateWithCutExtend(self->context_, scale,
                                                          set, cut);
    else if (hasScale && hasSet)
        length = LengthScalePipeline::CreateWithSet(self->context_, scale, set);
    else if (hasScale)
        length = LengthScalePipeline::Create(self->context_, scale);
    std::shared_ptr<WidthBlendPipeline> blend;
    if (hasBlend) blend = WidthBlendPipeline::Create(self->context_, blendSpv);
    std::shared_ptr<NonWidthComparePipeline> compare;
    if (hasCompare)
        compare = NonWidthComparePipeline::Create(self->context_, compareSpv);
    std::shared_ptr<LengthCompactionPipeline> compact;
    if (hasCompact)
        compact = LengthCompactionPipeline::Create(self->context_, compactSpv);
    std::shared_ptr<NoisePipeline> noise;
    if (pick.shaderFloat64 && hasNoise)
        noise = NoisePipeline::Create(self->context_, noiseValidateSpv,
                                      noiseGenerateSpv);
    std::shared_ptr<DeformPipeline> deform;
    if (hasDeform)
        deform = DeformPipeline::Create(self->context_, deformEvalSpv,
                                        deformApplySpv);
    self->runtime_ = std::make_shared<UsdGenExecutionRuntime>(4);
    self->queueOwner_ = std::make_shared<UsdGenExecutionPipeline>(
        *self->runtime_, 64, 64);
    self->completion_ = VulkanCompletionService::Create(
        {self->context_, self->queueOwner_.get(), 64});
    if (!self->completion_) {
        SetReason(reason, "Vulkan default provider cannot create a completion service");
        return {};
    }
    // From here on a running completion waiter exists. CloseAndJoin is
    // forbidden on a pipeline worker (Submit's lane), so failures must shut
    // the partial provider down on a detached reaper instead of destroying
    // `self` inline (which would free the VkDevice under the waiter).
    auto Fail = [&](char const* message)
        -> std::shared_ptr<DefaultVulkanSessionProvider> {
        SetReason(reason, message);
        try {
            std::thread([held = std::move(self)]() {
                held->Shutdown();
            }).detach();
        } catch (...) {
            auto* leak = new std::shared_ptr<DefaultVulkanSessionProvider>(
                std::move(self));
            (void)leak;
        }
        return {};
    };
    self->domain_ = VulkanGenerationAdapterDomain::Create(
        {self->queueOwner_, self->completion_});
    if (!self->domain_)
        return Fail("Vulkan default provider cannot create a generation domain");
    self->preparer_ = UsdGenExecutionTaskGraph::GetOrCreate(
        *self->runtime_, "vulkan", self->native_->physicalIndex);
    std::string executorReason;
    self->executor_ = VulkanPlanExecutor::Create(
        {self->domain_, width, self->preparer_, length, blend, compare,
         compact, noise, deform},
        &executorReason);
    if (!self->executor_) {
        std::string message = executorReason.empty()
            ? "Vulkan default provider cannot create a plan executor"
            : executorReason;
        return Fail(message.c_str());
    }
    UsdGenSessionDeviceIdentity identity;
    identity.backend = UsdGenDeviceBackend::Vulkan;
    identity.deviceIndex = self->context_->physicalIndex();
    identity.capabilityVersion = 1;
    identity.logicalContextIdentity =
        reinterpret_cast<uintptr_t>(self->context_.get());
    identity.contextEpoch = 0;
    std::copy(self->context_->deviceUUID().begin(),
              self->context_->deviceUUID().end(),
              identity.physicalDeviceUuid.begin());
    self->identity_ = identity;
    std::string providerReason;
    self->inner_ = VulkanSessionProvider::Create(
        {self->executor_, self->context_, self->queueOwner_, identity, 256},
        &providerReason);
    if (!self->inner_) {
        std::string message = providerReason.empty()
            ? "Vulkan default provider cannot create a session provider"
            : providerReason;
        return Fail(message.c_str());
    }
    return self;
}

} // namespace

std::shared_ptr<UsdGenSessionDeviceProvider> CreateDefaultVulkanSessionProvider(
    std::string* reason) {
    return DefaultVulkanSessionProvider::Create(reason);
}

bool HasDefaultVulkanDevice() noexcept {
    VkInstance probe = VK_NULL_HANDLE;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "usdGen Vulkan default probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    if (vkCreateInstance(&info, nullptr, &probe) != VK_SUCCESS) return false;
    DevicePick pick;
    bool const found = PickDevice(probe, &pick);
    vkDestroyInstance(probe, nullptr);
    return found;
}

} // namespace usdGen::vulkan
