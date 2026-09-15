#include "deviceBuffer.h"
#include "cudaCompat.h"
#include "curveTileBounds.h"
#include "curveTiles.h"
#include <cstdint>

namespace usdGen { namespace gpu {
template <class T> cudaError_t DeviceBuffer<T>::reset(
    size_t count, UsdGenExecutionMemoryReservation* reservation,
    UsdGenExecutionResourceKind kind) {
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) return cudaErrorInvalidValue;
    int device = -1;
    if (cudaGetDevice(&device) != cudaSuccess) return cudaErrorInvalidDevice;
    if (count == size_ && data_) return device == device_ ? cudaSuccess : cudaErrorInvalidDevice;
    if (!count) { release(); return cudaSuccess; }
    auto permit = TryReserveCudaExecutionBytes(count * sizeof(T), kind,
                                               reservation);
    if (!permit) return cudaErrorMemoryAllocation;
    T* next = nullptr;
    cudaEvent_t nextReady = nullptr;
    cudaError_t e = cudaMalloc(reinterpret_cast<void**>(&next), count * sizeof(T));
    if (e != cudaSuccess) return e;
    e = cudaEventCreateWithFlags(&nextReady, cudaEventDisableTiming);
    if (e != cudaSuccess) {
        if (cudaFree(next) != cudaSuccess) permit->Abandon();
        return e;
    }
    release();
    data_ = next; size_ = count; ready_ = nextReady; device_ = device;
    permit_ = std::move(*permit);
    return cudaSuccess;
}
template <class T> void DeviceBuffer<T>::release() noexcept {
    if (!data_) { ready_ = nullptr; size_ = 0; device_ = -1; return; }
    int previous = -1;
    bool const selected = cudaGetDevice(&previous) == cudaSuccess &&
        device_ >= 0 && cudaSetDevice(device_) == cudaSuccess;
    // Device provenance cannot be proved: do not issue event/free calls in a
    // possibly foreign context, and retain the resource charge conservatively.
    if (!selected) {
        permit_.Abandon();
        ready_ = nullptr; data_ = nullptr; size_ = 0; device_ = -1;
        return;
    }
    if (ready_) cudaEventDestroy(ready_);
    if (cudaFree(data_) == cudaSuccess) permit_.Release();
    else permit_.Abandon();
    if (previous != device_) cudaSetDevice(previous);
    ready_ = nullptr; data_ = nullptr; size_ = 0; device_ = -1;
}
template <class T> cudaError_t DeviceBuffer<T>::recordUse(cudaStream_t stream) {
    if (!ready_) return cudaSuccess;
    int device = -1;
    if (cudaGetDevice(&device) != cudaSuccess || device != device_) return cudaErrorInvalidDevice;
    if (stream) {
        int streamDevice = -1;
        if (cudaStreamGetDevice(stream, &streamDevice) != cudaSuccess || streamDevice != device_)
            return cudaErrorInvalidDevice;
    }
    return cudaEventRecord(ready_, stream);
}
template <class T> cudaError_t DeviceBuffer<T>::waitOn(cudaStream_t stream) const {
    if (!ready_) return cudaSuccess;
    int device = -1;
    if (cudaGetDevice(&device) != cudaSuccess || device != device_) return cudaErrorInvalidDevice;
    if (stream) {
        int streamDevice = -1;
        if (cudaStreamGetDevice(stream, &streamDevice) != cudaSuccess || streamDevice != device_)
            return cudaErrorInvalidDevice;
    }
    return cudaStreamWaitEvent(stream, ready_, 0);
}
template <class T> cudaError_t DeviceBuffer<T>::queryUse() const {
    if (!ready_) return cudaSuccess;
    int device = -1;
    if (cudaGetDevice(&device) != cudaSuccess || device != device_)
        return cudaErrorInvalidDevice;
    return cudaEventQuery(ready_);
}
template <class T> cudaError_t DeviceBuffer<T>::synchronizeUse() const {
    if (!ready_) return cudaSuccess;
    int previous = -1;
    if (cudaGetDevice(&previous) != cudaSuccess || device_ < 0 ||
        cudaSetDevice(device_) != cudaSuccess)
        return cudaErrorInvalidDevice;
    cudaError_t const status = cudaEventSynchronize(ready_);
    if (previous != device_) (void)cudaSetDevice(previous);
    return status;
}
template <class T> void DeviceBuffer<T>::moveFrom(DeviceBuffer& o) noexcept {
    data_ = o.data_; size_ = o.size_; ready_ = o.ready_; device_ = o.device_;
    permit_ = std::move(o.permit_);
    o.data_ = nullptr; o.size_ = 0; o.ready_ = nullptr; o.device_ = -1;
}
template class DeviceBuffer<float>;
template class DeviceBuffer<double>;
template class DeviceBuffer<int>;
template class DeviceBuffer<unsigned char>;
template class DeviceBuffer<uint32_t>;
template class DeviceBuffer<uint64_t>;
template class DeviceBuffer<float3>;
template class DeviceBuffer<float2>;
template class DeviceBuffer<CurveTileBoundsScratch>;
template class DeviceBuffer<CurveTileSpan>;
}}
