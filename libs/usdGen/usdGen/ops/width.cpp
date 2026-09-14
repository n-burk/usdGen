// usdGen — UsdGenWidthOp implementation (M1). 02-schema.md §2.7.1, 04 §2.11.
// Authors `widths` from a base width, a root->tip ramp (usdGen:width:knots,
// S11) and taper; usdGen:replace selects set (true) vs multiply (false).
// TopologyEffect = None.
#include "usdGen/ops/width.h"

#include "usdGen/imagePayload.h"
#include "usdGen/mask.h"
#include "usdGen/maskParams.h"
#include "usdGenMath/usdGenMath/kernels.h"
#include "usdGenMath/usdGenMath/ramp.h"

#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

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

UsdGenParamValue const *FindMapParam(UsdGenMapDesc const &map,
                                     char const *name)
{
    TfToken const token(name);
    for (UsdGenParamValue const &param : map.params)
        if (param.name == token) return &param;
    return nullptr;
}

bool ReadMapFloat(UsdGenMapDesc const &map, char const *name, float fallback,
                  float *out, std::string *error)
{
    if (UsdGenParamValue const *param = FindMapParam(map, name)) {
        if (param->value.IsHolding<float>()) {
            *out = param->value.UncheckedGet<float>();
            return std::isfinite(*out);
        }
        if (param->value.IsHolding<double>()) {
            *out = static_cast<float>(param->value.UncheckedGet<double>());
            return std::isfinite(*out);
        }
        if (error) *error = std::string("map parameter '") + name +
            "' must be a finite float";
        return false;
    }
    *out = fallback;
    return true;
}

bool ReadMapToken(UsdGenMapDesc const &map, char const *name,
                  TfToken const &fallback, TfToken *out, std::string *error)
{
    if (UsdGenParamValue const *param = FindMapParam(map, name)) {
        if (param->value.IsHolding<TfToken>()) {
            *out = param->value.UncheckedGet<TfToken>();
            return true;
        }
        if (param->value.IsHolding<std::string>()) {
            *out = TfToken(param->value.UncheckedGet<std::string>());
            return true;
        }
        if (error) *error = std::string("map parameter '") + name +
            "' must be a token";
        return false;
    }
    *out = fallback;
    return true;
}

bool ReadImageMapOptions(UsdGenMapDesc const &map,
                         UsdGenImageSampleOptions *options,
                         std::string *error)
{
    TfToken domain, uvSet, channel, wrap, filter, colorSpace;
    if (!ReadMapToken(map, "map:domain", TfToken("root"), &domain, error) ||
        !ReadMapToken(map, "map:uvSet", TfToken("st"), &uvSet, error) ||
        !ReadMapToken(map, "map:channel", TfToken("r"), &channel, error) ||
        !ReadMapToken(map, "map:wrap", TfToken("clamp"), &wrap, error) ||
        !ReadMapToken(map, "map:filter", TfToken("bilinear"), &filter, error) ||
        !ReadMapToken(map, "map:colorSpace", TfToken("raw"), &colorSpace, error))
        return false;
    if (domain != TfToken("root")) {
        if (error) *error = "ImageMap domain must be 'root' for Width";
        return false;
    }
    if (uvSet != TfToken("st")) {
        if (error) *error = "ImageMap uvSet must be 'st' for Width";
        return false;
    }
    // The image payload already contains the scalar values consumed by Width.
    // Non-raw decoding would require a color transform and is intentionally
    // outside this bounded typed path.
    if (colorSpace != TfToken("raw")) {
        if (error) *error = "ImageMap colorSpace must be 'raw' for Width";
        return false;
    }
    if (channel == TfToken("r")) options->channel = UsdGenImageChannel::R;
    else if (channel == TfToken("g")) options->channel = UsdGenImageChannel::G;
    else if (channel == TfToken("b")) options->channel = UsdGenImageChannel::B;
    else if (channel == TfToken("a")) options->channel = UsdGenImageChannel::A;
    else if (channel == TfToken("luminance")) options->channel = UsdGenImageChannel::Luminance;
    else {
        if (error) *error = "ImageMap channel must be a scalar channel for Width";
        return false;
    }
    if (filter == TfToken("nearest")) options->filter = UsdGenImageFilter::Nearest;
    else if (filter == TfToken("bilinear")) options->filter = UsdGenImageFilter::Bilinear;
    else {
        if (error) *error = "ImageMap filter must be 'nearest' or 'bilinear'";
        return false;
    }
    if (wrap == TfToken("clamp")) options->wrap = UsdGenImageWrap::Clamp;
    else if (wrap == TfToken("repeat")) options->wrap = UsdGenImageWrap::Repeat;
    else if (wrap == TfToken("mirror")) options->wrap = UsdGenImageWrap::Mirror;
    else if (wrap == TfToken("black")) options->wrap = UsdGenImageWrap::Black;
    else {
        if (error) *error = "ImageMap wrap token is unsupported";
        return false;
    }
    if (!ReadMapFloat(map, "map:scale", 1.0f, &options->scale, error) ||
        !ReadMapFloat(map, "map:offset", 0.0f, &options->offset, error) ||
        !ReadMapFloat(map, "map:default", 0.0f, &options->defaultValue, error))
        return false;
    if (UsdGenParamValue const *param = FindMapParam(map, "map:clamp")) {
        if (!param->value.IsHolding<GfVec2f>()) {
            if (error) *error = "map parameter 'map:clamp' must be float2";
            return false;
        }
        GfVec2f const range = param->value.UncheckedGet<GfVec2f>();
        if (!std::isfinite(range[0]) || !std::isfinite(range[1])) {
            if (error) *error = "map parameter 'map:clamp' must be finite";
            return false;
        }
        options->outputMin = range[0];
        options->outputMax = range[1];
        options->clampOutput = range[0] != range[1];
    }
    return true;
}

bool ApplyImageMask(UsdGenCaptureContext const &ctx,
                    UsdGenCurveBuffer const &upstream,
                    UsdGenCapturePayload *capture,
                    UsdGenDiagnostics *diag)
{
    if (!ctx.mapBindingCount) return true;
    auto fail = [&](std::string const &message) {
        if (diag) diag->Error("UsdGenWidth: " + message);
        return false;
    };
    if (!ctx.mapBindings || ctx.mapBindingCount != 1 ||
        ctx.mapBindings[0].purpose != UsdGenMapBindingPurpose::MaskSource ||
        (!ctx.mapBindings[0].relationship.IsEmpty() &&
         ctx.mapBindings[0].relationship != TfToken("usdGen:mask:source")))
        return fail("requires exactly one typed usdGen:mask:source binding");
    if (!ctx.maps || ctx.mapCount != 1 || !ctx.maps[0])
        return fail("mask source has no resolved map value");
    if (!ctx.desc) return fail("mask source has no graph descriptor");
    UsdGenMapDesc const *map = nullptr;
    for (UsdGenMapDesc const &candidate : ctx.desc->maps) {
        if (candidate.path == ctx.maps[0]->path) { map = &candidate; break; }
    }
    if (!map || map->type != TfToken("UsdGenImageMap"))
        return fail("mask source must target a UsdGenImageMap");
    if (!map->imagePayload || !map->imagePayload->IsValid())
        return fail("UsdGenImageMap has no decoded immutable image payload");
    UsdGenImageSampleOptions options;
    std::string error;
    if (!ReadImageMapOptions(*map, &options, &error)) return fail(error);
    if (!ValidateUsdGenImageSampleOptions(*map->imagePayload, options, &error))
        return fail("invalid ImageMap sample options: " + error);
    size_t const curves = upstream.totalCurves
        ? upstream.totalCurves : upstream.rootUV.size();
    if (upstream.rootUV.size() != curves)
        return fail("ImageMap mask source requires one root st UV per curve (got " +
            std::to_string(upstream.rootUV.size()) + " for " +
            std::to_string(curves) + " curves)");
    if (capture->curveMask.size() != curves)
        capture->curveMask = VtFloatArray(curves, 1.0f);
    float const amount = ctx.params
        ? static_cast<float>(ctx.params->GetDouble(TfToken("mask:amount"), 1.0))
        : 1.0f;
    for (size_t c = 0; c != curves; ++c) {
        float const sampled = std::clamp(UsdGenImageSampler::Sample(
            *map->imagePayload, upstream.rootUV[c][0], upstream.rootUV[c][1], options),
            0.0f, 1.0f);
        capture->curveMask[c] *= std::clamp(amount * sampled, 0.0f, 1.0f);
    }
    return true;
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
    return ApplyImageMask(ctx, upstream, &cap, diag);
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
    float defaultInputWidth = ctx.desc ? ctx.desc->defaultWidth : 0.01f;
    if (!std::isfinite(defaultInputWidth) || defaultInputWidth < 0.0f)
        defaultInputWidth = 0.01f;
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
    const float taperSpan = (taperStart < 1.0f) ? 1.0f - taperStart : 1.0f;
    for (uint32_t c = 0; c < view->curveCount; ++c) {
        size_t const cv = view->cvCount ? size_t(view->cvCount)
            : (view->cvOffsets ? size_t(view->cvOffsets[c + 1] - view->cvOffsets[c]) : 0);
        size_t const first = view->cvCount ? size_t(c) * view->cvCount
            : (view->cvOffsets ? size_t(view->cvOffsets[c]) -
                 (view->desc ? view->desc->firstCv : 0) : 0);
        const float m = mask ? mask[c] : 1.0f;
        for (size_t i = 0; i < cv; ++i) {
            const size_t o = first + i;
            const float t = hairT ? hairT[o] : (cv > 1 ? float(i) / float(cv - 1) : 0.0f);
            // Interpolated ramp sampling — never a nearest LUT bin (04 §2.17).
            // Flat ramp (no knots) is exactly 1.0: skip the call (E-1).
            // `target` is the unmasked width profile.  The mask is the
            // operator-local envelope, not a multiplier on the target: set
            // and multiply have different identity values (04 §2.11).
            float target = width * (magFlat ? 1.0f : UsdGenEvalLut257(lutPtr, t));
            // Taper: linear fall-off from taperStart to the tip.
            if (taper > 0.0f && t > taperStart)
                target *= 1.0f - taper * (t - taperStart) / taperSpan;
            // Root/tip scale, linear in hairT.
            target *= rootScale + (tipScale - rootScale) * t;

            // The scheduler applies usdGen:blend as the framework envelope
            // after this kernel.  Apply only this capture's resolved mask
            // here, including its along-curve ramp.  This keeps blend from
            // being applied twice and makes a zero mask a true pass-through.
            const float envelope = m *
                (maskLut ? UsdGenEvalLut257(maskLut, t) : 1.0f);
            // A style input is always defined.  Grow normally materializes
            // one, but a generic source may not; use the description default
            // in that case so the same local mask envelope still applies.
            const float input = inWidth ? inWidth[o] : defaultInputWidth;
            float const w = replace
                ? input + (target - input) * envelope
                : input * (1.0f + (target - 1.0f) * envelope);
            view->width[o] = std::max(0.0f, w);
        }
    }
}

}  // namespace usdGen
