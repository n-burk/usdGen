// usdGen imaging — UsdGenRestAPI data sourcing (02-schema.md §2.15, S12;
// 06-imaging.md §2.6).
//
// The RestAPI container publishes the REST surface of a bound Mesh under
// the `usdGen` container of that prim's imaging data source:
//   usdGen/rest/points              VtVec3fArray
//   usdGen/rest/faceVertexCounts    VtIntArray
//   usdGen/rest/faceVertexIndices   VtIntArray
//   usdGen/rest/st                  VtVec2fArray (primary uv set, may be empty)
//
// Rest points are `primvars:rest` when authored (the Houdini convention,
// S12 — it passes through the chain untouched), otherwise the deformed
// `points` opinion at UsdTimeCode::Default(). Both are read ONCE, at
// Default time, into retained data sources: the rest channel is never
// flagged time-varying, so a SetTime costs it no per-frame dirty
// (MEASURED, research/G-stage-free-parameter-and-time-transport.md §3).
// Capture-on-first-cook is rejected (S12): the first drawn frame is not
// necessarily the rest frame.
//
// UsdGenRestAPI may be applied only to the parent Mesh, never to a
// GeomSubset (ADR §9 R15): a subset carries only face indices into the
// parent mesh. A geomSubset target is a hard diagnostic, reported once per
// prim (06 §9 diagnostic 2), and no container is published.
#include "usdGenImaging/usdGenRestApiDataSource.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/ts/spline.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/primvar.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/subset.h"

#include "usdGenImaging/usdGenTokens.h"

#include <unordered_set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

namespace {

// One hard diagnostic per prim, not per pull (06 §9: "emitted once per groom
// per compile"; a scene-index pull happens many times a frame).
bool
_WarnOnce(std::unordered_set<std::string> *warned, SdfPath const &path, char const *msg)
{
    if (warned->count(path.GetText())) {
        return false;
    }
    warned->insert(path.GetText());
    TF_WARN("usdGen: %s", msg);
    return true;
}

}  // namespace

HdContainerDataSourceHandle UsdGenRestApiContainerFactory(
    UsdPrim const &prim,
    UsdImagingDataSourceStageGlobals const &globals)
{
    TF_UNUSED(globals);

    static std::unordered_set<std::string> _warned;

    // R15: a GeomSubset target is the sibling case of hard diagnostic 2.
    if (prim.IsA<UsdGeomSubset>()) {
        _WarnOnce(&_warned, prim.GetPath(),
            ("usdGen: UsdGenRestAPI applied to geomSubset " +
             std::string(prim.GetPath().GetText()) +
             " - apply it to the parent Mesh instead (ADR §9 R15).").c_str());
        return nullptr;
    }

    if (!prim.IsA<UsdGeomMesh>()) {
        std::string text = "usdGen: UsdGenRestAPI applied to '" +
                           prim.GetPrimTypeInfo().GetTypeName().GetString() +
                           "' at " + prim.GetPath().GetText() +
                           " — expected a UsdGenDescription-bound Mesh surface.";
        _WarnOnce(&_warned, prim.GetPath(), text.c_str());
        return nullptr;
    }

    UsdGeomMesh mesh(prim);
    const UsdGeomPrimvarsAPI primvars(prim);
    const UsdTimeCode rest = UsdTimeCode::Default();

    // points: authored primvars:rest wins; otherwise the Default-time
    // deformed opinion (S12).
    VtVec3fArray restPoints;
    if (UsdGeomPrimvar restPrimvar = primvars.GetPrimvar(TfToken("rest"));
        restPrimvar && restPrimvar.HasValue()) {
        restPrimvar.Get(&restPoints, rest);
    }
    if (restPoints.empty()) {
        VtValue pointsValue;
        if (mesh.GetPointsAttr().Get(&pointsValue, rest) &&
            pointsValue.IsHolding<VtVec3fArray>()) {
            restPoints = pointsValue.UncheckedGet<VtVec3fArray>();
        }
    }

    VtValue fvcValue, fviValue;
    mesh.GetFaceVertexCountsAttr().Get(&fvcValue, rest);
    mesh.GetFaceVertexIndicesAttr().Get(&fviValue, rest);
    const VtIntArray faceVertexCounts =
        fvcValue.IsHolding<VtIntArray>() ? fvcValue.UncheckedGet<VtIntArray>() : VtIntArray();
    const VtIntArray faceVertexIndices =
        fviValue.IsHolding<VtIntArray>() ? fviValue.UncheckedGet<VtIntArray>() : VtIntArray();

    VtVec2fArray uv;
    if (UsdGeomPrimvar stPrimvar = primvars.GetPrimvar(TfToken("st"));
        stPrimvar && stPrimvar.HasValue()) {
        stPrimvar.Get(&uv, rest);
    }

    HdSampledDataSourceHandle pointsSrc = HdRetainedTypedSampledDataSource<VtVec3fArray>::New(restPoints);
    HdSampledDataSourceHandle countsSrc = HdRetainedTypedSampledDataSource<VtIntArray>::New(faceVertexCounts);
    HdSampledDataSourceHandle indicesSrc = HdRetainedTypedSampledDataSource<VtIntArray>::New(faceVertexIndices);
    HdSampledDataSourceHandle uvSrc = HdRetainedTypedSampledDataSource<VtVec2fArray>::New(uv);

    // Build the retained tree by hand: usdGen/rest/{points,faceVertexCounts,
    // faceVertexIndices,st}. Retained sources are never time-varying, so the
    // container inherits the rest channel's zero per-frame cost (S12).
    TfTokenVector restNames{TfToken("points"), TfToken("faceVertexCounts"), TfToken("faceVertexIndices"), TfToken("st")};
    HdDataSourceBaseHandle restValues[4] = {
        pointsSrc, countsSrc, indicesSrc, uvSrc};
    HdRetainedContainerDataSourceHandle restContainer =
        HdRetainedContainerDataSource::New(4, restNames.data(), restValues);

    HdDataSourceBaseHandle rootValues[1] = { restContainer };
    return HdRetainedContainerDataSource::New(
        1, &UsdGenContainerToken(), rootValues);
}

HdDataSourceLocatorSet UsdGenRestApiInvalidateMapping(
    UsdPrim const &prim,
    TfTokenVector const &properties,
    UsdImagingPropertyInvalidationType invalidationType)
{
    TF_UNUSED(prim);
    TF_UNUSED(invalidationType);

    HdDataSourceLocatorSet result;
    static HdDataSourceLocator const points{
        TfToken("usdGen"), TfToken("rest"), TfToken("points")};
    static HdDataSourceLocator const fvc{
        TfToken("usdGen"), TfToken("rest"), TfToken("faceVertexCounts")};
    static HdDataSourceLocator const fvi{
        TfToken("usdGen"), TfToken("rest"), TfToken("faceVertexIndices")};
    static HdDataSourceLocator const st{
        TfToken("usdGen"), TfToken("rest"), TfToken("st")};

    for (TfToken const &property : properties) {
        const std::string p = property.GetString();
        if (p == "points" || p == "primvars:rest") {
            result.insert(points);
        } else if (p == "faceVertexCounts") {
            result.insert(fvc);
        } else if (p == "faceVertexIndices") {
            result.insert(fvi);
        } else if (p == "st" || p == "primvars:st") {
            result.insert(st);
        } else if (p.rfind("usdGen:rest:", 0) == 0) {
            // The API's own authored knobs (source/primvar/file, S12) all
            // reach the evaluator through the points leaf; the file asset
            // path additionally dirty-points via the asset dependency.
            result.insert(points);
        }
    }
    return result;
}

}  // namespace usdGenImaging
