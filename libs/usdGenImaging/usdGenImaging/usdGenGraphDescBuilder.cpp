// usdGen imaging — graph description builder implementation.
//
// The engine must never see a UsdStage (S8; link-enforced by B-1), so
// everything the graph needs is pulled to pure values here (06 §3.10,
// 03 §2.5). Missing properties keep the C1 defaults already materialized in
// the UsdGenGraphDesc member initializers; the S14 "pull everything" rule is
// honored by sweeping every usdGen:* attribute of every participating prim
// into UsdGenNodeDesc::params, whether or not a dedicated field exists.
#include "usdGenImaging/usdGenGraphDescBuilder.h"
#include "usdGenImaging/usdGenTokens.h"

#include "pxr/usd/sdf/assetPath.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/imageable.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/primvar.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"

#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hd/geomSubsetSchema.h"
#include "pxr/imaging/hd/materialBindingSchema.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/purposeSchema.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/xformSchema.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

namespace {

using usdGen::UsdGenCurveSetDesc;
using usdGen::UsdGenGraphDesc;
using usdGen::UsdGenNodeDesc;
using usdGen::UsdGenParamValue;
using usdGen::UsdGenRole;
using usdGen::UsdGenLookDesc;
using usdGen::UsdGenSurfaceDesc;
using usdGen::UsdGenSurfaceSample;

// Attributes that already own a dedicated desc field; everything else
// reaches the engine through params (S14 pull-all).
bool
_isDedicated(TfToken const &name)
{
    static std::unordered_set<std::string> const dedicated{
        "usdGen:type", "usdGen:mode", "usdGen:algorithmVersion",
        "usdGen:enabled", "usdGen:seed", "usdGen:blend", "usdGen:space",
        "usdGen:readPhase", "usdGen:input", "usdGen:terminal",
        "usdGen:references", "usdGen:guides", "usdGen:curves",
        "usdGen:frozen:curves", "usdGen:surface", "usdGen:mask:source",
        "usdGen:map",
        // description-level dedicated fields
        "usdGen:densityScale", "usdGen:renderDensityScale",
        "usdGen:tileTarget", "usdGen:pickTarget", "usdGen:curve:basis",
        "usdGen:motion:mode", "usdGen:motion:sampleCount",
    };
    // usdGen:look:* lives in UsdGenLookDesc, not params.
    return dedicated.count(name.GetString()) != 0 ||
           name.GetString().rfind("usdGen:look:", 0) == 0;
}

template <class T>
bool
_GetTyped(UsdAttribute const &attr, UsdTimeCode time, T *out)
{
    if (!attr) {
        return false;
    }
    VtValue value;
    if (!attr.Get(&value, time) || value.IsEmpty()) {
        return false;
    }
    if (value.IsHolding<T>()) {
        *out = value.UncheckedGet<T>();
        return true;
    }
    if (value.CanCast<T>()) {
        *out = value.template Cast<T>().template UncheckedGet<T>();
        return true;
    }
    return false;
}

/// point3f[] → VtVec3fArray elementwise conversion. USD mesh points author as point3f[];
/// the engine surface descs carry VtVec3fArray.
// rest/points pulls come back empty, Scatter captures 0 roots and the
// commit publishes 0 tiles (surgery gen=0 root cause family).
bool
_GetVec3fArray(UsdAttribute const &attr, UsdTimeCode time, VtVec3fArray *out)
{
    if (!attr) {
        return false;
    }
    VtValue value;
    if (!attr.Get(&value, time) || value.IsEmpty()) {
        return false;
    }
    if (value.IsHolding<VtVec3fArray>()) {
        *out = value.UncheckedGet<VtVec3fArray>();
        return true;
    }
    return _GetTyped(attr, time, out);
}

void
_GetToken(UsdPrim const &prim, char const *name, TfToken *out)
{
    TfToken v;
    if (_GetTyped(prim.GetAttribute(TfToken(name)), UsdTimeCode::Default(), &v)
        && !v.IsEmpty()) {
        *out = v;
    }
}

void
_GetPrimvar(UsdPrim const &prim, TfToken const &name, UsdTimeCode time,
            VtValue *out)
{
    UsdGeomPrimvar pv = UsdGeomPrimvarsAPI(prim).GetPrimvar(name);
    if (!pv) {
        return;
    }
    pv.Get(out, time);
    if (out->IsEmpty() && time != UsdTimeCode::Default()) {
        pv.Get(out, UsdTimeCode::Default());
    }
}

template <class T>
void
_GetPrimvarTyped(UsdPrim const &prim, TfToken const &name, UsdTimeCode time,
                 T *out)
{
    VtValue v;
    _GetPrimvar(prim, name, time, &v);
    if (v.IsHolding<T>()) {
        *out = v.UncheckedGet<T>();
    } else if (v.CanCast<T>()) {
        *out = v.template Cast<T>().template UncheckedGet<T>();
    }
}

void
_PullParams(UsdPrim const &prim, double time,
            std::vector<UsdGenParamValue> *params)
{
    for (TfToken const &attrName : prim.GetPrimDefinition().GetPropertyNames()) {
        std::string const s = attrName.GetString();
        if (s.compare(0, 7, "usdGen:") != 0 || _isDedicated(attrName)) {
            continue;
        }
        UsdAttribute const attr = prim.GetAttribute(attrName);
        if (!attr) {
            continue;  // a relationship carries the name, not an attribute
        }
        VtValue value;
        if (!attr.Get(&value, UsdTimeCode(time)) || value.IsEmpty()) {
            continue;  // no authored/default opinion at all
        }
        UsdGenParamValue p;
        p.name = TfToken(s.substr(7));  // strip "usdGen:" (02 §0.7)
        p.value = std::move(value);
        p.animated = attr.ValueMightBeTimeVarying();
        params->push_back(std::move(p));
    }
    // ramps (S11) stay empty here: the capture adapter resolves ramp
    // encodings at capture time (graphDesc.h: "already resolved by the
    // adapter"); the knot/position/color attributes themselves were swept
    // into params above, so S14's pull-all contract holds.
}

// Follows usdGen:input edges back from the terminal, collecting the operator
// set. Relationships are NOT traversed as data edges (02 §2.3).
struct _GraphWalker
{
    UsdStageRefPtr stage;
    std::unordered_set<std::string> operatorPaths;
    std::unordered_set<std::string> visited;
    // guides rels mark their curve sets as the Reference lane (I3)
    std::unordered_map<std::string, UsdGenRole> curveRoles;

    void Walk(UsdPrim const &prim)
    {
        if (!prim || !visited.insert(prim.GetPath().GetString()).second) {
            return;
        }
        operatorPaths.insert(prim.GetPath().GetString());
        for (UsdRelationship const &rel : prim.GetRelationships()) {
            // GetBaseName strips namespaces ("usdGen:input" -> "input").
            std::string const name = rel.GetBaseName().GetString();
            if (name == "input") {
                SdfPathVector targets;
                rel.GetTargets(&targets);
                for (SdfPath const &target : targets) {
                    Walk(stage->GetPrimAtPath(target));
                }
            } else if (name == "guides") {
                SdfPathVector targets;
                rel.GetTargets(&targets);
                for (SdfPath const &target : targets) {
                    curveRoles[target.GetString()] = UsdGenRole::Reference;
                }
            }
        }
    }
};

// Fills a surface desc from a Mesh; a GeomSubset target instead records
// subsetFaces onto the parent mesh's desc (R15; caller handles the parent).
void
_BuildSurface(UsdStageRefPtr const &stage, SdfPath const &path, double time,
              UsdGenSurfaceDesc *out)
{
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) {
        TF_CODING_ERROR("usdGen: usdGen:surface target %s does not exist.",
                        path.GetText());
        return;
    }
    UsdGeomMesh mesh(prim);
    if (!mesh) {
        TF_CODING_ERROR("usdGen: usdGen:surface target %s is a %s; M1 "
                        "surfaces must be Mesh or GeomSubset (R15).",
                        path.GetText(),
                        prim.GetPrimTypeInfo().GetTypeName().GetText());
        return;
    }
    UsdPrim const meshPrim = mesh.GetPrim();

    out->path = path;
    _GetTyped(mesh.GetFaceVertexCountsAttr(), UsdTimeCode::Default(),
              &out->faceVertexCounts);
    _GetTyped(mesh.GetFaceVertexIndicesAttr(), UsdTimeCode::Default(),
              &out->faceVertexIndices);
    _GetVec3fArray(mesh.GetPointsAttr(), UsdTimeCode(time), &out->points);
    _GetPrimvarTyped(meshPrim, TfToken("rest"), UsdTimeCode::Default(),
                     &out->restPoints);
    if (out->restPoints.empty()) {
        // S12: no authored rest -> the Default-time deformed opinion IS the
        // rest (the UsdGenRestAPI adapter publishes the same fallback).
        _GetVec3fArray(mesh.GetPointsAttr(), UsdTimeCode::Default(),
                       &out->restPoints);
    }
    _GetPrimvarTyped(meshPrim, TfToken("st"), UsdTimeCode::Default(), &out->uv);
    _GetPrimvarTyped(meshPrim, TfToken("velocities"), UsdTimeCode(time),
                     &out->velocities);  // motion profile P1 only

    out->samples.push_back(UsdGenSurfaceSample{time, out->points});
    out->worldMatrix = UsdGeomImageable(prim).ComputeLocalToWorldTransform(
        UsdTimeCode(time));
}

void
_BuildCurveSet(UsdStageRefPtr const &stage, SdfPath const &path,
               UsdGenRole role, double time, UsdGenCurveSetDesc *out)
{
    UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim) {
        TF_CODING_ERROR("usdGen: curve set %s does not exist.",
                        path.GetText());
        return;
    }
    UsdGeomBasisCurves curves(prim);
    out->path = path;
    out->role = role;

    _GetTyped(curves.GetCurveVertexCountsAttr(), UsdTimeCode(time),
              &out->curveVertexCounts);
    _GetTyped(curves.GetPointsAttr(), UsdTimeCode(time), &out->points);
    _GetPrimvarTyped(prim, TfToken("rest"), UsdTimeCode::Default(), &out->rest);
    if (out->rest.empty()) {
        _GetTyped(curves.GetPointsAttr(), UsdTimeCode::Default(), &out->rest);
    }
    _GetTyped(curves.GetWidthsAttr(), UsdTimeCode(time), &out->widths);
    _GetPrimvarTyped(prim, TfToken("skinprim"), UsdTimeCode(time),
                     &out->skinPrim);
    _GetPrimvarTyped(prim, TfToken("usdGen:curveId"), UsdTimeCode(time),
                     &out->curveId);
    _GetPrimvarTyped(prim, TfToken("skinprimuv"), UsdTimeCode(time),
                     &out->skinPrimUv);
    _GetPrimvarTyped(prim, TfToken("usdGen:rootFrame"), UsdTimeCode(time),
                     &out->rootFrame);

    TfToken curveRole;
    _GetPrimvarTyped(prim, TfToken("usdGen:role"), UsdTimeCode::Default(),
                     &curveRole);
    if (curveRole.IsEmpty()) {
        _GetToken(prim, "usdGen:role", &curveRole);
    }
    if (!curveRole.IsEmpty()) {
        out->curveRole = curveRole;
    }

    // frozenEpoch: constant string primvar "usdgen1:sha1:..." (S42).
    VtValue epoch;
    _GetPrimvar(prim, TfToken("usdGen:frozenEpoch"), UsdTimeCode::Default(),
                &epoch);
    if (epoch.IsHolding<VtStringArray>()) {
        VtStringArray const &arr = epoch.UncheckedGet<VtStringArray>();
        if (!arr.empty()) {
            out->frozenEpoch = arr[0].c_str();
        }
    } else if (epoch.IsHolding<TfToken>()) {
        out->frozenEpoch = epoch.UncheckedGet<TfToken>().GetString();
    }

    // guideBlend: usdGen:blend on UsdGenGuideSet prims, per guide.
    _GetPrimvarTyped(prim, TfToken("usdGen:blend"), UsdTimeCode(time),
                     &out->guideBlend);
}

// ---- Hydra-sourced reads (production path, 13 §7 V2-9 homing table) -----
//
// Mirror of the stage helpers above, reading UsdImaging data sources instead
// of Usd prims. Locator contract: the adapter overlays its mapped source at
// the prim root, so adapter-published usdGen:* properties are served FLAT
// with 02 §0.7 relative elements (usdGen:mask:source -> mask/source); an
// ancestor in a name-collision pair takes a "-value" (attribute) / "-rel"
// (relationship) suffix on its final element (primAdapter.cpp
// LocatorForProperty), which _HLocate retries transparently. Stock geometry
// arrives through the standard Hydra schemas (points, primvars,
// xform/matrix, purpose, visibility, materialBindings); the final scene
// index is flattened, so xform/matrix is already the world matrix (S4),
// matching ComputeLocalToWorldTransform. The S14 sweep prunes stock
// subtrees via _HIsStock (no usdGen prefix survives the overlay flattening).

using _HdTime = HdSampledDataSource::Time;

_HdTime
_HTime(double t)
{
    return static_cast<_HdTime>(t);
}

bool
_HPrim(HdSceneIndexBase &input, SdfPath const &path,
       HdContainerDataSourceHandle *outDs, TfToken *outType)
{
    HdSceneIndexPrim const prim = input.GetPrim(path);
    if (!prim.dataSource) {
        return false;
    }
    *outDs = prim.dataSource;
    if (outType) {
        *outType = prim.primType;
    }
    return true;
}

HdContainerDataSourceHandle
_HChild(HdContainerDataSourceHandle const &parent, char const *name)
{
    if (!parent) {
        return nullptr;
    }
    return HdContainerDataSource::Cast(parent->Get(TfToken(name)));
}

HdContainerDataSourceHandle
_HUsdGen(HdContainerDataSourceHandle const &primDs)
{
    // The adapter overlays its mapped source at the prim root
    // (OverlayedContainerDataSources(usdGen, base)), so mapped usdGen:*
    // properties are served FLAT: usdGen:terminal -> `terminal`,
    // usdGen:motion:mode -> motion/mode. There is no `usdGen` container in
    // the served tree (the `usdGen` prefix lives only on invalidation
    // locators). The mapped root IS the prim data source; stock subtrees
    // sharing it are pruned by _HIsStock during the S14 sweep.
    return primDs;
}

// Stock-served top-level names sharing the overlaid prim root. The S14 sweep
// must not mistake them for mapped usdGen:* properties (the stage builder's
// prefix filter has no Hydra equivalent once the overlay flattens serving).
bool
_HIsStock(std::string const &name)
{
    static std::unordered_set<std::string> const stock{
        "primvars", "xform", "mesh", "basisCurves", "geomSubset",
        "points", "widths", "visibility", "purpose", "materialBindings",
        "extent", "proxyPrim", "xformOpOrder", "model", "geomModel",
        "primOrigin", "__usdPrimInfo", "__usdUpAxis", "skelBinding",
        "coordSysBinding", "usdMaterialBindings", "displayStyle",
        "categories",
    };
    if (stock.count(name) != 0) {
        return true;
    }
    return name.rfind("xformOp:", 0) == 0;
}

// USD type name without a UsdPrim (B-2: UsdImagingUsdPrimInfoSchema's symbol
// contains UsdPrim, so the nm half would bite; raw reads stay clean).
// Defined after _HGetTyped below.

// Navigate intermediate containers exactly; the final element retries the
// LocatorForProperty ancestor-deviation suffixes ("-rel", then "-value").
HdDataSourceBaseHandle
_HLocate(HdContainerDataSourceHandle const &root,
         std::initializer_list<char const*> elems)
{
    HdContainerDataSourceHandle cur = root;
    size_t i = 0;
    size_t const n = elems.size();
    for (char const *e : elems) {
        if (!cur || n == 0) {
            return nullptr;
        }
        HdDataSourceBaseHandle h = cur->Get(TfToken(e));
        if (!h) {
            std::string const b(e);
            h = cur->Get(TfToken(b + "-rel"));
            if (!h) {
                h = cur->Get(TfToken(b + "-value"));
            }
        }
        if (!h) {
            return nullptr;
        }
        if (++i == n) {
            return h;
        }
        cur = HdContainerDataSource::Cast(h);
    }
    return nullptr;
}

bool
_HSampledValue(HdContainerDataSourceHandle const &root, _HdTime t,
               VtValue *out, std::initializer_list<char const*> elems)
{
    HdSampledDataSourceHandle const s =
        HdSampledDataSource::Cast(_HLocate(root, elems));
    if (!s) {
        return false;
    }
    *out = s->GetValue(t);
    return !out->IsEmpty();
}

template <class T>
bool
_HGetTyped(HdContainerDataSourceHandle const &root, _HdTime t, T *out,
           std::initializer_list<char const*> elems)
{
    VtValue value;
    if (!_HSampledValue(root, t, &value, elems)) {
        return false;
    }
    if (value.IsHolding<T>()) {
        *out = value.UncheckedGet<T>();
        return true;
    }
    if (value.CanCast<T>()) {
        *out = value.template Cast<T>().template UncheckedGet<T>();
        return true;
    }
    return false;
}

bool
_HGetToken(HdContainerDataSourceHandle const &root, _HdTime t, TfToken *out,
           std::initializer_list<char const*> elems)
{
    TfToken v;
    if (_HGetTyped(root, t, &v, elems) && !v.IsEmpty()) {
        *out = v;
        return true;
    }
    return false;
}

TfToken
_HUsdTypeName(HdContainerDataSourceHandle const &primDs)
{
    TfToken type;
    _HGetTyped(_HChild(primDs, "__usdPrimInfo"), _HTime(0.0), &type,
               {"typeName"});
    return type;
}

// Relationship targets do not vary with time; sample them at 0.
bool
_HGetSinglePath(HdContainerDataSourceHandle const &root, SdfPath *out,
                std::initializer_list<char const*> elems)
{
    VtValue value;
    if (!_HSampledValue(root, _HTime(0.0), &value, elems)) {
        return false;
    }
    if (value.IsHolding<SdfPath>()) {
        *out = value.UncheckedGet<SdfPath>();
        return true;
    }
    return false;
}

bool
_HGetPathArray(HdContainerDataSourceHandle const &root, SdfPathVector *out,
               std::initializer_list<char const*> elems)
{
    VtValue value;
    if (!_HSampledValue(root, _HTime(0.0), &value, elems)) {
        return false;
    }
    if (value.IsHolding<VtArray<SdfPath>>()) {
        VtArray<SdfPath> const &arr = value.UncheckedGet<VtArray<SdfPath>>();
        out->assign(arr.begin(), arr.end());
        return true;
    }
    if (value.IsHolding<SdfPathVector>()) {
        *out = value.UncheckedGet<SdfPathVector>();
        return true;
    }
    if (value.IsHolding<SdfPath>()) {
        out->push_back(value.UncheckedGet<SdfPath>());
        return true;
    }
    return false;
}

bool
_HPrimvarValue(HdContainerDataSourceHandle const &primDs, char const *name,
               _HdTime t, VtValue *out)
{
    // Indexed primvars arrive flattened (primvarSchema.h); fixtures author
    // plain arrays, matching the raw stage reads.
    HdSampledDataSourceHandle const s =
        HdPrimvarsSchema::GetFromParent(primDs)
            .GetPrimvar(TfToken(name))
            .GetPrimvarValue();
    if (!s) {
        return false;
    }
    *out = s->GetValue(t);
    return !out->IsEmpty();
}

template <class T>
bool
_HPrimvarTyped(HdContainerDataSourceHandle const &primDs, char const *name,
               _HdTime t, T *out)
{
    VtValue value;
    if (!_HPrimvarValue(primDs, name, t, &value)) {
        return false;
    }
    if (value.IsHolding<T>()) {
        *out = value.UncheckedGet<T>();
        return true;
    }
    if (value.CanCast<T>()) {
        *out = value.template Cast<T>().template UncheckedGet<T>();
        return true;
    }
    return false;
}

// Suffix left by LocatorForProperty on a collision ancestor's final element.
std::string
_HStripSuffix(std::string s)
{
    for (char const *suf : {"-value", "-rel"}) {
        size_t const n = std::char_traits<char>::length(suf);
        if (s.size() > n && s.compare(s.size() - n, n, suf) == 0) {
            s.resize(s.size() - n);
            break;
        }
    }
    return s;
}

bool
_HIsPathValue(VtValue const &v)
{
    return v.IsHolding<SdfPath>() || v.IsHolding<VtArray<SdfPath>>() ||
           v.IsHolding<SdfPathVector>();
}

void
_HAppendPaths(VtValue const &v, SdfPathVector *out)
{
    if (v.IsHolding<SdfPath>()) {
        out->push_back(v.UncheckedGet<SdfPath>());
    } else if (v.IsHolding<VtArray<SdfPath>>()) {
        VtArray<SdfPath> const &arr = v.UncheckedGet<VtArray<SdfPath>>();
        out->insert(out->end(), arr.begin(), arr.end());
    } else if (v.IsHolding<SdfPathVector>()) {
        SdfPathVector const &vec = v.UncheckedGet<SdfPathVector>();
        out->insert(out->end(), vec.begin(), vec.end());
    }
}

// One recursive walk over the usdGen container fills BOTH the S14 param
// sweep (attribute leaves) and the graph edges (path-valued leaves),
// applying the stage builder's bucket switch on (base name, full name) so
// the two builders classify identically. `references` stays empty on both
// sides: the stage switch has no references case (its default bucket is
// unreachable). A null node skips edge classification (map prims: params
// only).
void
_HPullUsdGen(HdContainerDataSourceHandle const &usdGen, _HdTime t,
             UsdGenNodeDesc *node, std::vector<UsdGenParamValue> *params,
             std::string const &prefix)
{
    if (!usdGen) {
        return;
    }
    bool const top = prefix == "usdGen";
    for (TfToken const &child : usdGen->GetNames()) {
        if (top && _HIsStock(child.GetString())) {
            continue;
        }
        HdDataSourceBaseHandle const h = usdGen->Get(child);
        if (HdContainerDataSourceHandle const sub =
                HdContainerDataSource::Cast(h)) {
            _HPullUsdGen(sub, t, node, params,
                         prefix + ":" + child.GetString());
            continue;
        }
        HdSampledDataSourceHandle const s = HdSampledDataSource::Cast(h);
        if (!s) {
            continue;
        }
        VtValue const v = s->GetValue(t);
        if (v.IsEmpty()) {
            continue;  // no authored/default opinion at all
        }
        std::string const leaf = _HStripSuffix(child.GetString());
        std::string const full = prefix + ":" + leaf;
        if (_HIsPathValue(v)) {
            if (node) {
                SdfPathVector *bucket = nullptr;
                if (leaf == "input") {
                    bucket = &node->inputs;
                } else if (leaf == "guides" || leaf == "curves") {
                    bucket = &node->curves;
                } else if (leaf == "surface") {
                    bucket = &node->surfaces;
                } else if (leaf == "source" || leaf == "map") {
                    if (full == "usdGen:mask:source" ||
                        full == "usdGen:map" ||
                        full == "usdGen:length:source") {
                        bucket = &node->maps;
                    } else {
                        continue;
                    }
                } else {
                    continue;  // not a graph edge (base-name match only)
                }
                _HAppendPaths(v, bucket);
            }
            continue;
        }
        if (_isDedicated(TfToken(full))) {
            continue;
        }
        UsdGenParamValue p;
        p.name = TfToken(full.substr(7));  // strip "usdGen:" (02 §0.7)
        p.value = v;
        // P6 follow-up: HdSampledDataSource exposes no ValueMightBeTimeVarying
        // equivalent; fixtures are static so false holds. Animated-param
        // detection (e.g. via GetContributingSampleTimesForInterval) arrives
        // with the P6 animated scalp.
        p.animated = false;
        params->push_back(std::move(p));
    }
}

// Follows usdGen:input edges back from the terminal over Hydra path arrays;
// guides rels mark their curve sets as the Reference lane (I3). Mirrors
// _GraphWalker exactly (including: guides targets are roles, not operators).
struct _HydraWalker
{
    HdSceneIndexBase *input = nullptr;
    std::unordered_set<std::string> operatorPaths;
    std::unordered_set<std::string> visited;
    // guides rels mark their curve sets as the Reference lane (I3)
    std::unordered_map<std::string, UsdGenRole> curveRoles;

    HdContainerDataSourceHandle _UsdGen(SdfPath const &path)
    {
        HdSceneIndexPrim const prim = input->GetPrim(path);
        if (!prim.dataSource) {
            return nullptr;
        }
        return _HUsdGen(prim.dataSource);
    }

    void Walk(SdfPath const &path)
    {
        if (!visited.insert(path.GetString()).second) {
            return;
        }
        HdContainerDataSourceHandle const ug = _UsdGen(path);
        if (!ug) {
            return;
        }
        operatorPaths.insert(path.GetString());
        SdfPathVector targets;
        if (_HGetPathArray(ug, &targets, {"input"})) {
            for (SdfPath const &target : targets) {
                Walk(target);
            }
        }
        targets.clear();
        if (_HGetPathArray(ug, &targets, {"guides"})) {
            for (SdfPath const &target : targets) {
                curveRoles[target.GetString()] = UsdGenRole::Reference;
            }
        }
    }
};

// S26 namespace order over Hydra: depth-first pre-order from the description,
// children in scene-index order (sorted, matching USD namespace order).
void
_HCollectSubtree(HdSceneIndexBase &input, SdfPath const &root,
                 SdfPathVector *out)
{
    out->push_back(root);
    for (SdfPath const &c : input.GetChildPrimPaths(root)) {
        _HCollectSubtree(input, c, out);
    }
}

// Fills a surface desc from a Mesh prim in the flattened index. A GeomSubset
// target is detected by the caller (R15); xform/matrix is already the world
// matrix post-flattening (S4).
void
_HBuildSurface(HdSceneIndexBase &input, SdfPath const &path, double time,
               _HdTime t, UsdGenSurfaceDesc *out)
{
    HdContainerDataSourceHandle primDs;
    TfToken primType;
    if (!_HPrim(input, path, &primDs, &primType)) {
        TF_CODING_ERROR("usdGen: usdGen:surface target %s does not exist.",
                        path.GetText());
        return;
    }
    if (primType != TfToken("mesh")) {
        TF_CODING_ERROR("usdGen: usdGen:surface target %s is a %s; M1 "
                        "surfaces must be Mesh or GeomSubset (R15).",
                        path.GetText(), primType.GetText());
        return;
    }

    out->path = path;
    HdContainerDataSourceHandle const mesh = _HChild(primDs, "mesh");
    HdContainerDataSourceHandle const topo = _HChild(mesh, "topology");
    _HGetTyped(topo, t, &out->faceVertexCounts, {"faceVertexCounts"});
    _HGetTyped(topo, t, &out->faceVertexIndices, {"faceVertexIndices"});
    if (!_HGetTyped(primDs, t, &out->points, {"points"})) {
        // Flattened mesh points arrive as primvars/points (no top-level
        // points source on the final index).
        _HPrimvarTyped(primDs, "points", t, &out->points);
    }
    _HPrimvarTyped(primDs, "rest", t, &out->restPoints);
    if (out->restPoints.empty()) {
        // S12: no authored rest -> the deformed opinion IS the rest (the
        // UsdGenRestAPI adapter publishes the same fallback; static fixtures
        // read it at the sample time, matching Default-time stage reads).
        out->restPoints = out->points;
    }
    _HPrimvarTyped(primDs, "st", t, &out->uv);
    _HPrimvarTyped(primDs, "velocities", t, &out->velocities);

    out->samples.push_back(UsdGenSurfaceSample{time, out->points});
    out->worldMatrix = GfMatrix4d(1.0);
    HdMatrixDataSourceHandle const m =
        HdXformSchema::GetFromParent(primDs).GetMatrix();
    if (m) {
        out->worldMatrix = m->GetTypedValue(t);
    }
}

void
_HBuildCurveSet(HdSceneIndexBase &input, SdfPath const &path,
                UsdGenRole role, _HdTime t, UsdGenCurveSetDesc *out)
{
    HdContainerDataSourceHandle primDs;
    if (!_HPrim(input, path, &primDs, nullptr)) {
        TF_CODING_ERROR("usdGen: curve set %s does not exist.",
                        path.GetText());
        return;
    }
    HdContainerDataSourceHandle const ug = _HUsdGen(primDs);
    out->path = path;
    out->role = role;

    HdContainerDataSourceHandle const topo =
        _HChild(_HChild(primDs, "basisCurves"), "topology");
    _HGetTyped(topo, t, &out->curveVertexCounts, {"curveVertexCounts"});
    if (!_HGetTyped(primDs, t, &out->points, {"points"})) {
        _HPrimvarTyped(primDs, "points", t, &out->points);
    }
    _HPrimvarTyped(primDs, "rest", t, &out->rest);
    if (out->rest.empty()) {
        out->rest = out->points;
    }
    if (!_HGetTyped(primDs, t, &out->widths, {"widths"})) {
        _HPrimvarTyped(primDs, "widths", t, &out->widths);
    }
    _HPrimvarTyped(primDs, "skinprim", t, &out->skinPrim);
    _HPrimvarTyped(primDs, "usdGen:curveId", t, &out->curveId);
    _HPrimvarTyped(primDs, "skinprimuv", t, &out->skinPrimUv);
    _HPrimvarTyped(primDs, "usdGen:rootFrame", t, &out->rootFrame);

    TfToken curveRole;
    _HPrimvarTyped(primDs, "usdGen:role", t, &curveRole);
    if (curveRole.IsEmpty()) {
        _HGetToken(ug, t, &curveRole, {"role"});
    }
    if (!curveRole.IsEmpty()) {
        out->curveRole = curveRole;
    }

    // frozenEpoch: constant string primvar "usdgen1:sha1:..." (S42).
    VtValue epoch;
    if (_HPrimvarValue(primDs, "usdGen:frozenEpoch", t, &epoch)) {
        if (epoch.IsHolding<VtStringArray>()) {
            VtStringArray const &arr = epoch.UncheckedGet<VtStringArray>();
            if (!arr.empty()) {
                out->frozenEpoch = arr[0].c_str();
            }
        } else if (epoch.IsHolding<TfToken>()) {
            out->frozenEpoch = epoch.UncheckedGet<TfToken>().GetString();
        }
    }

    // guideBlend: usdGen:blend on UsdGenGuideSet prims, per guide.
    _HPrimvarTyped(primDs, "usdGen:blend", t, &out->guideBlend);
}

}  // namespace

usdGen::UsdGenGraphDesc
BuildGraphDescFromStage(
    UsdStageRefPtr const &stage,
    SdfPath const &descriptionPath,
    UsdGenGraphDescBuildOptions const &options)
{
    UsdGenGraphDesc desc;
    desc.description = descriptionPath;
    desc.time = options.time;
    if (!stage) {
        return desc;
    }
    UsdPrim const descPrim = stage->GetPrimAtPath(descriptionPath);
    if (!descPrim) {
        TF_CODING_ERROR("usdGen: description prim %s does not exist.",
                        descriptionPath.GetText());
        return desc;
    }
    double const time = options.time;

    // ---- terminal (02 §2.3: exactly one target) --------------------------
    {
        SdfPathVector targets;
        if (UsdRelationship rel =
                descPrim.GetRelationship(TfToken("usdGen:terminal"))) {
            rel.GetTargets(&targets);
        }
        if (targets.empty()) {
            TF_CODING_ERROR("usdGen: %s has no usdGen:terminal target.",
                            descriptionPath.GetText());
            return desc;
        }
        if (targets.size() > 1) {
            TF_CODING_ERROR("usdGen: usdGen:terminal on %s must have exactly "
                            "one target; found %zu (first two: %s, %s).",
                            descriptionPath.GetText(), targets.size(),
                            targets[0].GetText(), targets[1].GetText());
        }
        desc.terminal = targets[0];
    }

    // ---- operator discovery: usdGen:input edges back from the terminal ---
    _GraphWalker walker;
    walker.stage = stage;
    walker.Walk(stage->GetPrimAtPath(desc.terminal));

    // ---- nodes, stage namespace order (S26 Kahn tie-break) ---------------
    for (UsdPrim const &prim : UsdPrimRange(descPrim)) {
        if (!walker.operatorPaths.count(prim.GetPath().GetString())) {
            continue;
        }
        UsdGenNodeDesc node;
        node.path = prim.GetPath();
        TfToken type;
        _GetToken(prim, "usdGen:type", &type);
        node.type = type.IsEmpty() ? prim.GetPrimTypeInfo().GetTypeName() : type;
        _GetToken(prim, "usdGen:mode", &node.mode);
        _GetToken(prim, "usdGen:space", &node.space);
        _GetToken(prim, "usdGen:readPhase", &node.readPhase);
        _GetTyped(prim.GetAttribute(TfToken("usdGen:algorithmVersion")),
                  UsdTimeCode::Default(), &node.algorithmVersion);
        bool enabled = true;
        _GetTyped(prim.GetAttribute(TfToken("usdGen:enabled")),
                  UsdTimeCode::Default(), &enabled);
        node.enabled = enabled;
        _GetTyped(prim.GetAttribute(TfToken("usdGen:seed")),
                  UsdTimeCode::Default(), &node.seed);
        _GetTyped(prim.GetAttribute(TfToken("usdGen:blend")),
                  UsdTimeCode::Default(), &node.blend);

        for (UsdRelationship const &rel : prim.GetRelationships()) {
            std::string const name = rel.GetBaseName().GetString();
            SdfPathVector *bucket = &node.references;
            if (name == "input") {
                bucket = &node.inputs;
            } else if (name == "guides" || name == "curves" ||
                       name == "frozen:curves") {
                bucket = &node.curves;
            } else if (name == "surface") {
                bucket = &node.surfaces;
            } else if (name == "source" || name == "map") {
                // Base names collide ("mask:source" vs "length:source" both
                // → "source"): disambiguate by full relationship name.
                std::string const full = rel.GetName().GetString();
                if (full == "usdGen:mask:source" || full == "usdGen:map" ||
                    full == "usdGen:length:source") {
                    bucket = &node.maps;
                } else {
                    continue;
                }
            } else {
                continue;  // not a graph edge (base-name match only)
            }
            SdfPathVector relTargets;
            rel.GetTargets(&relTargets);
            for (SdfPath const &t : relTargets) {
                bucket->push_back(t);
            }
        }

        _PullParams(prim, time, &node.params);
        desc.nodes.push_back(std::move(node));
    }

    // Surface inheritance (02 §2): an operator with no usdGen:surface of its
    // own inherits the description's bound surface. The walker collects only
    // operator prims, so read the description prim's relationship directly.
    {
        SdfPathVector descSurfaces;
        if (UsdRelationship rel = descPrim.GetRelationship(
                TfToken("usdGen:surface"))) {
            rel.GetTargets(&descSurfaces);
        }
        if (!descSurfaces.empty()) {
            for (UsdGenNodeDesc &node : desc.nodes) {
                if (node.surfaces.empty()) {
                    node.surfaces = descSurfaces;
                }
            }
        }
    }

    // ---- shared pools: surfaces / curve sets / maps ----------------------
    std::map<std::string, size_t> surfaceIndex;
    std::map<std::string, size_t> curveIndex;
    std::map<std::string, size_t> mapIndex;

    // Returns the pool slot; ensures desc.nodes references resolve. Nodes
    // store SdfPath vectors, so the pool membership itself is the contract
    // (the engine re-derives ids in SetGraphDesc; 03 §2.5).
    auto surfaceFor = [&](SdfPath const &p) {
        if (surfaceIndex.count(p.GetString())) {
            return;
        }
        UsdGenSurfaceDesc surface;
        surface.path = p;
        surface.id = usdGen::UsdGenSurfaceId(desc.surfaces.size());
        surfaceIndex.emplace(p.GetString(), desc.surfaces.size());
        desc.surfaces.push_back(std::move(surface));
        UsdPrim prim = stage->GetPrimAtPath(p);
        if (prim && prim.IsA<UsdGeomSubset>()) {
            // R15: subsetFaces are PARENT-mesh face indices; the parent's
            // desc (added separately) carries the geometry.
            UsdGeomSubset subset(prim);
            _GetTyped(subset.GetIndicesAttr(), UsdTimeCode(time),
                      &desc.surfaces[surfaceIndex[p.GetString()]].subsetFaces);
        } else {
            _BuildSurface(stage, p, time,
                          &desc.surfaces[surfaceIndex[p.GetString()]]);
        }
    };

    auto curveFor = [&](SdfPath const &p, UsdGenRole role) {
        auto it = curveIndex.find(p.GetString());
        if (it == curveIndex.end()) {
            UsdGenCurveSetDesc cs;
            _BuildCurveSet(stage, p, role, time, &cs);
            it = curveIndex.emplace(p.GetString(), desc.curveSets.size())
                     .first;
            desc.curveSets.push_back(std::move(cs));
        } else if (role == UsdGenRole::Reference) {
            // Reference is the stronger lane claim (I3).
            desc.curveSets[it->second].role = UsdGenRole::Reference;
        }
    };

    auto mapFor = [&](SdfPath const &p) {
        if (mapIndex.count(p.GetString())) {
            return;
        }
        usdGen::UsdGenMapDesc map;
        map.path = p;
        if (UsdPrim prim = stage->GetPrimAtPath(p)) {
            TfToken type;
            _GetToken(prim, "usdGen:type", &type);
            map.type = type.IsEmpty() ? prim.GetPrimTypeInfo().GetTypeName() : type;
            SdfAssetPath asset;
            if (_GetTyped(prim.GetAttribute(TfToken("usdGen:source")),
                          UsdTimeCode::Default(), &asset)) {
                // Stage-free by value (S13): the RESOLVED path travels.
                map.resolvedAssetPath = asset.GetResolvedPath();
                if (map.resolvedAssetPath.empty()) {
                    map.resolvedAssetPath = asset.GetAssetPath();
                }
            }
            _PullParams(prim, time, &map.params);
        }
        mapIndex.emplace(p.GetString(), desc.maps.size());
        desc.maps.push_back(std::move(map));
    };

    for (UsdGenNodeDesc &node : desc.nodes) {
        for (SdfPath const &s : node.surfaces) {
            surfaceFor(s);
            UsdPrim sPrim = stage->GetPrimAtPath(s);
            if (sPrim && sPrim.IsA<UsdGeomSubset>()) {
                surfaceFor(sPrim.GetParent().GetPath());  // parent mesh too
            }
        }
        for (SdfPath const &c : node.curves) {
            UsdGenRole role = UsdGenRole::Curves;
            auto roleIt = walker.curveRoles.find(c.GetString());
            if (roleIt != walker.curveRoles.end()) {
                role = roleIt->second;
            }
            curveFor(c, role);
        }
        for (SdfPath const &m : node.maps) {
            mapFor(m);
        }
    }

    // ---- description-level fields (02 §2.3/§2.12/§2.14) -------------------
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:densityScale")),
              UsdTimeCode::Default(), &desc.densityScale);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:renderDensityScale")),
              UsdTimeCode::Default(), &desc.renderDensityScale);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:tileTarget")),
              UsdTimeCode::Default(), &desc.tileTarget);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:motion:sampleCount")),
              UsdTimeCode::Default(), &desc.motionSampleCount);
    desc.motionSampleCount = std::max(2, std::min(16, desc.motionSampleCount));
    _GetToken(descPrim, "usdGen:motion:mode", &desc.motionMode);
    _GetToken(descPrim, "usdGen:curve:basis", &desc.curveBasis);
    _GetToken(descPrim, "usdGen:pickTarget", &desc.pickTarget);

    UsdGenLookDesc &look = desc.look;
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:look:rootColor")),
              UsdTimeCode::Default(), &look.rootColor);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:look:tipColor")),
              UsdTimeCode::Default(), &look.tipColor);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:look:colorRamp:colors")),
              UsdTimeCode::Default(), &look.rampColors);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:look:colorRamp:positions")),
              UsdTimeCode::Default(), &look.rampPositions);
    _GetToken(descPrim, "usdGen:look:colorRamp:interpolation",
              &look.rampInterpolation);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:look:rampExponent")),
              UsdTimeCode::Default(), &look.rampExponent);
    _GetToken(descPrim, "usdGen:look:bakeMode", &look.bakeMode);
    _GetToken(descPrim, "usdGen:look:bakeTarget", &look.bakeTarget);
    _GetToken(descPrim, "usdGen:look:bakePrimvar", &look.bakePrimvar);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:look:hueJitter")),
              UsdTimeCode::Default(), &look.hueJitter);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:look:valueJitter")),
              UsdTimeCode::Default(), &look.valueJitter);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:look:jitterSeed")),
              UsdTimeCode::Default(), &look.jitterSeed);

    // purpose / visibility inherited by hand to every tile (C2): read the
    // description's own opinions; the engine copies them onto publications.
    // NOTE (2026-09-12): this read "usdGeom:purpose", which is not a USD
    // attribute name — the Imageable attribute is "purpose" — so the read
    // never matched. Fixed to "purpose" with one more subtlety: the schema
    // declares fallback `uniform token purpose = "default"`
    // (usdGeom/imageable.h), which UsdAttribute::Get returns when nothing is
    // authored — so only an AUTHORED opinion inherits. An unauthored purpose
    // stays EMPTY: the publisher omits the purpose container then, which
    // Hydra resolves to the geometry render tag
    // (HdSceneIndexAdapterSceneDelegate::GetRenderTag falls back to geometry
    // only when the container is absent or empty). Publishing
    // purpose="default" instead is FATAL: "default" is not a render tag, so
    // the tile matches no collection and Storm never syncs it (2026-09-12:
    // 49 published tiles, itemsDrawn == 1, zero HdStBasisCurves sync lines).
    // Visibility keeps its (valid) fallback: "inherited" is a real Hydra
    // visibility value.
    if (UsdAttribute const purposeAttr =
            descPrim.GetAttribute(TfToken("purpose"))) {
        if (purposeAttr.GetResolveInfo().HasAuthoredValueOpinion()) {
            _GetToken(descPrim, "purpose", &desc.purpose);
        }
    }
    TfToken visibility;
    _GetToken(descPrim, "visibility", &visibility);
    desc.visibility = visibility.IsEmpty() ? TfToken("inherited") : visibility;

    UsdShadeMaterialBindingAPI const binding(descPrim);
    if (UsdShadeMaterial material = binding.ComputeBoundMaterial()) {
        desc.materialPath = material.GetPath();
    }

    desc.xformMatrix = UsdGeomImageable(descPrim).ComputeLocalToWorldTransform(
        UsdTimeCode(time));

    return desc;
}

usdGen::UsdGenGraphDesc
BuildGraphDescFromHydra(
    HdSceneIndexBase &input,
    SdfPath const &descriptionPath,
    UsdGenGraphDescBuildOptions const &options)
{
    UsdGenGraphDesc desc;
    desc.description = descriptionPath;
    desc.time = options.time;
    double const time = options.time;
    _HdTime const t = _HTime(time);

    HdContainerDataSourceHandle descDs;
    if (!_HPrim(input, descriptionPath, &descDs, nullptr)) {
        TF_CODING_ERROR("usdGen: description prim %s does not exist.",
                        descriptionPath.GetText());
        return desc;
    }
    HdContainerDataSourceHandle const descUg = _HUsdGen(descDs);

    // ---- terminal (02 §2.3: exactly one target) --------------------------
    {
        SdfPath term;
        if (!_HGetSinglePath(descUg, &term, {"terminal"}) ||
            term.IsEmpty()) {
            TF_CODING_ERROR("usdGen: %s has no usdGen:terminal target.",
                            descriptionPath.GetText());
            return desc;
        }
        // Multi-target policing lives in the adapter's single-target factory;
        // the first target stages either way, matching the stage path.
        desc.terminal = term;
    }

    // ---- operator discovery: usdGen:input edges back from the terminal ---
    _HydraWalker walker;
    walker.input = &input;
    walker.Walk(desc.terminal);

    // ---- nodes, S26 namespace order (DFS pre-order from description) -----
    {
        SdfPathVector order;
        _HCollectSubtree(input, descriptionPath, &order);
        for (SdfPath const &p : order) {
            if (!walker.operatorPaths.count(p.GetString())) {
                continue;
            }
            HdContainerDataSourceHandle primDs;
            TfToken primType;
            if (!_HPrim(input, p, &primDs, &primType)) {
                continue;
            }
            HdContainerDataSourceHandle const ug = _HUsdGen(primDs);
            UsdGenNodeDesc node;
            node.path = p;
            TfToken type;
            _HGetToken(ug, t, &type, {"type"});
            if (type.IsEmpty()) {
                type = primType.IsEmpty() ? _HUsdTypeName(primDs) : primType;
            }
            node.type = type;
            _HGetToken(ug, t, &node.mode, {"mode"});
            _HGetToken(ug, t, &node.space, {"space"});
            _HGetToken(ug, t, &node.readPhase, {"readPhase"});
            _HGetTyped(ug, t, &node.algorithmVersion,
                       {"algorithmVersion"});
            bool enabled = true;
            _HGetTyped(ug, t, &enabled, {"enabled"});
            node.enabled = enabled;
            _HGetTyped(ug, t, &node.seed, {"seed"});
            _HGetTyped(ug, t, &node.blend, {"blend"});

            _HPullUsdGen(ug, t, &node, &node.params, "usdGen");
            desc.nodes.push_back(std::move(node));
        }
    }

    // ---- surface inheritance (02 §2) -------------------------------------
    {
        SdfPathVector descSurfaces;
        _HGetPathArray(descUg, &descSurfaces, {"surface"});
        if (!descSurfaces.empty()) {
            for (UsdGenNodeDesc &node : desc.nodes) {
                if (node.surfaces.empty()) {
                    node.surfaces = descSurfaces;
                }
            }
        }
    }

    // ---- shared pools: surfaces / curve sets / maps ----------------------
    std::map<std::string, size_t> surfaceIndex;
    std::map<std::string, size_t> curveIndex;
    std::map<std::string, size_t> mapIndex;

    auto surfaceFor = [&](SdfPath const &p) {
        if (surfaceIndex.count(p.GetString())) {
            return;
        }
        UsdGenSurfaceDesc surface;
        surface.path = p;
        surface.id = usdGen::UsdGenSurfaceId(desc.surfaces.size());
        surfaceIndex.emplace(p.GetString(), desc.surfaces.size());
        desc.surfaces.push_back(std::move(surface));
        UsdGenSurfaceDesc &slot = desc.surfaces[surfaceIndex[p.GetString()]];
        HdContainerDataSourceHandle primDs;
        if (_HPrim(input, p, &primDs, nullptr) &&
            HdGeomSubsetSchema::GetFromParent(primDs).IsDefined()) {
            // R15: subsetFaces are PARENT-mesh face indices; the parent's
            // desc (added separately) carries the geometry.
            HdIntArrayDataSourceHandle const idx =
                HdGeomSubsetSchema::GetFromParent(primDs).GetIndices();
            if (idx) {
                slot.subsetFaces = idx->GetTypedValue(t);
            }
        } else {
            _HBuildSurface(input, p, time, t, &slot);
        }
    };

    auto curveFor = [&](SdfPath const &p, UsdGenRole role) {
        auto it = curveIndex.find(p.GetString());
        if (it == curveIndex.end()) {
            UsdGenCurveSetDesc cs;
            _HBuildCurveSet(input, p, role, t, &cs);
            it = curveIndex.emplace(p.GetString(), desc.curveSets.size())
                     .first;
            desc.curveSets.push_back(std::move(cs));
        } else if (role == UsdGenRole::Reference) {
            // Reference is the stronger lane claim (I3).
            desc.curveSets[it->second].role = UsdGenRole::Reference;
        }
    };

    auto mapFor = [&](SdfPath const &p) {
        if (mapIndex.count(p.GetString())) {
            return;
        }
        usdGen::UsdGenMapDesc map;
        map.path = p;
        HdContainerDataSourceHandle primDs;
        TfToken primType;
        if (_HPrim(input, p, &primDs, &primType)) {
            HdContainerDataSourceHandle const ug = _HUsdGen(primDs);
            TfToken type;
            _HGetToken(ug, t, &type, {"type"});
            if (type.IsEmpty()) {
                type = primType.IsEmpty() ? _HUsdTypeName(primDs) : primType;
            }
            map.type = type;
            SdfAssetPath asset;
            if (_HGetTyped(ug, t, &asset, {"source"})) {
                // Stage-free by value (S13): the RESOLVED path travels.
                map.resolvedAssetPath = asset.GetResolvedPath();
                if (map.resolvedAssetPath.empty()) {
                    map.resolvedAssetPath = asset.GetAssetPath();
                }
            }
            _HPullUsdGen(ug, t, nullptr, &map.params, "usdGen");
        }
        mapIndex.emplace(p.GetString(), desc.maps.size());
        desc.maps.push_back(std::move(map));
    };

    for (UsdGenNodeDesc &node : desc.nodes) {
        for (SdfPath const &s : node.surfaces) {
            surfaceFor(s);
            HdContainerDataSourceHandle primDs;
            if (_HPrim(input, s, &primDs, nullptr) &&
                HdGeomSubsetSchema::GetFromParent(primDs).IsDefined()) {
                surfaceFor(s.GetParentPath());  // parent mesh too
            }
        }
        for (SdfPath const &c : node.curves) {
            UsdGenRole role = UsdGenRole::Curves;
            auto roleIt = walker.curveRoles.find(c.GetString());
            if (roleIt != walker.curveRoles.end()) {
                role = roleIt->second;
            }
            curveFor(c, role);
        }
        for (SdfPath const &m : node.maps) {
            mapFor(m);
        }
    }

    // ---- description-level fields (02 §2.3/§2.12/§2.14) -------------------
    _HGetTyped(descUg, t, &desc.densityScale, {"densityScale"});
    _HGetTyped(descUg, t, &desc.renderDensityScale, {"renderDensityScale"});
    _HGetTyped(descUg, t, &desc.tileTarget, {"tileTarget"});
    _HGetTyped(descUg, t, &desc.motionSampleCount, {"motion", "sampleCount"});
    desc.motionSampleCount = std::max(2, std::min(16, desc.motionSampleCount));
    _HGetToken(descUg, t, &desc.motionMode, {"motion", "mode"});
    _HGetToken(descUg, t, &desc.curveBasis, {"curve", "basis"});
    _HGetToken(descUg, t, &desc.pickTarget, {"pickTarget"});

    UsdGenLookDesc &look = desc.look;
    HdContainerDataSourceHandle const lookDs = _HChild(descUg, "look");
    _HGetTyped(lookDs, t, &look.rootColor, {"rootColor"});
    _HGetTyped(lookDs, t, &look.tipColor, {"tipColor"});
    _HGetTyped(lookDs, t, &look.rampColors, {"colorRamp", "colors"});
    _HGetTyped(lookDs, t, &look.rampPositions, {"colorRamp", "positions"});
    _HGetToken(lookDs, t, &look.rampInterpolation,
               {"colorRamp", "interpolation"});
    _HGetTyped(lookDs, t, &look.rampExponent, {"rampExponent"});
    _HGetToken(lookDs, t, &look.bakeMode, {"bakeMode"});
    _HGetToken(lookDs, t, &look.bakeTarget, {"bakeTarget"});
    _HGetToken(lookDs, t, &look.bakePrimvar, {"bakePrimvar"});
    _HGetTyped(lookDs, t, &look.hueJitter, {"hueJitter"});
    _HGetTyped(lookDs, t, &look.valueJitter, {"valueJitter"});
    _HGetTyped(lookDs, t, &look.jitterSeed, {"jitterSeed"});

    // Purpose keeps the V2-9a absence semantics for free: Hydra carries only
    // authored opinions, so an unauthored purpose simply has no data source
    // and the desc stays EMPTY (the publisher then omits the purpose
    // container and Storm falls back to the geometry render tag). A served
    // "default" can only be the schema fallback leaking through, never an
    // authored render tag, so it maps to empty too — mirroring the stage
    // path's HasAuthoredValueOpinion gate without a stage query.
    // Visibility keeps its (valid) fallback: "inherited" is a real Hydra
    // visibility value.
    {
        HdTokenDataSourceHandle const ps =
            HdPurposeSchema::GetFromParent(descDs).GetPurpose();
        if (ps) {
            TfToken const v = ps->GetTypedValue(t);
            if (!v.IsEmpty() && v != TfToken("default")) {
                desc.purpose = v;
            }
        }
    }
    {
        HdBoolDataSourceHandle const vs =
            HdVisibilitySchema::GetFromParent(descDs).GetVisibility();
        bool visible = true;
        if (vs) {
            visible = vs->GetTypedValue(t);
        }
        desc.visibility = visible ? TfToken("inherited") : TfToken("invisible");
    }

    {
        HdMaterialBindingSchema const b =
            HdMaterialBindingsSchema::GetFromParent(descDs)
                .GetMaterialBinding();
        HdPathDataSourceHandle const p = b.GetPath();
        if (p) {
            SdfPath const mp = p->GetTypedValue(t);
            if (!mp.IsEmpty()) {
                desc.materialPath = mp;
            }
        }
    }

    desc.xformMatrix = GfMatrix4d(1.0);
    {
        HdMatrixDataSourceHandle const m =
            HdXformSchema::GetFromParent(descDs).GetMatrix();
        if (m) {
            desc.xformMatrix = m->GetTypedValue(t);
        }
    }

    return desc;
}

}  // namespace usdGenImaging
