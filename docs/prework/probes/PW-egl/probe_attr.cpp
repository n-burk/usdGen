#include "pxr/pxr.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/base/vt/array.h"
#include <cstdio>
PXR_NAMESPACE_USING_DIRECTIVE
int main(int argc, char** argv) {
    UsdStageRefPtr stage = UsdStage::Open(std::string(argv[1]));
    UsdGeomBasisCurves curves(stage->GetPrimAtPath(SdfPath("/World/Hair")));
    for (const auto& a : curves.GetPrim().GetAttributes()) {
        printf("  attr: %s\n", a.GetName().GetString().c_str());
    }
    UsdAttribute at = curves.GetPrim().GetAttribute(TfToken("primvars:hairTangent"));
    printf("GetAttribute(primvars:hairTangent) valid=%d\n", at.IsValid() ? 1 : 0);
    UsdAttribute a2 = curves.GetPrim().GetAttribute(TfToken("hairTangent"));
    printf("GetAttribute(hairTangent) valid=%d\n", a2.IsValid() ? 1 : 0);

    if (at.IsValid()) {
        VtVec3fArray v;
        bool ok = at.Get(&v);
        printf("Get ok=%d size=%zu\n", ok ? 1 : 0, v.size());
    }
    return 0;
}
