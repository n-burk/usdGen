# usdGenTonicTools.tonicLoopsTube -- Tube mode's viewport loop (plan/18
# section 3.6 TubeLoop, section 2.4, plan/17 section 5.2).
#
# Qt-free like every loop: pixels, modifiers and a camera in, C ABI calls
# out. What Tube mode owns is the shape of a tube -- its center CVs, its
# cross-section rings and the CVs of those rings -- and one gizmo family
# per sub-mode:
#
#   Center   click/marquee selects center CVs (or a whole tube through its
#            surface); a TRANSLATE gizmo on the selection bounds drags them
#            in the screen plane, along a handle's axis, or along the tube's
#            root normal while Ctrl is held.
#   Ring     click selects whole rings; a RING_TRS gizmo translates the ring
#            inside its own plane (handles u and v), scales it (the circle
#            handle) and TWISTS it (the w handle).
#   Section  click selects one ring CV; the same translate gizmo drags it,
#            always inside the ring plane.
#
# Three rules the code below exists to keep:
#
#   * a drag never touches the stage and never re-picks. The press resolves
#     the camera, the selection, the ring frames and the gizmo once; every
#     move is arithmetic plus one ABI call per moved item plus the publish
#     the controller does (plan/18 section 3.2).
#   * every mutation goes through the PER-TUBE ABI (plan/18 section 7 G2),
#     so the loop is already right for the day K11 reports a child tube
#     rather than always tube 0.
#   * the section chart is two dimensional. Tonic_MoveTubeSectionRing takes
#     (du, dv) in the ring's own plane, so a world delta has to be resolved
#     against that plane -- which is what Tonic_GetTubeSectionFrame answers
#     and tonicLibStage.worldToChart converts. A drag along the tube's
#     tangent has no chart spelling at all, which is why the w handle of a
#     ring gizmo twists instead of translating.
#
# Guides during a drag follow plan/17 section 4.2: the press drops the
# density to the panel's preview fraction, every move refills at it, and
# the release restores the fraction and refills at full.
from __future__ import annotations

import ctypes

from . import tonicBridge
from . import tonicGizmo
from . import tonicHierarchy
from . import tonicLib
from . import tonicModes
from .tonicLoops import ToolLoop

# The gizmo's on-screen size: axes this many pixels long, whatever the zoom
# (plan/18 section 2.4a). Maya's translate manipulator is about this long on
# a 1080p viewport, and it has to stay clear of the 8 px CV dots it sits
# among without covering the tube it moves.
GIZMO_PIXELS = 90.0
# A drag step smaller than this in world units is not worth an ABI call.
MIN_STEP = 1e-7
# `[` / `]` step for the soft-selection radius (in t).
SOFT_RADIUS_STEP = 0.05


def worldSizeForPixels(camera, origin, pixels=GIZMO_PIXELS):
    """World length that spans `pixels` on screen at `origin`."""
    if camera is None:
        return 1.0
    perPixel = camera.worldPerPixel(origin)
    if not perPixel > 0.0:
        return 1.0
    return max(perPixel * float(pixels), 1e-6)


def previewGuides(session, state):
    """Drop the guide density to the panel's preview fraction for a drag.

    Returns the fraction the model had, so the release can put it back
    (plan/17 section 4.2, section 5.3 "full-density refill on release").
    """
    model = session.model
    if model is None:
        return None
    stored = float(session.dll.Tonic_GetPreviewFraction(model))
    fraction = max(0.0, min(1.0, float(state.previewFraction)))
    session.dll.Tonic_SetPreviewFraction(model, ctypes.c_float(fraction))
    return stored


def refillPreview(session, state):
    """Refill at the preview fraction; False when there is nothing to fill."""
    model = session.model
    if model is None:
        return False
    guides = ctypes.c_int(0)
    session.dll.Tonic_GetGuideCounts(model, ctypes.byref(guides), None)
    if guides.value <= 0:
        return False                 # nothing filled yet: nothing to preview
    fraction = max(0.0, min(1.0, float(state.previewFraction)))
    return session.dll.Tonic_RefillGuides(
        model, ctypes.c_float(fraction)) == tonicLib.TONIC_OK


def restoreGuides(session, stored, refill=True):
    """Put the stored preview fraction back and refill at full density."""
    model = session.model
    if model is None:
        return False
    if stored is not None:
        session.dll.Tonic_SetPreviewFraction(model, ctypes.c_float(stored))
    if not refill:
        return False
    guides = ctypes.c_int(0)
    session.dll.Tonic_GetGuideCounts(model, ctypes.byref(guides), None)
    if guides.value <= 0:
        return False
    return session.dll.Tonic_RefillGuides(
        model, ctypes.c_float(1.0)) == tonicLib.TONIC_OK


class TubeLoop(ToolLoop):
    """Center / Ring / Section editing with one gizmo per sub-mode."""

    modeId = "tube"
    label = "Tube"
    subModes = tonicModes.TUBE_SUBMODES
    defaultSubMode = "center"

    # What a click asks K11 for, per sub-mode. The tube surface is in every
    # mask: clicking the body of a tube is how an artist selects the whole
    # thing, and it is the only candidate that exists before the CVs are
    # close enough to hit.
    KIND_MASKS = {
        "center": (tonicLib.TONIC_PICK_CENTER_CV |
                   tonicLib.TONIC_PICK_TUBE_VERT),
        "ring": (tonicLib.TONIC_PICK_SECTION_RING |
                 tonicLib.TONIC_PICK_TUBE_VERT),
        "section": (tonicLib.TONIC_PICK_SECTION_CV |
                    tonicLib.TONIC_PICK_SECTION_RING |
                    tonicLib.TONIC_PICK_TUBE_VERT),
    }

    def __init__(self, session, state):
        super(TubeLoop, self).__init__(session, state)
        self._gizmo = tonicGizmo.GizmoState()
        self._bracketOpen = False
        self._dragging = False       # a gizmo handle is under the cursor
        self._marquee = None         # (x0, y0) while a rubber band is live
        self._applied = (0.0, 0.0, 0.0)   # world delta already pushed
        self._appliedScale = 1.0
        self._appliedTwist = 0.0
        self._centerDrag = {}        # tubeId -> ([cv...], anchorCv)
        self._ringDrag = []          # [(tubeId, ring, frame)]
        self._sectionDrag = []       # [(tubeId, ring, slot, frame)]
        self._pickedCV = None        # (tubeId, cv) of the last CV clicked
        self._constrain = None       # world axis a modifier pinned us to
        self._storedPreview = None
        self._storedSoft = None
        self._pendingEdit = False
        self._lastStatus = ""

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return self.state.tubeSubMode or self.defaultSubMode

    def setSubMode(self, subId):
        status = tonicModes.SetActiveTubeSubMode(self.state, subId)
        if status:
            # The kinds a click means changed, so what is selected no longer
            # matches what the gizmo would drag.
            self.session.clearSelection(self._selectableMask())
            self._placeGizmo(None)
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return status

    @property
    def pickMask(self):
        return self.KIND_MASKS.get(self.subMode(), self.KIND_MASKS["center"])

    @property
    def _stage(self):
        """The per-tube ABI, re-read every time.

        A loop can outlive the moment the library loads (the shelf opens
        in Tube mode before Bind scalp), so caching this at construction
        would leave the whole mode inert for the rest of the session.
        """
        return getattr(self.session, "stageLib", None)

    def _selectableMask(self):
        """Every kind Tube mode may have put in the selection."""
        return (tonicLib.TONIC_PICK_TUBE_VERT |
                tonicLib.TONIC_PICK_CENTER_CV |
                tonicLib.TONIC_PICK_SECTION_CV |
                tonicLib.TONIC_PICK_SECTION_RING)

    # -- status ------------------------------------------------------------

    def _status(self, text):
        self._lastStatus = text
        self.session.report(text)

    def statusLine(self):
        from .tonicTube import tubeStatus
        dll, model = self.session.dll, self.session.model
        if model is None:
            return "Tube: no model."
        return tubeStatus(int(dll.Tonic_GetCenterCVCount(model)),
                          int(dll.Tonic_GetSectionCount(model)),
                          self._ringVerts(0),
                          int(dll.Tonic_GetTubeRegionId(model)))

    def _ringVerts(self, tubeId):
        section = self._section(tubeId, 0)
        return len(section[1]) if section else 0

    # -- reads -------------------------------------------------------------

    def _section(self, tubeId, ring):
        """One ring's chart: (t, [(u, v)], scale, twist), or None."""
        if self.session.model is None:
            return None
        try:
            return tonicBridge.tubeSection(self.session.dll,
                                           self.session.model, int(tubeId),
                                           int(ring))
        except (RuntimeError, NotImplementedError):
            return None

    def _ringFrame(self, tubeId, ring):
        """The ring's world placement, or None when the ABI has no answer."""
        if self._stage is None or self.session.model is None:
            return None
        return self._stage.sectionFrame(self.session.model, int(tubeId),
                                        int(ring))

    def _centers(self, tubeId):
        try:
            return tonicHierarchy.tubeCenters(self.session.dll,
                                              self.session.model, int(tubeId))
        except (RuntimeError, NotImplementedError):
            return []

    def _rootNormal(self, tubeId):
        """The direction the tube grows in: its root tangent."""
        centers = self._centers(tubeId)
        if len(centers) < 2:
            return None
        d = tuple(centers[1][i] - centers[0][i] for i in range(3))
        length = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) ** 0.5
        if not length > 1e-9:
            return None
        return (d[0] / length, d[1] / length, d[2] / length)

    def _selectionBounds(self):
        dll, model = self.session.dll, self.session.model
        if model is None:
            return None
        lo = (ctypes.c_float * 3)()
        hi = (ctypes.c_float * 3)()
        if dll.Tonic_GetSelectionBounds(model, lo, hi) != tonicLib.TONIC_OK:
            return None
        return ((lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2]))

    def _tubeCenterCount(self, tubeId):
        try:
            return int(tonicHierarchy.tubeCenterCount(self.session.dll,
                                                      self.session.model,
                                                      int(tubeId)))
        except (RuntimeError, NotImplementedError):
            return 0

    # -- the gizmo ---------------------------------------------------------

    def _placeGizmo(self, camera):
        """Put the gizmo on the selection, or take it away.

        The origin is the selection bounds' midpoint (plan/18 section 3.6:
        "a translate gizmo appears at the selection bounds"); the frame is
        the ring's own plane in Ring/Section and the screen plane in
        Center, so a drag reads the way it looks.
        """
        bounds = self._selectionBounds()
        if bounds is None or self.session.model is None:
            self._gizmo.clear()
            self._gizmo.push(self.session)
            return False
        origin = tuple(0.5 * (bounds[0][i] + bounds[1][i]) for i in range(3))
        sub = self.subMode()
        kind = (tonicGizmo.GIZMO_RING_TRS if sub == "ring"
                else tonicGizmo.GIZMO_TRANSLATE)
        frame = None
        ring = self._firstSelectedRing()
        if ring is not None:
            placement = self._ringFrame(ring[0], ring[1])
            if placement is not None:
                frame = (placement["u"] + placement["v"] + placement["w"])
        if frame is None:
            frame = tonicGizmo.screenFrame(camera, origin)
        # Without a camera (a key action, not a click) the gizmo keeps the
        # size it had rather than jumping to a world-unit default.
        size = (worldSizeForPixels(camera, origin) if camera is not None
                else self._gizmo.sizeWorld)
        self._gizmo.place(origin, size, kind, frame)
        self._gizmo.push(self.session)
        return True

    def _firstSelectedRing(self):
        """(tubeId, ring) of the ring a section gizmo should align with."""
        rings = self.session.readSelection(tonicLib.TONIC_PICK_SECTION_RING)
        if rings:
            return (rings[0][0], rings[0][1])
        cvs = self.session.readSelection(tonicLib.TONIC_PICK_SECTION_CV)
        if cvs:
            return (cvs[0][0], cvs[0][1])
        return None

    # -- gesture -----------------------------------------------------------

    def press(self, sample):
        if self.session.model is None:
            return False
        self._pendingEdit = False
        self._constrain = None
        modifiers = sample.modifiers
        # Shift always extends the selection, even over a handle. Ctrl does
        # not: within the handle tolerance it is the normal constraint, and
        # everywhere else it toggles.
        handle = (tonicGizmo.HANDLE_NONE if sample.has("shift")
                  else self._gizmo.handleAt(sample.camera, sample.x,
                                            sample.y))
        if handle != tonicGizmo.HANDLE_NONE and self._beginDrag(sample,
                                                                handle):
            return True
        item = sample.item(self.pickMask, self.pickRadiusPx())
        if item is None:
            self._marquee = (sample.x, sample.y)
            return True
        self._selectItem(item, modifiers)
        self._placeGizmo(sample.camera)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        self._status(self._selectionStatus())
        return True

    def move(self, sample):
        if self.session.model is None:
            return False
        if self._marquee is not None:
            return self._moveMarquee(sample)
        if not self._dragging:
            return False
        handle = self._gizmo.activeHandle
        if handle == tonicGizmo.HANDLE_RING:
            changed = self._applyRingScale(sample)
        elif handle == tonicGizmo.HANDLE_W and self.subMode() == "ring":
            changed = self._applyRingTwist(sample)
        else:
            changed = self._applyTranslate(sample)
        if changed:
            # plan/17 section 4.2: the guides follow the shape at preview
            # density while the drag runs, at full density on release.
            refillPreview(self.session, self.state)
            self._pendingEdit = True
        return True

    def release(self, sample):
        if self._marquee is not None:
            self._moveMarquee(sample)
            self._marquee = None
            self._placeGizmo(sample.camera)
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
            self._status(self._selectionStatus())
            return True
        if not self._dragging:
            return False
        self._gizmo.end()
        self._gizmo.push(self.session)
        self._dragging = False
        if self._bracketOpen:
            self.session.endGesture()
            self._bracketOpen = False
        self._restoreDragState()
        if self._pendingEdit:
            restoreGuides(self.session, self._storedPreview)
            self.session.enqueueCommit()
            self._status(self.statusLine())
        self._storedPreview = None
        self._pendingEdit = False
        self._placeGizmo(sample.camera)
        return True

    def cancel(self):
        """Escape: the press-time shape comes back, bit for bit."""
        if self._marquee is not None:
            self._marquee = None
            return True
        if not self._dragging:
            return False
        dirty = 0
        if self._bracketOpen:
            dirty = self.session.cancelGesture()
            self._bracketOpen = False
        self._gizmo.end()
        self._dragging = False
        self._restoreDragState()
        restoreGuides(self.session, self._storedPreview,
                      refill=self._pendingEdit)
        self._storedPreview = None
        self._pendingEdit = False
        self._placeGizmo(None)
        self.session.publish(dirty)
        self._status("Tonic Tube: cancelled")
        return True

    def hover(self, sample):
        if self.session.model is None or self._dragging:
            return False
        item = sample.item(self.pickMask, self.pickRadiusPx())
        if item:
            changed = self.session.setHover(item["kind"], item["id"],
                                            item["subId"], item["subSubId"])
        else:
            changed = self.session.setHover(0, -1, -1, -1)
        if changed:
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return False

    def deactivate(self):
        """Leaving the mode takes the gizmo and the hover with it."""
        self._gizmo.clear()
        self._gizmo.push(self.session)
        self.session.setHover(0, -1, -1, -1)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION |
                             tonicLib.TONIC_DIRTY_GIZMO)
        return True

    # -- selection ---------------------------------------------------------

    def _selectItem(self, item, modifiers):
        mode = tonicLib.TONIC_SELECT_SET
        if "shift" in modifiers:
            mode = tonicLib.TONIC_SELECT_ADD
        elif "ctrl" in modifiers:
            mode = tonicLib.TONIC_SELECT_TOGGLE
        if mode == tonicLib.TONIC_SELECT_SET:
            # One kind per call, so a plain click has to drop the others
            # itself or a center CV would stay selected under a ring.
            self.session.clearSelection(self._selectableMask())
        self.session.select(item["kind"], [item["id"]], [item["subId"]],
                            [item["subSubId"]], mode)
        if item["kind"] == tonicLib.TONIC_PICK_CENTER_CV:
            self._pickedCV = (item["id"], item["subId"])
            if float(self.state.softRadius) > 0.0:
                self._selectSoftSpan(item["id"], item["subId"])

    def _selectSoftSpan(self, tubeId, cv):
        """Show the soft span: every CV the falloff will carry.

        The reference look (plan/18 section 2.4a) wants a lightness ramp
        along the span; the publication has one colour per CV state, so the
        span shows as its CV dots instead. The weights themselves are the
        model's -- this only says which CVs they reach.
        """
        count = self._tubeCenterCount(tubeId)
        radius = float(self.state.softRadius)
        if count < 2 or not radius > 0.0:
            return
        center = float(cv) / float(count - 1)
        span = [i for i in range(count)
                if abs(float(i) / float(count - 1) - center) < radius]
        if len(span) > 1:
            self.session.select(tonicLib.TONIC_PICK_CENTER_CV,
                                [tubeId] * len(span), span, [-1] * len(span),
                                tonicLib.TONIC_SELECT_ADD)

    def _moveMarquee(self, sample):
        x0, y0 = self._marquee
        mode = (tonicLib.TONIC_SELECT_ADD if sample.has("shift")
                else tonicLib.TONIC_SELECT_SET)
        self.session.selectRect(sample.camera, x0, y0, sample.x, sample.y,
                                self.pickMask, mode)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    def _selectionStatus(self):
        counts = (
            ("tube", tonicLib.TONIC_PICK_TUBE_VERT),
            ("center CV", tonicLib.TONIC_PICK_CENTER_CV),
            ("ring", tonicLib.TONIC_PICK_SECTION_RING),
            ("section CV", tonicLib.TONIC_PICK_SECTION_CV),
        )
        parts = []
        for label, kind in counts:
            n = len(self.session.readSelection(kind))
            if n:
                parts.append("%d %s%s" % (n, label, "s" if n > 1 else ""))
        if not parts:
            return "Tonic Tube: nothing selected"
        return "Tonic Tube: " + ", ".join(parts) + " selected"

    # -- dragging ----------------------------------------------------------

    def _beginDrag(self, sample, handle):
        """Open one undo bracket and freeze what the drag will move."""
        self._centerDrag = {}
        self._ringDrag = []
        self._sectionDrag = []
        sub = self.subMode()
        if sub == "center":
            self._centerDrag = self._centerDragSet()
            if not self._centerDrag:
                return False
        else:
            self._ringDrag = [(t, r, self._ringFrame(t, r))
                              for t, r, _s in self.session.readSelection(
                                  tonicLib.TONIC_PICK_SECTION_RING)]
            self._sectionDrag = [(t, r, s, self._ringFrame(t, r))
                                 for t, r, s in self.session.readSelection(
                                     tonicLib.TONIC_PICK_SECTION_CV)]
            self._ringDrag = [e for e in self._ringDrag if e[2] is not None]
            self._sectionDrag = [e for e in self._sectionDrag
                                 if e[3] is not None]
            if not self._ringDrag and not self._sectionDrag:
                return False
        if not self._gizmo.begin(handle, sample.camera, sample.x, sample.y):
            return False
        self._gizmo.push(self.session)
        self._applied = (0.0, 0.0, 0.0)
        self._appliedScale = 1.0
        self._appliedTwist = 0.0
        self._constrain = (self._rootNormalConstraint(handle, sample))
        self._pushSoftSelection()
        self._storedPreview = previewGuides(self.session, self.state)
        self.session.beginGesture("Tube %s" % sub)
        self._bracketOpen = True
        self._dragging = True
        return True

    def _rootNormalConstraint(self, handle, sample):
        """Ctrl on a screen-plane drag pins it to the tube's root normal."""
        if handle != tonicGizmo.HANDLE_PLANE or not sample.has("ctrl"):
            return None
        tubeId = (sorted(self._centerDrag)[0] if self._centerDrag
                  else (self._ringDrag[0][0] if self._ringDrag
                        else self._sectionDrag[0][0]))
        return self._rootNormal(tubeId)

    def _centerDragSet(self):
        """{tubeId: ([cv...], anchor)} for the current selection.

        A selected TUBE means its whole center column: clicking the body of
        a tube and dragging translates the curve. A selected CV set moves
        as itself, and with a soft radius the ABI spreads the delta around
        ONE anchor per tube -- moving every selected CV with a falloff each
        would count the neighbours twice.
        """
        out = {}
        for tubeId, _s, _ss in self.session.readSelection(
                tonicLib.TONIC_PICK_TUBE_VERT):
            count = self._tubeCenterCount(tubeId)
            if count > 0:
                out[tubeId] = (list(range(count)), -1)
        for tubeId, cv, _ss in self.session.readSelection(
                tonicLib.TONIC_PICK_CENTER_CV):
            if tubeId in out and out[tubeId][1] == -1:
                continue          # the whole tube is already moving
            cvs, anchor = out.get(tubeId, ([], cv))
            cvs.append(cv)
            # The anchor is the CV the artist actually clicked when there
            # is one: a soft drag spreads around it, and the lowest index
            # of a span is not where the hand is.
            if self._pickedCV is not None and self._pickedCV[0] == tubeId:
                anchor = self._pickedCV[1]
            out[tubeId] = (cvs, anchor)
        return out

    def _pushSoftSelection(self):
        """Tell the model what falloff this drag runs with.

        Whole-tube drags are exact (a falloff would bend the curve the
        artist asked to translate); a CV drag takes the panel's radius,
        centred on the anchor. The panel's own values go back on release.
        """
        dll, model = self.session.dll, self.session.model
        stored = (ctypes.c_float(0.0), ctypes.c_float(0.0))
        dll.Tonic_GetSoftSelection(model, ctypes.byref(stored[0]),
                                   ctypes.byref(stored[1]))
        self._storedSoft = (float(stored[0].value), float(stored[1].value))
        radius = 0.0
        center = 0.0
        for tubeId, (cvs, anchor) in sorted(self._centerDrag.items()):
            if anchor < 0:
                continue
            count = self._tubeCenterCount(tubeId)
            if count > 1:
                center = float(anchor) / float(count - 1)
                radius = max(0.0, float(self.state.softRadius))
            break
        dll.Tonic_SetSoftSelection(model, ctypes.c_float(center),
                                   ctypes.c_float(radius))

    def _restoreDragState(self):
        if self._storedSoft is not None and self.session.model is not None:
            self.session.dll.Tonic_SetSoftSelection(
                self.session.model, ctypes.c_float(self._storedSoft[0]),
                ctypes.c_float(self._storedSoft[1]))
        self._storedSoft = None
        self._centerDrag = {}
        self._ringDrag = []
        self._sectionDrag = []
        self._constrain = None

    def _dragDelta(self, sample):
        """The world delta since the press, with any constraint applied."""
        delta = self._gizmo.drag(sample.camera, sample.x, sample.y)
        axis = self._constrain
        if axis is None:
            return delta
        along = sum(delta[i] * axis[i] for i in range(3))
        return (axis[0] * along, axis[1] * along, axis[2] * along)

    def _applyTranslate(self, sample):
        delta = self._dragDelta(sample)
        step = tuple(delta[i] - self._applied[i] for i in range(3))
        if max(abs(v) for v in step) < MIN_STEP:
            return False
        self._applied = delta
        if self._centerDrag:
            self._moveCenters(step)
        self._moveRings(step)
        self._moveSectionCVs(step)
        self._followGizmo(delta)
        return True

    def _moveCenters(self, step):
        dll, model = self.session.dll, self.session.model
        for tubeId, (cvs, anchor) in sorted(self._centerDrag.items()):
            soft = anchor >= 0 and float(self.state.softRadius) > 0.0
            targets = [anchor] if soft else cvs
            for cv in targets:
                try:
                    tonicHierarchy.moveTubeCenterCV(dll, model, tubeId, cv,
                                                    step[0], step[1], step[2])
                except (RuntimeError, NotImplementedError) as exc:
                    self._status("Tonic Tube: %s" % exc)
                    return

    def _moveRings(self, step):
        for tubeId, ring, frame in self._ringDrag:
            du, dv = self._stage.worldToChart(frame, step)
            self._stageCall(self._stage.moveSectionRing, tubeId, ring, du, dv)

    def _moveSectionCVs(self, step):
        for tubeId, ring, slot, frame in self._sectionDrag:
            du, dv = self._stage.worldToChart(frame, step)
            self._stageCall(self._stage.moveSectionCV, tubeId, ring, slot,
                            du, dv)

    def _applyRingScale(self, sample):
        wanted = self._gizmo.ringScale(sample.camera, sample.x, sample.y)
        if not wanted > 0.0 or abs(wanted - self._appliedScale) < 1e-6:
            return False
        factor = wanted / self._appliedScale
        self._appliedScale = wanted
        for tubeId, ring, _frame in self._ringDrag:
            self._stageCall(self._stage.scaleSectionRing, tubeId, ring,
                            factor)
        return True

    def _applyRingTwist(self, sample):
        wanted = self._gizmo.ringTwist(sample.camera, sample.x, sample.y)
        if abs(wanted - self._appliedTwist) < 1e-6:
            return False
        radians = wanted - self._appliedTwist
        self._appliedTwist = wanted
        for tubeId, ring, _frame in self._ringDrag:
            self._stageCall(self._stage.twistSectionRing, tubeId, ring,
                            radians)
        return True

    def _stageCall(self, entry, *args):
        """One per-tube ABI call; a refusal is a status line, not a raise."""
        if self._stage is None:
            return False
        try:
            entry(self.session.model, *args)
            return True
        except RuntimeError as exc:
            self._status("Tonic Tube: %s" % exc)
            return False

    def _followGizmo(self, delta):
        """Keep the gizmo under the cursor while the drag runs."""
        origin = tuple(self._gizmo.pressOrigin[i] + delta[i]
                       for i in range(3))
        self._gizmo.origin = origin
        self._gizmo.push(self.session)

    # -- keys --------------------------------------------------------------

    def deleteSelection(self):
        """Delete: the selected center CVs, else the selected rings."""
        if self.session.model is None or self._stage is None:
            return False
        centers = self.session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
        rings = self.session.readSelection(tonicLib.TONIC_PICK_SECTION_RING)
        if not centers and not rings:
            return False
        self.session.beginGesture("Tube delete")
        removed = 0
        # Descending, so an earlier removal cannot shift a later index.
        for tubeId, cv, _ss in sorted(centers, reverse=True):
            if self._stageCall(self._stage.deleteCenterCV, tubeId, cv):
                removed += 1
        for tubeId, ring, _ss in sorted(rings, reverse=True):
            if self._stageCall(self._stage.removeSectionRing, tubeId, ring):
                removed += 1
        self.session.endGesture()
        self.session.clearSelection(self._selectableMask())
        self._placeGizmo(None)
        self.session.enqueueCommit()
        self._status("Tonic Tube: removed %d" % removed)
        return True

    def adjustRadius(self, delta):
        """`[` / `]`: the soft-selection radius, in t."""
        value = max(0.0, min(1.0, float(self.state.softRadius) +
                             SOFT_RADIUS_STEP * float(delta)))
        self.state.softRadius = value
        if self.session.model is not None:
            self.session.dll.Tonic_SetSoftSelection(
                self.session.model, ctypes.c_float(self.state.softCenter),
                ctypes.c_float(value))
        status = "Tonic Tube: soft radius %.2f" % value
        self._status(status)
        return status
