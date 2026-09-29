// usdGenPomade — per-level publication staging (plan/18 §2.2).
//
// Before V0 the index published exactly one mesh (the P0 test tube) and the
// hierarchy in PomadeModel::_tubes had no publication path at all (plan/18 §0
// F2). This TU turns the whole model into one prim family per hierarchy
// level — few fat prims, plan/17 R2 — and hands the scene index Hydra-ready
// arrays:
//
//   tubes/L<n>      one mesh holding every tube at level n (K5 tessellation)
//   centers/L<n>    one linear basisCurves per tube, through its center CVs
//   centerCVs/L<n>  the center CVs as a points prim
//   rings/L<n>      the cross-section rings, closed linear curves
//   ringCVs/L<n>    the ring CVs as a points prim
//   guides/L<n>     the K9/K10 guide preview of the tubes at that level
//
// Two rules make the live drag cheap:
//
//   * per-tube slices. Each level keeps a host mirror plus one slice record
//     per tube (point/face/curve ranges and a content hash). A publish
//     re-tessellates exactly the tubes whose hash changed and copies the
//     rest, so moving one CV of one tube in a 200-tube groom costs one
//     tessellation, not 200.
//   * grow-only buffers. The pinned block tube 0's device mirror lands in,
//     and the per-level host mirrors, only ever grow, so a gesture never
//     allocates after its first frame.
//
// Ring geometry is read back out of the staged surface grid rather than
// re-evaluated: a ring curve is the grid row the section sits on, so the
// rings an artist drags can never drift from the surface they bound.
#ifndef USDGEN_POMADE_PUBLISH_H
#define USDGEN_POMADE_PUBLISH_H

#include "usdGenPomade/api.h"
#include "usdGenPomade/pomadeModel.h"
#include "usdGenPomade/pomadeTransport.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenPomade {

// Where one tube lives inside its level's buffers.
struct USDGENPOMADE_API PomadeTubeSlice {
    int tubeId = 0;
    int level = 1;
    int regionId = -1;
    int childIndex = -1;
    // Hash of everything the tessellation depends on. Equal hashes mean the
    // staged slice is still correct and is copied, not recomputed.
    uint64_t contentHash = 0;
    int ringCount = 0;   // grid rows
    int ringVerts = 0;   // verts per row
    int ringStride = 1;  // grid rows per authored section (display segments)
    int pointOffset = 0;  // in vertices, into the level's point array
    int pointCount = 0;
    int faceOffset = 0;   // in faces
    int faceCount = 0;
    int centerOffset = 0;  // in vertices, into the level's center arrays
    int centerCount = 0;
    int ringCurveOffset = 0;  // in curves, into the level's ring arrays
    int ringCurveCount = 0;
    GfVec3f clumpColor = GfVec3f(0.6f);
    float radius = 0.5f;  // mean root-ring radius; sets overlay widths
};

// One level's Hydra-ready arrays.
struct USDGENPOMADE_API PomadeStagedLevel {
    int level = 0;
    std::vector<PomadeTubeSlice> tubes;

    // tubes/L<n> (mesh). tubeId/clumpColor/selected are uniform: Hydra
    // uniform means one value per FACE, so they are expanded to the real
    // face count rather than published as a single-element array (which is
    // what Storm rejects on a multi-face mesh, and why the P3 index
    // hard-coded `constant` instead).
    VtVec3fArray points;
    VtVec3fArray normals;
    VtIntArray faceVertexCounts;
    VtIntArray faceVertexIndices;
    VtIntArray faceTubeId;
    VtVec3fArray faceClumpColor;
    VtIntArray faceSelected;
    GfVec3f extentMin = GfVec3f(0.0f);
    GfVec3f extentMax = GfVec3f(0.0f);

    // centers/L<n> (linear basisCurves, one per tube) + centerCVs/L<n>.
    VtVec3fArray centerPoints;
    VtIntArray centerVertexCounts;
    VtIntArray centerIndices;
    VtVec3fArray centerCurveColor;  // uniform (per curve)
    VtFloatArray centerCurveWidth;  // uniform
    VtIntArray centerCurveTubeId;   // uniform
    VtIntArray centerCurveSelected; // uniform
    VtVec3fArray centerCVColor;     // vertex
    VtFloatArray centerCVWidth;     // vertex
    VtIntArray centerCVTubeId;      // vertex
    VtIntArray centerCVIndex;       // vertex (CV index within its tube)
    GfVec3f centerMin = GfVec3f(0.0f);
    GfVec3f centerMax = GfVec3f(0.0f);

    // rings/L<n> (closed linear basisCurves) + ringCVs/L<n>.
    VtVec3fArray ringPoints;         // ringVerts + 1 per curve (closed)
    VtIntArray ringVertexCounts;
    VtIntArray ringIndices;
    VtVec3fArray ringCurveColor;     // uniform
    VtFloatArray ringCurveWidth;     // uniform
    VtIntArray ringCurveTubeId;      // uniform
    VtIntArray ringCurveSection;     // uniform (section index within its tube)
    VtIntArray ringCurveSelected;    // uniform (0 none / 1 selected / 2 hover)
    VtVec3fArray ringCVPoints;       // ringVerts per ring (no repeat)
    VtVec3fArray ringCVColor;
    VtFloatArray ringCVWidth;
    VtIntArray ringCVTubeId;
    VtIntArray ringCVSection;
    GfVec3f ringMin = GfVec3f(0.0f);
    GfVec3f ringMax = GfVec3f(0.0f);

    // guides/L<n> (cubic bspline basisCurves, the committed Guides spelling).
    VtVec3fArray guidePoints;
    VtIntArray guideVertexCounts;
    VtIntArray guideIndices;
    VtFloatArray guideWidths;
    VtFloatArray guideHairT;
    VtVec3fArray guideCurveColor;  // uniform, = the owning tube's clump colour
    VtIntArray guideCurveTubeId;   // uniform
    GfVec3f guideMin = GfVec3f(0.0f);
    GfVec3f guideMax = GfVec3f(0.0f);
    int guideCount = 0;

    // Display state (plan/18 §2.2): visibility, x-ray and focus never
    // re-tessellate anything.
    bool visible = true;
    bool xray = false;
    // The alpha the level's mesh draws with while `xray` is set. It is the
    // ABSOLUTE opacity, published straight onto the mesh as the `xray`
    // primvar, and 0 means "not x-ray" (the shader then uses the
    // material's own opacity). Per level, because plan/18 §2.4a asks for
    // the focused level at 25 % with the rest fainter behind it.
    float xrayOpacity = 0.0f;
    // Whether the center curves and CV dots draw (plan/18 §2.4a: Graph and
    // Output show opaque tubes only).
    bool centers = true;
    // The active Tube component's point glyphs. Curves remain available as
    // orientation guides while inactive control dots are hidden.
    bool centerCVDots = true;
    bool ringCVDots = true;
    bool focused = false;
    // World units per screen pixel (PomadeModel::SetDisplayScale), copied
    // in so the width arrays are a pure function of the staged level. 0
    // selects the radius-relative fallback.
    float displayScale = 0.0f;
    // Centers-only (plan/18 section 3.7, the ladder's fourth step): the
    // level publishes its center curves and CV dots and hides everything
    // else -- mesh, rings, guides -- so a heavy level outside the edited
    // subtree costs a polyline instead of a tessellation.
    bool centersOnly = false;
    // The Pomade guide preview hides while the usdGen cook's amplified tiles
    // are the thing on screen (plan/17 §3.2, plan/18 §7 G7).
    bool guidesVisible = true;

    // Layout signature: tube ids, grid sizes and curve counts. A change
    // means topology, not just points.
    uint64_t topologyHash = 0;
    // Combined content hash of every tube in the level.
    uint64_t pointsHash = 0;
    // Combined hash of the per-face/per-curve identity primvars (tube id
    // and clump colour). Separate from pointsHash so a move dirties points
    // alone and a re-colour dirties the uniforms alone.
    uint64_t uniformHash = 0;
    // Which tubes of this level publish rings and ring CV dots (plan/18
    // §2.4a). Its own hash, deliberately: ring visibility follows the
    // selection and the active sub-mode, and neither may dirty the level's
    // mesh, centers or points -- only the two ring prims.
    uint64_t ringHash = 0;
    // Hash of the selection state that touches THIS level (plan/18 §2.3).
    // Its own hash, deliberately: a click has to dirty the `selected`
    // primvar and nothing else, and it must not restage a single vertex.
    uint64_t selectionHash = 0;
};

// Everything the index publishes for one model.
struct USDGENPOMADE_API PomadeStagedModel {
    std::map<int, PomadeStagedLevel> levels;  // by level, 1-based
    // Tubes re-tessellated by the Stage call that produced this snapshot.
    int restagedTubeCount = 0;
    // Whether the usdGen cook's amplified tiles (the prims the groom scene
    // index publishes under <description>/__usdGenRender/) draw. The Pomade
    // index authors visibility = false over them when this is false: during
    // a gesture, and whenever "show amplified hair" is off (plan/17 §3.2).
    bool amplifiedTilesVisible = false;
};

// The staging owner. One per scene index: it keeps the host mirrors and the
// pinned block alive across publishes so a gesture never reallocates.
class USDGENPOMADE_API PomadePublisher {
public:
    PomadePublisher();
    ~PomadePublisher();
    PomadePublisher(PomadePublisher const &) = delete;
    PomadePublisher &operator=(PomadePublisher const &) = delete;

    // Stage `model` into a fresh snapshot, reusing the slices of every tube
    // whose content is unchanged. Returns false (leaving the snapshot as it
    // was) when the model holds no tube.
    bool Stage(PomadeModel const &model);
    // Drop the snapshot. The caller MUST do this when it switches to a
    // different model: slice reuse is keyed on tube id, and two models
    // share tube ids without sharing geometry.
    void Clear();

    PomadeStagedModel const &Current() const { return _current; }

private:
    struct LevelMirror {
        std::vector<float> positions;  // 3 per vertex, level-wide
        std::vector<float> normals;
    };
    PomadeStagedModel _current;
    std::map<int, LevelMirror> _mirrors;
    PomadePinnedStaging _pinnedPositions;
    PomadePinnedStaging _pinnedNormals;
};

// The plan/18 §2.4a pixel targets for the overlay geometry. Exposed so the
// T1 test states the same numbers the publisher uses, and so the whole size
// contract reads in one place.
//
// Storm takes `widths` in WORLD units, for points and curves alike, so a
// screen-constant dot is `pixels * worldPerPixel at its depth`. That scale
// is PomadeModel::GetDisplayScale(); before V8 there was none and every
// overlay was sized against its tube's root radius, which is why a
// one-unit-thick tube drew half-unit CV blobs.
struct USDGENPOMADE_API PomadeOverlayPixels {
    // Center curves: the focused level's are the thick control curves the
    // 2018 stills show, descendants stay hairlines.
    static constexpr float kCenterCurveFocused = 3.0f;
    static constexpr float kCenterCurveOther = 1.0f;
    // Center CV dots: "8 px focused / 5 px else".
    static constexpr float kCenterCVFocused = 8.0f;
    static constexpr float kCenterCVOther = 5.0f;
    // Cross-section rings and their CV dots.
    static constexpr float kRingCurve = 1.0f;
    static constexpr float kRingCV = 5.0f;
    // Guide preview strands, and the scalp graph's node dots.
    static constexpr float kGuide = 1.5f;
    static constexpr float kGraphNode = 6.0f;
};

// The world-space width of one overlay element: `pixels` screen pixels at
// `displayScale` world units per pixel. With no display scale (0 — a
// headless model, or a harness that never resolved a camera) it answers
// `fallbackWidth`, which every call site fills with the radius-relative
// value V0–V7 used, so "no camera draws what it always drew" is a fact a
// test can check rather than a comment.
float USDGENPOMADE_API PomadeOverlayWidth(float pixels, float displayScale,
                                        float fallbackWidth);

// The content hash one tube's staged geometry depends on. Exposed for the
// T1 test, which asserts that a move changes exactly one tube's hash.
uint64_t USDGENPOMADE_API PomadeTubeContentHash(PomadeTubeDesc const &desc,
                                             int displaySegments,
                                             PomadeTubeShape const &shape,
                                             bool isPrimaryTube);

// The static test tube, straight from the shared pomadeTessellate.h formula.
// No model is involved: this is the record-harness convenience the index
// publishes while no model is active (plan/18 §2.1), and it must not
// resurrect the model the index used to own.
void USDGENPOMADE_API PomadeStageTestTubeMesh(PomadeTubeShape const &shape,
                                            PomadeStagedTubeMesh *out);

} // namespace usdGenPomade

#endif // USDGEN_POMADE_PUBLISH_H
