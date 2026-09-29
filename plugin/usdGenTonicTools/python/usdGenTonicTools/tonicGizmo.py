# usdGenTonicTools.tonicGizmo -- the gizmo state machine (plan/18 section
# 2.4, section 3.1).
#
# Qt-free and model-free: it owns which handle is live, what the drag is
# constrained to, and how a pixel delta becomes a world delta. The shared
# screen-space geometry is consumed by the Qt overlay, with the Hydra scene
# index as fallback; tonicCamera supplies projection, so this state machine
# itself does not draw or pick.
#
# Handle ids match the ABI (tonicGizmo.h TonicGizmoHandle): 0/1/2 are the
# u/v/w axes, 3 the ringTRS scale ring, 4 the camera-plane centre square,
# 5/6/7 the yz/xz/xy planar squares, 8 the rotate view ring and 9 the free
# rotate ball.
from __future__ import annotations

from . import tonicGizmoScreen
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

IDENTITY_FRAME = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)

# How near the cursor must come to a handle, in LOGICAL pixels: handleAt
# multiplies it by the camera's device pixel ratio, as RigExec does
# (gizmoUI._HitTest, HIT_PIXELS * devicePixelRatioF).
HANDLE_TOLERANCE_PX = tonicGizmoScreen.HIT_PIXELS

# The handle layout, hit priority and anti-foreshortening rules are the
# vendored RigExec ones (tonicGizmoScreen, from usdRig gizmoScreen.py).
# These names stay for the Qt overlay and older callers; they ARE the
# vendored values, so drawing and picking cannot drift apart.
PLANE_OFFSET = tonicGizmoScreen.PLANE_OFFSET
PLANE_SIDE = tonicGizmoScreen.PLANE_SIDE
CENTER_SIDE = tonicGizmoScreen.CENTER_SIDE
MIN_AXIS_PIXELS = tonicGizmoScreen.MIN_AXIS_PIXELS
MIN_PLANE_AREA_FRACTION = tonicGizmoScreen.MIN_PLANE_AREA_FRACTION

# Handle colours: RigExec's palette (gizmoScreen.py), so the Tonic gizmo
# reads like the RigExec manipulators.  The dragged handle, and the
# last-dragged one after the release (RigExec's persistent "selected"
# handle, which a middle drag repeats), read pure yellow; the handle under
# the idle cursor reads in the paler hover colour, so the artist sees what
# a press will grab.  The C++ Hydra fallback (tonicGizmo.cpp kAxisColors,
# kActiveColor) uses the same values.
AXIS_COLORS = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
ACTIVE_COLOR = tonicGizmoScreen.COLOR_SELECTED
SELECTED_COLOR = tonicGizmoScreen.COLOR_SELECTED
HOVER_COLOR = tonicGizmoScreen.COLOR_HOVER
VIEW_COLOR = tonicGizmoScreen.COLOR_VIEW
SPHERE_COLOR = tonicGizmoScreen.COLOR_SPHERE
# Tonic's own ringTRS scale ring (no RigExec counterpart).
RING_COLOR = (0.85, 0.85, 0.90)

# How the overlay paints, from RigExec gizmoUI.py (the Qt shell there;
# kept here so the Qt-free tests can pin them).  Widths are LOGICAL
# pixels.  An ungrabbable handle is its own colour dimmed, so the artist
# sees it is there and inert; a planar square is a half-filled patch; the
# rotation wedge and the free-rotate ball are washes the rings show through.
LINE_WIDTH = 2.0
LOCKED_OPACITY = 0.4
PLANE_FILL_OPACITY = 0.5
PIE_OPACITY = 0.3
SPHERE_OPACITY = 0.5
SPHERE_FILL_OPACITY = 0.30
# A move arrowhead is this many times its base radius long.
CONE_LENGTH_RATIO = 3.0
CONE_RADIUS = tonicGizmoScreen.CONE_RADIUS
CUBE_SIDE = tonicGizmoScreen.CUBE_SIDE

# Vendored handle kinds -> Tonic ABI handle ids.
_PLANE_HANDLES = {0: HANDLE_PLANE_YZ, 1: HANDLE_PLANE_XZ, 2: HANDLE_PLANE_XY}
# The C++ overlay draws the ring at the gizmo's full world radius.  Keep the
# picker in lockstep with that geometry: a tilted ring projects to an ellipse,
# never to the convenient screen-space circle an earlier picker assumed.
RING_FRACTION = 1.0
RING_SEGMENTS = 48
# RigExec gizmoDrag.PLANE_DELTA_SANITY: a ray/plane delta more than this
# many times the camera-plane delta for the same travel is an edge-on miss.
PLANE_DELTA_SANITY = 50.0


def _length(v):
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5


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


def cameraPixelRatio(camera, pixelRatio=None):
    """The device pixel ratio a pixel constant must be scaled by.

    An explicit ratio wins; otherwise the camera's (TonicCamera resolved
    from a StageView carries devicePixelRatioF); a bare camera means 1.
    """
    if pixelRatio is None:
        pixelRatio = getattr(camera, "pixelRatio", 1.0)
    try:
        ratio = float(pixelRatio)
    except (TypeError, ValueError):
        return 1.0
    return ratio if ratio > 0.0 else 1.0


# Click-versus-drag slop, in LOGICAL pixels (parity G24: every pixel
# constant is logical x the device pixel ratio, as RigExec's DRAG_SLOP).
# The viewport's move threshold and the Tube loop's click-or-band test both
# compare physical event pixels against clickSlopPixels(camera).
CLICK_SLOP_PX = 2.0


def clickSlopPixels(camera, pixelRatio=None):
    """CLICK_SLOP_PX in the physical pixels a Sample carries."""
    return CLICK_SLOP_PX * cameraPixelRatio(camera, pixelRatio)


class GizmoState:
    """Where the gizmo is, which handle is live, and what a drag means."""

    def __init__(self):
        self.kind = GIZMO_NONE
        self.origin = (0.0, 0.0, 0.0)
        self.frame = IDENTITY_FRAME
        self.sizeWorld = 1.0
        self.activeHandle = HANDLE_NONE
        # The handle under the idle cursor (prehighlight), and the handle
        # the last drag grabbed.  The latter survives the release, as in
        # RigExec, so it stays lit and a middle drag can repeat it.
        self.hoverHandle = HANDLE_NONE
        self.selectedHandle = HANDLE_NONE
        self._selectedKind = GIZMO_NONE
        self._pressX = 0.0
        self._pressY = 0.0
        self._pressDepth = 0.0
        self._pressOrigin = (0.0, 0.0, 0.0)
        self._dragging = False
        self._lastRotationAngle = 0.0
        self._rotationTotal = 0.0
        # The angle (degrees) the target actually turned when Step Snap
        # quantised it (RigExec state.angle = _DisplayAngle): the wedge
        # draws this so it ends where the object ended.  None = unsnapped.
        self._displayAngle = None
        self._allowedHandles = None
        self._freeQuaternion = (1.0, 0.0, 0.0, 0.0)
        self._freeLast = (0.0, 0.0)
        # The ring a rotate drag grabbed, as it was at the press, and where
        # on it the cursor landed: the rotation wedge is drawn from these
        # (RigExec DragState.handle/startParameter), so the pie stays on
        # the ring the hand is on however the sweep runs.
        self._pressHandle = None
        self._startParameter = 0.0
        # The grabbed handle as it was built at the press, for every tool:
        # an axis drag measures its travel against the press-time screen
        # span and a scale drag its ratio against the press-time direction
        # (RigExec DragState.handle), however the gizmo moves meanwhile.
        self._dragBuilt = None
        # The last accepted ray/plane delta, for the edge-on sanity net.
        self._lastPlaneDelta = None
        # Parity G23: the Rotate gizmo's free-rotate ball is optional.
        self.freeRotate = True
        # GZ-06: shown but not grabbable (a pinned root CV).
        self.locked = False

    # -- placement ---------------------------------------------------------

    def place(self, origin, sizeWorld, kind=GIZMO_TRANSLATE,
              frame=IDENTITY_FRAME, allowedHandles=None, freeRotate=True,
              locked=False):
        """Put the gizmo somewhere.

        `locked` shows every handle but makes none grabbable (GZ-06): a
        pinned root CV under Rotate/Scale keeps its manipulator on screen,
        dimmed, so the artist sees why nothing happens instead of a gizmo
        that silently vanished.
        """
        self.kind = int(kind)
        self.origin = tuple(float(v) for v in origin)
        self.frame = tuple(float(v) for v in frame)
        self.sizeWorld = max(float(sizeWorld), 1e-6)
        self.activeHandle = HANDLE_NONE
        self._dragging = False
        self.freeRotate = bool(freeRotate)
        self.locked = bool(locked)
        self._allowedHandles = (None if allowedHandles is None else
                                frozenset(int(h) for h in allowedHandles))
        if not self.freeRotate and self.kind == GIZMO_ROTATE:
            # The ball is gone, so it can be neither hovered nor repeated.
            if self.selectedHandle == HANDLE_FREE:
                self.selectedHandle = HANDLE_NONE
            if self.hoverHandle == HANDLE_FREE:
                self.hoverHandle = HANDLE_NONE
        # The remembered handle means "the same handle of the same
        # manipulator": a different tool/kind, or a sub-mode that no longer
        # offers it, forgets it (parity G04).
        if self.kind != self._selectedKind or \
                not self.handleAllowed(self.selectedHandle):
            self.selectedHandle = HANDLE_NONE
            self._selectedKind = GIZMO_NONE
        if not self.handleAllowed(self.hoverHandle):
            self.hoverHandle = HANDLE_NONE

    def setAllowedHandles(self, handles):
        """Restrict visible and pickable handles for component semantics."""
        self._allowedHandles = (None if handles is None else
                                frozenset(int(h) for h in handles))

    def handleAllowed(self, handle):
        """True when `handle` is a real handle this placement offers."""
        if handle is None or int(handle) == HANDLE_NONE or self.locked:
            return False
        return self._allowedHandles is None or \
            int(handle) in self._allowedHandles

    def allowedMask(self):
        """The allowed handles as the ABI's bit mask (bit = handle id).

        Tonic_SetGizmoEx hands it to the headless Hydra fallback so that
        draws exactly the handles the Qt overlay does (GZ-06).
        """
        if self._allowedHandles is None:
            return 0xFFFFFFFF
        mask = 0
        for handle in self._allowedHandles:
            if 0 <= handle < 32:
                mask |= 1 << handle
        return mask

    def clear(self):
        self.kind = GIZMO_NONE
        self.activeHandle = HANDLE_NONE
        self.hoverHandle = HANDLE_NONE
        self._dragging = False

    def resetHandles(self):
        """Forget the hovered and the remembered handle (leaving the mode)."""
        self.hoverHandle = HANDLE_NONE
        self.selectedHandle = HANDLE_NONE
        self._selectedKind = GIZMO_NONE

    def remembered(self):
        """The remembered (last-dragged) handle, as restoreRemembered takes."""
        return (self.selectedHandle, self._selectedKind)

    def restoreRemembered(self, remembered):
        """Put back what `remembered()` returned.

        A tweak press drives the centre handle without the artist ever
        grabbing it; when it ends as a plain click the handle it lit must
        not stay yellow or arm the middle-drag repeat (RigExec only
        remembers a handle a press actually hit).
        """
        self.selectedHandle, self._selectedKind = remembered

    def handleGrabbable(self, camera, handle):
        """True when `handle` is offered here AND a press could grab it now.

        handleAllowed() plus the built handle's own `grabbable` at this
        camera: an axis foreshortened below MIN_AXIS_PIXELS (drawn dimmed,
        skipped by HitTest) is not.  Without a camera only handleAllowed()
        can be answered.
        """
        if not self.handleAllowed(handle):
            return False
        if camera is None:
            return True
        for built in self.handles(camera):
            if built.handleId == int(handle):
                return bool(built.grabbable)
        return False

    def setHoverHandle(self, handle):
        """Prehighlight `handle` (HANDLE_NONE clears); True when it changed."""
        handle = int(handle) if handle is not None else HANDLE_NONE
        if handle != HANDLE_NONE and (not self.visible or
                                      not self.handleAllowed(handle)):
            handle = HANDLE_NONE
        changed = handle != self.hoverHandle
        self.hoverHandle = handle
        return changed

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

    def handles(self, camera, pixelRatio=None):
        """The vendored RigExec handles of this placement, Tonic ids stamped.

        One list feeds both the Qt overlay (screenHandles) and the picker
        (handleAt), so what the artist sees is exactly what a press grabs.
        Tonic keeps three things of its own on top of the RigExec layout:
        the gizmo is sized in world units at placement (`sizeWorld`), the
        ringTRS gizmo has only the in-plane square plus its scale ring, and
        `allowedHandles` hides whatever a sub-mode cannot drive.
        """
        if camera is None or not self.visible:
            return []
        ratio = cameraPixelRatio(camera, pixelRatio)
        if self.kind == GIZMO_ROTATE:
            tool = tonicGizmoScreen.TOOL_ROTATE
        elif self.kind == GIZMO_SCALE:
            tool = tonicGizmoScreen.TOOL_SCALE
        else:
            tool = tonicGizmoScreen.TOOL_TRANSLATE
        built = tonicGizmoScreen.BuildHandles(
            tool, self.origin, self.frame, camera, ratio,
            worldLength=self.sizeWorld, freeRotate=self.freeRotate)
        out = []
        for handle in built:
            if handle.kind == "plane":
                handle.handleId = _PLANE_HANDLES[handle.axisIndex]
                if self.kind == GIZMO_RING_TRS and \
                        handle.handleId != HANDLE_PLANE_XY:
                    continue      # a section chart has only its own plane
            elif handle.kind in ("axis", "ring"):
                handle.handleId = int(handle.axisIndex)
            elif handle.kind == "center":
                handle.handleId = HANDLE_CENTER
            elif handle.kind == "view":
                handle.handleId = HANDLE_VIEW
            elif handle.kind == "sphere":
                handle.handleId = HANDLE_FREE
            else:
                continue
            out.append(handle)
        if self.kind == GIZMO_RING_TRS and out:
            ring = self._ringPixels(camera)
            if ring:
                # Tonic's section scale ring: drawn whole and picked whole
                # (it is not one of three overlapping rotate rings, so it
                # has no back half to hide).  Its kind sorts after the
                # axes in the vendored hit priority, as it always did.
                out.append(tonicGizmoScreen.Handle(
                    "scale", "ring", None, ring, None,
                    self.sizeWorld * RING_FRACTION, RING_COLOR,
                    out[0].center, frontPoints=[ring],
                    worldCenter=self.origin, worldOrigin=self.origin,
                    sizePixels=out[0].sizePixels, handleId=HANDLE_RING))
        if self._allowedHandles is not None:
            out = [handle for handle in out
                   if handle.handleId in self._allowedHandles]
        if self.locked:
            # Drawn dimmed (LOCKED_OPACITY) and skipped by HitTest.
            for handle in out:
                handle.grabbable = False
        return out

    def screenHandles(self, camera, pixelRatio=None):
        """Drawable screen geometry shared by the transparent viewport layer.

        Each item has `kind`, `handle`, `points`, `color` and `grabbable`,
        plus `hovered` (the prehighlit handle, or the dragged one while a
        drag runs), `selected` (the last-dragged handle, kept after the
        release) and `active` (the handle being dragged).  Rings also carry
        `frontPoints`, the camera-side runs RigExec draws and picks.  The
        viewport renderer draws these records; handleAt picks the same
        vendored handles, so visual size and interaction cannot drift apart.

        The look rides along so the painter needs no gizmo knowledge:
        `sizePx` (the manipulator size in physical pixels, which the cone,
        cube and centre square scale with), `opacity` (LOCKED_OPACITY for
        an ungrabbable handle), `fillAlpha` (planes and the free ball) and,
        on axes, `tip`: "cone" for Move, "cube" for Scale, None for the
        node gizmo's bare lines.
        """
        handles = self.handles(camera, pixelRatio)
        if not handles:
            return []
        highlight = self.activeHandle if self._dragging else self.hoverHandle
        tip = self.tipKind()
        out = []
        for handle in handles:
            ident = handle.handleId
            dragged = self._dragging and ident == self.activeHandle
            hovered = ident == highlight and ident != HANDLE_NONE
            selected = ident == self.selectedHandle and \
                ident != HANDLE_NONE
            # RigExec's state precedence (gizmoUI._HandleColor): an
            # ungrabbable handle keeps its own colour (the overlay dims
            # it), the dragged handle is yellow, then the hover colour,
            # then the remembered handle, then the handle's own colour.
            color = handle.color
            if handle.grabbable:
                if dragged:
                    color = ACTIVE_COLOR
                elif hovered:
                    color = HOVER_COLOR
                elif selected:
                    color = ACTIVE_COLOR
            points = [(p[0], p[1]) for p in handle.points]
            if handle.kind in ("ring", "view") and len(points) > 2 and \
                    points[0] != points[-1]:
                points.append(points[0])      # a closed circle to draw
            record = {"kind": "free" if handle.kind == "sphere"
                      else handle.kind,
                      "handle": ident, "points": points, "color": color,
                      "grabbable": bool(handle.grabbable),
                      "hovered": hovered, "selected": selected,
                      "active": dragged,
                      "sizePx": float(handle.sizePixels),
                      "opacity": 1.0 if handle.grabbable else LOCKED_OPACITY}
            if handle.kind in ("ring", "view"):
                record["frontPoints"] = [[(p[0], p[1]) for p in run]
                                         for run in handle.frontPoints]
            if handle.kind == "sphere":
                record["radiusPx"] = handle.radiusPixels
                record["fillAlpha"] = SPHERE_FILL_OPACITY
            elif handle.kind == "plane":
                record["fillAlpha"] = PLANE_FILL_OPACITY
            elif handle.kind == "axis":
                record["tip"] = tip
            out.append(record)
        # Keep Tonic's established record order (the overlay paints in it
        # and callers index it): rotate rings, view, free; otherwise the
        # centre, the planes, the axes and the ringTRS scale ring.
        order = {"center": 0, "plane": 1, "axis": 2, "ring": 3, "view": 4,
                 "free": 5}
        if self.kind != GIZMO_ROTATE:
            out.sort(key=lambda record: order[record["kind"]])
        return out

    def tipKind(self):
        """What caps an axis: "cone" (Move), "cube" (Scale), None (node).

        RigExec tells Move and Scale apart by the tip alone (gizmoUI
        _DrawArrow / _DrawScaleAxis); the ringTRS gizmo moves along its
        axes, so it takes the cone too.
        """
        if self.kind == GIZMO_SCALE:
            return "cube"
        if self.kind == GIZMO_NODE_TRANSLATE:
            return None
        return "cone"

    def dragAngle(self):
        """Degrees the live rotate drag has swept (0.0 when not rotating).

        RigExec's DragAngle: the accumulated ring/view angle (it runs past
        180 in one sweep), or the free trackball's total turn.
        """
        import math
        if not self._dragging or self.kind != GIZMO_ROTATE:
            return 0.0
        if self.activeHandle == HANDLE_FREE:
            qw, qx, qy, qz = self._freeQuaternion
            magnitude = (qx * qx + qy * qy + qz * qz) ** 0.5
            return math.degrees(2.0 * math.atan2(magnitude, qw))
        return math.degrees(self._rotationTotal)

    def setDisplayAngle(self, degrees):
        """The snapped angle the target turned (None = follow the drag)."""
        self._displayAngle = None if degrees is None else float(degrees)

    def displayAngle(self):
        """Degrees the wedge shows: the snapped angle when Step Snap applied
        one (setDisplayAngle), else the raw swept dragAngle()."""
        if not self._dragging or self.kind != GIZMO_ROTATE:
            return 0.0
        if self._displayAngle is not None:
            return self._displayAngle
        return self.dragAngle()

    @property
    def startParameter(self):
        """Ring parameter (radians) of the press on a rotate ring."""
        return self._startParameter

    def pieSlice(self):
        """(polygon in physical pixels, rgb) of the rotation wedge, or None.

        the host application's rotation-amount pie (RigExec PieSlice): the centre plus the
        arc of the grabbed ring from where it was pressed through the swept
        angle, drawn in the ring's own colour.  Only ring and view drags
        have one; the free ball has no arc to fill.
        """
        handle = self._pressHandle
        if handle is None or not self._dragging or \
                self.kind != GIZMO_ROTATE:
            return None
        # The snapped angle when Step Snap is on, as RigExec's pie: the
        # wedge, the readout and the target all end on the same step.
        angle = self.displayAngle()
        if abs(angle) < 1e-6:
            return None
        polygon = tonicGizmoScreen.PiePolygon(handle, self._startParameter,
                                              angle)
        if len(polygon) < 3:
            return None
        return ([(p[0], p[1]) for p in polygon], handle.color)

    # -- hit testing -------------------------------------------------------

    def handleAt(self, camera, x, y, tolerancePx=None, pixelRatio=None):
        """The handle under pixel (x, y), or HANDLE_NONE.

        The vendored RigExec HitTest: resolution by KIND first -- centre,
        plane, axis, ring, view, free disc -- then the nearest within the
        tolerance, so the overlapping centre and squares stay reachable.
        Planes answer inside or within the tolerance of their outline,
        rotate rings only on their camera-side half, and foreshortened axes
        and edge-on planes are visible but ungrabbable so pixel travel
        cannot explode into a world move.  The tolerance is
        HANDLE_TOLERANCE_PX logical pixels times the device pixel ratio.
        """
        if camera is None or not self.visible:
            return HANDLE_NONE
        ratio = cameraPixelRatio(camera, pixelRatio)
        tolerance = (HANDLE_TOLERANCE_PX * ratio if tolerancePx is None
                     else float(tolerancePx))
        hit = tonicGizmoScreen.HitTest(self.handles(camera, ratio),
                                       float(x), float(y), tolerance)
        if hit is None or hit.handleId is None:
            return HANDLE_NONE
        return int(hit.handleId)

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
        self.selectedHandle = int(handle)
        self._selectedKind = self.kind
        self.hoverHandle = HANDLE_NONE
        self._pressX = float(x)
        self._pressY = float(y)
        self._pressDepth = projected[2]
        self._pressOrigin = self.origin
        self._lastRotationAngle = 0.0
        self._rotationTotal = 0.0
        self._displayAngle = None
        self._freeQuaternion = (1.0, 0.0, 0.0, 0.0)
        self._freeLast = (self._pressX, self._pressY)
        self._pressHandle = None
        self._startParameter = 0.0
        self._dragBuilt = None
        self._lastPlaneDelta = None
        for built in self.handles(camera):
            if built.handleId == self.activeHandle:
                self._dragBuilt = built
                break
        if self.kind == GIZMO_ROTATE and self._dragBuilt is not None and \
                self.activeHandle in (HANDLE_U, HANDLE_V, HANDLE_W,
                                      HANDLE_VIEW):
            self._pressHandle = self._dragBuilt
            self._startParameter = tonicGizmoScreen.RingParameter(
                self._dragBuilt, (self._pressX, self._pressY))
        self._dragging = True
        return True

    def _cameraPlaneDelta(self, camera, x, y):
        """The press-depth camera-plane delta: the cursor, exactly."""
        here = camera.pixelsToWorld(float(x), float(y), self._pressDepth)
        there = camera.pixelsToWorld(self._pressX, self._pressY,
                                     self._pressDepth)
        if here is None or there is None:
            return (0.0, 0.0, 0.0)
        return _sub(here, there)

    def _planeDelta(self, camera, normal, x, y):
        """A drag in the plane through the press origin with `normal`.

        RigExec gizmoDrag.PlaneDelta: the vendored ray/plane intersection
        keeps the grabbed point under the cursor as the plane recedes, and
        it falls back to the camera plane when the plane is edge-on.  A
        plane tipping towards edge-on still sends the intersection towards
        infinity, so a result far larger than the always-bounded
        camera-plane delta for the same travel is a miss and the last good
        delta stands (PLANE_DELTA_SANITY).
        """
        reference = self._cameraPlaneDelta(camera, x, y)
        if not hasattr(camera, "rayThrough"):
            return reference
        delta = tonicGizmoScreen.RayPlaneDragDelta(
            camera, self._pressOrigin, normal,
            (self._pressX, self._pressY), (float(x), float(y)))
        limit = max(_length(reference), 1e-9) * PLANE_DELTA_SANITY
        if self._lastPlaneDelta is not None and _length(delta) > limit:
            return self._lastPlaneDelta
        self._lastPlaneDelta = delta
        return delta

    def drag(self, camera, x, y, ctrl=False):
        """The world delta from the press for the live handle.

        Axis handles project the pixel travel onto the axis's press-time
        screen span (the vendored AxisDragParameter, floored at
        MIN_AXIS_PIXELS logical pixels), which is the host application feel: the handle
        follows the cursor's component along the axis and ignores the rest.
        With `ctrl` -- read on EVERY sample, as RigExec's DragState does --
        an axis moves in the plane PERPENDICULAR to it instead (parity G07).
        A planar square moves in its own plane, keeping the grabbed point
        under the cursor; the centre unprojects at the press depth.
        """
        if not self._dragging or camera is None:
            return (0.0, 0.0, 0.0)
        handle = self.activeHandle
        if handle in (HANDLE_U, HANDLE_V, HANDLE_W):
            axis = _unit(self.axis(handle))
            if axis is None:
                return (0.0, 0.0, 0.0)
            if ctrl:
                return self._planeDelta(camera, axis, x, y)
            built = self._dragBuilt
            if built is None or built.kind != "axis" or \
                    built.handleId != handle:
                return (0.0, 0.0, 0.0)
            travel = tonicGizmoScreen.AxisDragParameter(
                built, (self._pressX, self._pressY), (float(x), float(y)),
                cameraPixelRatio(camera))
            return _scale(axis, travel * built.worldLength)
        planeNormals = {
            HANDLE_PLANE_YZ: self.axis(HANDLE_U),
            HANDLE_PLANE_XZ: self.axis(HANDLE_V),
            HANDLE_PLANE_XY: self.axis(HANDLE_W),
        }
        normal = _unit(planeNormals.get(handle, (0.0, 0.0, 0.0)))
        if normal is not None:
            return self._planeDelta(camera, normal, x, y)
        return self._cameraPlaneDelta(camera, x, y)

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
        """(worldAxis, radians) for the live the host application rotation handle."""
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

    def scaleFactor(self, camera, x, y, allowNegative=True):
        """The scale factor from the press for the live Scale handle.

        Axis and plane handles are RigExec's MayaScaleFactor (vendored):
        the cursor's distance from the pivot along the press-time handle
        now over at the press, so dragging onto the pivot gives 0 and
        carrying on through it mirrors -- unless `allowNegative` is False
        (Prevent Negative Scale), which clamps at MIN_SCALE_FACTOR.

        The centre is uniform scale and has no direction of its own, so it
        is MayaScaleFactor's centre rule too (parity G08): 1 + dx / size,
        horizontal travel against the manipulator's pixel size -- right
        grows, left shrinks, one manipulator size of travel is 2x, vertical
        travel does nothing -- and carrying on left past 0 mirrors unless
        Prevent Negative Scale clamps it at MIN_SCALE_FACTOR, exactly as
        the axes and planes do.
        """
        if not self._dragging or self.kind != GIZMO_SCALE:
            return 1.0
        centre = camera.worldToPixels(self._pressOrigin)
        if centre is None:
            return 1.0
        built = self._dragBuilt
        if built is None or built.handleId != self.activeHandle or \
                built.kind not in ("axis", "plane", "center"):
            return 1.0
        return tonicGizmoScreen.MayaScaleFactor(
            built, (centre[0], centre[1]), (self._pressX, self._pressY),
            (float(x), float(y)), bool(allowNegative))

    def end(self):
        # `selectedHandle` deliberately survives: RigExec keeps the last
        # dragged handle lit and a middle drag repeats it.
        self.activeHandle = HANDLE_NONE
        self._dragging = False
        self._displayAngle = None
        self._pressHandle = None
        self._dragBuilt = None
        self._lastPlaneDelta = None

    def paintSignature(self):
        """A cheap key of everything the drawn gizmo depends on but the camera.

        The viewport paints from cached records and rebuilds them only when
        this key or the camera changes (RigExec rebuilds its handles on
        frustum/resize/selection events, never per paint).  The drag angle
        is in it because the rotation wedge grows with every sample.
        """
        if not self.visible:
            return (GIZMO_NONE,)
        return (self.kind, self.origin, self.frame, self.sizeWorld,
                self.activeHandle, self.hoverHandle, self.selectedHandle,
                self._dragging, self._allowedHandles, self._rotationTotal,
                self._displayAngle, self._freeQuaternion, self.freeRotate,
                self.locked)

    # -- publication -------------------------------------------------------

    def abiHandle(self):
        """`activeHandle` as the ABI spells it: ids 0-9 pass through
        unchanged (they ARE tonicGizmo.h's TonicGizmoHandle values, the
        centre, planes, view ring and free ball included); anything else,
        HANDLE_NONE among them, is -1."""
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
        # GZ-06: a restricted placement hands its whitelist to the Hydra
        # fallback, so a ring's inert handles are hidden there too.  An
        # unrestricted one keeps the original entry (and a DLL that
        # predates Tonic_SetGizmoEx still draws every handle).
        mask = self.allowedMask()
        entry = (_setGizmoEx(session.dll) if mask != 0xFFFFFFFF else None)
        if entry is not None:
            return entry(model, int(self.kind), origin, frame,
                         ctypes.c_float(self.sizeWorld),
                         int(self.abiHandle()),
                         ctypes.c_uint(mask)) == tonicLib.TONIC_OK
        return session.dll.Tonic_SetGizmo(
            model, int(self.kind), origin, frame,
            ctypes.c_float(self.sizeWorld),
            int(self.abiHandle())) == tonicLib.TONIC_OK


def _setGizmoEx(dll):
    """Tonic_SetGizmoEx with its argtypes bound, or None when absent.

    Bound here rather than in tonicLib so an older staged DLL (no Ex
    entry) degrades to Tonic_SetGizmo instead of failing to load.
    """
    import ctypes
    try:
        entry = getattr(dll, "Tonic_SetGizmoEx")
    except AttributeError:
        return None
    if getattr(entry, "argtypes", None) is None:
        try:
            entry.argtypes = [ctypes.c_void_p, ctypes.c_int,
                              ctypes.POINTER(ctypes.c_float),
                              ctypes.POINTER(ctypes.c_float),
                              ctypes.c_float, ctypes.c_int, ctypes.c_uint]
            entry.restype = ctypes.c_int
        except (AttributeError, TypeError):
            pass
    return entry


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


def normalFrame(normal):
    """A right-handed u/v/w frame whose w is `normal`, or None.

    The Tube orientation's frame (GZ-05, RigExec "Object"): w runs along
    the owner tube's root normal and u is the world axis least parallel to
    it with the normal removed (ties X before Y before Z, the same rule as
    tonicGizmoSnap._PlaneBasis), so the frame depends on the tube alone
    and holds still while the camera orbits.
    """
    w = _unit(tuple(float(value) for value in normal)) \
        if normal is not None else None
    if w is None:
        return None
    worldAxes = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    dots = [sum(a * b for a, b in zip(axis, w)) for axis in worldAxes]
    pick = min(range(3), key=lambda i: abs(dots[i]))
    u = _unit(_sub(worldAxes[pick], _scale(w, dots[pick])))
    if u is None:
        return None
    v = _unit(_cross(w, u))
    if v is None:
        return None
    return u + v + w


def orientationFrame(orientation, camera, origin, normal=None):
    """The gizmo frame an Axis Orientation asks for at `origin`.

    "world" is the identity (red/green/blue are world X/Y/Z); "screen" is
    the camera plane (screenFrame); "tube" is normalFrame(`normal`), the
    owner tube's root-normal frame, and falls back to World when the tube
    has no normal to give.  Anything unknown is World.
    """
    value = str(orientation).lower()
    if value == "screen":
        return screenFrame(camera, origin)
    if value == "tube":
        frame = normalFrame(normal)
        return frame if frame is not None else IDENTITY_FRAME
    return IDENTITY_FRAME


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _normalize(v):
    length = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5
    if length <= 0.0:
        return (0.0, 0.0, 0.0)
    return (v[0] / length, v[1] / length, v[2] / length)
