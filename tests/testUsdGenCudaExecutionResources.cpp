#include "usdGen/gpu/deviceBuffer.h"
#include "usdGen/gpu/deviceResources.h"

#include <cstdio>

using namespace usdGen;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (false)

int main() {
    int device = -1;
    if (cudaGetDevice(&device) != cudaSuccess) return 77;
    // This process configures the device before its first DeviceBuffer.
    CHECK(gpu::ConfigureCudaExecutionResources(device, {1024, 128}));
    gpu::DeviceBuffer<float> empty, buffer;
    CHECK(empty.recordUse(nullptr) == cudaSuccess && empty.waitOn(nullptr) == cudaSuccess);
    CHECK(buffer.reset(33) == cudaSuccess);
    float const sentinel = 7.25f;
    CHECK(cudaMemcpy(buffer.data(), &sentinel, sizeof(sentinel), cudaMemcpyHostToDevice) == cudaSuccess);
    auto resources = FindUsdGenExecutionResourcePool(
        {UsdGenExecutionResourceBackend::Cuda, device});
    CHECK(resources && resources->Snapshot().usedBytes == 33 * sizeof(float));
    auto const pointer = buffer.data();
    auto moved = std::move(buffer);
    CHECK(buffer.recordUse(nullptr) == cudaSuccess && buffer.waitOn(nullptr) == cudaSuccess);
    CHECK(moved.reset(200) == cudaErrorMemoryAllocation);
    float restored = 0;
    CHECK(moved.data() == pointer && moved.size() == 33 &&
          cudaMemcpy(&restored, moved.data(), sizeof(restored), cudaMemcpyDeviceToHost) == cudaSuccess &&
          restored == sentinel && resources->Snapshot().usedBytes == 33 * sizeof(float));
    moved.release();
    CHECK(resources->Snapshot().usedBytes == 0);
    CHECK(moved.reset(200) == cudaSuccess);
    CHECK(resources->Snapshot().usedBytes == 200 * sizeof(float));
    return 0;
}
