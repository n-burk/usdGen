// usdGen engine — mask evaluator implementation (M1).
//
// 02-schema.md §2.13 is canonical for the arithmetic; 04-operators.md §5.2
// is the executable spelling and the combine truth table. M1 evaluates ONLY
// the constant source term (s = 1.0) and the salted `usdGen:mask:random`
// term (plan/11-roadmap.md M1 scope: "only the constant and mask:random
// terms evaluate before M4"). The range remap, invert, amount, combine mode,
// locked-curve suppression, along-curve ramp, noise, region and map-source
// terms are declared in the settings but NEUTRAL in M1: a non-default value
// is ignored with a diagnostic, never silently honoured.
#include "usdGen/mask.h"
#include "usdGen/op.h"

#include "usdGenMath/usdGenMath/hash.h"

#include "pxr/pxr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

constexpr int kRampLutSize = 257;  // 257 entries keep the j+1 lerp read in range

// Combine per the 04 §5.2 truth table (staged for M4: M1's neutral defaults
// are combine=multiply, amount=1, so the combine step reduces to r).
[[maybe_unused]] float CombineMask(TfToken const &t, float m, float r)
{
    if (t == TfToken("add"))       return std::clamp(m + r, 0.0f, 1.0f);
    if (t == TfToken("subtract"))  return std::clamp(m - r, 0.0f, 1.0f);
    if (t == TfToken("max"))       return std::max(m, r);
    if (t == TfToken("min"))       return std::min(m, r);
    if (t == TfToken("average"))   return 0.5f * (m + r);
    if (t == TfToken("replace"))   return r;
    return m * r;  // "multiply" (default) and any unknown token
}

// The ramp LUT (Houdini shortcut band and knot ramps of 02 §2.17) is built
// here once the ramp term lands: with no ramp term M1 publishes a flat
// 1.0 LUT, so `MaskWeight` below reduces to blend * curveMask. The builder
// is staged for M4 (map/region terms) and the S11 spline evaluator.
[[maybe_unused]] void BuildRampLUT(
    UsdGenMaskSettings const &s, VtVec2fArray const &knots, float *lut)
{
    if (knots.size() < 2) {
        // Houdini shortcut: flat 1.0 across [rangeMin, rangeMax] with shaped
        // shoulders of width influenceWidth*2 centred on effectPosition*span.
        const float lo = s.rangeMin, hi = s.rangeMax;
        const float span = std::max(hi - lo, 1e-6f);
        const float halfBand = 0.5f * span;
        const float center = lo + s.effectPosition * span;
        const float shoulder = std::max(s.influenceWidth, 1e-6f) * span * s.falloff;
        for (int i = 0; i < kRampLutSize; ++i) {
            const float u = i / 256.0f;  // root->tip parameter
            const float x = lo + u * span;
            const float d = x - center;
            lut[i] = (std::fabs(d) <= halfBand - shoulder) ? 1.0f
                   : (std::fabs(d) <= halfBand + shoulder)
                       ? 1.0f - (std::fabs(d) - (halfBand - shoulder)) / (2.0f * shoulder + 1e-6f)
                       : 0.0f;
            lut[i] = std::clamp(lut[i], 0.0f, 1.0f);
        }
        return;
    }

    // Knot ramp.
    if (s.rampInterpolation == TfToken("constant")) {
        for (int i = 0; i < kRampLutSize; ++i) {
            const float u = i / 256.0f;
            float v = knots[0][1];
            for (size_t k = 1; k < knots.size(); ++k) {
                if (u >= knots[k][0]) v = knots[k][1]; else break;
            }
            lut[i] = v;
        }
        return;
    }
    // "linear" (and, until the S11 evaluator lands, catmullRom/bspline too):
    // piecewise-linear through the knots, clamped at both ends.
    // TODO(S11 evaluator): 02 §2.17 uniform Catmull-Rom / cubic B-spline.
    for (int i = 0; i < kRampLutSize; ++i) {
        const float u = i / 256.0f;
        if (u <= knots[0][0]) { lut[i] = knots[0][1]; continue; }
        float v = knots.back()[1];
        for (size_t k = 1; k < knots.size(); ++k) {
            if (u <= knots[k][0]) {
                const float x0 = knots[k-1][0], y0 = knots[k-1][1];
                const float x1 = knots[k][0],   y1 = knots[k][1];
                v = (x1 - x0 > 1e-6f) ? y0 + (y1 - y0) * ((u - x0) / (x1 - x0)) : y1;
                break;
            }
        }
        lut[i] = v;
    }
}
}  // namespace

UsdGenMaskResult EvaluateMask(
    UsdGenMaskSettings const &settings,
    VtArray<uint64_t> const &curveIds,
    VtFloatArray const &restRoots,
    VtFloatArray const &lockedCurve,
    double time)
{
    UsdGenMaskResult result;
    TF_UNUSED(restRoots);
    TF_UNUSED(lockedCurve);
    TF_UNUSED(time);

    // M1 formula (04 §5.2 restricted per plan/11-roadmap.md M1):
    //   s = 1.0                                   (constant source term)
    //   r = lerp(1, Draw01(seed, curveId, kSaltMaskRandom), random)
    //   curveMask(c) = clamp(combine(multiply, 1.0 * s, r), 0, 1) = r
    // The other terms stay neutral until M4/M3 — see the file header.
    const size_t n = curveIds.size();
    result.curveMask = VtFloatArray(n);
    if (n) {
        auto const *ids = curveIds.data();
        auto *mask = result.curveMask.data();
        const float random = settings.random;
        const int randomSeed = settings.randomSeed;
        for (size_t c = 0; c < n; ++c) {
            mask[c] = random <= 0.0f
                ? 1.0f
                : 1.0f - random
                    * (1.0f - UsdGenDraw01(randomSeed, ids[c], kSaltMaskRandom));
        }
    }

    // Neutral ramp: w(c,i) = blend * curveMask(c) * 1.
    result.rampLut = VtFloatArray(kRampLutSize, 1.0f);

    // Unsupported authored terms are ignored, never silently (review M-6):
    // one short diagnostic per term, at most.
    auto &diag = result.diagnostics;
    if (!settings.mapSource.empty())
        diag.push_back("usdGen:mask:source is evaluated from M4; ignored in M1");
    if (settings.rangeX != 0.0f || settings.rangeY != 1.0f
        || settings.rangeMode != TfToken("normalized"))
        diag.push_back("usdGen:mask:range* remap is evaluated from M4; ignored in M1");
    if (settings.invert)
        diag.push_back("usdGen:mask:invert is evaluated from M4; ignored in M1");
    if (settings.amount != 1.0f)
        diag.push_back("usdGen:mask:amount is neutral (1.0) in M1");
    if (settings.combine != TfToken("multiply"))
        diag.push_back("usdGen:mask:combine is 'multiply' in M1");
    if (settings.noiseAmount != 0.0f)
        diag.push_back("usdGen:mask:noise is evaluated in M3; ignored in M1");
    if (!settings.region.empty())
        diag.push_back("usdGen:mask:region is evaluated from M4; ignored in M1");
    if (!settings.rampKnots.empty() || settings.rangeMin != 0.0f
        || settings.rangeMax != 1.0f || settings.effectPosition != 0.5f
        || settings.falloff != 0.5f || settings.influenceWidth != 0.5f)
        diag.push_back("the mask ramp term is evaluated from M4; a flat 1.0 ramp is used in M1");
    if (!lockedCurve.empty())
        diag.push_back("usdGen:mask:lockedCurve suppression is neutral in M1");
    return result;
}
UsdGenMaskSettings UsdGenMaskSettingsFromParams(UsdGenParamView const &params)
{
    UsdGenMaskSettings m;
    m.random = static_cast<float>(
        params.GetDouble(TfToken("mask:random"), 0.0));
    m.randomSeed = params.GetInt(TfToken("mask:randomSeed"), 0);
    m.combine = params.GetToken(TfToken("mask:combine"), TfToken("multiply"));
    m.amount = static_cast<float>(params.GetDouble(TfToken("mask:amount"), 1.0));
    m.invert = params.GetBool(TfToken("mask:invert"), false);
    m.rangeX = static_cast<float>(params.GetDouble(TfToken("mask:rangeMin"), 0.0));
    m.rangeY = static_cast<float>(params.GetDouble(TfToken("mask:rangeMax"), 1.0));
    m.rangeMode = params.GetToken(TfToken("mask:rangeMode"), TfToken("normalized"));
    m.effectPosition = static_cast<float>(
        params.GetDouble(TfToken("mask:effectPosition"), 0.5));
    m.falloff = static_cast<float>(params.GetDouble(TfToken("mask:falloff"), 0.5));
    m.influenceWidth = static_cast<float>(
        params.GetDouble(TfToken("mask:influenceWidth"), 0.5));
    if (VtValue const v = params.GetVtValue(TfToken("mask:ramp:knots"), VtValue());
        v.IsHolding<VtVec2fArray>()) {
        m.rampKnots = v.UncheckedGet<VtVec2fArray>();
    }
    m.rampInterpolation =
        params.GetToken(TfToken("mask:ramp:interpolation"), TfToken("catmullRom"));
    // M4 terms (map source, noise, region) stay neutral until then.
    return m;
}

float MaskWeight(float blend, float curveMask, UsdGenMaskResult const &ramp, float hairT)
{
    const float t = std::min(hairT, 1.0f) * 256.0f;
    const int j = int(std::min(t, 255.0f));
    const float a = t - float(j);
    const float rampWeight =
        (1.0f - a) * ramp.rampLut[j] + a * ramp.rampLut[j + 1];
    return blend * curveMask * rampWeight;
}


}  // namespace usdGen
