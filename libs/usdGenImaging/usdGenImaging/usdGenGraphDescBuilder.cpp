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

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/sdf/path.h"

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
#include <cmath>
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
        "__usdGenValidationErrors",
        "primvars", "xform", "mesh", "basisCurves", "geomSubset",
        "points", "widths", "visibility", "purpose", "materialBindings",
        "extent", "proxyPrim", "xformOpOrder", "model", "geomModel",
        "primOrigin", "__usdPrimInfo", "__usdUpAxis", "skelBinding",
        "coordSysBinding", "usdMaterialBindings", "displayStyle",
        "categories",
        // Adapter-owned derived metadata is consumed explicitly by the
        // builder and must not enter S14's authored parameter sweep.
        "expressionBindings", "expressions", "operatorOrder",
        "usdGenRuntime", "usdGenCurveRest",
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

template <class T>
void
_HGetDedicated(HdContainerDataSourceHandle const &root, _HdTime t, T *out,
               std::vector<std::string> *errors, SdfPath const &path,
               char const *name, std::initializer_list<char const*> elems)
{
    VtValue value;
    if (!_HSampledValue(root, t, &value, elems)) return;
    if (value.IsHolding<T>()) {
        *out = value.UncheckedGet<T>();
        return;
    }
    errors->push_back(path.GetString() + ": " + name + " has wrong authored type");
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

// Backend selection is intentionally stricter than the ordinary token
// helpers.  Those helpers perform VtValue casts and cannot distinguish an
// absent property from a malformed value.  Hydra's mapped schema source
// includes the resolved "cuda" fallback, so only a missing leaf keeps the
// legacy CPU baseline.  Any present non-token/empty/unknown value is an
// explicit invalid request and must never fall through to CPU.
usdGen::UsdGenExecutionBackend
_HResolveExecutionBackend(HdContainerDataSourceHandle const &root,
                          _HdTime t)
{
    HdDataSourceBaseHandle const source =
        _HLocate(root, {"execution", "backend"});
    if (!source) {
        return usdGen::UsdGenExecutionBackend::CpuReference;
    }
    HdSampledDataSourceHandle const sampled =
        HdSampledDataSource::Cast(source);
    if (!sampled) {
        return usdGen::UsdGenExecutionBackend::Invalid;
    }
    VtValue const value = sampled->GetValue(t);
    if (!value.IsHolding<TfToken>()) {
        return usdGen::UsdGenExecutionBackend::Invalid;
    }
    TfToken const backend = value.UncheckedGet<TfToken>();
    if (backend == TfToken("cuda")) {
        return usdGen::UsdGenExecutionBackend::Cuda;
    }
    return usdGen::UsdGenExecutionBackend::Invalid;
}

usdGen::expr::ValueShape
_HExpressionShape(TfToken const &native)
{
    usdGen::expr::ValueShape s;
    std::string n = native.GetString();
    if (n.size() > 5 && n.substr(n.size()-5) == "Array") { s.isArray = true; n.resize(n.size()-5); }
    if (!n.empty() && n.back() >= '2' && n.back() <= '4') { s.components = n.back()-'0'; n.pop_back(); }
    if (n == "bool") s.scalar=usdGen::expr::ScalarType::Bool;
    else if (n == "int") s.scalar=usdGen::expr::ScalarType::Int32;
    else if (n == "uint") s.scalar=usdGen::expr::ScalarType::UInt32;
    else if (n == "float") s.scalar=usdGen::expr::ScalarType::Float32;
    else if (n == "double") s.scalar=usdGen::expr::ScalarType::Float64;
    return s;
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
    out->type = out->basis = out->wrap = TfToken();
    _HGetToken(topo, t, &out->type, {"type"});
    _HGetToken(topo, t, &out->basis, {"basis"});
    _HGetToken(topo, t, &out->wrap, {"wrap"});
    _HGetToken(primDs, t, &out->widthsInterpolation,
               {"primvars", "widths", "interpolation"});
    _HGetTyped(topo, t, &out->curveVertexCounts, {"curveVertexCounts"});
    if (!_HGetTyped(primDs, t, &out->points, {"points"})) {
        _HPrimvarTyped(primDs, "points", t, &out->points);
    }
    // UsdGenCurveAPI publishes a live Default-time rest source.  Presence of
    // the source, rather than array emptiness, is significant: an explicitly
    // authored empty primvar must remain empty and must not be replaced by
    // current-frame points.  The provenance leaf distinguishes that source
    // from the compatibility fallback used when the API adapter is absent.
    bool curveRestRead = false;
    bool curveRestProvenanceRead = false;
    HdContainerDataSourceHandle const curveRest =
        _HChild(primDs, "usdGenCurveRest");
    if (curveRest) {
        HdSampledDataSourceHandle const restPoints =
            HdSampledDataSource::Cast(curveRest->Get(TfToken("points")));
        if (restPoints) {
            VtValue const value = restPoints->GetValue(t);
            if (value.IsHolding<VtVec3fArray>()) {
                out->rest = value.UncheckedGet<VtVec3fArray>();
                curveRestRead = true;
            }
        }
        HdSampledDataSourceHandle const provenance =
            HdSampledDataSource::Cast(
                curveRest->Get(TfToken("hasAuthoredRest")));
        if (provenance) {
            VtValue const value = provenance->GetValue(t);
            if (value.IsHolding<bool>()) {
                // The bool records provenance, not whether the valid
                // Default-time fallback is usable.  Both authored rest and
                // Default-time points are proper C3 rest sources.
                curveRestProvenanceRead = true;
            }
        }
    }
    if (curveRestRead) {
        // A valid adapter read is never a current-frame fallback, regardless
        // of hasAuthoredRest's value.  Missing/malformed provenance remains
        // marked conservatively for CUDA admission.
        out->restFromCurrentPoints = !curveRestProvenanceRead;
    } else {
        // Missing adapter/current fallback is retained for compatibility but
        // explicitly marked so CUDA admission cannot treat it as bound rest.
        _HPrimvarTyped(primDs, "rest", t, &out->rest);
        if (out->rest.empty()) {
            out->rest = out->points;
        }
        out->restFromCurrentPoints = true;
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
        } else if (epoch.IsHolding<std::string>()) {
            out->frozenEpoch = epoch.UncheckedGet<std::string>();
        } else if (epoch.IsHolding<TfToken>()) {
            out->frozenEpoch = epoch.UncheckedGet<TfToken>().GetString();
        }
    }

    // guideBlend: usdGen:blend on UsdGenGuideSet prims, per guide.
    _HPrimvarTyped(primDs, "usdGen:blend", t, &out->guideBlend);
}

}  // namespace


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
    VtStringArray descriptionErrors;
    _HGetTyped(descDs, t, &descriptionErrors, {"__usdGenValidationErrors"});
    desc.validationErrors.insert(desc.validationErrors.end(), descriptionErrors.begin(), descriptionErrors.end());

    // The description adapter exposes the composed stage rate as a live
    // metadata source.  Inspect its raw VtValue: malformed, non-finite, and
    // non-positive rates must be diagnosed instead of reaching $time math.
    if (HdContainerDataSourceHandle const runtime =
            _HChild(descUg, "usdGenRuntime")) {
        if (HdSampledDataSourceHandle const rateSource =
                HdSampledDataSource::Cast(
                    runtime->Get(TfToken("timeCodesPerSecond")))) {
            VtValue const value = rateSource->GetValue(t);
            if (!value.IsHolding<double>()) {
                desc.validationErrors.push_back(
                    desc.description.GetString() +
                    ": timeCodesPerSecond has wrong authored type");
            } else {
                double const rate = value.UncheckedGet<double>();
                if (!std::isfinite(rate) || rate <= 0.0) {
                    desc.validationErrors.push_back(
                        desc.description.GetString() +
                        ": timeCodesPerSecond must be finite and positive");
                } else {
                    desc.timeCodesPerSecond = rate;
                }
            }
        }
    }

    // The Description adapter carries composed child order explicitly: a
    // scene-index child enumeration is not an authoring-order API.  This is
    // reverse-sibling post-order, and it is also the execution order.
    SdfPathVector operatorOrder;
    _HGetPathArray(descUg, &operatorOrder, {"operatorOrder"});
    if (operatorOrder.empty()) {
        TF_CODING_ERROR("usdGen: %s has no usdGen:operatorOrder.",
                        descriptionPath.GetText());
        return desc;
    }
    desc.terminal = operatorOrder.back();
    std::unordered_map<std::string, UsdGenRole> curveRoles;

    if (HdContainerDataSourceHandle expressions = _HChild(descUg, "expressions")) {
        for (TfToken const &name : expressions->GetNames()) {
            HdContainerDataSourceHandle const e = HdContainerDataSource::Cast(expressions->Get(name));
            usdGen::UsdGenExpressionDesc expression;
            _HGetTyped(e, t, &expression.path, {"path"});
            _HGetTyped(e, t, &expression.source, {"source"});
            if (HdContainerDataSourceHandle outputs = _HChild(e, "outputs")) for (TfToken const &on : outputs->GetNames()) {
                HdContainerDataSourceHandle const o = HdContainerDataSource::Cast(outputs->Get(on));
                usdGen::UsdGenExpressionOutputDesc output;
                _HGetToken(o, t, &output.name, {"name"});
                _HGetToken(o, t, &output.nativeType, {"nativeType"});
                output.shape = _HExpressionShape(output.nativeType);
                expression.outputs.push_back(std::move(output));
            }
            desc.expressions.push_back(std::move(expression));
        }
    }

    // ---- nodes, supplied composed reverse-sibling post-order -------------
    {
        for (SdfPath const &p : operatorOrder) {
            HdContainerDataSourceHandle primDs;
            TfToken primType;
            if (!_HPrim(input, p, &primDs, &primType)) {
                continue;
            }
            HdContainerDataSourceHandle const ug = _HUsdGen(primDs);
            VtStringArray operatorErrors;
            _HGetTyped(primDs, t, &operatorErrors, {"__usdGenValidationErrors"});
            desc.validationErrors.insert(desc.validationErrors.end(), operatorErrors.begin(), operatorErrors.end());
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
            _HGetDedicated(ug, t, &node.algorithmVersion, &desc.validationErrors,
                       node.path, "algorithmVersion", {"algorithmVersion"});
            bool enabled = true;
            _HGetDedicated(ug, t, &enabled, &desc.validationErrors,
                       node.path, "enabled", {"enabled"});
            node.enabled = enabled;
            _HGetDedicated(ug, t, &node.seed, &desc.validationErrors,
                       node.path, "seed", {"seed"});
            _HGetDedicated(ug, t, &node.blend, &desc.validationErrors,
                       node.path, "blend", {"blend"});

            _HPullUsdGen(ug, t, &node, &node.params, "usdGen");
            if (HdContainerDataSourceHandle bindings = _HChild(ug, "expressionBindings")) {
                for (TfToken const &bn : bindings->GetNames()) {
                    HdContainerDataSourceHandle const b = HdContainerDataSource::Cast(bindings->Get(bn));
                    usdGen::UsdGenExpressionBinding binding;
                    _HGetTyped(b, t, &binding.expression, {"expression"});
                    _HGetToken(b, t, &binding.output, {"output"});
                    _HGetToken(b, t, &binding.destination, {"destination"});
                    _HGetToken(b, t, &binding.nativeType, {"nativeType"});
                    binding.destinationShape = _HExpressionShape(binding.nativeType);
                    TfToken domain; _HGetToken(b, t, &domain, {"domain"});
                    binding.domain = domain == TfToken("point") ? usdGen::expr::Domain::Point :
                        (domain == TfToken("primitive") ? usdGen::expr::Domain::Primitive : usdGen::expr::Domain::Groom);
                    _HGetTyped(b, t, &binding.literal, {"literal"});
                    node.expressionBindings.push_back(std::move(binding));
                }
            }
            SdfPathVector guides;
            if (_HGetPathArray(ug, &guides, {"guides"})) {
                for (SdfPath const &guide : guides) {
                    curveRoles[guide.GetString()] = UsdGenRole::Reference;
                }
            }
            // Hierarchy is the topology contract.  Do not permit a legacy
            // authored input relationship to alter it.
            node.inputs.clear();
            if (!desc.nodes.empty()) {
                node.inputs.push_back(desc.nodes.back().path);
            }
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
            auto roleIt = curveRoles.find(c.GetString());
            if (roleIt != curveRoles.end()) {
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
    _HGetDedicated(descUg, t, &desc.defaultWidth, &desc.validationErrors,
                   desc.description, "width:default", {"width", "default"});
    _HGetDedicated(descUg, t, &desc.tileTarget, &desc.validationErrors,
                   desc.description, "tileTarget", {"tileTarget"});
    _HGetTyped(descUg, t, &desc.motionSampleCount, {"motion", "sampleCount"});
    desc.motionSampleCount = std::max(2, std::min(16, desc.motionSampleCount));
    _HGetToken(descUg, t, &desc.motionMode, {"motion", "mode"});
    _HGetToken(descUg, t, &desc.curveBasis, {"curve", "basis"});
    _HGetToken(descUg, t, &desc.pickTarget, {"pickTarget"});

    HdContainerDataSourceHandle groomDs;
    if (_HPrim(input, descriptionPath.GetParentPath(), &groomDs, nullptr)) {
        desc.executionBackend = _HResolveExecutionBackend(
            _HUsdGen(groomDs), t);
    }

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
