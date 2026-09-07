// probe_usdcat_extent.cpp
//
// M0 exit gate: "usdcat --flatten on the M0 fixture reports a non-empty extent
// on UsdGenDescription". usdcat itself prints no extent, so this probe is the
// faithful stand-in: it links ONLY stock OpenUSD (usd/usdGeom + deps), NEVER
// links libusdGenSchema, and relies entirely on PXR_PLUGINPATH_NAME so that
// Plug dlopens the build-tree schema plugin (Type:library,
// implementsComputeExtent:true, LibraryPath:../../../libusdGenSchema.so) on
// demand and calls our _ComputeDescriptionExtent.
//
// If a non-empty 2-point extent comes back, the whole
//  plugInfo -> LibraryPath -> dlopen -> TF_REGISTRY_FUNCTION(UsdGeomBoundable)
//  -> UsdGeomRegisterComputeExtentFunction
// chain is proven to fire in a plain tool-style process.
//
// TYPE-RESOLUTION EVIDENCE (SOL C-11): flattened authored type-name tokens are
// NOT proof the codeless types resolved — a fixture that simply names
// `def UsdGenGroom` / `def UsdGenDescription` will flatten those tokens even
// when the schema plugin never loads (that prim is just an unknown type). To
// distinguish real Plug registration from unknown authored tokens, this probe
// also asserts, for BOTH codeless types:
//   * TfType::FindByName(name) is not TfType::Unknown()   (type is registered)
//   * UsdSchemaRegistry::IsConcrete(name) is true         (schema says concrete)
// with the negative control (no plugin dir on PXR_PLUGINPATH_NAME) expected to
// leave both unknown / non-concrete, and the positive control (plugin dir on
// the path) expected to register + concrete.
#include "pxr/pxr.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usdGeom/boundable.h"
#include "pxr/usd/usdGeom/boundableComputeExtent.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/type.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/sdf/path.h"

#include <cstdio>
#include <cstdlib>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Reports registration + concreteness for one codeless type. Returns 0 when
// the type is resolved-and-concrete, 1 otherwise (and prints the observed
// state so a negative control reads as "unknown / not concrete").
int
CheckCodelessType(const TfToken& typeName)
{
    const TfType type = TfType::FindByName(typeName);
    const bool unknown = type.IsUnknown();
    const bool concrete = !unknown && UsdSchemaRegistry::IsConcrete(typeName);
    std::printf(
        "type-check %-20s TfType::FindByName known=%d  UsdSchemaRegistry::IsConcrete=%d\n",
        typeName.GetString().c_str(),
        unknown ? 0 : 1,
        concrete ? 1 : 0);
    return (unknown || !concrete) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: probe_usdcat_extent <fixture.usda>\n");
        return 2;
    }

    UsdStageRefPtr stage = UsdStage::Open(std::string(argv[1]));
    if (!stage) {
        std::printf("FAIL: could not open stage\n");
        return 1;
    }

    UsdPrim desc = stage->GetPrimAtPath(SdfPath("/Groom/Description"));
    if (!desc.IsValid()) {
        std::printf("FAIL: /Groom/Description not found\n");
        return 1;
    }
    std::printf("prim type = %s\n", desc.GetTypeName().GetString().c_str());

    // --- C-11: type-resolution assertions.
    //     NOTE: codeless schema types become visible to TfType/UsdSchemaRegistry
    //     only once SdrCatalog has run plugin discovery, which is triggered by
    //     the prim/path resolution above (a bare UsdStage::Open does NOT
    //     consult the type dictionary). These checks therefore run AFTER
    //     GetPrimAtPath. The negative control (no usdGen plugin dir on
    //     PXR_PLUGINPATH_NAME) must read known=0/concrete=0 here even though
    //     the fixture's authored `def UsdGenGroom` / `def UsdGenDescription`
    //     tokens are present in the layer.
    // ---
    const int groomTc   = CheckCodelessType(TfToken("UsdGenGroom"));
    const int descTc    = CheckCodelessType(TfToken("UsdGenDescription"));

    VtVec3fArray extent;
    const bool ok = UsdGeomBoundable(desc).ComputeExtent(UsdTimeCode(), &extent);
    std::printf("ComputeExtent ok=%d size=%zu\n", static_cast<int>(ok), extent.size());
    if (!ok || extent.size() != 2) {
        // In a negative control the extent legitimately cannot fire; the probe
        // still reports it distinctly from a positive result.
        std::printf("FAIL: no extent returned (plugin did not fire?)\n");
        return 1;
    }
    const GfVec3f lo = extent[0], hi = extent[1];
    const bool empty = (lo == GfVec3f(0, 0, 0)) && (hi == GfVec3f(0, 0, 0));
    std::printf("extent = [(%.3f %.3f %.3f) .. (%.3f %.3f %.3f)] empty=%d\n",
                static_cast<double>(lo[0]), static_cast<double>(lo[1]), static_cast<double>(lo[2]),
                static_cast<double>(hi[0]), static_cast<double>(hi[1]), static_cast<double>(hi[2]),
                empty ? 1 : 0);
    if (empty) {
        std::printf("FAIL: extent is empty\n");
        return 1;
    }

    // A positive control must BOTH fire the extent AND resolve the types.
    if (groomTc != 0 || descTc != 0) {
        std::printf("FAIL: codeless types not resolved+concrete despite extent\n");
        return 1;
    }
    std::printf("PASS: non-empty extent + codeless types registered & concrete\n");
    return 0;
}
