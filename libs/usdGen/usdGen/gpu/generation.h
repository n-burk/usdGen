#ifndef USDGEN_GPU_GENERATION_H
#define USDGEN_GPU_GENERATION_H

#include "usdGen/deviceGeneration.h"
#include "curveSource.h"

namespace usdGen::gpu {

// Takes exclusive ownership of a completed source. Each published generation
// gets distinct mutable storage; it is never replaced underneath a reader.
std::shared_ptr<const UsdGenDeviceGeneration> MakeSourceGeneration(
    std::unique_ptr<CudaCurveSource> source, uint64_t generation,
    std::string* reason = nullptr, bool alreadyDeformed = false,
    std::unique_ptr<DeviceBuffer<float>> widths = {},
    std::unique_ptr<DeviceBuffer<float3>> points = {});

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

CudaGeometryLease AcquireGeometry(
    std::shared_ptr<const UsdGenDeviceGeneration> const& generation,
    cudaStream_t stream);

} // namespace usdGen::gpu
#endif
