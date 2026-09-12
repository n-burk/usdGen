// usdGen engine — operator kernel interface (03-execution-engine.md §8.1).
//
// Contract rules (03 §8.1 table):
//  * Evaluate is const, allocates nothing, touches no stage, reads nothing
//    outside view/capture/ctx (S8).
//  * Capture may allocate into its own node's capture and buffer under the
//    graph's scheduled work owner; it runs there or inside a tbb::parallel_for
//    over independent nodes in the private arena (R22).
//  * TopologyParameters() ∪ ValueParameters() == the node's mapped property set
//    (S14); asserted in debug builds (USDGEN_OP_CHECKS=1).
//  * Cross-curve reads come from capture data plus reference buffers, never
//    from another chunk (I3).
//  * Kernels live in usdGenMath as free functions over raw pointers, compiled
//    -ffp-contract=off, deterministic reductions.
#ifndef USDGEN_OP_H
#define USDGEN_OP_H

#include "usdGen/curveBuffer.h"
#include "usdGen/graphDesc.h"
#include "usdGen/mask.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"

#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

class UsdGenWorkDispatcher;  // defined in usdGen/scheduler.h

/// Diagnostics collected by a node; the imaging layer is the one that logs
/// (the engine core stays TF_WARN-free on hot paths, 03 §9.2).
struct UsdGenDiagnostics
{
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool HasErrors() const { return !errors.empty(); }
    void Error(std::string msg)    { errors.push_back(std::move(msg)); }
    void Warn(std::string msg)     { warnings.push_back(std::move(msg)); }
};

/// This node's resolved parameters and ramps, handed in by the scheduler at
/// Bind time. Ramps arrive already resolved (03 §2.2 UsdGenRampDesc); the
/// adapter does the S11 encoding, the kernel does the LUT (257 entries).
///
/// 26.08 VtValue note: there is no GetAsSafe/SafeGet; the getters below use
/// IsHolding<T>() + UncheckedGet<T>() with the C1 default as fallback.
struct UsdGenParamView
{
    UsdGenGraphDesc const *desc = nullptr;
    UsdGenNodeDesc const  *node = nullptr;

    /// nullptr when the property is not authored and carries no C1 default in
    /// the node description (the builder materializes C1 defaults).
    UsdGenParamValue const *FindParam(TfToken const &name) const
    {
        if (!node) return nullptr;
        for (auto const &p : node->params) if (p.name == name) return &p;
        return nullptr;
    }
    TfToken GetToken(TfToken const &name, TfToken fallback) const
    {
        if (UsdGenParamValue const *p = FindParam(name)) {
            if (p->value.IsHolding<TfToken>())
                return p->value.UncheckedGet<TfToken>();
            if (p->value.IsHolding<std::string>())
                return TfToken(p->value.UncheckedGet<std::string>().c_str());
        }
        return fallback;
    }
    double GetDouble(TfToken const &name, double fallback) const
    {
        if (UsdGenParamValue const *p = FindParam(name)) {
            if (p->value.IsHolding<double>()) return p->value.UncheckedGet<double>();
            if (p->value.IsHolding<float>())  return p->value.UncheckedGet<float>();
        }
        return fallback;
    }
    int GetInt(TfToken const &name, int fallback) const
    {
        if (UsdGenParamValue const *p = FindParam(name)) {
            if (p->value.IsHolding<int>()) return p->value.UncheckedGet<int>();
            // Integer-valued authored VtValues are tolerated (schema declares
            // e.g. mask:randomSeed as `uniform int`; callers may author the
            // unsigned spelling of the same value).
            if (p->value.IsHolding<uint32_t>())
                return static_cast<int>(p->value.UncheckedGet<uint32_t>());
        }
        return fallback;
    }
    bool GetBool(TfToken const &name, bool fallback) const
    {
        if (UsdGenParamValue const *p = FindParam(name)) {
            if (p->value.IsHolding<bool>()) return p->value.UncheckedGet<bool>();
        }
        return fallback;
    }
    VtValue GetVtValue(TfToken const &name, VtValue const &fallback) const
    {
        if (UsdGenParamValue const *p = FindParam(name)) return p->value;
        return fallback;
    }
};

/// Opaque per-node capture payload; M1 operators derive their own concrete
/// payload types. Lifetime: owned by the compiled node, keyed by the node's
/// capture epoch (03 §3.4).
struct UsdGenCapture
{
    virtual ~UsdGenCapture() = default;
    /// True when this capture is usable for the given upstream topology
    /// (curve/CV counts); false forces a re-capture.
    virtual bool ValidForTopology(UsdGenCurveBuffer const &) const { return true; }
    /// The buffer this capture owns/wraps (a generator's output topology);
    /// default-constructed (empty) for non-generator captures.
    virtual UsdGenCurveBuffer const &Buffer() const { return _buffer; }
    /// True for generator captures: the framework installs Buffer() into the
    /// owning node after a successful Capture.
    virtual bool OwnsBuffer() const { return false; }
    UsdGenCurveBuffer &MutableBuffer() { return _buffer; }
    /// Type-erased copy for the incremental recompile path (E-6);
    /// concrete payloads override.
    virtual std::unique_ptr<UsdGenCapture> Clone() const { return nullptr; }
protected:
    UsdGenCurveBuffer _buffer;
};

/// M1 shared capture payload base: op-specific per-curve / per-CV scalars
/// plus the mask resolution (02 §2.13, I4: computed once per capture epoch).
/// Copyable (VtArray CoW); every CONCRETE payload must override Clone() with
/// a full copy of its own derived type — a base-level clone would silently
/// slice the op-specific fields (the E-6 reuse defect this closes).
struct UsdGenCapturePayload : public UsdGenCapture
{
    VtFloatArray perCurve;      // op-specific meaning (target length, ...)
    VtFloatArray perCv;         // op-specific meaning (noise displacement field, ...)
    VtFloatArray curveMask;     // resolved per-curve mask; empty == all 1.0
    VtFloatArray rampLut;       // 257-entry op-specific LUT over hairT; empty == unused
    VtFloatArray maskRampLut;   // 257-entry mask ramp LUT over hairT; empty == unused

    /// Abstract: every concrete payload overrides this with a full-type copy
    /// (`return std::make_unique<Self>(*this);` — see ops/*.cpp).
    std::unique_ptr<UsdGenCapture> Clone() const override = 0;
};

/// Resolve this node's UsdGenMaskAPI settings from its mapped parameters
/// (02 §2.13 + §2.6 mask rows). The caller then runs UsdGenMaskSettings +
/// EvaluateMask() with the curve ids (the random term is per-curve).
UsdGenMaskSettings UsdGenMaskSettingsFromParams(UsdGenParamView const &params);

struct UsdGenCaptureContext
{
    UsdGenGraphDesc const *desc = nullptr;   // surfaces, maps, density scales
    UsdGenParamView const *params = nullptr; // this node's resolved parameters and ramps
    UsdGenReferenceSet const **references = nullptr; // resolved ReferenceInputs(), evaluated
    UsdGenSurfaceId        surface = 0;
    UsdGenReadPhase        readPhase = UsdGenReadPhase::Final;
    uint32_t               seed = 0;
    uint64_t               upstreamGeneration = 0; // upstream buffer topologyVersion
    UsdGenWorkDispatcher  *dispatcher = nullptr;  // capture may parallelise in the arena
    UsdGenDiagnostics     *diag = nullptr;
};

struct UsdGenEvalContext
{
    double time = 0.0;
    double shutterOffset = 0.0;              // 0 in P0/P1 (03 §7)
    UsdGenGraphDesc const *desc = nullptr;   // surface points already resolved to time+offset
    UsdGenParamView const *params = nullptr;
    UsdGenReferenceSet const **references = nullptr;
    uint32_t seed = 0;
};

/// The operator kernel interface. Five M1 types: UsdGenScatterOp,
/// UsdGenGrowOp, UsdGenNoiseOp, UsdGenLengthOp, UsdGenWidthOp (02 §2.6/§2.7.1).
class UsdGenOp
{
public:
    virtual ~UsdGenOp() = default;

    // ---- static description, read at compile -------------------------------
    /// C1 type name, e.g. "UsdGenScatter".
    virtual TfToken Type() const = 0;
    /// What usdGen:space="auto" resolves to for this type (S25).
    virtual UsdGenSpace Space() const { return UsdGenSpace::Rest; }
    /// Type-level fallback when no usdGen:readPhase is authored (R9, default Final).
    virtual UsdGenReadPhase ReadPhase() const { return UsdGenReadPhase::Final; }
    /// Static TopologyEffect (R14): e.g. UsdGenLengthOp is always CurveCount
    /// because cull mode exists.
    virtual UsdGenTopoFx TopologyEffect() const { return UsdGenTopoFx::None; }
    virtual UsdGenRole Role() const { return UsdGenRole::Curves; }
    /// C1-frozen parameter partition (02 §6 rows; docs/freezes/C1.md):
    /// TopologyParameters = capture/topology-class edits, ValueParameters =
    /// per-frame edits. The union must equal the node's mapped property set
    /// (S14); usdGen:enabled joins whichever set its dirty class implies
    /// (02 §6.2 topology for generators/Length, §6.3 value-toggle for the rest).
    virtual TfSpan<const TfToken> TopologyParameters() const = 0;
    virtual TfSpan<const TfToken> ValueParameters() const = 0;
    /// Named reference inputs (guide sets, clump centres, card roots).
    virtual TfSpan<const TfToken> ReferenceInputs() const { return {}; }
    /// Extra planes this operator writes, in slot order (R24 fixed types/arity).
    virtual TfSpan<const TfToken> OutputPrimvars() const { return {}; }
    /// Upstream planes this operator reads, in slot order.
    virtual TfSpan<const TfToken> InputPrimvars() const { return {}; }

    // ---- binding -------------------------------------------------------------
    virtual bool Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag) = 0;

    // ---- capture: topology-dependent, cached by epoch (03 §3.4) --------------
    virtual UsdGenEpoch CaptureDigest(UsdGenCaptureContext const &ctx) const = 0;
    /// Fill *out (derive the concrete payload). A generator (TopologyEffect != None)
    /// must also populate its output topology (chunks, cvCount, totalCurves)
    /// into *out->buffer when it creates curves.
    virtual bool Capture(UsdGenCaptureContext const &ctx,
                         UsdGenCurveBuffer const &upstream,
                         UsdGenCapture *out,
                         UsdGenDiagnostics *diag) = 0;

    // ---- evaluate: per chunk, parallel, allocation-free (03 §8.1) ------------
    virtual void Evaluate(UsdGenEvalContext const &ctx,
                          UsdGenCapture const &capture,
                          UsdGenChunkView *view) const = 0;

    // ---- generators only ------------------------------------------------------
    /// When true, Capture produces the node's topology from scratch (no
    /// upstream buffer). False for all stylers/deformers.
    virtual bool IsGenerator() const { return false; }

    /// Allocate the concrete capture payload for this operator. The payload
    /// owns its per-curve results; generators additionally fill
    /// UsdGenCapture::MutableBuffer() (OwnsBuffer() == true).
    virtual std::unique_ptr<UsdGenCapture> CreateCapture() const
    {
        return std::make_unique<UsdGenCapture>();
    }

    /// Plane bitmask (03 §8.5 envelope + tile-change tracking): which CV
    /// planes Evaluate() may write for this operator type.
    static constexpr uint32_t kPlanePoints = 1u << 0;
    static constexpr uint32_t kPlaneWidths = 1u << 1;
    static constexpr uint32_t kPlaneHairT  = 1u << 2;
    virtual uint32_t PlanesTouched() const { return 0; }
};

/// The blend envelope, applied by the framework, not by kernels (03 §8.5).
/// Exact endpoints at 0 and 1 (usdRig's RigExecBlendEnvelope contract):
/// w <= 0 aliases the input; w >= 1 runs no blend pass.
void UsdGenBlendEnvelope(float const *in, float *out, float w, size_t n);
void UsdGenBlendEnvelopeVec3(float const *inPx, float const *inPy, float const *inPz,
                             float *outPx, float *outPy, float *outPz, float w, size_t n);

}  // namespace usdGen

#endif  // USDGEN_OP_H
