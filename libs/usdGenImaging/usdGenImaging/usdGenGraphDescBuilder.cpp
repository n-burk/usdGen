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
            std::string const name = rel.GetBaseName().GetString();
            if (name == "usdGen:input") {
                SdfPathVector targets;
                rel.GetTargets(&targets);
                for (SdfPath const &target : targets) {
                    Walk(stage->GetPrimAtPath(target));
                }
            } else if (name == "usdGen:guides") {
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
    _GetTyped(mesh.GetPointsAttr(), UsdTimeCode(time), &out->points);
    _GetPrimvarTyped(meshPrim, TfToken("rest"), UsdTimeCode::Default(),
                     &out->restPoints);
    if (out->restPoints.empty()) {
        // S12: no authored rest -> the Default-time deformed opinion IS the
        // rest (the UsdGenRestAPI adapter publishes the same fallback).
        _GetTyped(mesh.GetPointsAttr(), UsdTimeCode::Default(),
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

}  // namespace

usdGen::UsdGenGraphDesc
BuildGraphDesc(
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
            if (name == "usdGen:input") {
                bucket = &node.inputs;
            } else if (name == "usdGen:guides" || name == "usdGen:curves" ||
                       name == "usdGen:frozen:curves") {
                bucket = &node.curves;
            } else if (name == "usdGen:surface") {
                bucket = &node.surfaces;
            } else if (name == "usdGen:mask:source" || name == "usdGen:map") {
                bucket = &node.maps;
            } else if (name.compare(0, 7, "usdGen:") != 0) {
                continue;  // not ours
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
    _GetToken(descPrim, "usdGeom:purpose", &desc.purpose);
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

}  // namespace usdGenImaging
