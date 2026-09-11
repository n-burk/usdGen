#include "curveCompaction.h"

#include <cub/device/device_scan.cuh>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace usdGen::gpu {
namespace {

constexpr int kBadShape = 1;
constexpr int kNonFinite = 2;
constexpr int kBadValue = 3;

__device__ void SetError(int *error, int code) { atomicCAS(error, 0, code); }
__device__ bool Finite(float x) { return isfinite(x); }
__device__ bool Finite(float3 const &p) {
    return Finite(p.x) && Finite(p.y) && Finite(p.z);
}
__device__ bool Finite(float2 const &p) { return Finite(p.x) && Finite(p.y); }

__global__ void ValidateInput(
    DeviceCurveGeometryView geometry, DeviceView<const float> hairT,
    DeviceView<const int32_t> rootPrim, DeviceView<const float2> rootUV,
    DeviceView<const uint8_t> keep, DeviceView<uint32_t> flags,
    DeviceView<uint32_t> pointLengths, int *error) {
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        if (!geometry.curveOffsets.data || geometry.curveOffsets.size != geometry.curveCount + 1 ||
            geometry.curveOffsets.data[0] != 0 ||
            geometry.curveOffsets.data[geometry.curveCount] != geometry.pointCount)
            SetError(error, kBadShape);
    }
    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    size_t stride = size_t(blockDim.x) * gridDim.x;
    for (size_t curve = i; curve < geometry.curveCount; curve += stride) {
        uint32_t begin = geometry.curveOffsets.data[curve];
        uint32_t end = geometry.curveOffsets.data[curve + 1];
        bool valid = end > begin && end <= geometry.pointCount;
        if (!valid) SetError(error, kBadShape);
        uint8_t value = keep.data[curve];
        if (value > 1u) SetError(error, kBadValue);
        flags.data[curve] = valid && value ? 1u : 0u;
        pointLengths.data[curve] = valid && value ? end - begin : 0u;
        if (rootPrim.data && (rootPrim.data[curve] < 0 || !rootUV.data ||
                              !Finite(rootUV.data[curve]) ||
                              rootUV.data[curve].x < 0 || rootUV.data[curve].x > 1 ||
                              rootUV.data[curve].y < 0 || rootUV.data[curve].y > 1))
            SetError(error, kBadValue);
        if (hairT.data && valid &&
            (hairT.data[begin] != 0.0f || hairT.data[end - 1] != 1.0f))
            SetError(error, kBadValue);
        if (hairT.data && valid)
            for (uint32_t p = begin + 1; p < end; ++p)
                if (hairT.data[p] < hairT.data[p - 1]) SetError(error, kBadValue);
    }
    for (size_t point = i; point < geometry.pointCount; point += stride) {
        if ((geometry.points.data && !Finite(geometry.points.data[point])) ||
            (geometry.restPoints.data && !Finite(geometry.restPoints.data[point])) ||
            (geometry.widths.data && !Finite(geometry.widths.data[point])))
            SetError(error, kNonFinite);
        if (geometry.widths.data && Finite(geometry.widths.data[point]) &&
            geometry.widths.data[point] < 0)
            SetError(error, kBadValue);
        if (hairT.data && (!Finite(hairT.data[point]) || hairT.data[point] < 0 || hairT.data[point] > 1))
            SetError(error, kBadValue);
    }
}

__global__ void FinalizeCounts(DeviceView<const uint32_t> flags,
                               DeviceView<const uint32_t> curvePrefix,
                               DeviceView<const uint32_t> pointLengths,
                               DeviceView<const uint32_t> pointPrefix,
                               DeviceView<uint32_t> counts) {
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        size_t last = flags.size - 1;
        counts.data[0] = curvePrefix.data[last] + flags.data[last];
        counts.data[1] = pointPrefix.data[last] + pointLengths.data[last];
    }
}

__global__ void ScatterCurves(
    DeviceCurveGeometryView input, DeviceView<const float> inputHairT,
    DeviceView<const int32_t> inputRootPrim, DeviceView<const float2> inputRootUV,
    DeviceView<const uint8_t> keep, DeviceView<const uint32_t> curvePrefix,
    DeviceView<const uint32_t> pointPrefix, DeviceView<uint32_t> outputOffsets,
    DeviceView<float3> outputPoints, DeviceView<float3> outputRest,
    DeviceView<float> outputWidths, DeviceView<uint64_t> outputIds,
    DeviceView<float> outputHairT, DeviceView<int32_t> outputRootPrim,
    DeviceView<float2> outputRootUV) {
    size_t firstCurve = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    size_t stride = size_t(blockDim.x) * gridDim.x;
    for (size_t curve = firstCurve; curve < input.curveCount; curve += stride) {
        if (!keep.data[curve]) continue;
        uint32_t begin = input.curveOffsets.data[curve];
        uint32_t end = input.curveOffsets.data[curve + 1];
        uint32_t outputCurve = curvePrefix.data[curve];
        uint32_t outputPoint = pointPrefix.data[curve];
        outputOffsets.data[outputCurve] = outputPoint;
        if (outputIds.data) outputIds.data[outputCurve] = input.stableIds.data[curve];
        if (outputRootPrim.data) outputRootPrim.data[outputCurve] = inputRootPrim.data[curve];
        if (outputRootUV.data) outputRootUV.data[outputCurve] = inputRootUV.data[curve];
        for (uint32_t p = begin; p < end; ++p) {
            uint32_t q = outputPoint + p - begin;
            if (outputPoints.data) outputPoints.data[q] = input.points.data[p];
            if (outputRest.data) outputRest.data[q] = input.restPoints.data[p];
            if (outputWidths.data) outputWidths.data[q] = input.widths.data[p];
            if (outputHairT.data) outputHairT.data[q] = inputHairT.data[p];
        }
    }
}

__global__ void FinalizeOutputOffset(DeviceView<uint32_t> offsets,
                                     uint32_t outputCurves,
                                     uint32_t outputPoints) {
    if (blockIdx.x == 0 && threadIdx.x == 0)
        offsets.data[outputCurves] = outputPoints;
}

unsigned Blocks(size_t count) {
    size_t blocks = (count + 255) / 256;
    return static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(blocks, 65535)));
}

cudaError_t CheckStream(int deviceIndex, cudaStream_t stream) {
    int current = -1;
    if (cudaGetDevice(&current) != cudaSuccess) return cudaErrorUnknown;
    if (deviceIndex >= 0 && current != deviceIndex) return cudaErrorInvalidDevice;
    if (stream) {
        int streamDevice = -1;
        if (cudaStreamGetDevice(stream, &streamDevice) != cudaSuccess)
            return cudaErrorUnknown;
        if (streamDevice != current || (deviceIndex >= 0 && streamDevice != deviceIndex))
            return cudaErrorInvalidDevice;
    }
    return cudaSuccess;
}

template <class T>
DeviceView<const T> ReadView(DeviceBuffer<T> const& buffer) {
    return {buffer.data(), buffer.size()};
}

} // namespace

void CudaCurveCompaction::Storage::clear() noexcept {
    points.reset(0); restPoints.reset(0); widths.reset(0); curveOffsets.reset(0);
    stableIds.reset(0); hairT.reset(0); rootPrim.reset(0); rootUV.reset(0);
    curveCount = pointCount = 0;
}

void CudaCurveCompaction::Storage::quarantine() noexcept {
    points.quarantine(); restPoints.quarantine(); widths.quarantine();
    curveOffsets.quarantine(); stableIds.quarantine(); hairT.quarantine();
    rootPrim.quarantine(); rootUV.quarantine();
    curveCount = pointCount = 0;
}

void CudaCurveCompaction::Storage::swap(Storage& other) noexcept {
    using std::swap;
    swap(points, other.points); swap(restPoints, other.restPoints);
    swap(widths, other.widths); swap(curveOffsets, other.curveOffsets);
    swap(stableIds, other.stableIds); swap(hairT, other.hairT);
    swap(rootPrim, other.rootPrim); swap(rootUV, other.rootUV);
    swap(curveCount, other.curveCount); swap(pointCount, other.pointCount);
}

DeviceCurveGeometryView CudaCurveCompaction::Storage::geometry() const noexcept {
    return {points.view(), restPoints.view(), widths.view(), curveOffsets.view(),
            stableIds.view(), curveCount, pointCount};
}

DeviceView<const float> CudaCurveCompaction::Storage::hairTView() const noexcept {
    return hairT.view();
}
DeviceView<const int32_t> CudaCurveCompaction::Storage::rootPrimView() const noexcept {
    return rootPrim.view();
}
DeviceView<const float2> CudaCurveCompaction::Storage::rootUVView() const noexcept {
    return {reinterpret_cast<const float2*>(rootUV.data()), rootUV.size() / sizeof(float2)};
}

cudaError_t CudaCurveCompaction::Storage::recordUse(cudaStream_t stream) {
    cudaError_t result = points.recordUse(stream);
    if (result == cudaSuccess) result = restPoints.recordUse(stream);
    if (result == cudaSuccess) result = widths.recordUse(stream);
    if (result == cudaSuccess) result = curveOffsets.recordUse(stream);
    if (result == cudaSuccess) result = stableIds.recordUse(stream);
    if (result == cudaSuccess) result = hairT.recordUse(stream);
    if (result == cudaSuccess) result = rootPrim.recordUse(stream);
    if (result == cudaSuccess) result = rootUV.recordUse(stream);
    return result;
}

cudaError_t CudaCurveCompaction::Storage::waitOn(cudaStream_t stream) const {
    cudaError_t result = points.waitOn(stream);
    if (result == cudaSuccess) result = restPoints.waitOn(stream);
    if (result == cudaSuccess) result = widths.waitOn(stream);
    if (result == cudaSuccess) result = curveOffsets.waitOn(stream);
    if (result == cudaSuccess) result = stableIds.waitOn(stream);
    if (result == cudaSuccess) result = hairT.waitOn(stream);
    if (result == cudaSuccess) result = rootPrim.waitOn(stream);
    if (result == cudaSuccess) result = rootUV.waitOn(stream);
    return result;
}

CudaCurveCompaction::~CudaCurveCompaction() {
    const bool owns = active_.points.size() || active_.curveOffsets.size() ||
        pendingStorage_.points.size() || pendingStorage_.curveOffsets.size() ||
        error_.size() || ready_;
    int previous = -1;
    const bool selected = !owns ||
        (cudaGetDevice(&previous) == cudaSuccess && deviceIndex_ >= 0 &&
         cudaSetDevice(deviceIndex_) == cudaSuccess);
    const bool synchronized = !owns ||
        (selected && (!ready_ || cudaEventSynchronize(ready_) == cudaSuccess));
    if (!selected || !synchronized) {
        active_.quarantine(); pendingStorage_.quarantine();
        error_.quarantine(); curveFlags_.quarantine(); curvePrefix_.quarantine();
        pointLengths_.quarantine(); pointPrefix_.quarantine(); counts_.quarantine();
        scanTemp_.quarantine(); ready_ = nullptr;
        if (selected && previous >= 0 && previous != deviceIndex_) cudaSetDevice(previous);
        return;
    }
    if (ready_) { cudaEventDestroy(ready_); ready_ = nullptr; }
    active_.clear(); pendingStorage_.clear(); error_.reset(0);
    curveFlags_.reset(0); curvePrefix_.reset(0); pointLengths_.reset(0);
    pointPrefix_.reset(0); counts_.reset(0); scanTemp_.reset(0);
    if (previous >= 0 && previous != deviceIndex_) cudaSetDevice(previous);
}

cudaError_t CudaCurveCompaction::validateStream(cudaStream_t stream) const {
    return CheckStream(deviceIndex_, stream);
}

CurveCompactionStatus CudaCurveCompaction::finishError(int code) const {
    if (code == kNonFinite) return CurveCompactionStatus::NonFiniteInput;
    return CurveCompactionStatus::InvalidArgument;
}

CurveCompactionStatus CudaCurveCompaction::Apply(
    DeviceCurveGeometryView geometry, DeviceView<const float> inputHairT,
    DeviceView<const int32_t> inputRootPrim, DeviceView<const float2> inputRootUV,
    DeviceView<const uint8_t> keep, cudaStream_t stream) {
    if (pending_) return CurveCompactionStatus::InvalidArgument;
    cudaError_t streamStatus = validateStream(stream);
    if (streamStatus != cudaSuccess) return CurveCompactionStatus::InvalidArgument;
    if (deviceIndex_ < 0 && cudaGetDevice(&deviceIndex_) != cudaSuccess)
        return CurveCompactionStatus::CudaError;
    const size_t max = std::numeric_limits<uint32_t>::max();
    if (geometry.curveCount > max || geometry.pointCount > max ||
        (geometry.curveCount == 0) != (geometry.pointCount == 0) ||
        geometry.curveOffsets.size != geometry.curveCount + 1 ||
        !geometry.curveOffsets.data ||
        (geometry.pointCount && (!geometry.points.data || geometry.points.size != geometry.pointCount)) ||
        (!geometry.pointCount && geometry.points.size != 0) ||
        (geometry.restPoints.size && (!geometry.restPoints.data || geometry.restPoints.size != geometry.pointCount)) ||
        (geometry.widths.size && (!geometry.widths.data || geometry.widths.size != geometry.pointCount)) ||
        (geometry.stableIds.size && (!geometry.stableIds.data || geometry.stableIds.size != geometry.curveCount)) ||
        (!inputHairT.data && inputHairT.size != 0) ||
        (inputHairT.data && inputHairT.size != geometry.pointCount) ||
        (!inputRootPrim.data && inputRootPrim.size != 0) ||
        (!inputRootUV.data && inputRootUV.size != 0) ||
        ((inputRootPrim.size != 0) != (inputRootUV.size != 0)) ||
        (inputRootPrim.size && inputRootPrim.size != geometry.curveCount) ||
        (inputRootUV.size && inputRootUV.size != geometry.curveCount) ||
        (!keep.data && keep.size != 0) || keep.size != geometry.curveCount)
        return CurveCompactionStatus::InvalidArgument;
    if (geometry.curveCount && (!geometry.points.data || !keep.data))
        return CurveCompactionStatus::InvalidArgument;
    if (!ready_ && cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming) != cudaSuccess)
        return CurveCompactionStatus::CudaError;
    if (error_.reset(1) != cudaSuccess ||
        counts_.reset(2) != cudaSuccess ||
        curveFlags_.reset(geometry.curveCount) != cudaSuccess ||
        curvePrefix_.reset(geometry.curveCount) != cudaSuccess ||
        pointLengths_.reset(geometry.curveCount) != cudaSuccess ||
        pointPrefix_.reset(geometry.curveCount) != cudaSuccess ||
        cudaMemsetAsync(error_.data(), 0, sizeof(int), stream) != cudaSuccess)
        return CurveCompactionStatus::CudaError;
    const size_t workCount = std::max(geometry.curveCount, geometry.pointCount);
    ValidateInput<<<Blocks(workCount), 256, 0, stream>>>(
        geometry, inputHairT, inputRootPrim, inputRootUV, keep,
        curveFlags_.view(), pointLengths_.view(), error_.data());
    if (cudaGetLastError() != cudaSuccess) return CurveCompactionStatus::CudaError;
    int error = 0;
    if (cudaMemcpyAsync(&error, error_.data(), sizeof(error), cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
        cudaStreamSynchronize(stream) != cudaSuccess)
        return CurveCompactionStatus::CudaError;
    if (error) return finishError(error);

    uint32_t outputCurves = 0, outputPoints = 0;
    if (geometry.curveCount) {
        size_t curveTempBytes = 0, pointTempBytes = 0;
        if (cub::DeviceScan::ExclusiveSum(nullptr, curveTempBytes,
                curveFlags_.data(), curvePrefix_.data(), geometry.curveCount, stream) != cudaSuccess ||
            cub::DeviceScan::ExclusiveSum(nullptr, pointTempBytes,
                pointLengths_.data(), pointPrefix_.data(), geometry.curveCount, stream) != cudaSuccess)
            return CurveCompactionStatus::CudaError;
        if (scanTemp_.reset(std::max(curveTempBytes, pointTempBytes)) != cudaSuccess)
            return CurveCompactionStatus::CudaError;
        if (cub::DeviceScan::ExclusiveSum(scanTemp_.data(), curveTempBytes,
                curveFlags_.data(), curvePrefix_.data(), geometry.curveCount, stream) != cudaSuccess ||
            cub::DeviceScan::ExclusiveSum(scanTemp_.data(), pointTempBytes,
                pointLengths_.data(), pointPrefix_.data(), geometry.curveCount, stream) != cudaSuccess)
            return CurveCompactionStatus::CudaError;
        FinalizeCounts<<<1, 1, 0, stream>>>(
            ReadView(curveFlags_), ReadView(curvePrefix_), ReadView(pointLengths_),
            ReadView(pointPrefix_), counts_.view());
        if (cudaGetLastError() != cudaSuccess ||
            cudaMemcpyAsync(&outputCurves, counts_.data(), sizeof(uint32_t), cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
            cudaMemcpyAsync(&outputPoints, counts_.data() + 1, sizeof(uint32_t), cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
            cudaStreamSynchronize(stream) != cudaSuccess)
            return CurveCompactionStatus::CudaError;
    }
    pendingStorage_.clear();
    pendingStorage_.curveCount = outputCurves;
    pendingStorage_.pointCount = outputPoints;
    if (pendingStorage_.curveOffsets.reset(size_t(outputCurves) + 1) != cudaSuccess ||
        (outputPoints && pendingStorage_.points.reset(outputPoints) != cudaSuccess) ||
        (geometry.restPoints.size && outputPoints && pendingStorage_.restPoints.reset(outputPoints) != cudaSuccess) ||
        (geometry.widths.size && outputPoints && pendingStorage_.widths.reset(outputPoints) != cudaSuccess) ||
        (geometry.stableIds.size && outputCurves && pendingStorage_.stableIds.reset(outputCurves) != cudaSuccess) ||
        (inputHairT.size && outputPoints && pendingStorage_.hairT.reset(outputPoints) != cudaSuccess) ||
        (inputRootPrim.size && outputCurves && pendingStorage_.rootPrim.reset(outputCurves) != cudaSuccess) ||
        (inputRootUV.size && outputCurves && pendingStorage_.rootUV.reset(size_t(outputCurves) * sizeof(float2)) != cudaSuccess)) {
        pendingStorage_.clear();
        return CurveCompactionStatus::CudaError;
    }
    if (outputCurves) {
        ScatterCurves<<<Blocks(geometry.curveCount), 256, 0, stream>>>(
            geometry, inputHairT, inputRootPrim, inputRootUV, keep,
            ReadView(curvePrefix_), ReadView(pointPrefix_), pendingStorage_.curveOffsets.view(),
            pendingStorage_.points.view(), pendingStorage_.restPoints.view(),
            pendingStorage_.widths.view(), pendingStorage_.stableIds.view(),
            pendingStorage_.hairT.view(), pendingStorage_.rootPrim.view(),
            {reinterpret_cast<float2*>(pendingStorage_.rootUV.data()), outputCurves});
        if (cudaGetLastError() != cudaSuccess) { pendingStorage_.clear(); return CurveCompactionStatus::CudaError; }
    }
    FinalizeOutputOffset<<<1, 1, 0, stream>>>(pendingStorage_.curveOffsets.view(),
                                               outputCurves, outputPoints);
    if (cudaGetLastError() != cudaSuccess) { pendingStorage_.clear(); return CurveCompactionStatus::CudaError; }
    if (cudaEventRecord(ready_, stream) != cudaSuccess) {
        pendingStorage_.clear();
        return CurveCompactionStatus::CudaError;
    }
    pending_ = true;
    return CurveCompactionStatus::Ok;
}

CurveCompactionStatus CudaCurveCompaction::Finish(cudaStream_t stream) {
    cudaError_t streamStatus = validateStream(stream);
    if (streamStatus != cudaSuccess) return CurveCompactionStatus::InvalidArgument;
    if (!pending_) return CurveCompactionStatus::NoPendingUpdate;
    int error = 0;
    if (cudaStreamWaitEvent(stream, ready_, 0) != cudaSuccess ||
        cudaMemcpyAsync(&error, error_.data(), sizeof(error), cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
        cudaStreamSynchronize(stream) != cudaSuccess)
        return CurveCompactionStatus::CudaError;
    if (error) {
        pending_ = false;
        pendingStorage_.clear();
        return finishError(error);
    }
    if (active_.curveCount || active_.pointCount || active_.curveOffsets.size()) {
        if (active_.waitOn(stream) != cudaSuccess || cudaStreamSynchronize(stream) != cudaSuccess)
            return CurveCompactionStatus::CudaError;
    }
    active_.swap(pendingStorage_);
    pendingStorage_.clear();
    pending_ = false;
    ++generation_;
    return CurveCompactionStatus::Ok;
}

DeviceCurveGeometryView CudaCurveCompaction::view() const noexcept { return active_.geometry(); }
DeviceView<const float> CudaCurveCompaction::hairT() const noexcept { return active_.hairTView(); }
DeviceView<const int32_t> CudaCurveCompaction::rootPrim() const noexcept { return active_.rootPrimView(); }
DeviceView<const float2> CudaCurveCompaction::rootUV() const noexcept { return active_.rootUVView(); }

cudaError_t CudaCurveCompaction::recordUse(cudaStream_t stream) {
    if (validateStream(stream) != cudaSuccess) return cudaErrorInvalidDevice;
    return active_.recordUse(stream);
}
cudaError_t CudaCurveCompaction::waitOn(cudaStream_t stream) const {
    if (validateStream(stream) != cudaSuccess) return cudaErrorInvalidDevice;
    return active_.waitOn(stream);
}

} // namespace usdGen::gpu
