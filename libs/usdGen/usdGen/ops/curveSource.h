#ifndef USDGEN_OP_CURVESOURCE_H
#define USDGEN_OP_CURVESOURCE_H

#include "usdGen/op.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// UsdGenCurveSourceOp — loads curves from the scene index into a buffer.
/// Produces uniform or ragged chunks depending on the source data.
class UsdGenCurveSourceOp final : public UsdGenOp
{
public:
   UsdGenCurveSourceOp() = default;
   ~UsdGenCurveSourceOp() override = default;

   TfToken Type() const override { return TfToken("UsdGenCurveSource"); }
   UsdGenSpace Space() const override { return UsdGenSpace::Rest; }
   UsdGenTopoFx TopologyEffect() const override { return UsdGenTopoFx::CurveCount; }
   UsdGenRole Role() const override { return UsdGenRole::Curves; }
   bool IsGenerator() const override { return true; }

   TfSpan<const TfToken> TopologyParameters() const override;
   TfSpan<const TfToken> ValueParameters() const override;
   TfSpan<const TfToken> ReferenceInputs() const override { return {}; }
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

#endif  // USDGEN_OP_CURVESOURCE_H