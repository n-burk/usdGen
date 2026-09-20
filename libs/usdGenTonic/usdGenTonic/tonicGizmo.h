// usdGenTonic — gizmo and brush overlay records (plan/18 §2.4).
//
// The manipulator the artist drags is geometry like everything else the tool
// draws: the model holds a record of what should be on screen, the scene
// index turns it into the `gizmo` and `brushRing` basisCurves under
// /__usdGenTonic/, and Hydra draws it with the unlit overlay material. No Qt
// paint pass, no separate overlay renderer, and — because it is a prim — the
// gizmo is depth-sorted against the tubes it manipulates.
//
// The records are set and cleared explicitly (Tonic_SetGizmo /
// Tonic_SetBrushRing): the model never invents a gizmo from the selection,
// because only the tool knows which handle the pointer is over and what the
// current mode manipulates. Handle hit-testing is screen-space in Python
// (tonicMath.gizmoHit), so nothing here needs a camera.
//
// Geometry, in world units, from one record:
//
//   translate      3 axis lines from the origin, one per frame column,
//                  red / green / blue, the active handle yellow
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
#ifndef USDGEN_TONIC_GIZMO_H
#define USDGEN_TONIC_GIZMO_H

#include "usdGenTonic/api.h"

#include <vector>

namespace usdGenTonic {

enum TonicGizmoKind : int {
    TonicGizmo_None = 0,
    TonicGizmo_Translate = 1,
    TonicGizmo_RingTRS = 2,
    TonicGizmo_NodeTranslate = 3,
};

struct USDGENTONIC_API TonicGizmoRecord {
    int kind = TonicGizmo_None;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    // u, v, w axes (3 floats each). The identity basis is the default so a
    // caller that only has a position still gets a usable gizmo.
    float frame[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float sizeWorld = 1.0f;
    int activeHandle = -1;  // -1 = none; else the handle id below

    bool operator==(TonicGizmoRecord const &o) const;
    bool operator!=(TonicGizmoRecord const &o) const { return !(*this == o); }
};

struct USDGENTONIC_API TonicBrushRingRecord {
    bool active = false;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float radiusWorld = 0.0f;

    bool operator==(TonicBrushRingRecord const &o) const;
    bool operator!=(TonicBrushRingRecord const &o) const
    {
        return !(*this == o);
    }
};

// A flat basisCurves payload: linear curves, one colour / handle id /
// active flag per curve. Deliberately free of pxr types so the T1 test can
// assert on the geometry without Hydra, and so this header can be included
// from the C ABI.
struct USDGENTONIC_API TonicOverlayCurves {
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
// axes, so "handle 0 is the u axis" holds for every kind.
enum TonicGizmoHandle : int {
    TonicGizmoHandle_AxisU = 0,
    TonicGizmoHandle_AxisV = 1,
    TonicGizmoHandle_AxisW = 2,
    TonicGizmoHandle_Ring = 3,
};

// Number of segments a gizmo/brush circle is drawn with. Fixed, so the
// published topology only changes when the gizmo kind changes.
int USDGENTONIC_API TonicGizmoCircleSegments();

// Build the curves for one record. False (and an empty payload) for kind
// none, a non-finite record or a non-positive size — "no gizmo" is a state,
// not an error, and the caller publishes nothing.
bool USDGENTONIC_API TonicBuildGizmoCurves(TonicGizmoRecord const &record,
                                           TonicOverlayCurves *out);

// The brush ring: one closed circle of radius `radiusWorld` around
// `center`, in the plane `normal` is the normal of. False for an inactive
// record, a degenerate normal or a non-positive radius.
bool USDGENTONIC_API TonicBuildBrushRingCurves(
    TonicBrushRingRecord const &record, TonicOverlayCurves *out);

}  // namespace usdGenTonic

#endif  // USDGEN_TONIC_GIZMO_H
