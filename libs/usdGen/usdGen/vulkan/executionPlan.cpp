#include "usdGen/vulkan/executionPlan.h"
#include "usdGen/authoredNamedChannels.h"
#include "usdGen/surfaceRootFrames.h"

#include "usdGen/op.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <string>

namespace usdGen::vulkan {
namespace {

bool Fail(UsdGenDiagnostics *diagnostics, std::string const &message) {
    if (diagnostics) diagnostics->Error("Vulkan source-width plan: " + message);
    return false;
}

bool IsIdentity(GfMatrix4d const &matrix) {
    return matrix == GfMatrix4d(1.0);
}

// GfMatrix4d is row-major and uses row-vector affine transforms: translation
// occupies matrix[3][0..2].  Keep this native-free check aligned with C3 root
// capture, because the immutable descriptor remains source-local and root
// frames are converted through source.worldMatrix later.
bool IsFiniteInvertibleAffine(GfMatrix4d const &matrix) {
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column)
            if (!std::isfinite(matrix[row][column])) return false;
    if (matrix[0][3] != 0.0 || matrix[1][3] != 0.0 ||
        matrix[2][3] != 0.0 || matrix[3][3] != 1.0)
        return false;
    double determinant = 0.0;
    GfMatrix4d const inverse = matrix.GetInverse(&determinant);
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= std::numeric_limits<double>::epsilon())
        return false;
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column)
            if (!std::isfinite(inverse[row][column])) return false;
    return inverse[0][3] == 0.0 && inverse[1][3] == 0.0 &&
        inverse[2][3] == 0.0 && inverse[3][3] == 1.0;
}

bool IsFinite(float value) { return std::isfinite(value); }

bool ReadFloat(UsdGenParamValue const &param, float *value) {
    if (!param.value.IsHolding<float>()) return false;
    *value = param.value.UncheckedGet<float>();
    return IsFinite(*value);
}

// C1 rework: mask survives as one scalar `usdGen:mask` attribute per operator.
// Vulkan admits only the neutral value; a muted operator would alias its
// input, which the shader lanes express as their own envelope path instead.
bool IsIdentityMask(UsdGenParamValue const &param) {
    return param.name == TfToken("mask") && param.value == VtValue(1.0f);
}

bool ValidateSource(UsdGenGraphDesc const &desc, UsdGenNodeDesc const &node,
                    UsdGenCurveSetDesc const **source, VulkanSourceControls *controls,
                    UsdGenDiagnostics *diagnostics) {
    if (!node.enabled || !node.mode.IsEmpty() ||
        !node.inputs.empty() || !node.references.empty() ||
        !node.maps.empty() || !node.mapBindings.empty() ||
        !node.expressionBindings.empty() || !node.ramps.empty())
        return Fail(diagnostics, "CurveSource has unsupported enabled/input configuration");
    if (node.curves.size() != 1) return Fail(diagnostics, "CurveSource requires exactly one C3 curve target");
    auto found = std::find_if(desc.curveSets.begin(), desc.curveSets.end(), [&](auto const &item) {
        return item.path == node.curves.front();
    });
    if (found == desc.curveSets.end() || found->role != UsdGenRole::Curves ||
        found->curveRole != TfToken("hair"))
        return Fail(diagnostics, "CurveSource target must be one resolved C3 hair curve set");
    // Source points/rest stay source-local in the immutable plan.  A later
    // root capture needs this transform's inverse to move rest-surface frames
    // into that same space, so admit arbitrary affine transforms (including
    // row-vector translation) only when the full inverse contract is sound.
    if (!IsFiniteInvertibleAffine(found->worldMatrix))
        return Fail(diagnostics, "C3 source worldMatrix must be finite, affine and invertible");
    // Vulkan uses the same immutable rest-surface/root-frame capture as the
    // native C3 loader.  Relationship resolution is a compiler fact, never a
    // later live-stage lookup.
    if (node.surfaces.size() > 1)
        return Fail(diagnostics, "CurveSource permits at most one rest surface");
    if (node.surfaces.size() == 1) {
        if (node.surfaces.front().IsEmpty())
            return Fail(diagnostics, "CurveSource rest surface relationship is empty");
        size_t const resolved = std::count_if(desc.surfaces.begin(), desc.surfaces.end(),
            [&](UsdGenSurfaceDesc const &item) { return item.path == node.surfaces.front(); });
        if (resolved != 1)
            return Fail(diagnostics, "CurveSource rest surface relationship is unresolved or ambiguous");
        auto const surface = std::find_if(desc.surfaces.begin(), desc.surfaces.end(),
            [&](UsdGenSurfaceDesc const &item) { return item.path == node.surfaces.front(); });
        if (!IsFiniteInvertibleAffine(surface->worldMatrix))
            return Fail(diagnostics, "CurveSource rest surface worldMatrix must be finite, affine and invertible");
    }
    controls->hasAuthoredRest = !found->rest.empty();
    controls->restFromCurrentPoints = found->restFromCurrentPoints;
    if (found->type != TfToken("cubic") || found->basis != TfToken("bspline") ||
        found->wrap != TfToken("pinned") || found->widthsInterpolation != TfToken("vertex"))
        return Fail(diagnostics, "C3 source must be cubic bspline pinned vertex-width data");
    if ((found->curveVertexCounts.empty() != found->points.empty()) ||
        (!found->rest.empty() && found->rest.size() != found->points.size()) ||
        (!found->widths.empty() && found->widths.size() != found->points.size()))
        return Fail(diagnostics, "C3 source has unsupported topology or binding data");
    uint64_t points = 0;
    std::set<uint64_t> ids;
    for (int count : found->curveVertexCounts) {
        if (count < 2 || uint64_t(count) > std::numeric_limits<uint64_t>::max() - points)
            return Fail(diagnostics, "C3 source has invalid curve topology");
        points += uint64_t(count);
    }
    if (points != found->points.size()) return Fail(diagnostics, "C3 source point count disagrees with topology");
    if (points > uint64_t(std::numeric_limits<int>::max()) ||
        found->curveVertexCounts.size() > size_t(UINT32_MAX))
        return Fail(diagnostics, "C3 source exceeds the native offset/cardinality domain");
    std::string namedReason;
    if (!ValidateAuthoredNamedChannels(*found, found->curveVertexCounts.size(),
                                      size_t(points), &namedReason))
        return Fail(diagnostics, "C3 authored named channels: " + namedReason);
    for (auto const &point : found->points) for (int component = 0; component != 3; ++component)
        if (!std::isfinite(point[component])) return Fail(diagnostics, "C3 source contains a non-finite point");
    for (auto const &point : found->rest) for (int component = 0; component != 3; ++component)
        if (!std::isfinite(point[component])) return Fail(diagnostics, "C3 source contains a non-finite rest point");
    for (float width : found->widths)
        if (!IsFinite(width) || width < 0) return Fail(diagnostics, "C3 source contains an invalid width");
    std::set<TfToken> seen;
    for (auto const &param : node.params) {
        if (!seen.insert(param.name).second) return Fail(diagnostics, "duplicate CurveSource parameter " + param.name.GetString());
        auto const &name = param.name.GetString();
        bool valid = false;
        if (name == "useRest") { valid = param.value.IsHolding<bool>(); if (valid) controls->useRest = param.value.UncheckedGet<bool>(); }
        else if (name == "lane") valid = param.value == VtValue(TfToken("hair"));
        else if (name == "idSource") { valid = param.value == VtValue(TfToken("primvar")) || param.value == VtValue(TfToken("index")); if (valid) controls->idSource = param.value.UncheckedGet<TfToken>(); }
        else if (name == "resampleTo") valid = param.value == VtValue(0);
        else if (name == "expectEpoch") { valid = param.value.IsHolding<std::string>(); if (valid) controls->expectEpoch = param.value.UncheckedGet<std::string>(); }
        else if (name == "staleAction") { valid = param.value == VtValue(TfToken("warn")) || param.value == VtValue(TfToken("ignore")) || param.value == VtValue(TfToken("block")); if (valid) controls->staleAction = param.value.UncheckedGet<TfToken>(); }
        else if (name == "rebind") { valid = param.value == VtValue(TfToken("never")) || param.value == VtValue(TfToken("onError")) || param.value == VtValue(TfToken("always")); if (valid) controls->rebind = param.value.UncheckedGet<TfToken>(); }
        else if (name == "label") valid = param.value.IsHolding<std::string>();
        if (!valid) return Fail(diagnostics, "unsupported or malformed CurveSource parameter " + name);
    }
    if (controls->idSource == TfToken("primvar") &&
        found->curveId.empty()) {
        controls->stableIdsSynthesized = true;
        if (diagnostics) diagnostics->Warn("Vulkan source-width plan: CurveSource idSource=primvar synthesizes index stable IDs");
    }
    if (controls->idSource == TfToken("primvar") && !found->curveId.empty() &&
        found->curveId.size() != found->curveVertexCounts.size())
        return Fail(diagnostics, "CurveSource idSource=primvar has incomplete stable IDs");
    // Index mode deliberately ignores the authored ID array, including its
    // shape and duplicate values, just as the CPU source capture does.
    if (controls->idSource == TfToken("primvar"))
        for (uint64_t id : found->curveId)
            if (!ids.insert(id).second) return Fail(diagnostics, "C3 source stable IDs must be unique");
    if (controls->useRest && (found->restFromCurrentPoints ||
        found->rest.size() != found->points.size()))
        return Fail(diagnostics, "CurveSource useRest=true requires an authored non-current rest sample");
    if (node.surfaces.empty() && !found->curveVertexCounts.empty() &&
        controls->rebind != TfToken("never"))
        return Fail(diagnostics, "surface-free CurveSource requires rebind=never");
    if (!controls->expectEpoch.empty() && controls->expectEpoch != found->frozenEpoch) {
        if (controls->staleAction == TfToken("block"))
            return Fail(diagnostics, "CurveSource expectEpoch mismatches frozenEpoch with staleAction=block");
        if (controls->staleAction == TfToken("warn") && diagnostics)
            diagnostics->Warn("Vulkan source-width plan: CurveSource expectEpoch mismatches frozenEpoch");
    }
    // Admission does not build a surface index or query roots.  Keep the
    // exact authored-frame contract in the shared helper; validity of an
    // individual face/UV is intentionally left to CurveLoader root capture,
    // where never drops and onError/always can rebind it.
    if (found->skinPrim.size() != found->skinPrimUv.size() &&
        controls->rebind == TfToken("never"))
        return Fail(diagnostics, "C3 rootPrim/rootUV cardinalities differ for rebind=never");
    if (!found->rootFrame.empty() &&
        found->rootFrame.size() != found->curveVertexCounts.size())
        return Fail(diagnostics, "C3 authored rootFrame cardinality disagrees with curves");
    for (GfMatrix4d const &frame : found->rootFrame)
        if (!UsdGenValidateAuthoredRootFrame(frame))
            return Fail(diagnostics, "C3 authored rootFrame violates the affine orthonormal frame contract");
    *source = &*found;
    return true;
}

bool ValidateWidth(UsdGenNodeDesc const &node, VulkanLiteralWidthControls *controls,
                   UsdGenDiagnostics *diagnostics) {
    // Every parameter of an enabled Width validates as the authored literal;
    // only the neutral `mask` value is admissible on this lane.
    if (!node.mode.IsEmpty() ||
        !node.references.empty() || !node.curves.empty() ||
        !node.surfaces.empty() || !node.maps.empty() || !node.mapBindings.empty() ||
        !node.expressionBindings.empty() || !node.ramps.empty())
        return Fail(diagnostics, "Width has unsupported mode/input configuration");
    std::set<TfToken> seen;
    for (auto const &param : node.params) {
        if (!seen.insert(param.name).second) return Fail(diagnostics, "duplicate Width parameter " + param.name.GetString());
        auto const &name = param.name.GetString();
        if (name == "width") {
            if (!ReadFloat(param, &controls->width) || controls->width < 0)
                return Fail(diagnostics, "Width parameter width must be a finite non-negative float");
        } else if (name == "replace") {
            if (!param.value.IsHolding<bool>()) return Fail(diagnostics, "Width parameter replace must be bool");
            controls->replace = param.value.UncheckedGet<bool>();
        } else if (name == "width:knots") {
            if (!param.value.IsHolding<VtVec2fArray>() || !param.value.UncheckedGet<VtVec2fArray>().empty())
                return Fail(diagnostics, "Width ramps are not supported");
        } else if (name == "width:interpolation") {
            if (param.value != VtValue(TfToken("catmullRom"))) return Fail(diagnostics, "Width interpolation must be neutral catmullRom");
        } else if (!IsIdentityMask(param)) return Fail(diagnostics, "unsupported or non-neutral Width parameter " + name);
    }
    return true;
}

bool ValidateLength(UsdGenNodeDesc const& node, VulkanSourceWidthStage* stage, UsdGenDiagnostics* diagnostics) {
    // C1 rework removed the node-level mute: `usdGen:mask` is the one
    // envelope control and it feeds the stage envelope directly.
    if (!node.mode.IsEmpty() ||
        !node.references.empty() || !node.curves.empty() || !node.surfaces.empty() ||
        !node.maps.empty() || !node.mapBindings.empty() || !node.expressionBindings.empty() || !node.ramps.empty())
        return Fail(diagnostics, "Length requires literal identity-envelope scale, set or cull configuration");
    bool cull = false, set = false;
    for (auto const& param : node.params)
        if (param.name == TfToken("length:mode") && param.value == VtValue(TfToken("cull"))) cull = true;
        else if (param.name == TfToken("length:mode") && param.value == VtValue(TfToken("set"))) set = true;
    stage->kind = cull ? VulkanSourceWidthStage::Kind::LengthCull : VulkanSourceWidthStage::Kind::LengthScale;
    stage->lengthMode = set ? VulkanSourceWidthStage::LengthMode::Set
                            : VulkanSourceWidthStage::LengthMode::Scale;
    std::set<TfToken> seen;
    for (auto const& param : node.params) {
        if (param.animated) return Fail(diagnostics, "Length requires non-animated literal controls");
        if (!seen.insert(param.name).second) return Fail(diagnostics, "duplicate Length parameter");
        auto const& name = param.name.GetString();
        if (name == "length:value") {
            double value;
            if (param.value.IsHolding<double>()) value = param.value.UncheckedGet<double>();
            else if (param.value.IsHolding<float>()) value = param.value.UncheckedGet<float>();
            else return Fail(diagnostics, "Length value requires a finite numeric literal");
            stage->factor = float(value);
            if (!std::isfinite(value) || value < 0 || !std::isfinite(stage->factor))
                return Fail(diagnostics, "Length factor must be finite, nonnegative and float-representable");
        } else if (name == "length:mode") {
            if (param.value != VtValue(TfToken("scale")) && param.value != VtValue(TfToken("set")) &&
                param.value != VtValue(TfToken("cull")))
                return Fail(diagnostics, "only literal Length scale, set or cull modes supported");
        } else if (name == "length:method") {
            if (param.value == VtValue(TfToken("scale"))) stage->lengthMethod = VulkanSourceWidthStage::LengthMethod::Scale;
            else if (param.value == VtValue(TfToken("cutExtend"))) stage->lengthMethod = VulkanSourceWidthStage::LengthMethod::CutExtend;
            else return Fail(diagnostics, "Length method must be scale or cutExtend");
        } else if (name == "length:random") {
            if (!param.value.IsHolding<GfVec2f>()) return Fail(diagnostics, "Length random requires a finite nonnegative vec2 literal");
            auto const range = param.value.UncheckedGet<GfVec2f>();
            stage->randomLo = range[0]; stage->randomHi = range[1];
            if (!std::isfinite(stage->randomLo) || !std::isfinite(stage->randomHi) ||
                stage->randomLo < 0 || stage->randomHi < 0)
                return Fail(diagnostics, "Length random requires finite nonnegative endpoints");
        } else if (name == "mask") {
            if (!ReadFloat(param, &stage->lengthMaskAmount) || stage->lengthMaskAmount < 0 ||
                stage->lengthMaskAmount > 1)
                return Fail(diagnostics, "Length mask amount must be a finite [0,1] literal");
        } else if (name == "cullThreshold") {
            double threshold;
            if (param.value.IsHolding<float>()) threshold = param.value.UncheckedGet<float>();
            else if (param.value.IsHolding<double>()) threshold = param.value.UncheckedGet<double>();
            else return Fail(diagnostics, "Length cull threshold must be a finite nonnegative literal");
            stage->cullThreshold = float(threshold);
            if (!std::isfinite(threshold) || threshold < 0 ||
                !std::isfinite(stage->cullThreshold))
                return Fail(diagnostics, "Length cull threshold must be a finite nonnegative literal");
        } else if (name == "minRemainingLength") {
            double minimum;
            if (param.value.IsHolding<float>()) minimum = param.value.UncheckedGet<float>();
            else if (param.value.IsHolding<double>()) minimum = param.value.UncheckedGet<double>();
            else return Fail(diagnostics, "Length minimum must be a finite nonnegative literal");
            stage->minRemainingLength = float(minimum);
            if (!std::isfinite(minimum) || minimum < 0 || !std::isfinite(stage->minRemainingLength))
                return Fail(diagnostics, "Length minimum must be a finite nonnegative literal");
        } else if (name == "rebuild") {
            if (param.value == VtValue(TfToken("keepParam"))) stage->lengthRebuild = VulkanSourceWidthStage::LengthRebuild::KeepParam;
            else if (param.value == VtValue(TfToken("reparam"))) stage->lengthRebuild = VulkanSourceWidthStage::LengthRebuild::Reparam;
            else return Fail(diagnostics, "Length rebuild must be keepParam or reparam");
        } else if (name == "label") {
            if (!param.value.IsHolding<std::string>()) return Fail(diagnostics, "Length label must be string");
        } else if (!IsIdentityMask(param)) return Fail(diagnostics, "unsupported Length control " + name);
    }
    if (set && cull) return Fail(diagnostics, "Length set and cull are mutually exclusive");
    return true;
}

bool ValidateNoise(UsdGenNodeDesc const& node, VulkanLiteralNoiseControls* out, UsdGenDiagnostics* diagnostics) {
    // Literal-only lane: no references/curves/surfaces/maps/expressions/ramps.
    // Magnitude ramps are excluded (the pipeline uses a flat 257-entry profile).
    if (!node.mode.IsEmpty() ||
        !node.references.empty() || !node.curves.empty() || !node.surfaces.empty() ||
        !node.maps.empty() || !node.mapBindings.empty() ||
        !node.expressionBindings.empty() || !node.ramps.empty())
        return Fail(diagnostics, "Noise requires literal configuration");
    std::set<TfToken> seen;
    for (auto const& param : node.params) {
        if (param.animated) return Fail(diagnostics, "Noise requires non-animated literal controls");
        if (!seen.insert(param.name).second) return Fail(diagnostics, "duplicate Noise parameter");
        auto const& name = param.name.GetString();
        if (name == "noise:magnitude") {
            if (!ReadFloat(param, &out->magnitude) || out->magnitude < 0)
                return Fail(diagnostics, "Noise magnitude must be a finite nonnegative literal");
        } else if (name == "noise:frequency") {
            if (!ReadFloat(param, &out->frequency) || out->frequency <= 0)
                return Fail(diagnostics, "Noise frequency must be a finite positive literal");
        } else if (name == "noise:correlation") {
            if (!ReadFloat(param, &out->correlation) || out->correlation < 0 || out->correlation > 1)
                return Fail(diagnostics, "Noise correlation must be a finite [0,1] literal");
        } else if (name == "noise:octaves") {
            if (!param.value.IsHolding<int>())
                return Fail(diagnostics, "Noise octaves must be an int literal");
            out->octaves = param.value.UncheckedGet<int>();
            if (out->octaves < 1 || out->octaves > 6)
                return Fail(diagnostics, "Noise octaves must be in [1,6]");
        } else if (name == "noise:lacunarity") {
            if (!ReadFloat(param, &out->lacunarity) || out->lacunarity <= 1)
                return Fail(diagnostics, "Noise lacunarity must be a finite >1 literal");
        } else if (name == "noise:gain") {
            if (!ReadFloat(param, &out->gain) || out->gain < 0 || out->gain > 1)
                return Fail(diagnostics, "Noise gain must be a finite [0,1] literal");
        } else if (name == "preserveLength") {
            if (!ReadFloat(param, &out->preserveLength) || out->preserveLength < 0 || out->preserveLength > 1)
                return Fail(diagnostics, "Noise preserveLength must be a finite [0,1] literal");
        } else if (name == "mask") {
            if (!ReadFloat(param, &out->mask) || out->mask < 0 || out->mask > 1)
                return Fail(diagnostics, "Noise mask must be a finite [0,1] literal");
        } else if (name == "noise:seed") {
            if (!param.value.IsHolding<int>())
                return Fail(diagnostics, "Noise seed must be an int literal");
            out->seed = param.value.UncheckedGet<int>();
        } else if (name == "cumulative") {
            if (!param.value.IsHolding<bool>())
                return Fail(diagnostics, "Noise cumulative must be a bool literal");
            out->cumulative = param.value.UncheckedGet<bool>();
        } else if (name == "noise:magnitude:knots") {
            if (!param.value.IsHolding<VtVec2fArray>() || !param.value.UncheckedGet<VtVec2fArray>().empty())
                return Fail(diagnostics, "Noise magnitude ramps are not supported");
        } else if (name == "noise:magnitude:interpolation") {
            if (param.value != VtValue(TfToken("catmullRom")))
                return Fail(diagnostics, "Noise magnitude interpolation must be neutral catmullRom");
        } else if (name == "enabled") {
            if (!param.value.IsHolding<bool>())
                return Fail(diagnostics, "Noise enabled must be a bool literal");
        } else {
            return Fail(diagnostics, "unsupported Noise parameter " + name);
        }
    }
    return true;
}

bool Estimate(UsdGenCurveSetDesc const &source, bool rootBindings,
              uint64_t *bytes, uint64_t *widthBytes) {
    uint64_t points = source.points.size(), curves = source.curveVertexCounts.size(), total = 0, value = 0;
    if (curves == std::numeric_limits<uint64_t>::max()) return false;
    bool const ok = UsdGenExecutionCheckedBytes::Multiply(points, 12, &value) && UsdGenExecutionCheckedBytes::Add(&total, value) &&
        UsdGenExecutionCheckedBytes::Multiply(curves + 1, 4, &value) && UsdGenExecutionCheckedBytes::Add(&total, value) &&
        UsdGenExecutionCheckedBytes::Multiply(curves, 8, &value) && UsdGenExecutionCheckedBytes::Add(&total, value) &&
        UsdGenExecutionCheckedBytes::Multiply(points, 12, &value) && UsdGenExecutionCheckedBytes::Add(&total, value) &&
        UsdGenExecutionCheckedBytes::Multiply(points, 4, widthBytes) && UsdGenExecutionCheckedBytes::Add(&total, *widthBytes) &&
        UsdGenExecutionCheckedBytes::Add(&total, *widthBytes); // immutable hairT
    // Reserve the source cardinality, not the surviving cardinality: root
    // capture can compact geometrically dropped roots only at execution.
    if (ok && rootBindings &&
        (!UsdGenExecutionCheckedBytes::Multiply(curves, 48, &value) ||
         !UsdGenExecutionCheckedBytes::Add(&total, value))) return false;
    if (!ok) return false;
    for (auto const& plane : source.authoredPlanes) {
        uint64_t scalars = plane.floatValues.size();
        if (!UsdGenExecutionCheckedBytes::Add(&scalars, plane.intValues.size()) ||
            !UsdGenExecutionCheckedBytes::Multiply(scalars, sizeof(uint32_t), &value) ||
            !UsdGenExecutionCheckedBytes::Add(&total, value)) return false;
    }
    *bytes = total;
    return true;
}
} // namespace

VulkanSourceWidthPlan::VulkanSourceWidthPlan(std::shared_ptr<const UsdGenGraphDesc> descriptor,
    UsdGenCurveSetDesc source, SdfPath sourcePath, SdfPath widthPath,
    VulkanSourceControls sourceControls, VulkanLiteralWidthControls width, uint64_t generation, uint64_t topology,
    uint64_t value, bool rootBindings) noexcept : descriptor_(std::move(descriptor)), source_(std::move(source)),
    sourceNodePath_(std::move(sourcePath)), widthNodePath_(std::move(widthPath)), sourceControls_(std::move(sourceControls)), width_(width),
    curveGeneration_(generation), topologyVersion_(topology), valueVersion_(value), hasRootBindings_(rootBindings) {}

std::shared_ptr<const UsdGenExecutionPlanHandle> CompileVulkanSourceWidthPlan(
    UsdGenGraphDesc const& desc, UsdGenDiagnostics* diagnostics) {
    if (!desc.validationErrors.empty() || desc.executionBackend != UsdGenExecutionBackend::Vulkan ||
        desc.nodes.size() < 2 || desc.nodes.size() > 65 || !desc.expressions.empty() || !desc.maps.empty() ||
        desc.curveSets.size() != 1 || !IsIdentity(desc.xformMatrix) ||
        !IsFinite(desc.defaultWidth) || desc.defaultWidth < 0) {
        Fail(diagnostics, "requires a bounded single-source literal Vulkan graph"); return {};
    }
    if (desc.curveBasis != TfToken("bspline")) {
        Fail(diagnostics, "unsupported motion, density or basis"); return {};
    }
    size_t sourceIndex = desc.nodes.size(), terminalIndex = desc.nodes.size();
    std::set<SdfPath> paths;
    for (size_t i = 0; i < desc.nodes.size(); ++i) {
        auto const& node = desc.nodes[i];
        if (node.path.IsEmpty() || !paths.insert(node.path).second) {
            Fail(diagnostics, "node paths must be unique and nonempty"); return {};
        }
        if (node.path == desc.terminal) terminalIndex = i;
        if (node.type == TfToken("UsdGenCurveSource")) {
            if (sourceIndex != desc.nodes.size()) { Fail(diagnostics, "only one source is supported"); return {}; }
            sourceIndex = i;
        }
    }
    if (sourceIndex == desc.nodes.size() || terminalIndex == desc.nodes.size() || sourceIndex == terminalIndex) {
        Fail(diagnostics, "missing source or operator terminal"); return {};
    }
    auto indexFor = [&](SdfPath const& path) {
        for (size_t i = 0; i < desc.nodes.size(); ++i) if (desc.nodes[i].path == path) return i;
        return desc.nodes.size();
    };
    std::vector<std::vector<size_t>> inputs(desc.nodes.size());
    for (size_t i = 0; i < desc.nodes.size(); ++i) {
        for (auto const& path : desc.nodes[i].inputs) {
            auto j = indexFor(path);
            if (j == desc.nodes.size() || j == i) { Fail(diagnostics, "invalid graph input"); return {}; }
            inputs[i].push_back(j);
        }
    }
    std::vector<bool> reachable(desc.nodes.size(), false);
    std::vector<size_t> pending{terminalIndex};
    while (!pending.empty()) {
        auto i = pending.back(); pending.pop_back();
        if (reachable[i]) continue;
        reachable[i] = true;
        pending.insert(pending.end(), inputs[i].begin(), inputs[i].end());
    }
    if (std::find(reachable.begin(), reachable.end(), false) != reachable.end()) {
        Fail(diagnostics, "unused authored nodes are not supported"); return {};
    }
    UsdGenCurveSetDesc const* source = nullptr;
    VulkanSourceControls sourceControls;
    auto const& sourceNode = desc.nodes[sourceIndex];
    if (!ValidateSource(desc, sourceNode, &source, &sourceControls, diagnostics)) return {};
    bool const hasRootBindings = !source->curveVertexCounts.empty() &&
        (!sourceNode.surfaces.empty() || !source->skinPrim.empty() ||
         !source->skinPrimUv.empty() || !source->rootFrame.empty());
    std::vector<uint32_t> nodeValue(desc.nodes.size(), UINT32_MAX);
    nodeValue[sourceIndex] = 0;
    std::vector<VulkanSourceWidthStage> steps;
    // An authored positive threshold after a fixed-topology Length lowers to
    // two native stages.  Keep its original descriptor ordinal separately:
    // the internal point transform has no semantic node, while the following
    // cull remains the one externally visible authored operator.
    std::vector<size_t> authoredIndices;
    std::vector<bool> internalStages;
    bool hasBlend = false;
    size_t processedAuthored = 1;
    uint32_t syntheticPathOrdinal = 0;
    auto appendStage = [&](VulkanSourceWidthStage value, size_t authored,
                           bool internal) {
        if (steps.size() >= 64) return false;
        steps.push_back(std::move(value));
        authoredIndices.push_back(authored);
        internalStages.push_back(internal);
        return true;
    };
    auto uniqueTransformPath = [&](SdfPath const& parent) {
        // The graph's existing admission predicate accepts nonempty paths;
        // a property path cannot parent a synthetic child.  Anchor such an
        // internal-only task at root instead of weakening that graph rule.
        SdfPath const anchor = (parent.IsPrimPath() ||
            parent == SdfPath::AbsoluteRootPath()) ? parent : SdfPath::AbsoluteRootPath();
        SdfPath path;
        do {
            path = anchor.AppendChild(TfToken(std::string("__vulkanLengthTransform") +
                                               std::to_string(syntheticPathOrdinal++)));
        } while (!paths.insert(path).second);
        return path;
    };
    while (processedAuthored < desc.nodes.size()) {
        size_t ready = desc.nodes.size();
        for (size_t i = 0; i < desc.nodes.size(); ++i) {
            if (nodeValue[i] != UINT32_MAX || inputs[i].empty()) continue;
            bool available = true;
            for (auto j : inputs[i]) if (nodeValue[j] == UINT32_MAX) available = false;
            if (available) { ready = i; break; }
        }
        if (ready == desc.nodes.size()) { Fail(diagnostics, "cycle or unsupported additional root"); return {}; }
        auto const& node = desc.nodes[ready];
        VulkanSourceWidthStage stage; stage.path = node.path; stage.input = nodeValue[inputs[ready][0]];
        if (node.type == TfToken("UsdGenWidth")) {
            if (inputs[ready].size() != 1 || !ValidateWidth(node, &stage.width, diagnostics)) return {};
            stage.disabled = !node.enabled;
        } else if (node.type == TfToken("UsdGenLength")) {
            stage.kind = VulkanSourceWidthStage::Kind::LengthScale;
            stage.randomSeed = node.seed;
            if (inputs[ready].size() != 1 || !ValidateLength(node, &stage, diagnostics)) return {};
            stage.disabled = !node.enabled;
        } else if (node.type == TfToken("UsdGenWidthBlend")) {
            stage.kind = VulkanSourceWidthStage::Kind::WidthBlend;
            bool paramsOk = true;
            float weight = 1.0f;
            for (auto const& param : node.params) {
                bool const ok = param.name == TfToken("widthBlend:weight") && !param.animated &&
                    param.value.IsHolding<float>() &&
                    std::isfinite(param.value.UncheckedGet<float>()) &&
                    param.value.UncheckedGet<float>() >= 0.0f &&
                    param.value.UncheckedGet<float>() <= 1.0f;
                paramsOk = paramsOk && ok;
                if (ok) weight = param.value.UncheckedGet<float>();
            }
            if (inputs[ready].size() != 2 || inputs[ready][0] == inputs[ready][1] ||
                !node.enabled || !paramsOk || !node.mode.IsEmpty() || !node.ramps.empty() ||
                !node.expressionBindings.empty() || !node.references.empty() || !node.curves.empty() ||
                !node.surfaces.empty() || !node.maps.empty() || !node.mapBindings.empty()) {
                Fail(diagnostics, "WidthBlend requires two ordered inputs and literal blend"); return {};
            }
            stage.rightInput = nodeValue[inputs[ready][1]]; stage.blend = weight; hasBlend = true;
        } else if (node.type == TfToken("UsdGenNoise")) {
            stage.kind = VulkanSourceWidthStage::Kind::Noise;
            stage.noise.seed = node.seed;
            if (inputs[ready].size() != 1 || !ValidateNoise(node, &stage.noise, diagnostics)) return {};
            if (!hasRootBindings) { Fail(diagnostics, "Noise requires a root-bound source"); return {}; }
            stage.disabled = !node.enabled;
        } else { Fail(diagnostics, "unsupported Vulkan operator"); return {}; }
        if (stage.kind == VulkanSourceWidthStage::Kind::LengthScale &&
            stage.cullThreshold > 0.0f) {
            // Do not ask compaction to reapply a scalar factor: it must
            // consume the actual scale/set transformed points so its native
            // length rounding is identical for both authored modes.
            VulkanSourceWidthStage cull;
            cull.kind = VulkanSourceWidthStage::Kind::LengthCull;
            cull.path = stage.path;
            cull.input = uint32_t(steps.size() + 1);
            cull.factor = stage.factor;
            cull.lengthMode = stage.lengthMode;
            cull.lengthMethod = stage.lengthMethod;
            cull.lengthRebuild = stage.lengthRebuild;
            cull.minRemainingLength = stage.minRemainingLength;
            cull.randomLo = stage.randomLo; cull.randomHi = stage.randomHi;
            cull.randomSeed = stage.randomSeed;
            cull.lengthBlend = stage.lengthBlend;
            cull.lengthMaskAmount = stage.lengthMaskAmount;
            cull.cullThreshold = stage.cullThreshold;
            stage.path = uniqueTransformPath(node.path);
            stage.cullThreshold = 0.0f;
            if (!appendStage(std::move(stage), ready, true) ||
                !appendStage(std::move(cull), ready, false)) {
                Fail(diagnostics, "expanded Vulkan stage count exceeds 64"); return {};
            }
        } else if (stage.kind == VulkanSourceWidthStage::Kind::LengthCull &&
                   stage.lengthMaskAmount != 1.0f) {
            // Envelope phase 3 validates/preserves the fixed-topology
            // lineage before the authored topology owner performs culling.
            VulkanSourceWidthStage cull = stage;
            cull.input = uint32_t(steps.size() + 1);
            cull.lengthCullOnly = false;
            stage.kind = VulkanSourceWidthStage::Kind::LengthScale;
            stage.path = uniqueTransformPath(node.path);
            stage.cullThreshold = 0.0f;
            stage.lengthCullOnly = true;
            if (!appendStage(std::move(stage), ready, true) ||
                !appendStage(std::move(cull), ready, false)) {
                Fail(diagnostics, "expanded Vulkan stage count exceeds 64"); return {};
            }
        } else if (!appendStage(std::move(stage), ready, false)) {
            Fail(diagnostics, "expanded Vulkan stage count exceeds 64"); return {};
        }
        nodeValue[ready] = uint32_t(steps.size());
        ++processedAuthored;
    }
    if (authoredIndices.back() != terminalIndex || steps.back().kind == VulkanSourceWidthStage::Kind::LengthScale ||
        steps.back().kind == VulkanSourceWidthStage::Kind::LengthCull ||
        steps.back().kind == VulkanSourceWidthStage::Kind::Noise) {
        Fail(diagnostics, "terminal must be Width or WidthBlend"); return {};
    }
    uint64_t bytes = 0, widthBytes = 0, pointBytes = 0, peak = 0;
    if (!Estimate(*source, hasRootBindings, &bytes, &widthBytes) ||
        !UsdGenExecutionCheckedBytes::Multiply(source->points.size(), 12, &pointBytes) ||
        !UsdGenExecutionCheckedBytes::Multiply(bytes, 2, &peak)) {
        Fail(diagnostics, "payload estimate overflow"); return {};
    }
    auto capture = std::make_shared<UsdGenGraphDesc>(desc);
    auto payload = std::shared_ptr<VulkanSourceWidthPlan>(new VulkanSourceWidthPlan(
        capture, *source, sourceNode.path, desc.terminal, sourceControls,
        VulkanLiteralWidthControls{}, source->curveGeneration, source->curveGeneration, source->curveGeneration,
        hasRootBindings));
    payload->steps_ = steps;
    for (size_t i = 0; i < steps.size(); ++i) {
        auto const& stage = steps[i];
        if (stage.kind == VulkanSourceWidthStage::Kind::Width) payload->width_ = stage.width;
        if ((stage.kind == VulkanSourceWidthStage::Kind::LengthScale ||
             stage.kind == VulkanSourceWidthStage::Kind::LengthCull) &&
            !internalStages[i] && !payload->HasLength()) {
            payload->lengthNodePath_ = stage.path; payload->lengthFactor_ = stage.factor;
        }
    }
    uint32_t const finalTask = uint32_t(steps.size());
    using K = UsdGenExecutionDataKind;
    using A = UsdGenExecutionResourceAccess;
    using S = UsdGenExecutionValueStorage;
    std::vector<UsdGenExecutionValueMetadata> values{
        {0,K::CurveGeometry,UINT32_MAX,0,S::ExternalImmutable},
        {1,K::CurveGeometry,0,1,S::JobOwnedImmutable},
        {2,K::Widths,0,1,S::JobOwnedImmutable},
        {3,K::Widths,finalTask,finalTask+1,S::PublishedImmutable},
        {4,K::CurveTopology,0,1,S::JobOwnedImmutable},
        {5,K::StableIds,0,1,S::JobOwnedImmutable},
        // One immutable source bundle: hairT plus every authored named plane.
        // Point-only stages retain/read it; topology-changing stages publish a
        // gathered replacement bundle for their downstream lineage.
        {6,K::NamedChannels,0,1,S::JobOwnedImmutable}};
    // Preserve the historical ids above.  Root bindings are a separate
    // structural bundle (rootPrim, rootUV, private T/B/N) and are appended
    // only when source capture can produce a non-empty bundle.
    std::vector<uint32_t> sourceImmutableIds{4u, 5u, 6u};
    if (hasRootBindings) {
        values.push_back({uint32_t(values.size()),K::RootBindings,0,1,S::JobOwnedImmutable});
        sourceImmutableIds.push_back(uint32_t(values.size() - 1));
    }
    struct Bundle { uint32_t geometry, widths; std::vector<uint32_t> inherited; };
    std::vector<Bundle> bundles{{1,2,sourceImmutableIds}};
    std::vector<UsdGenExecutionTaskMetadata> tasks;
    UsdGenExecutionTaskMetadata sourceTask;
    sourceTask.id=0; sourceTask.semanticNode=uint32_t(sourceIndex);
    sourceTask.authoredOrderKey=uint32_t(sourceIndex); sourceTask.kind=UsdGenExecutionTaskKind::Source;
    sourceTask.path=sourceNode.path; sourceTask.type=sourceNode.type; sourceTask.exclusiveWorkspace=false;
    sourceTask.resources={{K::CurveGeometry,A::ReadWrite,UINT32_MAX,0,1},{K::Widths,A::Write,UINT32_MAX,UINT32_MAX,2}};
    for (uint32_t id : sourceImmutableIds)
        sourceTask.resources.push_back({values[id].resource,A::Write,UINT32_MAX,UINT32_MAX,id});
    sourceTask.estimate={0,bytes,0,0,false,false,false,0}; tasks.push_back(std::move(sourceTask));
    std::vector<UsdGenCompiledOperatorCapability> operators{{sourceNode.path,sourceNode.type,uint32_t(sourceIndex),1,UsdGenCapabilityCopyOnWriteWrites,UsdGenExecutionCapabilityStatus::Supported}};
    for (size_t i=0; i<steps.size(); ++i) {
        auto& stage=steps[i]; uint32_t taskId=uint32_t(i+1);
        auto in=bundles[stage.input], out=in;
        UsdGenExecutionTaskMetadata task;
        auto const& authored=desc.nodes[authoredIndices[i]];
        task.id=taskId;
        task.semanticNode=internalStages[i] ? UINT32_MAX : uint32_t(authoredIndices[i]);
        task.authoredOrderKey=uint32_t(authoredIndices[i]);
        task.kind=UsdGenExecutionTaskKind::Operator; task.path=stage.path;
        task.type=internalStages[i] ? TfToken("VulkanLengthTransform") : authored.type;
        task.exclusiveWorkspace=false;
        bool const cull=stage.kind==VulkanSourceWidthStage::Kind::LengthCull;
        bool const length=stage.kind==VulkanSourceWidthStage::Kind::LengthScale || cull;
        task.topologyBarrier = cull;
        uint32_t output=taskId==finalTask ? 3u : uint32_t(values.size());
        if (output!=3) values.push_back({});
        if (output!=3) values[output]={output,length?K::CurveGeometry:K::Widths,taskId,taskId+1,S::JobOwnedImmutable};
        if (length) out.geometry=output; else out.widths=output;
        task.resources.push_back({K::CurveGeometry,length?A::ReadWrite:A::Read,values[in.geometry].producerTask,in.geometry,length?output:UINT32_MAX});
        if (cull) {
            out.widths = uint32_t(values.size());
            values.push_back({out.widths,K::Widths,taskId,taskId+1,S::JobOwnedImmutable});
        }
        task.resources.push_back({K::Widths,length&&!cull?A::Read:A::ReadWrite,
            values[in.widths].producerTask,in.widths,cull?out.widths:length?UINT32_MAX:output});
        if (stage.kind==VulkanSourceWidthStage::Kind::WidthBlend) {
            auto right=bundles[stage.rightInput];
            stage.requiresNonWidthProof = right.geometry != in.geometry;
            if (stage.requiresNonWidthProof) {
                task.resources.push_back({K::CurveGeometry,A::Read,values[right.geometry].producerTask,right.geometry,UINT32_MAX});
                for (uint32_t id : right.inherited)
                    task.resources.push_back({values[id].resource,A::Read,values[id].producerTask,id,UINT32_MAX});
            }
            task.resources.push_back({K::Widths,A::Read,values[right.widths].producerTask,right.widths,UINT32_MAX});
        }
        for (size_t j=0; j<in.inherited.size(); ++j) {
            uint32_t const id = in.inherited[j];
            uint32_t replacement = UINT32_MAX;
            if (cull) {
                replacement = uint32_t(values.size());
                values.push_back({replacement,values[id].resource,taskId,taskId+1,S::JobOwnedImmutable});
                out.inherited[j] = replacement;
            }
            task.resources.push_back({values[id].resource,cull?A::ReadWrite:A::Read,
                values[id].producerTask,id,replacement});
        }
        uint64_t delta=cull?bytes:length?pointBytes:widthBytes;
        // The comparison packs both non-width packets with per-plane padding
        // and a scalar status. This remains a logical estimate, not a Vulkan
        // allocation-requirements bound; the pool enforces actual admission.
        uint64_t proofScratch = 0;
        if (stage.requiresNonWidthProof) {
            proofScratch = bytes;
            if (!UsdGenExecutionCheckedBytes::Add(&proofScratch, bytes) ||
                !UsdGenExecutionCheckedBytes::Add(&proofScratch, 64)) {
                Fail(diagnostics,"DAG comparison estimate overflow"); return {};
            }
        }
        if (!UsdGenExecutionCheckedBytes::Add(&peak,delta)) { Fail(diagnostics,"DAG delta estimate overflow"); return {}; }
        if (!UsdGenExecutionCheckedBytes::Add(&peak,proofScratch)) { Fail(diagnostics,"DAG proof estimate overflow"); return {}; }
        task.estimate={0,delta,bytes,proofScratch,false,false,false,0}; tasks.push_back(std::move(task)); bundles.push_back(out);
        if (!internalStages[i])
            operators.push_back({stage.path,authored.type,uint32_t(authoredIndices[i]),1,
                UsdGenCapabilityCopyOnWriteWrites,UsdGenExecutionCapabilityStatus::Supported});
    }
    UsdGenExecutionTaskMetadata publication;
    publication.id=finalTask+1; publication.kind=UsdGenExecutionTaskKind::Publication;
    publication.type=TfToken("VulkanPublication"); publication.exclusiveWorkspace=false;
    auto terminal=bundles.back();
    publication.resources={{K::CurveGeometry,A::Read,values[terminal.geometry].producerTask,terminal.geometry,UINT32_MAX},
        {K::Widths,A::Read,finalTask,3,UINT32_MAX}};
    for (uint32_t id : terminal.inherited)
        publication.resources.push_back({values[id].resource,A::Read,values[id].producerTask,id,UINT32_MAX});
    for(uint32_t i=0;i<=finalTask;++i) publication.declaredDependencies.push_back({i,UsdGenExecutionDependencyLifetimePublicationJoin});
    tasks.push_back(std::move(publication));
    payload->steps_ = steps;
    std::string reason;
    if(!UsdGenExecutionDependencyCompiler::Lower(&tasks,values,&reason)){Fail(diagnostics,reason);return{};}
    UsdGenExecutionGraphMemoryEstimate memory{peak,bytes,false,false,false};
    auto metadata=std::make_shared<const UsdGenExecutionPlanMetadata>("vulkan",1,
        hasBlend?UsdGenExecutionPlanShape::SourceRootedValueDag:UsdGenExecutionPlanShape::LinearAuthoredChain,
        std::move(operators),std::move(tasks),std::move(values),finalTask+1,memory);
    auto handle=UsdGenExecutionPlanHandle::Create(UsdGenExecutionBackend::Vulkan,metadata,payload,&reason);
    if(!handle) Fail(diagnostics,reason);
    return handle;
}
} // namespace usdGen::vulkan
