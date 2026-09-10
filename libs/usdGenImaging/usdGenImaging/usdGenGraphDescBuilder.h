// usdGen imaging — graph description builder (06-imaging.md §2.2/§3.10;
// 03-execution-engine.md §2.5, the S14 pull-everything rule).
//
// Builds the pure-value usdGen::UsdGenGraphDesc from the scene index / stage
// so the engine never touches a UsdStage (S8; enforced at link time by B-1).
#ifndef USDGEN_IMAGING_GRAPH_DESC_BUILDER_H
#define USDGEN_IMAGING_GRAPH_DESC_BUILDER_H

#include "usdGen/graphDesc.h"

#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

struct UsdGenGraphDescBuildOptions
{
    /// Sample time for deformed surface points / C3 curve sets.
    double time = 0.0;
};

/// Walk one UsdGenDescription:
///  - usdGen:terminal resolves the single terminal operator (more than one
///    target is a compile error naming both paths, 02 §2.3);
///  - operator prims are discovered by following usdGen:input edges in
///    reverse from the terminal; namespace order is preserved for the
///    compiler's Kahn tie-break (S26);
///  - EVERY mapped usdGen:* property of every node is pulled at least once
///    per topology generation (S14) so time-varying and mode-irrelevant
///    parameters still register dependencies;
///  - usdGen:surface targets accept Mesh and GeomSubset prims (ADR §9 R15);
///    a GeomSubset yields UsdGenSurfaceDesc::subsetFaces in PARENT-mesh face
///    indices (02 §2.20 rules 1-2);
///  - usdGen:guides / usdGen:curves / usdGen:frozen:curves yield
///    UsdGenCurveSetDesc entries (rest points at UsdTimeCode::Default(),
///    S12 via the UsdGenRestAPI adapter where applied).
///
/// Fill-in policy: C1 schema defaults (docs/freezes/C1.md) are materialized
/// for missing properties so the engine's param partition (S14 union rule)
/// sees a complete set.
usdGen::UsdGenGraphDesc BuildGraphDesc(
    UsdStageRefPtr const &stage,
    SdfPath const &descriptionPath,
    UsdGenGraphDescBuildOptions const &options = UsdGenGraphDescBuildOptions());

}  // namespace usdGenImaging

#endif  // USDGEN_IMAGING_GRAPH_DESC_BUILDER_H
