#ifndef USDGEN_GPU_GENERATION_H
#define USDGEN_GPU_GENERATION_H

#include "usdGen/deviceGeneration.h"
#include "curveSource.h"

namespace usdGen::gpu {

class CudaCurveCompaction;

// Takes exclusive ownership of a completed source. Each published generation
// gets distinct mutable storage; it is never replaced underneath a reader.
std::shared_ptr<const UsdGenDeviceGeneration> MakeSourceGeneration(
    std::unique_ptr<CudaCurveSource> source, uint64_t generation,
    std::string* reason = nullptr, bool alreadyDeformed = false,
    std::unique_ptr<DeviceBuffer<float>> widths = {},
    std::unique_ptr<DeviceBuffer<float3>> points = {},
    uint64_t topologyVersion = UINT64_MAX);

// Owns a completed GPU topology revision, including every reordered channel.
std::shared_ptr<const UsdGenDeviceGeneration> MakeCompactedGeneration(
    std::unique_ptr<CudaCurveCompaction> geometry, uint64_t generation,
    std::string* reason = nullptr, bool alreadyDeformed = false,
    std::unique_ptr<DeviceBuffer<float>> widths = {},
    std::unique_ptr<DeviceBuffer<float3>> points = {},
    uint64_t topologyVersion = UINT64_MAX);

// Completed device-only point edit of an immutable snapshot. All other
// channels and the topology version are shared with base. A null points
// revision republishes the unmodified base (gesture cancellation).
std::shared_ptr<const UsdGenDeviceGeneration> MakePointRevisionGeneration(
    std::shared_ptr<const UsdGenDeviceGeneration> base, uint64_t generation,
    std::unique_ptr<DeviceBuffer<float3>> points,
    UsdGenDeviceToolMetadata tool = {}, std::string* reason = nullptr);

// Read-only geometry access for CUDA tools/consumers. The native stream must
// remain alive until every copy of the lease is released. Destruction waits
// for queued consumer work before releasing the last storage reference.
// Acquire/release run on ordinary host threads, never CUDA host callbacks
// (CUDA forbids runtime API calls from those callbacks).
class CudaGeometryLease {
public:
    explicit operator bool() const noexcept { return bool(lease_); }
    DeviceCurveGeometryView Geometry() const noexcept { return lease_ ? geometry_ : DeviceCurveGeometryView{}; }
    DeviceView<const float> HairT() const noexcept { return lease_ ? hairT_ : DeviceView<const float>{}; }
    DeviceView<const int32_t> RootPrim() const noexcept { return lease_ ? rootPrim_ : DeviceView<const int32_t>{}; }
    DeviceView<const float2> RootUV() const noexcept { return lease_ ? rootUV_ : DeviceView<const float2>{}; }
private:
    UsdGenDeviceLease lease_;
    DeviceCurveGeometryView geometry_{};
    DeviceView<const float> hairT_{};
    DeviceView<const int32_t> rootPrim_{};
    DeviceView<const float2> rootUV_{};
    friend CudaGeometryLease AcquireGeometry(
        std::shared_ptr<const UsdGenDeviceGeneration> const&, cudaStream_t);
};

/// A zero-copy tile view over a geometry lease. curveOffsets retain their
/// global values (the slice has curveCount + 1 entries); use CurveIndexSpan
/// when passing this view to the index builder.
struct CudaGeometryTileView {
    DeviceView<const float3> points;
    DeviceView<const float3> restPoints;
    DeviceView<const float> widths;
    DeviceView<const float> hairT;
    DeviceView<const uint32_t> curveOffsets;
    DeviceView<const uint64_t> stableIds;
    DeviceView<const int32_t> rootPrim;
    DeviceView<const float2> rootUV;
    UsdGenDeviceTileMetadata range;
    uint64_t generation = 0;
    uint64_t topologyVersion = 0;
    uint64_t valueVersion = 0;
};

/// Owns the parent geometry lease while exposing one zero-copy tile slice.
/// This deliberately does not expose DeviceCurveGeometryView, whose offsets
/// are implicitly zero-based and would misrepresent a tile slice.
class CudaGeometryTileLease {
public:
    explicit operator bool() const noexcept { return bool(parent_); }
    CudaGeometryTileView View() const noexcept {
        return parent_ ? view_ : CudaGeometryTileView{};
    }
private:
    CudaGeometryLease parent_;
    CudaGeometryTileView view_{};
    friend CudaGeometryTileLease AcquireGeometryTile(
        std::shared_ptr<const UsdGenDeviceGeneration> const&, uint32_t,
        uint64_t, cudaStream_t);
};

CudaGeometryLease AcquireGeometry(
    std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
    cudaStream_t stream);

CudaGeometryTileLease AcquireGeometryTile(
    std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
    uint32_t tileId, uint64_t expectedGeneration, cudaStream_t stream);

/// Attach immutable tile-range metadata without copying or replacing any
/// backend channels/owner. Returns a new immutable wrapper on success.
std::shared_ptr<const UsdGenDeviceGeneration> WithTileMetadata(
    std::shared_ptr<const UsdGenDeviceGeneration> const& candidate,
    std::vector<UsdGenDeviceTileMetadata> tiles,
    std::string* reason = nullptr,
    UsdGenDeviceCurveTopologyMetadata curveTopology = {});

} // namespace usdGen::gpu
#endif
