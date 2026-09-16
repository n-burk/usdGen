// Deform's actual RBF execution lives in the persistent CUDA graph plan.
// The operator registry supplies metadata; no synthetic CPU displacement is
// a valid substitute for rest-bound animation.
#include "usdGen/ops/deform.h"
#include "usdGen/opParams.h"

namespace usdGen {

UsdGenDeformOp::UsdGenDeformOp()
    : topologyParameters_(UsdGenBaseTopologyParams()),
      valueParameters_(UsdGenBaseValueParams())
{
    topologyParameters_.push_back(TfToken("rbfSamples"));
    valueParameters_.push_back(TfToken("enabled"));
    valueParameters_.push_back(TfToken("lockRoots"));
    valueParameters_.push_back(TfToken("mask"));   // operator envelope (02 §2.13)
}

bool UsdGenDeformOp::Bind(UsdGenParamView const& params, UsdGenDiagnostics* diagnostics) {
    if (!params.desc || params.desc->executionBackend != UsdGenExecutionBackend::Cuda) {
        if (diagnostics) diagnostics->Error("UsdGenDeform requires the CUDA RBF executor; CPU reference deformation is unavailable");
        return false;
    }
    if (!params.node || !params.node->mode.IsEmpty()) {
        if (diagnostics) diagnostics->Error("UsdGenDeform has no mode property; it is always RBF");
        return false;
    }
    return true;
}

TfSpan<const TfToken> UsdGenDeformOp::TopologyParameters() const {
    return TfSpan<const TfToken>(topologyParameters_.data(), topologyParameters_.size());
}
TfSpan<const TfToken> UsdGenDeformOp::ValueParameters() const {
    return TfSpan<const TfToken>(valueParameters_.data(), valueParameters_.size());
}

UsdGenEpoch UsdGenDeformOp::CaptureDigest(UsdGenCaptureContext const&) const {
    // CUDA rest-cache identity is exact surface data + correspondence +
    // algorithm policy, not a host capture keyed by the animated pose.
    return {};
}
std::unique_ptr<UsdGenCapture> UsdGenDeformOp::CreateCapture() const {
    return std::make_unique<UsdGenCapture>();
}
uint32_t UsdGenDeformOp::PlanesTouched() const { return kPlanePoints; }
bool UsdGenDeformOp::Capture(UsdGenCaptureContext const&, UsdGenCurveBuffer const&,
                            UsdGenCapture*, UsdGenDiagnostics* diagnostics) {
    if (diagnostics) diagnostics->Error("UsdGenDeform cannot run through the host scheduler");
    return false;
}
void UsdGenDeformOp::Evaluate(UsdGenEvalContext const&, UsdGenCapture const&,
                             UsdGenChunkView*) const {
    // The host scheduler cannot reach evaluation after Capture rejects it.
}
} // namespace usdGen
