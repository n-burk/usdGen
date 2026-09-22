# usdGenTonicTools.tonicGizmo -- the gizmo state machine (plan/18 section
# 2.4, section 3.1).
#
# Qt-free and model-free: it owns which handle is live, what the drag is
# constrained to, and how a pixel delta becomes a world delta. The shared
# screen-space geometry is consumed by the Qt overlay, with the Hydra scene
# index as fallback; tonicCamera supplies projection, so this state machine
# itself does not draw or pick.
#
# Handle ids match the ABI (tonicApi.h Tonic_SetGizmo): 0/1/2 are the u/v/w
# axes and 3 is the ring. The screen-plane handle (4) is the Maya-style
# centre and planar-square move handle, published so a free drag highlights.
from __future__ import annotations

from . import tonicLib

GIZMO_NONE = tonicLib.TONIC_GIZMO_NONE
GIZMO_TRANSLATE = tonicLib.TONIC_GIZMO_TRANSLATE
GIZMO_RING_TRS = tonicLib.TONIC_GIZMO_RING_TRS
GIZMO_NODE_TRANSLATE = tonicLib.TONIC_GIZMO_NODE_TRANSLATE
GIZMO_ROTATE = tonicLib.TONIC_GIZMO_ROTATE
GIZMO_SCALE = tonicLib.TONIC_GIZMO_SCALE

HANDLE_NONE = -1
HANDLE_U = 0
HANDLE_V = 1
HANDLE_W = 2
HANDLE_RING = 3
HANDLE_CENTER = 4
HANDLE_PLANE_YZ = 5
HANDLE_PLANE_XZ = 6
HANDLE_PLANE_XY = 7
HANDLE_VIEW = 8
HANDLE_FREE = 9
# Compatibility spelling for the free camera-plane handle.
HANDLE_PLANE = HANDLE_CENTER

IDENTITY_FRAME = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)

# How near the cursor must come to a handle, in pixels.
HANDLE_TOLERANCE_PX = 8.0

# Vendored/adapted from usdRig/plugin/rigExecUsdview/gizmoScreen.py. RigExec
# uses pxr.Gf.Camera; Tonic has the narrower worldToPixels/pixelsToWorld
# camera contract, so this local adapter avoids a runtime RigExec dependency
# while retaining its geometry, hit priority and anti-foreshortening rules.
PLANE_OFFSET = 0.30
PLANE_SIDE = 0.15
CENTER_SIDE = 0.12
MIN_AXIS_PIXELS = 12.0
MIN_PLANE_AREA_FRACTION = 0.20
# The C++ overlay draws the ring at the gizmo's full world radius.  Keep the
# picker in lockstep with that geometry: a tilted ring projects to an ellipse,
# never to the convenient screen-space circle an earlier picker assumed.
RING_FRACTION = 1.0
RING_SEGMENTS = 48


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _scale(v, s):
    return (v[0] * s, v[1] * s, v[2] * s)


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _unit(v):
    length = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5
    if length <= 1e-12:
        return None
    return _scale(v, 1.0 / length)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def pointToSegmentPx(px, py, ax, ay, bx, by):
    """Pixel distance from (px, py) to the segment (ax, ay)-(bx, by)."""
    dx = bx - ax
    dy = by - ay
    denom = dx * dx + dy * dy
    if denom <= 1e-12:
        return ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
    t = ((px - ax) * dx + (py - ay) * dy) / denom
    t = max(0.0, min(1.0, t))
    cx = ax + t * dx
    cy = ay + t * dy
    return ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5


def _polygonAreaPx(points):
    return abs(sum(points[i][0] * points[(i + 1) % len(points)][1] -
                   points[(i + 1) % len(points)][0] * points[i][1]
                   for i in range(len(points)))) * 0.5


def _pointInPolygonPx(x, y, points):
    inside = False
    for i in range(len(points)):
        x0, y0 = points[i]
        x1, y1 = points[(i + 1) % len(points)]
        if (y0 > y) != (y1 > y):
            crossing = x0 + (y - y0) * (x1 - x0) / (y1 - y0)
            if x < crossing:
                inside = not inside
    return inside


class GizmoState:
    """Where the gizmo is, which handle is live, and what a drag means."""

    def __init__(self):
        self.kind = GIZMO_NONE
        self.origin = (0.0, 0.0, 0.0)
        self.frame = IDENTITY_FRAME
        self.sizeWorld = 1.0
        self.activeHandle = HANDLE_NONE
        self._pressX = 0.0
        self._pressY = 0.0
        self._pressDepth = 0.0
        self._pressOrigin = (0.0, 0.0, 0.0)
        self._dragging = False
        self._lastRotationAngle = 0.0
        self._rotationTotal = 0.0
        self._allowedHandles = None
        self._freeQuaternion = (1.0, 0.0, 0.0, 0.0)
        self._freeLast = (0.0, 0.0)

    # -- placement ---------------------------------------------------------

    def place(self, origin, sizeWorld, kind=GIZMO_TRANSLATE,
              frame=IDENTITY_FRAME, allowedHandles=None):
        self.kind = int(kind)
        self.origin = tuple(float(v) for v in origin)
        self.frame = tuple(float(v) for v in frame)
        self.sizeWorld = max(float(sizeWorld), 1e-6)
        self.activeHandle = HANDLE_NONE
        self._dragging = False
        self._allowedHandles = (None if allowedHandles is None else
                                frozenset(int(h) for h in allowedHandles))

    def setAllowedHandles(self, handles):
        """Restrict visible and pickable handles for component semantics."""
        self._allowedHandles = (None if handles is None else
                                frozenset(int(h) for h in handles))

    def clear(self):
        self.kind = GIZMO_NONE
        self.activeHandle = HANDLE_NONE
        self._dragging = False

    @property
    def visible(self):
        return self.kind != GIZMO_NONE

    @property
    def dragging(self):
        return self._dragging

    @property
    def pressOrigin(self):
        """Where the gizmo sat when the drag began.

        A drag reads its delta from the press, so a loop that walks the
        gizmo along with the cursor has to move it from here, never from
        the origin it moved to on the previous sample.
        """
        return self._pressOrigin

    def axis(self, index):
        base = int(index) * 3
        return (self.frame[base], self.frame[base + 1], self.frame[base + 2])

    def axisEndpoint(self, index):
        return _add(self.origin, _scale(self.axis(index), self.sizeWorld))

    def _planePixels(self, camera, first, second):
        """Projected Maya two-axis move square, or None behind the camera."""
        u, v = _unit(self.axis(first)), _unit(self.axis(second))
        if u is None or v is None:
            return None
        half = PLANE_SIDE * 0.5
        corners = []
        for su, sv in ((-half, -half), (half, -half), (half, half),
                       (-half, half)):
            point = _add(self.origin, _add(
                _scale(u, (PLANE_OFFSET + su) * self.sizeWorld),
                _scale(v, (PLANE_OFFSET + sv) * self.sizeWorld)))
            projected = camera.worldToPixels(point)
            if projected is None:
                return None
            corners.append((projected[0], projected[1]))
        return corners

    def _circlePixels(self, camera, u, v, radius):
        import math
        points = []
        for i in range(RING_SEGMENTS + 1):
            angle = 2.0 * math.pi * float(i) / float(RING_SEGMENTS)
            p = _add(self.origin, _add(_scale(u, radius * math.cos(angle)),
                                       _scale(v, radius * math.sin(angle))))
            hit = camera.worldToPixels(p)
            if hit is None:
                return []
            points.append((hit[0], hit[1]))
        return points

    def screenHandles(self, camera):
        """Drawable screen geometry shared by the transparent viewport layer.

        Each item has `kind`, `handle`, `points`, `color` and `grabbable`.
        The viewport renderer draws these records; handleAt below picks the
        same records, so visual size and interaction cannot drift apart.
        """
        if camera is None or not self.visible:
            return []
        centre = camera.worldToPixels(self.origin)
        if centre is None:
            return []
        active = self.activeHandle
        axisColors = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                      (0.0, 0.0, 1.0))
        yellow = (1.0, 0.85, 0.10)
        if self.kind == GIZMO_ROTATE:
            out = []
            axes = [_unit(self.axis(i)) for i in (0, 1, 2)]
            if any(axis is None for axis in axes):
                return out
            for index, axis in enumerate(axes):
                u = axes[(index + 1) % 3]
                u = _unit(_sub(u, _scale(axis, sum(u[i] * axis[i]
                                                   for i in range(3)))))
                if u is None:
                    continue
                ring = self._circlePixels(camera, u, _cross(axis, u),
                                          self.sizeWorld * 0.85)
                if ring:
                    out.append({"kind": "ring", "handle": index,
                                "points": ring,
                                "color": yellow if active == index
                                else axisColors[index], "grabbable": True})
            screen = screenFrame(camera, self.origin)
            u, v, w = screen[0:3], screen[3:6], screen[6:9]
            ring = self._circlePixels(camera, u, v, self.sizeWorld * 1.0625)
            if ring:
                out.append({"kind": "view", "handle": HANDLE_VIEW,
                            "points": ring,
                            "color": yellow if active == HANDLE_VIEW
                            else (0.40, 0.75, 1.0), "grabbable": True})
            out.append({"kind": "free", "handle": HANDLE_FREE,
                        "points": [(centre[0], centre[1])],
                        "color": yellow if active == HANDLE_FREE
                        else (0.60, 0.60, 0.60), "grabbable": True,
                        "radiusPx": self.sizeWorld * 0.85 /
                        max(camera.worldPerPixel(self.origin), 1e-12)})
            return self._filterAllowedHandles(out)
        out = [{"kind": "center", "handle": HANDLE_CENTER,
                "points": [(centre[0], centre[1])],
                "color": yellow if active == HANDLE_CENTER else
                (0.40, 0.75, 1.0), "grabbable": True}]
        planeSpecs = ((1, 2, 0, HANDLE_PLANE_YZ),
                      (0, 2, 1, HANDLE_PLANE_XZ),
                      (0, 1, 2, HANDLE_PLANE_XY))
        if self.kind == GIZMO_RING_TRS:
            planeSpecs = ((0, 1, 2, HANDLE_PLANE_XY),)
        for first, second, normal, handle in planeSpecs:
            square = self._planePixels(camera, first, second)
            if square is None:
                continue
            perPixel = camera.worldPerPixel(self.origin)
            faceOn = ((PLANE_SIDE * self.sizeWorld / perPixel) ** 2
                      if perPixel > 0.0 else float("inf"))
            out.append({"kind": "plane", "handle": handle,
                        "points": square,
                        "color": yellow if active == handle
                        else axisColors[normal],
                        "grabbable": _polygonAreaPx(square) >=
                        faceOn * MIN_PLANE_AREA_FRACTION})
        for index in (HANDLE_U, HANDLE_V, HANDLE_W):
            end = camera.worldToPixels(self.axisEndpoint(index))
            if end is None:
                continue
            length = ((end[0] - centre[0]) ** 2 +
                      (end[1] - centre[1]) ** 2) ** 0.5
            out.append({"kind": "axis", "handle": index,
                        "points": [(centre[0], centre[1]), (end[0], end[1])],
                        "color": yellow if active == index else axisColors[index],
                        "grabbable": length >= MIN_AXIS_PIXELS})
        if self.kind == GIZMO_RING_TRS:
            ring = self._ringPixels(camera)
            if ring:
                out.append({"kind": "ring", "handle": HANDLE_RING,
                            "points": ring,
                            "color": yellow if active == HANDLE_RING
                            else (0.85, 0.85, 0.90), "grabbable": True})
        return self._filterAllowedHandles(out)

    def _filterAllowedHandles(self, handles):
        if self._allowedHandles is None:
            return handles
        return [handle for handle in handles
                if handle["handle"] in self._allowedHandles]

    # -- hit testing -------------------------------------------------------

    def handleAt(self, camera, x, y, tolerancePx=HANDLE_TOLERANCE_PX):
        """The handle under pixel (x, y), or HANDLE_NONE.

        The priority is RigExec/Maya's centre, plane, axis, ring.  Overlap is
        deliberate, while foreshortened axes and edge-on planes are visible
        but ungrabbable so pixel travel cannot explode into a world move.
        """
        if camera is None or not self.visible:
            return HANDLE_NONE
        centre = camera.worldToPixels(self.origin)
        if centre is None:
            return HANDLE_NONE
        if self.kind != GIZMO_ROTATE and (self._allowedHandles is None or
                HANDLE_CENTER in self._allowedHandles) and \
                ((x - centre[0]) ** 2 + (y - centre[1]) ** 2) ** 0.5 \
                <= float(tolerancePx):
            return HANDLE_PLANE
        if self.kind == GIZMO_ROTATE:
            rings = [h for h in self.screenHandles(camera)
                     if h["kind"] in ("ring", "view")]
            for handle in rings:
                if self._polylineDistance(x, y, handle["points"]) <= tolerancePx:
                    return handle["handle"]
            for handle in self.screenHandles(camera):
                if handle["kind"] == "free":
                    dx, dy = x - centre[0], y - centre[1]
                    if dx * dx + dy * dy <= handle["radiusPx"] ** 2:
                        return HANDLE_FREE
            return HANDLE_NONE
        for handle in self.screenHandles(camera):
            if not handle["grabbable"]:
                continue
            points = handle["points"]
            if handle["kind"] == "plane" and _pointInPolygonPx(x, y, points):
                return handle["handle"]
        best = HANDLE_NONE
        bestDist = float(tolerancePx)
        for handle in self.screenHandles(camera):
            if handle["kind"] != "axis" or not handle["grabbable"]:
                continue
            a, b = handle["points"]
            dist = pointToSegmentPx(x, y, a[0], a[1], b[0], b[1])
            if dist < bestDist:
                best, bestDist = handle["handle"], dist
        if best != HANDLE_NONE:
            return best
        for handle in self.screenHandles(camera):
            if handle["kind"] == "ring" and handle["grabbable"] and \
                    self._ringDistancePx(camera, x, y) <= float(tolerancePx):
                return handle["handle"]
        return HANDLE_NONE

    @staticmethod
    def _polylineDistance(x, y, points):
        if len(points) < 2:
            return float("inf")
        return min(pointToSegmentPx(x, y, a[0], a[1], b[0], b[1])
                   for a, b in zip(points, points[1:]))

    def _ringPixels(self, camera):
        """The scale ring's projected polyline, matching tonicGizmo.cpp."""
        u = _unit(self.axis(HANDLE_U))
        v = _unit(self.axis(HANDLE_V))
        if u is None or v is None:
            return []
        import math
        out = []
        radius = self.sizeWorld * RING_FRACTION
        for i in range(RING_SEGMENTS + 1):
            angle = 2.0 * math.pi * float(i) / float(RING_SEGMENTS)
            point = _add(self.origin, _add(_scale(u, radius * math.cos(angle)),
                                           _scale(v, radius * math.sin(angle))))
            projected = camera.worldToPixels(point)
            if projected is None:
                return []
            out.append((projected[0], projected[1]))
        return out

    def _ringDistancePx(self, camera, x, y):
        pixels = self._ringPixels(camera)
        if len(pixels) < 2:
            return float("inf")
        return min(pointToSegmentPx(x, y, a[0], a[1], b[0], b[1])
                   for a, b in zip(pixels, pixels[1:]))

    def _ringEllipseRadius(self, camera, x, y):
        """Screen-space radius in the projected U/V basis, when invertible."""
        centre = camera.worldToPixels(self._pressOrigin)
        if centre is None:
            return None
        u = _unit(self.axis(HANDLE_U))
        v = _unit(self.axis(HANDLE_V))
        if u is None or v is None:
            return None
        uPoint = camera.worldToPixels(_add(self._pressOrigin,
                                           _scale(u, self.sizeWorld)))
        vPoint = camera.worldToPixels(_add(self._pressOrigin,
                                           _scale(v, self.sizeWorld)))
        if uPoint is None or vPoint is None:
            return None
        ux, uy = uPoint[0] - centre[0], uPoint[1] - centre[1]
        vx, vy = vPoint[0] - centre[0], vPoint[1] - centre[1]
        determinant = ux * vy - uy * vx
        if abs(determinant) <= 1e-6:
            return None
        dx, dy = float(x) - centre[0], float(y) - centre[1]
        a = (dx * vy - dy * vx) / determinant
        b = (ux * dy - uy * dx) / determinant
        return (a * a + b * b) ** 0.5

    # -- dragging ----------------------------------------------------------

    def begin(self, handle, camera, x, y):
        """Start a drag on `handle`; False when it cannot be projected."""
        if camera is None or handle == HANDLE_NONE:
            return False
        projected = camera.worldToPixels(self.origin)
        if projected is None:
            return False
        self.activeHandle = int(handle)
        self._pressX = float(x)
        self._pressY = float(y)
        self._pressDepth = projected[2]
        self._pressOrigin = self.origin
        self._lastRotationAngle = 0.0
        self._rotationTotal = 0.0
        self._freeQuaternion = (1.0, 0.0, 0.0, 0.0)
        self._freeLast = (self._pressX, self._pressY)
        self._dragging = True
        return True

    def drag(self, camera, x, y):
        """The world delta from the press for the live handle.

        Axis handles project the pixel travel onto the axis's screen span,
        which is the Maya feel: the handle follows the cursor's component
        along the axis and ignores the rest. The plane handle unprojects at
        the press depth, so the point tracks the cursor exactly.
        """
        if not self._dragging or camera is None:
            return (0.0, 0.0, 0.0)
        handle = self.activeHandle
        if handle in (HANDLE_U, HANDLE_V, HANDLE_W):
            centre = camera.worldToPixels(self._pressOrigin)
            end = camera.worldToPixels(
                _add(self._pressOrigin,
                     _scale(self.axis(handle), self.sizeWorld)))
            if centre is None or end is None:
                return (0.0, 0.0, 0.0)
            dirX = end[0] - centre[0]
            dirY = end[1] - centre[1]
            denom = dirX * dirX + dirY * dirY
            if denom <= 1e-9:
                return (0.0, 0.0, 0.0)
            travel = ((float(x) - self._pressX) * dirX +
                      (float(y) - self._pressY) * dirY) / denom
            return _scale(self.axis(handle), travel * self.sizeWorld)
        planeNormals = {
            HANDLE_PLANE_YZ: self.axis(HANDLE_U),
            HANDLE_PLANE_XZ: self.axis(HANDLE_V),
            HANDLE_PLANE_XY: self.axis(HANDLE_W),
        }
        normal = _unit(planeNormals.get(handle, (0.0, 0.0, 0.0)))
        if normal is not None and hasattr(camera, "rayThrough"):
            # Direct adaptation of RigExec gizmoScreen.RayPlaneDragDelta:
            # preserve the grabbed point in a tilted perspective plane,
            # falling back to the camera-plane delta when near-parallel.
            pressRay = camera.rayThrough(self._pressX, self._pressY)
            nowRay = camera.rayThrough(float(x), float(y))
            if pressRay is not None and nowRay is not None:
                if abs(sum(normal[i] * pressRay[1][i]
                           for i in range(3))) >= 0.05:
                    def intersect(ray):
                        denom = sum(normal[i] * ray[1][i] for i in range(3))
                        if abs(denom) < 1e-12:
                            return None
                        t = sum(normal[i] * (self._pressOrigin[i] - ray[0][i])
                                for i in range(3)) / denom
                        return _add(ray[0], _scale(ray[1], t))
                    start, end = intersect(pressRay), intersect(nowRay)
                    if start is not None and end is not None:
                        return _sub(end, start)
        here = camera.pixelsToWorld(float(x), float(y), self._pressDepth)
        there = camera.pixelsToWorld(self._pressX, self._pressY,
                                     self._pressDepth)
        if here is None or there is None:
            return (0.0, 0.0, 0.0)
        return _sub(here, there)

    def ringTwist(self, camera, x, y):
        """Radians swept about the w axis since the press (0 when idle).

        The Ring sub-mode's twist handle. A section chart is TWO
        dimensional -- Tonic_MoveTubeSectionRing takes (du, dv) and nothing
        else -- so a translation along the tube's own tangent cannot be
        expressed at all, and the w axis of a ringTRS gizmo carries the
        twist instead of a third translation.

        Positive is counter-clockwise AS THE ARTIST SEES IT when the w axis
        points at the camera, which is the right-hand rule about w; when w
        points away the screen rotation is mirrored and the sign flips with
        it, so the ring always follows the hand.
        """
        import math
        if not self._dragging or camera is None:
            return 0.0
        centre = camera.worldToPixels(self._pressOrigin)
        if centre is None:
            return 0.0
        was = math.atan2(self._pressY - centre[1], self._pressX - centre[0])
        now = math.atan2(float(y) - centre[1], float(x) - centre[0])
        delta = (now - was + math.pi) % (2.0 * math.pi) - math.pi
        # Pixels put y downward, so a growing atan2 is a CLOCKWISE sweep.
        visual = -delta
        tip = camera.worldToPixels(self.axisEndpoint(HANDLE_W))
        away = tip is not None and tip[2] > centre[2]
        return -visual if away else visual

    def ringScale(self, camera, x, y):
        """Uniform scale factor for a ring-handle drag (1.0 = unchanged)."""
        if not self._dragging or self.activeHandle != HANDLE_RING:
            return 1.0
        was = self._ringEllipseRadius(camera, self._pressX, self._pressY)
        now = self._ringEllipseRadius(camera, x, y)
        if was is None or now is None:
            # An edge-on section has no invertible projected ellipse.  Its
            # visible line still has a useful radial drag, so retain that
            # conservative fallback instead of making scale unreachable.
            centre = camera.worldToPixels(self._pressOrigin)
            if centre is None:
                return 1.0
            was = ((self._pressX - centre[0]) ** 2 +
                   (self._pressY - centre[1]) ** 2) ** 0.5
            now = ((float(x) - centre[0]) ** 2 +
                   (float(y) - centre[1]) ** 2) ** 0.5
        if was <= 1e-6:
            return 1.0
        return now / was

    def rotationDrag(self, camera, x, y):
        """(worldAxis, radians) for the live Maya rotation handle."""
        import math
        if not self._dragging or self.kind != GIZMO_ROTATE:
            return None
        centre = camera.worldToPixels(self._pressOrigin)
        if centre is None:
            return None
        if self.activeHandle in (HANDLE_U, HANDLE_V, HANDLE_W):
            axis = _unit(self.axis(self.activeHandle))
        elif self.activeHandle == HANDLE_VIEW:
            axis = screenFrame(camera, self._pressOrigin)[6:9]
        elif self.activeHandle == HANDLE_FREE:
            frame = screenFrame(camera, self._pressOrigin)
            dx = float(x) - self._freeLast[0]
            dy = float(y) - self._freeLast[1]
            motion = _add(_scale(frame[0:3], dx), _scale(frame[3:6], -dy))
            # screenFrame's w points TOWARD the viewer; RigExec's
            # CameraBasis.viewDir points from eye into the scene.
            axis = _unit(_cross(motion, _scale(frame[6:9], -1.0)))
            if axis is None:
                return None
            radius = self.sizeWorld * 0.85 / max(
                camera.worldPerPixel(self._pressOrigin), 1e-12)
            angle = (dx * dx + dy * dy) ** 0.5 / \
                max(2.0 * radius, 1e-12) * math.pi
            # Direct adaptation of RigExec's per-event TrackballRotation
            # composition.  Tonic's transform adapter evaluates from a
            # frozen press snapshot, so return the equivalent cumulative
            # axis-angle rather than losing turns when the hand changes
            # direction during a curved drag.
            half = angle * 0.5
            step = (math.cos(half), axis[0] * math.sin(half),
                    axis[1] * math.sin(half), axis[2] * math.sin(half))
            w0, x0, y0, z0 = self._freeQuaternion
            w1, x1, y1, z1 = step
            self._freeQuaternion = (
                w1 * w0 - x1 * x0 - y1 * y0 - z1 * z0,
                w1 * x0 + x1 * w0 + y1 * z0 - z1 * y0,
                w1 * y0 - x1 * z0 + y1 * w0 + z1 * x0,
                w1 * z0 + x1 * y0 - y1 * x0 + z1 * w0)
            self._freeLast = (float(x), float(y))
            qw, qx, qy, qz = self._freeQuaternion
            magnitude = (qx * qx + qy * qy + qz * qz) ** 0.5
            if magnitude <= 1e-12:
                # An exact backtrack composes to identity.  It is a real
                # update -- the frozen target must return to its press-time
                # shape -- rather than a no-motion sample.  -identity is
                # the same orientation represented as one full turn.
                return axis, (0.0 if qw >= 0.0 else 2.0 * math.pi)
            return ((qx / magnitude, qy / magnitude, qz / magnitude),
                    2.0 * math.atan2(magnitude, qw))
        else:
            return None
        if axis is None:
            return None
        a0 = math.atan2(-(self._pressY - centre[1]), self._pressX - centre[0])
        a1 = math.atan2(-(float(y) - centre[1]), float(x) - centre[0])
        # Direct port of RigExec gizmoScreen.RotationDragAngle plus its
        # AccumulateAngle call-site: each screen sample is wrapped locally,
        # then the shortest signed step is added so an artist can spin past
        # 180/360 degrees without the transform snapping backwards.
        angle = (a1 - a0 + math.pi) % (2.0 * math.pi) - math.pi
        tip = camera.worldToPixels(_add(self._pressOrigin,
                                        _scale(axis, self.sizeWorld)))
        if tip is not None and tip[2] > centre[2]:
            angle = -angle
        step = angle - self._lastRotationAngle
        while step > math.pi:
            step -= 2.0 * math.pi
        while step <= -math.pi:
            step += 2.0 * math.pi
        self._rotationTotal += step
        self._lastRotationAngle = angle
        return axis, self._rotationTotal

    def scaleFactor(self, camera, x, y):
        """Maya distance-ratio scale from press; center scales uniformly."""
        if not self._dragging or self.kind != GIZMO_SCALE:
            return 1.0
        centre = camera.worldToPixels(self._pressOrigin)
        if centre is None:
            return 1.0
        if self.activeHandle == HANDLE_CENTER:
            size = self.sizeWorld / max(camera.worldPerPixel(self._pressOrigin),
                                        1e-12)
            return 1.0 + (float(x) - self._pressX) / max(size, 1e-12)
        if self.activeHandle in (HANDLE_U, HANDLE_V, HANDLE_W):
            target = camera.worldToPixels(_add(
                self._pressOrigin,
                _scale(self.axis(self.activeHandle), self.sizeWorld)))
        else:
            target = None
            for handle in self.screenHandles(camera):
                if handle["handle"] == self.activeHandle and \
                        handle["kind"] == "plane":
                    pts = handle["points"]
                    target = (sum(p[0] for p in pts) / len(pts),
                              sum(p[1] for p in pts) / len(pts), centre[2])
                    break
        if target is None:
            return 1.0
        dx, dy = target[0] - centre[0], target[1] - centre[1]
        length = (dx * dx + dy * dy) ** 0.5
        if length <= 1e-12:
            return 1.0
        ux, uy = dx / length, dy / length
        before = ((self._pressX - centre[0]) * ux +
                  (self._pressY - centre[1]) * uy)
        after = ((float(x) - centre[0]) * ux +
                 (float(y) - centre[1]) * uy)
        return after / before if abs(before) > 1e-12 else 1.0

    def end(self):
        self.activeHandle = HANDLE_NONE
        self._dragging = False

    # -- publication -------------------------------------------------------

    def abiHandle(self):
        """`activeHandle` as the ABI spells it (the plane handle is -1)."""
        if self.activeHandle in (HANDLE_U, HANDLE_V, HANDLE_W, HANDLE_RING,
                                 HANDLE_CENTER, HANDLE_PLANE_YZ,
                                 HANDLE_PLANE_XZ, HANDLE_PLANE_XY,
                                 HANDLE_VIEW, HANDLE_FREE):
            return self.activeHandle
        return -1

    def push(self, session):
        """Write the record the scene index draws from."""
        import ctypes
        model = getattr(session, "model", None)
        if model is None:
            return False
        # The interactive viewport owns an unoccluded transparent Qt layer.
        # Explicitly clear a formerly published Hydra gizmo rather than
        # merely skipping this call, so fallback geometry never doubles it.
        if getattr(session, "qtGizmoOverlay", False):
            origin = (ctypes.c_float * 3)(*self.origin)
            frame = (ctypes.c_float * 9)(*self.frame)
            return session.dll.Tonic_SetGizmo(
                model, int(GIZMO_NONE), origin, frame,
                ctypes.c_float(self.sizeWorld), int(HANDLE_NONE)) == \
                tonicLib.TONIC_OK
        origin = (ctypes.c_float * 3)(*self.origin)
        frame = (ctypes.c_float * 9)(*self.frame)
        return session.dll.Tonic_SetGizmo(
            model, int(self.kind), origin, frame,
            ctypes.c_float(self.sizeWorld),
            int(self.abiHandle())) == tonicLib.TONIC_OK


def screenFrame(camera, origin):
    """A u/v/w frame whose u and v lie in the screen plane at `origin`.

    The default gizmo frame for a selection with no natural axes: drags
    read the way they look. `camera` may be None, which answers the world
    frame instead.
    """
    if camera is None:
        return IDENTITY_FRAME
    projected = camera.worldToPixels(origin)
    if projected is None:
        return IDENTITY_FRAME
    px, py, depth = projected
    here = camera.pixelsToWorld(px, py, depth)
    right = camera.pixelsToWorld(px + 1.0, py, depth)
    down = camera.pixelsToWorld(px, py + 1.0, depth)
    if here is None or right is None or down is None:
        return IDENTITY_FRAME
    u = _normalize(_sub(right, here))
    v = _normalize(_sub(here, down))
    w = _normalize(_cross(u, v))
    return u + v + w


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _normalize(v):
    length = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5
    if length <= 0.0:
        return (0.0, 0.0, 0.0)
    return (v[0] / length, v[1] / length, v[2] / length)
