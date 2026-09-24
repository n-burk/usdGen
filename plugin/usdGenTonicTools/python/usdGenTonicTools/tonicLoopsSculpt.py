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
from . import tonicLoopsTube
from . import tonicModes
from . import tonicSculpt
from .tonicLoops import ToolLoop

# Comb's fixed push, in pixels of world per move sample.
COMB_PUSH_PX = 4.0
# The drag length that means "one full unit" of lengthen or twist.
DRAG_SPAN_PX = 200.0
# The ring's normal only when no camera can say which way the view looks.
# A live ring always faces the camera (_cameraForward): a ring laid flat in
# world XZ reads as a thin ellipse, or a line, from a side view.
RING_NORMAL = (0.0, 1.0, 0.0)
# The radius range lives in tonicSculpt so the panel, [ ] and the F-drag
# share one clamp.  F-drag changes the *radius* shown by the panel and used
# by the brush, not a diameter masquerading as width.  Half a radius pixel
# per mouse pixel is responsive without making a short drag jump across the
# useful range.
BRUSH_RADIUS_MIN_PX = tonicSculpt.BRUSH_RADIUS_MIN_PX
BRUSH_RADIUS_MAX_PX = tonicSculpt.BRUSH_RADIUS_MAX_PX
BRUSH_RESIZE_RADIUS_PER_PX = 0.5


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
        self._camera = None
        self._planeNormal = RING_NORMAL
        self._planePerPixel = 0.0
        self._lastXY = None
        self._pressXY = None
        self._pressStampXY = None
        self._strokeAnchorXY = None
        self._touched = 0
        self._storedPreview = None
        self._lastStatus = ""
        self._resizingRadius = False
        self._resizeStartXY = None
        self._resizeStartRadius = 0.0
        self._resizeRadius = 0.0
        self._resizeLastSample = None
        self._lastResizeReport = None
        # The idle ring: whether one is on screen, the anchor CV it hovers
        # (kind, tube, cv) and the last hover sample, so [ ] can redraw it.
        self._ringShown = False
        self._ringHover = None
        self._hoverKey = None
        self._lastHoverSample = None

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
            self.state.sculptPreserveLength, self.state.sculptMirrorX,
            strength=getattr(self.state, "sculptStrength", 1.0))

    # -- the brush ring ------------------------------------------------------

    def brushRadiusPx(self):
        value = (self._resizeRadius if self._resizingRadius
                 else float(self.state.brushRadiusPx))
        return max(float(value), BRUSH_RADIUS_MIN_PX)

    @staticmethod
    def _cameraForward(camera):
        """The view direction: the ray through the frame centre.

        One direction for the whole view.  A perspective ray through the
        cursor tilts toward the frame edge, so using it would turn the ring
        as the cursor travels.
        """
        ray = (camera.rayThrough(camera.width * 0.5, camera.height * 0.5)
               if camera is not None else None)
        if ray is None:
            return RING_NORMAL
        return tuple(float(v) for v in ray[1])

    def _ringAt(self, sample):
        """(centre, normal, hover) of the idle ring; centre None off-target.

        The ring faces the camera and sits exactly under the cursor.  Over a
        tube (or near a selected one, where a press would still stroke) it
        takes the depth of the anchor CV the press would stroke, and that CV
        is the hover prehighlight: the ring reads at the tube's distance but
        follows the cursor continuously instead of snapping CV to CV.  Over
        the scalp it sits on the hit point.  A sculpt brush operates on
        centre curves, so the tube wins over the scalp ray behind it.
        """
        camera = sample.camera
        normal = self._cameraForward(camera)
        item = self._brushItem(sample)
        if item is None:
            item = self._selectedAnchor(sample)
        if item is not None and camera is not None:
            tubeId = int(item["id"])
            cv, projected = self._centerAnchor(tubeId, camera, sample.x,
                                               sample.y, int(item["subId"]))
            if projected is None:
                point = self._cvPoint(tubeId, cv)
                projected = (camera.worldToPixels(point)
                             if point is not None else None)
            if projected is not None:
                centre = camera.pixelsToWorld(sample.x, sample.y,
                                              float(projected[2]))
                if centre is not None:
                    return (centre, normal,
                            (tonicLib.TONIC_PICK_CENTER_CV, tubeId, int(cv)))
        hit = sample.surface()
        if hit is not None:
            return hit["point"], normal, None
        return None, None, None

    def _setHoverItem(self, key):
        """Prehighlight the ring's anchor CV; True when the target changed.

        Tonic_SetHover always succeeds, so the loop remembers what it set
        and only republishes the selection prim when the CV changes.
        """
        key = tuple(key) if key is not None else None
        if key == self._hoverKey:
            return False
        self._hoverKey = key
        try:
            if key is None:
                self.session.setHover(0, -1, -1, -1)
            else:
                self.session.setHover(key[0], key[1], key[2], -1)
        except (AttributeError, RuntimeError):
            return False
        return True

    def _centerAnchor(self, tubeId, camera, x, y, preferred=-1):
        """Nearest projected center CV, preserving an exact component hit.

        A body pick carries only its owning tube.  Convert it to a center
        anchor locally so the shaped brush has a real footprint even when a
        side-wall press is farther than its radius from the center line.
        """
        try:
            count = tonicHierarchy.tubeCenterCount(self.session.dll,
                                                   self.session.model,
                                                   tubeId)
        except (RuntimeError, NotImplementedError):
            return max(int(preferred), 0), None
        if count <= 0:
            return 0, None
        if 0 <= int(preferred) < count:
            point = self._cvPoint(tubeId, int(preferred))
            projected = (camera.worldToPixels(point)
                         if point is not None and camera is not None else None)
            return int(preferred), projected
        # A side-wall click must anchor an editable CV.  Roots are pinned by
        # sculpt, so choosing CV0 on a short tube makes an otherwise valid
        # body stroke a no-op.
        first = 1 if int(preferred) < 0 and count > 1 else 0
        best = (float("inf"), first, None)
        for cv in range(first, count):
            point = self._cvPoint(tubeId, cv)
            projected = (camera.worldToPixels(point)
                         if point is not None and camera is not None else None)
            if projected is None:
                continue
            d2 = ((float(projected[0]) - float(x)) ** 2 +
                  (float(projected[1]) - float(y)) ** 2)
            if d2 < best[0]:
                best = (d2, cv, projected)
        return best[1], best[2]

    def _brushItem(self, sample):
        """The precise CV first, then the body owner of a visible tube.

        The body fallback is deliberately second.  A center-CV click keeps
        its exact component identity; a broad side-wall click becomes the
        backend's nearest centre-curve anchor instead of failing outright.
        """
        item = sample.item(self.pickMask, self.brushRadiusPx())
        if item is not None:
            return item
        return sample.item(tonicLib.TONIC_PICK_TUBE_VERT,
                           self.brushRadiusPx())

    def _viewPlanePoint(self, sample):
        """Cursor point on the press-time camera plane, never a ray hit."""
        camera = self._camera if self._active else sample.camera
        if camera is None:
            return None
        return camera.pixelsToWorld(sample.x, sample.y, self._depth)

    def _setRing(self, sample):
        """Tonic_SetBrushRing under the cursor; False when it is nowhere."""
        session = self.session
        if session.model is None:
            return False
        if self._active:
            # A drag owns its press-time camera plane.  Do not raycast, pick,
            # or look at the changing tube while it is live: the overlay and
            # the brush must carry through empty background together.
            point, normal = self._viewPlanePoint(sample), self._planeNormal
            self._ringHover = None
        else:
            point, normal, self._ringHover = self._ringAt(sample)
        if point is None:
            session.dll.Tonic_SetBrushRing(session.model, None, None,
                                           ctypes.c_float(0.0))
            self._ringShown = False
            return False
        camera = self._camera if self._active else sample.camera
        perPixel = (camera.worldPerPixel(point) if camera is not None else 0.0)
        radius = max(perPixel * self.brushRadiusPx(), 1e-6)
        centre = (ctypes.c_float * 3)(*[float(v) for v in point])
        nrm = (ctypes.c_float * 3)(*[float(v) for v in
                                     (normal or RING_NORMAL)])
        session.dll.Tonic_SetBrushRing(session.model, centre, nrm,
                                       ctypes.c_float(radius))
        self._ringShown = True
        return True

    def _cvPoint(self, tubeId, cv):
        """Return the displayed core handle used for sculpt UI anchoring.

        The authored center CV can sit away from an asymmetric section's
        visible core.  Keep raw centers in the native deformation path, but
        anchor the cursor footprint, ring and frozen view depth to the same
        handle position the artist sees.
        """
        session = self.session
        try:
            return tonicHierarchy.tubeCenterHandle(session.dll, session.model,
                                                   tubeId, max(cv, 0))
        except (RuntimeError, NotImplementedError):
            return None

    def _selectedAnchor(self, sample):
        """A selected owner under this screen footprint, if one exists.

        Empty space never guesses a tube.  It can continue an intentional
        selection only when that selection's projected editable center lies
        under the brush, which is useful while center display is hidden by a
        Sculpt draw mode or an occluding scalp is in front of the curve.
        """
        read = getattr(self.session, "readSelection", None)
        if not callable(read) or sample.camera is None:
            return None
        owners = set()
        for kind in (tonicLib.TONIC_PICK_TUBE_VERT,
                     tonicLib.TONIC_PICK_CENTER_CV,
                     tonicLib.TONIC_PICK_SECTION_CV,
                     tonicLib.TONIC_PICK_SECTION_RING):
            try:
                owners.update(int(entry[0]) for entry in (read(kind) or ()))
            except (IndexError, TypeError, ValueError, RuntimeError):
                continue
        radius2 = self.brushRadiusPx() ** 2
        best = None
        for tubeId in sorted(owners):
            cv, projected = self._centerAnchor(tubeId, sample.camera,
                                                sample.x, sample.y)
            if projected is None:
                continue
            d2 = ((float(projected[0]) - float(sample.x)) ** 2 +
                  (float(projected[1]) - float(sample.y)) ** 2)
            if d2 <= radius2 and (best is None or d2 < best[0]):
                best = (d2, tubeId, cv)
        if best is None:
            return None
        return {"kind": tonicLib.TONIC_PICK_CENTER_CV,
                "id": best[1], "subId": best[2], "subSubId": -1}

    # -- gesture --------------------------------------------------------------

    def press(self, sample):
        session = self.session
        if session.model is None:
            return False
        self._bind()
        item = self._brushItem(sample)
        if item is None:
            item = self._selectedAnchor(sample)
        if item is None:
            # An honest miss: say so and open nothing.  The loop does not
            # claim the press (no gesture, no bracket); the controller still
            # swallows it as a Tonic click, so usdview never picks the prim
            # under a brush that missed.
            self._setRing(sample)
            session.publish(tonicLib.TONIC_DIRTY_BRUSH)
            self._status("Tonic Sculpt: no tube under the brush -- press on "
                         "a tube, or grow the brush with ] or F-drag")
            return False
        self._tube = int(item["id"])
        self._cv, anchorXY = self._centerAnchor(
            self._tube, sample.camera, sample.x, sample.y,
            int(item["subId"]))
        self._strokeAnchorXY = (anchorXY if
                                int(item["kind"]) ==
                                tonicLib.TONIC_PICK_TUBE_VERT else None)
        self._tCenter = self._tOf(self._tube, self._cv)
        point = self._cvPoint(self._tube, self._cv)
        projected = (sample.camera.worldToPixels(point)
                     if point is not None and sample.camera is not None
                     else None)
        self._depth = float(projected[2]) if projected is not None else 0.0
        self._camera = sample.camera
        planePoint = self._viewPlanePoint(sample)
        # A view plane has one camera-forward normal, the same one the idle
        # ring used, so the ring does not turn as the press lands.
        self._planeNormal = self._cameraForward(sample.camera)
        self._planePerPixel = (sample.camera.worldPerPixel(planePoint)
                               if planePoint is not None and
                               sample.camera is not None else 0.0)
        self._lastXY = (sample.x, sample.y)
        self._pressXY = self._lastXY
        self._pressStampXY = ((anchorXY[0], anchorXY[1])
                              if anchorXY is not None else self._pressXY)
        self._touched = 0
        if not session.beginGesture(
                "Sculpt %s" % tonicSculpt.brushLabel(self.brush())):
            self._pressXY = None
            self._pressStampXY = None
            self._strokeAnchorXY = None
            self._camera = None
            return False
        self._active = True
        self._storedPreview = tonicLoopsTube.previewGuides(session,
                                                            self.state)
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
        travelled = (self._pressXY is not None and
                     (float(sample.x), float(sample.y)) !=
                     (float(self._pressXY[0]), float(self._pressXY[1])))
        self._active = False
        self._lastXY = None
        self._pressXY = None
        self._pressStampXY = None
        self._strokeAnchorXY = None
        self._camera = None
        if not self._touched:
            # A click, or a drag that moved no CV, changed nothing: drop the
            # bracket rather than seal an empty undo step that the next
            # Ctrl+Z would silently spend.
            dirty = self.session.cancelGesture()
            tonicLoopsTube.restoreGuides(self.session, self._storedPreview,
                                         refill=False)
            self._storedPreview = None
            self.session.publish(dirty)
            self._status(
                "Tonic Sculpt: %s %s -- nothing changed (drag to sculpt)"
                % (tonicSculpt.brushLabel(self.brush()),
                   "moved no CVs" if travelled else "click without a drag"))
            return True
        self.session.endGesture()
        tonicLoopsTube.restoreGuides(self.session, self._storedPreview,
                                     refill=bool(self._touched))
        self._storedPreview = None
        self.session.publish()
        if self._touched:
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
        self._pressXY = None
        self._pressStampXY = None
        self._strokeAnchorXY = None
        self._camera = None
        dirty = self.session.cancelGesture()
        # The gesture snapshot restored the model, including its guide cache.
        # Only put the preview-fraction control back; a refill here would
        # invent a preview on a previously empty/frozen groom.
        tonicLoopsTube.restoreGuides(self.session, self._storedPreview,
                                     refill=False)
        self._storedPreview = None
        self.session.publish(dirty)
        self._status("Tonic Sculpt: cancelled")
        return True

    def hover(self, sample):
        if self.session.model is None:
            return False
        self._lastHoverSample = sample
        wasShown = self._ringShown
        shown = self._setRing(sample)
        dirty = 0
        if shown or wasShown:
            dirty |= tonicLib.TONIC_DIRTY_BRUSH
        if not self._active and self._setHoverItem(self._ringHover):
            dirty |= tonicLib.TONIC_DIRTY_SELECTION
        if dirty:
            self.session.publish(dirty)
        return False

    def clearHover(self):
        """The pointer left the view: the idle ring and its CV hover go.

        A live stroke or F-drag keeps its ring (a captured drag may leave
        the view and come back); only an idle ring is cleared.
        """
        session = self.session
        self._lastHoverSample = None
        if self._active or self._resizingRadius or session.model is None:
            return False
        session.dll.Tonic_SetBrushRing(session.model, None, None,
                                       ctypes.c_float(0.0))
        self._ringShown = False
        self._ringHover = None
        dirty = tonicLib.TONIC_DIRTY_BRUSH
        if self._setHoverItem(None):
            dirty |= tonicLib.TONIC_DIRTY_SELECTION
        session.publish(dirty)
        return True

    def deactivate(self):
        """Leaving Sculpt takes the ring with it (plan/18 section 2.4)."""
        if self._resizingRadius:
            self.cancelRadiusResize()
        session = self.session
        self._lastHoverSample = None
        if session.model is None:
            return False
        session.dll.Tonic_SetBrushRing(session.model, None, None,
                                       ctypes.c_float(0.0))
        self._ringShown = False
        self._ringHover = None
        self._setHoverItem(None)
        session.publish(tonicLib.TONIC_DIRTY_BRUSH |
                        tonicLib.TONIC_DIRTY_SELECTION)
        return True

    # -- F + LMB brush width -------------------------------------------------

    @property
    def resizingRadius(self):
        return self._resizingRadius

    def beginRadiusResize(self, sample):
        """Start a UI-only brush-radius drag; never opens a sculpt bracket."""
        if self._active or self._resizingRadius or self.session.model is None:
            return False
        self._resizeStartXY = (float(sample.x), float(sample.y))
        self._resizeStartRadius = max(float(self.state.brushRadiusPx),
                                      BRUSH_RADIUS_MIN_PX)
        self._resizeRadius = self._resizeStartRadius
        self._resizingRadius = True
        self._resizeLastSample = sample
        self._lastResizeReport = None
        self._setRing(sample)
        self.session.publish(tonicLib.TONIC_DIRTY_BRUSH)
        return True

    def resizeRadius(self, sample):
        """Apply the absolute horizontal F-drag distance to brush radius."""
        if not self._resizingRadius:
            return False
        start = self._resizeStartXY
        if start is None:
            return False
        value = self._resizeStartRadius + (
            float(sample.x) - start[0]) * BRUSH_RESIZE_RADIUS_PER_PX
        value = tonicSculpt.clampBrushRadius(value)
        self._resizeRadius = value
        self._resizeLastSample = sample
        self._setRing(sample)
        # A live readout per sample, but only when the whole-pixel value
        # changes, so a slow drag does not flood the status bar.
        report = int(round(value))
        if report != self._lastResizeReport:
            self._lastResizeReport = report
            self._status("Tonic Sculpt: brush radius %d px (release to set)"
                         % report)
        return True

    def _finishRadiusResize(self, sample=None, cancelled=False):
        if not self._resizingRadius:
            return False
        if sample is not None and not cancelled:
            self.resizeRadius(sample)
        if not cancelled:
            # The panel/state sees one complete user width change per drag;
            # the transient value above exists solely for the live ring.
            self.state.brushRadiusPx = self._resizeRadius
        last = sample or self._resizeLastSample
        self._resizingRadius = False
        self._resizeStartXY = None
        self._resizeRadius = 0.0
        self._resizeLastSample = None
        self._lastResizeReport = None
        # Draw only after dropping the transient mode.  Escape/capture
        # cancellation must put the ring back at the committed start radius,
        # not leave the last preview radius until the next hover.
        if last is not None:
            self._setRing(last)
        self.session.publish(tonicLib.TONIC_DIRTY_BRUSH)
        if cancelled:
            self._status("Tonic Sculpt: brush resize cancelled")
        else:
            self._status("Tonic Sculpt: brush radius %.0f px" %
                         self.brushRadiusPx())
        return True

    def endRadiusResize(self, sample):
        return self._finishRadiusResize(sample, cancelled=False)

    def cancelRadiusResize(self):
        return self._finishRadiusResize(cancelled=True)

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
        camera = self._camera if self._active and self._camera is not None else camera
        delta = self._worldDelta(camera, fromXY, toXY)
        if brush == "grab":
            return tuple(v * strength for v in delta), 0.0
        if brush == "comb":
            perPixel = self._planePerPixel
            push = max(perPixel, 0.0) * COMB_PUSH_PX * strength
            return delta, push
        if brush == "smooth":
            return (0.0, 0.0, 0.0), min(strength, 1.0)
        if brush == "lengthen":
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
        # Stamp at the preceding sample.  On the first move that is the
        # press footprint, so a body hit gets one valid center-curve stamp
        # before a fast cursor leaves its small radius.  Subsequent samples
        # retain the usual contiguous brush trail.
        x, y = lastXY
        if self.brush() == "grab" and self._pressStampXY is not None:
            # Native Grab shapes against the press-time gesture base.  Keep
            # its screen footprint fixed while incremental plane deltas carry
            # the result across empty background.
            x, y = self._pressStampXY
        elif self._strokeAnchorXY is not None and self._pressXY is not None:
            x = self._strokeAnchorXY[0] + lastXY[0] - self._pressXY[0]
            y = self._strokeAnchorXY[1] + lastXY[1] - self._pressXY[1]
        try:
            moved = tonicLibStage.sculptStrokeShaped(
                session.dll, session.model, self._tube, self.brush(),
                self._camera or sample.camera, x, y, self.brushRadiusPx(),
                delta, amount, self._tCenter,
                float(self.state.brushTRadius),
                bool(self.state.sculptPreserveLength),
                bool(self.state.sculptMirrorX))
        except RuntimeError:
            self._status("Tonic Sculpt: " + session.lastError())
            return False
        self._touched += int(moved)
        if moved:
            tonicLoopsTube.refillPreview(session, self.state)
        return True

    # -- keys ------------------------------------------------------------------

    def adjustRadius(self, delta):
        """`[` / `]`: scale the brush radius (tonicSculpt.steppedBrushRadius).

        The sign of `delta` picks grow or shrink.  A magnitude below 1 is
        the fine step (Shift+[ / Shift+] once the hotkey table sends +/-0.5);
        the plain keys send +/-1.
        """
        delta = float(delta)
        value = tonicSculpt.steppedBrushRadius(
            self.state.brushRadiusPx, delta, fine=0.0 < abs(delta) < 1.0)
        self.state.brushRadiusPx = value
        # Redraw the idle ring at its new size now, not on the next move.
        sample = self._lastHoverSample
        if (sample is not None and not self._active and
                not self._resizingRadius and self.session.model is not None):
            try:
                if self._setRing(sample):
                    self.session.publish(tonicLib.TONIC_DIRTY_BRUSH)
            except (RuntimeError, AttributeError, TypeError):
                pass
        return self._status("Tonic Sculpt: brush radius %.0f px" % value)
