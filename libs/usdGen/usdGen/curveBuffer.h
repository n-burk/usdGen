// usdGen engine — curve buffers, chunk views, tile views, publication boundary.
//
// Plan source: 03-execution-engine.md §1.2/§1.5/§6.3; tile contract C2:
// docs/freezes/C2.md. I1: per-CV planar SoA, per-curve AoS.
#ifndef USDGEN_CURVE_BUFFER_H
#define USDGEN_CURVE_BUFFER_H

#include "usdGen/types.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/sdf/path.h"

#include <cstdint>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// One named extra plane (03 §1.2). R24 pins type/arity per emitted name:
///   clumpId_<level> : uniform int,       arity 1
///   guideIndex      : uniform int[],     arity 3
///   guideWeight     : uniform float[],   arity 3
/// `int[3]`/`float[3]` are not USD type names. Exactly one of `f`/`i` holds
/// the payload.
struct UsdGenPlane
{
    TfToken      name;
    TfToken      interpolation;   // vertex | uniform | constant
    TfToken      type;            // float | int
    uint8_t      arity = 1;
    VtFloatArray f;
    VtIntArray   i;
};

/// One chunk (03 §1.2). Surface-major, then Morton order over rest roots (§1.7).
struct UsdGenChunkDesc
{
    uint32_t        firstCurve = 0;   // index into the per-curve arrays
    uint32_t        curveCount = 0;   // allocated curves, <= chunkSize
    uint32_t        liveCount  = 0;   // curves surviving density decimation; <= curveCount
    uint32_t        firstCv    = 0;   // index into the per-CV arrays
    uint32_t        cvCount    = 0;   // CVs per curve; 0 == ragged chunk (use cvOffsets)
    UsdGenTileId    tile      = 0;
    UsdGenSurfaceId surface   = 0;
    GfRange3f       boundsRest;       // filled at capture; brush/footprint rejection, tile extents
};

/// One node's output. Per-CV data planar (I1); per-curve data AoS.
/// Handoff to imaging is by value (VtArray copy-on-write refcount bump, R20).
struct UsdGenCurveBuffer
{
    // ---- per CV ----------------------------------------------------------------
    VtFloatArray px, py, pz;          // size == totalCvs
    VtFloatArray width;               // empty == inherit upstream
    VtFloatArray hairT;               // root->tip parameter, written once by the generator
    std::vector<UsdGenPlane> extraCv; // named per-CV planes, sorted by name

    // ---- per curve ---------------------------------------------------------------
    // primvars:usdGen:curveId (uniform uint64[], R12):
    //   UsdGenHash64(seed, faceIndex, k, kSaltScatter), see usdGenMath hash.h.
    // NOTE: OpenUSD 26.08 ships no VtUInt64Array alias; the type is
    //       VtArray<uint64_t> (VT_INTEGRAL_BUILTIN_VALUE_TYPES, pxr/base/vt/types.h:56).
    VtArray<uint64_t> curveId;
    VtIntArray   rootPrim;            // surface face index (== primvars:skinprim)
    VtVec2fArray rootUV;              // == primvars:skinprimuv, and the "st" primvar
    VtVec3fArray rootT, rootN, rootB; // rest root frame, 36 B/curve
    VtIntArray   cvOffsets;           // size totalCurves+1; present iff any chunk is ragged
    VtFloatArray curveMask;           // resolved per-curve mask for the owning node
    std::vector<UsdGenPlane> extraCurve;  // clumpId_<level>, guideIndex, guideWeight

    // ---- geometry ------------------------------------------------------------------
    std::vector<UsdGenChunkDesc> chunks; // surface-major, then Morton order
    uint32_t totalCurves = 0, totalCvs = 0;
    uint64_t topologyVersion = 0;        // bumped when curve or CV counts change
    uint64_t valueVersion    = 0;        // bumped by every write to this buffer
};

/// What a kernel is allowed to touch. All pointers are chunk-local bases
/// (03 §1.2). cvCount == 0 on the ragged path.
struct UsdGenChunkView
{
    const UsdGenChunkDesc *desc;
    float       *px, *py, *pz;               // out: writable
    const float *inPx, *inPy, *inPz, *inWidth;  // in: read-only, never const_cast
    float       *width;                      // out when the node touches kPlaneWidths (scheduler binds fillOut); otherwise aliases upstream
    float       *hairT;                      // out when the node touches kPlaneHairT; otherwise aliases upstream
    const uint64_t *curveId;
    const int   *rootPrim;
    const int   *cvOffsets;                  // null on the uniform fast path
    const GfVec2f *rootUV;
    const GfVec3f *rootT, *rootN, *rootB;
    const float *curveMask;                  // resolved once at capture; may be null == 1.0
    const float *rampLut;                    // 257 entries over hairT, or null
    // Extra planes this node declared in OutputPrimvars(), bound by slot at compile.
    // Per-curve slots have curveCount * arity entries; per-CV slots curveCount * cvCount.
    float       **outF;                      // writable float planes, null when the node emits none
    int         **outI;                      // writable int planes
    const float **inF;                       // upstream planes this node reads
    const int   **inI;
    uint32_t      outCount, inCount;
    uint32_t curveCount, cvCount;            // cvCount == 0 on the ragged path
    uint32_t inCvCount = 0;                  // upstream's CVs per curve (in* base + c*inCvCount + i)
    inline size_t Cv(uint32_t c, uint32_t i) const { return size_t(c) * cvCount + i; }
    inline size_t CvRagged(uint32_t c, uint32_t i) const { return size_t(cvOffsets[c]) + i; }
    inline float *OutF(uint32_t slot) const { return outF[slot]; }
    inline int   *OutI(uint32_t slot) const { return outI[slot]; }
    inline const float *InF(uint32_t slot) const { return inF[slot]; }
    inline const int   *InI(uint32_t slot) const { return inI[slot]; }
};

/// A read-only window over one tile's chunk range, used by the interleaver (03 §6.3).
struct UsdGenTileView
{
    UsdGenTileId tile;
    uint32_t     firstChunk, chunkCount;
    uint32_t     totalLiveCurves, totalLiveCvs;
    GfRange3f    extent;                      // written by the interleave pass
    // Engine-internal tile-change flags, set by the interleave pass and
    // consumed by the session's generation builder (not part of C2).
    bool pointsDirty = false;
    bool widthsDirty = false;
};

/// One reference set per reference node, per generation (03 §1.5, I3).
struct UsdGenReferenceSet
{
    UsdGenCurveBuffer buffer;                 // evaluated in full, never chunk-dirty
    VtFloatArray      localX, localY, localZ; // root-local offsets, refreshed per frame
    VtFloatArray      guideBlend;             // per-guide usdGen:blend, XGen range of influence
    uint64_t          generation = 0;
};

// ---------------------------------------------------------------------------
// Imaging boundary types (contract C2, docs/freezes/C2.md; 03 §9.1: "the engine
// only fills them"). One UsdGenTilePublication per published tile prim
// <description>/__usdGenRender/tile_NNNN.
// ---------------------------------------------------------------------------

struct UsdGenTilePublication
{
    UsdGenTileId  tile = 0;
    SdfPath       primPath;                   // <description>/__usdGenRender/tile_NNNN (4-digit zero pad)

    // --- topology (C2) ---
    VtIntArray    curveVertexCounts;          // basisCurves/topology/curveVertexCounts
    std::string   basis = "bspline";          // basisCurves/topology/basis (usdGen:curve:basis)
    // Contract constants, never authorable: type="cubic", wrap="pinned".
    int           refineLevel = 2;            // displayStyle/refineLevel (S-9: no M1 tumble tier)

    // --- exact-size primvar arrays (C2): points.size() == sum(curveVertexCounts) (SI-1)
    VtVec3fArray  points;                     // primvars/points, vertex
    VtFloatArray  widths;                     // primvars/widths, vertex or constant; never varying
    VtFloatArray  hairT;                      // primvars/hairT, vertex, root 0 -> tip 1
    VtFloatArray  hairId;                     // primvars/hairId, uniform float [0,1)
    VtVec2fArray  st;                         // primvars/st, uniform (root UV)
    VtVec3fArray  displayColor;               // uniform (perCurve bake) or vertex (perCV)
    VtVec3fArray  bakeColor;                  // optional primvars:<bakePrimvar> (bakeTarget=primvar)
    // Operator-emitted planes (C2): clumpId_<level> uniform int; guideIndex/guideWeight
    // uniform with elementSize 3.
    std::vector<UsdGenPlane> extraUniform;

    // Motion profile P1 only (usdGen:motion:mode = "velocities"); empty otherwise,
    // and the primvar is BLOCKED (HdBlockDataSource) in P0/P2.
    VtVec3fArray  velocities;

    // --- per-tile data (recomputed every deforming frame; C2 extent rule) ---
    GfVec3d       extentMin, extentMax;       // extent/min, extent/max

    // --- hand-authored per tile, post-flattening (ADR §5.3) ---
    GfMatrix4d    xformMatrix;                // xform/matrix, surface world matrix
    // xform/resetXformStack = true (constant), never a payload.
    TfToken       purpose;                    // inherited by hand from the description
    TfToken       visibility;                 // inherited by hand from the description
    SdfPath       materialPath;               // description's Material
    TfToken       materialPurpose = TfToken("allPurpose"); // the empty-token container child
    SdfPath       primOrigin;                 // pickTarget-dependent; absolute outside prototypes
    SdfPath       dependencySurface;          // __dependencies: dependedOnPrimPath
};

/// M6-only; empty in M1 (UsdGenInstance publishes instancers instead of tiles).
struct UsdGenInstancerPublication
{
    SdfPath       primPath;                   // <description>/__usdGenRender/inst_<opName>
    std::vector<SdfPath> prototypes;          // instancerTopology/prototypes (namespace children)
    std::vector<VtIntArray> instanceIndices;  // instancerTopology/instanceIndices, one per prototype
    VtBoolArray   mask;                       // instancerTopology/mask; empty == all true
};

/// Structural signature for the generation diff (03 §6.1): what changed in the
/// published prim set, independent of payload values.
struct UsdGenPrimSetSignature
{
    int           tileCount = 0;
    int           instancerCount = 0;
    std::vector<std::string> primPaths;       // sorted
    // per prim path (parallel to primPaths): sorted list of published primvar names
    std::vector<std::vector<std::string>> primvarNames;
    // per prim path (parallel to primPaths): prim type string
    std::vector<std::string> primTypes;

    bool operator==(UsdGenPrimSetSignature const &rhs) const;
};

/// Diff result consumed by the imaging notice emitter (06 §5.1 rules).
struct UsdGenTileDirty
{
    UsdGenTileId tile = 0;
    SdfPath      primPath;
    bool         removed = false;            // -> PrimsRemoved
    bool         added   = false;            // -> PrimsAdded
    bool         pointsDirty = false;        // -> primvars/points/primvarValue + extent/min|max
    bool         widthsDirty = false;        // -> primvars/widths/primvarValue
    bool         xformDirty  = false;        // -> xform/matrix
    std::vector<TfToken> newPrimvars;        // first appearance -> primvars/<name> once
    std::vector<TfToken> dirtyPrimvars;      // -> primvars/<name>/primvarValue (never with points)
};

struct UsdGenDirtyReport
{
    std::vector<UsdGenTileDirty> tiles;
    bool surfaceXformDirty = false;         // description-level xform moved
};

}  // namespace usdGen

#endif  // USDGEN_CURVE_BUFFER_H
