#include "deviceBuffer.h"
#include "curveTiles.h"
#include <cstdint>

namespace usdGen { namespace gpu {
template <class T> cudaError_t DeviceBuffer<T>::reset(size_t count) {
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) return cudaErrorInvalidValue;
    if (count == size_ && data_) return cudaSuccess;
    release();
    if (!count) return cudaSuccess;
    cudaError_t e = cudaMalloc(reinterpret_cast<void**>(&data_), count * sizeof(T));
    if (e != cudaSuccess) { data_ = nullptr; return e; }
    e = cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming);
    if (e != cudaSuccess) { cudaFree(data_); data_ = nullptr; return e; }
    size_ = count;
    return cudaSuccess;
}
template <class T> void DeviceBuffer<T>::release() noexcept {
    if (ready_) cudaEventDestroy(ready_);
    if (data_) cudaFree(data_);
    ready_ = nullptr; data_ = nullptr; size_ = 0;
}
template <class T> cudaError_t DeviceBuffer<T>::recordUse(cudaStream_t stream) {
    return ready_ ? cudaEventRecord(ready_, stream) : cudaSuccess;
}
template <class T> cudaError_t DeviceBuffer<T>::waitOn(cudaStream_t stream) const {
    return ready_ ? cudaStreamWaitEvent(stream, ready_, 0) : cudaSuccess;
}
template <class T> void DeviceBuffer<T>::moveFrom(DeviceBuffer& o) noexcept {
    data_ = o.data_; size_ = o.size_; ready_ = o.ready_;
    o.data_ = nullptr; o.size_ = 0; o.ready_ = nullptr;
}
template class DeviceBuffer<float>;
template class DeviceBuffer<double>;
template class DeviceBuffer<int>;
template class DeviceBuffer<unsigned char>;
template class DeviceBuffer<uint32_t>;
template class DeviceBuffer<uint64_t>;
template class DeviceBuffer<float3>;
template class DeviceBuffer<CurveTileSpan>;
}}
