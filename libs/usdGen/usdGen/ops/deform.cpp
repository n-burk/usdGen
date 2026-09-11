// Deform's actual RBF execution lives in the persistent CUDA graph plan.
// The operator registry supplies metadata; no synthetic CPU displacement is
// a valid substitute for rest-bound animation.
#include "usdGen/ops/deform.h"
#include "usdGen/maskParams.h"

namespace usdGen {

bool UsdGenDeformOp::Bind(UsdGenParamView const& params, UsdGenDiagnostics* diagnostics) {
    if (!params.desc || params.desc->executionBackend != UsdGenExecutionBackend::Cuda) {
        if (diagnostics) diagnostics->Error("UsdGenDeform requires the CUDA RBF executor; CPU reference deformation is unavailable");
        return false;
    }
    if (!params.node || params.node->mode != TfToken("rbf")) {
        if (diagnostics) diagnostics->Error("UsdGenDeform currently requires mode=rbf");
        return false;
    }
    return true;
}

TfSpan<const TfToken> UsdGenDeformOp::TopologyParameters() const {
    static TfTokenVector values = [] {
        auto result = UsdGenBaseTopologyParams();
        result.push_back(TfToken("rbfSamples"));
        result.push_back(TfToken("twistAware"));
        auto mask = UsdGenMaskTopologyParams();
        result.insert(result.end(), mask.begin(), mask.end());
        return result;
    }();
    return TfSpan<const TfToken>(values.data(), values.size());
}
TfSpan<const TfToken> UsdGenDeformOp::ValueParameters() const {
    static TfTokenVector values = [] {
        auto result = UsdGenBaseValueParams();
        result.push_back(TfToken("enabled"));
        result.push_back(TfToken("lockRoots"));
        result.push_back(TfToken("preserveShape"));
        result.push_back(TfToken("preserveShape:iterations"));
        auto mask = UsdGenMaskValueParams();
        result.insert(result.end(), mask.begin(), mask.end());
        return result;
    }();
    return TfSpan<const TfToken>(values.data(), values.size());
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
