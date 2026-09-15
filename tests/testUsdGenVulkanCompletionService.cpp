#include "vulkanNativeFixture.h"
#include "usdGen/vulkan/completionService.h"
#include "usdGen/vulkan/sourceGeneration.h"

#include <atomic>
#include <string>

using namespace usdGen;
using namespace usdGen::vulkan;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Vulkan completion service check failed: %s (%d)\n", #x, __LINE__); return 1; } } while (false)

static DeviceContext::CreateInfo ContextInfo(std::shared_ptr<NativeOwner> const& native) {
    DeviceContext::CreateInfo info;
    info.instance = native->instance; info.physicalDevice = native->physical;
    info.device = native->device; info.computeQueue = native->queue;
    info.computeQueueFamily = native->family; info.physicalIndex = native->physicalIndex;
    info.resourceDeviceId = 7016; info.nativeLifetime = native;
    info.resources = {size_t{8} << 20, 0};
    return info;
}

int main(int argc, char** argv) {
    bool const idleClose = argc == 2 && std::string(argv[1]) == "--idle-close";
    bool const unavailableOwner = argc == 2 && std::string(argv[1]) == "--owner-unavailable";
    bool const closePending = unavailableOwner || (argc == 2 && std::string(argv[1]) == "--close-pending");
    CHECK(argc == 1 || closePending || idleClose);
    bool unavailable = false;
    auto probe = CreateNative(&unavailable);
    if (unavailable) return 77;
    CHECK(probe);
    VkPhysicalDeviceTimelineSemaphoreFeatures supported{};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    VkPhysicalDeviceFeatures2 features{}; features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &supported;
    vkGetPhysicalDeviceFeatures2(probe->physical, &features);
    if (!supported.timelineSemaphore) return 77;
    probe.reset();
    VkPhysicalDeviceTimelineSemaphoreFeatures enabled{};
    enabled.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    enabled.timelineSemaphore = VK_TRUE;
    auto native = CreateNative(&unavailable, {}, &enabled); CHECK(native);
    UsdGenExecutionRuntime runtime{2};
    UsdGenExecutionPipeline owner(runtime), foreign(runtime);
    auto ci = ContextInfo(native);
    auto unclaimed = DeviceContext::Create(ci); CHECK(unclaimed);
    VkResult status = VK_SUCCESS;
    CHECK(!VulkanCompletionService::Create({unclaimed, &owner, 2}, &status));
    ci.timelineSemaphoreEnabled = true;
    auto context = DeviceContext::Create(ci); CHECK(context);
    CHECK(!VulkanCompletionService::Create({context, &owner, 0}, &status));
    CHECK(!VulkanCompletionService::Create({context, nullptr, 2}, &status));
    if (idleClose) {
        // Exercise Close racing the waiter's first target capture. No work
        // can supply a second wake if shutdown's first wake is missed.
        for (unsigned i = 0; i != 256; ++i) {
            auto idle = VulkanCompletionService::Create({context, &owner, 1}); CHECK(idle);
            idle->CloseAndJoin();
        }
        CHECK(owner.OutstandingCommands() == 0);
        return 0;
    }
    auto service = VulkanCompletionService::Create({context, &owner, 1}, &status);
    CHECK(service && status == VK_SUCCESS);
    using Result = VulkanCompletionService::DeliveryResult;
    auto noDelivery = [](VkResult) { return Result::Stale; };
    CHECK(!service->Reserve(std::make_shared<int>(1), noDelivery));

    bool admissionOk = false;
    foreign.InvokeOwner([&] { admissionOk = !service->Reserve(std::make_shared<int>(1), noDelivery); });
    CHECK(admissionOk);
    std::weak_ptr<int> cancelledLifetime;
    owner.InvokeOwner([&] {
        auto life = std::make_shared<int>(1); cancelledLifetime = life;
        auto watch = service->Reserve(std::move(life), noDelivery);
        admissionOk = watch && !cancelledLifetime.expired();
        admissionOk = admissionOk && !service->Reserve(std::make_shared<int>(2), noDelivery);
        admissionOk = admissionOk && service->CancelBeforeSubmit(*watch);
    });
    CHECK(admissionOk && cancelledLifetime.expired());

    // Real Source work advances only through the native timeline waiter. No
    // manual NotifyCompletion, fence waits, polling loop, or queue idle here.
    VulkanSourceGenerationCreateInfo source; source.context = context;
    source.source.totalCurves = 1; source.source.totalCvs = 2;
    source.source.topologyVersion = 1; source.source.valueVersion = 1;
    source.source.px = {0, 1}; source.source.py = {0, 0}; source.source.pz = {0, 0};
    source.source.curveId = {7}; source.source.cvOffsets = {0, 2};
    source.source.width = {1, 2};
    auto upload = VulkanSourceUpload::Create(source, &status); CHECK(upload);
    auto charged = context->resources()->Snapshot().usedBytes;
    std::unique_ptr<VulkanCompletionService::Watch> watch;
    if (closePending) {
        unsigned failures = 0;
        unsigned fallbackFailures = 0;
        bool ownerFailure = false;
        bool fallbackFailure = false;
        std::weak_ptr<VulkanSourceUpload> weakUpload = upload;
        owner.InvokeOwner([&] {
            watch = service->Reserve(upload, [&](VkResult proof) {
                ++failures; ownerFailure = owner.IsExecutingOwner() && proof != VK_SUCCESS;
                upload->Quarantine();
                return Result::Posted;
            }, [&](VkResult proof) {
                ++fallbackFailures;
                fallbackFailure = !owner.IsExecutingOwner() && proof != VK_SUCCESS;
            });
            admissionOk = watch && watch->MarkPhaseSubmitted() &&
                upload->Submit() == SourceGenerationStatus::Submitted;
            // Intentionally omit Arm: external close must settle this accepted
            // uncertain phase without waiting for a notification never issued.
        });
        CHECK(admissionOk);
        if (unavailableOwner) owner.Shutdown();
        service->CloseAndJoin(); owner.Drain();
        if (unavailableOwner) {
            CHECK(fallbackFailure && fallbackFailures == 1 && failures == 0);
            watch.reset();
        } else {
            CHECK(ownerFailure && failures == 1 && fallbackFailures == 0);
            owner.InvokeOwner([&] { watch.reset(); });
        }
        upload.reset(); service.reset();
        CHECK(!weakUpload.expired());
        CHECK(context->resources()->Snapshot().usedBytes == charged);
        CHECK(owner.OutstandingCommands() == 0);
        return 0;
    }
    std::shared_ptr<const VulkanSourceGeneration> generation;
    std::atomic<unsigned> notifications{0};
    bool completionOk = false;
    uint64_t completionEpoch = 0;
    owner.Await([&](auto finish) {
        if (!owner.PostCommand([&, finish] {
            auto ticket = std::make_shared<UsdGenExecutionPipeline::CommandTicket>(owner.ReserveCommandTicket());
            if (!*ticket) { finish(); return; }
            watch = service->Reserve(upload, [&, ticket, finish](VkResult proof) {
                ++notifications;
                bool const posted = owner.PostCommand(std::move(*ticket), [&, proof, finish] {
                    completionOk = owner.IsExecutingOwner() && proof == VK_SUCCESS &&
                        upload->Poll() == SourceGenerationStatus::Ready;
                    if (completionOk) generation = upload->TakeReady();
                    completionOk = watch->Retire() && completionOk;
                    finish();
                }, finish);
                return posted ? Result::Posted : Result::LostProof;
            });
            if (!watch || !watch->MarkPhaseSubmitted() ||
                upload->Submit() != SourceGenerationStatus::Submitted ||
                !service->Arm(*watch, 41, &completionEpoch)) {
                // A failure remains visible; uncertain native work stays
                // retained by the watch/service and upload quarantine paths.
                finish();
            }
        })) finish();
    });
    service->CloseAndJoin();
    owner.Drain();
    CHECK(completionOk && generation && notifications == 1 && completionEpoch > 0);
    CHECK(generation->pointCount() == 2 && generation->valueVersion() == 1);
    CHECK(context->resources()->Snapshot().usedBytes < charged);
    owner.InvokeOwner([&] { admissionOk = !service->Reserve(std::make_shared<int>(1), noDelivery); });
    CHECK(admissionOk);
    owner.InvokeOwner([&] { watch.reset(); });
    generation.reset(); upload.reset(); service.reset();
    CHECK(context->resources()->Snapshot().usedBytes == 0);
    CHECK(owner.OutstandingCommands() == 0);
    return 0;
}
