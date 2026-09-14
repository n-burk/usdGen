#include "usdGen/sessionBackendExecutor.h"

#include <stdexcept>
#include <utility>

namespace usdGen {
namespace {

class SessionBackendExecutor final : public UsdGenSessionBackendExecutor {
public:
    SessionBackendExecutor(int threadLimit, size_t cacheBytes,
                           std::shared_ptr<UsdGenExecutionCacheDomain> domain,
                           std::shared_ptr<UsdGenSessionDeviceProvider> provider)
        : _cooker(threadLimit, cacheBytes, std::move(domain)), _provider(std::move(provider)) {
        _cooker.SetDeviceProvider(_provider);
    }

    bool CanRoute(UsdGenExecutionBackend backend) const noexcept override {
        if (backend == UsdGenExecutionBackend::CpuReference) return true;
        if (backend == UsdGenExecutionBackend::Vulkan) return bool(_provider);
#ifdef USDGEN_ENABLE_CUDA
        if (backend == UsdGenExecutionBackend::Cuda) return true;
#endif
        return false;
    }

    void Submit(UsdGenSessionBackendRequest request) override {
        auto fail = [&request](std::exception_ptr error) {
            if (request.completion) request.completion(request.previous, std::move(error));
        };
        try {
            if (!request.desc) {
                fail(std::make_exception_ptr(std::runtime_error(
                    "Session backend request has no graph description")));
                return;
            }
            // Context-loss ordering is Session semantics, but belongs on this
            // executor's serial work lane with the workspace it can retire.
            for (UsdGenDeviceContextLoss const& loss : request.contextLosses) {
                if (!_cooker.ApplyDeviceContextLoss(loss)) {
                    fail(std::make_exception_ptr(std::runtime_error(
                        "device context-loss invalidation failed")));
                    return;
                }
            }

            UsdGenExecutionBackend const backend = request.desc
                ? request.desc->executionBackend : UsdGenExecutionBackend::Invalid;
            if (!CanRoute(backend)) {
                // Preserve the existing precise compiler validation and
                // retained-previous publication behavior.  Crucially, no
                // native or CPU scheduler execution starts for this backend.
                auto generation = _cooker.Cook(std::move(request.desc), request.context,
                    request.devicePublicationEnabled, std::move(request.pending),
                    request.frame, request.reason, std::move(request.previous),
                    std::move(request.publishedStats), request.invalidateValues,
                    request.previousPublishedWorkerEpoch, request.cancellation.epoch,
                    request.callerDevice, std::move(request.coalescedHooks));
                if (request.completion) request.completion(std::move(generation), {});
                return;
            }

            if (backend == UsdGenExecutionBackend::Vulkan) {
                if (!request.runtime || !request.deviceReturnBinder)
                    throw std::runtime_error("injected device Session executor has no runtime/return route");
                auto completion = request.completion;
                _cooker.CookDeviceAsync(*request.runtime, request.cancellation,
                    std::move(request.desc), request.context,
                    request.devicePublicationEnabled, std::move(request.pending),
                    request.frame, request.reason, std::move(request.previous),
                    std::move(request.publishedStats), request.invalidateValues,
                    request.previousPublishedWorkerEpoch, request.callerDevice,
                    _provider, std::move(request.deviceReturnBinder),
                    std::move(completion), std::move(request.coalescedHooks));
                return;
            }

            if (backend == UsdGenExecutionBackend::Cuda) {
                if (!request.runtime)
                    throw std::runtime_error("CUDA Session executor has no runtime");
                // Keep request.completion intact for the catch path: a
                // synchronous setup exception must still release Pipeline's
                // async gate exactly once.
                auto completion = request.completion;
                _cooker.CookCudaAsync(*request.runtime, request.cancellation,
                    std::move(request.desc), request.context,
                    request.devicePublicationEnabled, std::move(request.pending),
                    request.frame, request.reason, std::move(request.previous),
                    std::move(request.publishedStats), request.invalidateValues,
                    request.previousPublishedWorkerEpoch, request.callerDevice,
                    std::move(completion), std::move(request.coalescedHooks));
                return;
            }

            auto generation = _cooker.Cook(std::move(request.desc), request.context,
                request.devicePublicationEnabled, std::move(request.pending),
                request.frame, request.reason, std::move(request.previous),
                std::move(request.publishedStats), request.invalidateValues,
                request.previousPublishedWorkerEpoch, request.cancellation.epoch,
                request.callerDevice, std::move(request.coalescedHooks));
            if (_cooker.GetCoalescedRole() ==
                    UsdGenSessionCooker::CoalescedRole::Follower ||
                _cooker.GetCoalescedRole() ==
                    UsdGenSessionCooker::CoalescedRole::Resident)
                return;
            if (request.completion) request.completion(std::move(generation), {});
        } catch (...) {
            fail(std::current_exception());
        }
    }

    void Shutdown() noexcept override {
        // The owner guarantees that every async CUDA completion has returned
        // before this object is destroyed.  Coalesced followers are cancelled
        // by Session before pipeline shutdown; silence a leftover registration
        // without allowing a callback to reenter a dying command owner.
        _cooker.AbandonCoalesced(false);
    }

    UsdGenSessionCooker &Cooker() noexcept override { return _cooker; }
    UsdGenSessionCooker const &Cooker() const noexcept override { return _cooker; }

private:
    UsdGenSessionCooker _cooker;
    std::shared_ptr<UsdGenSessionDeviceProvider> _provider;
};

} // namespace

std::shared_ptr<UsdGenSessionBackendExecutor> CreateUsdGenSessionBackendExecutor(
    int threadLimit, size_t executionCacheBytes,
    std::shared_ptr<UsdGenExecutionCacheDomain> executionCacheDomain,
    std::shared_ptr<UsdGenSessionDeviceProvider> deviceProvider)
{
    return std::make_shared<SessionBackendExecutor>(threadLimit,
        executionCacheBytes, std::move(executionCacheDomain), std::move(deviceProvider));
}

} // namespace usdGen
