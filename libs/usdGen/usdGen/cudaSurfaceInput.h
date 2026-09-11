#ifndef USDGEN_CUDA_SURFACE_INPUT_H
#define USDGEN_CUDA_SURFACE_INPUT_H

#include "gpu/curveGeometry.h"
#include "graphDesc.h"
#include <cstdint>
#include <string>
#include <vector>

namespace usdGen {

struct CudaSurfaceBindingKey {
    SdfPath path;
    VtVec3fArray restPoints;
    VtIntArray faceVertexCounts;
    VtIntArray faceVertexIndices;
    uint32_t sampleBudget = 0;
    int algorithmVersion = 0;
};

struct CudaSurfacePrepared {
    std::vector<float3> restPoints, currentPoints;
    std::vector<uint32_t> faceOffsets, faceVertexIndices;
    CudaSurfaceBindingKey key;
    uint32_t sampleBudget = 0;
    int algorithmVersion = 0;
};

enum class CudaSurfacePreparationStatus {
    Ok, InvalidArgument, InvalidTopology, NonFiniteInput,
    UnsupportedFeature, AllocationFailure
};

CudaSurfacePreparationStatus PrepareCudaSurface(
    UsdGenSurfaceDesc const&, uint32_t sampleBudget, int algorithmVersion,
    CudaSurfacePrepared*, std::vector<std::string>* diagnostics = nullptr);

bool RestBindingMatches(CudaSurfacePrepared const&, CudaSurfacePrepared const&);

} // namespace usdGen
#endif
