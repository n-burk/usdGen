#include "usdGen/cudaExecutionQueue.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace usdGen {

struct UsdGenCudaExecutionQueue::Impl {
    // Declaration order is intentional: the pipeline drains before the
    // workspace is destroyed, and the workspace owns all mutable CUDA state.
    std::unique_ptr<UsdGenCudaExecutionWorkspace> workspace;
    std::shared_ptr<const UsdGenCudaQueueSnapshot> snapshot;
    std::unique_ptr<Pipeline> pipeline;

    Impl(UsdGenExecutionRuntime& runtime, int device,
         UsdGenDiagnostics* diagnostics)
        : workspace(CreateCudaExecutionWorkspace(device, diagnostics)),
          pipeline(std::make_unique<Pipeline>(runtime)) {}
};

UsdGenCudaExecutionQueue::UsdGenCudaExecutionQueue(
    UsdGenExecutionRuntime& runtime, int device, UsdGenDiagnostics* diagnostics)
    : impl_(std::make_unique<Impl>(runtime, device, diagnostics)) {}

UsdGenCudaExecutionQueue::~UsdGenCudaExecutionQueue() = default;

bool UsdGenCudaExecutionQueue::Valid() const noexcept {
    return impl_ && impl_->workspace && impl_->pipeline;
}

uint64_t UsdGenCudaExecutionQueue::Submit(
    std::shared_ptr<const UsdGenCudaExecutionPlan> plan, double frame,
    Completion completion) {
    if (!Valid() || !plan || !std::isfinite(frame)) return 0;

    auto diagnostics = std::make_shared<UsdGenDiagnostics>();
    auto* impl = impl_.get();
    auto work = [impl, plan = std::move(plan), frame, diagnostics]
                (Pipeline::Cancellation const& cancel) -> Pipeline::Publish {
        // Cancellation is checked by the pipeline before publication.  Once
        // device work starts, execute it to completion so workspace buffers
        // and CUDA events remain valid; a superseding epoch discards the
        // resulting publication action in the owner node.
        auto previousSnapshot = std::atomic_load_explicit(
            &impl->snapshot, std::memory_order_acquire);
        auto previous = previousSnapshot ? previousSnapshot->generation
                                         : std::shared_ptr<const UsdGenDeviceGeneration>{};
        auto generation = ExecuteCudaGraph(*plan, *impl->workspace, frame,
                                           cancel.epoch, diagnostics.get(), previous);
        if (!generation) {
            if (!diagnostics->HasErrors())
                diagnostics->Error("CUDA execution produced no generation");
            throw std::runtime_error("CUDA execution failed");
        }
        auto bindings = GetCudaBindingStats(*impl->workspace);
        auto next = std::make_shared<UsdGenCudaQueueSnapshot>();
        next->epoch = cancel.epoch;
        next->frame = frame;
        next->generation = std::move(generation);
        next->bindings = std::move(bindings);
        return [impl, next = std::move(next)] {
            std::atomic_store_explicit(&impl->snapshot,
                                       std::shared_ptr<const UsdGenCudaQueueSnapshot>(next),
                                       std::memory_order_release);
        };
    };
    auto callback = [diagnostics, completion = std::move(completion)](
                        uint64_t epoch, Pipeline::Outcome outcome,
                        std::exception_ptr error) {
        if (!diagnostics->HasErrors() && error &&
            outcome == Pipeline::Outcome::Failed)
            diagnostics->Error("CUDA execution request failed");
        if (completion) completion(epoch, outcome, *diagnostics);
    };
    return impl_->pipeline->Submit(std::move(work), std::move(callback));
}

uint64_t UsdGenCudaExecutionQueue::CancelPending() {
    return Valid() ? impl_->pipeline->CancelPending() : 0;
}

std::shared_ptr<const UsdGenCudaQueueSnapshot>
UsdGenCudaExecutionQueue::Snapshot() const noexcept {
    if (!impl_) return {};
    return std::atomic_load_explicit(&impl_->snapshot,
                                     std::memory_order_acquire);
}

void UsdGenCudaExecutionQueue::Drain() {
    if (!Valid()) return;
    impl_->pipeline->Drain();
}

} // namespace usdGen
