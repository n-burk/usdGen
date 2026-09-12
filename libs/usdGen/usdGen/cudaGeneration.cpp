#ifdef USDGEN_ENABLE_CUDA
#include "usdGen/gpu/generation.h"
#include "usdGen/gpu/curveCompaction.h"

#include <atomic>
#include <limits>
#include <memory>
#include <utility>

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
    SourceOwner(std::shared_ptr<const UsdGenDeviceGeneration> base, int device,
                std::unique_ptr<DeviceBuffer<float3>> points)
        : base_(std::move(base)), points_(std::move(points)), device_(device) {}
    ~SourceOwner() override;
    bool ProducerReady() const noexcept override;
    std::unique_ptr<UsdGenDeviceConsumer> AcquireConsumer(
        UsdGenDeviceStream stream) const noexcept override;
    DeviceView<const float> HairT() const noexcept {
        return base_ ? BaseOwner()->HairT() : compacted_ ? compacted_->hairT() : source_->hairT();
    }
    DeviceView<const int32_t> RootPrim() const noexcept {
        return base_ ? BaseOwner()->RootPrim() : compacted_ ? compacted_->rootPrim() : source_->rootPrim();
    }
    DeviceView<const float2> RootUV() const noexcept {
        return base_ ? BaseOwner()->RootUV() : compacted_ ? compacted_->rootUV() : source_->rootUV();
    }
    DeviceCurveGeometryView Geometry() const noexcept {
        auto view = base_ ? BaseOwner()->Geometry() : compacted_ ? compacted_->view() : source_->view();
        if (widths_) view.widths = {widths_->data(), widths_->size()};
        if (points_) view.points = {points_->data(), points_->size()};
        return view;
    }
private:
    class Consumer;
    SourceOwner const* BaseOwner() const noexcept {
        return static_cast<SourceOwner const*>(base_->Owner().get());
    }
    std::shared_ptr<const UsdGenDeviceGeneration> base_;
    std::unique_ptr<CudaCurveSource> source_;
    std::unique_ptr<CudaCurveCompaction> compacted_;
    std::unique_ptr<DeviceBuffer<float>> widths_;
    std::unique_ptr<DeviceBuffer<float3>> points_;
    int device_;
    mutable std::atomic<bool> quarantined_{false};
};

class SourceOwner::Consumer final : public UsdGenDeviceConsumer {
public:
    Consumer(SourceOwner const* owner, UsdGenDeviceStream stream,
             UsdGenDeviceLease parent)
        : owner_(owner), stream_(stream), parent_(std::move(parent)) {}

    ~Consumer() override { Complete(); }

    UsdGenDeviceStream Stream() const noexcept override { return stream_; }

    Status WaitUntilReady() const noexcept override {
        if (!owner_ || completed_.load()) return Status::InvalidLease;
        if (owner_->quarantined_.load()) return Status::ConsumerRejected;

        int current = -1;
        if (cudaGetDevice(&current) != cudaSuccess || current != owner_->device_)
            return Status::ConsumerRejected;
        if (stream_ != 0) {
            int streamDevice = -1;
            if (cudaStreamGetDevice(reinterpret_cast<cudaStream_t>(stream_),
                                    &streamDevice) != cudaSuccess ||
                streamDevice != owner_->device_)
                return Status::ConsumerRejected;
        }

        if (parent_ && parent_.WaitUntilReady() != Status::Ok)
            return Status::SynchronizationFailed;
        cudaStream_t const native = reinterpret_cast<cudaStream_t>(stream_);
        if ((owner_->source_ && owner_->source_->waitOn(native) != CurveSourceStatus::Ok) ||
            (owner_->compacted_ && owner_->compacted_->waitOn(native) != cudaSuccess) ||
            (owner_->widths_ && owner_->widths_->waitOn(native) != cudaSuccess) ||
            (owner_->points_ && owner_->points_->waitOn(native) != cudaSuccess))
            return Status::SynchronizationFailed;
        return Status::Ok;
    }

    void Complete() noexcept override {
        bool expected = false;
        if (!completed_.compare_exchange_strong(expected, true)) return;

        int previous = -1;
        bool const havePrevious = cudaGetDevice(&previous) == cudaSuccess;
        bool const selected = havePrevious &&
                              cudaSetDevice(owner_->device_) == cudaSuccess;
        bool const fenced = selected &&
                            cudaStreamSynchronize(
                                reinterpret_cast<cudaStream_t>(stream_)) == cudaSuccess;
        if (!fenced) owner_->quarantined_.store(true);
        if (havePrevious && previous != owner_->device_) cudaSetDevice(previous);

        // Keep parent allocations alive until this consumer stream is fenced.
        parent_.Complete();
        parent_ = {};
    }

private:
    SourceOwner const* owner_ = nullptr; // State owns the corresponding owner.
    UsdGenDeviceStream stream_ = 0;
    UsdGenDeviceLease parent_;
    std::atomic<bool> completed_{false};
};

SourceOwner::~SourceOwner()
{
    int previous = -1;
    bool const havePrevious = cudaGetDevice(&previous) == cudaSuccess;
    bool const selected = havePrevious && cudaSetDevice(device_) == cudaSuccess;
    if (quarantined_.load() || !selected) {
        // A failed device selection means allocation completion cannot be
        // proven. Abandon every CUDA handle; context teardown is the recovery
        // boundary. DeviceBuffer::quarantine prevents a wrong-device free.
        if (widths_) widths_->quarantine();
        if (points_) points_->quarantine();
        source_.release();
        compacted_.release();
        widths_.release();
        points_.release();
        if (selected && previous != device_) cudaSetDevice(previous);
        return;
    }
    source_.reset();
    compacted_.reset();
    widths_.reset();
    points_.reset();
    if (previous != device_) cudaSetDevice(previous);
}

bool SourceOwner::ProducerReady() const noexcept
{
    return !quarantined_.load() &&
        ((source_ && source_->generation() && !source_->pending()) ||
         (compacted_ && compacted_->generation() && !compacted_->pending()) ||
         (base_ && base_->Owner()->ProducerReady()));
}

std::unique_ptr<UsdGenDeviceConsumer>
SourceOwner::AcquireConsumer(UsdGenDeviceStream stream) const noexcept
{
    int device = -1;
    if (!ProducerReady() || cudaGetDevice(&device) != cudaSuccess || device != device_)
        return {};
    // Zero denotes the default stream and is associated with the current
    // device. cudaStreamGetDevice(nullptr) is not portable, so only query
    // explicitly supplied streams.
    if (stream != 0) {
        int streamDevice = -1;
        if (cudaStreamGetDevice(reinterpret_cast<cudaStream_t>(stream),
                                &streamDevice) != cudaSuccess ||
            streamDevice != device_)
            return {};
    }
    try {
        UsdGenDeviceLease parentLease = base_ ? base_->AcquireLease(stream)
                                              : UsdGenDeviceLease{};
        if (base_ && !parentLease) return {};
        return std::make_unique<Consumer>(this, stream, std::move(parentLease));
    } catch (...) {
        return {};
    }
}
} // namespace

static std::shared_ptr<const UsdGenDeviceGeneration> MakeGeneration(
    std::unique_ptr<CudaCurveSource> source, uint64_t generation, std::string* reason,
    bool alreadyDeformed, std::unique_ptr<DeviceBuffer<float>> widths,
    std::unique_ptr<DeviceBuffer<float3>> points, std::unique_ptr<CudaCurveCompaction> compacted,
    uint64_t topologyVersion) {
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
    info.geometry.topologyVersion = generation;
    info.geometry.valueVersion = generation;
    info.geometry.curveCount = view.curveCount;
    info.geometry.pointCount = view.pointCount;
    if (topologyVersion != UINT64_MAX) info.geometry.topologyVersion = topologyVersion;
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
    std::unique_ptr<DeviceBuffer<float3>> points, uint64_t topologyVersion) {
    return MakeGeneration(std::move(source), generation, reason, alreadyDeformed,
        std::move(widths), std::move(points), {}, topologyVersion);
}

std::shared_ptr<const UsdGenDeviceGeneration> MakeCompactedGeneration(
    std::unique_ptr<CudaCurveCompaction> compacted, uint64_t generation, std::string* reason,
    bool alreadyDeformed, std::unique_ptr<DeviceBuffer<float>> widths,
    std::unique_ptr<DeviceBuffer<float3>> points, uint64_t topologyVersion) {
    return MakeGeneration({}, generation, reason, alreadyDeformed,
        std::move(widths), std::move(points), std::move(compacted), topologyVersion);
}

std::shared_ptr<const UsdGenDeviceGeneration> MakePointRevisionGeneration(
    std::shared_ptr<const UsdGenDeviceGeneration> base, uint64_t generation,
    std::unique_ptr<DeviceBuffer<float3>> points, UsdGenDeviceToolMetadata tool,
    std::string* reason) {
    int device = -1;
    if (!base || base->Identity().backend != UsdGenDeviceBackend::Cuda ||
        !std::dynamic_pointer_cast<SourceOwner const>(base->Owner()) ||
        !base->Owner()->ProducerReady() || cudaGetDevice(&device) != cudaSuccess ||
        device != base->Identity().deviceIndex || generation <= base->Identity().generation) {
        if (reason) *reason = "CUDA point edit requires a completed local base and newer generation";
        return {};
    }
    cudaPointerAttributes attributes{};
    if (points && (points->size() != base->Geometry().pointCount ||
        (points->size() && (cudaPointerGetAttributes(&attributes, points->data()) != cudaSuccess ||
         attributes.type != cudaMemoryTypeDevice || attributes.device != device)))) {
        if (reason) *reason = "CUDA point edit has incompatible shape or device";
        return {};
    }
    UsdGenDeviceGeneration::CreateInfo info;
    info.identity = {UsdGenDeviceBackend::Cuda, device, generation};
    info.geometry = base->Geometry();
    info.geometry.valueVersion = generation;
    if (points) {
        for (UsdGenDeviceTileMetadata &tile : info.geometry.tiles) {
            tile.boundsValid = false;
        }
    }
    info.channels = base->Channels();
    info.tool = std::move(tool);
    info.owner = std::make_shared<SourceOwner>(std::move(base), device, std::move(points));
    return UsdGenDeviceGeneration::Create(std::move(info), reason);
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

namespace {
template <class T>
bool _Slice(DeviceView<const T> source, uint64_t begin, uint64_t count,
            DeviceView<const T>* result)
{
    if (begin > source.size || count > source.size - begin ||
        begin > std::numeric_limits<size_t>::max() ||
        count > std::numeric_limits<size_t>::max()) return false;
    size_t const b = static_cast<size_t>(begin);
    size_t const n = static_cast<size_t>(count);
    if (n && !source.data) return false;
    result->data = n ? source.data + b : nullptr;
    result->size = n;
    return true;
}
}

CudaGeometryTileLease AcquireGeometryTile(
    std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
    uint32_t tileId, uint64_t expectedGeneration, cudaStream_t stream)
{
    CudaGeometryTileLease result;
    if (!generation || generation->Identity().backend != UsdGenDeviceBackend::Cuda ||
        generation->Identity().generation != expectedGeneration)
        return result;

    UsdGenDeviceTileMetadata const* tile = nullptr;
    for (UsdGenDeviceTileMetadata const& candidate : generation->Geometry().tiles) {
        if (candidate.tile == tileId) { tile = &candidate; break; }
    }
    if (!tile) return result;

    CudaGeometryLease parent = AcquireGeometry(generation, stream);
    if (!parent) return result;
    DeviceCurveGeometryView const geometry = parent.Geometry();
    DeviceView<const uint32_t> offsets{geometry.curveOffsets.data,
                                       geometry.curveOffsets.size};
    CudaGeometryTileView view;
    view.range = *tile;
    view.generation = generation->Identity().generation;
    view.topologyVersion = generation->Geometry().topologyVersion;
    view.valueVersion = generation->Geometry().valueVersion;

    // The extra offset sentinel is part of every non-empty and empty tile.
    if (tile->curveCount > tile->pointCount ||
        (tile->curveCount == 0 && tile->pointCount != 0) ||
        tile->firstCurve > geometry.curveCount ||
        tile->curveCount > geometry.curveCount - tile->firstCurve ||
        tile->curveCount == std::numeric_limits<uint64_t>::max() ||
        tile->firstPoint > std::numeric_limits<uint32_t>::max() ||
        tile->pointCount > std::numeric_limits<uint32_t>::max() - tile->firstPoint)
        return result;
    uint64_t const offsetCount = tile->curveCount + 1;
    if (!_Slice(offsets, tile->firstCurve, offsetCount, &view.curveOffsets) ||
        !_Slice(parent.HairT(), tile->firstPoint, tile->pointCount, &view.hairT) ||
        !_Slice(geometry.points, tile->firstPoint, tile->pointCount, &view.points) ||
        !_Slice(geometry.restPoints, tile->firstPoint, tile->pointCount, &view.restPoints) ||
        !_Slice(geometry.widths, tile->firstPoint, tile->pointCount, &view.widths) ||
        !_Slice(geometry.stableIds, tile->firstCurve, tile->curveCount, &view.stableIds))
        return result;

    DeviceView<const int32_t> const roots = parent.RootPrim();
    DeviceView<const float2> const uvs = parent.RootUV();
    if ((roots.size == 0) != (uvs.size == 0)) return result;
    if (roots.size == 0) view.rootPrim = {};
    else if (!_Slice(roots, tile->firstCurve, tile->curveCount, &view.rootPrim)) return result;
    if (uvs.size == 0) view.rootUV = {};
    else if (!_Slice(uvs, tile->firstCurve, tile->curveCount, &view.rootUV)) return result;
    result.view_ = view;
    result.parent_ = std::move(parent);
    return result;
}

std::shared_ptr<const UsdGenDeviceGeneration> WithTileMetadata(
    std::shared_ptr<const UsdGenDeviceGeneration> const& candidate,
    std::vector<UsdGenDeviceTileMetadata> tiles, std::string* reason,
    UsdGenDeviceCurveTopologyMetadata curveTopology)
{
    if (!candidate || !candidate->Owner()) {
        if (reason) *reason = "tile metadata requires an owned generation";
        return {};
    }
    auto const& geometry = candidate->Geometry();
    uint64_t curveEnd = 0, pointEnd = 0, previousId = 0;
    bool first = true;
    for (UsdGenDeviceTileMetadata const& tile : tiles) {
        if (!first && tile.tile <= previousId) {
            if (reason) *reason = "tile IDs must be strictly increasing";
            return {};
        }
        first = false;
        previousId = tile.tile;
        if (tile.curveCount > tile.pointCount ||
            (tile.curveCount == 0 && tile.pointCount != 0) ||
            tile.firstPoint > std::numeric_limits<uint32_t>::max() ||
            tile.pointCount > std::numeric_limits<uint32_t>::max() - tile.firstPoint ||
            tile.firstCurve != curveEnd || tile.firstPoint != pointEnd ||
            tile.curveCount > geometry.curveCount - curveEnd ||
            tile.pointCount > geometry.pointCount - pointEnd ||
            tile.curveCount > std::numeric_limits<uint64_t>::max() - curveEnd ||
            tile.pointCount > std::numeric_limits<uint64_t>::max() - pointEnd) {
            if (reason) *reason = "tile ranges must be contiguous and in bounds";
            return {};
        }
        curveEnd += tile.curveCount;
        pointEnd += tile.pointCount;
    }
    if (tiles.empty()) {
        if (geometry.curveCount != 0 || geometry.pointCount != 0) {
            if (reason) *reason = "non-empty geometry requires tile metadata";
            return {};
        }
    } else if (curveEnd != geometry.curveCount || pointEnd != geometry.pointCount) {
        if (reason) *reason = "tile metadata does not cover geometry";
        return {};
    }
    UsdGenDeviceGeneration::CreateInfo info;
    info.identity = candidate->Identity();
    info.geometry = geometry;
    info.geometry.tiles = std::move(tiles);
    if (curveTopology.type != UsdGenDeviceCurveType::Unknown ||
        curveTopology.basis != UsdGenDeviceCurveBasis::Unknown ||
        curveTopology.wrap != UsdGenDeviceCurveWrap::Unknown) {
        info.geometry.curveTopology = curveTopology;
    }
    info.tool = candidate->Tool();
    info.channels = candidate->Channels();
    info.owner = candidate->Owner();
    return UsdGenDeviceGeneration::Create(std::move(info), reason);
}
} // namespace usdGen::gpu
#endif
