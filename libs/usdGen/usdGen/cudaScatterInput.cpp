#include "usdGen/cudaScatterInput.h"

#include "usdGen/opRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace usdGen {
namespace {

CudaScatterInputStatus Fail(CudaScatterInputStatus status, std::string message,
                            std::string* reason) {
    if (reason) *reason = std::move(message);
    return status;
}

bool Finite(GfVec3f const& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
bool Finite(GfVec2f const& v) { return std::isfinite(v[0]) && std::isfinite(v[1]); }

// Scatter is a generator: it declares only density and flip.
bool AllowedScatterParam(TfToken const& name) {
    return name == TfToken("density") || name == TfToken("flip") || name == TfToken("subdivisionLevel");
}

bool IsFloat(VtValue const& v) { return v.IsHolding<float>() || v.IsHolding<double>(); }

bool ValidScatterParam(UsdGenParamValue const& param) {
    if(param.name==TfToken("subdivisionLevel"))
        return param.value.IsHolding<int>() && param.value.UncheckedGet<int>()==0;
    if (param.name == TfToken("density")) return IsFloat(param.value);
    return param.name == TfToken("flip") && param.value.IsHolding<bool>();
}

CudaScatterInputStatus ValidateSurface(UsdGenSurfaceDesc const& surface,
                                       std::string* reason) {
    if (surface.faceVertexCounts.empty()) {
        if (!surface.faceVertexIndices.empty())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has indices but no faces", reason);
        if (!surface.uv.empty() && surface.uv.size() != surface.restPoints.size())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface UV cardinality differs from rest positions", reason);
        for (GfVec3f const& p : surface.restPoints)
            if (!Finite(p)) return Fail(CudaScatterInputStatus::InvalidSurface,
                                        "Scatter surface has non-finite rest position", reason);
        for (GfVec2f const& uv : surface.uv)
            if (!Finite(uv)) return Fail(CudaScatterInputStatus::InvalidSurface,
                                         "Scatter surface has non-finite UV", reason);
        return CudaScatterInputStatus::Ok;
    }
    if (surface.restPoints.empty())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface faces require rest positions", reason);
    if (!surface.uv.empty() && surface.uv.size() != surface.restPoints.size())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface UV cardinality differs from rest positions", reason);
    size_t corners = 0;
    for (int count : surface.faceVertexCounts) {
        if (count < 3 || size_t(count) > std::numeric_limits<size_t>::max() - corners)
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has invalid face cardinality", reason);
        corners += size_t(count);
    }
    if (corners != surface.faceVertexIndices.size())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter surface face-index cardinality differs from face counts", reason);
    for (GfVec3f const& p : surface.restPoints)
        if (!Finite(p)) return Fail(CudaScatterInputStatus::InvalidSurface,
                                    "Scatter surface has non-finite rest position", reason);
    for (GfVec2f const& uv : surface.uv)
        if (!Finite(uv)) return Fail(CudaScatterInputStatus::InvalidSurface,
                                     "Scatter surface has non-finite UV", reason);
    for (int index : surface.faceVertexIndices)
        if (index < 0 || size_t(index) >= surface.restPoints.size())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has out-of-range face index", reason);
    std::set<int> subset;
    for (int face : surface.subsetFaces)
        if (face < 0 || size_t(face) >= surface.faceVertexCounts.size())
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has out-of-range subset face", reason);
        else if (!subset.insert(face).second)
            return Fail(CudaScatterInputStatus::InvalidSurface,
                        "Scatter surface has duplicate subset face", reason);
    return CudaScatterInputStatus::Ok;
}

} // namespace

CudaScatterInputStatus PrepareCudaScatterInput(
    UsdGenGraphDesc const& desc, SdfPath const& scatterPath,
    std::shared_ptr<const gpu::ScatterGrowRoots>* out, std::string* reason) {
    if (reason) reason->clear();
    if (!out || scatterPath.IsEmpty())
        return Fail(CudaScatterInputStatus::InvalidArgument,
                    "Scatter input needs an output owner and node path", reason);
    auto nodeIt = std::find_if(desc.nodes.begin(), desc.nodes.end(),
        [&](UsdGenNodeDesc const& node) { return node.path == scatterPath; });
    if (nodeIt == desc.nodes.end() || nodeIt->type != TfToken("UsdGenScatter"))
        return Fail(CudaScatterInputStatus::InvalidArgument,
                    "Scatter input path does not resolve to a Scatter node", reason);
    UsdGenNodeDesc const& node = *nodeIt;
    // 02 §6.3: a disabled generator publishes an EMPTY curve set, so the
    // description publishes no curves. Every operator downstream of the empty
    // root set is a no-op, which is exactly the CPU lane's behaviour. Any
    // disabled generator in the description empties this root set: Grow and
    // CurveSource are fused into the same native slice and have no separate
    // producer to empty.
    {
        usdGenRegisterM1Operators();
        bool generatorDisabled = false;
        for (UsdGenNodeDesc const& candidate : desc.nodes) {
            if (candidate.enabled) continue;
            std::unique_ptr<UsdGenOp> probe =
                UsdGenOpRegistry::Get().Create(candidate.type);
            generatorDisabled = generatorDisabled || (probe && probe->IsGenerator());
        }
        if (generatorDisabled) {
            *out = std::make_shared<gpu::ScatterGrowRoots>();
            return CudaScatterInputStatus::Ok;
        }
    }
    if (!node.mode.IsEmpty())
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter has no usdGen:mode property; it is always random", reason);
    if (!node.inputs.empty() || !node.references.empty() || !node.curves.empty() ||
        !node.maps.empty() || !node.mapBindings.empty() || !node.expressionBindings.empty() ||
        !node.ramps.empty())
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input does not support geometry/reference/map/expression/ramp controls", reason);
    if (node.surfaces.size() != 1)
        return Fail(CudaScatterInputStatus::InvalidArgument,
                    "Scatter input requires exactly one surface path", reason);
    std::set<TfToken> seen;
    for (UsdGenParamValue const& param : node.params) {
        if (param.animated || !seen.insert(param.name).second ||
            !AllowedScatterParam(param.name) || !ValidScatterParam(param))
            return Fail(CudaScatterInputStatus::Unsupported,
                        "Scatter input has unsupported, duplicate, or animated parameter '" +
                        param.name.GetString() + "'", reason);
    }
    auto surfaceIt = std::find_if(desc.surfaces.begin(), desc.surfaces.end(),
        [&](UsdGenSurfaceDesc const& surface) { return surface.path == node.surfaces.front(); });
    if (surfaceIt == desc.surfaces.end())
        return Fail(CudaScatterInputStatus::InvalidSurface,
                    "Scatter input cannot resolve its surface path", reason);
    CudaScatterInputStatus validated = ValidateSurface(*surfaceIt, reason);
    if (validated != CudaScatterInputStatus::Ok) return validated;
    if (surfaceIt->restFromCurrentPoints)
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input requires authored rest points, not current points", reason);

    UsdGenParamView params{&desc, &node};
    if (params.GetBool(TfToken("flip"), false))
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input does not yet support flip frame handedness", reason);
    std::unique_ptr<UsdGenOp> op = UsdGenOpRegistry::Get().Create(TfToken("UsdGenScatter"));
    if (!op || op->GeometryInputArity() != 0)
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input cannot resolve the authoritative random Scatter kernel", reason);
    UsdGenDiagnostics diagnostics;
    if (!op->Bind(params, &diagnostics))
        return Fail(CudaScatterInputStatus::CaptureFailed,
                    diagnostics.errors.empty() ? "Scatter Bind failed" : diagnostics.errors.front(), reason);
    std::unique_ptr<UsdGenCapture> capture = op->CreateCapture();
    if (!capture)
        return Fail(CudaScatterInputStatus::CaptureFailed,
                    "Scatter kernel did not create capture storage", reason);
    UsdGenCurveBuffer emptyUpstream;
    UsdGenCaptureContext context;
    context.desc = &desc;
    context.params = &params;
    context.surface = static_cast<UsdGenSurfaceId>(surfaceIt - desc.surfaces.begin());
    context.seed = static_cast<uint32_t>(node.seed);
    context.diag = &diagnostics;
    if (!op->Capture(context, emptyUpstream, capture.get(), &diagnostics))
        return Fail(CudaScatterInputStatus::CaptureFailed,
                    diagnostics.errors.empty() ? "Scatter Capture failed" : diagnostics.errors.front(), reason);
    UsdGenCurveBuffer const& roots = capture->Buffer();
    size_t const n = roots.totalCurves;
    if (roots.totalCvs != n || roots.px.size() != n || roots.py.size() != n ||
        roots.pz.size() != n || roots.curveId.size() != n || roots.rootPrim.size() != n ||
        roots.rootUV.size() != n || roots.rootT.size() != n || roots.rootB.size() != n ||
        roots.rootN.size() != n)
        return Fail(CudaScatterInputStatus::CaptureFailed,
                    "Scatter Capture produced an invalid root topology", reason);
    auto prepared = std::make_shared<gpu::ScatterGrowRoots>();
    prepared->positions.reserve(n); prepared->stableIds.reserve(n); prepared->rootPrim.reserve(n);
    prepared->rootUV.reserve(n); prepared->rootT.reserve(n); prepared->rootB.reserve(n); prepared->rootN.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        prepared->positions.push_back(make_float3(roots.px[i], roots.py[i], roots.pz[i]));
        prepared->stableIds.push_back(roots.curveId[i]); prepared->rootPrim.push_back(roots.rootPrim[i]);
        prepared->rootUV.push_back(make_float2(roots.rootUV[i][0], roots.rootUV[i][1]));
        prepared->rootT.push_back(make_float3(roots.rootT[i][0], roots.rootT[i][1], roots.rootT[i][2]));
        prepared->rootB.push_back(make_float3(roots.rootB[i][0], roots.rootB[i][1], roots.rootB[i][2]));
        prepared->rootN.push_back(make_float3(roots.rootN[i][0], roots.rootN[i][1], roots.rootN[i][2]));
    }
    *out = std::move(prepared);
    return CudaScatterInputStatus::Ok;
}

} // namespace usdGen
