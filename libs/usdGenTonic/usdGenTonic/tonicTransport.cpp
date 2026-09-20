// usdGenTonic — host-staged transport implementation (Phase A).
#include "usdGenTonic/tonicTransport.h"
#include "usdGenTonic/tonicModel.h"

#ifdef USDGEN_TONIC_HAS_CUDA
#include <cuda_runtime.h>
#endif

#include <cstdlib>
#include <cstring>

namespace usdGenTonic {

TonicPinnedStaging::TonicPinnedStaging() = default;

TonicPinnedStaging::~TonicPinnedStaging()
{
    if (!_data) {
        return;
    }
#ifdef USDGEN_TONIC_HAS_CUDA
    if (_pinned) {
        cudaFreeHost(_data);
    } else {
        std::free(_data);
    }
#else
    std::free(_data);
#endif
    _data = nullptr;
}

float *
TonicPinnedStaging::Reset(size_t floatCount)
{
    if (floatCount == 0) {
        return nullptr;
    }
    if (floatCount == _floatCount && _data) {
        return _data;
    }
    if (_data) {
#ifdef USDGEN_TONIC_HAS_CUDA
        if (_pinned) {
            cudaFreeHost(_data);
        } else {
            std::free(_data);
        }
#else
        std::free(_data);
#endif
        _data = nullptr;
        _floatCount = 0;
        _pinned = false;
    }
#ifdef USDGEN_TONIC_HAS_CUDA
    // cudaHostAlloc fails cleanly without a device; fall back to malloc so
    // GPU-less hosts stage through pageable memory with identical semantics.
    if (cudaHostAlloc(reinterpret_cast<void **>(&_data),
                      floatCount * sizeof(float),
                      cudaHostAllocDefault) == cudaSuccess) {
        _pinned = true;
    } else {
        _data = nullptr;
    }
#endif
    if (!_data) {
        _data = static_cast<float *>(
            std::malloc(floatCount * sizeof(float)));
        _pinned = false;
    }
    if (!_data) {
        _floatCount = 0;
        return nullptr;
    }
    _floatCount = floatCount;
    return _data;
}

float *
TonicPinnedStaging::Ensure(size_t floatCount)
{
    if (floatCount == 0) {
        return nullptr;
    }
    if (_data && _floatCount >= floatCount) {
        return _data;
    }
    return Reset(floatCount);
}

bool
TonicStageTubeMesh(TonicModel const &model,
                   TonicPinnedStaging *positions,
                   TonicPinnedStaging *normals,
                   TonicStagedTubeMesh *out)
{
    if (!positions || !normals || !out) {
        return false;
    }
    TonicModel::HostTubeMesh const &host = model.GetHostMesh();
    if (host.positions.empty() || host.normals.empty() ||
        host.positions.size() != host.normals.size() ||
        host.faceVertexCounts.empty() || host.faceVertexIndices.empty()) {
        return false;
    }
    // Device -> pinned host. When the CUDA mirror exists the pinned block
    // is filled by a real D2H of the kernel's output; otherwise the
    // authoritative host mirror is staged instead (GPU-less hosts). Either
    // way the pinned block is the copy Hydra reads from, never the model's
    // storage.
    float *stagedPositions = positions->Reset(host.positions.size());
    float *stagedNormals = normals->Reset(host.normals.size());
    if (!stagedPositions || !stagedNormals) {
        return false;
    }
    if (!model.CopyDeviceToHost(stagedPositions, stagedNormals,
                                host.positions.size())) {
        std::memcpy(stagedPositions, host.positions.data(),
                    host.positions.size() * sizeof(float));
        std::memcpy(stagedNormals, host.normals.data(),
                    host.normals.size() * sizeof(float));
    }

    size_t const vertexCount = host.positions.size() / 3;
    TonicStagedTubeMesh staged;
    staged.points.resize(vertexCount);
    staged.normals.resize(vertexCount);
    std::memcpy(staged.points.data(), stagedPositions,
                host.positions.size() * sizeof(float));
    std::memcpy(staged.normals.data(), stagedNormals,
                host.normals.size() * sizeof(float));
    staged.faceVertexCounts.resize(host.faceVertexCounts.size());
    staged.faceVertexIndices.resize(host.faceVertexIndices.size());
    std::memcpy(staged.faceVertexCounts.data(), host.faceVertexCounts.data(),
                host.faceVertexCounts.size() * sizeof(int));
    std::memcpy(staged.faceVertexIndices.data(), host.faceVertexIndices.data(),
                host.faceVertexIndices.size() * sizeof(int));
    staged.extentMin = GfVec3f(host.extentMin[0], host.extentMin[1],
                               host.extentMin[2]);
    staged.extentMax = GfVec3f(host.extentMax[0], host.extentMax[1],
                               host.extentMax[2]);
    staged.version = model.GetVersion();
    *out = std::move(staged);
    return true;
}

} // namespace usdGenTonic
