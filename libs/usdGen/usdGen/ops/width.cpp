// usdGen — UsdGenWidthOp implementation (M1). 02-schema.md §2.7.1, 04 §2.11.
// Authors `widths` from a base width, a root->tip ramp (usdGen:width:knots,
// S11) and taper; usdGen:replace selects set (true) vs multiply (false).
// TopologyEffect = None.
#include "usdGen/ops/width.h"

#include "usdGen/mask.h"
#include "usdGen/maskParams.h"
#include "usdGenMath/usdGenMath/kernels.h"
#include "usdGenMath/usdGenMath/ramp.h"

#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

/// Read a float2[] ramp-knots property (02 §2.17). Missing or empty -> {}
/// (UsdGenBuildRampLut maps {} to the flat constant ramp).
VtVec2fArray ReadRampKnots(UsdGenParamView const *p, TfToken const &name)
{
    if (!p) return {};
    if (VtValue const v = p->GetVtValue(name, VtValue());
        v.IsHolding<VtVec2fArray>()) {
        return v.UncheckedGet<VtVec2fArray>();
    }
    return {};
}

struct UsdGenWidthCapture final : public UsdGenCapturePayload
{
    float width = 0.01f;
    float taper = 0.0f;
    float taperStart = 0.5f;
    float rootScale = 1.0f;
    float tipScale = 1.0f;
    bool replace = true;

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenWidthCapture>(*this);
    }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        // Width never changes topology; hairT must exist (generator written it).
        return !upstream.hairT.empty();
    }
};

}  // namespace

static TfTokenVector _topoParams = [] {
    TfTokenVector v = UsdGenBaseTopologyParams();
    v.push_back(TfToken("replace"));       // §6.1 structural: set vs multiply changes
                                          // nothing about topology, but it is a
                                          // capture-class authoring choice
    auto m = UsdGenMaskTopologyParams();
    v.insert(v.end(), m.begin(), m.end());
    return v;
}();

static TfTokenVector _valueParams = [] {
    TfTokenVector v = UsdGenBaseValueParams();
    v.push_back(TfToken("enabled"));       // toggle: value-class (02 §6.3)
    v.push_back(TfToken("width"));
    v.push_back(TfToken("width:knots"));
    // C1 property is usdGen:width:interpolation (schema.usda:369, 02 §2.7.1);
    // the param name is the prefix-stripped form. Value-class: the 257-entry
    // LUT is rebuilt from the live parameters every evaluate (see below).
    v.push_back(TfToken("width:interpolation"));
    v.push_back(TfToken("taper"));
    v.push_back(TfToken("taperStart"));
    v.push_back(TfToken("rootScale"));
    v.push_back(TfToken("tipScale"));
    auto m = UsdGenMaskValueParams();
    v.insert(v.end(), m.begin(), m.end());
    return v;
}();

TfSpan<const TfToken> UsdGenWidthOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenWidthOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
}
std::unique_ptr<UsdGenCapture> UsdGenWidthOp::CreateCapture() const
{
    return std::make_unique<UsdGenWidthCapture>();
}
uint32_t UsdGenWidthOp::PlanesTouched() const
{
    return kPlaneWidths;
}

bool UsdGenWidthOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
    const float taper = static_cast<float>(params.GetDouble(TfToken("taper"), 0.0));
    const float taperStart = static_cast<float>(params.GetDouble(TfToken("taperStart"), 0.5));
    if (taper < 0.0f || taper > 1.0f) {
        if (diag) diag->Error("UsdGenWidth: usdGen:taper must be in [0, 1]");
        return false;
    }
    if (taperStart < 0.0f || taperStart > 1.0f) {
        if (diag) diag->Error("UsdGenWidth: usdGen:taperStart must be in [0, 1]");
        return false;
    }
    if (params.GetDouble(TfToken("rootScale"), 1.0) < 0.0 ||
        params.GetDouble(TfToken("tipScale"), 1.0) < 0.0) {
        if (diag) diag->Error("UsdGenWidth: rootScale/tipScale must be >= 0");
        return false;
    }
    return true;
}

UsdGenEpoch UsdGenWidthOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    // Only capture-class terms: `replace` (structural, 02 §6.1) and the
    // upstream generation. `width:knots` is value-class (02 :665) and is
    // deliberately excluded — its ramp is rebuilt every evaluate (gate SI-2).
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    uint64_t h = 1469598103934665603ULL;
    auto feed = [&](std::string const &k, uint64_t v) {
        h ^= v; h *= 0x100000001b3ULL;
        h ^= k.size(); h *= 0x100000001b3ULL;
    };
    if (p) {
        feed("replace", p->GetBool(TfToken("replace"), true) ? 1 : 0);
    }
    feed("upstream", ctx.upstreamGeneration);
    return {h, h ^ 0x9E3779B97F4A7C15ull};
}

bool UsdGenWidthOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    TF_UNUSED(diag);
    TF_UNUSED(upstream);
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    auto &cap = *static_cast<UsdGenWidthCapture *>(out);

    cap.width = p ? static_cast<float>(p->GetDouble(TfToken("width"), 0.01)) : 0.01f;
    cap.taper = p ? static_cast<float>(p->GetDouble(TfToken("taper"), 0.0)) : 0.0f;
    cap.taperStart = p ? static_cast<float>(p->GetDouble(TfToken("taperStart"), 0.5)) : 0.5f;
    cap.rootScale = p ? static_cast<float>(p->GetDouble(TfToken("rootScale"), 1.0)) : 1.0f;
    cap.tipScale = p ? static_cast<float>(p->GetDouble(TfToken("tipScale"), 1.0)) : 1.0f;
    cap.replace = p ? p->GetBool(TfToken("replace"), true) : true;

    // `width:knots` is value-class (02 :665): the ramp LUT is rebuilt every
    // evaluate from the live parameters (review M-5). Baking it into the
    // capture payload here would freeze a value-class edit out of the frame
    // response, and the digest above deliberately excludes it.

    if (p) {
        UsdGenMaskSettings m = UsdGenMaskSettingsFromParams(*p);
        auto const mask = EvaluateMask(m, upstream.curveId, upstream.px, {}, 0.0);
        cap.curveMask = mask.curveMask;
        // Mask ramp LUT resolved with the block (capture-epoch, I4).
        cap.maskRampLut = mask.rampLut;
    }
    return true;
}

void UsdGenWidthOp::Evaluate(
    UsdGenEvalContext const &ctx,
    UsdGenCapture const &captureIn,
    UsdGenChunkView *view) const
{
    UsdGenWidthCapture const &cap =
        static_cast<UsdGenWidthCapture const &>(captureIn);
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    // Parameter tokens are operator-owned: no per-chunk interning and no
    // function-static destruction racing an asynchronous cook at exit.
    const float width = p ? static_cast<float>(p->GetDouble(sWidth, 0.01)) : 0.01f;
    const float taper = p ? static_cast<float>(p->GetDouble(sTaper, 0.0)) : 0.0f;
    const float taperStart = p ? static_cast<float>(p->GetDouble(sTaperStart, 0.5)) : 0.5f;
    const float rootScale = p ? static_cast<float>(p->GetDouble(sRootScale, 1.0)) : 1.0f;
    const float tipScale = p ? static_cast<float>(p->GetDouble(sTipScale, 1.0)) : 1.0f;
    const bool replace = p ? p->GetBool(sReplace, true) : true;
    auto const *hairT = view->hairT;
    auto const *inWidth = view->inWidth;
    auto const *mask = view->curveMask;
    // Reuse worker-local LUT storage for nonempty knots. Flat ramps use the
    // exact scalar 1.0 below and require no allocated table.
    float const *lutPtr = nullptr;
    VtVec2fArray const widthKnots = ReadRampKnots(p, sKnots);
    bool const magFlat = widthKnots.empty();
    thread_local std::vector<float> tLut;
    if (!magFlat) {
        if (tLut.size() != kUsdGenRampLutSize) tLut.assign(kUsdGenRampLutSize, 1.0f);
        UsdGenBuildRampLut(widthKnots, p ? p->GetToken(sKnotsInterp, sCatmullRom) : sCatmullRom, tLut.data(), kUsdGenRampLutSize);
        lutPtr = tLut.data();
    }
    auto const *maskLut = cap.maskRampLut.empty() ? nullptr : cap.maskRampLut.data();
    const size_t cv = size_t(view->cvCount);

    const float taperSpan = (taperStart < 1.0f) ? 1.0f - taperStart : 1.0f;
    for (uint32_t c = 0; c < view->curveCount; ++c) {
        const float m = mask ? mask[c] : 1.0f;
        for (size_t i = 0; i < cv; ++i) {
            const size_t o = view->Cv(c, i);
            const float t = hairT ? hairT[o] : (cv > 1 ? float(i) / float(cv - 1) : 0.0f);
            // Interpolated ramp sampling — never a nearest LUT bin (04 §2.17).
            // Flat ramp (no knots) is exactly 1.0: skip the call (E-1).
            float w = width * (magFlat ? 1.0f : UsdGenEvalLut257(lutPtr, t))
                      * (maskLut ? UsdGenEvalLut257(maskLut, t) : 1.0f);
            // Taper: linear fall-off from taperStart to the tip.
            if (taper > 0.0f && t > taperStart)
                w *= 1.0f - taper * (t - taperStart) / taperSpan;
            // Root/tip scale, linear in hairT.
            w *= rootScale + (tipScale - rootScale) * t;
            if (!replace && inWidth) w *= inWidth[o];
            view->width[o] = std::max(0.0f, w * m);
        }
    }
}

}  // namespace usdGen
