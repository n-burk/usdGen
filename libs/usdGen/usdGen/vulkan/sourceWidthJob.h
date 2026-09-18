#ifndef USDGEN_VULKAN_SOURCE_WIDTH_JOB_H
#define USDGEN_VULKAN_SOURCE_WIDTH_JOB_H
#include "sourceGeneration.h"
#include "completionService.h"
#include "lengthScalePipeline.h"
#include "lengthCompactionPipeline.h"
#include "widthBlendPipeline.h"
#include "nonWidthComparePipeline.h"
#include "noisePipeline.h"
#include "executionPlan.h"
#include "usdGen/executionPipeline.h"
#include "usdGen/executionTaskGraph.h"
#include <atomic>
#include <functional>
namespace usdGen::vulkan {
// Owner-confined, value-indexed execution. Stages submit serially in validated
// topological order; fan-out shares immutable values and fan-in uses explicit
// ordered inputs. This does not claim concurrent GPU branch execution.
class VulkanSourceWidthJob final : public std::enable_shared_from_this<VulkanSourceWidthJob> {
public:
    enum class State { Created, Preparing, UploadPending, WidthPending, Ready, Superseded, Failed, LostProof, LengthPending, BlendPending, ComparePending, CullCountsPending, CullScatterPending, NoisePending };
    struct CreateInfo {
        VulkanSourceGenerationCreateInfo source;
        std::shared_ptr<WidthPipeline> widthPipeline;
        float width=0;
        uint32_t replace=0;
        uint64_t valueVersion=0;
        std::shared_ptr<const void> requestLifetime;
        std::shared_ptr<VulkanCompletionService> completionService;
        // Shared bounded CPU task dispatcher. When supplied, validation and
        // packing run off-owner; native allocation/copy/submission remain on
        // owner. Null preserves the explicit synchronous-preparation probe
        // path, not the intended production scheduling policy.
        std::shared_ptr<UsdGenExecutionTaskGraph> preparer;
        std::shared_ptr<LengthScalePipeline> lengthPipeline = {};
        float lengthFactor = 1.0f;
        uint64_t lengthValueVersion = 0;
        std::shared_ptr<WidthBlendPipeline> widthBlendPipeline = {};
        // Empty stages retain the legacy optional-Length/Width shorthand.
        // Explicit stages require one strictly increasing revision per stage.
        std::vector<VulkanSourceWidthStage> stages = {};
        std::vector<uint64_t> stageValueVersions = {};
        std::shared_ptr<NonWidthComparePipeline> nonWidthComparePipeline = {};
        std::shared_ptr<LengthCompactionPipeline> lengthCompactionPipeline = {};
        std::shared_ptr<NoisePipeline> noisePipeline = {};
    };
    using Completion = std::function<void(std::shared_ptr<const VulkanSourceGeneration>, State, VkResult)>;
    static std::shared_ptr<VulkanSourceWidthJob> Create(CreateInfo, std::string* reason=nullptr);
    // Bind once and reserve progress, terminal and optional preparation-return
    // admission before submission.
    // False means no native work/callback admitted; Start is single-attempt,
    // including admission failure (create a fresh job to retry).
    // Externally retain owner and
    // stop/join notification producers through terminal completion.
    bool Start(UsdGenExecutionPipeline&, UsdGenExecutionPipeline::Cancellation, Completion);
    // Thread-safe after Start returns. Coalesced real completion notifications.
    // If both reserved owner routes fail, retain the entire job permanently
    // and report LostProof thread-neutrally. That exceptional callback must
    // not perform queue work or destroy native ownership.
    bool NotifyCompletion();
    // Thread-neutral failure ingress used by the completion service's owner route.
    void NotifyFailure(VkResult) noexcept;
    void SuppressPublication() noexcept { suppressed_.store(true, std::memory_order_release); }
    State state() const noexcept { return state_.load(std::memory_order_acquire); }
private:
    struct Control;
    struct Relay;
    struct PrepareRelay;
    explicit VulkanSourceWidthJob(CreateInfo);
    void Advance();
    void BeginPreparation();
    void TakePrepared(std::shared_ptr<PrepareRelay>) noexcept;
    void FinishWidth();
    void BeginWidth();
    void BeginLength();
    void FinishLength();
    void BeginNoise();
    void FinishNoise();
    void BeginCull(bool scatter = false);
    void FinishCull(bool scatter);
    void BeginStage();
    void FinishStage(std::shared_ptr<const VulkanSourceGeneration>);
    void BeginBlend();
    void BeginCompare();
    void FinishCompare();
    void FinishBlend();
    void FailOnOwner(VkResult = VK_ERROR_DEVICE_LOST);
    void RouteFailure(VkResult = VK_ERROR_DEVICE_LOST) noexcept;
    bool Suppressed() const noexcept;
    void Terminal(std::shared_ptr<const VulkanSourceGeneration>, State, VkResult) noexcept;
    CreateInfo info_;
    std::shared_ptr<VulkanSourceUpload> upload_;
    std::shared_ptr<const VulkanSourceGeneration> base_;
    std::unique_ptr<WidthPipeline::Candidate> width_;
    std::unique_ptr<LengthScalePipeline::Candidate> length_;
    std::unique_ptr<LengthCompactionPipeline::Candidate> cull_;
    std::unique_ptr<WidthBlendPipeline::Candidate> blend_;
    std::unique_ptr<NonWidthComparePipeline::Candidate> compare_;
    std::unique_ptr<NoisePipeline::Candidate> noise_;
    std::vector<std::shared_ptr<const VulkanSourceGeneration>> values_;
    size_t stageIndex_ = 0;
    State stageFrom_ = State::UploadPending;
    UsdGenExecutionPipeline* owner_=nullptr;
    std::shared_ptr<Control> control_;
    std::unique_ptr<VulkanCompletionService::Watch> watch_;
    bool watchProofReady_ = false;
    UsdGenExecutionPipeline::Cancellation cancellation_;
    std::shared_ptr<Relay> relay_;
    std::shared_ptr<PrepareRelay> prepareRelay_;
    std::shared_ptr<const VulkanPreparedSource> prepared_;
    std::atomic<State> state_{State::Created};
    std::atomic<bool> suppressed_{false}, started_{false};
};
} // namespace usdGen::vulkan
#endif
