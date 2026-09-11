// usdGen imaging — graph description builder (06-imaging.md §2.2/§3.10;
// 03-execution-engine.md §2.5, the S14 pull-everything rule).
//
// Builds the pure-value usdGen::UsdGenGraphDesc from Hydra data sources
// (production path, 13 §7 V2-7–V2-11) or, fenced to T0/offline/tests, from
// a UsdStage — so the engine never touches a UsdStage (S8; link-enforced
// by B-1, staging-fenced by B-2).
#ifndef USDGEN_IMAGING_GRAPH_DESC_BUILDER_H
#define USDGEN_IMAGING_GRAPH_DESC_BUILDER_H

#include "usdGen/graphDesc.h"

#include "pxr/pxr.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"  // stage path only (BuildGraphDescFromStage)

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
///
/// Hydra-sourced staging (production path, V2-9 homing table): operator
/// discovery follows input path-array data sources back from the
/// terminal target; parameters are read from flat relative-locator sampled
/// data sources (the adapter overlays its mapped source at the prim root,
/// so usdGen:terminal -> `terminal`, usdGen:motion:mode -> motion/mode)
/// with S14 pull-all preserved (every mapped locator pulled at least once
/// per topology generation so dependencies register; stock subtrees sharing
/// the overlaid root are pruned from the sweep). Sibling of
/// BuildGraphDescFromStage below; SI-12 asserts both builders agree exactly
/// before the stage path is unplugged.
usdGen::UsdGenGraphDesc BuildGraphDescFromHydra(
    HdSceneIndexBase &input,
    SdfPath const &descriptionPath,
    UsdGenGraphDescBuildOptions const &options = UsdGenGraphDescBuildOptions());

/// Stage-sourced staging, fenced to T0/offline/tests (unit tests, fixture
/// authoring) per V2-11; the production path is BuildGraphDescFromHydra.
/// Carries the V2-9a absence semantics by hand: Hydra data sources carry
/// only authored opinions, so stage reads whose fallback differs from the
/// desired absent-value must gate on HasAuthoredValueOpinion() (notably
/// `purpose`, whose "default" fallback is render-tag poison).
usdGen::UsdGenGraphDesc BuildGraphDescFromStage(
    UsdStageRefPtr const &stage,
    SdfPath const &descriptionPath,
    UsdGenGraphDescBuildOptions const &options = UsdGenGraphDescBuildOptions());

}  // namespace usdGenImaging

#endif  // USDGEN_IMAGING_GRAPH_DESC_BUILDER_H
