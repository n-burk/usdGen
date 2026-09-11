#ifdef USDGEN_ENABLE_CUDA
#include "usdGen/gpu/generation.h"
#include "usdGen/gpu/curveCompaction.h"

#include <map>
#include <mutex>
#include <atomic>

namespace usdGen::gpu {
namespace {
using Status = UsdGenDeviceStatus;

class SourceOwner final : public UsdGenDeviceOwner {
public:
    SourceOwner(std::unique_ptr<CudaCurveSource> source, int device,
                std::unique_ptr<DeviceBuffer<float>> widths,
                std::unique_ptr<DeviceBuffer<float3>> points,
                std::unique_ptr<CudaCurveCompaction> compacted = {})
        : source_(std::move(source)), compacted_(std::move(compacted)),
          widths_(std::move(widths)), points_(std::move(points)), device_(device) {}
    ~SourceOwner() override {
        int previous = -1;
        cudaGetDevice(&previous);
        if (quarantined_.load() || cudaSetDevice(device_) != cudaSuccess) {
            // Device failure makes completion unprovable. Quarantine instead
            // of freeing storage that a consumer could still be touching.
            // Driver/context teardown is the recovery boundary for this case.
            source_.release();
            compacted_.release();
            widths_.release();
            points_.release();
            return;
        }
        // Leases keep this owner alive through ReleaseConsumer. No outstanding
        // consumer may remain when its last strong reference is destroyed.
        source_.reset();
        compacted_.reset();
        widths_.reset();
        points_.reset();
        if (previous >= 0 && previous != device_) cudaSetDevice(previous);
    }
    bool ProducerReady() const noexcept override {
        return !quarantined_.load() &&
            ((source_ && source_->generation() && !source_->pending()) ||
             (compacted_ && compacted_->generation() && !compacted_->pending()));
    }
    Status AcquireConsumer(UsdGenDeviceStream stream, uint64_t* token) const noexcept override {
        int device = -1;
        int streamDevice = -1;
        if (!token || !ProducerReady() || cudaGetDevice(&device) != cudaSuccess || device != device_)
            return Status::ConsumerRejected;
        if (cudaStreamGetDevice(reinterpret_cast<cudaStream_t>(stream), &streamDevice) != cudaSuccess || streamDevice != device_)
            return Status::ConsumerRejected;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (nextToken_ == UINT64_MAX) return Status::ConsumerRejected;
            const uint64_t next = ++nextToken_;
            consumers_.emplace(next, stream);
            *token = next;
            return Status::Ok;
        } catch (...) { return Status::ConsumerRejected; }
    }
    Status WaitForProducer(UsdGenDeviceStream stream, uint64_t token) const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = consumers_.find(token);
        if (it == consumers_.end() || it->second != stream) return Status::InvalidLease;
        int device = -1;
        if (cudaGetDevice(&device) != cudaSuccess || device != device_)
            return Status::ConsumerRejected;
        auto nativeStream = reinterpret_cast<cudaStream_t>(stream);
        if ((source_ && source_->waitOn(nativeStream) != CurveSourceStatus::Ok) ||
            (compacted_ && compacted_->waitOn(nativeStream) != cudaSuccess) ||
            (widths_ && widths_->waitOn(nativeStream) != cudaSuccess) ||
            (points_ && points_->waitOn(nativeStream) != cudaSuccess))
            return Status::SynchronizationFailed;
        return Status::Ok;
    }
    void ReleaseConsumer(UsdGenDeviceStream stream, uint64_t token) const noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = consumers_.find(token);
            if (it == consumers_.end() || it->second != stream) return;
        }
        int previous = -1;
        cudaGetDevice(&previous);
        const bool selected = cudaSetDevice(device_) == cudaSuccess;
        // Correctness-first reclamation. This can later be retired by an event
        // queue; simply recording an event then freeing the owner is unsafe.
        if (!selected || cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream)) != cudaSuccess)
            quarantined_.store(true);
        if (previous >= 0 && previous != device_) cudaSetDevice(previous);
        std::lock_guard<std::mutex> lock(mutex_);
        consumers_.erase(token);
    }
    DeviceView<const float> HairT() const noexcept { return compacted_ ? compacted_->hairT() : source_->hairT(); }
    DeviceView<const int32_t> RootPrim() const noexcept { return compacted_ ? compacted_->rootPrim() : source_->rootPrim(); }
    DeviceView<const float2> RootUV() const noexcept { return compacted_ ? compacted_->rootUV() : source_->rootUV(); }
    DeviceCurveGeometryView Geometry() const noexcept {
        auto view = compacted_ ? compacted_->view() : source_->view();
        if (widths_) view.widths = {widths_->data(), widths_->size()};
        if (points_) view.points = {points_->data(), points_->size()};
        return view;
    }
private:
    std::unique_ptr<CudaCurveSource> source_;
    std::unique_ptr<CudaCurveCompaction> compacted_;
    std::unique_ptr<DeviceBuffer<float>> widths_;
    std::unique_ptr<DeviceBuffer<float3>> points_;
    int device_;
    mutable std::mutex mutex_;
    mutable uint64_t nextToken_ = 0;
    mutable std::map<uint64_t, UsdGenDeviceStream> consumers_;
    mutable std::atomic<bool> quarantined_{false};
};
} // namespace

static std::shared_ptr<const UsdGenDeviceGeneration> MakeGeneration(
    std::unique_ptr<CudaCurveSource> source, uint64_t generation, std::string* reason,
    bool alreadyDeformed, std::unique_ptr<DeviceBuffer<float>> widths,
    std::unique_ptr<DeviceBuffer<float3>> points, std::unique_ptr<CudaCurveCompaction> compacted) {
    int device = -1;
    if (bool(source) == bool(compacted) || cudaGetDevice(&device) != cudaSuccess ||
        (source && (source->pending() || !source->generation() || source->deviceIndex() != device)) ||
        (compacted && (compacted->pending() || !compacted->generation() || compacted->deviceIndex() != device))) {
        if (reason) *reason = "CUDA source is not a completed generation";
        return {};
    }
    auto view = compacted ? compacted->view() : source->view();
    auto hairT = compacted ? compacted->hairT() : source->hairT();
    if (view.points.size != view.pointCount || view.restPoints.size != view.pointCount ||
        view.widths.size != view.pointCount || view.stableIds.size != view.curveCount ||
        view.curveOffsets.size != view.curveCount + 1 || hairT.size != view.pointCount ||
        !view.points || !view.restPoints || !view.widths || !view.stableIds || !view.curveOffsets || !hairT) {
        if (reason) *reason = "CUDA geometry revision is missing required C3 channels";
        return {};
    }
    if (widths) {
        cudaPointerAttributes attributes{};
        if (widths->size() != view.pointCount ||
            (widths->size() && (cudaPointerGetAttributes(&attributes, widths->data()) != cudaSuccess ||
              attributes.type != cudaMemoryTypeDevice || attributes.device != device))) {
            if (reason) *reason = "CUDA width revision has incompatible shape or device";
            return {};
        }
    }
    if (points) {
        cudaPointerAttributes attributes{};
        if (points->size() != view.pointCount ||
            (points->size() && (cudaPointerGetAttributes(&attributes, points->data()) != cudaSuccess ||
              attributes.type != cudaMemoryTypeDevice || attributes.device != device))) {
            if (reason) *reason = "CUDA point revision has incompatible shape or device";
            return {};
        }
    }
    UsdGenDeviceGeneration::CreateInfo info;
    info.identity = {UsdGenDeviceBackend::Cuda, device, generation};
    info.geometry = {generation, generation, view.curveCount, view.pointCount, {}};
    info.geometry.alreadyDeformed = alreadyDeformed;
    using Type = UsdGenDeviceValueType;
    using Domain = UsdGenDeviceDomain;
    using Semantic = UsdGenDeviceChannelSemantic;
    auto channel = [&](char const* name, Type type, Domain domain,
                       size_t count, unsigned arity, unsigned stride, Semantic semantic) {
        info.channels.push_back({name, type, domain, count, arity, stride, true, semantic});
    };
    channel("points", Type::Float32x3, Domain::Point, view.pointCount, 3, sizeof(float3), Semantic::Points);
    channel("rest", Type::Float32x3, Domain::Point, view.pointCount, 3, sizeof(float3), Semantic::RestPoints);
    channel("widths", Type::Float32, Domain::Point, view.pointCount, 1, sizeof(float), Semantic::Widths);
    channel("hairT", Type::Float32, Domain::Point, view.pointCount, 1, sizeof(float), Semantic::HairT);
    // Offsets are topology storage (C+1), not a per-primitive value (C).
    channel("curveOffsets", Type::UInt32, Domain::Topology, view.curveOffsets.size, 1, sizeof(uint32_t), Semantic::CurveOffsets);
    channel("curveId", Type::UInt64, Domain::Primitive, view.curveCount, 1, sizeof(uint64_t), Semantic::StableIds);
    if ((compacted ? compacted->rootPrim() : source->rootPrim()).size)
        channel("skinprim", Type::Int32, Domain::Primitive, view.curveCount, 1, sizeof(int32_t), Semantic::RootPrim);
    if ((compacted ? compacted->rootUV() : source->rootUV()).size)
        channel("skinprimuv", Type::Float32x2, Domain::Primitive, view.curveCount, 2, sizeof(float2), Semantic::RootUV);
    info.owner = std::make_shared<SourceOwner>(std::move(source), device, std::move(widths), std::move(points), std::move(compacted));
    return UsdGenDeviceGeneration::Create(std::move(info), reason);
}

std::shared_ptr<const UsdGenDeviceGeneration> MakeSourceGeneration(
    std::unique_ptr<CudaCurveSource> source, uint64_t generation, std::string* reason,
    bool alreadyDeformed, std::unique_ptr<DeviceBuffer<float>> widths,
    std::unique_ptr<DeviceBuffer<float3>> points) {
    return MakeGeneration(std::move(source), generation, reason, alreadyDeformed,
        std::move(widths), std::move(points), {});
}

std::shared_ptr<const UsdGenDeviceGeneration> MakeCompactedGeneration(
    std::unique_ptr<CudaCurveCompaction> compacted, uint64_t generation, std::string* reason,
    bool alreadyDeformed, std::unique_ptr<DeviceBuffer<float>> widths,
    std::unique_ptr<DeviceBuffer<float3>> points) {
    return MakeGeneration({}, generation, reason, alreadyDeformed,
        std::move(widths), std::move(points), std::move(compacted));
}

CudaGeometryLease AcquireGeometry(
    std::shared_ptr<const UsdGenDeviceGeneration> const& generation, cudaStream_t stream) {
    CudaGeometryLease result;
    if (!generation || generation->Identity().backend != UsdGenDeviceBackend::Cuda) return result;
    auto owner = std::dynamic_pointer_cast<SourceOwner const>(generation->Owner());
    if (!owner) return result;
    auto lease = generation->AcquireLease(reinterpret_cast<UsdGenDeviceStream>(stream));
    if (!lease || lease.WaitUntilReady() != Status::Ok) return result;
    result.geometry_ = owner->Geometry();
    result.hairT_ = owner->HairT();
    result.rootPrim_ = owner->RootPrim();
    result.rootUV_ = owner->RootUV();
    result.lease_ = std::move(lease);
    return result;
}
} // namespace usdGen::gpu
#endif
