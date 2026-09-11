#include "usdGen/cudaExecution.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <array>
#include <mutex>
#include <atomic>

#ifdef USDGEN_ENABLE_CUDA
#include "usdGen/cudaSourceInput.h"
#include "usdGen/gpu/generation.h"
#include "usdGen/cudaParameters.h"
#include "usdGen/gpu/width.h"
#include "usdGen/cudaSurfaceInput.h"
#include "usdGen/gpu/surfaceBinding.h"
#include "usdGen/gpu/deformCurves.h"
#include "usdGenMath/usdGenMath/ramp.h"
#endif

namespace usdGen {
namespace {
[[maybe_unused]] std::map<std::string, VtValue> const& IdentityMask() {
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
    return identityMask;
}
bool Fail(UsdGenDiagnostics* diagnostics, std::string message) {
    if (diagnostics) diagnostics->Error("CUDA: " + std::move(message));
    return false;
}
[[maybe_unused]] std::string LocalName(TfToken const& name) {
    auto value = name.GetString();
    if (value.compare(0, 7, "usdGen:") == 0) value.erase(0, 7);
    return value;
}
#ifdef USDGEN_ENABLE_CUDA
bool ValidateWidth(UsdGenNodeDesc const& node, UsdGenDiagnostics* diagnostics) {
    if (node.type != TfToken("UsdGenWidth") || node.algorithmVersion < 0 || node.algorithmVersion > 1)
        return Fail(diagnostics, "only Width operators may follow CurveSource in this CUDA executor");
    // Surface inheritance is metadata on Width; this operator does not sample
    // it. Do not reject the Description's ordinary inherited scalp target.
    if (!node.references.empty() || !node.curves.empty() || !node.maps.empty() || !node.mode.IsEmpty())
        return Fail(diagnostics, "Width references/maps/mode are not supported");
    if (!std::isfinite(node.blend) || node.blend < 0 || node.blend > 1)
        return Fail(diagnostics, "Width blend must be in [0,1]");
    static const std::set<std::string> floats{
        "width", "rootScale", "tipScale", "taper", "taperStart", "mask:amount"};
    std::set<std::string> seen;
    for (auto const& param : node.params) {
        auto const& name = param.name.GetString();
        if (!seen.insert(name).second) return Fail(diagnostics, "duplicate Width parameter " + name);
        bool valid = false;
        if (floats.count(name)) valid = param.value.IsHolding<float>() && std::isfinite(param.value.UncheckedGet<float>());
        else if (name == "replace") valid = param.value.IsHolding<bool>();
        else if (name == "label") valid = param.value.IsHolding<std::string>();
        else if (name == "width:interpolation" || name == "mask:ramp:interpolation") valid = param.value.IsHolding<TfToken>();
        else if (name == "width:knots" || name == "mask:ramp:knots") {
            valid = param.value.IsHolding<VtVec2fArray>();
            float previous = -1;
            if (valid) for (auto const& knot : param.value.UncheckedGet<VtVec2fArray>()) {
                if (!std::isfinite(knot[0]) || !std::isfinite(knot[1]) || knot[0] < 0 || knot[0] > 1 || knot[0] < previous)
                    valid = false;
                previous = knot[0];
            }
        } else {
            auto it = IdentityMask().find(name);
            valid = it != IdentityMask().end() && param.value == it->second;
        }
        if (!valid) return Fail(diagnostics, "unsupported or malformed Width parameter " + name);
    }
    // Typed parameter ramps above are authoritative. Anonymous ramps cannot
    // be associated with a destination without guessing.
    for (auto const& ramp : node.ramps)
        if (!ramp.knots.empty() || !ramp.positions.empty() || !ramp.colors.empty())
            return Fail(diagnostics, "anonymous Width ramps require a named parameter");
    seen.clear();
    for (auto const& binding : node.expressionBindings) {
        auto name = LocalName(binding.destination);
        bool boolean = name == "replace" || name == "enabled";
        auto const& shape = binding.destinationShape;
        if ((!boolean && !floats.count(name) && name != "blend") ||
            !seen.insert(name).second || shape.isArray || shape.elementCount != 1 ||
            shape.components != 1 || shape.rows != 1 || shape.columns != 1 ||
            shape.scalar != (boolean ? expr::ScalarType::Bool : expr::ScalarType::Float32) ||
            binding.nativeType != TfToken(boolean ? "bool" : "float"))
            return Fail(diagnostics, "unsupported/incorrectly typed Width expression target " + name);
        if (name == "enabled" && binding.domain != expr::Domain::Groom)
            return Fail(diagnostics, "Width enabled must evaluate at groom granularity");
    }
    return true;
}
bool ValidateDeform(UsdGenNodeDesc const& node, UsdGenDiagnostics* diagnostics) {
    if (node.mode != TfToken("rbf") || node.algorithmVersion < 0 || node.algorithmVersion > 1)
        return Fail(diagnostics, "Deform requires the supported rbf mode/version");
    if (node.surfaces.size() != 1 || !node.references.empty() || !node.curves.empty() || !node.maps.empty())
        return Fail(diagnostics, "RBF Deform requires exactly one surface and no guide/map inputs");
    if ((!node.space.IsEmpty() && node.space != TfToken("auto") && node.space != TfToken("deformed")) ||
        (!node.readPhase.IsEmpty() && node.readPhase != TfToken("final")))
        return Fail(diagnostics, "RBF Deform requires deformed space and final driver sampling");
    if (!std::isfinite(node.blend) || node.blend < 0 || node.blend > 1)
        return Fail(diagnostics, "Deform blend must be in [0,1]");
    std::set<std::string> seen;
    for (auto const& param : node.params) {
        auto name = param.name.GetString();
        bool valid = seen.insert(name).second;
        if (name == "rbfSamples")
            valid &= param.value.IsHolding<int>() && param.value.UncheckedGet<int>() >= 4 && param.value.UncheckedGet<int>() <= 46336;
        else if (name == "lockRoots") valid &= param.value.IsHolding<bool>();
        else if (name == "twistAware") valid &= param.value == VtValue(true);
        else if (name == "preserveShape") valid &= param.value == VtValue(0.0f);
        else if (name == "preserveShape:iterations") valid &= param.value == VtValue(0);
        else if (name == "label") valid &= param.value.IsHolding<std::string>();
        else if (name == "mask:amount")
            valid &= param.value.IsHolding<float>() && std::isfinite(param.value.UncheckedGet<float>()) &&
                param.value.UncheckedGet<float>() >= 0 && param.value.UncheckedGet<float>() <= 1;
        else if (name == "mask:ramp:interpolation") valid &= param.value.IsHolding<TfToken>();
        else if (name == "mask:ramp:knots") {
            valid &= param.value.IsHolding<VtVec2fArray>();
            float previous = -1;
            if (valid) for (auto const& knot : param.value.UncheckedGet<VtVec2fArray>()) {
                valid &= std::isfinite(knot[0]) && std::isfinite(knot[1]) &&
                    knot[0] >= 0 && knot[0] <= 1 && knot[0] >= previous;
                previous = knot[0];
            }
        } else {
            auto found = IdentityMask().find(name);
            valid &= found != IdentityMask().end() && param.value == found->second;
        }
        if (!valid) return Fail(diagnostics, "unsupported or malformed RBF parameter " + name);
    }
    for (auto const& ramp : node.ramps)
        if (!ramp.knots.empty() || !ramp.colors.empty() || !ramp.positions.empty())
            return Fail(diagnostics, "RBF ramps require named parameters");
    seen.clear();
    for (auto const& binding : node.expressionBindings) {
        auto name = LocalName(binding.destination);
        bool boolean = name == "enabled" || name == "lockRoots";
        bool integer = name == "rbfSamples";
        auto const& shape = binding.destinationShape;
        if ((!boolean && !integer && name != "blend" && name != "mask:amount") ||
            !seen.insert(name).second || shape.isArray || shape.elementCount != 1 ||
            shape.components != 1 || shape.rows != 1 || shape.columns != 1 ||
            shape.scalar != (boolean ? expr::ScalarType::Bool : integer ? expr::ScalarType::Int32 : expr::ScalarType::Float32) ||
            binding.nativeType != TfToken(boolean ? "bool" : integer ? "int" : "float"))
            return Fail(diagnostics, "unsupported or incorrectly typed RBF expression " + name);
        if (((name == "enabled" || integer) && binding.domain != expr::Domain::Groom) ||
            (name == "lockRoots" && binding.domain == expr::Domain::Point))
            return Fail(diagnostics, "RBF structural/enabled controls require groom; root locking requires groom/primitive");
    }
    return true;
}
#endif
}

class UsdGenCudaExecutionPlan {
public:
    std::mutex mutex;
#ifdef USDGEN_ENABLE_CUDA
    struct RbfCache;
    struct Step {
        SdfPath path;
        TfToken type;
        CudaParameterPlan parameters;
        std::array<float, kUsdGenRampLutSize> profile{}, mask{};
        CudaSurfacePrepared surface;
        std::shared_ptr<RbfCache> rbf;
    };
    std::vector<std::unique_ptr<Step>> steps;
#endif
};

#ifdef USDGEN_ENABLE_CUDA
struct UsdGenCudaExecutionPlan::RbfCache {
    struct State {
        struct Resources {
            gpu::CudaSurfaceBinding surface;
            gpu::CudaRbfBinding field;
            gpu::DeviceBuffer<float3> currentVertices;
        };
        int device = -1;
        uint32_t sampleBudget = 0;
        uint64_t identity = 0;
        std::unique_ptr<Resources> resources = std::make_unique<Resources>();
        ~State() {
            int previous = -1;
            cudaGetDevice(&previous);
            if (device < 0 || cudaSetDevice(device) != cudaSuccess) {
                resources.release(); // cannot safely free a foreign/lost context
                return;
            }
            resources.reset();
            if (previous >= 0 && previous != device) cudaSetDevice(previous);
        }
    };
    std::mutex mutex;
    CudaSurfaceBindingKey key;
    std::unique_ptr<State> state;
    uint64_t bindCount = 0, solveCount = 0;
};

namespace {
bool SameRest(CudaSurfaceBindingKey const& a, CudaSurfaceBindingKey const& b) {
    // Runtime sample-count expressions control a rebind separately. Sharing
    // this cache must not depend on a frame's evaluated count or posed points.
    return a.path == b.path && a.restPoints == b.restPoints &&
        a.faceVertexCounts == b.faceVertexCounts && a.faceVertexIndices == b.faceVertexIndices &&
        a.algorithmVersion == b.algorithmVersion;
}
template<class T> bool Upload(gpu::DeviceBuffer<T>& buffer, std::vector<T> const& values) {
    return buffer.reset(values.size()) == cudaSuccess &&
        (values.empty() || cudaMemcpyAsync(buffer.data(), values.data(), values.size() * sizeof(T),
            cudaMemcpyHostToDevice, nullptr) == cudaSuccess);
}

bool UpdateRbf(UsdGenCudaExecutionPlan::Step& step, uint32_t budget,
               gpu::CudaCurveSource const& source, UsdGenDiagnostics* diagnostics) {
    auto& cache = *step.rbf;
    int device = -1;
    if (cudaGetDevice(&device) != cudaSuccess) return Fail(diagnostics, "cannot select RBF device");
    if (cache.state && cache.state->device != device)
        return Fail(diagnostics, "RBF binding belongs to a different CUDA device");
    if (!cache.state || cache.state->sampleBudget != budget) {
        auto candidate = std::make_unique<UsdGenCudaExecutionPlan::RbfCache::State>();
        candidate->device = device;
        candidate->sampleBudget = budget;
        auto& state = *candidate->resources;
        gpu::DeviceBuffer<float3> rest;
        gpu::DeviceBuffer<uint32_t> offsets, indices;
        if (!Upload(rest, step.surface.restPoints) || !Upload(offsets, step.surface.faceOffsets) ||
            !Upload(indices, step.surface.faceVertexIndices))
            return Fail(diagnostics, "RBF rest-surface upload failed");
        if (state.surface.Bind({rest.data(), rest.size()}, {offsets.data(), offsets.size()},
                offsets.size() - 1, {indices.data(), indices.size()}, budget, nullptr) != gpu::SurfaceBindingStatus::Ok ||
            state.surface.Finish(nullptr) != gpu::SurfaceBindingStatus::Ok)
            return Fail(diagnostics, std::string("RBF sample binding: ") + state.surface.diagnostic());
        if (state.field.Bind(state.surface.restSamples(), 0.0, nullptr) != gpu::RbfStatus::Ok)
            return Fail(diagnostics, std::string("RBF factorization: ") + state.field.diagnostic());
        static std::atomic<uint64_t> nextIdentity{1};
        candidate->identity = nextIdentity.fetch_add(1);
        cache.state = std::move(candidate);
        ++cache.bindCount;
    }
    auto& state = *cache.state->resources;
    if (!Upload(state.currentVertices, step.surface.currentPoints))
        return Fail(diagnostics, "RBF animated-surface upload failed");
    if (state.surface.Update({state.currentVertices.data(), state.currentVertices.size()},
            source.rootPrim(), source.rootUV(), nullptr) != gpu::SurfaceBindingStatus::Ok ||
        state.surface.Finish(nullptr) != gpu::SurfaceBindingStatus::Ok)
        return Fail(diagnostics, std::string("RBF current samples/roots: ") + state.surface.diagnostic());
    if (state.field.Solve(state.surface.currentSamples(), nullptr) != gpu::RbfStatus::Ok)
        return Fail(diagnostics, std::string("RBF pose solve: ") + state.field.diagnostic());
    ++cache.solveCount;
    return true;
}
} // namespace
#endif

std::vector<UsdGenCudaBindingStats> GetCudaBindingStats(UsdGenCudaExecutionPlan& plan) {
    std::lock_guard<std::mutex> lock(plan.mutex);
    std::vector<UsdGenCudaBindingStats> result;
#ifdef USDGEN_ENABLE_CUDA
    for (auto const& step : plan.steps) if (step->rbf) {
        std::lock_guard<std::mutex> cacheLock(step->rbf->mutex);
        auto const& cache = *step->rbf;
        result.push_back({step->path, cache.state ? cache.state->identity : 0,
            cache.bindCount, cache.solveCount,
            cache.state ? cache.state->resources->field.sampleCount() : 0});
    }
#endif
    return result;
}

bool ValidateCudaGraph(UsdGenGraphDesc const& desc, UsdGenDiagnostics* diagnostics) {
#ifndef USDGEN_ENABLE_CUDA
    (void)desc;
    return Fail(diagnostics, "backend is not built");
#else
    if (desc.nodes.empty() || desc.nodes.front().type != TfToken("UsdGenCurveSource"))
        return Fail(diagnostics, "CUDA hierarchy must begin with CurveSource");
    if (!std::isfinite(desc.timeCodesPerSecond) || desc.timeCodesPerSecond <= 0)
        return Fail(diagnostics, "timeCodesPerSecond must be finite and positive");
    bool sawDeform = false;
    for (size_t i = 1; i < desc.nodes.size(); ++i) {
        auto const& next = desc.nodes[i];
        if (next.inputs.size() != 1 || next.inputs[0] != desc.nodes[i-1].path)
            return Fail(diagnostics, "CUDA chain must follow the hierarchy-derived execution order");
        if (next.type == TfToken("UsdGenDeform")) {
            if (sawDeform) return Fail(diagnostics, "a second rest-to-animated deformation would apply surface motion twice");
            sawDeform = true;
            if (!ValidateDeform(next, diagnostics)) return false;
            if (desc.nodes.front().surfaces.size() != 1 ||
                desc.nodes.front().surfaces.front() != next.surfaces.front())
                return Fail(diagnostics, "RBF target must match the CurveSource root-binding surface");
        } else if (!ValidateWidth(next, diagnostics)) return false;
    }
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
    if (!desc.terminal.IsEmpty() && desc.terminal != desc.nodes.back().path)
        return Fail(diagnostics, "terminal does not match the hierarchy result");
    if (node.curves.size() != 1)
        return Fail(diagnostics, "CurveSource requires exactly one C3 curve target");
    if (node.surfaces.size() > 1)
        return Fail(diagnostics, "CurveSource binding requires one resolved parent surface");
    if (!std::isfinite(desc.defaultWidth) || desc.defaultWidth < 0)
        return Fail(diagnostics, "description default width must be finite and non-negative");
    if (!node.mode.IsEmpty()) return Fail(diagnostics, "CurveSource has no mode property");
    UsdGenParamView params; params.desc = &desc; params.node = &node;
    if (sawDeform && !params.GetBool(TfToken("useRest"), true))
        return Fail(diagnostics, "already-deformed CurveSource cannot feed rest-to-animated RBF Deform");
    if (sawDeform) {
        auto source = std::find_if(desc.curveSets.begin(), desc.curveSets.end(),
            [&](auto const& curves) { return curves.path == node.curves.front(); });
        if (desc.xformMatrix != GfMatrix4d(1.0) || source == desc.curveSets.end() ||
            source->worldMatrix != GfMatrix4d(1.0))
            return Fail(diagnostics, "RBF currently requires identity Description and CurveSource transforms");
    }
    if (params.GetToken(TfToken("lane"), TfToken("hair")) != TfToken("hair"))
        return Fail(diagnostics, "reference-lane CurveSource is not yet integrated");
    // Never silently ignore an authored effect just because this is a source.
    static const std::set<std::string> supported{
        "useRest", "idSource", "lane", "expectEpoch", "staleAction",
        "resampleTo", "rebind", "label"};
    auto const& identityMask = IdentityMask();
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

std::shared_ptr<UsdGenCudaExecutionPlan> CompileCudaGraph(
    UsdGenGraphDesc const& desc, UsdGenDiagnostics* diagnostics,
    std::shared_ptr<UsdGenCudaExecutionPlan> const& previous) {
    if (!ValidateCudaGraph(desc, diagnostics)) return {};
    auto plan = std::make_shared<UsdGenCudaExecutionPlan>();
#ifdef USDGEN_ENABLE_CUDA
    std::unique_lock<std::mutex> previousLock;
    if (previous) previousLock = std::unique_lock<std::mutex>(previous->mutex);
    for (size_t i = 1; i < desc.nodes.size(); ++i) {
        auto width = std::make_unique<UsdGenCudaExecutionPlan::Step>();
        width->path = desc.nodes[i].path;
        width->type = desc.nodes[i].type;
        std::vector<std::string> errors;
        if (CudaParameterPlan::Compile(desc, desc.nodes[i], &width->parameters, &errors) != CudaParameterStatus::Ok) {
            for (auto const& error : errors) Fail(diagnostics, error);
            if (errors.empty()) Fail(diagnostics, "operator parameter compilation failed");
            return {};
        }
        UsdGenParamView params; params.desc = &desc; params.node = &desc.nodes[i];
        auto lut = [&](char const* knotsName, char const* interpolationName, auto& output) {
            auto value = params.GetVtValue(TfToken(knotsName), VtValue(VtVec2fArray{}));
            UsdGenBuildRampLut(value.UncheckedGet<VtVec2fArray>(),
                params.GetToken(TfToken(interpolationName), TfToken("catmullRom")), output.data(), output.size());
        };
        if (width->type == TfToken("UsdGenWidth"))
            lut("width:knots", "width:interpolation", width->profile);
        lut("mask:ramp:knots", "mask:ramp:interpolation", width->mask);
        if (width->type == TfToken("UsdGenDeform")) {
            auto found = std::find_if(desc.surfaces.begin(), desc.surfaces.end(),
                [&](auto const& surface) { return surface.path == desc.nodes[i].surfaces.front(); });
            if (found == desc.surfaces.end()) { Fail(diagnostics, "missing RBF surface"); return {}; }
            if (PrepareCudaSurface(*found, params.GetInt(TfToken("rbfSamples"), 100),
                    desc.nodes[i].algorithmVersion == 0 ? 1 : desc.nodes[i].algorithmVersion,
                    &width->surface, &errors) != CudaSurfacePreparationStatus::Ok) {
                for (auto const& error : errors) Fail(diagnostics, error);
                return {};
            }
            if (previous) for (auto const& old : previous->steps)
                if (old->path == width->path && old->rbf && SameRest(old->rbf->key, width->surface.key)) {
                    width->rbf = old->rbf;
                    break;
                }
            if (!width->rbf) {
                width->rbf = std::make_shared<UsdGenCudaExecutionPlan::RbfCache>();
                width->rbf->key = width->surface.key;
            }
        }
        plan->steps.push_back(std::move(width));
    }
#else
    (void)previous;
#endif
    return plan;
}

std::shared_ptr<const UsdGenDeviceGeneration> ExecuteCudaGraph(
    UsdGenCudaExecutionPlan& plan, UsdGenGraphDesc const& desc, double frame,
    uint64_t generation, UsdGenDiagnostics* diagnostics) {
    std::lock_guard<std::mutex> lock(plan.mutex);
    if (!ValidateCudaGraph(desc, diagnostics)) return {};
#ifndef USDGEN_ENABLE_CUDA
    (void)generation;
    (void)frame;
    return {};
#else
    if (!std::isfinite(frame) || plan.steps.size() + 1 != desc.nodes.size()) {
        Fail(diagnostics, "invalid frame or mismatched compiled CUDA plan"); return {};
    }
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
    auto geometry = source->view();
    std::unique_ptr<gpu::DeviceBuffer<float>> finalWidths;
    std::unique_ptr<gpu::DeviceBuffer<float3>> finalPoints;
    bool deformed = !options.useRest;
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        auto& width = *plan.steps[i];
        auto const& op = desc.nodes[i+1];
        expr::Context context;
        context.frame = frame;
        context.time = frame / desc.timeCodesPerSecond;
        context.seed = op.seed;
        context.descId = expr::DescriptionId(desc.description.GetText());
        std::vector<std::string> errors;
        if (width.parameters.Evaluate(geometry, {source->hairT(), source->rootUV()},
                context, nullptr, &errors) != CudaParameterStatus::Ok) {
            for (auto const& error : errors) Fail(diagnostics, error);
            if (errors.empty()) Fail(diagnostics, "expression evaluation failed at " + op.path.GetString());
            return {};
        }
        UsdGenParamView values; values.desc = &desc; values.node = &op;
        auto field = [&](char const* name, float fallback) {
            if (auto const* expression = width.parameters.Find(TfToken(name)))
                return gpu::ScalarField::Device(
                    {static_cast<float const*>(expression->data), expression->count}, expression->domain);
            return gpu::ScalarField::Literal(static_cast<float>(values.GetDouble(TfToken(name), fallback)));
        };
        auto boolean = [&](char const* name, bool fallback) {
            if (auto const* expression = width.parameters.Find(TfToken(name)))
                return gpu::BoolField::Device(
                    {static_cast<uint8_t const*>(expression->data), expression->count}, expression->domain);
            return gpu::BoolField::Literal(values.GetBool(TfToken(name), fallback));
        };
        if (width.type == TfToken("UsdGenDeform")) {
            int budget = values.GetInt(TfToken("rbfSamples"), 100);
            if (auto const* expression = width.parameters.Find(TfToken("rbfSamples"))) {
                // This one groom-level integer changes binding structure. It
                // is control data, not a geometry/parameter-field readback.
                if (cudaMemcpy(&budget, expression->data, sizeof(budget), cudaMemcpyDeviceToHost) != cudaSuccess) {
                    Fail(diagnostics, "cannot read expression-driven RBF sample count"); return {};
                }
            }
            if (budget < 4 || budget > 46336) {
                Fail(diagnostics, "RBF sample count exceeds the supported solver range [4,46336]"); return {};
            }
            auto nextPoints = std::make_unique<gpu::DeviceBuffer<float3>>();
            if (nextPoints->reset(geometry.pointCount) != cudaSuccess) {
                Fail(diagnostics, "RBF point allocation failed"); return {};
            }
            if (geometry.pointCount) {
                if (source->rootPrim().size != geometry.curveCount || source->rootUV().size != geometry.curveCount) {
                    Fail(diagnostics, "RBF deformation requires persistent C3 root bindings"); return {};
                }
                std::lock_guard<std::mutex> cacheLock(width.rbf->mutex);
                if (!UpdateRbf(width, static_cast<uint32_t>(budget), *source, diagnostics)) return {};
                gpu::DeformParameters parameters;
                parameters.blend = field("blend", op.blend);
                parameters.maskAmount = field("mask:amount", 1);
                parameters.enabled = boolean("enabled", op.enabled);
                parameters.lockRoots = boolean("lockRoots", true);
                parameters.hairT = source->hairT();
                gpu::DeviceBuffer<float> mask;
                if (mask.reset(width.mask.size()) != cudaSuccess ||
                    cudaMemcpyAsync(mask.data(), width.mask.data(), sizeof(width.mask), cudaMemcpyHostToDevice, nullptr) != cudaSuccess) {
                    Fail(diagnostics, "RBF mask upload failed"); return {};
                }
                parameters.maskProfile = {mask.data(), mask.size()};
                auto& state = *width.rbf->state->resources;
                gpu::CudaRbfCurveDeformer kernel;
                if (kernel.Deform(state.field, geometry, state.surface.rootTargets(), parameters,
                        nextPoints->view(), nullptr) != gpu::RbfStatus::Ok ||
                    kernel.Finish(state.field, nullptr) != gpu::RbfStatus::Ok) {
                    Fail(diagnostics, "RBF deformation failed; previous generation retained"); return {};
                }
            }
            if (nextPoints->recordUse(nullptr) != cudaSuccess) {
                Fail(diagnostics, "RBF publication event failed"); return {};
            }
            finalPoints = std::move(nextPoints);
            geometry.points = {finalPoints->data(), finalPoints->size()};
            deformed = true;
            continue;
        }
        gpu::WidthParameters parameters;
        parameters.width = field("width", .01f);
        parameters.rootScale = field("rootScale", 1);
        parameters.tipScale = field("tipScale", 1);
        parameters.taper = field("taper", 0);
        parameters.taperStart = field("taperStart", .5f);
        parameters.blend = field("blend", op.blend);
        parameters.maskAmount = field("mask:amount", 1);
        parameters.enabled = boolean("enabled", op.enabled);
        parameters.replace = boolean("replace", true);
        gpu::DeviceBuffer<float> profile, mask;
        auto nextWidths = std::make_unique<gpu::DeviceBuffer<float>>();
        if (profile.reset(width.profile.size()) != cudaSuccess || mask.reset(width.mask.size()) != cudaSuccess ||
            nextWidths->reset(geometry.pointCount) != cudaSuccess ||
            cudaMemcpyAsync(profile.data(), width.profile.data(), sizeof(width.profile), cudaMemcpyHostToDevice, nullptr) != cudaSuccess ||
            cudaMemcpyAsync(mask.data(), width.mask.data(), sizeof(width.mask), cudaMemcpyHostToDevice, nullptr) != cudaSuccess) {
            Fail(diagnostics, "Width device allocation/upload failed"); return {};
        }
        parameters.widthProfile = {profile.data(), profile.size()};
        parameters.maskProfile = {mask.data(), mask.size()};
        gpu::CudaWidth kernel;
        if (kernel.Apply(geometry, source->hairT(), parameters, nextWidths->view(), nullptr) != gpu::StyleStatus::Ok ||
            kernel.Finish(nullptr) != gpu::StyleStatus::Ok || nextWidths->recordUse(nullptr) != cudaSuccess) {
            Fail(diagnostics, "Width execution failed at " + op.path.GetString() + "; previous generation retained"); return {};
        }
        // Finish validates and completes the consumer before its predecessor
        // is freed. Points/rest/topology/IDs remain in the original allocation.
        finalWidths = std::move(nextWidths);
        geometry.widths = {finalWidths->data(), finalWidths->size()};
    }
    std::string reason;
    auto result = gpu::MakeSourceGeneration(std::move(source), generation, &reason,
        deformed, std::move(finalWidths), std::move(finalPoints));
    if (!result) Fail(diagnostics, reason);
    return result;
#endif
}
} // namespace usdGen
