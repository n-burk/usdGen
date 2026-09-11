#include "usdGen/cudaExecution.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <map>

#ifdef USDGEN_ENABLE_CUDA
#include "usdGen/cudaSourceInput.h"
#include "usdGen/gpu/generation.h"
#endif

namespace usdGen {
namespace {
bool Fail(UsdGenDiagnostics* diagnostics, std::string message) {
    if (diagnostics) diagnostics->Error("CUDA: " + std::move(message));
    return false;
}
}

bool ValidateCudaGraph(UsdGenGraphDesc const& desc, UsdGenDiagnostics* diagnostics) {
#ifndef USDGEN_ENABLE_CUDA
    (void)desc;
    return Fail(diagnostics, "backend is not built");
#else
    if (desc.nodes.size() != 1 || desc.nodes.front().type != TfToken("UsdGenCurveSource"))
        return Fail(diagnostics, "session executor currently supports a single CurveSource; remaining operator integration is required");
    auto const& node = desc.nodes.front();
    if (!node.expressionBindings.empty())
        return Fail(diagnostics, "connected CurveSource controls are not yet wired to the execution-time evaluator");
    if (!node.enabled || node.blend != 1 || node.algorithmVersion < 0 || node.algorithmVersion > 1)
        return Fail(diagnostics, "unsupported CurveSource enabled/blend/algorithmVersion configuration");
    if (!node.inputs.empty() || !node.references.empty() || !node.maps.empty())
        return Fail(diagnostics, "CurveSource cannot consume an upstream/reference/map in the current executor");
    for (auto const& ramp : node.ramps) {
        if (!ramp.positions.empty() || !ramp.colors.empty() ||
            std::any_of(ramp.knots.begin(), ramp.knots.end(), [](auto const& k) { return k[1] != 1.0f || !std::isfinite(k[0]); }))
            return Fail(diagnostics, "non-identity source ramps require CUDA mask integration");
    }
    if (!desc.terminal.IsEmpty() && desc.terminal != node.path)
        return Fail(diagnostics, "terminal does not match the hierarchy source");
    if (node.curves.size() != 1)
        return Fail(diagnostics, "CurveSource requires exactly one C3 curve target");
    if (node.surfaces.size() > 1)
        return Fail(diagnostics, "CurveSource binding requires one resolved parent surface");
    if (!std::isfinite(desc.defaultWidth) || desc.defaultWidth < 0)
        return Fail(diagnostics, "description default width must be finite and non-negative");
    if (!node.mode.IsEmpty()) return Fail(diagnostics, "CurveSource has no mode property");
    UsdGenParamView params; params.desc = &desc; params.node = &node;
    if (params.GetToken(TfToken("lane"), TfToken("hair")) != TfToken("hair"))
        return Fail(diagnostics, "reference-lane CurveSource is not yet integrated");
    // Never silently ignore an authored effect just because this is a source.
    static const std::set<std::string> supported{
        "useRest", "idSource", "lane", "expectEpoch", "staleAction",
        "resampleTo", "rebind", "label"};
    static const std::map<std::string, VtValue> identityMask{
        {"mask:amount", VtValue(1.0f)}, {"mask:invert", VtValue(false)},
        {"mask:range", VtValue(GfVec2f(0,1))},
        {"mask:rangeMode", VtValue(TfToken("normalized"))},
        {"mask:combine", VtValue(TfToken("multiply"))},
        {"mask:random", VtValue(0.0f)}, {"mask:randomSeed", VtValue(0)},
        {"mask:ramp:knots", VtValue(VtVec2fArray{GfVec2f(0,1),GfVec2f(1,1)})},
        {"mask:ramp:interpolation", VtValue(TfToken("catmullRom"))},
        {"mask:rangeMin", VtValue(0.0f)}, {"mask:rangeMax", VtValue(1.0f)},
        {"mask:effectPosition", VtValue(.5f)}, {"mask:falloff", VtValue(.5f)},
        {"mask:influenceWidth", VtValue(.5f)}, {"mask:noise:amount", VtValue(0.0f)},
        {"mask:noise:frequency", VtValue(1.0f)}, {"mask:noise:gain", VtValue(.5f)},
        {"mask:noise:bias", VtValue(.5f)}, {"mask:noise:seed", VtValue(0)}
    };
    std::set<TfToken> seen;
    for (auto const& param : node.params) {
        if (!seen.insert(param.name).second)
            return Fail(diagnostics, "duplicate source parameter " + param.name.GetString());
        auto const& name = param.name.GetString();
        auto mask = identityMask.find(name);
        if (mask != identityMask.end()) {
            if (param.value != mask->second)
                return Fail(diagnostics, "non-identity mask requires CUDA mask integration: " + name);
            continue;
        }
        if (!supported.count(name))
            return Fail(diagnostics, "unsupported CurveSource parameter " + param.name.GetString());
        const bool validType = name == "useRest" ? param.value.IsHolding<bool>() :
            name == "resampleTo" ? param.value.IsHolding<int>() :
            (name == "label" || name == "expectEpoch") ? param.value.IsHolding<std::string>() :
            param.value.IsHolding<TfToken>();
        if (!validType) return Fail(diagnostics, "wrong native type for source parameter " + name);
    }
    return true;
#endif
}

std::shared_ptr<const UsdGenDeviceGeneration> ExecuteCudaGraph(
    UsdGenGraphDesc const& desc, uint64_t generation, UsdGenDiagnostics* diagnostics) {
    if (!ValidateCudaGraph(desc, diagnostics)) return {};
#ifndef USDGEN_ENABLE_CUDA
    (void)generation;
    return {};
#else
    auto const& node = desc.nodes.front();
    auto found = std::find_if(desc.curveSets.begin(), desc.curveSets.end(),
        [&](auto const& c) { return c.path == node.curves.front(); });
    if (found == desc.curveSets.end()) {
        Fail(diagnostics, "missing C3 input " + node.curves.front().GetString()); return {};
    }
    auto const& curves = *found;
    if (curves.type != TfToken("cubic") ||
        (curves.basis != TfToken("bspline") && curves.basis != TfToken("catmullRom")) ||
        (curves.wrap != TfToken("pinned") && curves.wrap != TfToken("nonperiodic"))) {
        Fail(diagnostics, "C3 requires cubic bspline/catmullRom curves with pinned/nonperiodic wrap"); return {};
    }
    if (curves.wrap == TfToken("nonperiodic") && diagnostics)
        diagnostics->Warn("CUDA CurveSource promotes nonperiodic wrap to pinned; endpoints now reach the first/last CV");
    if (!curves.widths.empty() &&
        curves.widthsInterpolation != TfToken("vertex") && curves.widthsInterpolation != TfToken("constant")) {
        Fail(diagnostics, "C3 widths must use vertex or constant interpolation"); return {};
    }
    if (!curves.widths.empty() &&
        ((curves.widthsInterpolation == TfToken("constant") && curves.widths.size() != 1) ||
         (curves.widthsInterpolation == TfToken("vertex") && curves.widths.size() != curves.points.size()))) {
        Fail(diagnostics, "C3 widths cardinality does not match interpolation"); return {};
    }
    if (curves.curveRole != TfToken("hair")) {
        Fail(diagnostics, "CurveSource requires the C3 hair role"); return {};
    }
    UsdGenParamView params; params.desc = &desc; params.node = &node;
    CudaSourcePreparationOptions options;
    options.defaultWidth = desc.defaultWidth;
    options.useRest = params.GetBool(TfToken("useRest"), true);
    if (options.useRest && !curves.points.empty() &&
        (curves.rest.empty() || curves.restFromCurrentPoints)) {
        Fail(diagnostics, "useRest requires an authored/default-time C3 rest snapshot; current-frame fallback is not a rest binding");
        return {};
    }
    auto idSource = params.GetToken(TfToken("idSource"), TfToken("primvar"));
    if (idSource != TfToken("primvar") && idSource != TfToken("index")) {
        Fail(diagnostics, "invalid idSource"); return {};
    }
    options.idSource = idSource == TfToken("index") ? CudaSourceIdSource::Index : CudaSourceIdSource::Primvar;
    auto staleAction = params.GetToken(TfToken("staleAction"), TfToken("warn"));
    if (staleAction != TfToken("warn") && staleAction != TfToken("ignore") && staleAction != TfToken("block")) {
        Fail(diagnostics, "invalid staleAction"); return {};
    }
    options.staleAction = staleAction == TfToken("block") ? CudaSourceStaleAction::Block :
        staleAction == TfToken("ignore") ? CudaSourceStaleAction::Ignore : CudaSourceStaleAction::Warn;
    auto epoch = params.GetVtValue(TfToken("expectEpoch"), VtValue(std::string{}));
    if (!epoch.IsHolding<std::string>()) { Fail(diagnostics, "expectEpoch must be a string"); return {}; }
    options.expectedEpoch = epoch.UncheckedGet<std::string>();
    options.actualEpoch = curves.frozenEpoch;
    options.resampleTo = params.GetInt(TfToken("resampleTo"), 0);
    options.rebind = params.GetToken(TfToken("rebind"), TfToken("onError")).GetString();
    const UsdGenSurfaceDesc* surface = nullptr;
    if (!node.surfaces.empty()) {
        auto s = std::find_if(desc.surfaces.begin(), desc.surfaces.end(),
            [&](auto const& value) { return value.path == node.surfaces.front(); });
        if (s == desc.surfaces.end()) { Fail(diagnostics, "missing CurveSource binding surface"); return {}; }
        surface = &*s;
    }
    bool validBindings = curves.skinPrim.size() == curves.curveVertexCounts.size() &&
                         curves.skinPrimUv.size() == curves.curveVertexCounts.size();
    if (!curves.curveVertexCounts.empty() && !surface) validBindings = false;
    if (validBindings) for (size_t c = 0; c < curves.skinPrim.size(); ++c) {
        auto const uv = curves.skinPrimUv[c];
        if (curves.skinPrim[c] < 0 || size_t(curves.skinPrim[c]) >= surface->faceVertexCounts.size() ||
            !std::isfinite(uv[0]) || !std::isfinite(uv[1]) ||
            uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1) {
            validBindings = false; break;
        }
    }
    if (options.rebind == "onError") {
        if (!validBindings) {
            Fail(diagnostics, "missing/out-of-range root bindings require CUDA rest-surface rebind, which is not yet integrated");
            return {};
        }
        options.rebind = "never";
    }
    options.hasRootFrame = !curves.rootFrame.empty();
    CudaSourcePreparationInput input;
    input.curveVertexCounts.assign(curves.curveVertexCounts.begin(), curves.curveVertexCounts.end());
    for (auto const& p : curves.points) input.points.push_back(make_float3(p[0], p[1], p[2]));
    for (auto const& p : curves.rest) input.rest.push_back(make_float3(p[0], p[1], p[2]));
    input.widths.assign(curves.widths.begin(), curves.widths.end());
    input.curveId.assign(curves.curveId.begin(), curves.curveId.end());
    if (validBindings) {
        input.rootPrim.assign(curves.skinPrim.begin(), curves.skinPrim.end());
        for (auto const& uv : curves.skinPrimUv) input.rootUV.push_back(make_float2(uv[0], uv[1]));
    } else if (options.rebind == "never" && diagnostics) {
        diagnostics->Warn("CUDA CurveSource drops unvalidated root bindings under rebind=never");
    }
    CudaSourcePrepared prepared;
    std::vector<std::string> messages;
    if (PrepareCudaSource(input, options, &prepared, &messages) != CudaSourcePreparationStatus::Ok) {
        for (auto const& message : messages) Fail(diagnostics, message);
        if (messages.empty()) Fail(diagnostics, "C3 source validation failed");
        return {};
    }
    if (diagnostics) for (auto const& message : messages) diagnostics->Warn(message);
    auto source = std::make_unique<gpu::CudaCurveSource>();
    if (source->Set(prepared.Input(), nullptr) != gpu::CurveSourceStatus::Ok ||
        source->Finish(nullptr) != gpu::CurveSourceStatus::Ok) {
        Fail(diagnostics, "source upload failed; previous generation retained"); return {};
    }
    std::string reason;
    auto result = gpu::MakeSourceGeneration(std::move(source), generation, &reason, !options.useRest);
    if (!result) Fail(diagnostics, reason);
    return result;
#endif
}
} // namespace usdGen
