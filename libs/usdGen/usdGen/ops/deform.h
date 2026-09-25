#ifndef USDGEN_OP_DEFORM_H
#define USDGEN_OP_DEFORM_H

#include "usdGen/op.h"
#include "usdGen/ops/rbfField.h"

#include "pxr/base/gf/vec3d.h"

#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// UsdGenDeform: topology-preserving cubic RBF deformation.
///
/// CUDA lane: driven by samples of the bound surface (the persistent RBF
/// library in cudaExecution.cpp); this class supplies its metadata only.
/// CPU lane: driven by usdGen:guides, or the bound surface when absent. Every
/// driver CV (up to usdGen:rbfSamples of them, farthest-point sampled) is an
/// RBF sample bound at its rest position and moved to its current one, and
/// every incoming CV moves by the resulting field. Surface input requires
/// Default-time rest data and the same object space as the groom; a shared
/// animated parent is applied once by publication.
class UsdGenDeformOp final : public UsdGenOp
{
public:
   UsdGenDeformOp();
   ~UsdGenDeformOp() override = default;

   TfToken Type() const override { return TfToken("UsdGenDeform"); }
   UsdGenTopoFx TopologyEffect() const override { return UsdGenTopoFx::None; }
   UsdGenRole Role() const override { return UsdGenRole::Curves; }
   bool IsGenerator() const override { return false; }

   TfSpan<const TfToken> TopologyParameters() const override;
   TfSpan<const TfToken> ValueParameters() const override;
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

private:
   TfTokenVector topologyParameters_;
   TfTokenVector valueParameters_;
   // The factored system survives poses that keep the same rest samples.
   rbf::CubicField field_;
   std::vector<GfVec3d> boundRest_;
};

}  // namespace usdGen

#endif  // USDGEN_OP_DEFORM_H
