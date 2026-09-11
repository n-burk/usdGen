// usdGen engine — UsdGenGraphDesc, the engine's pure-value input (ADR §4.2.3,
// 03-execution-engine.md §2.2). Built by usdGenImaging from Hydra data sources;
// the engine never sees a UsdStage, a scene index or an SdfLayer (S8).
#ifndef USDGEN_GRAPH_DESC_H
#define USDGEN_GRAPH_DESC_H

#include "usdGen/types.h"
#include "usdGen/expressions/context.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/path.h"

#include <cstdint>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// One authored usdGen:* property, resolved. `name` is the property name with
/// the `usdGen:` prefix stripped (C1 registry, docs/freezes/C1.md).
struct UsdGenParamValue
{
    TfToken name;                // "clump:size", "mask:ramp:knots", "surface"
    VtValue value;               // scalar, token, array, or resolved asset path
    bool    animated = false;    // authored with a .spline / time samples
};

// Invalid is deliberately distinct from CpuReference: a present but
// malformed/unknown backend request must fail closed instead of silently
// selecting the legacy CPU implementation.  Keep the existing values of the
// two established backends stable for descriptor compatibility.
enum class UsdGenExecutionBackend : uint8_t {
    CpuReference,
    Cuda,
    Invalid
};

struct UsdGenExpressionOutputDesc { TfToken name{"result"}; TfToken nativeType{"float"}; expr::ValueShape shape; };
struct UsdGenExpressionDesc { SdfPath path; std::string source; std::vector<UsdGenExpressionOutputDesc> outputs; };
struct UsdGenExpressionBinding {
    SdfPath expression; TfToken output{"result"}; TfToken nativeType{"float"}; TfToken destination;
    expr::ValueShape destinationShape; expr::Domain domain = expr::Domain::Groom;
    VtValue literal;
};

/// S11 ramp encodings, already resolved by the adapter (02-schema.md §2.17).
struct UsdGenRampDesc
{
    VtVec2fArray knots;          // scalar ramp: (position, value), sorted by x
    VtFloatArray positions;     // colour ramp
    VtVec3fArray colors;        // colour ramp
    TfToken      interpolation; // linear | catmullRom | bspline | constant (default catmullRom, R11)
};

struct UsdGenNodeDesc
{
    SdfPath                      path;         // the operator prim's scene path (identity + Kahn tie-break)
    TfToken                      type;         // "UsdGenClump", "UsdGenScatter", ...
    TfToken                      mode;         // usdGen:mode, when the type has one
    int                          algorithmVersion = 0;   // 0 == "track the newest kernel" (R17)
    bool                         enabled = true;
    int                          seed = 0;
    float                        blend = 1.0f;
    TfToken                      space;        // auto | rest | deformed (auto == the type's Space(), R9)
    TfToken                      readPhase;    // base | preceding | final | @<absolute prim path>
    SdfPathVector                inputs;       // compiler-owned hierarchy dependency edges
    SdfPathVector                references;   // guide sets, clump centres, card roots
    SdfPathVector                curves;       // usdGen:guides / usdGen:curves / usdGen:frozen:curves
                                               //   -> indices into UsdGenGraphDesc::curveSets
    SdfPathVector                surfaces;     // usdGen:surface targets (Mesh or GeomSubset, ADR R15)
    SdfPathVector                maps;         // usdGen:mask:source, per-parameter map targets
    std::vector<UsdGenParamValue> params;      // EVERY mapped locator of this prim (S14 pull-all)
    std::vector<UsdGenExpressionBinding> expressionBindings;
    std::vector<UsdGenRampDesc>   ramps;
};

/// One authored C3 `BasisCurves` input: a UsdGenGuideSet child, a freeze, an
/// import, a sim cache. The only way curve data reaches the engine (R23).
struct UsdGenCurveSetDesc
{
    SdfPath         path;
    UsdGenRole      role;              // Reference (guides, clump centres) | Curves (a CurveSource)
    TfToken         curveRole;         // primvars:usdGen:role: hair | guide (C3 marker)
    VtIntArray      curveVertexCounts;
    VtVec3fArray    points, rest;      // rest == primvars:rest; may share points' buffer
    // Compatibility builders may lack a Default-time C3 snapshot. CUDA
    // admission must not mistake a current-frame fallback for bound rest.
    bool            restFromCurrentPoints = false;
    VtFloatArray    widths;
    TfToken         type{"cubic"};
    TfToken         basis{"bspline"};
    TfToken         wrap{"pinned"};
    TfToken         widthsInterpolation{"vertex"};
    VtIntArray      skinPrim;          // primvars:skinprim (uniform int)
    VtArray<uint64_t> curveId;         // primvars:usdGen:curveId (uniform uint64[], R12)
    VtVec2fArray    skinPrimUv;        // primvars:skinprimuv (uniform texCoord2f, not "st")
    VtMatrix4dArray rootFrame;         // primvars:usdGen:rootFrame; may be empty
    std::string     frozenEpoch;       // constant string primvar, "usdgen1:sha1:..." (S42)
    VtFloatArray    guideBlend;        // per-guide usdGen:blend on UsdGenGuideSet
    uint64_t        curveGeneration = 0;   // bumped by any points/topology/id change on the prim
};

/// A surface geometry sample at an absolute time (R23).
struct UsdGenSurfaceSample { double time; VtVec3fArray points; };

struct UsdGenSurfaceDesc
{
    SdfPath        path;
    UsdGenSurfaceId id = 0;
    VtIntArray     faceVertexCounts, faceVertexIndices;
    VtVec3fArray   restPoints;        // usdGen/rest/points (S12), UsdTimeCode::Default()
    VtVec3fArray   points;            // deformed, at UsdGenGraphDesc::time
    /// Sorted by time; samples[0].time == UsdGenGraphDesc::time (R23).
    std::vector<UsdGenSurfaceSample> samples;
    VtVec3fArray   velocities;        // motion profile P1 only; empty otherwise
    VtVec2fArray   uv;                // the surface's primary uv set
    VtIntArray     subsetFaces;       // empty == whole mesh; a GeomSubset restricts scatter (R15)
    GfMatrix4d     worldMatrix;       // post-flattening, resetXformStack (S4)
    uint64_t       surfaceGeneration = 0; // bumped by any points/topology change
};

struct UsdGenMapDesc
{
    SdfPath      path;
    TfToken      type;                // UsdGenImageMap | UsdGenPtexMap | UsdGenExprMap | ...
    std::string  resolvedAssetPath;   // stage-free (S13)
    uint64_t     textureGeneration = 0;  // bumped by ReloadMaps()
    std::vector<UsdGenParamValue> params;
};

/// The description's look block (02-schema.md §2.12 defaults; 07 §1.2 bake
/// order). M1 bakes the CPU half of the displayColor contract: base colour
/// (white, no maps until M4), the root->tip ramp and the per-curve jitter.
struct UsdGenLookDesc
{
    GfVec3f  rootColor{0.035f, 0.018f, 0.008f};
    GfVec3f  tipColor{0.210f, 0.115f, 0.045f};
    VtVec3fArray rampColors;          // usdGen:look:colorRamp:colors (empty == two-colour mix)
    VtFloatArray rampPositions;       // usdGen:look:colorRamp:positions
    TfToken  rampInterpolation{"catmullRom"};
    float    rampExponent = 1.0f;     // usdGen:look:rampExponent
    TfToken  bakeMode{"perCurve"};    // perCurve | perCV
    TfToken  bakeTarget{"displayColor"};  // displayColor | primvar | none
    TfToken  bakePrimvar{"displayColor"};
    float    hueJitter = 0.0f;
    float    valueJitter = 0.0f;
    int      jitterSeed = 0;
};

struct UsdGenGraphDesc
{
    SdfPath                        description;   // the UsdGenDescription prim
    SdfPath                        terminal;      // hierarchy-derived final operator
    std::vector<UsdGenNodeDesc>    nodes;         // composed execution order from the builder
    std::vector<UsdGenCurveSetDesc> curveSets;    // every C3 BasisCurves the graph names (R23)
    std::vector<UsdGenSurfaceDesc> surfaces;
    std::vector<UsdGenMapDesc>     maps;
    std::vector<UsdGenExpressionDesc> expressions;
    UsdGenLookDesc                 look;
    GfMatrix4d                     xformMatrix;    // description world matrix (post-flattening, S4)
    TfToken                        purpose;        // inherited by hand to every tile (C2)
    TfToken                        visibility;     // inherited by hand to every tile (C2)
    SdfPath                        materialPath;   // the description's bound Material (C2)
    TfToken                        pickTarget{"description"};  // usdGen:pickTarget (C2 primOrigin)
    float    densityScale = 1.0f, renderDensityScale = 1.0f;
    float    defaultWidth = 0.01f;                 // usdGen:width:default
    int      tileTarget = 64;                      // uniform int usdGen:tileTarget
    TfToken  curveBasis{"bspline"};                // usdGen:curve:basis (C2)
    TfToken  motionMode;                           // single | velocities | samples
    int      motionSampleCount = 3;                // clamped [2,16]
    bool     forwardSurfaceSamples = false;
    int      schemaVersion = 1;
    double   time = 0.0;
    UsdGenExecutionBackend executionBackend = UsdGenExecutionBackend::CpuReference;
    std::vector<std::string> validationErrors;
};

}  // namespace usdGen

#endif  // USDGEN_GRAPH_DESC_H
