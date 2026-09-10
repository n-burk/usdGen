// usdGen engine — mask evaluator (02-schema.md §2.13 formula, R16).
//
// The mask arithmetic is canonical here; 04-operators.md §5.2 must be
// executable spelling and the combine truth table. M1 evaluates ONLY the
// CONSTANT + RANDOM terms (plan/11-roadmap.md M1); the along-curve ramp, the
// map-fed source, the noise term and the region constraint evaluate at
// M4 (L-3/L-4/L-5) and return neutral (1.0) until then. The resolved
// per-curve mask and the 257-entry ramp LUT are computed ONCE per capture
// and cached in the owning node's UsdGenCapture (I4: capture never runs per
// frame).
#ifndef USDGEN_MASK_H
#define USDGEN_MASK_H

#include "usdGen/types.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/vt/array.h"

#include <cstdint>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// C1 mask block parameters, resolved by the graph desc builder
/// (02-schema.md §2.13; docs/freezes/C1.md).
struct UsdGenMaskSettings
{
    // source term (M4; M1 treats as s = 1.0)
    std::string mapSource;              // usdGen:mask:source target; empty == none
    TfToken mapType;                    // UsdGenMap subtype

    // range remap: remap(s,x,y) = clamp((s-x)/max(y-x,1e-6), 0, 1)  (load-bearing guard)
    float rangeX = 0.0f;                // usdGen:mask:rangeMin / range.x
    float rangeY = 1.0f;                // usdGen:mask:rangeMax / range.y
    TfToken rangeMode = TfToken("normalized");  // normalized | absoluteLength

    bool invert = false;               // usdGen:mask:invert (after range remap)

    // random term (M1): r = lerp(1, Draw01(randomSeed, curveId, kSaltMaskRandom), random)
    float random = 0.0f;
    int randomSeed = 0;

    // combine: multiply | add | subtract | max | min | average | replace
    TfToken combine = TfToken("multiply");
    float amount = 1.0f;               // usdGen:mask:amount

    // noise term (M4; M1 neutral n = 1.0)
    float noiseAmount = 0.0f;
    float noiseFrequency = 1.0f;
    float noiseGain = 0.5f;
    float noiseBias = 0.5f;
    int noiseSeed = 0;

    // region constraint (M3/M4; M1 neutral region = 1.0)
    std::string region;                // usdGen:mask:region target; empty == none

    // along-curve ramp (M4): 257-entry LUT over hairT; neutral (flat 1.0) in M1
    TfToken rampInterpolation = TfToken("catmullRom");  // linear | catmullRom | bspline | constant
    VtVec2fArray rampKnots;           // usdGen:mask:ramp:knots, (position, value), sorted by x

    // Houdini shortcut ramp (M4, used when fewer than two ramp knots are authored):
    // usdGen:mask:rangeMin/rangeMax/effectPosition/falloff/influenceWidth
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;
    float effectPosition = 0.5f;
    float falloff = 0.5f;
    float influenceWidth = 0.5f;
};

struct UsdGenMaskResult
{
    /// Per-curve resolved mask, clamped to [0,1]
    ///   curveMask(c) = clamp( combine(amount*s, r) * n * region(c) * locked(c), 0, 1 )
    VtFloatArray curveMask;
    /// 257-entry LUT over hairT: rampLUT[i] = ramp(i/256); the 257th entry
    /// keeps the lerp read (j+1) in range (02 §2.13).
    VtFloatArray rampLut;
    std::vector<std::string> diagnostics;
};

/// The full 02-schema.md §2.13 formula. M1 evaluates the constant + random
/// terms only; range/invert/amount/combine, ramp, noise and region are neutral.
/// `lockedCurve` (UsdGenSculptLayer usdGen:sculpt:lockedCurves suppression) is
/// passed in as a per-curve factor, 1.0 when absent.
UsdGenMaskResult EvaluateMask(
    UsdGenMaskSettings const &settings,
    VtArray<uint64_t> const &curveIds,
    VtFloatArray const &restRoots,      // for the M4 noise term; may be empty in M1
    VtFloatArray const &lockedCurve,    // per-curve; empty == all 1.0
    double time);

/// w(c, i) = usdGen:blend * curveMask(c) * rampWeight — the final per-CV weight.
/// The framework's envelope pass (03 §8.5) applies usdGen:blend; kernels
/// multiply only curveMask(c) * rampWeight. Helper kept here for parity tests.
float MaskWeight(float blend, float curveMask, UsdGenMaskResult const &ramp, float hairT);

}  // namespace usdGen

#endif  // USDGEN_MASK_H
