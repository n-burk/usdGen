# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
#
# usdGenPomadeTools.pomadeGizmoScreen -- screen-space gizmo geometry, hit
# testing and drag parameters.
#
# Adapted for PomadeCamera. The handle layout, the kind-ordered HitTest and
# the drag mapping follow a rig-viewport manipulator, rule for rule.
#
# What changed in the port, and why:
#
#   * Gf.Vec3d arithmetic is spelled over plain 3-tuples. The Pomade T0/T1
#     tests import this package under an interpreter with no pxr
#     (pomadeTestPackage), so the Qt-free half must stay pxr-free too.
#   * The four camera touchpoints go through PomadeCamera instead of a
#     Gf.Camera plus a viewport rectangle: ViewProjection -> camera.viewProj,
#     ProjectPoint -> camera.worldToPixels, CameraBasis -> the camera plane
#     measured at a world point (the same construction as
#     pomadeGizmo.screenFrame), WorldPerPixel -> camera.worldPerPixel and
#     _PickRay -> camera.rayThrough. A PomadeCamera already carries its pixel
#     rectangle, so the `viewport` arguments are gone.
#   * BuildHandles takes the gizmo origin and a packed 9-float frame (rows
#     are the world axes, Pomade's GizmoState.frame) in place of the Gf
#     gizmoMatrix/orientation pair, and an optional `worldLength`: Pomade
#     sizes its gizmo in world units once at placement, and the drawn and
#     picked handles must be exactly the placed ones.
#   * Handle gains `handleId`, the Pomade ABI handle id (pomadeGizmo.HANDLE_*)
#     the adapter in pomadeGizmo stamps on each handle.
#
# Pixel space is the PHYSICAL pixel space PomadeCamera maps to; every pixel
# constant here is LOGICAL and multiplied by the device pixel ratio.
from __future__ import annotations

import math

TOOL_TRANSLATE = "translate"
TOOL_ROTATE = "rotate"
TOOL_SCALE = "scale"

# Handle sizes in LOGICAL pixels (multiplied by the device pixel ratio).
GIZMO_PIXELS = 90.0
HIT_PIXELS = 8.0
CENTER_PIXELS = 6.0
RING_FRACTION = 0.85
RING_SEGMENTS = 48

# a DCC manipulator geometry, as fractions of the manipulator size (design
# section 8.1-8.4).  PLANE_OFFSET places each planar handle 30% out along
# both of its axes; PLANE_SIDE is the square's side; CENTER_SIDE the
# view-plane / uniform-scale square; CUBE_SIDE the scale axis cubes;
# CONE_RADIUS the base radius of the move arrowheads.  The view-axis ring
# is drawn outside the axis rings so it can be grabbed on its own.
PLANE_OFFSET = 0.30
PLANE_SIDE = 0.15
CENTER_SIDE = 0.12
CUBE_SIDE = 0.08
CONE_RADIUS = 0.05
VIEW_RING_FRACTION = 1.25

# a DCC's manipulator palette.  The axis colours are the flat primaries
# a DCC uses, not softened pastels, so a screenshot matches a DCC's.
COLOR_VIEW = (0.4, 0.75, 1.0)
COLOR_HOVER = (1.0, 0.85, 0.4)
COLOR_SELECTED = (1.0, 1.0, 0.0)
COLOR_SPHERE = (0.6, 0.6, 0.6)

# Prevent Negative Scale clamps to this rather than to zero: a zero scale
# is not invertible, so the artist could never drag back out of it.
MIN_SCALE_FACTOR = 1e-4

# A planar handle seen nearly edge-on collapses onto one of its axes and
# would steal that axis's picks, since planes outrank axes in HitTest.
# Below this fraction of its face-on area it stops being grabbable, the
# same bargain MIN_AXIS_PIXELS strikes for a foreshortened axis.
MIN_PLANE_AREA_FRACTION = 0.2

# Below this |dot(planeNormal, rayDirection)| the ray/plane intersection
# is numerically worthless (the plane is edge-on), so the drag falls back
# to sliding in the camera plane.
_MIN_PLANE_FACING = 0.05

# Shortest projected axis that may still be grabbed, in LOGICAL pixels.
# An axis pointing nearly at the camera has almost no screen direction, so
# every pixel of mouse travel becomes a huge world move: at 4 px a 100 px
# drag would push the object 11 world units with the camera 10 units away.
# Every other DCC gizmo answers this the same way -- the axis goes
# ungrabbable and the artist orbits a few degrees before dragging it.
MIN_AXIS_PIXELS = 12.0

_AXES = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
_AXIS_NAMES = ("x", "y", "z")
_AXIS_COLORS = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))

# Planar handles, as (name, normal axis, first axis, second axis).  Each
# is coloured like the axis PERPENDICULAR to its plane, so the yz square
# is red, xz green and xy blue (design section 8.2).
_PLANE_SPECS = (("yz", 0, 1, 2), ("xz", 1, 0, 2), ("xy", 2, 0, 1))

# HitTest resolves ties by this order, not by distance.  The centre wins
# because every axis starts there; planes beat axes because their squares
# sit on top of the axis lines; the free-rotate disc is last because it
# covers the whole manipulator and would otherwise swallow every ring.
_HIT_PRIORITY = ("center", "plane", "axis", "ring", "view", "sphere")


# -- tuple vector helpers (the Gf.Vec3d operators the source uses) ---------

def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _mul(v, s):
    return (v[0] * s, v[1] * s, v[2] * s)


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _length(v):
    return math.sqrt(_dot(v, v))


def _normalized(v):
    length = _length(v)
    if length < 1e-300:
        return (0.0, 0.0, 0.0)
    return (v[0] / length, v[1] / length, v[2] / length)


class Handle(object):
    """
    One drawable, pickable piece of the gizmo.

    `kind` is one of "axis", "plane", "ring", "view", "sphere" or
    "center".  `points` is that kind's projected geometry: two endpoints
    for an axis, four corners for a plane, RING_SEGMENTS points for a
    ring or the view ring, and a single point for the centre and for the
    free-rotate disc (whose size is `radiusPixels`).

    `worldCenter` is the world point the handle is centred on -- the
    square's centre for a plane, the gizmo origin for everything else --
    and `worldCenterScreen` is its projection.  A planar handle also
    carries `worldNormal`.

    `worldOrigin` is the point the gizmo is placed at, for EVERY handle
    kind -- the gizmo origin even for a plane, whose `worldCenter` is
    offset to its square.  Code needing the pivot must read it, not
    `worldCenter` (snapping design sections 3 and 4.2).

    Rings carry `frontPoints`, the runs of projected points on the
    camera side of the ring centre, and `frontWorld`, those points in
    world space.  the host application hides the back half of each ring so the three
    rings stay tellable apart; drawing and picking both use the front.

    `grabbable` is False for an axis that is too foreshortened to drag
    (see MIN_AXIS_PIXELS) or a plane too edge-on to aim at (see
    MIN_PLANE_AREA_FRACTION). Such a handle is still returned so it can
    be drawn dimmed -- the artist needs to see that the handle is there
    and why it will not respond -- but HitTest refuses to pick it.

    Pomade: `handleId` is the ABI handle id (pomadeGizmo.HANDLE_*), stamped
    by the GizmoState adapter; None until then.
    """

    def __init__(self, name, kind, axisIndex, points, worldAxis,
                 worldLength, color, center, grabbable=True,
                 worldCenter=None, worldOrigin=None,
                 worldCenterScreen=None, worldNormal=None,
                 frontPoints=None, frontWorld=None, visible=True,
                 radiusPixels=0.0, sizePixels=0.0, handleId=None):
        self.name = name
        self.kind = kind
        self.axisIndex = axisIndex
        self.points = points
        self.worldAxis = worldAxis
        self.worldLength = worldLength
        self.color = color
        self.center = center
        self.grabbable = grabbable
        self.worldCenter = worldCenter
        # Older callers pass no origin, so it defaults to the centre;
        # BuildHandles always stamps the true gizmo origin instead.
        self.worldOrigin = (worldOrigin if worldOrigin is not None
                            else worldCenter)
        self.worldCenterScreen = worldCenterScreen
        self.worldNormal = worldNormal
        self.frontPoints = frontPoints if frontPoints is not None else []
        self.frontWorld = frontWorld if frontWorld is not None else []
        self.visible = visible
        self.radiusPixels = radiusPixels
        self.sizePixels = sizePixels
        self.handleId = handleId

    def __repr__(self):
        return "<Handle %s %s%s>" % (
            self.name, self.kind, "" if self.grabbable else " (locked)")


# -- camera touchpoints (the adapted half) --------------------------------

def ViewProjection(camera):
    """The row-major 4x4 PomadeCamera projects with, as a flat 16-tuple."""
    return camera.viewProj


def ProjectPoint(camera, p):
    """World -> physical pixels; None behind the eye."""
    projected = camera.worldToPixels(p)
    if projected is None:
        return None
    return (projected[0], projected[1])


def ProjectPointWithW(camera, p):
    """World -> ((x, y) or None, clip-space w).

    The screen half is ProjectPoint unchanged; the w half is what the
    perspective-correct edge parameter divides by (snapping design
    section 3), so it is returned even when the point is behind the
    eye and the screen half is None.
    """
    m = camera.viewProj
    w = (float(p[0]) * m[3] + float(p[1]) * m[7] + float(p[2]) * m[11] +
         m[15])
    if w <= 1e-9:
        return None, w
    return ProjectPoint(camera, p), w


def CameraBasis(camera, worldPoint):
    """(viewDir, up, right) of the camera plane at `worldPoint`.

    Gf.Frustum answers this directly; PomadeCamera is only a matrix, so the
    basis is measured the way pomadeGizmo.screenFrame measures it: step one
    pixel right and one pixel down at the point's depth and unproject.
    `worldPoint` is needed because a perspective camera plane is measured
    at a depth; the directions are the same at every depth. Returns None
    when the point is behind the eye.
    """
    projected = camera.worldToPixels(worldPoint)
    if projected is None:
        return None
    px, py, depth = projected
    here = camera.pixelsToWorld(px, py, depth)
    rightPoint = camera.pixelsToWorld(px + 1.0, py, depth)
    downPoint = camera.pixelsToWorld(px, py + 1.0, depth)
    if here is None or rightPoint is None or downPoint is None:
        return None
    right = _normalized(_sub(rightPoint, here))
    up = _normalized(_sub(here, downPoint))
    # right x up points at the viewer; the view direction is its negation.
    viewDir = _normalized(_cross(up, right))
    return viewDir, up, right


def WorldPerPixel(camera, worldPoint):
    """World units per physical pixel in the camera plane at worldPoint."""
    perPixel = camera.worldPerPixel(worldPoint)
    if not perPixel or perPixel < 1e-300:
        return None
    return float(perPixel)


def _PickRay(camera, point):
    """A world ray (origin, direction) through a physical-pixel point."""
    return camera.rayThrough(point[0], point[1])


# -- layout ----------------------------------------------------------------

def _PolygonArea(points):
    """Unsigned area of a projected polygon, in square pixels."""
    total = 0.0
    for i in range(len(points)):
        x0, y0 = points[i]
        x1, y1 = points[(i + 1) % len(points)]
        total += x0 * y1 - x1 * y0
    return abs(total) * 0.5


def _RingBasis(axis, fallback):
    """
    An orthonormal pair spanning the plane perpendicular to `axis`.

    Gram-Schmidt against `fallback` rather than just taking two of the
    frame's other axes: the Gimbal orientation hands us three axes that
    are NOT mutually perpendicular, and a ring must still be a circle in
    its own axis's plane. For an orthonormal frame this returns exactly
    the other two axes, so the ordinary case is unchanged.
    """
    u = _sub(fallback, _mul(axis, _dot(fallback, axis)))
    if _length(u) < 1e-9:
        for candidate in _AXES:
            u = _sub(candidate, _mul(axis, _dot(candidate, axis)))
            if _length(u) >= 1e-9:
                break
    u = _normalized(u)
    return u, _normalized(_cross(axis, u))


def _FrontRuns(mask):
    """
    Maximal cyclic runs of True in `mask`, as lists of indices.

    A ring that faces the camera is one run of every point, not two runs
    split at index 0, and that run repeats its first index at the end so
    it closes: a circle drawn or picked as an open polyline has a notch
    between the last point and the first, and a click landing in that
    notch finds nothing. A partly hidden ring's runs stay open, because
    their ends are the horizon rather than a seam.
    """
    count = len(mask)
    if not any(mask):
        return []
    if all(mask):
        return [list(range(count)) + [0]]
    start = next(i for i in range(count) if mask[i] and not mask[i - 1])
    runs, run = [], []
    for step in range(count):
        index = (start + step) % count
        if mask[index]:
            run.append(index)
        elif run:
            runs.append(run)
            run = []
    if run:
        runs.append(run)
    return runs


def _ProjectRing(camera, origin, u, v, radius):
    """Projected and world points of a RING_SEGMENTS circle, or None."""
    screen, world = [], []
    for step in range(RING_SEGMENTS):
        theta = 2.0 * math.pi * step / RING_SEGMENTS
        point = _add(origin, _mul(_add(_mul(u, math.cos(theta)),
                                       _mul(v, math.sin(theta))), radius))
        projected = ProjectPoint(camera, point)
        if projected is None:
            return None, None
        screen.append(projected)
        world.append(point)
    return screen, world


def FrameAxes(frame):
    """The three unit axes of a packed 9-float frame, or None if degenerate."""
    axes = []
    for i in range(3):
        axis = (float(frame[3 * i]), float(frame[3 * i + 1]),
                float(frame[3 * i + 2]))
        if _length(axis) < 1e-12:
            return None
        axes.append(_normalized(axis))
    return axes


def BuildHandles(tool, origin, frame, camera, pixelRatio,
                 sizePixels=GIZMO_PIXELS, worldLength=None, gimbalAxes=None,
                 freeRotate=True):
    """
    the host application's manipulator for `tool`, laid out in screen space.

    `origin` places the manipulator; `frame` (9 floats, rows are the world
    axes to draw along) sets the direction of the handles, which is what
    the source's Axis Orientation option switches. `gimbalAxes` replaces
    the three rotate ring axes for Gimbal mode. `worldLength`, when given,
    is the handle length in world units and overrides
    `sizePixels * pixelRatio` pixels at the current zoom (Pomade sizes the
    gizmo at placement).

    Returns [] when the origin is behind the eye or the frame is
    degenerate.
    """
    origin = tuple(float(value) for value in origin)
    wpp = WorldPerPixel(camera, origin)
    if wpp is None:
        return []
    center = ProjectPoint(camera, origin)
    if center is None:
        return []
    if worldLength is not None and worldLength > 0.0:
        length = float(worldLength)
        pixels = length / wpp
    else:
        pixels = sizePixels * pixelRatio
        length = pixels * wpp
    axes = FrameAxes(frame)
    if axes is None:
        return []

    def _Make(name, kind, axisIndex, points, worldAxis, worldLength, color,
              **extra):
        extra.setdefault("worldCenter", origin)
        extra.setdefault("worldOrigin", origin)
        extra.setdefault("worldCenterScreen", center)
        extra.setdefault("sizePixels", pixels)
        return Handle(name, kind, axisIndex, points, worldAxis, worldLength,
                      color, center, **extra)

    handles = []
    if tool in (TOOL_TRANSLATE, TOOL_SCALE):
        for i in range(3):
            end = ProjectPoint(camera, _add(origin, _mul(axes[i], length)))
            if end is None:
                continue
            screen = math.hypot(end[0] - center[0], end[1] - center[1])
            handles.append(_Make(
                _AXIS_NAMES[i], "axis", i, [center, end], axes[i], length,
                _AXIS_COLORS[i],
                grabbable=screen >= MIN_AXIS_PIXELS * pixelRatio))
        half = PLANE_SIDE * 0.5
        for name, normalIndex, first, second in _PLANE_SPECS:
            a, b = axes[first], axes[second]
            corners = []
            for sa, sb in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                point = _add(origin, _add(
                    _mul(a, (PLANE_OFFSET + sa * half) * length),
                    _mul(b, (PLANE_OFFSET + sb * half) * length)))
                projected = ProjectPoint(camera, point)
                if projected is None:
                    corners = None
                    break
                corners.append(projected)
            if corners is None:
                continue
            squareCenter = _add(origin, _mul(_add(a, b),
                                             PLANE_OFFSET * length))
            squareScreen = ProjectPoint(camera, squareCenter)
            if squareScreen is None:
                continue
            faceOn = (PLANE_SIDE * pixels) ** 2
            handles.append(_Make(
                name, "plane", normalIndex, corners, None, PLANE_SIDE * length,
                _AXIS_COLORS[normalIndex],
                worldNormal=axes[normalIndex], worldCenter=squareCenter,
                worldCenterScreen=squareScreen,
                grabbable=_PolygonArea(corners)
                >= faceOn * MIN_PLANE_AREA_FRACTION))
        handles.append(_Make(
            "center", "center", None, [center], None, length, COLOR_VIEW))
    elif tool == TOOL_ROTATE:
        radius = length * RING_FRACTION
        basis = CameraBasis(camera, origin)
        if basis is None:
            return []
        viewDir, up, right = basis
        toCamera = _mul(viewDir, -1.0)
        ringAxes = list(gimbalAxes) if gimbalAxes else axes
        for i in range(3):
            axis = _normalized(ringAxes[i])
            u, v = _RingBasis(axis, ringAxes[(i + 1) % 3])
            screen, world = _ProjectRing(camera, origin, u, v, radius)
            if screen is None:
                continue
            # a DCC hides the half of each ring that is behind the ring
            # centre, so three overlapping circles stay readable.
            mask = [_dot(_sub(p, origin), toCamera) >= -1e-9 for p in world]
            runs = _FrontRuns(mask)
            handles.append(_Make(
                _AXIS_NAMES[i], "ring", i, screen, axis, radius,
                _AXIS_COLORS[i],
                frontPoints=[[screen[k] for k in run] for run in runs],
                frontWorld=[world[k] for run in runs for k in run],
                visible=bool(runs)))
        viewRadius = radius * VIEW_RING_FRACTION
        screen, world = _ProjectRing(camera, origin, right, up, viewRadius)
        if screen is not None:
            # Always fully visible, and closed for the same reason the
            # face-on axis rings are.
            runs = _FrontRuns([True] * len(screen))
            handles.append(_Make(
                "view", "view", None, screen, toCamera, viewRadius,
                COLOR_VIEW,
                frontPoints=[[screen[k] for k in run] for run in runs],
                frontWorld=[world[k] for run in runs for k in run]))
        if freeRotate:
            handles.append(_Make(
                "free", "sphere", None, [center], toCamera, radius,
                COLOR_SPHERE, radiusPixels=pixels * RING_FRACTION))
    return handles


# -- hit testing (verbatim) ------------------------------------------------

def _PointSegmentDistance(p, a, b):
    """Screen distance to a segment, plus the clamped foot parameter.

    The t half is what the snap edge parameter inverts with the
    endpoints' clip w (snapping design section 3); callers needing
    only the distance take [0].
    """
    ax, ay = a[0], a[1]
    bx, by = b[0], b[1]
    dx, dy = bx - ax, by - ay
    length2 = dx * dx + dy * dy
    if length2 < 1e-12:
        return math.hypot(p[0] - ax, p[1] - ay), 0.0
    t = ((p[0] - ax) * dx + (p[1] - ay) * dy) / length2
    t = max(0.0, min(1.0, t))
    return (math.hypot(p[0] - (ax + t * dx), p[1] - (ay + t * dy)),
            t)


def _PointInPolygon(p, points):
    """Even-odd containment test for a projected convex-ish polygon."""
    inside = False
    count = len(points)
    for i in range(count):
        x0, y0 = points[i][0], points[i][1]
        x1, y1 = points[(i + 1) % count][0], points[(i + 1) % count][1]
        if (y0 > p[1]) != (y1 > p[1]):
            crossing = x0 + (p[1] - y0) / (y1 - y0) * (x1 - x0)
            if p[0] < crossing:
                inside = not inside
    return inside


def _PolylineDistance(p, points, closed):
    """Nearest distance from p to a polyline, or None when it is empty."""
    if not points:
        return None
    if len(points) == 1:
        return math.hypot(p[0] - points[0][0], p[1] - points[0][1])
    last = len(points) if closed else len(points) - 1
    return min(_PointSegmentDistance(p, points[i],
                                     points[(i + 1) % len(points)])[0]
               for i in range(last))


def _HandleDistance(handle, p):
    """Screen distance from p to the handle, or None when unpickable."""
    kind = handle.kind
    if kind in ("center", "sphere"):
        return math.hypot(p[0] - handle.points[0][0],
                          p[1] - handle.points[0][1])
    if kind == "axis":
        return _PointSegmentDistance(
            p, handle.points[0], handle.points[1])[0]
    if kind == "plane":
        if _PointInPolygon(p, handle.points):
            return 0.0
        return _PolylineDistance(p, handle.points, True)
    if kind in ("ring", "view"):
        # Only the front half is drawn, so only the front half is aimed
        # at. Each run is treated as an open polyline: closing it here
        # would add a chord straight across the manipulator. A fully
        # visible ring is already closed by _FrontRuns, so its seam is
        # a real segment and is pickable.
        distances = [_PolylineDistance(p, run, False)
                     for run in handle.frontPoints]
        distances = [d for d in distances if d is not None]
        return min(distances) if distances else None
    return _PolylineDistance(p, handle.points, True)


def HitTest(handles, x, y, radius):
    """
    The handle under (x, y), or None.

    Resolution is by kind first (_HIT_PRIORITY) and only then by
    distance, because the handles deliberately overlap: every axis starts
    at the centre, the planar squares sit on the axis lines, and the
    free-rotate disc covers all three rings. Picking the nearest edge
    outright would make the centre and the planes almost unreachable.

    `radius` is the pick tolerance in physical pixels for everything with
    an outline; the free-rotate disc instead claims its whole interior,
    which is how the host application's manipulator works.

    Handles marked not grabbable are skipped, so a foreshortened axis or
    an edge-on plane cannot be picked by accident -- it has collapsed
    onto something else the artist is more likely to have meant.
    """
    p = (x, y)
    pickable = [h for h in handles if h.grabbable]
    for kind in _HIT_PRIORITY:
        best = None
        for handle in pickable:
            if handle.kind != kind:
                continue
            distance = _HandleDistance(handle, p)
            if distance is None:
                continue
            limit = handle.radiusPixels if kind == "sphere" else radius
            if distance <= limit and (best is None or distance < best[0]):
                best = (distance, handle)
        if best is not None:
            return best[1]
    return None


# -- drag parameters -------------------------------------------------------

def AxisDragParameter(handle, press, current, pixelRatio=1.0):
    """
    Mouse travel projected onto the handle's screen direction, as a
    fraction of the handle's screen length (so 1.0 == one handle length
    == handle.worldLength in world units). The screen length is floored at
    MIN_AXIS_PIXELS so a few pixels cannot become a huge move. That is a
    safety net only: BuildHandles already marks such an axis ungrabbable,
    so a drag should never start on one.

    Pomade adaptation: `pixelRatio` scales the LOGICAL floor to physical
    pixels, the same scaling BuildHandles gives the grabbable test, so the
    floor and the ungrabbable cut-off agree on a HiDPI display (parity G09).
    """
    floor = MIN_AXIS_PIXELS * (float(pixelRatio) if pixelRatio else 1.0)
    ax, ay = handle.points[0][0], handle.points[0][1]
    bx, by = handle.points[-1][0], handle.points[-1][1]
    dx, dy = bx - ax, by - ay
    length = math.hypot(dx, dy)
    if length < floor:
        if length < 1e-9:
            return 0.0
        dx = dx / length * floor
        dy = dy / length * floor
        length = floor
    ux, uy = dx / length, dy / length
    travel = (current[0] - press[0]) * ux + (current[1] - press[1]) * uy
    return travel / length


def PlaneDragDelta(camera, worldOrigin, press, current):
    """World delta for a drag in the camera plane through worldOrigin."""
    wpp = WorldPerPixel(camera, worldOrigin)
    basis = CameraBasis(camera, worldOrigin)
    if wpp is None or basis is None:
        return (0.0, 0.0, 0.0)
    _, up, right = basis
    dx = current[0] - press[0]
    dy = current[1] - press[1]
    return _sub(_mul(right, dx * wpp), _mul(up, dy * wpp))


def AxisFacesCamera(camera, worldAxis, worldOrigin):
    basis = CameraBasis(camera, worldOrigin)
    if basis is None:
        return True
    return _dot(worldAxis, basis[0]) < 0.0


def RotationDragAngle(center, press, current, axisFacesCamera):
    """
    Degrees swept around `center` from press to current, positive for
    counter-clockwise ON SCREEN when the rotation axis points at the
    camera (right-hand rule seen from the axis tip), wrapped to
    (-180, 180]. Screen y grows downward, hence the negation.
    """
    a0 = math.atan2(-(press[1] - center[1]), press[0] - center[0])
    a1 = math.atan2(-(current[1] - center[1]), current[0] - center[0])
    degrees = math.degrees(a1 - a0)
    while degrees > 180.0:
        degrees -= 360.0
    while degrees <= -180.0:
        degrees += 360.0
    return degrees if axisFacesCamera else -degrees


def _RayPlanePoint(ray, normal, planePoint):
    """Where `ray` meets the plane, or None when they do not meet."""
    if ray is None:
        return None
    origin, direction = ray
    denom = _dot(normal, direction)
    if abs(denom) < 1e-12:
        return None
    t = _dot(normal, _sub(planePoint, origin)) / denom
    if t < 0.0:
        return None
    return _add(origin, _mul(direction, t))


def RayPlaneDragDelta(camera, worldOrigin, worldNormal, press, current):
    """
    World delta for a drag in the plane through worldOrigin.

    Ray/plane intersection rather than scaled screen travel, so the point
    the artist grabbed stays under the cursor as the plane recedes --
    the host application's planar handles behave this way and a plain screen mapping
    visibly slides away from the cursor in a perspective view.

    Falls back to the camera-plane mapping when the plane is edge-on:
    the intersection is then either at infinity or wildly unstable.
    """
    normal = tuple(float(value) for value in worldNormal)
    if _length(normal) < 1e-12:
        return PlaneDragDelta(camera, worldOrigin, press, current)
    normal = _normalized(normal)
    pressRay = _PickRay(camera, press)
    if pressRay is None or \
            abs(_dot(normal, pressRay[1])) < _MIN_PLANE_FACING:
        return PlaneDragDelta(camera, worldOrigin, press, current)
    start = _RayPlanePoint(pressRay, normal, worldOrigin)
    end = _RayPlanePoint(_PickRay(camera, current), normal, worldOrigin)
    if start is None or end is None:
        return PlaneDragDelta(camera, worldOrigin, press, current)
    return _sub(end, start)


def AccumulateAngle(total, previous, current):
    """
    `total` plus the shortest way round from `previous` to `current`.

    RotationDragAngle wraps into (-180, 180], but a rotate drag has to
    keep counting: the host application lets one sweep run to 400 degrees. Accumulating
    the wrapped step rather than the raw difference is what makes the
    crossing at 180 invisible.
    """
    delta = current - previous
    while delta > 180.0:
        delta -= 360.0
    while delta <= -180.0:
        delta += 360.0
    return total + delta


def TrackballRotation(camera, worldOrigin, press, current, radiusPixels):
    """
    (axis, degrees) for a free-rotate drag, or None when nothing moved.

    A virtual trackball: the axis is perpendicular to the mouse travel in
    the camera plane, so the surface follows the cursor, and dragging one
    diameter sweeps a half turn.
    """
    dx = current[0] - press[0]
    dy = current[1] - press[1]
    travel = math.hypot(dx, dy)
    if travel < 1e-9 or radiusPixels <= 0.0:
        return None
    basis = CameraBasis(camera, worldOrigin)
    if basis is None:
        return None
    viewDir, up, right = basis
    motion = _sub(_mul(right, dx), _mul(up, dy))   # screen y grows downward
    axis = _cross(motion, viewDir)
    if _length(axis) < 1e-12:
        return None
    return _normalized(axis), travel / (2.0 * radiusPixels) * 180.0


def scaleFactor(handle, origin2d, press, current, allowNegative):
    """
    the host application's scale ratio: how far the cursor is from the manipulator origin
    along the handle, over how far it was when the drag started.

    Dragging the handle onto the origin therefore gives 0 and carrying it
    through gives a mirrored negative, unless Prevent Negative Scale is
    on. The centre handle is uniform scale and uses horizontal travel
    against the manipulator size instead, since it has no direction.
    """
    if handle.kind == "center":
        size = handle.sizePixels if handle.sizePixels > 1e-9 else 1.0
        factor = 1.0 + (current[0] - press[0]) / size
    else:
        if handle.kind == "plane":
            dx = handle.worldCenterScreen[0] - origin2d[0]
            dy = handle.worldCenterScreen[1] - origin2d[1]
        else:
            dx = handle.points[-1][0] - handle.points[0][0]
            dy = handle.points[-1][1] - handle.points[0][1]
        length = math.hypot(dx, dy)
        if length < 1e-9:
            return 1.0
        ux, uy = dx / length, dy / length
        at = ((press[0] - origin2d[0]) * ux + (press[1] - origin2d[1]) * uy)
        now = ((current[0] - origin2d[0]) * ux
               + (current[1] - origin2d[1]) * uy)
        if abs(at) < 1e-9:
            return 1.0
        factor = now / at
    if not allowNegative:
        factor = max(MIN_SCALE_FACTOR, factor)
    return factor


def _RoundToStep(value, step):
    """Nearest multiple of `step`, rounding halves away from zero."""
    if step <= 0.0:
        return value
    quotient = value / step
    if quotient >= 0.0:
        return math.floor(quotient + 0.5) * step
    return math.ceil(quotient - 0.5) * step


def SnapRelative(value, step):
    """
    Quantise a DELTA to a multiple of `step` (the host application's Discrete move /
    Snap rotate). Relative to the drag start, so an object that began off
    the grid stays off it and only moves in whole steps.

    Accepts a float or a 3-tuple.
    """
    if isinstance(value, (tuple, list)):
        return tuple(_RoundToStep(v, step) for v in value)
    return _RoundToStep(value, step)


def SnapAbsolute(value, step):
    """
    Quantise a POSITION onto a grid of `step` (the host application's `X` hold). Same
    arithmetic as SnapRelative but a different meaning, and the two are
    separate names because a caller must not confuse a delta with a
    position: this one lands the object ON the grid.

    Accepts a float or a 3-tuple.
    """
    if isinstance(value, (tuple, list)):
        return tuple(_RoundToStep(v, step) for v in value)
    return _RoundToStep(value, step)


def RingParameter(handle, point2d):
    """The ring parameter, in radians, of the ring point nearest point2d."""
    points = handle.points
    if not points:
        return 0.0
    best, bestIndex = None, 0
    for index, p in enumerate(points):
        distance = math.hypot(point2d[0] - p[0], point2d[1] - p[1])
        if best is None or distance < best:
            best, bestIndex = distance, index
    return 2.0 * math.pi * bestIndex / len(points)


def PiePolygon(handle, startParameter, sweepDegrees):
    """
    the host application's rotation-amount wedge: the manipulator centre followed by the
    arc from `startParameter` through `sweepDegrees`.

    `sweepDegrees` is degrees about the ring's OWN world axis -- what
    RotationDragAngle returns and what the controller applies -- so a
    positive sweep always walks increasing parameter. The ring is
    parameterised as origin + (u cos t + v sin t) r with (u, v, axis)
    right-handed, so increasing t is a positive turn about the axis
    whichever way that axis happens to face. Inferring the direction
    from the projected winding instead would send the wedge to the
    opposite side of the ring from the cursor whenever the axis points
    away from the camera, because RotationDragAngle has already flipped
    the sign for exactly that case.
    """
    points = handle.points
    count = len(points)
    if count == 0:
        return [handle.center]
    start = int(round(startParameter / (2.0 * math.pi) * count)) % count
    steps = int(round(abs(sweepDegrees) / 360.0 * count))
    direction = 1 if sweepDegrees >= 0.0 else -1
    wedge = [handle.center]
    for step in range(steps + 1):
        wedge.append(points[(start + direction * step) % count])
    return wedge
