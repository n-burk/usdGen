// usdGenPomade — gizmo and brush overlay records (plan/18 §2.4).
//
// The manipulator the artist drags is geometry like everything else the tool
// draws: the model holds a record of what should be on screen, the scene
// index turns it into the `gizmo` and `brushRing` basisCurves under
// /__usdGenPomade/, and Hydra draws it with the unlit overlay material. No Qt
// paint pass, no separate overlay renderer, and — because it is a prim — the
// gizmo is depth-sorted against the tubes it manipulates.
//
// An interactive usdview session clears this depth-sorted fallback and draws
// the same handles in its transparent Qt viewport overlay, keeping the
// manipulator unoccluded.  Headless sessions continue to use these curves.
//
// The records are set and cleared explicitly (Pomade_SetGizmo /
// Pomade_SetBrushRing): the model never invents a gizmo from the selection,
// because only the tool knows which handle the pointer is over and what the
// current mode manipulates. Handle hit-testing is screen-space in Python
// (pomadeMath.gizmoHit), so nothing here needs a camera.
//
// Geometry, in world units, from one record:
//
//   translate      host-application RGB axis arrows, one planar square for each
//                  two-axis move, and a cyan centre square for free
//                  camera-plane moves.  The active handle is yellow.
//   ringTRS        a circle in the frame's uv plane (handle 0) plus the
//                  three axes (handles 1..3): the Ring sub-mode's scale
//                  ring and twist handle
//   nodeTranslate  3 short axes (a third of the length): the Graph-mode
//                  node drag, which must not swamp the head it sits on
//
// The frame is COLUMN-major in the plan's sense: entries 0..2 are the u
// axis, 3..5 the v axis, 6..8 the w axis. They are used as given (never
// re-orthonormalised): a ring gizmo has to lie in the ring's own plane,
// including the twist the artist authored.
#ifndef USDGEN_POMADE_GIZMO_H
#define USDGEN_POMADE_GIZMO_H

#include "usdGenPomade/api.h"

#include <vector>

namespace usdGenPomade {

enum PomadeGizmoKind : int {
    PomadeGizmo_None = 0,
    PomadeGizmo_Translate = 1,
    PomadeGizmo_RingTRS = 2,
    PomadeGizmo_NodeTranslate = 3,
    PomadeGizmo_Rotate = 4,
    PomadeGizmo_Scale = 5,
};

// PomadeGizmoRecord::allowedMask with every handle allowed.
constexpr unsigned int PomadeGizmoAllHandles = 0xFFFFFFFFu;

struct USDGENPOMADE_API PomadeGizmoRecord {
    int kind = PomadeGizmo_None;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    // u, v, w axes (3 floats each). The identity basis is the default so a
    // caller that only has a position still gets a usable gizmo.
    float frame[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float sizeWorld = 1.0f;
    int activeHandle = -1;  // -1 = none; else the handle id below
    // Bit (1 << handleId) set = that handle may be drawn.  The tool hides
    // what a sub-mode cannot drive (a section chart has no tangent move),
    // and the headless fallback must hide the same handles the Qt overlay
    // does (GZ-06), so the tool's whitelist rides on the record.  All bits
    // set is "every handle this kind has".
    unsigned int allowedMask = PomadeGizmoAllHandles;

    bool operator==(PomadeGizmoRecord const &o) const;
    bool operator!=(PomadeGizmoRecord const &o) const { return !(*this == o); }
};

struct USDGENPOMADE_API PomadeBrushRingRecord {
    bool active = false;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float radiusWorld = 0.0f;

    bool operator==(PomadeBrushRingRecord const &o) const;
    bool operator!=(PomadeBrushRingRecord const &o) const
    {
        return !(*this == o);
    }
};

// A flat basisCurves payload: linear curves, one colour / handle id /
// active flag per curve. Deliberately free of pxr types so the T1 test can
// assert on the geometry without Hydra, and so this header can be included
// from the C ABI.
struct USDGENPOMADE_API PomadeOverlayCurves {
    std::vector<float> points;      // 3 per CV, curve-major
    std::vector<int> vertexCounts;  // CVs per curve
    std::vector<float> colors;      // 3 per curve
    std::vector<float> widths;      // 1 per curve
    std::vector<int> handleIds;     // 1 per curve
    std::vector<int> active;        // 1 per curve (0/1)

    int CurveCount() const { return int(vertexCounts.size()); }
    void Clear();
};

// Handle ids, stable across kinds so the Python hit-test and the published
// `handleId` primvar agree: 0..2 are the u/v/w axes of a translate gizmo and
// of a node gizmo; a ringTRS numbers its circle 3 and keeps 0..2 for the
// axes, so "handle 0 is the u axis" holds for every kind.  The centre is 4;
// planar handles name their normal axis, so Scale can constrain its two
// affected axes.  Rotate reserves 8/9 for the camera-view ring and free
// trackball ball.
enum PomadeGizmoHandle : int {
    PomadeGizmoHandle_AxisU = 0,
    PomadeGizmoHandle_AxisV = 1,
    PomadeGizmoHandle_AxisW = 2,
    PomadeGizmoHandle_Ring = 3,
    PomadeGizmoHandle_Center = 4,
    PomadeGizmoHandle_PlaneYZ = 5,
    PomadeGizmoHandle_PlaneXZ = 6,
    PomadeGizmoHandle_PlaneXY = 7,
    PomadeGizmoHandle_View = 8,
    PomadeGizmoHandle_Free = 9,
};

// Number of segments a gizmo/brush circle is drawn with. Fixed, so the
// published topology only changes when the gizmo kind changes.
int USDGENPOMADE_API PomadeGizmoCircleSegments();

// Build the curves for one record. False (and an empty payload) for kind
// none, a non-finite record or a non-positive size — "no gizmo" is a state,
// not an error, and the caller publishes nothing.  Curves whose handle id
// is not in `record.allowedMask` are left out; a mask that leaves nothing
// is "no gizmo" too.
bool USDGENPOMADE_API PomadeBuildGizmoCurves(PomadeGizmoRecord const &record,
                                           PomadeOverlayCurves *out);

// The brush ring: one closed circle of radius `radiusWorld` around
// `center`, in the plane `normal` is the normal of. False for an inactive
// record, a degenerate normal or a non-positive radius.
bool USDGENPOMADE_API PomadeBuildBrushRingCurves(
    PomadeBrushRingRecord const &record, PomadeOverlayCurves *out);

}  // namespace usdGenPomade

#endif  // USDGEN_POMADE_GIZMO_H
