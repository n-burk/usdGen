#include "gpu/topology.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace usdGen::gpu {
namespace {

struct Result {
    int error;
    int equal;
};

__device__ void SetError(int* error) { atomicCAS(error, 0, 1); }

__global__ void CompareKernel(DeviceCurveGeometryView a,
                              DeviceCurveGeometryView b, Result* result) {
    const size_t index = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    const size_t stride = size_t(blockDim.x) * gridDim.x;

    for (size_t curve = index; curve < a.curveCount; curve += stride) {
        const uint32_t begin = a.curveOffsets.data[curve];
        const uint32_t end = a.curveOffsets.data[curve + 1];
        if (end <= begin || end > a.pointCount) SetError(&result->error);
    }
    for (size_t curve = index; curve < b.curveCount; curve += stride) {
        const uint32_t begin = b.curveOffsets.data[curve];
        const uint32_t end = b.curveOffsets.data[curve + 1];
        if (end <= begin || end > b.pointCount) SetError(&result->error);
    }

    // The first offset is checked separately so an empty curve set accepts
    // the canonical one-element offset array {0}.
    if (index == 0) {
        if (a.curveOffsets.data[0] != 0 ||
            a.curveOffsets.data[a.curveCount] != a.pointCount ||
            b.curveOffsets.data[0] != 0 ||
            b.curveOffsets.data[b.curveCount] != b.pointCount)
            SetError(&result->error);
    }

    const size_t curves = min(a.curveCount, b.curveCount);
    for (size_t curve = index; curve < curves; curve += stride) {
        if (a.stableIds.data[curve] != b.stableIds.data[curve] ||
            a.curveOffsets.data[curve] != b.curveOffsets.data[curve] ||
            a.curveOffsets.data[curve + 1] != b.curveOffsets.data[curve + 1])
            atomicExch(&result->equal, 0);
    }
    if (index == 0 && (a.curveCount != b.curveCount ||
                       a.pointCount != b.pointCount))
        atomicExch(&result->equal, 0);
}

cudaError_t CheckDevicePointer(const void* pointer, size_t count, int device) {
    if (!count) return cudaSuccess;
    if (!pointer) return cudaErrorInvalidValue;
    cudaPointerAttributes attributes{};
    const cudaError_t status = cudaPointerGetAttributes(&attributes, pointer);
    if (status != cudaSuccess) return status;
    if (attributes.type != cudaMemoryTypeDevice || attributes.device != device)
        return cudaErrorInvalidDevicePointer;
    return cudaSuccess;
}

cudaError_t Validate(DeviceCurveGeometryView const& geometry, int device) {
    if (geometry.curveCount == std::numeric_limits<size_t>::max() ||
        geometry.curveCount > std::numeric_limits<uint32_t>::max() ||
        geometry.pointCount > std::numeric_limits<uint32_t>::max() ||
        geometry.curveOffsets.size != geometry.curveCount + 1 ||
        geometry.stableIds.size != geometry.curveCount)
        return cudaErrorInvalidValue;
    cudaError_t status = CheckDevicePointer(geometry.curveOffsets.data,
                                            geometry.curveOffsets.size, device);
    if (status != cudaSuccess) return status;
    return CheckDevicePointer(geometry.stableIds.data, geometry.stableIds.size, device);
}

unsigned Blocks(size_t count) {
    const size_t needed = (count + 255) / 256;
    return static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(needed, 65535)));
}

} // namespace

cudaError_t CompareCurveTopology(DeviceCurveGeometryView a,
                                 DeviceCurveGeometryView b,
                                 cudaStream_t stream, bool* equal) {
    if (!equal) return cudaErrorInvalidValue;
    int device = -1;
    cudaError_t status = cudaGetDevice(&device);
    if (status != cudaSuccess) return status;
    if (stream) {
        int streamDevice = -1;
        status = cudaStreamGetDevice(stream, &streamDevice);
        if (status != cudaSuccess) return status;
        if (streamDevice != device) return cudaErrorInvalidDevice;
    }
    status = Validate(a, device);
    if (status != cudaSuccess) return status;
    status = Validate(b, device);
    if (status != cudaSuccess) return status;

    Result* deviceResult = nullptr;
    status = cudaMalloc(&deviceResult, sizeof(Result));
    if (status != cudaSuccess) return status;
    Result initial{0, 1};
    bool workEnqueued = false;
    status = cudaMemcpyAsync(deviceResult, &initial, sizeof(initial),
                             cudaMemcpyHostToDevice, stream);
    if (status == cudaSuccess) {
        workEnqueued = true;
        const size_t work = std::max(a.curveCount, b.curveCount);
        CompareKernel<<<Blocks(work), 256, 0, stream>>>(a, b, deviceResult);
        status = cudaGetLastError();
    }
    Result host{};
    if (status == cudaSuccess)
        status = cudaMemcpyAsync(&host, deviceResult, sizeof(host),
                                 cudaMemcpyDeviceToHost, stream);
    cudaError_t syncStatus = cudaSuccess;
    if (workEnqueued) syncStatus = cudaStreamSynchronize(stream);
    if (status == cudaSuccess && syncStatus != cudaSuccess) status = syncStatus;
    // Do not free a result allocation after an unproven stream failure.
    if (!workEnqueued || syncStatus == cudaSuccess) {
        const cudaError_t freeStatus = cudaFree(deviceResult);
        if (status == cudaSuccess) status = freeStatus;
    }
    if (status != cudaSuccess) return status;
    if (host.error) return cudaErrorInvalidValue;
    *equal = host.equal != 0;
    return cudaSuccess;
}

} // namespace usdGen::gpu
