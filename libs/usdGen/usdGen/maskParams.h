// usdGen engine — shared parameter-partition helpers (M1).
//
// C1-frozen parameter names (docs/freezes/C1.md; 02-schema.md §2.5/§2.13/§6).
//
// 03 §2.3's coarse partition, used for S14's "union == mapped property set"
// assertion:
//   TopologyParameters() = capture/topology/structural-class edits (everything
//     that can change what capture produced or the graph shape).
//   ValueParameters()    = per-frame edits (chunk re-evaluate only).
// usdGen:enabled is NOT in either set: the router routes it by type
// (02 §6.2 vs §6.3 — topology for generators/Length, value-toggle for the rest).
#ifndef USDGEN_MASK_PARAMS_H
#define USDGEN_MASK_PARAMS_H

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

inline TfTokenVector UsdGenBaseTopologyParams()
{
    return { TfToken("input"), TfToken("space"), TfToken("readPhase"),
             TfToken("surface"), TfToken("algorithmVersion"), TfToken("seed") };
}

inline TfTokenVector UsdGenBaseValueParams()
{
    return { TfToken("blend") };
}

/// The UsdGenMaskAPI block (02 §2.13), auto-applied to every operator.
inline TfTokenVector UsdGenMaskTopologyParams()
{
    return {
        // graph-structural (02 §6.1):
        TfToken("mask:source"), TfToken("mask:combine"), TfToken("mask:rangeMode"),
        // capture-class (02 §6.4):
        TfToken("mask:random"), TfToken("mask:randomSeed"),
        TfToken("mask:noise:amount"), TfToken("mask:noise:frequency"),
        TfToken("mask:noise:gain"), TfToken("mask:noise:bias"),
        TfToken("mask:noise:seed"), TfToken("mask:region"),
    };
}

inline TfTokenVector UsdGenMaskValueParams()
{
    return {
        TfToken("mask:amount"), TfToken("mask:invert"), TfToken("mask:range"),
        TfToken("mask:ramp:knots"), TfToken("mask:ramp:interpolation"),
        TfToken("mask:ramp:spline"), TfToken("mask:rangeMin"),
        TfToken("mask:rangeMax"), TfToken("mask:effectPosition"),
        TfToken("mask:falloff"), TfToken("mask:influenceWidth"),
    };
}

}  // namespace usdGen

#endif  // USDGEN_MASK_PARAMS_H
