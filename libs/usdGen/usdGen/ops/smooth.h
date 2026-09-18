// usdGen — UsdGenSmoothOp. 02-schema.md §2.7.1, 04-operators.md §2.12.
// Laplacian smoothing in two modes: alongCurve relaxes each strand toward
// its own neighbours (new[i] = lerp(old[i], neighborAvg, strength * mask),
// endpoints clamped to the single adjacent CV, CV0 pinned by lockRoot);
// neighbours blends each CV toward the same-index CVs of nearby strands
// (roots within searchRadius, up to numNeighbors nearest, self excluded).
// TopologyEffect = None.
#ifndef USDGEN_OP_SMOOTH_H
#define USDGEN_OP_SMOOTH_H

#include "usdGen/op.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

class UsdGenSmoothOp final : public UsdGenOp
{
public:
    UsdGenSmoothOp() = default;
    ~UsdGenSmoothOp() override = default;

    TfToken Type() const override { return TfToken("UsdGenSmooth"); }
    UsdGenTopoFx TopologyEffect() const override { return UsdGenTopoFx::None; }

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
    const TfToken sStrength{"strength"}, sIterations{"iterations"};
    const TfToken sMode{"mode"}, sLockRoot{"lockRoot"};
    const TfToken sSearchRadius{"searchRadius"}, sNumNeighbors{"numNeighbors"};
    const TfToken sAlongCurve{"alongCurve"}, sNeighbours{"neighbours"};
    const TfToken sMask{"mask"};
};

}  // namespace usdGen

#endif  // USDGEN_OP_SMOOTH_H
