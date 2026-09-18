// usdGen — UsdGenCollideOp. 02-schema.md §2.8, 04-operators.md §4,
// docs/freezes/C1.md §2 (UsdGenCollide row).
//
// Iterative push-out from collider meshes. Every surface named in the node's
// surfaces list is a collider: the inherited bound skin (front) plus the
// usdGen:colliders targets the builders append after it, so this matches 02
// §1's "out of colliders and the skin" with no boundary marker and no change
// to root-surface/front semantics. Only deformed points are read.
//
// Per iteration, per CV p: closest point q over all collider triangles
// (node order, ascending faces, fan triangulation, first-win ties),
// penetration pen = offset - |p - q|; no push when pen <= 0. The push is
// dir * pen * pushAmount * mask with dir = (p - q)/|p - q| (the winning
// triangle's unit normal when p == q), computed in double precision.
// resolveType = flexible pushes each CV independently (A7 S10: "push CVs to
// closest surface point"); stiff finds the first penetrating CV and moves
// the strand rigidly — a translation when the root penetrates, otherwise a
// rotation of the downstream CVs about the last clean CV (the hinge), which
// preserves every segment length (A7 S10: "rotate first intersecting
// segment"). mask (the UsdGenDeformer envelope) clamps to [0,1]; with
// pushAmount == 0, mask == 0, or no penetration, Evaluate copies the input
// bit-for-bit. TopologyEffect = None. CPU lane only: the CUDA capability
// matrix has no UsdGenCollide row, so the planner rejects the graph exactly
// like any other CPU-only operator (Clump precedent).
#ifndef USDGEN_OP_COLLIDE_H
#define USDGEN_OP_COLLIDE_H

#include "usdGen/op.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

class UsdGenCollideOp final : public UsdGenOp
{
public:
    UsdGenCollideOp() = default;
    ~UsdGenCollideOp() override = default;

    TfToken Type() const override { return TfToken("UsdGenCollide"); }
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
};

}  // namespace usdGen

#endif  // USDGEN_OP_COLLIDE_H
