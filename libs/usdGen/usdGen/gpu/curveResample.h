#ifndef USDGEN_GPU_CURVE_RESAMPLE_H
#define USDGEN_GPU_CURVE_RESAMPLE_H

#include "curveGeometry.h"

#include <cstddef>
#include <cstdint>

namespace usdGen::gpu {

enum class CurveResampleStatus { Ok, InvalidArgument, NonFiniteInput, CudaError, NoPendingUpdate };

// Device-only indexed-CV resampling.  target==0 clones the ragged layout;
// target>=2 makes every curve uniform.  Borrowed inputs remain live through
// Finish; active output is replaced only by a successful Finish.
class CudaCurveResample {
public:
    CudaCurveResample() = default;
    ~CudaCurveResample();
    CudaCurveResample(CudaCurveResample const&) = delete;
    CudaCurveResample& operator=(CudaCurveResample const&) = delete;
    CurveResampleStatus Apply(
        DeviceCurveGeometryView geometry, DeviceView<const float> hairT,
        DeviceView<const int32_t> rootPrim, DeviceView<const float2> rootUV,
        int32_t targetPointCount, cudaStream_t stream);
    CurveResampleStatus Finish(cudaStream_t);
    DeviceCurveGeometryView view() const noexcept;
    DeviceView<const float> hairT() const noexcept;
    DeviceView<const int32_t> rootPrim() const noexcept;
    DeviceView<const float2> rootUV() const noexcept;
    cudaError_t recordUse(cudaStream_t);
    cudaError_t waitOn(cudaStream_t) const;
    int deviceIndex() const noexcept { return device_; }
    bool pending() const noexcept { return pending_; }
private:
    struct Storage {
        DeviceBuffer<float3> points, restPoints;
        DeviceBuffer<float> widths, hairT;
        DeviceBuffer<uint32_t> curveOffsets;
        DeviceBuffer<uint64_t> stableIds;
        DeviceBuffer<int32_t> rootPrim;
        DeviceBuffer<unsigned char> rootUV;
        size_t curveCount = 0, pointCount = 0;
        void clear() noexcept;
        void quarantine() noexcept;
        void swap(Storage&) noexcept;
        DeviceCurveGeometryView geometry() const noexcept;
        DeviceView<const float2> rootUVView() const noexcept;
        cudaError_t recordUse(cudaStream_t);
        cudaError_t waitOn(cudaStream_t) const;
    } active_, staging_;
    CurveResampleStatus finishError(int error) const;
    void discardStaging(cudaStream_t stream) noexcept;
    DeviceBuffer<int> error_;
    cudaEvent_t ready_ = nullptr;
    int device_ = -1;
    bool pending_ = false;
    cudaError_t validateStream(cudaStream_t) const;
};
}
#endif
