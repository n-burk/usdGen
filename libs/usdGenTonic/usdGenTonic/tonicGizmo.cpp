// usdGenTonic — gizmo and brush overlay geometry (plan/18 §2.4).
#include "usdGenTonic/tonicGizmo.h"

#include "usdGenTonic/tonicTube.h"

#include <cmath>
#include <utility>

namespace usdGenTonic {

namespace {

constexpr int kCircleSegments = 48;

// RigExec's manipulator palette (gizmoScreen.py _AXIS_COLORS,
// COLOR_SELECTED, COLOR_VIEW), which the Qt overlay in tonicGizmo.py /
// tonicViewport.py draws with too: x, y, z the flat primaries, the handle
// under the drag pure yellow.  The headless fallback must look like the
// interactive gizmo, so these change only together with tonicGizmo.py.
constexpr float kAxisColors[3][3] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
};
constexpr float kActiveColor[3] = {1.0f, 1.0f, 0.0f};
// Tonic's own ringTRS scale ring (tonicGizmo.py RING_COLOR).
constexpr float kRingColor[3] = {0.85f, 0.85f, 0.90f};
constexpr float kViewColor[3] = {0.40f, 0.75f, 1.00f};
constexpr float kBrushColor[3] = {1.0f, 0.75f, 0.25f};

// Kept in lockstep with the vendored viewport picker in tonicGizmo.py and
// RigExec's gizmoScreen.py.  They are fractions of a screen-constant gizmo.
constexpr float kPlaneOffset = 0.30f;
constexpr float kPlaneSide = 0.15f;
constexpr float kCentreSide = 0.12f;
constexpr float kCubeSide = 0.08f;
constexpr float kConeRadius = 0.05f;
// A cone is CONE_LENGTH_RATIO (3) base radii long and ends at the tip.
constexpr float kConeLengthRatio = 3.0f;
constexpr float kConeStart = 1.0f - kConeRadius * kConeLengthRatio;

bool _Finite(float v)
{
    return v == v && std::fabs(v) < 1e30f;
}

bool _Finite3(float const v[3])
{
    return _Finite(v[0]) && _Finite(v[1]) && _Finite(v[2]);
}

void _PushCurve(TonicOverlayCurves *out, int vertexCount,
                float const color[3], float width, int handleId, bool active)
{
    out->vertexCounts.push_back(vertexCount);
    out->colors.push_back(color[0]);
    out->colors.push_back(color[1]);
    out->colors.push_back(color[2]);
    out->widths.push_back(width);
    out->handleIds.push_back(handleId);
    out->active.push_back(active ? 1 : 0);
}

void _PushPoint(TonicOverlayCurves *out, float const p[3])
{
    out->points.push_back(p[0]);
    out->points.push_back(p[1]);
    out->points.push_back(p[2]);
}

// One axis line from the origin along `axis` (already scaled).
void _AddAxis(TonicOverlayCurves *out, float const origin[3],
              float const axis[3], int handleId, bool active, float width)
{
    float const tip[3] = {origin[0] + axis[0], origin[1] + axis[1],
                          origin[2] + axis[2]};
    _PushPoint(out, origin);
    _PushPoint(out, tip);
    _PushCurve(out, 2, active ? kActiveColor : kAxisColors[handleId % 3],
               width, handleId, active);
}

void _AddArrowHead(TonicOverlayCurves *out, float const origin[3],
                   float const axis[3], int handleId, bool active,
                   float width)
{
    float const length = TonicLen3(axis);
    if (length < 1e-9f) {
        return;
    }
    float unit[3] = {axis[0] / length, axis[1] / length, axis[2] / length};
    float u[3], v[3];
    TonicPerp3(unit, u);
    TonicCross3(unit, u, v);
    float const tip[3] = {origin[0] + axis[0], origin[1] + axis[1],
                          origin[2] + axis[2]};
    float const base[3] = {origin[0] + axis[0] * kConeStart,
                           origin[1] + axis[1] * kConeStart,
                           origin[2] + axis[2] * kConeStart};
    float const radius = length * kConeRadius;
    float const color[3] = {active ? kActiveColor[0] : kAxisColors[handleId % 3][0],
                            active ? kActiveColor[1] : kAxisColors[handleId % 3][1],
                            active ? kActiveColor[2] : kAxisColors[handleId % 3][2]};
    // Four outline spokes read as an arrowhead without requiring a mesh
    // overlay, keeping the gizmo in the basisCurves publication path.
    for (int i = 0; i < 4; ++i) {
        float const a = 6.2831853071795864f * float(i) / 4.0f;
        float const p[3] = {base[0] + radius * (u[0] * std::cos(a) + v[0] * std::sin(a)),
                            base[1] + radius * (u[1] * std::cos(a) + v[1] * std::sin(a)),
                            base[2] + radius * (u[2] * std::cos(a) + v[2] * std::sin(a))};
        _PushPoint(out, tip);
        _PushPoint(out, p);
        _PushCurve(out, 2, color, width, handleId, active);
    }
}

void _AddSquare(TonicOverlayCurves *out, float const origin[3],
                float const u[3], float const v[3], float offset,
                float side, float const color[3], int handleId, bool active,
                float width)
{
    float const centre[3] = {origin[0] + (u[0] + v[0]) * offset,
                             origin[1] + (u[1] + v[1]) * offset,
                             origin[2] + (u[2] + v[2]) * offset};
    float const half = side * 0.5f;
    for (int i = 0; i <= 4; ++i) {
        float const su = (i == 0 || i == 3 || i == 4) ? -half : half;
        float const sv = (i == 0 || i == 1 || i == 4) ? -half : half;
        float const p[3] = {centre[0] + u[0] * su + v[0] * sv,
                            centre[1] + u[1] * su + v[1] * sv,
                            centre[2] + u[2] * su + v[2] * sv};
        _PushPoint(out, p);
    }
    _PushCurve(out, 5, active ? kActiveColor : color, width, handleId, active);
}

// A wire cube of edge `side` centred on `centre`, its edges along the unit
// axes a, b, c: the Scale tip (RigExec draws a filled square in screen
// space; with no camera here the wire cube reads the same from any side).
// Two closed faces plus the four edges joining them: six curves.
void _AddCube(TonicOverlayCurves *out, float const centre[3],
              float const a[3], float const b[3], float const c[3],
              float side, float const color[3], int handleId, bool active,
              float width)
{
    float const half = side * 0.5f;
    float const *const tint = active ? kActiveColor : color;
    auto corner = [&](float sa, float sb, float sc, float p[3]) {
        for (int i = 0; i < 3; ++i) {
            p[i] = centre[i] + (a[i] * sa + b[i] * sb + c[i] * sc) * half;
        }
    };
    static int const kLoop[5][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1},
                                     {-1, -1}};
    for (int face = 0; face < 2; ++face) {
        float const sc = face == 0 ? -1.0f : 1.0f;
        for (int i = 0; i < 5; ++i) {
            float p[3];
            corner(float(kLoop[i][0]), float(kLoop[i][1]), sc, p);
            _PushPoint(out, p);
        }
        _PushCurve(out, 5, tint, width, handleId, active);
    }
    for (int i = 0; i < 4; ++i) {
        float p[3];
        corner(float(kLoop[i][0]), float(kLoop[i][1]), -1.0f, p);
        _PushPoint(out, p);
        corner(float(kLoop[i][0]), float(kLoop[i][1]), 1.0f, p);
        _PushPoint(out, p);
        _PushCurve(out, 2, tint, width, handleId, active);
    }
}

// A closed circle of radius `r` around `center` in the (u, v) plane; the
// last CV repeats the first so a linear nonperiodic curve reads closed (the
// spelling the ring overlays already use).
void _AddCircle(TonicOverlayCurves *out, float const center[3],
                float const u[3], float const v[3], float r,
                float const color[3], int handleId, bool active, float width)
{
    for (int i = 0; i <= kCircleSegments; ++i) {
        float const a =
            6.2831853071795864f * float(i % kCircleSegments) /
            float(kCircleSegments);
        float const cu = std::cos(a) * r;
        float const cv = std::sin(a) * r;
        float const p[3] = {center[0] + u[0] * cu + v[0] * cv,
                            center[1] + u[1] * cu + v[1] * cv,
                            center[2] + u[2] * cu + v[2] * cv};
        _PushPoint(out, p);
    }
    _PushCurve(out, kCircleSegments + 1, active ? kActiveColor : color, width,
               handleId, active);
}

// Drop every curve whose handle id is not in `mask` (GZ-06): the builders
// above stay kind-driven and the tool's per-sub-mode whitelist is applied
// once, the same way GizmoState.handles filters its records.
void _KeepAllowedHandles(TonicOverlayCurves *out, unsigned int mask)
{
    TonicOverlayCurves kept;
    size_t point = 0;
    for (int c = 0; c < out->CurveCount(); ++c) {
        int const count = out->vertexCounts[c];
        int const id = out->handleIds[c];
        bool const allowed = id >= 0 && id < 32 && ((mask >> id) & 1u) != 0;
        if (allowed) {
            kept.points.insert(kept.points.end(),
                               out->points.begin() + point * 3,
                               out->points.begin() + (point + count) * 3);
            kept.vertexCounts.push_back(count);
            kept.colors.insert(kept.colors.end(), out->colors.begin() + c * 3,
                               out->colors.begin() + c * 3 + 3);
            kept.widths.push_back(out->widths[c]);
            kept.handleIds.push_back(id);
            kept.active.push_back(out->active[c]);
        }
        point += size_t(count);
    }
    *out = std::move(kept);
}

}  // namespace

bool
TonicGizmoRecord::operator==(TonicGizmoRecord const &o) const
{
    if (kind != o.kind || activeHandle != o.activeHandle ||
        sizeWorld != o.sizeWorld || allowedMask != o.allowedMask) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (origin[i] != o.origin[i]) {
            return false;
        }
    }
    for (int i = 0; i < 9; ++i) {
        if (frame[i] != o.frame[i]) {
            return false;
        }
    }
    return true;
}

bool
TonicBrushRingRecord::operator==(TonicBrushRingRecord const &o) const
{
    if (active != o.active || radiusWorld != o.radiusWorld) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (center[i] != o.center[i] || normal[i] != o.normal[i]) {
            return false;
        }
    }
    return true;
}

void
TonicOverlayCurves::Clear()
{
    points.clear();
    vertexCounts.clear();
    colors.clear();
    widths.clear();
    handleIds.clear();
    active.clear();
}

int
TonicGizmoCircleSegments()
{
    return kCircleSegments;
}

bool
TonicBuildGizmoCurves(TonicGizmoRecord const &record, TonicOverlayCurves *out)
{
    if (!out) {
        return false;
    }
    out->Clear();
    if (record.kind == TonicGizmo_None || !(record.sizeWorld > 0.0f) ||
        !_Finite(record.sizeWorld) || !_Finite3(record.origin)) {
        return false;
    }
    for (int i = 0; i < 9; ++i) {
        if (!_Finite(record.frame[i])) {
            return false;
        }
    }
    // A node gizmo is deliberately a third of the size: it sits on the
    // scalp among dozens of siblings, and a full-length one would hide the
    // graph it edits.
    float const scale = record.kind == TonicGizmo_NodeTranslate
                            ? record.sizeWorld / 3.0f
                            : record.sizeWorld;
    float const width = scale * 0.03f;
    float axes[3][3];
    for (int a = 0; a < 3; ++a) {
        float const raw[3] = {record.frame[a * 3 + 0], record.frame[a * 3 + 1],
                              record.frame[a * 3 + 2]};
        float const len = TonicLen3(raw);
        if (len < 1e-9f) {
            return false;  // a degenerate frame draws no gizmo at all
        }
        for (int i = 0; i < 3; ++i) {
            axes[a][i] = raw[i] / len * scale;
        }
    }
    if (record.kind == TonicGizmo_RingTRS) {
        float u[3], v[3];
        for (int i = 0; i < 3; ++i) {
            u[i] = axes[0][i] / scale;
            v[i] = axes[1][i] / scale;
        }
        _AddCircle(out, record.origin, u, v, scale, kRingColor,
                   TonicGizmoHandle_Ring,
                   record.activeHandle == TonicGizmoHandle_Ring, width);
    }
    if (record.kind == TonicGizmo_Rotate) {
        // The transparent Qt viewport layer adds the camera-view and free
        // rings.  This Hydra fallback has no camera, so it publishes the
        // three owner-frame rings only.
        for (int a = 0; a < 3; ++a) {
            int const first = (a + 1) % 3;
            int const second = (a + 2) % 3;
            float const u[3] = {axes[first][0] / scale, axes[first][1] / scale,
                                axes[first][2] / scale};
            float const v[3] = {axes[second][0] / scale, axes[second][1] / scale,
                                axes[second][2] / scale};
            _AddCircle(out, record.origin, u, v, scale * 0.85f,
                       kAxisColors[a], a, record.activeHandle == a, width);
        }
    } else {
        // The tip tells the tools apart, as in the Qt overlay
        // (GizmoState.tipKind): a cone for Move and the ringTRS gizmo, a
        // cube for Scale, bare lines for the node gizmo.
        for (int a = 0; a < 3; ++a) {
            bool const active = record.activeHandle == a;
            _AddAxis(out, record.origin, axes[a], a, active, width);
            if (record.kind == TonicGizmo_Scale) {
                float const tip[3] = {record.origin[0] + axes[a][0],
                                      record.origin[1] + axes[a][1],
                                      record.origin[2] + axes[a][2]};
                float unit[3][3];
                for (int k = 0; k < 3; ++k) {
                    for (int i = 0; i < 3; ++i) {
                        unit[k][i] = axes[k][i] / scale;
                    }
                }
                _AddCube(out, tip, unit[0], unit[1], unit[2],
                         kCubeSide * scale, kAxisColors[a], a, active, width);
            } else if (record.kind != TonicGizmo_NodeTranslate) {
                _AddArrowHead(out, record.origin, axes[a], a, active, width);
            }
        }
    }
    if (record.kind == TonicGizmo_Translate ||
        record.kind == TonicGizmo_Scale) {
        // yz is red, xz green, xy blue: plane colours name their missing
        // axis, exactly as in a DCC and RigExec's viewport gizmo.
        _AddSquare(out, record.origin, axes[1], axes[2], kPlaneOffset,
                   kPlaneSide, kAxisColors[0], TonicGizmoHandle_PlaneYZ,
                   record.activeHandle == TonicGizmoHandle_PlaneYZ, width);
        _AddSquare(out, record.origin, axes[0], axes[2], kPlaneOffset,
                   kPlaneSide, kAxisColors[1], TonicGizmoHandle_PlaneXZ,
                   record.activeHandle == TonicGizmoHandle_PlaneXZ, width);
        _AddSquare(out, record.origin, axes[0], axes[1], kPlaneOffset,
                   kPlaneSide, kAxisColors[2], TonicGizmoHandle_PlaneXY,
                   record.activeHandle == TonicGizmoHandle_PlaneXY, width);
    } else if (record.kind == TonicGizmo_RingTRS) {
        // A section chart is two dimensional: only its uv move square is
        // drawn, never a tempting tangent-plane handle that cannot move it.
        _AddSquare(out, record.origin, axes[0], axes[1], kPlaneOffset,
                   kPlaneSide, kAxisColors[2], TonicGizmoHandle_PlaneXY,
                   record.activeHandle == TonicGizmoHandle_PlaneXY, width);
    }
    if (record.kind != TonicGizmo_NodeTranslate) {
        _AddSquare(out, record.origin, axes[0], axes[1], 0.0f,
                   kCentreSide, kViewColor, TonicGizmoHandle_Center,
                   record.activeHandle == TonicGizmoHandle_Center, width);
    }
    if (record.allowedMask != TonicGizmoAllHandles) {
        _KeepAllowedHandles(out, record.allowedMask);
        if (out->CurveCount() == 0) {
            return false;  // everything hidden: no gizmo, as for kind none
        }
    }
    return true;
}

bool
TonicBuildBrushRingCurves(TonicBrushRingRecord const &record,
                          TonicOverlayCurves *out)
{
    if (!out) {
        return false;
    }
    out->Clear();
    if (!record.active || !(record.radiusWorld > 0.0f) ||
        !_Finite(record.radiusWorld) || !_Finite3(record.center) ||
        !_Finite3(record.normal)) {
        return false;
    }
    float n[3] = {record.normal[0], record.normal[1], record.normal[2]};
    float const len = TonicLen3(n);
    if (len < 1e-9f) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        n[i] /= len;
    }
    // The ring's own basis comes from the shared perpendicular rule the
    // tube frames use, so a brush ring and the section rings it hovers
    // over are built by the same maths.
    float u[3], v[3];
    TonicPerp3(n, u);
    TonicCross3(n, u, v);
    _AddCircle(out, record.center, u, v, record.radiusWorld, kBrushColor,
               TonicGizmoHandle_Ring, false, record.radiusWorld * 0.03f);
    return true;
}

}  // namespace usdGenTonic
