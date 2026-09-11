#ifndef USDGEN_GPU_CURVE_COMPACTION_H
#define USDGEN_GPU_CURVE_COMPACTION_H

#include "curveGeometry.h"

#include <cstddef>
#include <cstdint>

namespace usdGen::gpu {

enum class CurveCompactionStatus {
    Ok,
    InvalidArgument,
    NonFiniteInput,
    CudaError,
    NoPendingUpdate
};

// Device-only stable compaction of ragged C3 strands.  Every supplied input
// channel is copied in curve order for keep[c] != 0; optional root channels
// must be supplied together.  Input views are borrowed until Finish returns.
class CudaCurveCompaction {
public:
    CudaCurveCompaction() = default;
    ~CudaCurveCompaction();
    CudaCurveCompaction(CudaCurveCompaction const&) = delete;
    CudaCurveCompaction& operator=(CudaCurveCompaction const&) = delete;

    CurveCompactionStatus Apply(
        DeviceCurveGeometryView geometry, DeviceView<const float> hairT,
        DeviceView<const int32_t> rootPrim, DeviceView<const float2> rootUV,
        DeviceView<const uint8_t> keep, cudaStream_t stream);
    CurveCompactionStatus Finish(cudaStream_t stream);

    DeviceCurveGeometryView view() const noexcept;
    DeviceView<const float> hairT() const noexcept;
    DeviceView<const int32_t> rootPrim() const noexcept;
    DeviceView<const float2> rootUV() const noexcept;
    cudaError_t recordUse(cudaStream_t stream);
    cudaError_t waitOn(cudaStream_t stream) const;
    int deviceIndex() const noexcept { return deviceIndex_; }
    uint64_t generation() const noexcept { return generation_; }
    bool pending() const noexcept { return pending_; }

private:
    struct Storage {
        DeviceBuffer<float3> points;
        DeviceBuffer<float3> restPoints;
        DeviceBuffer<float> widths;
        DeviceBuffer<uint32_t> curveOffsets;
        DeviceBuffer<uint64_t> stableIds;
        DeviceBuffer<float> hairT;
        DeviceBuffer<int32_t> rootPrim;
        // DeviceBuffer<float2> is intentionally not part of the common
        // explicit-instantiation set; store the same bytes as CurveSource.
        DeviceBuffer<unsigned char> rootUV;
        size_t curveCount = 0;
        size_t pointCount = 0;

        void clear() noexcept;
        void quarantine() noexcept;
        void swap(Storage& other) noexcept;
        DeviceCurveGeometryView geometry() const noexcept;
        DeviceView<const float> hairTView() const noexcept;
        DeviceView<const int32_t> rootPrimView() const noexcept;
        DeviceView<const float2> rootUVView() const noexcept;
        cudaError_t recordUse(cudaStream_t stream);
        cudaError_t waitOn(cudaStream_t stream) const;
    };

    cudaError_t validateStream(cudaStream_t stream) const;
    CurveCompactionStatus finishError(int code) const;

    Storage active_, pendingStorage_;
    DeviceBuffer<int> error_;
    DeviceBuffer<uint32_t> curveFlags_, curvePrefix_;
    DeviceBuffer<uint32_t> pointLengths_, pointPrefix_;
    DeviceBuffer<uint32_t> counts_;
    DeviceBuffer<unsigned char> scanTemp_;
    cudaEvent_t ready_ = nullptr;
    bool pending_ = false;
    int deviceIndex_ = -1;
    uint64_t generation_ = 0;
};

} // namespace usdGen::gpu

#endif // USDGEN_GPU_CURVE_COMPACTION_H
