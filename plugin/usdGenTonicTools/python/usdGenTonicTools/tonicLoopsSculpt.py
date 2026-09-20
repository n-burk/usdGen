# usdGenTonicTools.tonicLoopsSculpt -- Sculpt mode's viewport loop
# (plan/18 section 3.6 SculptLoop, plan/17 section 5.5).
#
# Qt-free. The loop's whole job is to turn a drag into ONE stroke call per
# move: the brush ring follows the cursor, the press picks the center-CV
# footprint inside the brush radius, and every move hands
# Tonic_SculptStrokeShaped the stroke -- a world delta, a scalar, the
# cursor and the radius -- while the model does the falloff, the brush
# shape, the length preservation and the mirror (plan/18 section 7 G13).
# Nothing here weights a CV; that was the old contract and it is why comb
# and twist had no C++ at all.
#
# How a drag becomes each brush's scalar, once, here, so the panel's one
# "strength" number means something in every brush:
#
#   grab      the world delta between samples; CVs follow the cursor.
#   comb      the same direction, but a FIXED push per sample
#             (COMB_PUSH_PX pixels of world, times strength), so the
#             result does not depend on how fast the artist drags.
#   smooth    strength in [0, 1]: how far each CV goes toward the mean of
#             its neighbours.
#   lengthen  drag up to grow, down to shorten: a fraction of the tube's
#             length per DRAG_SPAN_PX pixels, times strength.
#   twist     drag right to twist: pi radians per DRAG_SPAN_PX pixels,
#             times strength.
from __future__ import annotations

import ctypes
import math

from . import tonicHierarchy
from . import tonicLib
from . import tonicLibStage
from . import tonicModes
from . import tonicSculpt
from .tonicLoops import ToolLoop

# Comb's fixed push, in pixels of world per move sample.
COMB_PUSH_PX = 4.0
# The drag length that means "one full unit" of lengthen or twist.
DRAG_SPAN_PX = 200.0
# The brush ring is drawn this many times the brush radius off the surface
# normal is unavailable; purely cosmetic.
RING_NORMAL = (0.0, 1.0, 0.0)


class SculptLoop(ToolLoop):
    """Brush ring, footprint pick, and one shaped stroke per move."""

    modeId = "sculpt"
    label = "Sculpt"
    pickMask = tonicLib.TONIC_PICK_CENTER_CV
    subModes = tonicModes.SCULPT_SUBMODES
    defaultSubMode = "grab"

    def __init__(self, session, state):
        super(SculptLoop, self).__init__(session, state)
        self._bound = False
        self._active = False
        self._tube = -1
        self._cv = -1
        self._tCenter = 0.5
        self._depth = 0.0
        self._lastXY = None
        self._touched = 0
        self._lastStatus = ""

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return self.state.sculptSubMode or self.defaultSubMode

    def setSubMode(self, subId):
        return tonicModes.SetActiveSculptSubMode(self.state, subId)

    def brush(self):
        return self.subMode()

    # -- status ------------------------------------------------------------

    def _status(self, text):
        self._lastStatus = text
        self.session.report(text)
        return text

    def statusLine(self):
        return tonicSculpt.sculptStatus(
            self.brush(), self.state.brushRadiusPx, self.state.brushTRadius,
            self.state.sculptPreserveLength, self.state.sculptMirrorX)

    # -- the brush ring ------------------------------------------------------

    def brushRadiusPx(self):
        return max(float(self.state.brushRadiusPx), 2.0)

    def _ringAt(self, sample):
        """Where the ring sits: the K1 surface hit, else the nearest CV."""
        hit = sample.surface()
        if hit is not None:
            return hit["point"], hit["normal"]
        item = sample.item(self.pickMask, self.brushRadiusPx())
        if item is None:
            return None, None
        point = self._cvPoint(int(item["id"]), int(item["subId"]))
        return point, RING_NORMAL

    def _setRing(self, sample):
        """Tonic_SetBrushRing under the cursor; False when it is nowhere."""
        session = self.session
        if session.model is None:
            return False
        point, normal = self._ringAt(sample)
        if point is None:
            session.dll.Tonic_SetBrushRing(session.model, None, None,
                                           ctypes.c_float(0.0))
            return False
        perPixel = (sample.camera.worldPerPixel(point)
                    if sample.camera is not None else 0.0)
        radius = max(perPixel * self.brushRadiusPx(), 1e-6)
        centre = (ctypes.c_float * 3)(*[float(v) for v in point])
        nrm = (ctypes.c_float * 3)(*[float(v) for v in
                                     (normal or RING_NORMAL)])
        session.dll.Tonic_SetBrushRing(session.model, centre, nrm,
                                       ctypes.c_float(radius))
        return True

    def _cvPoint(self, tubeId, cv):
        session = self.session
        try:
            return tonicHierarchy.tubeCenterCV(session.dll, session.model,
                                               tubeId, max(cv, 0))
        except (RuntimeError, NotImplementedError):
            return None

    # -- gesture --------------------------------------------------------------

    def press(self, sample):
        session = self.session
        if session.model is None:
            return False
        self._bind()
        item = sample.item(self.pickMask, self.brushRadiusPx())
        if item is None:
            self._setRing(sample)
            session.publish(tonicLib.TONIC_DIRTY_BRUSH)
            self._status("Tonic Sculpt: no center CV under the brush")
            return False
        self._tube = int(item["id"])
        self._cv = max(int(item["subId"]), 0)
        self._tCenter = self._tOf(self._tube, self._cv)
        point = self._cvPoint(self._tube, self._cv)
        projected = (sample.camera.worldToPixels(point)
                     if point is not None and sample.camera is not None
                     else None)
        self._depth = float(projected[2]) if projected is not None else 0.0
        self._lastXY = (sample.x, sample.y)
        self._touched = 0
        self._active = True
        session.beginGesture("Sculpt %s" % self.brush())
        self._setRing(sample)
        session.publish(tonicLib.TONIC_DIRTY_BRUSH)
        return True

    def move(self, sample):
        if not self._active or self.session.model is None:
            return False
        last = self._lastXY
        self._lastXY = (sample.x, sample.y)
        if last is None:
            return True
        self._stroke(sample, last)
        self._setRing(sample)
        return True

    def release(self, sample):
        if not self._active:
            return False
        self._active = False
        self._lastXY = None
        self.session.endGesture()
        self.session.publish()
        self.session.enqueueCommit()
        self._status("Tonic Sculpt: %s over tube %d (%d CV move(s))"
                     % (tonicSculpt.brushLabel(self.brush()), self._tube,
                        self._touched))
        return True

    def cancel(self):
        """Escape: the press-time base comes back bit-exactly."""
        if not self._active:
            return False
        self._active = False
        self._lastXY = None
        dirty = self.session.cancelGesture()
        self.session.publish(dirty)
        self._status("Tonic Sculpt: cancelled")
        return True

    def hover(self, sample):
        if self.session.model is None:
            return False
        if self._setRing(sample):
            self.session.publish(tonicLib.TONIC_DIRTY_BRUSH)
        return False

    def deactivate(self):
        """Leaving Sculpt takes the ring with it (plan/18 section 2.4)."""
        session = self.session
        if session.model is None:
            return False
        session.dll.Tonic_SetBrushRing(session.model, None, None,
                                       ctypes.c_float(0.0))
        session.publish(tonicLib.TONIC_DIRTY_BRUSH)
        return True

    # -- the stroke ------------------------------------------------------------

    def _bind(self):
        """Bind the V5 stage entries onto the session's handle, once."""
        if not self._bound:
            tonicLibStage.bindV5(self.session.dll)
            self._bound = True
        return self._bound

    def _tOf(self, tubeId, cv):
        try:
            count = tonicHierarchy.tubeCenterCount(self.session.dll,
                                                   self.session.model,
                                                   tubeId)
        except (RuntimeError, NotImplementedError):
            return 0.5
        return float(cv) / float(count - 1) if count > 1 else 0.0

    def _worldDelta(self, camera, fromXY, toXY):
        """The world travel between two pixels at the press-time depth."""
        if camera is None:
            return (0.0, 0.0, 0.0)
        a = camera.pixelsToWorld(fromXY[0], fromXY[1], self._depth)
        b = camera.pixelsToWorld(toXY[0], toXY[1], self._depth)
        if a is None or b is None:
            return (0.0, 0.0, 0.0)
        return (b[0] - a[0], b[1] - a[1], b[2] - a[2])

    def strokeParams(self, camera, fromXY, toXY):
        """(deltaWorld, amount) for the active brush; pure arithmetic."""
        brush = self.brush()
        strength = max(float(getattr(self.state, "sculptStrength", 1.0)),
                       0.0)
        delta = self._worldDelta(camera, fromXY, toXY)
        if brush == "grab":
            return delta, 0.0
        if brush == "comb":
            point = self._cvPoint(self._tube, self._cv)
            perPixel = (camera.worldPerPixel(point)
                        if camera is not None and point is not None else 0.0)
            push = max(perPixel, 0.0) * COMB_PUSH_PX * strength
            return delta, push
        if brush == "smooth":
            return (0.0, 0.0, 0.0), min(strength, 1.0)
        if brush in ("lengthen", "shorten"):
            grow = (fromXY[1] - toXY[1]) / DRAG_SPAN_PX * strength
            return (0.0, 0.0, 0.0), grow
        # twist
        return ((0.0, 0.0, 0.0),
                (toXY[0] - fromXY[0]) / DRAG_SPAN_PX * math.pi * strength)

    def _stroke(self, sample, lastXY):
        session = self.session
        if self._tube < 0:
            return False
        delta, amount = self.strokeParams(sample.camera, lastXY,
                                          (sample.x, sample.y))
        if delta == (0.0, 0.0, 0.0) and amount == 0.0:
            return False
        try:
            moved = tonicLibStage.sculptStrokeShaped(
                session.dll, session.model, self._tube, self.brush(),
                sample.camera, sample.x, sample.y, self.brushRadiusPx(),
                delta, amount, self._tCenter,
                float(self.state.brushTRadius),
                bool(self.state.sculptPreserveLength),
                bool(self.state.sculptMirrorX))
        except RuntimeError:
            self._status("Tonic Sculpt: " + session.lastError())
            return False
        self._touched += int(moved)
        return True

    # -- keys ------------------------------------------------------------------

    def adjustRadius(self, delta):
        """`[` / `]`: the brush radius in pixels."""
        value = max(2.0, min(512.0, float(self.state.brushRadiusPx) +
                             float(delta)))
        self.state.brushRadiusPx = value
        return self._status("Tonic Sculpt: brush radius %.0f px" % value)
