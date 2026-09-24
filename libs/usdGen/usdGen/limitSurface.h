#ifndef USDGEN_LIMIT_SURFACE_H
#define USDGEN_LIMIT_SURFACE_H
#include "usdGen/graphDesc.h"
#include "usdGen/export.h"
#include <memory>
#include <string>
namespace usdGen {
// Patch evaluation, not interpolation of a finitely subdivided polygon cage.
// Current Scatter admission is quad Catmull-Clark, preserving rootPrim/rootUV
// as coarse-face/Ptex coordinates without changing the binding ABI.
class USDGEN_CORE_API UsdGenLimitSurface {
public:
    UsdGenLimitSurface();
    ~UsdGenLimitSurface();
    bool Build(UsdGenSurfaceDesc const&, int isolationLevel, std::string* error);
    bool Evaluate(int face, float u, float v, pxr::GfVec3f* p,
                  pxr::GfVec3f* du, pxr::GfVec3f* dv) const;
    bool IsHole(int face) const;
private:
    struct State;
    std::unique_ptr<State> _state;
};
USDGEN_CORE_API uint64_t UsdGenSubdivisionDigest(UsdGenSurfaceDesc const&);
}
#endif
