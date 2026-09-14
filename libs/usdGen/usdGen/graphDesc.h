// usdGen engine — UsdGenGraphDesc, the engine's pure-value input (ADR §4.2.3,
// 03-execution-engine.md §2.2). Built by usdGenImaging from Hydra data sources;
// the engine never sees a UsdStage, a scene index or an SdfLayer (S8).
#ifndef USDGEN_GRAPH_DESC_H
#define USDGEN_GRAPH_DESC_H

#include "usdGen/types.h"
#include "usdGen/expressions/context.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/path.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

class UsdGenImagePayload;

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
    // Keep the original descriptor values stable. These values are carried
    // by some host-side graph snapshots, so adding portable backend names
    // must not renumber either established backend or Invalid.
    CpuReference = 0,
    Cuda = 1,
    Invalid = 2,
    Metal = 3,
    Vulkan = 4
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

/// The authored relationship slot which consumes a map.  A path alone is not
/// sufficient: mask:source and length:source can intentionally name the same
/// map while retaining different evaluation semantics.  Generic is the
/// compatibility spelling for the historical usdGen:map relationship and
/// direct descriptor clients which only populate UsdGenNodeDesc::maps.
enum class UsdGenMapBindingPurpose : uint8_t {
    Generic,
    MaskSource,
    LengthSource
};

struct UsdGenMapBindingDesc
{
    SdfPath                  map;
    UsdGenMapBindingPurpose  purpose = UsdGenMapBindingPurpose::Generic;
    // Exact authored relationship name, retained for diagnostics. Empty is
    // valid only for direct/legacy descriptors; consumers which require a
    // semantic role must reject Generic rather than guessing from the path.
    TfToken                  relationship;
};

inline bool
UsdGenMapBindingHasSemanticPurpose(UsdGenMapBindingDesc const &binding)
{
    return binding.purpose != UsdGenMapBindingPurpose::Generic;
}

/// A named plane authored on a C3 curve set. Descriptor-only transport keeps
/// the source path backend-neutral: Point becomes vertex, Primitive uniform,
/// and Groom one constant value in the CPU CurveSource buffer.
enum class UsdGenAuthoredPlaneType : uint8_t { Float32, Int32 };
enum class UsdGenAuthoredPlaneDomain : uint8_t { Point, Primitive, Groom };

struct UsdGenAuthoredPlaneDesc
{
    TfToken name;
    UsdGenAuthoredPlaneType type = UsdGenAuthoredPlaneType::Float32;
    UsdGenAuthoredPlaneDomain domain = UsdGenAuthoredPlaneDomain::Point;
    uint8_t arity = 1;                 // [1, 16]
    VtFloatArray floatValues;          // populated only for Float32
    VtIntArray intValues;              // populated only for Int32
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
    // Legacy untyped map paths.  Keep this populated by builders and accept
    // it from old direct clients; mapBindings is the canonical typed form.
    SdfPathVector                maps;
    std::vector<UsdGenMapBindingDesc> mapBindings;
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
    GfMatrix4d      worldMatrix{1.0}; // source object space must be explicit for deformation
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
    std::vector<UsdGenAuthoredPlaneDesc> authoredPlanes;
    uint64_t        curveGeneration = 0;   // bumped by any points/topology/id change on the prim
};

/// A surface geometry sample at an absolute time (R23).
struct UsdGenSurfaceSample { double time; VtVec3fArray points; };

// The Default-time mesh-normal interpolation transported by UsdGenRestAPI.
// Invalid is deliberately distinct from None: a nonempty malformed snapshot
// must not be silently converted into geometric-normal fallback by CUDA.
enum class UsdGenSurfaceNormalDomain : uint8_t {
    None,
    Constant,
    Uniform,
    Vertex,
    FaceVarying,
    Invalid
};

struct UsdGenSurfaceDesc
{
    SdfPath        path;
    UsdGenSurfaceId id = 0;
    VtIntArray     faceVertexCounts, faceVertexIndices;
    VtVec3fArray   restPoints;        // usdGen/rest/points (S12), UsdTimeCode::Default()
    VtVec3fArray   restNormals;       // usdGen/rest/normals, Default-time Mesh normals
    UsdGenSurfaceNormalDomain restNormalDomain = UsdGenSurfaceNormalDomain::None;
    bool          restFromCurrentPoints = false; // compatibility fallback, never a valid RBF binding
    VtVec3fArray   points;            // deformed, at UsdGenGraphDesc::time
    /// Sorted by time; samples[0].time == UsdGenGraphDesc::time (R23).
    std::vector<UsdGenSurfaceSample> samples;
    VtVec3fArray   velocities;        // motion profile P1 only; empty otherwise
    VtVec2fArray   uv;                // the surface's primary uv set
    VtIntArray     subsetFaces;       // empty == whole mesh; a GeomSubset restricts scatter (R15)
    GfMatrix4d     worldMatrix{1.0};  // post-flattening, resetXformStack (S4)
    uint64_t       surfaceGeneration = 0; // bumped by any points/topology change
};

struct UsdGenMapDesc
{
    SdfPath      path;
    TfToken      type;                // UsdGenImageMap | UsdGenPtexMap | UsdGenExprMap | ...
    std::string  resolvedAssetPath;   // stage-free (S13)
    uint64_t     textureGeneration = 0;  // bumped by ReloadMaps()
    std::vector<UsdGenParamValue> params;
    // Optional stage-free decoded pixels for a typed ImageMap consumer. The
    // immutable payload is backend-neutral; callers replacing it must also
    // advance textureGeneration, which is the deterministic cache identity.
    std::shared_ptr<const UsdGenImagePayload> imagePayload;
};

/// Compiler-resolved map value.  It contains no stage or backend handle: the
/// identity is a deterministic digest of the fully resolved descriptor value
/// and can therefore be retained safely by a CPU capture (or rejected by a
/// backend which has no transport for it).
struct UsdGenResolvedMapValue
{
    SdfPath     path;
    TfToken     type;
    std::string resolvedAssetPath;
    uint64_t    textureGeneration = 0;
    uint64_t    identity = 0;
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
    GfMatrix4d                     xformMatrix{1.0}; // description world matrix (post-flattening, S4)
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
    double   timeCodesPerSecond = 24.0; // USD default; expressions expose seconds
    UsdGenExecutionBackend executionBackend = UsdGenExecutionBackend::CpuReference;
    std::vector<std::string> validationErrors;
};

}  // namespace usdGen

#endif  // USDGEN_GRAPH_DESC_H
