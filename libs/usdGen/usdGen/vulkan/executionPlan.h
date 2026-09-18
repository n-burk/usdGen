// Descriptor compiler for the explicitly injected Vulkan geometry-value path.
#ifndef USDGEN_VULKAN_EXECUTION_PLAN_H
#define USDGEN_VULKAN_EXECUTION_PLAN_H

#include "usdGen/executionBackend.h"

#include <memory>

namespace usdGen::vulkan {

// Literal scalar controls accepted by the flat Width shader. Ramps, maps,
// profiles and expression-driven controls remain excluded from this lane.
struct VulkanLiteralWidthControls {
    float width = 0.01f;
    bool replace = true;
};
struct VulkanLiteralNoiseControls {
    float magnitude = 0.05f;
    float frequency = 3.0f;
    float correlation = 0.5f;
    int32_t octaves = 1;
    float lacunarity = 2.0f;
    float gain = 0.5f;
    float preserveLength = 1.0f;
    float mask = 1.0f;
    int32_t seed = 0;
    bool cumulative = false;
};

struct VulkanSourceWidthStage {
    enum class Kind { Width, LengthScale, WidthBlend, LengthCull, Noise };
    enum class LengthMode : uint32_t { Scale = 0, Set = 1 };
    enum class LengthMethod : uint32_t { Scale = 0, CutExtend = 1 };
    enum class LengthRebuild : uint32_t { KeepParam = 0, Reparam = 1 };
    Kind kind = Kind::Width;
    // Muted operators alias their input: no dispatch, no new value. The
    // flag is plan data so toggling never recompiles native state.
    bool disabled = false;
    // Value 0 is Source; stage i publishes value i+1. Binary inputs retain
    // their authored left/right order even when the stage list is sorted.
    uint32_t input = 0, rightInput = 0;
    SdfPath path;
    VulkanLiteralWidthControls width;
    float factor = 1.0f, blend = 0.5f;
    LengthMode lengthMode = LengthMode::Scale;
    LengthMethod lengthMethod = LengthMethod::Scale;
    LengthRebuild lengthRebuild = LengthRebuild::KeepParam;
    float minRemainingLength = 0.0f;
    float randomLo = 1.0f, randomHi = 1.0f;
    int32_t randomSeed = 0;
    // EnvelopeV1 is deliberately literal-only. These are copied onto every
    // lowered stage so native admission never needs to revisit authoring.
    float lengthBlend = 1.0f, lengthMaskAmount = 1.0f;
    bool lengthCullOnly = false;
    bool requiresNonWidthProof = false;
    float cullThreshold = 0.0f;
    VulkanLiteralNoiseControls noise;
};

// All source options admitted by this compiler.  These are copied out for a
// later executor instead of being treated as compile-time no-ops.
struct VulkanSourceControls {
    bool useRest = true;
    bool hasAuthoredRest = false;
    bool restFromCurrentPoints = false;
    bool stableIdsSynthesized = false;
    TfToken idSource{"primvar"};
    std::string expectEpoch;
    TfToken staleAction{"warn"};
    TfToken rebind{"onError"};
};

// Typed, native-free payload behind UsdGenExecutionPlanHandle::Payload().
// It owns a full immutable descriptor capture as well as the selected C3
// source snapshot, so the native executor never consults a live graph. The
// historical class name is retained for compatibility with linear callers.
class VulkanSourceWidthPlan final {
public:
    std::shared_ptr<const UsdGenGraphDesc> const& Descriptor() const noexcept {
        return descriptor_;
    }
    UsdGenCurveSetDesc const& Source() const noexcept { return source_; }
    SdfPath const& SourceNodePath() const noexcept { return sourceNodePath_; }
    SdfPath const& WidthNodePath() const noexcept { return widthNodePath_; }
    VulkanSourceControls const& SourceControls() const noexcept { return sourceControls_; }
    VulkanLiteralWidthControls const& Width() const noexcept { return width_; }
    bool HasLength() const noexcept { return !lengthNodePath_.IsEmpty(); }
    float LengthFactor() const noexcept { return lengthFactor_; }
    SdfPath const& LengthNodePath() const noexcept { return lengthNodePath_; }
    std::vector<VulkanSourceWidthStage> const& Steps() const noexcept { return steps_; }
    size_t IntermediateCount() const noexcept { return steps_.empty() ? 0 : steps_.size() - 1; }
    uint64_t CurveGeneration() const noexcept { return curveGeneration_; }
    uint64_t TopologyVersion() const noexcept { return topologyVersion_; }
    uint64_t ValueVersion() const noexcept { return valueVersion_; }
    // Root capture produces this bundle only for a non-empty source with a
    // surface relationship or authored root channels.  The legacy
    // surface-free/no-root shape deliberately preserves its prior metadata.
    // It is deliberately separate from named channels: rootPrim/rootUV and
    // the private T/B/N frame are executor-owned structural data.
    bool HasRootBindings() const noexcept { return hasRootBindings_; }

private:
    friend std::shared_ptr<const UsdGenExecutionPlanHandle>
    CompileVulkanSourceWidthPlan(UsdGenGraphDesc const&, UsdGenDiagnostics*);
    VulkanSourceWidthPlan(std::shared_ptr<const UsdGenGraphDesc>,
                          UsdGenCurveSetDesc, SdfPath, SdfPath,
                          VulkanSourceControls, VulkanLiteralWidthControls, uint64_t, uint64_t,
                          uint64_t, bool) noexcept;
    std::shared_ptr<const UsdGenGraphDesc> descriptor_;
    UsdGenCurveSetDesc source_;
    SdfPath sourceNodePath_, widthNodePath_;
    VulkanSourceControls sourceControls_;
    VulkanLiteralWidthControls width_;
    SdfPath lengthNodePath_;
    float lengthFactor_ = 1.0f;
    std::vector<VulkanSourceWidthStage> steps_;
    uint64_t curveGeneration_ = 0, topologyVersion_ = 0, valueVersion_ = 0;
    bool hasRootBindings_ = false;
};

// Descriptor-only compilation: no backend registration or native allocation.
// Supports one CurveSource and bounded Width/LengthScale/WidthBlend stages,
// topologically sorted from arbitrary authored order while preserving ordered
// binary inputs. Every authored node must reach the Width/WidthBlend terminal.
// Joins across distinct Length lineages require native non-width equality proof; other
// operator families are not admitted.
std::shared_ptr<const UsdGenExecutionPlanHandle>
CompileVulkanSourceWidthPlan(UsdGenGraphDesc const&, UsdGenDiagnostics* = nullptr);

} // namespace usdGen::vulkan

#endif // USDGEN_VULKAN_EXECUTION_PLAN_H
