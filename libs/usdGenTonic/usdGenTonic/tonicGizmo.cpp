// usdGenTonic — gizmo and brush overlay geometry (plan/18 §2.4).
#include "usdGenTonic/tonicGizmo.h"

#include "usdGenTonic/tonicTube.h"

#include <cmath>

namespace usdGenTonic {

namespace {

constexpr int kCircleSegments = 48;

// Maya's axis colours, which is what plan/18 §2.4a asks for: x red, y
// green, z blue, the handle under the drag yellow.
constexpr float kAxisColors[3][3] = {
    {0.90f, 0.15f, 0.15f},
    {0.20f, 0.85f, 0.20f},
    {0.25f, 0.45f, 0.95f},
};
constexpr float kActiveColor[3] = {1.0f, 0.85f, 0.10f};
constexpr float kRingColor[3] = {0.85f, 0.85f, 0.90f};
constexpr float kBrushColor[3] = {1.0f, 0.75f, 0.25f};

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

}  // namespace

bool
TonicGizmoRecord::operator==(TonicGizmoRecord const &o) const
{
    if (kind != o.kind || activeHandle != o.activeHandle ||
        sizeWorld != o.sizeWorld) {
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
    for (int a = 0; a < 3; ++a) {
        _AddAxis(out, record.origin, axes[a], a, record.activeHandle == a,
                 width);
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
