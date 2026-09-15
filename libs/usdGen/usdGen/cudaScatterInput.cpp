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

bool AllowedScatterParam(TfToken const& name) {
    static std::set<TfToken> const allowed{
        TfToken("mode"), TfToken("density"), TfToken("flip"),
        TfToken("mask:amount"), TfToken("mask:invert"),
        TfToken("mask:combine"), TfToken("mask:random"),
        TfToken("mask:randomSeed"), TfToken("mask:rangeMin"),
        TfToken("mask:rangeMax"), TfToken("mask:effectPosition"),
        TfToken("mask:falloff"), TfToken("mask:influenceWidth"),
        TfToken("mask:ramp:knots"), TfToken("mask:ramp:interpolation")};
    return allowed.count(name) != 0;
}

bool IsToken(VtValue const& v) { return v.IsHolding<TfToken>() || v.IsHolding<std::string>(); }
bool IsFloat(VtValue const& v) { return v.IsHolding<float>() || v.IsHolding<double>(); }
bool IsInt(VtValue const& v) { return v.IsHolding<int>() || v.IsHolding<uint32_t>(); }
bool IntEquals(VtValue const& v, int expected) {
    return IsInt(v) && (v.IsHolding<int>() ? v.UncheckedGet<int>() == expected
                                           : v.UncheckedGet<uint32_t>() == uint32_t(expected));
}
bool FloatEquals(VtValue const& v, double expected) {
    return IsFloat(v) && (v.IsHolding<float>() ? double(v.UncheckedGet<float>()) == expected
                                               : v.UncheckedGet<double>() == expected);
}
bool FiniteFloat(VtValue const& v) {
    return IsFloat(v) && std::isfinite(v.IsHolding<float>() ? double(v.UncheckedGet<float>())
                                                            : v.UncheckedGet<double>());
}
// Controls this slice does not implement are admitted only at their neutral
// schema fallback. The imaging adapter serves every schema property, so a
// stage-authored Scatter always carries all of them; the random kernel reads
// none of the uniform/points/atGuides controls, and a neutral mask is a no-op.
bool IdentityControl(UsdGenParamValue const& param) {
    TfToken const& n = param.name; VtValue const& v = param.value;
    if (n == TfToken("relaxIterations")) return IntEquals(v, 0);
    if (n == TfToken("jitter")) return FloatEquals(v, 0.0);
    if (n == TfToken("areaCompensation")) return v == VtValue(true);
    if (n == TfToken("perGuide")) return IntEquals(v, 1);
    if (n == TfToken("spacingU") || n == TfToken("spacingV")) return FiniteFloat(v);
    if (n == TfToken("rootPrims")) return v.IsHolding<VtIntArray>() && v.UncheckedGet<VtIntArray>().empty();
    if (n == TfToken("rootUVs")) return v.IsHolding<VtVec2fArray>() && v.UncheckedGet<VtVec2fArray>().empty();
    if (n == TfToken("label")) return v.IsHolding<std::string>();
    if (n == TfToken("mask:range")) return v == VtValue(GfVec2f(0, 1));
    if (n == TfToken("mask:rangeMode")) return IsToken(v) && (v.IsHolding<TfToken>()
        ? v.UncheckedGet<TfToken>() == TfToken("normalized") : v.UncheckedGet<std::string>() == "normalized");
    if (n == TfToken("mask:noise:amount")) return FloatEquals(v, 0.0);
    if (n == TfToken("mask:noise:frequency")) return FloatEquals(v, 1.0);
    if (n == TfToken("mask:noise:gain") || n == TfToken("mask:noise:bias")) return FloatEquals(v, 0.5);
    if (n == TfToken("mask:noise:seed")) return IntEquals(v, 0);
    return false;
}
bool ValidScatterParam(UsdGenParamValue const& param) {
    TfToken const& n = param.name; VtValue const& v = param.value;
    if (n == TfToken("mode") || n == TfToken("mask:combine") ||
        n == TfToken("mask:ramp:interpolation")) return IsToken(v);
    if (n == TfToken("density") || n == TfToken("mask:amount") ||
        n == TfToken("mask:random") || n == TfToken("mask:rangeMin") ||
        n == TfToken("mask:rangeMax") || n == TfToken("mask:effectPosition") ||
        n == TfToken("mask:falloff") || n == TfToken("mask:influenceWidth")) return IsFloat(v);
    if (n == TfToken("flip") || n == TfToken("mask:invert")) return v.IsHolding<bool>();
    if (n == TfToken("mask:randomSeed")) return IsInt(v);
    return n == TfToken("mask:ramp:knots") && v.IsHolding<VtVec2fArray>();
}

// The authoritative M1 mask currently evaluates only its random term.
// Its diagnostic vector is not propagated by Scatter::Capture, so accepting
// non-neutral remaining controls here would silently ignore authored intent.
bool SupportedMaskValue(UsdGenParamValue const& param) {
    auto const& name = param.name.GetString();
    if (name.compare(0, 5, "mask:") != 0) return true;
    auto const& value = param.value;
    auto number = [&] { return value.IsHolding<float>()
        ? double(value.UncheckedGet<float>()) : value.UncheckedGet<double>(); };
    auto token = [&] { return value.IsHolding<TfToken>()
        ? value.UncheckedGet<TfToken>().GetString() : value.UncheckedGet<std::string>(); };
    if (name == "mask:randomSeed") return true;
    if (name == "mask:random") {
        double const random = number();
        return std::isfinite(random) && random >= 0 && random <= 1;
    }
    if (name == "mask:invert") return !value.UncheckedGet<bool>();
    if (name == "mask:combine") return token() == "multiply";
    if (name == "mask:ramp:interpolation") {
        auto const t = token();
        return t == "constant" || t == "linear" || t == "catmullRom" || t == "bspline";
    }
    if (name == "mask:ramp:knots") {
        float previous = -1;
        for (auto const& knot : value.UncheckedGet<VtVec2fArray>()) {
            if (!Finite(knot) || knot[0] < 0 || knot[0] > 1 ||
                knot[0] < previous || knot[1] != 1) return false;
            previous = knot[0];
        }
        return true;
    }
    double const neutral = name == "mask:amount" || name == "mask:rangeMax"
        ? 1.0 : name == "mask:rangeMin" ? 0.0 : 0.5;
    return number() == neutral;
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
    if (node.algorithmVersion != 0 || !node.enabled || node.blend != 1.0f ||
        (!node.mode.IsEmpty() && node.mode != TfToken("random")))
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input supports enabled random algorithm version 0 only", reason);
    if (!node.inputs.empty() || !node.references.empty() || !node.curves.empty() ||
        !node.maps.empty() || !node.mapBindings.empty() || !node.expressionBindings.empty() ||
        !node.ramps.empty())
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input does not support geometry/reference/map/expression/ramp controls", reason);
    if ((!node.space.IsEmpty() && node.space != TfToken("auto") && node.space != TfToken("rest")) ||
        (!node.readPhase.IsEmpty() && node.readPhase != TfToken("final")))
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input supports rest/auto space and final read phase only", reason);
    if (node.surfaces.size() != 1)
        return Fail(CudaScatterInputStatus::InvalidArgument,
                    "Scatter input requires exactly one surface path", reason);
    std::set<TfToken> seen;
    for (UsdGenParamValue const& param : node.params) {
        bool const allowed = AllowedScatterParam(param.name);
        if (param.animated || !seen.insert(param.name).second ||
            (!allowed && !IdentityControl(param)) ||
            (allowed && (!ValidScatterParam(param) || !SupportedMaskValue(param))))
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
    if (params.GetToken(TfToken("mode"), TfToken("random")) != TfToken("random"))
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input supports mode=random only", reason);
    if (params.GetBool(TfToken("flip"), false))
        return Fail(CudaScatterInputStatus::Unsupported,
                    "Scatter input does not yet support flip frame handedness", reason);
    int version = -1;
    std::unique_ptr<UsdGenOp> op = UsdGenOpRegistry::Get().Create(
        TfToken("UsdGenScatter"), node.algorithmVersion, &version);
    if (!op || version != 0 || op->GeometryInputArity() != 0)
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
