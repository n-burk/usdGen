#ifndef USDGEN_FUR_OCCLUSION_H
#define USDGEN_FUR_OCCLUSION_H

#include "usdGen/curveBuffer.h"

namespace usdGen {
// Geometry-derived optical depth towards +/- world X/Y/Z. Reserved vertex
// primvars furTauP/furTauN, six floats per CV. All tiles participate in one volume so tile
// boundaries do not become lighting boundaries. No light/camera dependency.
// Returns false for unchanged geometry and shares the previous COW planes.
// previous must have been built at the same resolution; pass null to change it.
bool UsdGenBuildFurOcclusion(std::vector<UsdGenTilePublication>* tiles,
    std::vector<UsdGenTilePublication> const* previous = nullptr,
    int resolution = 48);
}
#endif
