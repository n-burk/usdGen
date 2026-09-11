// usdGen imaging — stage-sourced graph description builder implementation.
//
// This translation unit is the offline UsdStage oracle. It is deliberately
// separate from the production Hydra builder so the production object has no
// UsdStage dependencies (B-2/V2-11).
#include "usdGenImaging/usdGenGraphDescBuilderStage.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/sdf/assetPath.h"
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

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

template <class T>
void
_GetDedicated(UsdAttribute const &attr, UsdTimeCode time, T *out,
              std::vector<std::string> *errors, SdfPath const &path,
              char const *name)
{
    if (!attr) return;
    VtValue value;
    if (!attr.Get(&value, time) || value.IsEmpty()) return;
    if (value.IsHolding<T>()) {
        *out = value.UncheckedGet<T>();
        return;
    }
    errors->push_back(path.GetString() + ": " + name + " has wrong authored type");
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

// Do not use _GetToken here.  The compatibility contract treats a genuinely
// absent property as the legacy CPU baseline, but a schema property with its
// resolved "cuda" fallback is still an explicit CUDA selection.  Inspect the
// raw VtValue and reject every present value except the approved token.
usdGen::UsdGenExecutionBackend
_ResolveExecutionBackend(UsdPrim const &prim)
{
    UsdAttribute const attr =
        prim.GetAttribute(TfToken("usdGen:execution:backend"));
    if (!attr) {
        return usdGen::UsdGenExecutionBackend::CpuReference;
    }
    VtValue value;
    if (!attr.Get(&value, UsdTimeCode::Default()) ||
        !value.IsHolding<TfToken>()) {
        return usdGen::UsdGenExecutionBackend::Invalid;
    }
    if (value.UncheckedGet<TfToken>() == TfToken("cuda")) {
        return usdGen::UsdGenExecutionBackend::Cuda;
    }
    return usdGen::UsdGenExecutionBackend::Invalid;
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

// Offline oracle equivalent of the Description aggregate.  GetChildren()
// returns final composed child order; reversing at every level gives the
// required lower-sibling-first post-order.  Scopes are traversal-only.
void
_AppendStageOperatorPostOrder(UsdPrim const &prim, SdfPathVector *out)
{
    std::vector<UsdPrim> children;
    for (UsdPrim const &child : prim.GetChildren()) children.push_back(child);
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
        _AppendStageOperatorPostOrder(*child, out);
    }
    if (prim.IsA(TfToken("UsdGenOperator"))) {
        out->push_back(prim.GetPath());
    }
}

SdfPathVector
_StageOperatorOrder(UsdPrim const &description)
{
    SdfPathVector out;
    UsdPrim const ops = description.GetChild(TfToken("Ops"));
    if (!ops) {
        return out;
    }
    std::vector<UsdPrim> children;
    for (UsdPrim const &child : ops.GetChildren()) children.push_back(child);
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
        _AppendStageOperatorPostOrder(*child, &out);
    }
    return out;
}

usdGen::expr::ValueShape
_ExpressionShape(SdfValueTypeName const &type)
{
    usdGen::expr::ValueShape shape;
    TfToken const scalar = type.GetScalarType().GetAsToken();
    shape.isArray = type.IsArray();
    if (scalar == TfToken("bool")) shape.scalar = usdGen::expr::ScalarType::Bool;
    else if (scalar == TfToken("int")) shape.scalar = usdGen::expr::ScalarType::Int32;
    else if (scalar == TfToken("uint")) shape.scalar = usdGen::expr::ScalarType::UInt32;
    else if (scalar == TfToken("int64")) shape.scalar = usdGen::expr::ScalarType::Int64;
    else if (scalar == TfToken("uint64")) shape.scalar = usdGen::expr::ScalarType::UInt64;
    else if (scalar == TfToken("half")) shape.scalar = usdGen::expr::ScalarType::Float16;
    else if (scalar == TfToken("float")) shape.scalar = usdGen::expr::ScalarType::Float32;
    else if (scalar == TfToken("double")) shape.scalar = usdGen::expr::ScalarType::Float64;
    else return shape;
    std::string const n = type.GetAsToken().GetString();
    if (!n.empty() && n.back() >= '2' && n.back() <= '4') {
        shape.components = uint32_t(n.back() - '0');
    }
    return shape;
}

usdGen::expr::Domain
_ExpressionDomain(UsdAttribute const &attr)
{
    VtValue v = attr.GetCustomDataByKey(TfToken("usdGen:evaluation"));
    TfToken const d = v.IsHolding<TfToken>() ? v.UncheckedGet<TfToken>() :
        (v.IsHolding<std::string>() ? TfToken(v.UncheckedGet<std::string>()) : TfToken("groom"));
    if (d == TfToken("primitive")) return usdGen::expr::Domain::Primitive;
    if (d == TfToken("point")) return usdGen::expr::Domain::Point;
    return usdGen::expr::Domain::Groom;
}

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
    if (!curves) return;
    out->path = path;
    out->role = role;

    _GetTyped(curves.GetTypeAttr(), UsdTimeCode(time), &out->type);
    _GetTyped(curves.GetBasisAttr(), UsdTimeCode(time), &out->basis);
    _GetTyped(curves.GetWrapAttr(), UsdTimeCode(time), &out->wrap);
    out->widthsInterpolation = curves.GetWidthsInterpolation();

    _GetTyped(curves.GetCurveVertexCountsAttr(), UsdTimeCode(time),
              &out->curveVertexCounts);
    _GetTyped(curves.GetPointsAttr(), UsdTimeCode(time), &out->points);
    _GetPrimvarTyped(prim, TfToken("rest"), UsdTimeCode::Default(), &out->rest);
    if (out->rest.empty()) {
        _GetTyped(curves.GetPointsAttr(), UsdTimeCode::Default(), &out->rest);
    }
    UsdGeomPrimvar widthsPrimvar = UsdGeomPrimvarsAPI(prim).GetPrimvar(TfToken("widths"));
    if (widthsPrimvar && widthsPrimvar.HasValue()) {
        widthsPrimvar.Get(&out->widths, UsdTimeCode(time));
        out->widthsInterpolation = widthsPrimvar.GetInterpolation();
    } else {
        _GetTyped(curves.GetWidthsAttr(), UsdTimeCode(time), &out->widths);
    }
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
    } else if (epoch.IsHolding<std::string>()) {
        out->frozenEpoch = epoch.UncheckedGet<std::string>();
    } else if (epoch.IsHolding<TfToken>()) {
        out->frozenEpoch = epoch.UncheckedGet<TfToken>().GetString();
    }

    // guideBlend: usdGen:blend on UsdGenGuideSet prims, per guide.
    _GetPrimvarTyped(prim, TfToken("usdGen:blend"), UsdTimeCode(time),
                     &out->guideBlend);
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

    SdfPathVector const operatorOrder = _StageOperatorOrder(descPrim);
    if (operatorOrder.empty()) {
        TF_CODING_ERROR("usdGen: %s has no Ops operators.",
                        descriptionPath.GetText());
        return desc;
    }
    desc.terminal = operatorOrder.back();
    std::unordered_map<std::string, UsdGenRole> curveRoles;

    // Expressions are providers, never stack nodes.  Preserve native output
    // type and source verbatim; compiler policy decides whether evaluation is
    // available, rather than silently discarding a connected authoring edit.
    if (UsdPrim const expressions = descPrim.GetChild(TfToken("Expressions"))) {
        for (UsdPrim const &prim : expressions.GetChildren()) {
            if (prim.GetPrimTypeInfo().GetTypeName() != TfToken("UsdGenExpression")) continue;
            usdGen::UsdGenExpressionDesc e;
            e.path = prim.GetPath();
            _GetTyped(prim.GetAttribute(TfToken("usdGen:expr:source")),
                      UsdTimeCode::Default(), &e.source);
            for (UsdAttribute const &a : prim.GetAttributes()) {
                std::string const n = a.GetName().GetString();
                if (n.rfind("outputs:", 0) != 0) continue;
                usdGen::UsdGenExpressionOutputDesc output;
                output.name = TfToken(n.substr(8));
                output.nativeType = a.GetTypeName().GetAsToken();
                output.shape = _ExpressionShape(a.GetTypeName());
                e.outputs.push_back(std::move(output));
            }
            desc.expressions.push_back(std::move(e));
        }
    }

    // ---- nodes, composed reverse-sibling post-order ----------------------
    for (SdfPath const &operatorPath : operatorOrder) {
        UsdPrim const prim = stage->GetPrimAtPath(operatorPath);
        UsdGenNodeDesc node;
        node.path = prim.GetPath();
        TfToken type;
        _GetToken(prim, "usdGen:type", &type);
        node.type = type.IsEmpty() ? prim.GetPrimTypeInfo().GetTypeName() : type;
        _GetToken(prim, "usdGen:mode", &node.mode);
        _GetToken(prim, "usdGen:space", &node.space);
        _GetToken(prim, "usdGen:readPhase", &node.readPhase);
        _GetDedicated(prim.GetAttribute(TfToken("usdGen:algorithmVersion")),
                  UsdTimeCode::Default(), &node.algorithmVersion, &desc.validationErrors, node.path, "usdGen:algorithmVersion");
        bool enabled = true;
        _GetDedicated(prim.GetAttribute(TfToken("usdGen:enabled")),
                  UsdTimeCode::Default(), &enabled, &desc.validationErrors, node.path, "usdGen:enabled");
        node.enabled = enabled;
        _GetDedicated(prim.GetAttribute(TfToken("usdGen:seed")),
                  UsdTimeCode::Default(), &node.seed, &desc.validationErrors, node.path, "usdGen:seed");
        _GetDedicated(prim.GetAttribute(TfToken("usdGen:blend")),
                  UsdTimeCode::Default(), &node.blend, &desc.validationErrors, node.path, "usdGen:blend");

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
                if (name == "guides") {
                    curveRoles[t.GetString()] = UsdGenRole::Reference;
                }
            }
        }

        node.inputs.clear();
        if (!desc.nodes.empty()) {
            node.inputs.push_back(desc.nodes.back().path);
        }

        _PullParams(prim, time, &node.params);
        for (UsdAttribute const &a : prim.GetAttributes()) {
            SdfPathVector connections;
            a.GetConnections(&connections);
            for (SdfPath const &c : connections) {
                TfToken const prop = c.GetNameToken();
                std::string const output = prop.GetString();
                usdGen::UsdGenExpressionBinding binding;
                binding.expression = c.GetPrimPath();
                binding.output = output.rfind("outputs:", 0) == 0
                    ? TfToken(output.substr(8)) : TfToken();
                binding.destination = a.GetName();
                binding.nativeType = a.GetTypeName().GetAsToken();
                binding.destinationShape = _ExpressionShape(a.GetTypeName());
                binding.domain = _ExpressionDomain(a);
                a.Get(&binding.literal, UsdTimeCode(time));
                node.expressionBindings.push_back(std::move(binding));
            }
        }
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
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:densityScale")),
              UsdTimeCode::Default(), &desc.densityScale);
    _GetTyped(descPrim.GetAttribute(TfToken("usdGen:renderDensityScale")),
              UsdTimeCode::Default(), &desc.renderDensityScale);
    _GetDedicated(descPrim.GetAttribute(TfToken("usdGen:width:default")),
              UsdTimeCode(options.time), &desc.defaultWidth, &desc.validationErrors, desc.description, "usdGen:width:default");
    _GetDedicated(descPrim.GetAttribute(TfToken("usdGen:tileTarget")),
              UsdTimeCode::Default(), &desc.tileTarget, &desc.validationErrors, desc.description, "usdGen:tileTarget");
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

    desc.executionBackend = _ResolveExecutionBackend(descPrim.GetParent());

    return desc;
}


}  // namespace usdGenImaging
