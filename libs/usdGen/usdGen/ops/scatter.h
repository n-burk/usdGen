// usdGen — UsdGenScatterOp (M1, mode="random" only). 02-schema.md §2.6,
// 04-operators.md §2.1. Emits roots on the rest surface with stable ids:
//   curveId = UsdGenHash64(seed, faceIndex, k, kSaltScatter) (R12).
// densityScale / renderDensityScale are NOT applied here: they decimate the
// captured root set by stable id at publish time, so a density scrub never
// re-runs Capture (ADR §9 R13).
#ifndef USDGEN_OP_SCATTER_H
#define USDGEN_OP_SCATTER_H

#include "usdGen/op.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

class UsdGenScatterOp final : public UsdGenOp
{
public:
    UsdGenScatterOp() = default;
    ~UsdGenScatterOp() override = default;

    TfToken Type() const override { return TfToken("UsdGenScatter"); }
    UsdGenSpace Space() const override { return UsdGenSpace::Rest; }
    UsdGenTopoFx TopologyEffect() const override { return UsdGenTopoFx::CurveCount; }
    UsdGenRole Role() const override { return UsdGenRole::Curves; }
    bool IsGenerator() const override { return true; }

    TfSpan<const TfToken> TopologyParameters() const override;
    TfSpan<const TfToken> ValueParameters() const override;
    /// atGuides mode (M3): the guide set is a reference input.
    TfSpan<const TfToken> ReferenceInputs() const override;

    bool Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag) override;
    UsdGenEpoch CaptureDigest(UsdGenCaptureContext const &ctx) const override;
    bool Capture(UsdGenCaptureContext const &ctx,
                 UsdGenCurveBuffer const &upstream,
                 UsdGenCapture *out,
                 UsdGenDiagnostics *diag) override;
    void Evaluate(UsdGenEvalContext const &ctx,
                  UsdGenCapture const &capture,
                  UsdGenChunkView *view) const override;
    std::unique_ptr<UsdGenCapture> CreateCapture() const override;
    uint32_t PlanesTouched() const override;
};

}  // namespace usdGen

#endif  // USDGEN_OP_SCATTER_H
