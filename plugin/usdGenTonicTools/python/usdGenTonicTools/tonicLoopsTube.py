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
from . import tonicTubeTransforms
from .tonicLoops import COMPONENT_PICK_RADIUS_PX, ToolLoop

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
    """Refill at the preview fraction after every accepted shape edit."""
    model = session.model
    if model is None:
        return False
    # A child edit can merge its shape into tube 0, which invalidates and
    # clears the old guide cache before this callback runs.  RefillGuides is
    # the authoritative rebuild operation; treating an empty cache as a
    # reason to skip it leaves the edited hierarchy with no preview at all.
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
    # See refillPreview: an empty cache after a child-to-parent merge still
    # has live leaf tubes and must be regenerated at release fidelity.
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
        "tube": tonicLib.TONIC_PICK_TUBE_VERT,
        "center": (tonicLib.TONIC_PICK_CENTER_CV |
                   tonicLib.TONIC_PICK_TUBE_VERT),
        "ring": (tonicLib.TONIC_PICK_SECTION_RING |
                 tonicLib.TONIC_PICK_TUBE_VERT),
        # Ring is its own explicit tool.  Keeping it out of Section avoids
        # selecting both a ring and an inner CV for the same slot, which
        # would apply their translates twice.
        "section": (tonicLib.TONIC_PICK_SECTION_CV |
                    tonicLib.TONIC_PICK_TUBE_VERT),
    }
    # Area selection is component editing in Tube mode. A body is an
    # intentional single-click fallback only; including its dense vertex
    # stream in a band turns a CV marquee into a whole-tube drag.
    COMPONENT_MASKS = {
        "tube": tonicLib.TONIC_PICK_TUBE_VERT,
        "center": tonicLib.TONIC_PICK_CENTER_CV,
        "ring": tonicLib.TONIC_PICK_SECTION_RING,
        "section": tonicLib.TONIC_PICK_SECTION_CV,
    }

    def __init__(self, session, state):
        super(TubeLoop, self).__init__(session, state)
        self._gizmo = tonicGizmo.GizmoState()
        self._bracketOpen = False
        self._dragging = False       # a gizmo handle is under the cursor
        self._marquee = None         # (x0, y0) while a rubber band is live
        self._lasso = []              # physical-pixel points while drawing
        self._applied = (0.0, 0.0, 0.0)   # world delta already pushed
        self._appliedScale = 1.0
        self._appliedTwist = 0.0
        # Frozen points used by Rotate/Scale.  Move keeps its established
        # incremental ABI path; the other tools must be absolute from press
        # so repeated mouse samples cannot accumulate numerical drift.
        self._transformOwners = []
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
        if self._marquee is not None or self._dragging or self._bracketOpen:
            self.cancel()
        status = tonicModes.SetActiveTubeSubMode(self.state, subId)
        if status:
            # The dock's F8--F11 row and the traditional Tube sub-mode shelf
            # name the same selection domain.  Keep either route in sync.
            self.state.tubeSelectionKind = subId
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
    def componentMask(self):
        return self.COMPONENT_MASKS.get(self.subMode(),
                                        self.COMPONENT_MASKS["center"])

    def componentPickRadiusPx(self):
        """Screen target for displayed Tube components.

        Snap controls graph welding, not whether an artist can take the CV
        dot already visible under the cursor.  Keep this aligned with Graph
        Region's glyph target and deliberately separate it from broad tube
        body picking, which still follows the user's snap preference.
        """
        return COMPONENT_PICK_RADIUS_PX

    def _componentItem(self, sample):
        """The precise displayed component under this live event, if any."""
        if self.subMode() == "tube":
            return None
        if self.subMode() == "ring":
            # Ring controls are drawn as section vertices, while K11's
            # native Ring candidate intentionally sits at the centroid for
            # its generic point/marquee contract. A visible vertex must win
            # over an invisible nearby centroid of another ring.
            vertex = sample.item(tonicLib.TONIC_PICK_SECTION_CV,
                                 self.componentPickRadiusPx())
            if vertex is not None:
                return {"kind": tonicLib.TONIC_PICK_SECTION_RING,
                        "id": vertex["id"], "subId": vertex["subId"],
                        "subSubId": -1}
        return sample.item(self.componentMask, self.componentPickRadiusPx())

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

    def _centerHandles(self, tubeId):
        try:
            return tonicHierarchy.tubeCenterHandles(
                self.session.dll, self.session.model, int(tubeId))
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

    def transformTool(self):
        """The explicit Tube transform tool, normalized for old sessions."""
        tool = str(getattr(self.state, "transformTool", "move")).lower()
        return tool if tool in ("select", "move", "rotate", "scale") \
            else "move"

    def setTransformTool(self, tool):
        """Select Move/Rotate/Scale and redraw the current transform now."""
        value = str(tool).lower()
        if value not in ("select", "move", "rotate", "scale"):
            return False
        if self._dragging:
            return False
        self.state.transformTool = value
        self._placeGizmo(None)
        self.session.publish(tonicLib.TONIC_DIRTY_GIZMO)
        return True

    def refreshGizmo(self, camera):
        """Track an orbit/resize without changing a frozen drag camera."""
        if self._dragging:
            return False
        return self._placeGizmo(camera)

    def _transformPivot(self, tool, bounds):
        """The visible pivot for the current transform tool.

        Translate is selection-centric, so its group pivot is the bounds
        midpoint.  Rotate and scale communicate the owner-space pivot: a
        ring uses its frame origin and a center/whole tube uses its root CV.
        The transform adapter applies that same rule independently to every
        selected owner; this method only chooses the one artist sees.
        """
        if tool == "move":
            return tuple(0.5 * (bounds[0][i] + bounds[1][i])
                         for i in range(3))
        ring = self._firstSelectedRing()
        if ring is not None:
            frame = self._ringFrame(ring[0], ring[1])
            if frame is not None:
                return tuple(frame["origin"])
        owners = []
        for kind in (tonicLib.TONIC_PICK_CENTER_CV,
                     tonicLib.TONIC_PICK_TUBE_VERT,
                     tonicLib.TONIC_PICK_SECTION_CV,
                     tonicLib.TONIC_PICK_SECTION_RING):
            owners.extend(item[0] for item in self.session.readSelection(kind))
        for tubeId in owners:
            handles = self._centerHandles(tubeId)
            if handles:
                return tuple(handles[0])
        return tuple(0.5 * (bounds[0][i] + bounds[1][i])
                     for i in range(3))

    def _placeGizmo(self, camera):
        """Put the gizmo on the selection, or take it away.

        The origin is the selection bounds' midpoint (plan/18 section 3.6:
        "a translate gizmo appears at the selection bounds"); the frame is
        the ring's own plane in Ring/Section and the screen plane in
        Center, so a drag reads the way it looks.
        """
        bounds = self._selectionBounds()
        tool = self.transformTool()
        if bounds is None or self.session.model is None or tool == "select":
            self._gizmo.clear()
            self._gizmo.push(self.session)
            return False
        origin = self._transformPivot(tool, bounds)
        sub = self.subMode()
        if tool == "rotate":
            kind = tonicGizmo.GIZMO_ROTATE
        elif tool == "scale":
            kind = tonicGizmo.GIZMO_SCALE
        else:
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
        allowed = None
        if sub in ("ring", "section"):
            if tool == "rotate":
                allowed = (tonicGizmo.HANDLE_W,)
            elif tool == "scale":
                allowed = (tonicGizmo.HANDLE_U, tonicGizmo.HANDLE_V,
                           tonicGizmo.HANDLE_CENTER,
                           tonicGizmo.HANDLE_PLANE_XY)
            elif tool == "move":
                allowed = (tonicGizmo.HANDLE_U, tonicGizmo.HANDLE_V,
                           tonicGizmo.HANDLE_CENTER,
                           tonicGizmo.HANDLE_PLANE_XY)
        # A selected root is a valid Move target but Rotate/Scale would
        # transform a zero-length offset.  Hide that inert manipulator.
        if tool in ("rotate", "scale") and sub == "center":
            centers = self.session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
            tubes = self.session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)
            if centers and not tubes and all(item[1] == 0 for item in centers):
                allowed = ()
        self._gizmo.place(origin, size, kind, frame, allowed)
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
        # A previous cancelled lasso must never affect this press.  Area
        # selection records physical-pixel points only while it is live.
        self._lasso = []
        modifiers = sample.modifiers
        # Shift reserves a drag for marquee selection even over a visible
        # tube or gizmo. A no-travel release remains the normal Shift-add.
        if sample.has("shift"):
            self._marquee = (sample.x, sample.y)
            self._lasso = ([(sample.x, sample.y)]
                           if self._selectionShape() == "lasso" else [])
            return True
        # A live press-time component query must precede generic gizmo
        # handles.  Qt can deliver a direct press without any hover event,
        # and a prior hover can be stale after a camera/model update; either
        # case must still let an unselected displayed CV win over the old
        # group's plane handle.
        component = self._componentItem(sample)
        if component is not None and not self._itemSelected(component):
            self._selectItem(component, modifiers)
            self._placeGizmo(sample.camera)
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
            self._status(self._selectionStatus())
            return True
        # Shift always extends the selection, even over a handle. Ctrl does
        # not: within the handle tolerance it is the normal constraint, and
        # everywhere else it toggles.
        handle = (tonicGizmo.HANDLE_NONE if sample.has("shift")
                  else self._gizmo.handleAt(sample.camera, sample.x,
                                            sample.y))
        if handle != tonicGizmo.HANDLE_NONE and self._beginDrag(sample,
                                                                handle):
            return True
        # A body has no component priority path.  Its explicit F8 mask is
        # resolved after a real gizmo handle, just like every broad fallback.
        # Lasso is an explicit selection-shape choice.  It therefore wins
        # over the broad whole-tube body fallback on an empty/body press;
        # controls and real gizmo handles above still retain their precise
        # click/drag behaviour.  Shift changes the eventual apply mode to
        # Add, it is not required to start a lasso.
        if self._selectionShape() == "lasso":
            self._marquee = (sample.x, sample.y)
            self._lasso = [(sample.x, sample.y)]
            return True
        # A selected component that was not on a gizmo handle still takes
        # the normal click path (for Ctrl-toggle, for example).  Only the
        # absence of a precise component defers the broad body to release.
        if component is not None:
            self._selectItem(component, modifiers)
            self._placeGizmo(sample.camera)
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
            self._status(self._selectionStatus())
            return True
        # In a component tool, defer the broad tube surface fallback until a
        # no-travel release.  That makes a box drag which begins over the
        # tube body select CVs/rings, instead of silently selecting and
        # moving the whole tube.  Explicit Whole Tube mode still resolves
        # its body immediately.
        if self.subMode() != "tube":
            self._marquee = (sample.x, sample.y)
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
            if self._lasso:
                point = (sample.x, sample.y)
                if not self._lasso or point != self._lasso[-1]:
                    self._lasso.append(point)
                return True
            return self._moveMarquee(sample)
        if not self._dragging:
            return False
        tool = self.transformTool()
        handle = self._gizmo.activeHandle
        if tool == "rotate":
            changed = self._applyRotation(sample)
        elif tool == "scale":
            changed = self._applyScale(sample)
        elif handle == tonicGizmo.HANDLE_RING:
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
            x0, y0 = self._marquee
            if self._lasso and len(set(self._lasso)) >= 3:
                # A normal closed lasso ends where it started.  Its endpoint
                # travel is therefore zero even though it enclosed an area;
                # test the recorded polygon before applying click semantics.
                if (sample.x, sample.y) != self._lasso[-1]:
                    self._lasso.append((sample.x, sample.y))
                self._moveLasso(sample)
            elif abs(sample.x - x0) + abs(sample.y - y0) < 2.0:
                item = self._componentItem(sample)
                if item is None:
                    item = sample.item(self.pickMask, self.pickRadiusPx())
                if item is not None:
                    self._selectItem(item, sample.modifiers)
                else:
                    self._moveMarquee(sample)
            elif self._lasso:
                self._lasso.append((sample.x, sample.y))
                self._moveLasso(sample)
            else:
                self._moveMarquee(sample)
            self._marquee = None
            self._lasso = []
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
        # _beginDrag changes the preview fraction even when the pointer
        # never travels, so always restore it.  Only a real edit needs the
        # full-density guide rebuild on release.
        restoreGuides(self.session, self._storedPreview,
                      refill=self._pendingEdit)
        if self._pendingEdit:
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
            self._lasso = []
            return True
        # `_beginDrag` opens the native bracket before it freezes its Python
        # baseline and sets `_dragging`.  A later callback can fail in that
        # narrow interval, and Escape/mode changes must still close the
        # bracket rather than leave an undo gesture live in the model.
        if not self._dragging and not self._bracketOpen:
            return False
        dirty = 0
        if self._bracketOpen:
            dirty = self.session.cancelGesture()
            self._bracketOpen = False
        self._gizmo.end()
        self._dragging = False
        self._restoreDragState()
        # Cancel restored the complete press-time model snapshot, including
        # its guide cache.  Restore only the viewport fraction here; a
        # refill could turn an originally empty preview into populated data.
        restoreGuides(self.session, self._storedPreview,
                      refill=False)
        self._storedPreview = None
        self._pendingEdit = False
        self._placeGizmo(None)
        self.session.publish(dirty)
        self._status("Tonic Tube: cancelled")
        return True

    def hover(self, sample):
        if self.session.model is None or self._dragging:
            return False
        item = self._componentItem(sample)
        if item is None:
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
        if self._marquee is not None or self._dragging or self._bracketOpen:
            self.cancel()
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
        else:
            self._clearIncompatibleComponentKinds(item["kind"])
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
        self._prepareAreaComponentDomain(mode)
        if self.subMode() == "ring":
            self.session.selectRect(sample.camera, x0, y0, sample.x, sample.y,
                                    tonicLib.TONIC_PICK_SECTION_CV, mode)
            self._normalizeRingAreaSelection(mode)
        else:
            self.session.selectRect(sample.camera, x0, y0, sample.x, sample.y,
                                    self.componentMask, mode)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    def _moveLasso(self, sample):
        if len(self._lasso) < 3:
            return self._moveMarquee(sample)
        mode = (tonicLib.TONIC_SELECT_ADD if sample.has("shift")
                else tonicLib.TONIC_SELECT_SET)
        self._prepareAreaComponentDomain(mode)
        if self.subMode() == "ring":
            self.session.selectPolygon(sample.camera, self._lasso,
                                       tonicLib.TONIC_PICK_SECTION_CV, mode)
            self._normalizeRingAreaSelection(mode)
        else:
            self.session.selectPolygon(sample.camera, self._lasso,
                                       self.componentMask, mode)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    def _normalizeRingAreaSelection(self, mode):
        """Turn displayed section-vertex area hits into unique ring owners."""
        owners = sorted({(int(tubeId), int(ring))
                         for tubeId, ring, _slot in
                         self.session.readSelection(
                             tonicLib.TONIC_PICK_SECTION_CV)})
        self.session.clearSelection(tonicLib.TONIC_PICK_SECTION_CV)
        if owners:
            self.session.select(tonicLib.TONIC_PICK_SECTION_RING,
                                [owner[0] for owner in owners],
                                [owner[1] for owner in owners],
                                [-1] * len(owners), mode)

    def _selectionShape(self):
        shape = str(getattr(self.state, "selectionShape", "box")).lower()
        return shape if shape in ("box", "lasso") else "box"

    def lassoPoints(self):
        return tuple(self._lasso)

    def _itemSelected(self, item):
        if item["kind"] == tonicLib.TONIC_PICK_TUBE_VERT:
            return any(entry[0] == item["id"] for entry in
                       self.session.readSelection(item["kind"]))
        return (item["id"], item["subId"], item["subSubId"]) in \
            self.session.readSelection(item["kind"])

    def _clearIncompatibleComponentKinds(self, kind):
        """Keep Shift/Ctrl edits in one unambiguous Tube selection domain."""
        sameKind = int(kind)
        self.session.clearSelection(self._selectableMask() & ~sameKind)

    def _prepareAreaComponentDomain(self, mode):
        # Shift-add preserves existing CVs/rings, but it never leaves a
        # hierarchy's old TubeVert set active under a component edit.
        if mode == tonicLib.TONIC_SELECT_SET:
            self.session.clearSelection(self._selectableMask())
        else:
            # The area mask contains exactly one component kind.  Preserve
            # its existing Shift-add set, while dropping whole tubes and any
            # other component class that would make one gesture transform
            # the same owner twice.
            self._clearIncompatibleComponentKinds(self.componentMask)

    def _selectionStatus(self):
        from .tonicTube import tubeEditHint
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
        hint = tubeEditHint(self.subMode())
        return ("Tonic Tube: " + ", ".join(parts) + " selected" +
                (". " + hint if hint else ""))

    # -- dragging ----------------------------------------------------------

    def _beginDrag(self, sample, handle):
        """Open one undo bracket and freeze what the drag will move."""
        self._centerDrag = {}
        self._ringDrag = []
        self._sectionDrag = []
        sub = self.subMode()
        if sub in ("center", "tube"):
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
        if not self.session.beginGesture("Tube %s" % sub):
            self._gizmo.end()
            self._gizmo.push(self.session)
            return False
        self._bracketOpen = True
        try:
            self._pushSoftSelection()
            self._freezeTransformBaseline()
            self._storedPreview = previewGuides(self.session, self.state)
        except Exception:
            # The model now owns an open bracket but no visible drag.  Use
            # the same cancellation path as Escape so soft selection and the
            # preview fraction return to their press-time state.
            self.cancel()
            return False
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
        # Rotate and Scale calculate their soft influence explicitly from
        # frozen points.  Leaving the backend radius live and then writing
        # every weighted point would apply the same falloff a second time.
        radius = 0.0
        if self.transformTool() in ("rotate", "scale"):
            dll.Tonic_SetSoftSelection(model, ctypes.c_float(0.0),
                                       ctypes.c_float(0.0))
            return
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
        self._transformOwners = []
        self._constrain = None

    @staticmethod
    def _sectionWorldPoint(section, frame, slot):
        """A frozen chart slot in the world position the gizmo displays."""
        if section is None or frame is None or slot < 0 or slot >= len(section[1]):
            return None
        import math
        du, dv = section[1][slot]
        # Section charts are not required to be zero-centred.  The frame
        # origin is the world ring centroid, so map chart positions relative
        # to their mean instead of incorrectly treating raw UV as offsets.
        count = float(len(section[1]))
        meanU = sum(float(pair[0]) for pair in section[1]) / count
        meanV = sum(float(pair[1]) for pair in section[1]) / count
        du, dv = float(du) - meanU, float(dv) - meanV
        scale, twist = float(section[2]), float(section[3])
        ct, st = math.cos(twist), math.sin(twist)
        # Inverse of StageLibrary.worldToChart: chart U/V first receive the
        # ring's twist and scale, then become offsets along the frame axes.
        a = scale * (float(du) * ct - float(dv) * st)
        b = scale * (float(du) * st + float(dv) * ct)
        return tuple(float(frame["origin"][i]) + a * float(frame["u"][i]) +
                     b * float(frame["v"][i]) for i in range(3))

    def _freezeTransformBaseline(self):
        """Resolve all transform targets exactly once, at mouse press."""
        self._transformOwners = []
        tool = self.transformTool()
        if tool not in ("rotate", "scale"):
            return
        # Each owner gets its own pivot.  This is why a selected L2 child
        # bends/scales about its root without moving a sibling or its parent.
        for tubeId, (cvs, anchor) in sorted(self._centerDrag.items()):
            centers = self._centers(tubeId)
            if not centers:
                continue
            handles = self._centerHandles(tubeId)
            # The visible root core is the transform pivot advertised by the
            # gizmo. Center coordinates below remain raw authored values so
            # the native CV delta writes and soft weighting keep their ABI.
            pivot = handles[0] if handles else centers[0]
            ids = set(int(cv) for cv in cvs if 0 <= int(cv) < len(centers))
            weights = {cv: 1.0 for cv in ids}
            if anchor >= 0 and float(self.state.softRadius) > 0.0:
                radius = float(self.state.softRadius)
                ids = set(range(len(centers)))
                weights = {}
                centerT = float(anchor) / float(max(len(centers) - 1, 1))
                for cv in ids:
                    distance = abs(float(cv) / float(max(len(centers) - 1, 1)) -
                                   centerT)
                    x = max(0.0, 1.0 - distance / radius)
                    weights[cv] = x * x * (3.0 - 2.0 * x)
            points = {cv: centers[cv] for cv in ids}
            frozen = tonicTubeTransforms.FrozenPoints(
                points, pivot, self._gizmo.frame)
            self._transformOwners.append({"kind": "center", "tube": tubeId,
                                           "frozen": frozen,
                                           "previous": dict(points),
                                           "weights": weights})

        # A whole selected ring owns every chart slot.  A bare Section-CV
        # selection owns only its slots, even when another ring is selected.
        selectedRings = {(int(t), int(r)) for t, r, _s in
                         self.session.readSelection(tonicLib.TONIC_PICK_SECTION_RING)}
        # Whole-tube Scale owns its section charts as well as its centre
        # curve.  Move/Rotate need only edit centers: the native frames and
        # rings follow their owner curve, whereas Scale deliberately changes
        # the U/V offsets about each ring centroid.
        if self.subMode() == "tube" and tool == "scale":
            for tubeId in self._centerDrag:
                try:
                    count = tonicBridge.tubeSectionCount(
                        self.session.dll, self.session.model, tubeId)
                except (RuntimeError, NotImplementedError):
                    count = 0
                selectedRings.update((int(tubeId), ring)
                                     for ring in range(max(int(count), 0)))
        selectedSlots = {}
        for tubeId, ring, slot in self.session.readSelection(
                tonicLib.TONIC_PICK_SECTION_CV):
            selectedSlots.setdefault((int(tubeId), int(ring)), set()).add(int(slot))
        for tubeId, ring, frame in self._ringDrag:
            selectedRings.add((int(tubeId), int(ring)))
        for tubeId, ring, _slot, frame in self._sectionDrag:
            selectedSlots.setdefault((int(tubeId), int(ring)), set()).add(_slot)
        for tubeId, ring in sorted(selectedRings | set(selectedSlots)):
            frame = self._ringFrame(tubeId, ring)
            section = self._section(tubeId, ring)
            if frame is None or section is None:
                continue
            slots = (set(range(len(section[1]))) if (tubeId, ring) in selectedRings
                     else selectedSlots[(tubeId, ring)])
            points = {slot: self._sectionWorldPoint(section, frame, slot)
                      for slot in slots}
            points = {slot: point for slot, point in points.items()
                      if point is not None}
            if not points:
                continue
            packed = frame["u"] + frame["v"] + frame["w"]
            frozen = tonicTubeTransforms.FrozenPoints(points, frame["origin"],
                                                       packed)
            self._transformOwners.append({"kind": "section", "tube": tubeId,
                                           "ring": ring, "frame": frame,
                                           "frozen": frozen,
                                           "previous": dict(points),
                                           "weights": {slot: 1.0
                                                       for slot in points}})

    @staticmethod
    def _unit(vector):
        length = sum(float(value) * float(value) for value in vector) ** 0.5
        return (tuple(float(value) / length for value in vector)
                if length > 1e-12 else None)

    def _applyFrozenTargets(self, rotateAxis=None, radians=0.0,
                            scale=(1.0, 1.0, 1.0)):
        """Write absolute press-time targets as accepted incremental ABI calls."""
        changed = False
        for owner in self._transformOwners:
            frozen = owner["frozen"]
            # Each selected ring has its own normal.  A multi-ring rotate is
            # therefore the same signed angle about each owner's frozen W,
            # rather than one world axis that rejects curved selections.
            ownerAxis = (owner["frame"]["w"] if
                         rotateAxis is not None and owner["kind"] == "section"
                         else rotateAxis)
            desired = frozen.absolute(scale=scale, rotateAxis=ownerAxis,
                                      radians=radians)
            # A soft center transform blends the one fully-transformed point
            # set against its frozen baseline once.  Root rotate/scale stays
            # pinned even if it falls inside a neighbouring CV's radius.
            if owner["kind"] == "center":
                for cv, point in list(desired.items()):
                    if int(cv) == 0:
                        point = frozen.points[cv]
                    weight = float(owner["weights"].get(cv, 0.0))
                    base = frozen.points[cv]
                    desired[cv] = tuple(base[i] + (point[i] - base[i]) * weight
                                        for i in range(3))
            for key, point in desired.items():
                previous = owner["previous"].get(key, frozen.points[key])
                step = tuple(point[i] - previous[i] for i in range(3))
                if max(abs(value) for value in step) < MIN_STEP:
                    continue
                accepted = False
                if owner["kind"] == "center":
                    try:
                        tonicHierarchy.moveTubeCenterCV(
                            self.session.dll, self.session.model,
                            owner["tube"], int(key), *step)
                        accepted = True
                    except (RuntimeError, NotImplementedError) as exc:
                        self._status("Tonic Tube: %s" % exc)
                else:
                    du, dv = self._stage.worldToChart(owner["frame"], step)
                    accepted = self._stageCall(self._stage.moveSectionCV,
                                               owner["tube"], owner["ring"],
                                               int(key), du, dv)
                # A rejected ABI write must not advance the remembered
                # absolute point; the next sample then retries the true gap.
                if accepted:
                    owner["previous"][key] = point
                    changed = True
        return changed

    def _applyRotation(self, sample):
        if not self._transformOwners:
            return False
        rotation = self._gizmo.rotationDrag(sample.camera, sample.x, sample.y)
        if rotation is None:
            return False
        axis, radians = rotation
        axis = self._unit(axis)
        if axis is None:
            return False
        # A ring chart has one valid rotation: its normal (W).  Center
        # curves may use every rotate axis, but a section never leaves plane.
        if (self._gizmo.activeHandle != tonicGizmo.HANDLE_W and any(
                owner["kind"] == "section" for owner in self._transformOwners)):
            return False
        return self._applyFrozenTargets(rotateAxis=axis, radians=radians)

    def _applyScale(self, sample):
        if not self._transformOwners:
            return False
        factor = float(self._gizmo.scaleFactor(sample.camera, sample.x,
                                               sample.y))
        if not factor > 1e-6:
            return False
        handle = self._gizmo.activeHandle
        factors = [factor, factor, factor]  # centre handle = uniform scale
        if handle in (tonicGizmo.HANDLE_U, tonicGizmo.HANDLE_V,
                      tonicGizmo.HANDLE_W):
            factors = [1.0, 1.0, 1.0]
            factors[int(handle)] = factor
        elif handle == tonicGizmo.HANDLE_PLANE_YZ:
            factors = [1.0, factor, factor]
        elif handle == tonicGizmo.HANDLE_PLANE_XZ:
            factors = [factor, 1.0, factor]
        elif handle == tonicGizmo.HANDLE_PLANE_XY:
            factors = [factor, factor, 1.0]
        # Section charts cannot scale along W.  The gizmo can still show the
        # axis for a center-curve selection, but it is disabled for rings.
        if handle == tonicGizmo.HANDLE_W and any(
                owner["kind"] == "section" for owner in self._transformOwners):
            return False
        return self._applyFrozenTargets(scale=tuple(factors))

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
        changed = False
        if self._centerDrag:
            changed = self._moveCenters(step) or changed
        changed = self._moveRings(step) or changed
        changed = self._moveSectionCVs(step) or changed
        # A refused write must not make the viewport, guides or undo stack
        # look like an edit.  Keep the incremental drag baseline advanced so
        # a later pointer sample does not replay a step on owners that did
        # accept this one.
        if changed:
            self._followGizmo(delta)
        return changed

    def _moveCenters(self, step):
        dll, model = self.session.dll, self.session.model
        changed = False
        for tubeId, (cvs, anchor) in sorted(self._centerDrag.items()):
            if anchor < 0:
                try:
                    # A surface selection means the whole tube.  Preserve
                    # its translation exactly by taking one snapshot and
                    # propagating K6/K7 once; updating its CVs one at a time
                    # derives children from transient bent parent poses.
                    tonicHierarchy.translateTube(dll, model, tubeId,
                                                 step[0], step[1], step[2])
                    changed = True
                except (RuntimeError, NotImplementedError) as exc:
                    self._status("Tonic Tube: %s" % exc)
                continue
            soft = anchor >= 0 and float(self.state.softRadius) > 0.0
            targets = [anchor] if soft else cvs
            for cv in targets:
                try:
                    tonicHierarchy.moveTubeCenterCV(dll, model, tubeId, cv,
                                                    step[0], step[1], step[2])
                    changed = True
                except (RuntimeError, NotImplementedError) as exc:
                    self._status("Tonic Tube: %s" % exc)
        return changed

    def _moveRings(self, step):
        changed = False
        for tubeId, ring, frame in self._ringDrag:
            du, dv = self._stage.worldToChart(frame, step)
            changed = (self._stageCall(self._stage.moveSectionRing, tubeId,
                                       ring, du, dv) or changed)
        return changed

    def _moveSectionCVs(self, step):
        changed = False
        for tubeId, ring, slot, frame in self._sectionDrag:
            du, dv = self._stage.worldToChart(frame, step)
            changed = (self._stageCall(self._stage.moveSectionCV, tubeId,
                                       ring, slot, du, dv) or changed)
        return changed

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
