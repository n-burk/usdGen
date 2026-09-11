// Small ownership primitives for buffers which never acquire a host mirror.
#ifndef USDGEN_GPU_DEVICE_BUFFER_H
#define USDGEN_GPU_DEVICE_BUFFER_H

#include <cuda_runtime.h>

#include <cstddef>
#include <limits>
#include <utility>

namespace usdGen { namespace gpu {

template <class T> struct DeviceView {
    T* data = nullptr;
    size_t size = 0;
    constexpr explicit operator bool() const { return data != nullptr || size == 0; }
};

// A move-only allocation.  The event is deliberately owned by the allocation:
// a published generation records its last producer use, and a consumer can put
// a wait on its own stream before dereferencing view().
template <class T> class DeviceBuffer {
public:
    DeviceBuffer() = default;
    ~DeviceBuffer() { release(); }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    DeviceBuffer(DeviceBuffer&& other) noexcept { moveFrom(other); }
    DeviceBuffer& operator=(DeviceBuffer&& other) noexcept {
        if (this != &other) { release(); moveFrom(other); }
        return *this;
    }

    cudaError_t reset(size_t count);
    void release() noexcept;
    // Only for a lost device or unprovable completion: abandon handles rather
    // than free storage still potentially in use. Context teardown reclaims
    // these intentionally quarantined allocations/events.
    void quarantine() noexcept { data_ = nullptr; size_ = 0; ready_ = nullptr; }
    DeviceView<T> view() { return {data_, size_}; }
    DeviceView<const T> view() const { return {data_, size_}; }
    size_t size() const { return size_; }
    T* data() { return data_; }
    const T* data() const { return data_; }
    cudaError_t recordUse(cudaStream_t stream);
    cudaError_t waitOn(cudaStream_t stream) const;

private:
    void moveFrom(DeviceBuffer& other) noexcept;
    T* data_ = nullptr;
    size_t size_ = 0;
    cudaEvent_t ready_ = nullptr;
};

}} // namespace usdGen::gpu
#endif
