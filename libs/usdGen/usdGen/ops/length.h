// usdGen — UsdGenLengthOp (M1). 02-schema.md §2.7.1, 04-operators.md §2.10.
// Set, scale, cut or cull strand length. The WHOLE type is topology-bumping
// (ADR §9 R14): static TopologyEffect() == CurveCount because cull mode
// exists, so the classification does not depend on usdGen:length:mode.
#ifndef USDGEN_OP_LENGTH_H
#define USDGEN_OP_LENGTH_H

#include "usdGen/op.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

class UsdGenLengthOp final : public UsdGenOp
{
public:
    UsdGenLengthOp() = default;
    ~UsdGenLengthOp() override = default;

    TfToken Type() const override { return TfToken("UsdGenLength"); }
    UsdGenTopoFx TopologyEffect() const override { return UsdGenTopoFx::CurveCount; }

    TfSpan<const TfToken> TopologyParameters() const override;
    TfSpan<const TfToken> ValueParameters() const override;

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
private:
    const TfToken sSet{"set"}, sMask{"mask"};
    const TfToken sValue{"length:value"}, sCull{"cullThreshold"};
    const TfToken sMinRemaining{"minRemainingLength"};
};

}  // namespace usdGen

#endif  // USDGEN_OP_LENGTH_H
