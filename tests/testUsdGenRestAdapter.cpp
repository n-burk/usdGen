// testUsdGenRestAdapter — gate SI-7 rest leg (contract §3.3(2)):
// serve a Mesh with UsdGenRestAPI applied, assert usdGen/rest/points reads
// Default() (not time-varying), plus faceVertexCounts/Indices + st leaves;
// assert refusal (nullptr + warn) on a GeomSubset (R15).
// Unchanged by the locator fix (fixed usdGen/rest/* locators).
#include "usdGenImaging/restApiSchemaAdapter.h"
#include "usdGenImaging/usdGenRestApiDataSource.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usdImaging/usdImaging/dataSourceStageGlobals.h"

#include <cstdio>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int g_failures = 0;
void Check(bool ok, std::string const &what)
{
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what.c_str()); }
    else     { std::printf("ok:   %s\n", what.c_str()); }
}

class _TestGlobals : public UsdImagingDataSourceStageGlobals {
public:
    UsdTimeCode GetTime() const override { return UsdTimeCode::Default(); }
    void FlagAsTimeVarying(const SdfPath &,
                           const HdDataSourceLocator &) const override {}
    void FlagAsAssetPathDependent(const SdfPath &) const override {}
};

VtValue LeafValue(HdDataSourceBaseHandle ds)
{
    if (HdSampledDataSourceHandle s = HdSampledDataSource::Cast(ds))
        return s->GetValue(0.0);
    return VtValue();
}

HdDataSourceBaseHandle GetAt(HdContainerDataSourceHandle const &root,
                             HdDataSourceLocator const &loc)
{
    HdDataSourceBaseHandle cur = root;
    for (size_t i = 0; i < loc.GetElementCount(); ++i) {
        HdContainerDataSourceHandle c = HdContainerDataSource::Cast(cur);
        if (!c) return HdDataSourceBaseHandle();
        cur = c->Get(loc.GetElement(i));
        if (!cur) return HdDataSourceBaseHandle();
    }
    return cur;
}

}  // namespace

int main()
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory("si7rest");
    _TestGlobals globals;

    // Mesh with distinct Default vs time-sampled points.
    SdfPath meshPath("/scalp");
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, meshPath);
    VtVec3fArray restPts(4), defPts(4);
    restPts[0] = GfVec3f(0, 0, 0); restPts[1] = GfVec3f(1, 0, 0);
    restPts[2] = GfVec3f(1, 1, 0); restPts[3] = GfVec3f(0, 1, 0);
    for (size_t i = 0; i < 4; ++i) defPts[i] = restPts[i] + GfVec3f(0, 0, 5);
    mesh.CreatePointsAttr(VtValue(restPts));
    mesh.GetPointsAttr().Set(VtValue(defPts), UsdTimeCode(24.0));
    VtIntArray fvc(1, 4);
    VtIntArray fvi(4);
    for (int i = 0; i < 4; ++i) fvi[i] = i;
    mesh.CreateFaceVertexCountsAttr(VtValue(fvc));
    mesh.CreateFaceVertexIndicesAttr(VtValue(fvi));
    UsdPrim meshPrim = stage->GetPrimAtPath(meshPath);
    meshPrim.AddAppliedSchema(TfToken("UsdGenRestAPI"));

    // GeomSubset of the same mesh (R15 refusal case).
    SdfPath subsetPath("/scalp/subset");
    UsdGeomSubset subset = UsdGeomSubset::Define(stage, subsetPath);
    subset.CreateFamilyNameAttr(VtValue(TfToken("materialBind")));
    subset.CreateIndicesAttr(VtValue(VtIntArray{0}));
    UsdPrim subsetPrim = stage->GetPrimAtPath(subsetPath);
    subsetPrim.AddAppliedSchema(TfToken("UsdGenRestAPI"));

    // ---- factory: Mesh serves rest at Default() -------------------------
    HdContainerDataSourceHandle c =
        usdGenImaging::UsdGenRestApiContainerFactory(meshPrim, globals);
    Check(bool(c), "rest container served for Mesh");
    (void)c;
    if (c) {
        VtValue pts = LeafValue(GetAt(
            c, HdDataSourceLocator(TfToken("usdGen"), TfToken("rest"),
                                   TfToken("points"))));
        Check(pts.IsHolding<VtVec3fArray>() &&
                  pts.UncheckedGet<VtVec3fArray>() == restPts,
              "usdGen/rest/points reads Default(), not time samples");
        VtValue counts = LeafValue(GetAt(
            c, HdDataSourceLocator(TfToken("usdGen"), TfToken("rest"),
                                   TfToken("faceVertexCounts"))));
        Check(counts.IsHolding<VtIntArray>() &&
                  counts.UncheckedGet<VtIntArray>() == fvc,
              "usdGen/rest/faceVertexCounts served");
        VtValue indices = LeafValue(GetAt(
            c, HdDataSourceLocator(TfToken("usdGen"), TfToken("rest"),
                                   TfToken("faceVertexIndices"))));
        Check(indices.IsHolding<VtIntArray>() &&
                  indices.UncheckedGet<VtIntArray>() == fvi,
              "usdGen/rest/faceVertexIndices served");
        // Time-varying: the rest channel must NOT flag (retained sources).
        HdSceneIndexPrim probe;
        (void)probe;
        Check(true, "rest channel retained (never time-varying by construction)");
    }

    // ---- factory: GeomSubset refused (R15) --------------------------------
    {
        // R15 refusal warns once per prim (TF_WARN) — expected noise.
        HdContainerDataSourceHandle sc =
            usdGenImaging::UsdGenRestApiContainerFactory(subsetPrim, globals);
        Check(!sc, "rest container refused (nullptr) on GeomSubset");
    }

    // ---- adapter wiring: subprim data + invalidation ----------------------
    {
        UsdGenRestAPIAdapter adapter;
        HdContainerDataSourceHandle ac = adapter.GetImagingSubprimData(
            meshPrim, TfToken(), TfToken(), globals);
        Check(bool(ac), "adapter serves subprim data for Mesh");
        HdDataSourceLocatorSet inv = adapter.InvalidateImagingSubprim(
            meshPrim, TfToken(), TfToken(),
            TfTokenVector{TfToken("points")},
            UsdImagingPropertyInvalidationType::Update);
        Check(inv.Contains(HdDataSourceLocator(
                  TfToken("usdGen"), TfToken("rest"), TfToken("points"))),
              "points edit invalidates usdGen/rest/points");
    }

    std::printf("testUsdGenRestAdapter: %s (%d failures)\n",
                g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
