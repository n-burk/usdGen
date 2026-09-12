#ifndef USDGEN_OP_DEFORM_H
#define USDGEN_OP_DEFORM_H

#include "usdGen/op.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// Metadata for the topology-preserving persistent CUDA RBF deformation.
/// Host execution is unavailable; it must never synthesize displacement.
class UsdGenDeformOp final : public UsdGenOp
{
public:
   UsdGenDeformOp();
   ~UsdGenDeformOp() override = default;

   TfToken Type() const override { return TfToken("UsdGenDeform"); }
   UsdGenSpace Space() const override { return UsdGenSpace::Deformed; }
   UsdGenTopoFx TopologyEffect() const override { return UsdGenTopoFx::None; }
   UsdGenRole Role() const override { return UsdGenRole::Curves; }
   bool IsGenerator() const override { return false; }

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

private:
   TfTokenVector topologyParameters_;
   TfTokenVector valueParameters_;
};

}  // namespace usdGen

#endif  // USDGEN_OP_DEFORM_H
