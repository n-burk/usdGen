# usdGenTonicTools.tonicGizmo -- the gizmo state machine (plan/18 section
# 2.4, section 3.1).
#
# Qt-free and model-free: it owns which handle is live, what the drag is
# constrained to, and how a pixel delta becomes a world delta. The geometry
# the artist sees is published by the scene index from Tonic_SetGizmo, and
# the projection comes from tonicCamera, so nothing here draws or picks
# through Hydra.
#
# Handle ids match the ABI (tonicApi.h Tonic_SetGizmo): 0/1/2 are the u/v/w
# axes and 3 is the ring. The screen-plane handle (4) is local to this
# module -- the ABI records it as "no active handle" because there is no
# axis to highlight.
from __future__ import annotations

from . import tonicLib

GIZMO_NONE = tonicLib.TONIC_GIZMO_NONE
GIZMO_TRANSLATE = tonicLib.TONIC_GIZMO_TRANSLATE
GIZMO_RING_TRS = tonicLib.TONIC_GIZMO_RING_TRS
GIZMO_NODE_TRANSLATE = tonicLib.TONIC_GIZMO_NODE_TRANSLATE

HANDLE_NONE = -1
HANDLE_U = 0
HANDLE_V = 1
HANDLE_W = 2
HANDLE_RING = 3
HANDLE_PLANE = 4

IDENTITY_FRAME = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)

# How near the cursor must come to a handle, in pixels.
HANDLE_TOLERANCE_PX = 8.0
# The ring handle sits at this fraction of the gizmo's screen size.
RING_FRACTION = 0.75


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _scale(v, s):
    return (v[0] * s, v[1] * s, v[2] * s)


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


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

    # -- placement ---------------------------------------------------------

    def place(self, origin, sizeWorld, kind=GIZMO_TRANSLATE,
              frame=IDENTITY_FRAME):
        self.kind = int(kind)
        self.origin = tuple(float(v) for v in origin)
        self.frame = tuple(float(v) for v in frame)
        self.sizeWorld = max(float(sizeWorld), 1e-6)
        self.activeHandle = HANDLE_NONE
        self._dragging = False

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

    # -- hit testing -------------------------------------------------------

    def handleAt(self, camera, x, y, tolerancePx=HANDLE_TOLERANCE_PX):
        """The handle under pixel (x, y), or HANDLE_NONE.

        The centre disc is the free (screen-plane) handle, and outside it
        axes win over the ring, so a constrained drag is never stolen by
        the free one beside it. The centre has to go to the plane handle:
        all three axes START there, so every pixel within tolerance of the
        origin is within tolerance of all three and no axis can claim it
        honestly -- which is exactly why Maya draws a square in the middle
        of its translate gizmo and treats it as the free move.
        """
        if camera is None or not self.visible:
            return HANDLE_NONE
        centre = camera.worldToPixels(self.origin)
        if centre is None:
            return HANDLE_NONE
        if ((x - centre[0]) ** 2 + (y - centre[1]) ** 2) ** 0.5 \
                <= float(tolerancePx):
            return HANDLE_PLANE
        best = HANDLE_NONE
        bestDist = float(tolerancePx)
        for index in (HANDLE_U, HANDLE_V, HANDLE_W):
            end = camera.worldToPixels(self.axisEndpoint(index))
            if end is None:
                continue
            dist = pointToSegmentPx(x, y, centre[0], centre[1], end[0],
                                    end[1])
            if dist < bestDist:
                best = index
                bestDist = dist
        if best != HANDLE_NONE:
            return best
        if self.kind == GIZMO_RING_TRS:
            radiusPx = self._ringRadiusPx(camera, centre)
            if radiusPx > 0.0:
                here = ((x - centre[0]) ** 2 + (y - centre[1]) ** 2) ** 0.5
                if abs(here - radiusPx) <= float(tolerancePx):
                    return HANDLE_RING
        if ((x - centre[0]) ** 2 + (y - centre[1]) ** 2) ** 0.5 \
                <= float(tolerancePx):
            return HANDLE_PLANE
        return HANDLE_NONE

    def _ringRadiusPx(self, camera, centre):
        perPixel = camera.worldPerPixel(self.origin)
        if perPixel <= 0.0:
            return 0.0
        return self.sizeWorld * RING_FRACTION / perPixel

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

    def end(self):
        self.activeHandle = HANDLE_NONE
        self._dragging = False

    # -- publication -------------------------------------------------------

    def abiHandle(self):
        """`activeHandle` as the ABI spells it (the plane handle is -1)."""
        if self.activeHandle in (HANDLE_U, HANDLE_V, HANDLE_W, HANDLE_RING):
            return self.activeHandle
        return -1

    def push(self, session):
        """Write the record the scene index draws from."""
        import ctypes
        model = getattr(session, "model", None)
        if model is None:
            return False
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
