// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT
#include "deviceGenerationAdapter.h"
#include "externalRetirement.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <utility>

namespace usdGen::vulkan {

VulkanGenerationAdapterDomain::VulkanGenerationAdapterDomain(CreateInfo info) noexcept
    : queueOwner_(std::move(info.queueOwner)),
      completionService_(std::move(info.completionService)) {}

std::shared_ptr<VulkanGenerationAdapterDomain>
VulkanGenerationAdapterDomain::Create(CreateInfo info, std::string* reason)
{
    if (!info.queueOwner || !info.completionService ||
        info.completionService->queueOwner() != info.queueOwner.get()) {
        if (reason) *reason = "Vulkan adapter domain queue owner/service mismatch";
        return {};
    }
    try {
        auto domain = std::shared_ptr<VulkanGenerationAdapterDomain>(
            new VulkanGenerationAdapterDomain(std::move(info)));
        // This intentional cycle makes a premature external last drop safe.
        // Close is the sole explicit release boundary after an external drain.
        domain->selfRetention_ = domain;
        return domain;
    } catch (...) {
        if (reason) *reason = "Vulkan adapter domain allocation failed";
        return {};
    }
}

VulkanGenerationAdapterDomain::OperationLease::~OperationLease()
{
    // Do not decrement on an arbitrary destruction thread: an abandoned
    // native operation has no proof, so the domain must remain fail-closed.
}

VulkanGenerationAdapterDomain::OperationLease&
VulkanGenerationAdapterDomain::OperationLease::operator=(OperationLease&& other) noexcept
{
    if (this != &other) {
        // Replacing an unretired token is fail-closed for the same reason.
        domain_ = std::move(other.domain_);
        owner_ = std::move(other.owner_);
        service_ = std::move(other.service_);
    }
    return *this;
}

bool VulkanGenerationAdapterDomain::OperationLease::Retire() noexcept
{
    if (!domain_ || !owner_ || !owner_->IsExecutingOwner()) return false;
    auto domain = std::move(domain_);
    owner_.reset(); service_.reset();
    domain->RetireConsumer();
    return true;
}

VulkanGenerationAdapterDomain::OperationLease
VulkanGenerationAdapterDomain::AcquireOperation() noexcept
{
    std::shared_ptr<UsdGenExecutionPipeline> owner;
    std::shared_ptr<VulkanCompletionService> service;
    if (!BeginConsumer(&owner, &service)) return {};
    if (!owner->IsExecutingOwner()) {
        owner.reset(); service.reset();
        RetireConsumer();
        return {};
    }
    return OperationLease(shared_from_this(), std::move(owner), std::move(service));
}

std::shared_ptr<UsdGenExecutionPipeline>
VulkanGenerationAdapterDomain::QueueOwnerForIngress() const noexcept
{
    return std::atomic_load_explicit(&queueOwner_, std::memory_order_acquire);
}

bool VulkanGenerationAdapterDomain::BeginConsumer(
    std::shared_ptr<UsdGenExecutionPipeline>* owner,
    std::shared_ptr<VulkanCompletionService>* service) noexcept
{
    if (!owner || !service || closing_.load(std::memory_order_seq_cst)) return false;
    activeConsumers_.fetch_add(1, std::memory_order_seq_cst);
    if (closing_.load(std::memory_order_seq_cst) || closed_.load(std::memory_order_seq_cst)) {
        RetireConsumer();
        return false;
    }
    *owner = std::atomic_load_explicit(&queueOwner_, std::memory_order_acquire);
    *service = std::atomic_load_explicit(&completionService_, std::memory_order_acquire);
    if (!*owner || !*service || (*service)->queueOwner() != owner->get()) {
        owner->reset(); service->reset();
        RetireConsumer();
        return false;
    }
    return true;
}

void VulkanGenerationAdapterDomain::RetireConsumer() noexcept
{
    uint64_t previous = activeConsumers_.load(std::memory_order_seq_cst);
    while (previous && !activeConsumers_.compare_exchange_weak(
        previous, previous - 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {}
    if (previous == 1) ScheduleRetirement();
}

bool VulkanGenerationAdapterDomain::RetireWhenIdle(
    std::function<void()> externalRetirement) noexcept
{
    if (!externalRetirement || !IsClosing() || IsClosed()) return false;
    try {
        auto callback = std::make_shared<std::function<void()>>(std::move(externalRetirement));
        std::shared_ptr<std::function<void()>> empty;
        if (!std::atomic_compare_exchange_strong_explicit(&deferredRetirement_, &empty,
                callback, std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        ScheduleRetirement();
        return true;
    } catch (...) { return false; }
}

void VulkanGenerationAdapterDomain::ScheduleRetirement() noexcept
{
    if (!IsClosing() || activeConsumers_.load(std::memory_order_seq_cst)) return;
    auto callback = std::atomic_exchange_explicit(&deferredRetirement_,
        std::shared_ptr<std::function<void()>>{}, std::memory_order_acq_rel);
    if (!callback) return;
    try {
        if (EnqueueVulkanExternalRetirement([callback] { (*callback)(); })) return;
    } catch (...) {
    }
    // A full/closed queue cannot justify dropping native ownership. Pending
    // consumers and late proofs never prolong process shutdown.
    std::atomic_store_explicit(&deferredRetirement_, std::move(callback),
                               std::memory_order_release);
}

void VulkanGenerationAdapterDomain::StopAdmission() noexcept
{
    closing_.store(true, std::memory_order_seq_cst);
}

bool VulkanGenerationAdapterDomain::Close() noexcept
{
    if (UsdGenExecutionPipeline::IsExecuting()) return false;
    StopAdmission();
    if (closed_.load(std::memory_order_seq_cst)) return true;
    bool expected = false;
    if (!draining_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return closed_.load(std::memory_order_seq_cst);
    // Close is deliberately an external boundary.  It does not invoke service
    // shutdown because service users outside this domain may still be alive.
    auto owner = std::atomic_load_explicit(&queueOwner_, std::memory_order_acquire);
    if (!owner) { draining_.store(false, std::memory_order_release); return false; }
    try { owner->Drain(); } catch (...) { draining_.store(false, std::memory_order_release); return false; }
    if (activeConsumers_.load(std::memory_order_seq_cst) != 0) {
        draining_.store(false, std::memory_order_release); return false;
    }
    std::atomic_store_explicit(&completionService_, std::shared_ptr<VulkanCompletionService>{},
                               std::memory_order_release);
    std::atomic_store_explicit(&queueOwner_, std::shared_ptr<UsdGenExecutionPipeline>{},
                               std::memory_order_release);
    closed_.store(true, std::memory_order_seq_cst);
    draining_.store(false, std::memory_order_release);
    (void)std::atomic_exchange_explicit(&selfRetention_,
        std::shared_ptr<VulkanGenerationAdapterDomain>{}, std::memory_order_acq_rel);
    return true;
}

bool VulkanGenerationAdapterDomain::IsClosing() const noexcept
{ return closing_.load(std::memory_order_acquire); }
bool VulkanGenerationAdapterDomain::IsClosed() const noexcept
{ return closed_.load(std::memory_order_acquire); }

class VulkanGenerationAdapterDomainAccess final {
public:
    static bool Begin(std::shared_ptr<VulkanGenerationAdapterDomain> const& domain,
                      std::shared_ptr<UsdGenExecutionPipeline>* owner,
                      std::shared_ptr<VulkanCompletionService>* service) noexcept {
        return domain && domain->BeginConsumer(owner, service);
    }
    static void Retire(std::shared_ptr<VulkanGenerationAdapterDomain> const& domain) noexcept {
        if (domain) domain->RetireConsumer();
    }
};

namespace {

UsdGenDeviceStream QueueToken(DeviceContext const& context) noexcept
{
    return reinterpret_cast<UsdGenDeviceStream>(context.computeQueue());
}

enum class LeasePhase : uint8_t { Reserved, CompleteQueued, Armed, Retired, Quarantined };

// All native-operation state is kept in this preallocated control block.  A
// lost transport intentionally installs a self-reference in `quarantine`;
// therefore neither a failure thread nor an off-owner lease destructor can
// become the last owner of a Watch/source/service graph.
class VulkanLeaseControl final : public std::enable_shared_from_this<VulkanLeaseControl> {
public:
    VulkanLeaseControl(std::shared_ptr<const VulkanSourceGeneration> source,
                       std::shared_ptr<VulkanGenerationAdapterDomain> domain,
                       std::shared_ptr<VulkanCompletionService> service,
                       std::shared_ptr<UsdGenExecutionPipeline> owner) noexcept
        : source(std::move(source)), domain(std::move(domain)), service(std::move(service)), owner(std::move(owner)) {}

    void Quarantine() noexcept {
        phase.store(LeasePhase::Quarantined, std::memory_order_release);
        try {
            std::shared_ptr<VulkanLeaseControl> empty;
            (void)std::atomic_compare_exchange_strong_explicit(
                &quarantine, &empty, shared_from_this(),
                std::memory_order_acq_rel, std::memory_order_acquire);
        } catch (...) {}
    }

    void ArmOnOwner() noexcept {
        if (!owner || !service || !owner->IsExecutingOwner() ||
            phase.load(std::memory_order_acquire) != LeasePhase::CompleteQueued ||
            !watch || !service->Arm(*watch, source->valueVersion())) {
            Quarantine();
            return;
        }
        phase.store(LeasePhase::Armed, std::memory_order_release);
    }

    VulkanCompletionService::DeliveryResult Deliver(VkResult proof) noexcept {
        if (!owner || !owner->IsExecutingOwner() || proof != VK_SUCCESS ||
            phase.load(std::memory_order_acquire) != LeasePhase::Armed || !watch) {
            Quarantine();
            return VulkanCompletionService::DeliveryResult::LostProof;
        }
        if (!watch->Retire()) {
            Quarantine();
            return VulkanCompletionService::DeliveryResult::LostProof;
        }
        // Retire clears the service's slot ownership on this same owner.
        watch.reset();
        phase.store(LeasePhase::Retired, std::memory_order_release);
        VulkanGenerationAdapterDomainAccess::Retire(domain);
        return VulkanCompletionService::DeliveryResult::Posted;
    }

    std::shared_ptr<const VulkanSourceGeneration> source;
    std::shared_ptr<VulkanGenerationAdapterDomain> domain;
    std::shared_ptr<VulkanCompletionService> service;
    std::shared_ptr<UsdGenExecutionPipeline> owner;
    std::unique_ptr<VulkanCompletionService::Watch> watch;
    UsdGenExecutionPipeline::CommandTicket armTicket;
    // A consumer is allocated before service reservation. It remains inert
    // until Mark succeeds, so every pre-Mark rollback is allocation-safe.
    std::atomic<LeasePhase> phase{LeasePhase::Retired};
    // Allocated as part of the control object, avoiding allocation on a loss
    // path. Its cycle is intentional quarantine ownership.
    std::shared_ptr<VulkanLeaseControl> quarantine;
};

class VulkanGenerationConsumer final : public UsdGenDeviceConsumer {
public:
    VulkanGenerationConsumer(std::shared_ptr<VulkanLeaseControl> control,
                             UsdGenDeviceStream stream) noexcept
        : control_(std::move(control)), stream_(stream) {}

    ~VulkanGenerationConsumer() override { Complete(); }
    UsdGenDeviceStream Stream() const noexcept override { return stream_; }

    UsdGenDeviceStatus WaitUntilReady() const noexcept override {
        auto const& control = control_;
        if (!control || !control->owner || !control->owner->IsExecutingOwner() ||
            control->phase.load(std::memory_order_acquire) != LeasePhase::Reserved)
            return UsdGenDeviceStatus::InvalidLease;
        // The source boundary admits only TakeReady snapshots: its upload
        // fence was proved before publication. Completion still fences the
        // later consumer queue work through ArmOnOwner.
        return control->source ? UsdGenDeviceStatus::Ok
                               : UsdGenDeviceStatus::ProducerNotReady;
    }

    void Complete() noexcept override {
        auto control = std::move(control_);
        if (!control) return;
        LeasePhase expected = LeasePhase::Reserved;
        if (!control->phase.compare_exchange_strong(expected, LeasePhase::CompleteQueued,
                                                     std::memory_order_acq_rel)) {
            if (expected != LeasePhase::Retired) control->Quarantine();
            return;
        }
        // The command admission was reserved before exposing this consumer.
        // It is moved exactly once, so arbitrary completion threads neither
        // submit native work nor touch/destroy the Watch.
        auto ticket = std::move(control->armTicket);
        bool posted = false;
        try {
            posted = control->owner && control->owner->PostCommand(std::move(ticket),
                [control] { control->ArmOnOwner(); },
                [control] { control->Quarantine(); });
        } catch (...) {}
        if (!posted) control->Quarantine();
    }

private:
    std::shared_ptr<VulkanLeaseControl> control_;
    UsdGenDeviceStream stream_ = 0;
};

class VulkanGenerationOwner final : public UsdGenDeviceOwner,
                                    public std::enable_shared_from_this<VulkanGenerationOwner> {
public:
    VulkanGenerationOwner(std::shared_ptr<const VulkanSourceGeneration> source,
                          std::shared_ptr<VulkanGenerationAdapterDomain> domain,
                          UsdGenDeviceStream stream) noexcept
        : source_(std::move(source)), domain_(std::move(domain)), stream_(stream) {}

    bool ProducerReady() const noexcept override {
        return source_ && domain_ && !domain_->IsClosing() && !domain_->IsClosed() &&
            source_->context() && QueueToken(*source_->context()) == stream_;
    }
    size_t ExclusiveRetainedBytes() const noexcept override {
        return source_ ? source_->ExclusiveRetainedBytes() : 0;
    }
    size_t InclusiveRetainedBytes() const noexcept override {
        return source_ ? source_->InclusiveRetainedBytes() : 0;
    }
    std::shared_ptr<const VulkanSourceGeneration> source() const noexcept { return source_; }
    std::shared_ptr<VulkanGenerationAdapterDomain> domain() const noexcept { return domain_; }

    std::unique_ptr<UsdGenDeviceConsumer> AcquireConsumer(
        UsdGenDeviceStream stream) const noexcept override {
        // Acquire itself is the queue-owner admission boundary.  This makes
        // both reservations before any caller can enqueue consumer commands.
        if (stream != stream_ || !ProducerReady()) return {};
        bool admitted = false;
        try {
            std::shared_ptr<UsdGenExecutionPipeline> owner;
            std::shared_ptr<VulkanCompletionService> service;
            if (!VulkanGenerationAdapterDomainAccess::Begin(domain_, &owner, &service)) return {};
            admitted = true;
            if (!owner->IsExecutingOwner()) {
                owner.reset(); service.reset();
                VulkanGenerationAdapterDomainAccess::Retire(domain_); return {};
            }
            auto control = std::make_shared<VulkanLeaseControl>(source_, domain_, service, owner);
            control->armTicket = owner->ReserveCommandTicket();
            if (!control->armTicket) {
                control.reset(); owner.reset(); service.reset();
                VulkanGenerationAdapterDomainAccess::Retire(domain_); return {};
            }
            // This is intentionally before Reserve: Reserve retains a
            // control/lifetime callback cycle, so no allocation can be left
            // to fail after it has accepted the watch.
            auto consumer = std::unique_ptr<UsdGenDeviceConsumer>(
                new VulkanGenerationConsumer(control, stream_));
            std::shared_ptr<const void> lifetime(control,
                static_cast<void const*>(control.get()));
            control->watch = service->Reserve(std::move(lifetime),
                [control](VkResult proof) { return control->Deliver(proof); },
                [control](VkResult) { control->Quarantine(); });
            if (!control->watch) {
                control->armTicket = {};
                consumer.reset(); control.reset(); owner.reset(); service.reset();
                VulkanGenerationAdapterDomainAccess::Retire(domain_);
                return {};
            }
            // The service contract requires this immediately before the first
            // native submission retained by the watch.  Mark before handing
            // the consumer out; consumer work can therefore never outrun it.
            if (!control->watch->MarkPhaseSubmitted()) {
                control->armTicket = {};
                if (service->CancelBeforeSubmit(*control->watch)) {
                    control->watch.reset();
                    control->phase.store(LeasePhase::Retired, std::memory_order_release);
                    consumer.reset(); control.reset(); owner.reset(); service.reset();
                    VulkanGenerationAdapterDomainAccess::Retire(domain_);
                } else control->Quarantine();
                return {};
            }
            control->phase.store(LeasePhase::Reserved, std::memory_order_release);
            return consumer;
        } catch (...) {
            if (admitted) VulkanGenerationAdapterDomainAccess::Retire(domain_);
            return {};
        }
    }

private:
    std::shared_ptr<const VulkanSourceGeneration> source_;
    std::shared_ptr<VulkanGenerationAdapterDomain> domain_;
    UsdGenDeviceStream stream_ = 0;
};

void Fail(std::string* reason, char const* text) { if (reason) *reason = text; }

UsdGenDeviceChannelMetadata PublicChannel(UsdGenDeviceChannelMetadata metadata)
{
    // Native pipeline storage names are private. Publish the same neutral
    // channel names as CUDA so consumers can select either backend unchanged.
    using Semantic = UsdGenDeviceChannelSemantic;
    switch (metadata.semantic) {
        case Semantic::Widths: metadata.name = "widths"; break;
        case Semantic::StableIds: metadata.name = "curveId"; break;
        case Semantic::RootPrim: metadata.name = "skinprim"; break;
        case Semantic::RootUV: metadata.name = "skinprimuv"; break;
        default: break;
    }
    return metadata;
}

bool PublishChannel(UsdGenDeviceChannelMetadata const& metadata)
{
    using Semantic = UsdGenDeviceChannelSemantic;
    // CUDA omits absent root bindings, including after compaction to zero
    // curves. Keep the native empty planes private to the pipeline packet.
    return metadata.elementCount ||
        (metadata.semantic != Semantic::RootPrim && metadata.semantic != Semantic::RootUV);
}

} // namespace

std::shared_ptr<const UsdGenDeviceGeneration>
VulkanDeviceGenerationAdapter::Create(CreateInfo info, std::string* reason)
{
    auto const source = info.source;
    auto const context = source ? source->context() : std::shared_ptr<DeviceContext>{};
    if (!source || !context || !info.domain) {
        Fail(reason, "Vulkan adapter requires source, context, and lifetime domain"); return {};
    }
    auto domain = info.domain;
    std::shared_ptr<UsdGenExecutionPipeline> owner;
    std::shared_ptr<VulkanCompletionService> service;
    // Publication is external-safe, unlike consumer acquisition.  This short
    // counted guard serializes it with Close and always drops snapshots before
    // returning its domain credit.
    if (!domain->BeginConsumer(&owner, &service)) {
        Fail(reason, "Vulkan adapter domain is closing"); return {};
    }
    auto releaseAccess = [&](void*) noexcept {
        owner.reset(); service.reset();
        domain->RetireConsumer();
    };
    // The custom deleter only returns admission; it does not delete domain.
    // No allocation is needed, and even a diagnostic string allocation that
    // throws will release snapshots before returning the activity credit.
    std::unique_ptr<void,decltype(releaseAccess)> accessGuard(domain.get(),releaseAccess);
    UsdGenDeviceStream const actual = QueueToken(*context);
    if (!actual || info.queueStream != actual || !service || !owner ||
        service->context() != context || service->queueOwner() != owner.get() ||
        domain->IsClosing() || domain->IsClosed()) {
        Fail(reason, "Vulkan adapter queue/service/context mismatch"); return {};
    }
    if (context->physicalIndex() < 0) {
        Fail(reason, "Vulkan adapter requires a concrete device identity"); return {};
    }
    std::vector<UsdGenDeviceChannelMetadata> channels;
    try {
        channels.reserve(source->planes().size());
        for (auto const& plane : source->planes())
            if (PublishChannel(plane.metadata)) channels.push_back(PublicChannel(plane.metadata));
        auto generationOwner = std::make_shared<VulkanGenerationOwner>(source,
            domain, actual);
        UsdGenDeviceGeneration::CreateInfo publication;
        publication.identity = {UsdGenDeviceBackend::Vulkan, context->physicalIndex(), info.generation};
        publication.geometry = source->geometry();
        publication.geometry.valueVersion = source->valueVersion();
        publication.tool = std::move(info.tool);
        publication.channels = std::move(channels);
        publication.owner = std::move(generationOwner);
        auto result = UsdGenDeviceGeneration::Create(std::move(publication), reason);
        return result;
    } catch (std::bad_alloc const&) {
        Fail(reason, "Vulkan adapter allocation failed"); return {};
    } catch (...) {
        Fail(reason, "Vulkan adapter publication failed"); return {};
    }
}

VulkanGenerationAccess GetVulkanGenerationAccess(
    std::shared_ptr<const UsdGenDeviceGeneration> const& generation) noexcept
{
    if (!generation || !generation->Owner()) return {};
    auto owner = std::dynamic_pointer_cast<VulkanGenerationOwner const>(generation->Owner());
    if (!owner || !owner->ProducerReady() || !owner->source() || !owner->domain()) return {};
    auto context = owner->source()->context();
    auto queue = owner->domain()->QueueOwnerForIngress();
    if (!context || !queue || generation->Identity().backend != UsdGenDeviceBackend::Vulkan ||
        generation->Identity().deviceIndex != context->physicalIndex()) return {};
    return {generation, std::move(context), std::move(queue), owner->domain()};
}

VulkanGenerationLease AcquireVulkanGeneration(
    std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
    UsdGenDeviceStream queueStream) noexcept
{
    if (!generation || generation->Identity().backend != UsdGenDeviceBackend::Vulkan) return {};
    auto owner = std::dynamic_pointer_cast<VulkanGenerationOwner const>(generation->Owner());
    if (!owner || queueStream != QueueToken(*owner->source()->context())) return {};
    auto lease = generation->AcquireLease(queueStream);
    if (!lease) return {};
    try {
        auto planes = owner->source()->planes();
        planes.erase(std::remove_if(planes.begin(), planes.end(), [](auto const& plane) {
            return !PublishChannel(plane.metadata);
        }), planes.end());
        for (auto& plane : planes) plane.metadata = PublicChannel(std::move(plane.metadata));
        return VulkanGenerationLease(std::move(lease), std::move(planes));
    }
    catch (...) { lease.Complete(); return {}; }
}

} // namespace usdGen::vulkan
