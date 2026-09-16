// usdGen engine — shared parameter-partition helpers (M1).
//
// C1-frozen parameter names (docs/freezes/C1.md; 02-schema.md §2.5/§6).
//
// 03 §2.3's coarse partition, used for S14's "union == mapped property set"
// assertion:
//   TopologyParameters() = capture/topology/structural-class edits (everything
//     that can change what capture produced or the graph shape).
//   ValueParameters()    = per-frame edits (chunk re-evaluate only).
// usdGen:enabled is NOT in either set: the router routes it by type
// (02 §6.2 vs §6.3 — topology for generators/Length, value-toggle for the rest).
//
// usdGen:mask is declared per operator, in ValueParameters(): it is the
// operator envelope and a literal edit re-evaluates without recapturing.
// Generators declare no mask.
#ifndef USDGEN_OP_PARAMS_H
#define USDGEN_OP_PARAMS_H

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

inline TfTokenVector UsdGenBaseTopologyParams()
{
    return { TfToken("input"), TfToken("surface"), TfToken("seed") };
}

inline TfTokenVector UsdGenBaseValueParams()
{
    return {};
}

}  // namespace usdGen

#endif  // USDGEN_OP_PARAMS_H
