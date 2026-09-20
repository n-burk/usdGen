#include "sourceWidthJob.h"
#include "lengthEnvelope.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>
#include <cstring>
namespace usdGen::vulkan {
struct VulkanSourceWidthJob::Control {
    UsdGenExecutionPipeline::CommandMailbox mailbox;
    UsdGenExecutionPipeline::CommandTicket terminalTicket;
    UsdGenExecutionPipeline::CommandTicket prepareTicket;
};
struct VulkanSourceWidthJob::PrepareRelay : std::enable_shared_from_this<PrepareRelay> {
    UsdGenExecutionPipeline* owner = nullptr;
    UsdGenExecutionPipeline::CommandTicket ticket;
    std::shared_ptr<VulkanSourceWidthJob> jobLifetime;
    std::shared_ptr<Relay> terminal;
    std::shared_ptr<const VulkanPreparedSource> prepared;
    std::atomic<bool> taskError{false}, failed{false};
    std::unique_ptr<std::shared_ptr<PrepareRelay>> quarantine;
    void Retain() noexcept { if (quarantine) { *quarantine=shared_from_this(); (void)quarantine.release(); } }
    void Fail() noexcept;
};
struct VulkanSourceWidthJob::Relay : std::enable_shared_from_this<Relay> {
    std::atomic<bool> settled{false}, failurePosted{false};
    Completion completion;
    std::shared_ptr<const void> requestLifetime;
    std::shared_ptr<VulkanSourceWidthJob> jobLifetime;
    std::unique_ptr<std::shared_ptr<Relay>> quarantine;
    void Settle(std::shared_ptr<const VulkanSourceGeneration> result, State state,
                VkResult status, bool retain) noexcept {
        if (settled.exchange(true, std::memory_order_acq_rel)) return;
        auto self=shared_from_this();
        std::atomic_exchange_explicit(&jobLifetime->control_,
            std::shared_ptr<Control>{}, std::memory_order_acq_rel);
        if (retain) {
            *quarantine=self;
            (void)quarantine.release(); // Preallocated retention: no native reads.
        }
        jobLifetime->state_.store(state, std::memory_order_release);
        if (retain) {
            // Preserve callback captures and request ownership as well as the
            // native job: none may last-drop on this exceptional caller thread.
            try { completion(std::move(result),state,status); } catch (...) {}
        } else {
            auto callback=std::move(completion);
            auto lifetime=std::move(requestLifetime);
            try { callback(std::move(result),state,status); } catch (...) {}
            jobLifetime.reset(); // Native cleanup remains owner-confined.
        }
    }
    void Fallback() noexcept { Settle({},State::LostProof,VK_ERROR_DEVICE_LOST,true); }
};
void VulkanSourceWidthJob::PrepareRelay::Fail() noexcept {
    if (failed.exchange(true,std::memory_order_acq_rel)) return;
    Retain();
    if (terminal) terminal->Fallback();
}
VulkanSourceWidthJob::VulkanSourceWidthJob(CreateInfo info):info_(std::move(info)){}
std::shared_ptr<VulkanSourceWidthJob> VulkanSourceWidthJob::Create(CreateInfo info,std::string* why) {
    if (!info.source.context || !info.widthPipeline || !info.requestLifetime ||
        info.widthPipeline->context() != info.source.context ||
        (info.lengthPipeline && info.lengthPipeline->context() != info.source.context) ||
        (info.lengthCompactionPipeline && info.lengthCompactionPipeline->context() != info.source.context) ||
        (info.nonWidthComparePipeline && info.nonWidthComparePipeline->context() != info.source.context) ||
        info.valueVersion<=info.source.source.valueVersion || !std::isfinite(info.width) ||
        info.width<0 || info.replace>1 ||
        (info.stages.empty() && info.lengthPipeline && (!std::isfinite(info.lengthFactor) || info.lengthFactor < 0 ||
            info.lengthValueVersion <= info.source.source.valueVersion ||
            info.valueVersion <= info.lengthValueVersion))) {
        if (why) *why="invalid source-width job controls or revision";
        return {};
    }
    try {
        if (info.stages.empty()) {
            if (info.lengthPipeline) {
                VulkanSourceWidthStage length; length.kind = VulkanSourceWidthStage::Kind::LengthScale;
                length.factor = info.lengthFactor; info.stages.push_back(length);
                info.stageValueVersions.push_back(info.lengthValueVersion);
            }
            VulkanSourceWidthStage width; width.input = uint32_t(info.stages.size());
            width.width.width = info.width; width.width.replace = info.replace != 0;
            info.stages.push_back(width);
            info.stageValueVersions.push_back(info.valueVersion);
        }
        if (info.stages.size() > 64 || info.stageValueVersions.size() != info.stages.size() ||
            info.stageValueVersions.back() != info.valueVersion) return {};
        auto previous = info.source.source.valueVersion;
        for (size_t i = 0; i < info.stages.size(); ++i) {
            auto const& stage = info.stages[i];
            if (stage.requiresNonWidthProof && !info.nonWidthComparePipeline) return {};
            if (stage.input > i || info.stageValueVersions[i] <= previous) return {};
            previous = info.stageValueVersions[i];
            if (stage.disabled) {
                // A muted stage aliases its input without dispatch, so no
                // control or capability admission applies. Only Width and
                // Length stages mute; blends and unknown kinds never do.
                // Structural ordering above still applies.
                if (stage.kind != VulkanSourceWidthStage::Kind::Width &&
                    stage.kind != VulkanSourceWidthStage::Kind::LengthScale &&
                    stage.kind != VulkanSourceWidthStage::Kind::LengthCull &&
                    stage.kind != VulkanSourceWidthStage::Kind::Noise &&
                    stage.kind != VulkanSourceWidthStage::Kind::Deform) return {};
                continue;
            }
            // These values may originate from a typed plan, but Create is
            // also a direct native admission boundary. Never let a cast or
            // future enum extension select an unintended pipeline path.
            bool const invalidLengthMethod =
                (stage.lengthMethod != VulkanSourceWidthStage::LengthMethod::Scale &&
                 stage.lengthMethod != VulkanSourceWidthStage::LengthMethod::CutExtend);
            bool const invalidLengthMode =
                (stage.lengthMode != VulkanSourceWidthStage::LengthMode::Scale &&
                 stage.lengthMode != VulkanSourceWidthStage::LengthMode::Set);
            bool const invalidLengthRebuild =
                stage.lengthRebuild != VulkanSourceWidthStage::LengthRebuild::KeepParam &&
                stage.lengthRebuild != VulkanSourceWidthStage::LengthRebuild::Reparam;
            bool const invalidMinimum = !std::isfinite(stage.minRemainingLength) || stage.minRemainingLength < 0;
            bool const invalidRandom = !std::isfinite(stage.randomLo) || !std::isfinite(stage.randomHi) ||
                stage.randomLo < 0 || stage.randomHi < 0;
            bool const invalidEnvelope = !std::isfinite(stage.lengthBlend) || stage.lengthBlend < 0 ||
                stage.lengthBlend > 1 || !std::isfinite(stage.lengthMaskAmount) ||
                stage.lengthMaskAmount < 0 || stage.lengthMaskAmount > 1;
            if ((stage.kind == VulkanSourceWidthStage::Kind::LengthScale ||
                 stage.kind == VulkanSourceWidthStage::Kind::LengthCull) &&
                (invalidLengthMethod || invalidLengthMode || invalidLengthRebuild || invalidMinimum || invalidRandom ||
                 invalidEnvelope)) return {};
            if (stage.kind == VulkanSourceWidthStage::Kind::Width) {
                if (!std::isfinite(stage.width.width) || stage.width.width < 0) return {};
            } else if (stage.kind == VulkanSourceWidthStage::Kind::LengthScale) {
                if (!info.lengthPipeline) return {};
                bool const literalV1 = stage.randomLo != 1.0f || stage.randomHi != 1.0f;
                bool const nonneutralEnvelope = stage.lengthBlend != 1.0f || stage.lengthMaskAmount != 1.0f;
                bool const hasEnvelope = info.lengthPipeline->HasEnvelopeV1();
                bool const missingEnvelope = nonneutralEnvelope && !hasEnvelope;
                bool const missingLiteral = !nonneutralEnvelope && literalV1 && !info.lengthPipeline->HasLiteralV1();
                bool const missingMinimum = !nonneutralEnvelope && !literalV1 &&
                    stage.minRemainingLength > 0 && !info.lengthPipeline->HasMinimum();
                bool const missingReparam = !nonneutralEnvelope && !literalV1 &&
                    stage.minRemainingLength == 0 && stage.lengthMethod == VulkanSourceWidthStage::LengthMethod::CutExtend &&
                    stage.lengthRebuild == VulkanSourceWidthStage::LengthRebuild::Reparam && !info.lengthPipeline->HasReparam();
                bool const missingCut = !nonneutralEnvelope && !literalV1 && stage.minRemainingLength == 0 &&
                    stage.lengthMethod == VulkanSourceWidthStage::LengthMethod::CutExtend &&
                    stage.lengthRebuild != VulkanSourceWidthStage::LengthRebuild::Reparam && !info.lengthPipeline->HasCutExtend();
                bool const missingSet = !nonneutralEnvelope && !literalV1 && stage.minRemainingLength == 0 &&
                    stage.lengthMethod != VulkanSourceWidthStage::LengthMethod::CutExtend &&
                    stage.lengthMode == VulkanSourceWidthStage::LengthMode::Set && !info.lengthPipeline->HasSet();
                if (!info.lengthPipeline || !std::isfinite(stage.factor) || stage.factor < 0 ||
                    (stage.lengthCullOnly && !nonneutralEnvelope) || missingEnvelope || missingLiteral ||
                    missingMinimum || missingReparam || missingCut || missingSet) return {};
            } else if (stage.kind == VulkanSourceWidthStage::Kind::LengthCull) {
                bool const nonneutralEnvelope = stage.lengthBlend != 1.0f || stage.lengthMaskAmount != 1.0f;
                bool const hasEnvelope = info.lengthPipeline && info.lengthPipeline->HasEnvelopeV1();
                bool malformedEnvelopeLineage = false;
                if (nonneutralEnvelope) {
                    if (stage.input == 0) malformedEnvelopeLineage = true;
                    else {
                        auto const& producer = info.stages[stage.input - 1];
                        bool const producerEnvelope = producer.lengthBlend != 1.0f ||
                            producer.lengthMaskAmount != 1.0f;
                        malformedEnvelopeLineage = producer.kind != VulkanSourceWidthStage::Kind::LengthScale ||
                            !producerEnvelope || producer.lengthBlend != stage.lengthBlend ||
                            producer.lengthMaskAmount != stage.lengthMaskAmount;
                    }
                }
                if (!info.lengthCompactionPipeline || !std::isfinite(stage.factor) || stage.factor < 0 ||
                    !std::isfinite(stage.cullThreshold) ||
                    stage.cullThreshold < 0 || !std::isfinite(stage.randomLo) ||
                    !std::isfinite(stage.randomHi) || stage.randomLo < 0 || stage.randomHi < 0 ||
                    stage.lengthCullOnly || malformedEnvelopeLineage ||
                    (nonneutralEnvelope && !hasEnvelope)) return {};
            } else if (stage.kind == VulkanSourceWidthStage::Kind::WidthBlend) {
                if (!info.widthBlendPipeline || info.widthBlendPipeline->context() != info.source.context ||
                    stage.rightInput > i || !std::isfinite(stage.blend) || stage.blend < 0 || stage.blend > 1) return {};
            } else if (stage.kind == VulkanSourceWidthStage::Kind::Noise) {
                if (!info.noisePipeline || info.noisePipeline->context() != info.source.context) return {};
                if (!std::isfinite(stage.noise.magnitude) || stage.noise.magnitude < 0 ||
                    !std::isfinite(stage.noise.frequency) || stage.noise.frequency <= 0 ||
                    !std::isfinite(stage.noise.correlation) || stage.noise.correlation < 0 || stage.noise.correlation > 1 ||
                    stage.noise.octaves < 1 || stage.noise.octaves > 6 ||
                    !std::isfinite(stage.noise.lacunarity) || stage.noise.lacunarity <= 1 ||
                    !std::isfinite(stage.noise.gain) || stage.noise.gain < 0 || stage.noise.gain > 1 ||
                    !std::isfinite(stage.noise.preserveLength) || stage.noise.preserveLength < 0 || stage.noise.preserveLength > 1 ||
                    !std::isfinite(stage.noise.mask) || stage.noise.mask < 0 || stage.noise.mask > 1) return {};
            } else if (stage.kind == VulkanSourceWidthStage::Kind::Deform) {
                if (!info.deformPipeline || info.deformPipeline->context() != info.source.context) return {};
                if (stage.deform.sampleCount < 4 || stage.deform.sampleCount > 100) return {};
                if (stage.deform.restSamples.size() != size_t(stage.deform.sampleCount) * 3 ||
                    stage.deform.posedSamples.size() != size_t(stage.deform.sampleCount) * 3) return {};
                if (!std::isfinite(stage.deform.mask) || stage.deform.mask < 0 || stage.deform.mask > 1 ||
                    !std::isfinite(stage.deform.groomEnvelope) || stage.deform.groomEnvelope < 0 || stage.deform.groomEnvelope > 1) return {};
            } else return {};
        }
        auto job = std::shared_ptr<VulkanSourceWidthJob>(new VulkanSourceWidthJob(std::move(info)));
        job->values_.reserve(job->info_.stages.size() + 1);
        return job;
    }
    catch (std::bad_alloc const&) { if(why)*why="source-width job allocation failed";return{}; }
}
bool VulkanSourceWidthJob::Start(UsdGenExecutionPipeline& owner,
    UsdGenExecutionPipeline::Cancellation cancel,Completion done) {
    if (!done || started_.exchange(true,std::memory_order_acq_rel)) return false;
    if (info_.completionService &&
        (info_.source.context != info_.completionService->context() ||
         info_.completionService->queueOwner() != &owner)) return false;
    try {
        auto terminal=owner.ReserveCommandTicket();
        if (!terminal) return false;
        auto mailbox=owner.ReserveCommandMailbox();
        if (!mailbox) return false;
        UsdGenExecutionPipeline::CommandTicket prepare;
        if (info_.preparer) { prepare=owner.ReserveCommandTicket(); if (!prepare) return false; }
        auto control=std::make_shared<Control>();
        control->mailbox=std::move(mailbox);
        control->terminalTicket=std::move(terminal);
        control->prepareTicket=std::move(prepare);
        auto relay=std::make_shared<Relay>();
        relay->quarantine=std::make_unique<std::shared_ptr<Relay>>();
        relay->completion=std::move(done);
        relay->requestLifetime=std::move(info_.requestLifetime);
        relay->jobLifetime=shared_from_this();
        owner_=&owner; cancellation_=std::move(cancel);
        relay_=std::move(relay);
        std::atomic_store_explicit(&control_,std::move(control),std::memory_order_release);
    } catch (...) { return false; }
    NotifyCompletion(); // Once admitted, even dispatch failure settles once.
    return true;
}
bool VulkanSourceWidthJob::NotifyCompletion() {
    if (!relay_ || relay_->settled.load(std::memory_order_acquire)) return false;
    if (state() == State::Preparing) return true; // CPU task owns the next owner handoff.
    auto control=std::atomic_load_explicit(&control_,std::memory_order_acquire);
    if (!control) return false;
    bool posted=false;
    try {
        auto self=shared_from_this();
        if (!owner_->CommandMailboxBroken(control->mailbox))
            posted=owner_->PostLatestCommand(control->mailbox,[self]{self->Advance();});
    } catch (...) {}
    if (!posted) RouteFailure();
    return posted;
}
void VulkanSourceWidthJob::NotifyFailure(VkResult status) noexcept { RouteFailure(status); }
void VulkanSourceWidthJob::RouteFailure(VkResult status) noexcept {
    auto relay=relay_;
    if (relay->settled.load(std::memory_order_acquire) ||
        relay->failurePosted.exchange(true,std::memory_order_acq_rel)) return;
    bool posted=false;
    try {
        auto control=std::atomic_exchange_explicit(&control_,
            std::shared_ptr<Control>{},std::memory_order_acq_rel);
        if (!control) { relay->Fallback(); return; }
        auto self=shared_from_this();
        posted=owner_->PostCommand(std::move(control->terminalTicket),
            [self,status]{self->FailOnOwner(status);},[relay]{relay->Fallback();});
    } catch (...) {}
    if (!posted) relay->Fallback();
}
bool VulkanSourceWidthJob::Suppressed() const noexcept {
    return suppressed_.load(std::memory_order_acquire)||cancellation_.Superseded();
}
void VulkanSourceWidthJob::Terminal(std::shared_ptr<const VulkanSourceGeneration> result,
    State state,VkResult status) noexcept {
    relay_->Settle(std::move(result),state,status,false);
}
void VulkanSourceWidthJob::FailOnOwner(VkResult status) {
    if (relay_->settled.load(std::memory_order_acquire)) return;
    // Break the job<->preparation relay anchor on the owner before terminal
    // settlement. A late worker completion then sees no job to adopt, while
    // unavailable transport keeps its explicitly quarantined relay alive.
    if (prepareRelay_) {
        prepareRelay_->jobLifetime.reset();
        prepareRelay_.reset();
    }
    if (upload_) upload_->Quarantine();
    if (width_) width_->Quarantine();
    if (length_) length_->Quarantine();
    if (cull_) cull_->Quarantine();
    if (blend_) blend_->Quarantine();
    if (compare_) compare_->Quarantine();
    Terminal({},State::LostProof,status);
}
void VulkanSourceWidthJob::BeginPreparation() {
    if (!owner_->IsExecutingOwner() || !info_.preparer || prepareRelay_) return;
    if (Suppressed()) { Terminal({},State::Superseded,VK_SUCCESS); return; }
    auto control=std::atomic_load_explicit(&control_,std::memory_order_acquire);
    if (!control || !control->prepareTicket) { Terminal({},State::Failed,VK_ERROR_OUT_OF_HOST_MEMORY); return; }
    auto relay=std::make_shared<PrepareRelay>();
    relay->owner=owner_; relay->ticket=std::move(control->prepareTicket);
    relay->jobLifetime=shared_from_this();
    relay->terminal=relay_;
    relay->quarantine=std::make_unique<std::shared_ptr<PrepareRelay>>();
    auto created=State::Created;
    if (!state_.compare_exchange_strong(created,State::Preparing,std::memory_order_acq_rel)) return;
    VulkanSourcePrepareInfo cpu{std::move(info_.source.source),std::move(info_.source.geometry),
                                std::move(info_.source.additionalNamed)};
    prepareRelay_=relay;
    UsdGenExecutionTaskGraph::Job task;
    task.cancellation=cancellation_;
    task.tasks.push_back({{},[cpu=std::move(cpu),relay](auto const&, auto finish) mutable {
        auto prepared=VulkanPreparedSource::Prepare(std::move(cpu));
        if (!prepared) { finish({},std::make_exception_ptr(
            std::runtime_error("source preparation failed"))); return; }
        finish([relay,prepared] { relay->prepared=prepared; },{});
    }});
    task.completion=[relay](UsdGenExecutionPipeline::Publish publication,std::exception_ptr error) mutable {
        relay->taskError.store(bool(error),std::memory_order_release);
        bool posted=false;
        try {
            auto ticket=std::move(relay->ticket);
            posted=relay->owner->PostCommand(std::move(ticket),
                [relay,publication=std::move(publication)]() mutable {
                    if (publication) publication();
                    auto job=relay->jobLifetime;
                    if (job) job->TakePrepared(relay);
                },[relay]{ relay->Fail(); });
        } catch (...) {}
        if (!posted) relay->Fail();
    };
    if (!info_.preparer->Submit(std::move(task))) {
        prepareRelay_.reset(); Terminal({},State::Failed,VK_ERROR_OUT_OF_HOST_MEMORY);
    }
}
void VulkanSourceWidthJob::TakePrepared(std::shared_ptr<PrepareRelay> relay) noexcept {
    if (!owner_ || !owner_->IsExecutingOwner() || relay != prepareRelay_) return;
    auto prepared=relay->prepared;
    relay->jobLifetime.reset(); // owner-only: worker relay can no longer own native job state.
    prepareRelay_.reset();
    if (Suppressed()) { Terminal({},State::Superseded,VK_SUCCESS); return; }
    if (!relay_ || relay_->settled.load(std::memory_order_acquire)) return;
    if (relay->taskError.load(std::memory_order_acquire) || !prepared) {
        Terminal({},State::Failed,VK_ERROR_INITIALIZATION_FAILED); return;
    }
    auto preparing=State::Preparing;
    if (!state_.compare_exchange_strong(preparing,State::Created,std::memory_order_acq_rel)) return;
    prepared_=std::move(prepared);
    Advance();
}
void VulkanSourceWidthJob::Advance() {
    if (relay_->settled.load(std::memory_order_acquire)) return;
    try {
        switch (state()) {
        case State::Created: {
            if (Suppressed()) {Terminal({},State::Superseded,VK_SUCCESS);return;}
            if (info_.preparer && !prepared_) { BeginPreparation(); return; }
            if (info_.completionService) {
                auto self=shared_from_this();
                watch_=info_.completionService->Reserve(self,
                    [self](VkResult proof) -> VulkanCompletionService::DeliveryResult {
                        if (proof == VK_SUCCESS) { self->watchProofReady_ = true; return self->NotifyCompletion()
                            ? VulkanCompletionService::DeliveryResult::Posted
                            : VulkanCompletionService::DeliveryResult::Stale; }
                        self->NotifyFailure(proof); return VulkanCompletionService::DeliveryResult::LostProof;
                    }, [self](VkResult proof) { self->NotifyFailure(proof); });
                if (!watch_) { RouteFailure(); return; }
            }
            VkResult status=VK_ERROR_INITIALIZATION_FAILED;
            upload_=prepared_ ? VulkanSourceUpload::Create(info_.source.context,std::move(prepared_),&status)
                              : VulkanSourceUpload::Create(std::move(info_.source),&status);
            if (!upload_) {
                if (watch_) { info_.completionService->CancelBeforeSubmit(*watch_); watch_.reset(); }
                Terminal({},State::Failed,status);return;
            }
            if (watch_ && !watch_->MarkPhaseSubmitted()) {
                watch_.reset(); RouteFailure(); return;
            }
            auto submitted=upload_->Submit();
            if (submitted!=SourceGenerationStatus::Submitted) {
                if (watch_) watch_.reset();
                Terminal({},submitted==SourceGenerationStatus::LostProof?State::LostProof:State::Failed,
                         VK_ERROR_INITIALIZATION_FAILED);return;
            }
            if (watch_ && !info_.completionService->Arm(*watch_,uint64_t(State::UploadPending))) {
                watch_.reset(); RouteFailure(); return;
            }
            auto expected=State::Created;
            state_.compare_exchange_strong(expected,State::UploadPending,std::memory_order_acq_rel);
            return;
        }
        case State::UploadPending: {
            if (watch_ && !watchProofReady_) return;
            if (watch_) { if (!watch_->Retire()) { watch_.reset(); Terminal({},State::LostProof,VK_ERROR_DEVICE_LOST); return; } watch_.reset(); watchProofReady_ = false; }
            auto proof=upload_->Poll();
            if (proof==SourceGenerationStatus::NotReady) {
                // A consumed service proof has no future notification. Its
                // proxy fence must prove this phase fence too; fail safely
                // if that contract is violated instead of silently stalling.
                if (info_.completionService) FailOnOwner();
                return;
            }
            if (proof!=SourceGenerationStatus::Ready) {
                Terminal({},State::LostProof,VK_ERROR_DEVICE_LOST);return;
            }
            if (Suppressed()) {Terminal({},State::Superseded,VK_SUCCESS);return;}
            base_=upload_->TakeReady();
            if (!base_) {Terminal({},State::Failed,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
            values_.push_back(base_);
            BeginStage();
            return;
        }
        case State::LengthPending: FinishLength(); return;
        case State::CullCountsPending: FinishCull(false); return;
        case State::CullScatterPending: FinishCull(true); return;
        case State::WidthPending: FinishWidth();return;
        case State::BlendPending: FinishBlend(); return;
        case State::ComparePending: FinishCompare(); return;
        case State::NoisePending: FinishNoise(); return;
        case State::DeformPending: FinishDeform(); return;
        default:return;
        }
    } catch (...) {FailOnOwner();}
}
void VulkanSourceWidthJob::BeginStage() {
    if (Suppressed()) { Terminal({}, State::Superseded, VK_SUCCESS); return; }
    auto const& stage = info_.stages[stageIndex_];
    base_ = values_[stage.input];
    compare_.reset();
    if (stage.disabled) {
        // Muted operators alias their input generation exactly: no native
        // work is submitted and no new value is published.
        FinishStage(values_[stage.input]);
        return;
    }
    if (stageIndex_ == 0) stageFrom_ = State::UploadPending;
    else {
        auto previous = info_.stages[stageIndex_ - 1].kind;
        stageFrom_ = previous == VulkanSourceWidthStage::Kind::Width ? State::WidthPending :
            previous == VulkanSourceWidthStage::Kind::LengthScale ? State::LengthPending :
            previous == VulkanSourceWidthStage::Kind::LengthCull ? State::CullScatterPending :
            previous == VulkanSourceWidthStage::Kind::Noise ? State::NoisePending :
            previous == VulkanSourceWidthStage::Kind::Deform ? State::DeformPending : State::BlendPending;
    }
    if (stage.kind == VulkanSourceWidthStage::Kind::Width) BeginWidth();
    else if (stage.kind == VulkanSourceWidthStage::Kind::LengthScale) BeginLength();
    else if (stage.kind == VulkanSourceWidthStage::Kind::LengthCull) BeginCull();
    else if (stage.kind == VulkanSourceWidthStage::Kind::Noise) BeginNoise();
    else if (stage.kind == VulkanSourceWidthStage::Kind::Deform) BeginDeform();
    else BeginBlend();
}
void VulkanSourceWidthJob::FinishStage(std::shared_ptr<const VulkanSourceGeneration> value) {
    values_.push_back(value);
    ++stageIndex_;
    if (stageIndex_ == info_.stages.size()) { Terminal(std::move(value), State::Ready, VK_SUCCESS); return; }
    BeginStage();
}
void VulkanSourceWidthJob::BeginCompare() {
    if (!info_.nonWidthComparePipeline) {
        Terminal({}, State::Failed, VK_ERROR_FEATURE_NOT_PRESENT); return;
    }
    VkResult status = VK_ERROR_INITIALIZATION_FAILED;
    if (info_.completionService) {
        auto self = shared_from_this();
        watch_ = info_.completionService->Reserve(self,
            [self](VkResult proof) -> VulkanCompletionService::DeliveryResult {
                if (proof == VK_SUCCESS) {
                    self->watchProofReady_ = true;
                    return self->NotifyCompletion() ? VulkanCompletionService::DeliveryResult::Posted
                                                   : VulkanCompletionService::DeliveryResult::Stale;
                }
                self->NotifyFailure(proof); return VulkanCompletionService::DeliveryResult::LostProof;
            }, [self](VkResult proof) { self->NotifyFailure(proof); });
        if (!watch_) { RouteFailure(); return; }
    }
    bool marked = false;
    NonWidthComparePipeline::BeforeSubmit before;
    if (watch_) before = [this, &marked] { marked = watch_ && watch_->MarkPhaseSubmitted(); return marked; };
    compare_ = info_.nonWidthComparePipeline->Begin(base_, values_[info_.stages[stageIndex_].rightInput],
        &status, std::move(before));
    if (!compare_) {
        if (watch_) { if (!marked) info_.completionService->CancelBeforeSubmit(*watch_); watch_.reset(); }
        Terminal({}, marked ? State::LostProof : State::Failed, status); return;
    }
    if (watch_ && (!marked || !info_.completionService->Arm(*watch_, uint64_t(State::ComparePending)))) {
        watch_.reset(); RouteFailure(); return;
    }
    auto expected = stageFrom_;
    state_.compare_exchange_strong(expected, State::ComparePending, std::memory_order_acq_rel);
}
void VulkanSourceWidthJob::FinishCompare() {
    if (watch_) {
        if (!watchProofReady_) return;
        if (!watch_->Retire()) { watch_.reset(); Terminal({}, State::LostProof, VK_ERROR_DEVICE_LOST); return; }
        watch_.reset(); watchProofReady_ = false;
    }
    auto proof = compare_->Poll();
    if (proof == VK_NOT_READY) { if (info_.completionService) FailOnOwner(); return; }
    if (proof != VK_SUCCESS) { Terminal({}, State::LostProof, proof); return; }
    if (Suppressed()) { Terminal({}, State::Superseded, VK_SUCCESS); return; }
    if (!compare_->succeeded()) { Terminal({}, State::Failed, VK_ERROR_VALIDATION_FAILED_EXT); return; }
    stageFrom_ = State::ComparePending;
    BeginBlend();
}
void VulkanSourceWidthJob::BeginBlend() {
    auto const& stage = info_.stages[stageIndex_];
    auto const& right = values_[stage.rightInput];
    if (base_->NonWidthIdentity() != right->NonWidthIdentity()) {
        if (!compare_) { BeginCompare(); return; }
        if (!compare_->succeeded() || compare_->leftOwner() != base_ || compare_->rightOwner() != right) {
            Terminal({}, State::Failed, VK_ERROR_VALIDATION_FAILED_EXT); return;
        }
    }
    VkResult status = VK_ERROR_INITIALIZATION_FAILED;
    if (info_.completionService && base_->pointCount()) {
        auto self = shared_from_this();
        watch_ = info_.completionService->Reserve(self,
            [self](VkResult proof) -> VulkanCompletionService::DeliveryResult {
                if (proof == VK_SUCCESS) {
                    self->watchProofReady_ = true;
                    return self->NotifyCompletion() ? VulkanCompletionService::DeliveryResult::Posted
                                                   : VulkanCompletionService::DeliveryResult::Stale;
                }
                self->NotifyFailure(proof); return VulkanCompletionService::DeliveryResult::LostProof;
            }, [self](VkResult proof) { self->NotifyFailure(proof); });
        if (!watch_) { RouteFailure(); return; }
    }
    bool marked = false;
    WidthBlendPipeline::BeforeSubmit before;
    if (watch_) before = [this, &marked] { marked = watch_ && watch_->MarkPhaseSubmitted(); return marked; };
    blend_ = info_.widthBlendPipeline->Begin(base_->PlaneOwner("width"), right->PlaneOwner("width"),
        base_->pointCount(), stage.blend, &status, std::move(before));
    if (!blend_) {
        if (watch_) { if (!marked) info_.completionService->CancelBeforeSubmit(*watch_); watch_.reset(); }
        Terminal({}, marked ? State::LostProof : State::Failed, status); return;
    }
    if (watch_ && (!marked || !info_.completionService->Arm(*watch_, uint64_t(State::BlendPending)))) {
        watch_.reset(); RouteFailure(); return;
    }
    auto expected = stageFrom_;
    state_.compare_exchange_strong(expected, State::BlendPending, std::memory_order_acq_rel);
    if (!base_->pointCount()) FinishBlend();
}
void VulkanSourceWidthJob::FinishBlend() {
    if (watch_) {
        if (!watchProofReady_) return;
        if (!watch_->Retire()) { watch_.reset(); Terminal({}, State::LostProof, VK_ERROR_DEVICE_LOST); return; }
        watch_.reset(); watchProofReady_ = false;
    }
    uint32_t semantic = 0;
    auto proof = blend_->Poll(&semantic);
    if (proof == VK_NOT_READY) { if (info_.completionService) FailOnOwner(); return; }
    if (proof != VK_SUCCESS) { Terminal({}, State::LostProof, proof); return; }
    if (Suppressed()) { Terminal({}, State::Superseded, VK_SUCCESS); return; }
    if (semantic) { Terminal({}, State::Failed, VK_ERROR_VALIDATION_FAILED_EXT); return; }
    auto value = VulkanSourceGeneration::WithWidthBlend(base_, values_[info_.stages[stageIndex_].rightInput],
        *blend_, info_.stageValueVersions[stageIndex_], nullptr, compare_.get());
    if (!value) { Terminal({}, State::Failed, VK_ERROR_INITIALIZATION_FAILED); return; }
    FinishStage(std::move(value));
}
void VulkanSourceWidthJob::BeginCull(bool scatter) {
    if (Suppressed()) { Terminal({}, State::Superseded, VK_SUCCESS); return; }
    VkResult status = VK_ERROR_INITIALIZATION_FAILED;
    if (info_.completionService) {
        auto self = shared_from_this();
        watch_ = info_.completionService->Reserve(self,
            [self](VkResult proof) -> VulkanCompletionService::DeliveryResult {
                if (proof == VK_SUCCESS) {
                    self->watchProofReady_ = true;
                    return self->NotifyCompletion() ? VulkanCompletionService::DeliveryResult::Posted
                                                   : VulkanCompletionService::DeliveryResult::Stale;
                }
                self->NotifyFailure(proof); return VulkanCompletionService::DeliveryResult::LostProof;
            }, [self](VkResult proof) { self->NotifyFailure(proof); });
        if (!watch_) { RouteFailure(); return; }
    }
    bool phaseMarked = false;
    LengthCompactionPipeline::BeforeSubmit beforeSubmit;
    if (watch_) beforeSubmit = [this, &phaseMarked] {
        phaseMarked = watch_ && watch_->MarkPhaseSubmitted(); return phaseMarked;
    };
    bool submitted = false;
    if (scatter) submitted = cull_->BeginScatter(&status, std::move(beforeSubmit));
    else {
        auto const& stage = info_.stages[stageIndex_];
        float const effectiveThreshold = ResolveLengthEnvelope(stage.lengthBlend, stage.lengthMaskAmount) == 0.0f
            ? 0.0f : stage.cullThreshold;
        cull_ = info_.lengthCompactionPipeline->Begin(base_, effectiveThreshold, &status, std::move(beforeSubmit));
        submitted = bool(cull_);
    }
    if (!submitted) {
        // Both cull submissions cross the native-lifetime admission boundary.
        // A failed scatter return after that boundary cannot prove that no
        // reads were queued, so retain the candidate's full native graph.
        // This also makes the job robust if the pipeline reports a post-mark
        // submission failure before it can quarantine itself.
        if (phaseMarked && cull_) cull_->Quarantine();
        if (watch_) {
            if (!phaseMarked) info_.completionService->CancelBeforeSubmit(*watch_);
            watch_.reset();
        }
        Terminal({}, phaseMarked ? State::LostProof : State::Failed, status); return;
    }
    State const pending = scatter ? State::CullScatterPending : State::CullCountsPending;
    if (watch_ && (!phaseMarked || !info_.completionService->Arm(*watch_, uint64_t(pending)))) {
        watch_.reset(); RouteFailure(); return;
    }
    auto expected = scatter ? State::CullCountsPending : stageFrom_;
    state_.compare_exchange_strong(expected, pending, std::memory_order_acq_rel);
}
void VulkanSourceWidthJob::FinishCull(bool scatter) {
    if (watch_) {
        if (!watchProofReady_) return;
        if (!watch_->Retire()) { watch_.reset(); Terminal({}, State::LostProof, VK_ERROR_DEVICE_LOST); return; }
        watch_.reset(); watchProofReady_ = false;
    }
    uint32_t semantic = 0;
    auto proof = scatter ? cull_->PollScatter(&semantic) : cull_->PollCounts(&semantic);
    if (proof == VK_NOT_READY) { if (info_.completionService) FailOnOwner(); return; }
    if (proof != VK_SUCCESS) { Terminal({}, State::LostProof, proof); return; }
    if (Suppressed()) { Terminal({}, State::Superseded, VK_SUCCESS); return; }
    if (semantic) { Terminal({}, State::Failed, VK_ERROR_VALIDATION_FAILED_EXT); return; }
    if (!scatter) { BeginCull(true); return; }
    auto child = VulkanSourceGeneration::WithCompacted(base_, *cull_,
        info_.stageValueVersions[stageIndex_]);
    if (!child) { Terminal({}, State::Failed, VK_ERROR_INITIALIZATION_FAILED); return; }
    FinishStage(std::move(child));
}
void VulkanSourceWidthJob::BeginLength() {
    VkResult status = VK_ERROR_INITIALIZATION_FAILED;
    if (info_.completionService && base_->pointCount()) {
        auto self = shared_from_this();
        watch_ = info_.completionService->Reserve(self,
            [self](VkResult proof) -> VulkanCompletionService::DeliveryResult {
                if (proof == VK_SUCCESS) {
                    self->watchProofReady_ = true;
                    return self->NotifyCompletion() ? VulkanCompletionService::DeliveryResult::Posted
                                                   : VulkanCompletionService::DeliveryResult::Stale;
                }
                self->NotifyFailure(proof); return VulkanCompletionService::DeliveryResult::LostProof;
            }, [self](VkResult proof) { self->NotifyFailure(proof); });
        if (!watch_) { RouteFailure(); return; }
    }
    bool phaseMarked = false;
    LengthScalePipeline::BeforeSubmit beforeSubmit;
    if (watch_) beforeSubmit = [this, &phaseMarked] {
        phaseMarked = watch_ && watch_->MarkPhaseSubmitted(); return phaseMarked;
    };
    auto const& stage = info_.stages[stageIndex_];
    bool const literalV1 = stage.randomLo != 1.0f || stage.randomHi != 1.0f;
    bool const nonneutralEnvelope = stage.lengthBlend != 1.0f || stage.lengthMaskAmount != 1.0f;
    length_ = nonneutralEnvelope
        ? info_.lengthPipeline->BeginEnvelopeV1(base_->PlaneOwner("points"),
            base_->PlaneOwner("curveOffsets"), base_->PlaneOwner("hairT"),
            base_->PlaneOwner("stableIds"), base_->curveCount(), base_->pointCount(),
            LengthScalePipeline::EnvelopeV1Controls{stage.factor, stage.minRemainingLength,
                stage.lengthMode == VulkanSourceWidthStage::LengthMode::Set,
                stage.lengthMethod == VulkanSourceWidthStage::LengthMethod::CutExtend
                    ? LengthScalePipeline::EnvelopeV1Controls::Method::CutExtend
                    : LengthScalePipeline::EnvelopeV1Controls::Method::Scale,
                stage.lengthRebuild == VulkanSourceWidthStage::LengthRebuild::Reparam
                    ? LengthScalePipeline::EnvelopeV1Controls::Rebuild::Reparam
                    : LengthScalePipeline::EnvelopeV1Controls::Rebuild::KeepParam,
                stage.randomLo, stage.randomHi, stage.randomSeed, stage.lengthBlend,
                stage.lengthMaskAmount, stage.lengthCullOnly},
            &status, std::move(beforeSubmit))
        : literalV1
        ? info_.lengthPipeline->BeginLiteralV1(base_->PlaneOwner("points"),
            base_->PlaneOwner("curveOffsets"), base_->PlaneOwner("hairT"),
            base_->PlaneOwner("stableIds"), base_->curveCount(), base_->pointCount(),
            LengthScalePipeline::LiteralV1Controls{stage.factor, stage.minRemainingLength,
                stage.lengthMode == VulkanSourceWidthStage::LengthMode::Set,
                stage.lengthMethod == VulkanSourceWidthStage::LengthMethod::CutExtend
                    ? LengthScalePipeline::LiteralV1Controls::Method::CutExtend
                    : LengthScalePipeline::LiteralV1Controls::Method::Scale,
                stage.lengthRebuild == VulkanSourceWidthStage::LengthRebuild::Reparam
                    ? LengthScalePipeline::LiteralV1Controls::Rebuild::Reparam
                    : LengthScalePipeline::LiteralV1Controls::Rebuild::KeepParam,
                stage.randomLo, stage.randomHi, stage.randomSeed},
            &status, std::move(beforeSubmit))
        : stage.minRemainingLength > 0
        ? info_.lengthPipeline->BeginMinimum(base_->PlaneOwner("points"),
            base_->PlaneOwner("curveOffsets"), base_->PlaneOwner("hairT"),
            base_->curveCount(), base_->pointCount(),
            LengthScalePipeline::MinimumControls{stage.factor, stage.minRemainingLength,
                stage.lengthMode == VulkanSourceWidthStage::LengthMode::Set,
                stage.lengthMethod == VulkanSourceWidthStage::LengthMethod::CutExtend
                    ? LengthScalePipeline::MinimumControls::Method::CutExtend
                    : LengthScalePipeline::MinimumControls::Method::Scale,
                stage.lengthRebuild == VulkanSourceWidthStage::LengthRebuild::Reparam
                    ? LengthScalePipeline::MinimumControls::Rebuild::Reparam
                    : LengthScalePipeline::MinimumControls::Rebuild::KeepParam},
            &status, std::move(beforeSubmit))
        : stage.lengthMethod == VulkanSourceWidthStage::LengthMethod::CutExtend &&
              stage.lengthRebuild == VulkanSourceWidthStage::LengthRebuild::Reparam
        ? info_.lengthPipeline->BeginReparam(base_->PlaneOwner("points"),
            base_->PlaneOwner("curveOffsets"), base_->PlaneOwner("hairT"),
            base_->curveCount(), base_->pointCount(), stage.factor,
            stage.lengthMode == VulkanSourceWidthStage::LengthMode::Set,
            &status, std::move(beforeSubmit))
        : stage.lengthMethod == VulkanSourceWidthStage::LengthMethod::CutExtend
        ? info_.lengthPipeline->BeginCutExtend(base_->PlaneOwner("points"),
            base_->PlaneOwner("curveOffsets"), base_->curveCount(), base_->pointCount(),
            stage.factor, stage.lengthMode == VulkanSourceWidthStage::LengthMode::Set,
            &status, std::move(beforeSubmit))
        : stage.lengthMode == VulkanSourceWidthStage::LengthMode::Set
        ? info_.lengthPipeline->BeginSet(base_->PlaneOwner("points"),
            base_->PlaneOwner("curveOffsets"), base_->curveCount(), base_->pointCount(),
            stage.factor, &status, std::move(beforeSubmit))
        : info_.lengthPipeline->Begin(base_->PlaneOwner("points"),
            base_->PlaneOwner("curveOffsets"), base_->curveCount(), base_->pointCount(),
            stage.factor, &status, std::move(beforeSubmit));
    if (!length_) {
        if (watch_) {
            if (!phaseMarked) info_.completionService->CancelBeforeSubmit(*watch_);
            watch_.reset();
        }
        Terminal({}, phaseMarked ? State::LostProof : State::Failed, status); return;
    }
    if (watch_ && (!phaseMarked || !info_.completionService->Arm(*watch_, uint64_t(State::LengthPending)))) {
        watch_.reset(); RouteFailure(); return;
    }
    auto expected = stageFrom_;
    state_.compare_exchange_strong(expected, State::LengthPending, std::memory_order_acq_rel);
    if (!base_->pointCount()) FinishLength();
}
void VulkanSourceWidthJob::FinishLength() {
    if (watch_) {
        if (!watchProofReady_) return;
        if (!watch_->Retire()) { watch_.reset(); Terminal({}, State::LostProof, VK_ERROR_DEVICE_LOST); return; }
        watch_.reset(); watchProofReady_ = false;
    }
    uint32_t semantic = 0;
    auto proof = length_->Poll(&semantic);
    if (proof == VK_NOT_READY) { if (info_.completionService) FailOnOwner(); return; }
    if (proof != VK_SUCCESS) { Terminal({}, State::LostProof, proof); return; }
    if (Suppressed()) { Terminal({}, State::Superseded, VK_SUCCESS); return; }
    if (semantic) { Terminal({}, State::Failed, VK_ERROR_VALIDATION_FAILED_EXT); return; }
    auto child = VulkanSourceGeneration::WithPoints(base_, *length_, info_.stageValueVersions[stageIndex_]);
    if (!child) { Terminal({}, State::Failed, VK_ERROR_INITIALIZATION_FAILED); return; }
    FinishStage(std::move(child));
}
void VulkanSourceWidthJob::BeginNoise() {
    VkResult status = VK_ERROR_INITIALIZATION_FAILED;
    if (info_.completionService && base_->pointCount()) {
        auto self = shared_from_this();
        watch_ = info_.completionService->Reserve(self,
            [self](VkResult proof) -> VulkanCompletionService::DeliveryResult {
                if (proof == VK_SUCCESS) {
                    self->watchProofReady_ = true;
                    return self->NotifyCompletion() ? VulkanCompletionService::DeliveryResult::Posted
                                                   : VulkanCompletionService::DeliveryResult::Stale;
                }
                self->NotifyFailure(proof); return VulkanCompletionService::DeliveryResult::LostProof;
            }, [self](VkResult proof) { self->NotifyFailure(proof); });
        if (!watch_) { RouteFailure(); return; }
    }
    bool phaseMarked = false;
    NoisePipeline::BeforeSubmit beforeSubmit;
    if (watch_) beforeSubmit = [this, &phaseMarked] {
        phaseMarked = watch_ && watch_->MarkPhaseSubmitted(); return phaseMarked;
    };
    auto const& stage = info_.stages[stageIndex_];
    // The literal lane has no magnitude knots: a flat 257-entry profile of 1.0.
    float profile[257];
    for (int i = 0; i < 257; ++i) profile[i] = 1.0f;
    auto const& ctx = base_->context();
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 257u * sizeof(float);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto magnitudeProfile = ChargedBuffer::Create(ctx, bi,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        UsdGenExecutionResourceKind::Scratch);
    if (!magnitudeProfile) {
        status = VK_ERROR_OUT_OF_HOST_MEMORY;
        if (watch_) { if (!phaseMarked) info_.completionService->CancelBeforeSubmit(*watch_); watch_.reset(); }
        Terminal({}, phaseMarked ? State::LostProof : State::Failed, status); return;
    }
    {
        void* p = nullptr;
        if (vkMapMemory(ctx->device(), magnitudeProfile->memory(), 0, bi.size, 0, &p) != VK_SUCCESS) {
            status = VK_ERROR_OUT_OF_HOST_MEMORY;
            if (watch_) { if (!phaseMarked) info_.completionService->CancelBeforeSubmit(*watch_); watch_.reset(); }
            Terminal({}, phaseMarked ? State::LostProof : State::Failed, status); return;
        }
        std::memcpy(p, profile, bi.size);
        vkUnmapMemory(ctx->device(), magnitudeProfile->memory());
    }
    NoisePipeline::BeginInfo info;
    info.points = base_->PlaneOwner("points");
    info.restPoints = base_->PlaneOwner("rest");
    info.curveOffsets = base_->PlaneOwner("curveOffsets");
    info.stableIds = base_->PlaneOwner("stableIds");
    info.curveCount = base_->curveCount();
    info.pointCount = base_->pointCount();
    info.hairT = base_->PlaneOwner("hairT");
    info.frameTangent = base_->SourceFrameOwner("sourceRootT");
    info.frameBinormal = base_->SourceFrameOwner("sourceRootB");
    info.frameNormal = base_->SourceFrameOwner("sourceRootN");
    info.frameStableIds = nullptr;
    info.frameCount = base_->curveCount();
    info.magnitudeProfile = std::move(magnitudeProfile);
    info.magnitude = {stage.noise.magnitude, 1, nullptr, 0};
    info.frequency = {stage.noise.frequency, 1, nullptr, 0};
    info.correlation = {stage.noise.correlation, 1, nullptr, 0};
    info.lacunarity = {stage.noise.lacunarity, 1, nullptr, 0};
    info.gain = {stage.noise.gain, 1, nullptr, 0};
    info.preserveLength = {stage.noise.preserveLength, 1, nullptr, 0};
    info.mask = {stage.noise.mask, 1, nullptr, 0};
    info.octaves = {stage.noise.octaves, 1, nullptr, 0};
    info.seed = {stage.noise.seed, 1, nullptr, 0};
    info.enabled = {1, 1, nullptr, 0};
    info.cumulative = {stage.noise.cumulative ? 1u : 0u, 1, nullptr, 0};
    NoiseSemantic beginSemantic = NoiseSemantic::Ok;
    noise_ = info_.noisePipeline->Begin(std::move(info), &status, &beginSemantic, std::move(beforeSubmit));
    if (!noise_) {
        if (watch_) {
            if (!phaseMarked) info_.completionService->CancelBeforeSubmit(*watch_);
            watch_.reset();
        }
        Terminal({}, phaseMarked ? State::LostProof : State::Failed, status); return;
    }
    if (watch_ && (!phaseMarked || !info_.completionService->Arm(*watch_, uint64_t(State::NoisePending)))) {
        watch_.reset(); RouteFailure(); return;
    }
    auto expected = stageFrom_;
    state_.compare_exchange_strong(expected, State::NoisePending, std::memory_order_acq_rel);
    if (!base_->pointCount()) FinishNoise();
}
void VulkanSourceWidthJob::FinishNoise() {
    if (watch_) {
        if (!watchProofReady_) return;
        if (!watch_->Retire()) { watch_.reset(); Terminal({}, State::LostProof, VK_ERROR_DEVICE_LOST); return; }
        watch_.reset(); watchProofReady_ = false;
    }
    NoiseSemantic semantic = NoiseSemantic::Ok;
    auto proof = noise_->Poll(&semantic);
    if (proof == VK_NOT_READY) { if (info_.completionService) FailOnOwner(); return; }
    if (proof != VK_SUCCESS) { Terminal({}, State::LostProof, proof); return; }
    if (Suppressed()) { Terminal({}, State::Superseded, VK_SUCCESS); return; }
    if (!noise_->succeeded() || semantic != NoiseSemantic::Ok) { Terminal({}, State::Failed, VK_ERROR_VALIDATION_FAILED_EXT); return; }
    auto child = VulkanSourceGeneration::WithNoise(base_, *noise_, info_.stageValueVersions[stageIndex_]);
    if (!child) { Terminal({}, State::Failed, VK_ERROR_INITIALIZATION_FAILED); return; }
    FinishStage(std::move(child));
}
void VulkanSourceWidthJob::BeginDeform() {
    VkResult status = VK_ERROR_INITIALIZATION_FAILED;
    if (info_.completionService && base_->pointCount()) {
        auto self = shared_from_this();
        watch_ = info_.completionService->Reserve(self,
            [self](VkResult proof) -> VulkanCompletionService::DeliveryResult {
                if (proof == VK_SUCCESS) {
                    self->watchProofReady_ = true;
                    return self->NotifyCompletion() ? VulkanCompletionService::DeliveryResult::Posted
                                                   : VulkanCompletionService::DeliveryResult::Stale;
                }
                self->NotifyFailure(proof); return VulkanCompletionService::DeliveryResult::LostProof;
            }, [self](VkResult proof) { self->NotifyFailure(proof); });
        if (!watch_) { RouteFailure(); return; }
    }
    bool phaseMarked = false;
    DeformPipeline::BeforeSubmit beforeSubmit;
    if (watch_) beforeSubmit = [this, &phaseMarked] {
        phaseMarked = watch_ && watch_->MarkPhaseSubmitted(); return phaseMarked;
    };
    auto const& stage = info_.stages[stageIndex_];
    auto const& ctx = base_->context();
    // rootTargets: upload host vector to a scratch host-visible buffer.
    // Vulkan forbids zero-size buffers; clamp to 4 bytes minimum.
    uint32_t const curves = base_->curveCount();
    VkDeviceSize rtBytes = std::max<VkDeviceSize>(VkDeviceSize(curves) * 12, 4u);
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = rtBytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto rootTargetsBuf = ChargedBuffer::Create(ctx, bi,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        UsdGenExecutionResourceKind::Scratch);
    if (!rootTargetsBuf) {
        status = VK_ERROR_OUT_OF_HOST_MEMORY;
        if (watch_) { if (!phaseMarked) info_.completionService->CancelBeforeSubmit(*watch_); watch_.reset(); }
        Terminal({}, phaseMarked ? State::LostProof : State::Failed, status); return;
    }
    {
        void* p = nullptr;
        if (vkMapMemory(ctx->device(), rootTargetsBuf->memory(), 0,
                std::min(rtBytes, VkDeviceSize(stage.deform.rootTargets.size() * 4)), 0, &p) != VK_SUCCESS) {
            status = VK_ERROR_OUT_OF_HOST_MEMORY;
            if (watch_) { if (!phaseMarked) info_.completionService->CancelBeforeSubmit(*watch_); watch_.reset(); }
            Terminal({}, phaseMarked ? State::LostProof : State::Failed, status); return;
        }
        if (!stage.deform.rootTargets.empty())
            std::memcpy(p, stage.deform.rootTargets.data(), stage.deform.rootTargets.size() * 4);
        vkUnmapMemory(ctx->device(), rootTargetsBuf->memory());
    }
    DeformPipeline::BeginInfo info;
    info.points = base_->PlaneOwner("points");
    info.curveOffsets = base_->PlaneOwner("curveOffsets");
    info.rootTargets = std::move(rootTargetsBuf);
    info.curveCount = base_->curveCount();
    info.pointCount = base_->pointCount();
    info.restSamples = stage.deform.restSamples;
    info.posedSamples = stage.deform.posedSamples;
    info.sampleCount = stage.deform.sampleCount;
    info.smoothing = 0.0;
    info.mask = {stage.deform.mask, 1, nullptr, 0};
    info.enabled = {1, 1, nullptr, 0};
    info.lockRoots = {stage.deform.lockRoots ? 1u : 0u, 1, nullptr, 0};
    info.groomEnvelope = stage.deform.groomEnvelope;
    DeformSemantic beginSemantic = DeformSemantic::Ok;
    deform_ = info_.deformPipeline->Begin(std::move(info), &status, &beginSemantic, std::move(beforeSubmit));
    if (!deform_) {
        if (watch_) {
            if (!phaseMarked) info_.completionService->CancelBeforeSubmit(*watch_);
            watch_.reset();
        }
        Terminal({}, phaseMarked ? State::LostProof : State::Failed, status); return;
    }
    if (watch_ && (!phaseMarked || !info_.completionService->Arm(*watch_, uint64_t(State::DeformPending)))) {
        watch_.reset(); RouteFailure(); return;
    }
    auto expected = stageFrom_;
    state_.compare_exchange_strong(expected, State::DeformPending, std::memory_order_acq_rel);
    if (!base_->pointCount()) FinishDeform();
}
void VulkanSourceWidthJob::FinishDeform() {
    if (watch_) {
        if (!watchProofReady_) return;
        if (!watch_->Retire()) { watch_.reset(); Terminal({}, State::LostProof, VK_ERROR_DEVICE_LOST); return; }
        watch_.reset(); watchProofReady_ = false;
    }
    DeformSemantic semantic = DeformSemantic::Ok;
    auto proof = deform_->Poll(&semantic);
    if (proof == VK_NOT_READY) { if (info_.completionService) FailOnOwner(); return; }
    if (proof != VK_SUCCESS) { Terminal({}, State::LostProof, proof); return; }
    if (Suppressed()) { Terminal({}, State::Superseded, VK_SUCCESS); return; }
    if (!deform_->succeeded() || semantic != DeformSemantic::Ok) { Terminal({}, State::Failed, VK_ERROR_VALIDATION_FAILED_EXT); return; }
    auto child = VulkanSourceGeneration::WithDeform(base_, *deform_, info_.stageValueVersions[stageIndex_]);
    if (!child) { Terminal({}, State::Failed, VK_ERROR_INITIALIZATION_FAILED); return; }
    FinishStage(std::move(child));
}
void VulkanSourceWidthJob::BeginWidth() {
            auto const& controls = info_.stages[stageIndex_].width;
            VkResult status=VK_ERROR_INITIALIZATION_FAILED;
            if (base_->pointCount() && !base_->PlaneOwner("width")) {
                Terminal({},State::Failed,status); return;
            }
            if (info_.completionService && base_->pointCount()) {
                auto self=shared_from_this();
                watch_=info_.completionService->Reserve(self,
                    [self](VkResult proof) -> VulkanCompletionService::DeliveryResult {
                        if (proof == VK_SUCCESS) { self->watchProofReady_ = true; return self->NotifyCompletion()
                            ? VulkanCompletionService::DeliveryResult::Posted
                            : VulkanCompletionService::DeliveryResult::Stale; }
                        self->NotifyFailure(proof); return VulkanCompletionService::DeliveryResult::LostProof;
                    }, [self](VkResult proof) { self->NotifyFailure(proof); });
                if (!watch_) { RouteFailure(); return; }
            }
            bool phaseMarked = false;
            WidthPipeline::BeforeSubmit beforeSubmit;
            if (watch_) beforeSubmit = [this, &phaseMarked] {
                phaseMarked = watch_ && watch_->MarkPhaseSubmitted();
                return phaseMarked;
            };
            width_=info_.widthPipeline->Begin(base_->PlaneOwner("width"),base_->pointCount(),
                controls.width,controls.replace ? 1u : 0u,&status,std::move(beforeSubmit));
            if (!width_) {
                if (watch_) {
                    if (!phaseMarked) {
                        info_.completionService->CancelBeforeSubmit(*watch_);
                    }
                    watch_.reset();
                }
                // A post-admission failure may have submitted native reads;
                // downstream publication must retain its lifetime domain.
                Terminal({},phaseMarked?State::LostProof:State::Failed,status);return;
            }
            if (watch_ && !phaseMarked) { watch_.reset(); RouteFailure(); return; }
            if (watch_ && !info_.completionService->Arm(*watch_,uint64_t(State::WidthPending))) {
                watch_.reset(); RouteFailure(); return;
            }
            auto expected=stageFrom_;
            state_.compare_exchange_strong(expected,State::WidthPending,std::memory_order_acq_rel);
            if (!base_->pointCount()) FinishWidth(); // Empty has no native event.
            return;
}
void VulkanSourceWidthJob::FinishWidth() {
    if (watch_) {
        if (!watchProofReady_) return;
        if (!watch_->Retire()) { watch_.reset(); Terminal({},State::LostProof,VK_ERROR_DEVICE_LOST); return; }
        watch_.reset(); watchProofReady_ = false;
    }
    uint32_t semantic=0;
    auto proof=width_->Poll(&semantic);
    if (proof==VK_NOT_READY) {
        if (info_.completionService) FailOnOwner();
        return;
    }
    if (proof!=VK_SUCCESS) {Terminal({},State::LostProof,proof);return;}
    if (Suppressed()) {Terminal({},State::Superseded,VK_SUCCESS);return;}
    if (semantic) {Terminal({},State::Failed,VK_ERROR_VALIDATION_FAILED_EXT);return;}
    auto result=VulkanSourceGeneration::WithWidth(base_,*width_,info_.stageValueVersions[stageIndex_]);
    if (!result) { Terminal({}, State::Failed, VK_ERROR_INITIALIZATION_FAILED); return; }
    FinishStage(std::move(result));
}
} // namespace usdGen::vulkan
