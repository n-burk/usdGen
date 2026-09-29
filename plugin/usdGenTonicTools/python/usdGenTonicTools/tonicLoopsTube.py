# usdGenTonicTools.tonicLoopsTube -- Tube mode's viewport loop (plan/18
# section 3.6 TubeLoop, section 2.4, plan/17 section 5.2).
#
# Qt-free like every loop: pixels, modifiers and a camera in, C ABI calls
# out. What Tube mode owns is the shape of a tube -- its center CVs, its
# cross-section rings and the CVs of those rings. The sub-mode (F8-F11) is
# the component kind a click picks; the transform tool (Q/W/E/R) is the
# gizmo family, drawn at the selection's pivot:
#
#   Whole tube (F8)  click selects the tube body; the tools move, rotate
#                    and scale the whole tube about its root.
#   Center CV (F9)   click/box selects center CVs. Move is a TRANSLATE
#                    gizmo in the Axis Orientation (World / Screen / Tube,
#                    `L` flips World <-> Tube): axes, planar squares (Ctrl
#                    on an axis = the plane perpendicular to it) and the
#                    camera-plane centre (Ctrl on the centre = along the
#                    tube's root normal). Rotate and Scale turn/scale the
#                    CVs above the root; a root CV alone is pinned.
#   Ring (F10)       click selects whole rings; Move is a RING_TRS gizmo
#                    that translates the ring inside its own plane (u, v,
#                    centre), scales it (the outer circle) and TWISTS it
#                    (the w arrow); Rotate/Scale work in the ring frame.
#   Section CV (F11) click selects ring CVs; the tools act inside the
#                    ring plane.
#
# The press ladder (RigExec/a DCC parity): a visible handle wins the press
# (hover pre-highlights it); off every handle Shift reserves a band; a
# component click selects through the one modifier table
# (tonicLoops.selectModeFor) and, with Move and no modifier, drags what it
# just selected in the same gesture (tweak); a plain middle drag repeats
# the last-dragged handle from anywhere. F8-F11 CONVERT the selection to
# the new kind rather than dropping it. During a drag Shift is precision
# (a tenth of the travel), J holds a step snap and X a world-grid snap.
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
import math
import time
import types

from . import tonicBridge
from . import tonicGizmo
from . import tonicGizmoScreen
from . import tonicGizmoSettings
from . import tonicGizmoSnap
from . import tonicHierarchy
from . import tonicLib
from . import tonicModes
from . import tonicTubeTransforms
from .tonicLoops import (COMPONENT_PICK_RADIUS_PX, ToolLoop, selectBand,
                         selectItems, selectModeFor)

# The gizmo's on-screen size: axes this many pixels long, whatever the zoom
# (plan/18 section 2.4a). a DCC's translate manipulator is about this long on
# a 1080p viewport, and it has to stay clear of the 8 px CV dots it sits
# among without covering the tube it moves.  LOGICAL pixels, the RigExec
# manipulator size (tonicGizmoScreen.GIZMO_PIXELS).
GIZMO_PIXELS = tonicGizmo.tonicGizmoScreen.GIZMO_PIXELS
# A drag step smaller than this in world units is not worth an ABI call.
MIN_STEP = 1e-7
# `[` / `]` step for the soft-selection radius (in t).
SOFT_RADIUS_STEP = 0.05
# GZ-06: what Rotate/Scale on a lone root CV says instead of vanishing.
PINNED_ROOT_STATUS = ("Root CV is pinned: Rotate/Scale act on CVs above the "
                      "root. Select the tube (F8) to transform it whole.")
# GZ-07: Shift held mid-drag moves the handle this fraction of the pointer
# travel (a DCC/a DCC precision), from the sample where it went down.
PRECISION_FACTOR = 0.1
# GZ-07: the live drag readout reaches the status line at most this often
# (seconds); the viewport label repaints with every sample regardless.
READOUT_STATUS_INTERVAL = 0.05
# Axis letters for the readout: the handles are red/green/blue in every
# orientation, which artists read as X/Y/Z.
_AXIS_LETTERS = {0: "X", 1: "Y", 2: "Z"}


def worldSizeForPixels(camera, origin, pixels=GIZMO_PIXELS):
    """World length that spans `pixels` LOGICAL pixels on screen at `origin`.

    The camera maps PHYSICAL pixels, so the logical size is scaled by its
    device pixel ratio (parity G24): on a 200 % display the gizmo keeps the
    size it has at 100 % instead of shrinking to half.
    """
    if camera is None:
        return 1.0
    perPixel = camera.worldPerPixel(origin)
    if not perPixel > 0.0:
        return 1.0
    ratio = tonicGizmo.cameraPixelRatio(camera)
    return max(perPixel * float(pixels) * ratio, 1e-6)


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


# -- SL-03: select all / invert, and whole-tube Delete -------------------------
#
# Shared by TubeLoop, HierarchyLoop and the controller's fallback for the
# modes whose loop has no command of its own (Graph, Fill).

def viewBand(session, camera, kindMask):
    """band(mode) over the whole view: what "every visible X" means.

    The rect select runs the candidate sets a marquee does, so it takes
    exactly what the display policy shows on the focused level -- a hidden
    level or a collapsed branch stays out, which enumerating ids would not.
    """
    width = float(getattr(camera, "width", 0) or 0)
    height = float(getattr(camera, "height", 0) or 0)

    def band(mode):
        if camera is None or width <= 0.0 or height <= 0.0:
            return False
        return session.selectRect(camera, 0.0, 0.0, width, height, kindMask,
                                  mode)
    return band


def invertBand(session, kindMask, band):
    """Flip every item `band` covers against the current selection.

    Not a TOGGLE band: a band hits a whole tube once per tessellated
    vertex, and toggling per hit flips it back and forth. The band runs as
    SET to learn what it covers, then the selection becomes (covered minus
    selected) plus (selected minus covered), kind by kind -- selected items
    off screen stay selected.
    """
    from .tonicLoops import (_kindsIn, _selectEntries, _selectionEntry,
                             _selectionKey)
    kinds = list(_kindsIn(kindMask))
    if not kinds:
        return False             # clearSelection(0) would mean "every kind"
    before = {kind: [_selectionEntry(entry)
                     for entry in session.readSelection(kind)]
              for kind in kinds}
    if not band(tonicLib.TONIC_SELECT_SET):
        return False
    covered = {kind: [_selectionEntry(entry)
                      for entry in session.readSelection(kind)]
               for kind in kinds}
    session.clearSelection(kindMask)
    for kind in kinds:
        coveredKeys = {_selectionKey(kind, entry) for entry in covered[kind]}
        beforeKeys = {_selectionKey(kind, entry) for entry in before[kind]}
        flipped = [entry for entry in covered[kind]
                   if _selectionKey(kind, entry) not in beforeKeys]
        flipped += [entry for entry in before[kind]
                    if _selectionKey(kind, entry) not in coveredKeys]
        if flipped:
            _selectEntries(session, kind, flipped, tonicLib.TONIC_SELECT_ADD)
    return True


def deleteWholeTubes(session, tubeIds):
    """Remove whole tubes (with their subtrees) as ONE undo step.

    Returns (removed ids, refused L1 ids, error text). An L1 root belongs to
    its graph region -- the next region sync would only rebuild it as a
    fresh stub -- so it is refused here with a reason rather than deleted.
    """
    from . import tonicLibStage
    dll, model = session.dll, session.model
    if model is None:
        return [], [], "no model"
    ids = sorted({int(t) for t in tubeIds})
    # Group parents mint negative ids and are free to go; every other
    # level-1 tube is a region's root.
    roots = [t for t in ids
             if t >= 0 and tonicHierarchy.tubeLevel(dll, model, t) <= 1]
    doomed = [t for t in ids if t not in roots]
    if not doomed:
        return [], roots, ""
    if not session.beginGesture("Delete tubes"):
        return [], roots, "could not start an undo step -- nothing changed"
    try:
        tonicLibStage.removeTubes(dll, model, doomed)
    except RuntimeError as exc:
        session.publish(int(session.cancelGesture() or 0))
        return [], roots, str(exc)
    session.endGesture()
    # The removed leaves' guides go, and a parent that lost its last child
    # produces again; the id map changed, so the bake follows.
    restoreGuides(session, None)
    session.publish()
    session.enqueueCommit()
    rebake = getattr(session, "rebake", None)
    if rebake is not None:
        rebake()
    return doomed, roots, ""


def deleteTubesStatus(prefix, removed, roots, error):
    """The one status line both loops show for a whole-tube Delete."""
    parts = []
    if removed:
        parts.append("deleted %d tube%s" % (len(removed),
                                            "" if len(removed) == 1 else "s"))
    if error:
        parts.append("delete failed: %s" % error)
    if roots:
        parts.append("%s %s an L1 root -- its graph region owns it; delete "
                     "the region in Graph mode"
                     % (", ".join("T%d" % t for t in roots),
                        "is" if len(roots) == 1 else "are each"))
    return "%s: %s" % (prefix, "; ".join(parts) if parts else
                       "nothing to delete")


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
        # GZ-03: a drag that began as a component press (tweak: select and
        # move in one gesture), and a Ctrl/Shift press on a selected
        # component under the centre handle.  Either one released without
        # travel is a click, not an edit.
        self._tweak = False
        self._clickCandidate = None  # (component, modifiers)
        # The gizmo's remembered handle before a tweak press lit the
        # centre: a tweak that ends as a plain click puts it back, so a
        # click never marks the centre as last dragged (RigExec remembers
        # only a handle a press hit).
        self._tweakRemembered = None
        self._lastStatus = ""
        # The camera the gizmo was last placed or hovered with: a key or a
        # dock setting re-places it without a mouse event, and the Screen
        # orientation and a manipulator resize both need a camera.
        self._lastCamera = None
        # The angle the last rotate sample applied after a step snap, or
        # None when it was not snapped (the status line reads it).
        self._snappedDegrees = None
        # GZ-06: the tool the pinned-root status was last said for, so it
        # is said once per pinned tool, not on every re-placement.
        self._pinnedReported = None
        # GZ-07: Shift precision integrates pointer travel per sample, so
        # the drag keeps the (raw, effective) cursor of the last sample.
        self._rawCursor = None
        self._effCursor = None
        self._precision = False
        # GZ-07: the live readout and when it last reached the status line.
        self._readout = ""
        self._readoutAt = None
        self._lastScaleFactor = 1.0
        self._statusSerial = 0

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return self.state.tubeSubMode or self.defaultSubMode

    def setSubMode(self, subId):
        if self._marquee is not None or self._dragging or self._bracketOpen:
            self.cancel()
        self._gizmo.setHoverHandle(tonicGizmo.HANDLE_NONE)
        previous = self.subMode()
        status = tonicModes.SetActiveTubeSubMode(self.state, subId)
        if status:
            # The dock's F8--F11 row and the traditional Tube sub-mode shelf
            # name the same selection domain.  Keep either route in sync.
            self.state.tubeSelectionKind = subId
            # GZ-04: the kinds a click means changed, so the selection is
            # CONVERTED to the new kind (a DCC's component conversion), not
            # thrown away -- F9 after a body click used to lose the tube.
            converted = self._convertSelection(self.subMode())
            self._pickedCV = None
            self._placeGizmo(None)
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
            if converted and self.subMode() != previous:
                status = "%s -- converted %d item(s)" % (status, converted)
        return status

    # -- GZ-04: component-kind conversion -----------------------------------

    def _ringOwnerCV(self, tubeId, ring):
        """The centre CV a ring hangs on: its station t in CV units.

        Rings are stationed at t in [0, 1] along the centre column, one per
        CV when the tube is built (tonicModel `t = r / (count - 1)`), so the
        owner is the CV nearest the ring's own t.  None when unreadable.
        """
        count = self._tubeCenterCount(tubeId)
        section = self._section(tubeId, ring)
        if count < 1 or section is None:
            return None
        cv = int(round(float(section[0]) * float(count - 1)))
        return max(0, min(count - 1, cv))

    def _tubeRingCount(self, tubeId):
        if self.session.model is None:
            return 0
        try:
            return max(0, int(tonicBridge.tubeSectionCount(
                self.session.dll, self.session.model, int(tubeId))))
        except (RuntimeError, NotImplementedError, AttributeError):
            return 0

    def _cvRings(self, tubeId, cv):
        """The rings a centre CV owns (the inverse of _ringOwnerCV).

        A CV no ring is stationed on (an added CV between two rings) takes
        the nearest ring, so converting it never comes back empty.
        """
        count = self._tubeCenterCount(tubeId)
        best, bestGap, owned = None, None, []
        for ring in range(self._tubeRingCount(tubeId)):
            section = self._section(tubeId, ring)
            if section is None or count < 1:
                continue
            at = float(section[0]) * float(count - 1)
            if int(round(at)) == int(cv):
                owned.append(ring)
            gap = abs(at - float(cv))
            if bestGap is None or gap < bestGap:
                best, bestGap = ring, gap
        if not owned and best is not None:
            owned.append(best)
        return owned

    def _ringSlots(self, tubeId, ring):
        section = self._section(tubeId, ring)
        return range(len(section[1])) if section else range(0)

    def _convertSelection(self, target):
        """Convert the Tube selection to sub-mode `target`'s kind.

        section CV -> its ring; ring -> the centre CV that owns it; centre
        CV -> the whole tube; a component to Whole Tube -> its distinct
        owner tubes.  Going down the ladder is the inverse (a ring -> its
        section CVs, a centre CV -> its rings).  A whole tube converted to a
        component kind stays selected as the OWNER SET: its TubeVert record
        is kept, and component picks are restricted to it while it exists
        (_componentItem), as the host application shows only the hilited object's
        components.  Returns how many items the new selection holds.
        """
        read = self.session.readSelection
        tubes = read(tonicLib.TONIC_PICK_TUBE_VERT)
        centers = read(tonicLib.TONIC_PICK_CENTER_CV)
        rings = read(tonicLib.TONIC_PICK_SECTION_RING)
        cvs = read(tonicLib.TONIC_PICK_SECTION_CV)
        if not (tubes or centers or rings or cvs):
            return 0
        out = {}

        def add(kind, item):
            bucket = out.setdefault(kind, [])
            item = (int(item[0]), int(item[1]), int(item[2]))
            if item not in bucket:
                bucket.append(item)

        # Whole tubes are carried over whatever the target: in Whole Tube
        # they are the selection, below it they are the owner set.
        for tubeId, _s, _ss in tubes:
            add(tonicLib.TONIC_PICK_TUBE_VERT, (tubeId, -1, -1))
        if target == "tube":
            for tubeId, _s, _ss in centers + rings + cvs:
                add(tonicLib.TONIC_PICK_TUBE_VERT, (tubeId, -1, -1))
        elif target == "center":
            for item in centers:
                add(tonicLib.TONIC_PICK_CENTER_CV, item)
            for tubeId, ring, _s in rings + cvs:
                cv = self._ringOwnerCV(tubeId, ring)
                if cv is not None:
                    add(tonicLib.TONIC_PICK_CENTER_CV, (tubeId, cv, -1))
        elif target == "ring":
            for tubeId, ring, _s in rings + cvs:
                add(tonicLib.TONIC_PICK_SECTION_RING, (tubeId, ring, -1))
            for tubeId, cv, _s in centers:
                for ring in self._cvRings(tubeId, cv):
                    add(tonicLib.TONIC_PICK_SECTION_RING, (tubeId, ring, -1))
        elif target == "section":
            for item in cvs:
                add(tonicLib.TONIC_PICK_SECTION_CV, item)
            owned = [(tubeId, ring) for tubeId, ring, _s in rings]
            for tubeId, cv, _s in centers:
                owned.extend((tubeId, ring)
                             for ring in self._cvRings(tubeId, cv))
            for tubeId, ring in owned:
                for slot in self._ringSlots(tubeId, ring):
                    add(tonicLib.TONIC_PICK_SECTION_CV, (tubeId, ring, slot))
        self.session.clearSelection(self._selectableMask())
        for kind, items in out.items():
            self.session.select(kind, [item[0] for item in items],
                                [item[1] for item in items],
                                [item[2] for item in items],
                                tonicLib.TONIC_SELECT_SET)
        return sum(len(items) for items in out.values())

    def _ownerTubes(self):
        """GZ-04: the whole tubes a component sub-mode is restricted to.

        Empty in Whole Tube (the body IS the selection there) and whenever
        no whole tube is selected, which leaves every component pickable.
        """
        if self.subMode() == "tube":
            return frozenset()
        return frozenset(int(item[0]) for item in self.session.readSelection(
            tonicLib.TONIC_PICK_TUBE_VERT))

    @property
    def pickMask(self):
        return self.KIND_MASKS.get(self.subMode(), self.KIND_MASKS["center"])

    @property
    def componentMask(self):
        return self.COMPONENT_MASKS.get(self.subMode(),
                                        self.COMPONENT_MASKS["center"])

    def componentPickRadiusPx(self, camera=None):
        """Screen target for displayed Tube components.

        Snap controls graph welding, not whether an artist can take the CV
        dot already visible under the cursor.  Keep this aligned with Graph
        Region's glyph target and deliberately separate it from broad tube
        body picking, which still follows the user's snap preference.

        COMPONENT_PICK_RADIUS_PX is LOGICAL; the picker measures PHYSICAL
        pixels, so a camera's device pixel ratio scales it (GZ-05) and a
        200 % display keeps the same finger-sized target.
        """
        return COMPONENT_PICK_RADIUS_PX * tonicGizmo.cameraPixelRatio(camera)

    def _componentItem(self, sample):
        """The precise displayed component under this live event, if any."""
        if self.subMode() == "tube":
            return None
        radius = self.componentPickRadiusPx(sample.camera)
        # GZ-04: a whole tube carried into a component sub-mode is the
        # owner set; only its own components answer until it is replaced.
        owners = self._ownerTubes()

        def owned(item):
            if item is None or not owners or int(item["id"]) in owners:
                return item
            return None

        if self.subMode() == "ring":
            # Ring controls are drawn as section vertices, while K11's
            # native Ring candidate intentionally sits at the centroid for
            # its generic point/marquee contract. A visible vertex must win
            # over an invisible nearby centroid of another ring.
            vertex = owned(sample.item(tonicLib.TONIC_PICK_SECTION_CV,
                                       radius))
            if vertex is not None:
                return {"kind": tonicLib.TONIC_PICK_SECTION_RING,
                        "id": vertex["id"], "subId": vertex["subId"],
                        "subSubId": -1}
        return owned(sample.item(self.componentMask, radius))

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
        # Counts reports so a drag sample can tell that its apply already
        # said something (a native refusal) the readout must not bury.
        self._statusSerial += 1
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
        # Parity G16: the orientation is per tool (Rotate starts in Tube),
        # so the switch banks the old tool's and restores the new one's.
        tonicGizmoSettings.SwitchTool(self.state, value)
        self.state.transformTool = value
        self._placeGizmo(None)
        self.session.publish(tonicLib.TONIC_DIRTY_GIZMO)
        return True

    def refreshGizmo(self, camera):
        """Track an orbit/resize without changing a frozen drag camera."""
        if self._dragging:
            return False
        return self._placeGizmo(camera)

    def gizmoDragActive(self):
        """True while a gizmo handle drag (not a marquee) is live.

        The controller claims the J / X snap holds only then (parity G11).
        """
        return bool(self._dragging)

    def _gizmoSettings(self):
        return tonicGizmoSettings.settingsFor(self.state)

    def toolSettings(self, tool=None):
        """The live per-tool settings (tonicGizmoSettings.ToolSettings)."""
        return self._gizmoSettings().For(tool or self.transformTool())

    def transformOrientation(self):
        """The Axis Orientation token: "world", "screen" or "tube"."""
        return tonicGizmoSettings.NormalizeOrientation(
            getattr(self.state, "transformOrientation", "world"))

    def setTransformOrientation(self, orientation, camera=None):
        """Point the handles another way (GZ-05); refused mid-drag."""
        if self._dragging:
            return False
        self.state.transformOrientation = \
            tonicGizmoSettings.NormalizeOrientation(orientation)
        self._placeGizmo(camera if camera is not None else self._lastCamera)
        self.session.publish(tonicLib.TONIC_DIRTY_GIZMO)
        return True

    def toggleOrientation(self, camera=None):
        """`L`: flip World <-> Tube (RigExec's Global/Local toggle).

        Returns the new token, or None when a drag holds the gizmo.
        """
        new = tonicGizmoSettings.NextToggleOrientation(
            self.transformOrientation())
        if not self.setTransformOrientation(new, camera):
            return None
        self._status("Tonic Tube: %s orientation (%s)" % (
            tonicGizmoSettings.ToggleLabel(new),
            tonicGizmoSettings.OrientationLabel(new)))
        return new

    def scaleManipulator(self, factor, camera=None):
        """`+` / `-`: grow or shrink the gizmo's on-screen size by `factor`.

        Returns the size taken (logical pixels, clamped), or None mid-drag:
        the drag's press-time handle must not change under the hand.
        """
        if self._dragging:
            return None
        size = self._gizmoSettings().ScaleManipulator(float(factor))
        self._placeGizmo(camera if camera is not None else self._lastCamera)
        self.session.publish(tonicLib.TONIC_DIRTY_GIZMO)
        self._status("Tonic Tube: manipulator size %d px" % int(round(size)))
        return size

    def groupPivot(self):
        """The live tool's group pivot token (parity G17).

        Move and Select have no choice (GroupPivotChoices is empty), so
        whatever their field holds, the maths treats them as moving every
        item by one delta.
        """
        return self.toolSettings().groupPivot

    def _centrePivot(self):
        """True when Rotate/Scale turn the selection about ONE shared point."""
        tool = self.transformTool()
        return (tool in ("rotate", "scale") and
                self.groupPivot() == tonicGizmoSettings.GROUP_PIVOT_CENTRE)

    def setGroupPivot(self, mode, camera=None):
        """Choose Individual Origins or Selection Centre for the live tool.

        Refused (False) mid-drag and for a tool that offers no choice.
        """
        if self._dragging:
            return False
        tool = self.transformTool()
        if mode not in tonicGizmoSettings.GroupPivotChoices(tool):
            return False
        self.toolSettings().groupPivot = mode
        self._placeGizmo(camera if camera is not None else self._lastCamera)
        self.session.publish(tonicLib.TONIC_DIRTY_GIZMO)
        return True

    def cycleGroupPivot(self, camera=None):
        """`P`: the live tool's next group pivot; None when refused."""
        tool = self.transformTool()
        new = tonicGizmoSettings.NextGroupPivot(self.groupPivot(), tool)
        if not self.setGroupPivot(new, camera):
            if not self._dragging and \
                    not tonicGizmoSettings.GroupPivotChoices(tool):
                self._status("Tonic Tube: group pivot applies to Rotate "
                             "and Scale (E / R)")
            return None
        self._status("Tonic Tube: %s pivot: %s" % (
            tool.title(), tonicGizmoSettings.GroupPivotLabel(new)))
        return new

    def _orientationNormal(self):
        """The root normal of the tube the Tube orientation follows."""
        for kind in (tonicLib.TONIC_PICK_CENTER_CV,
                     tonicLib.TONIC_PICK_TUBE_VERT,
                     tonicLib.TONIC_PICK_SECTION_CV,
                     tonicLib.TONIC_PICK_SECTION_RING):
            for item in self.session.readSelection(kind):
                normal = self._rootNormal(item[0])
                if normal is not None:
                    return normal
        return None

    def _transformPivot(self, tool, bounds):
        """The visible pivot for the current transform tool.

        Translate is selection-centric, so its group pivot is the bounds
        midpoint.  Rotate and scale communicate the owner-space pivot: a
        ring uses its frame origin and a center/whole tube uses its root CV.
        The transform adapter applies that same rule independently to every
        selected owner (Individual Origins); this method only chooses the
        one the artist sees, the lead owner's.  With the Selection Centre
        group pivot (parity G17) every owner turns about the bounds
        midpoint, and that is also where the gizmo is drawn --
        _freezeTransformBaseline takes the placed origin as the pivot, so
        the picture and the maths cannot disagree.
        """
        if tool == "move" or self._centrePivot():
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
        the ring's own plane in Ring/Section (its chart is two dimensional,
        so world axes would be inert there) and the Axis Orientation
        otherwise (GZ-05): World by default, so red/green/blue really are
        world X/Y/Z, or the screen plane, or the owner tube's root-normal
        frame.  The centre handle is the camera-plane move whatever the
        frame.  The size is the manipulator size (logical pixels, `+`/`-`)
        at this camera's zoom.
        """
        if camera is not None:
            self._lastCamera = camera
        frameCamera = camera if camera is not None else self._lastCamera
        bounds = self._selectionBounds()
        tool = self.transformTool()
        # The bounds cover EVERY selected kind (a Graph node too), so the
        # gizmo also needs something this sub-mode's _beginDrag can move:
        # a whole tube kept as the GZ-04 owner set in Ring/Section, or a
        # Graph selection met on entering Tube (GZ-08 activate), would be
        # a handle that refuses every press.
        if self.subMode() in ("ring", "section"):
            draggable = self._firstSelectedRing() is not None
        else:
            draggable = bool(
                self.session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) or
                self.session.readSelection(tonicLib.TONIC_PICK_CENTER_CV))
        if bounds is None or self.session.model is None or \
                tool == "select" or not draggable:
            self._gizmo.clear()
            self._gizmo.push(self.session)
            self._pinnedReported = None
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
            orientation = self.transformOrientation()
            normal = (self._orientationNormal()
                      if orientation == tonicGizmoSettings.ORIENT_TUBE
                      else None)
            frame = tonicGizmo.orientationFrame(orientation, frameCamera,
                                                origin, normal)
        # Without a camera (a key action, not a click) the gizmo keeps the
        # size it had rather than jumping to a world-unit default.
        settings = self._gizmoSettings()
        size = (worldSizeForPixels(camera, origin, settings.manipulatorSize)
                if camera is not None else self._gizmo.sizeWorld)
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
                if sub == "ring":
                    # GZ-06: the ringTRS gizmo's outer circle scales the
                    # ring and its W arrow twists it (_applyRingScale /
                    # _applyRingTwist); the Ring hint promises both.  A
                    # section CV has neither, so Section keeps the plain
                    # in-plane move.
                    allowed += (tonicGizmo.HANDLE_RING, tonicGizmo.HANDLE_W)
        # A selected root is a valid Move target but Rotate/Scale would
        # transform a zero-length offset.  Keep the manipulator on screen,
        # dimmed and ungrabbable, and say why (GZ-06) -- a gizmo that
        # silently vanished read as a broken tool.
        pinned = tool in ("rotate", "scale") and self._pinnedRootOnly()
        self._gizmo.place(origin, size, kind, frame, allowed,
                          freeRotate=bool(settings.For(
                              tonicGizmoSettings.TOOL_ROTATE).freeRotate),
                          locked=pinned)
        self._gizmo.push(self.session)
        if pinned and self._pinnedReported != tool:
            self._status(PINNED_ROOT_STATUS)
        self._pinnedReported = tool if pinned else None
        return True

    def _pinnedRootOnly(self):
        """True when the Tube selection is root center CVs and nothing else.

        Rotate and Scale pivot on the root, so a root alone has nothing to
        turn or grow.  Any other CV, ring or whole tube in the selection
        makes the transform meaningful again -- and so do two or more roots
        under the Selection Centre group pivot (G17), which turns them
        about their shared middle.
        """
        if self.subMode() != "center":
            return False
        centers = self.session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
        if not centers or not all(item[1] == 0 for item in centers):
            return False
        if self._centrePivot() and len(centers) > 1:
            return False
        return not (self.session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)
                    or self.session.readSelection(
                        tonicLib.TONIC_PICK_SECTION_RING)
                    or self.session.readSelection(
                        tonicLib.TONIC_PICK_SECTION_CV))

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
        if sample.has("middle"):
            return self.middlePress(sample)
        self._pendingEdit = False
        self._constrain = None
        self._tweak = False
        self._clickCandidate = None
        # A previous cancelled lasso must never affect this press.  Area
        # selection records physical-pixel points only while it is live.
        self._lasso = []
        modifiers = sample.modifiers
        # (a) A visible gizmo handle wins the press, as in RigExec/a DCC
        # (gizmoUI._OnPress: the hit test runs first and a hit ALWAYS
        # drags).  A CV dot inside a handle's tolerance no longer steals
        # the drag; the handle prehighlight (hover) shows what a press will
        # grab.  Shift over a handle drags too; Ctrl on the centre handle is
        # the root-normal constraint (_rootNormalConstraint).
        if self._gizmo.visible and self.transformTool() != "select":
            handle = self._gizmo.handleAt(sample.camera, sample.x, sample.y)
            if handle != tonicGizmo.HANDLE_NONE:
                # The centre handle covers the selected component it sits
                # on.  A Ctrl/Shift press there is remembered: released
                # without travel it is that component's selection click
                # (GZ-03), with travel it is the drag.  In Whole tube the
                # centre sits on the selected tube's body, which is the item.
                candidate = None
                if handle == tonicGizmo.HANDLE_CENTER and \
                        selectModeFor(sample) != tonicLib.TONIC_SELECT_SET:
                    component = self._componentItem(sample)
                    if component is None and self.subMode() == "tube":
                        component = sample.item(self.pickMask,
                                                self.pickRadiusPx())
                    if component is not None and \
                            self._itemSelected(component):
                        candidate = (component, modifiers)
                if self._beginDrag(sample, handle):
                    self._clickCandidate = candidate
                    return True
        # (b) Shift off every handle reserves a drag for marquee selection,
        # even over a visible tube. A no-travel release remains the normal
        # Shift click.
        if sample.has("shift"):
            self._marquee = (sample.x, sample.y)
            self._lasso = ([(sample.x, sample.y)]
                           if self._selectionShape() == "lasso" else [])
            return True
        # (c) The precise displayed component under the cursor, queried
        # live at the press (Qt may deliver a press with no hover first).
        component = self._componentItem(sample)
        if component is not None:
            self._selectItem(component, modifiers)
            self._placeGizmo(sample.camera)
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
            self._status(self._selectionStatus())
            self._beginTweak(sample)
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
        # The Whole tube body tweaks exactly like a component (GZ-03): a
        # plain Move press selects the tube and the same gesture translates
        # it.  Ctrl/Shift keep the click table (selectModeFor), no drag.
        self._beginTweak(sample)
        return True

    def _beginTweak(self, sample):
        """Tweak (GZ-03): hold a centre drag on what the press just selected.

        Only with Move and no modifier (a Ctrl/Shift press is a selection
        click); the drag runs through the camera-plane centre handle.  A
        release without travel stays a plain selection and leaves no undo
        step (release() closes the bracket with endGestureIfChanged).
        """
        remembered = self._gizmo.remembered()
        if self.transformTool() == "move" and \
                selectModeFor(sample) == tonicLib.TONIC_SELECT_SET and \
                self._gizmo.visible and \
                self._gizmo.handleAllowed(tonicGizmo.HANDLE_CENTER) and \
                self._beginDrag(sample, tonicGizmo.HANDLE_CENTER):
            self._tweak = True
            self._tweakRemembered = remembered
        return self._tweak

    def middleRepeatAvailable(self, camera=None):
        """True when a plain middle press would repeat a remembered handle.

        The controller's middle-button camera navigation asks this first:
        only a press this returns True for belongs to the gizmo (G04).
        As RigExec (gizmoUI._OnPress: `not handle.grabbable`), a remembered
        handle the overlay dims as ungrabbable at this camera -- an axis
        turned edge-on since it was dragged -- is declined, not driven.
        """
        if self.session.model is None or self._dragging or \
                self._marquee is not None:
            return False
        if not self._gizmo.visible or self.transformTool() == "select":
            return False
        return self._gizmo.handleGrabbable(
            camera if camera is not None else self._lastCamera,
            self._gizmo.selectedHandle)

    def middlePress(self, sample):
        """A middle drag anywhere repeats the last-dragged handle.

        RigExec/the host application (gizmoUI._OnPress): the middle button needs no hit
        test, it re-grabs the remembered handle from wherever the cursor
        is, so a small or crowded handle can be driven from open space.
        Declined (usdview keeps the press) when there is no gizmo or no
        remembered handle this placement still offers.
        """
        if not self.middleRepeatAvailable(sample.camera):
            return False
        handle = self._gizmo.selectedHandle
        self._pendingEdit = False
        self._constrain = None
        self._tweak = False
        self._clickCandidate = None
        self._lasso = []
        return self._beginDrag(sample, handle)

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
        # The controller only forwards moves past its travel threshold, so
        # a pending Ctrl/Shift click on the centre handle is now a drag.
        self._clickCandidate = None
        # GZ-07: Shift slows the pointer to a tenth from here on.
        sample = self._precisionSample(sample)
        tool = self.transformTool()
        handle = self._gizmo.activeHandle
        reported = self._statusSerial
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
        # RigExec's live readout (parity G10, GZ-07): the delta, angle or
        # factor so far, so the artist can stop on a number -- the snapped
        # value when a step snap is quantising it (G11).  The viewport
        # label reads dragReadout() every paint; the status line is
        # throttled so a fast drag does not flood the status sink, and it
        # never buries a refusal this sample's apply just reported.
        self._readout = self._buildReadout(tool, handle, sample)
        if self._statusSerial == reported:
            self._reportReadout()
        if changed:
            # plan/17 section 4.2: the guides follow the shape at preview
            # density while the drag runs, at full density on release.
            refillPreview(self.session, self.state)
            self._pendingEdit = True
        return True

    # -- GZ-07: live readout and Shift precision ----------------------------

    def dragReadout(self):
        """What the live drag has done so far, or "" when no drag runs.

        'Move X 0.420' along an axis (the signed distance), 'Move XY 0.420'
        in a plane, 'Rotate 23.4°', 'Scale 1.25', 'Twist 12.0°' on a ring,
        followed by ' · Shift precision' and ' · snap step 15' / ' · grid 1'
        when those shape the value.  The viewport draws it beside the gizmo
        centre and the status line repeats it.
        """
        return self._readout if self._dragging else ""

    def _precisionSample(self, sample):
        """`sample` with Shift's precision applied to its pointer travel.

        The effective cursor integrates each sample's travel, a tenth of it
        while Shift is held (PRECISION_FACTOR), so pressing Shift mid-drag
        slows the handle from where it is and releasing Shift carries on
        from there without a jump -- relative to the last unmodified
        sample, never to the press.
        """
        raw = (float(sample.x), float(sample.y))
        if self._rawCursor is None or self._effCursor is None:
            self._rawCursor = raw
            self._effCursor = raw
        factor = PRECISION_FACTOR if sample.has("shift") else 1.0
        eff = (self._effCursor[0] + (raw[0] - self._rawCursor[0]) * factor,
               self._effCursor[1] + (raw[1] - self._rawCursor[1]) * factor)
        self._rawCursor = raw
        self._effCursor = eff
        self._precision = factor != 1.0
        if eff == raw:
            return sample
        return type(sample)(getattr(sample, "_session", self.session),
                            sample.camera, eff[0], eff[1], sample.modifiers)

    def _buildReadout(self, tool, handle, sample):
        """The readout text for this sample (see dragReadout)."""
        planes = {tonicGizmo.HANDLE_PLANE_YZ: "YZ",
                  tonicGizmo.HANDLE_PLANE_XZ: "XZ",
                  tonicGizmo.HANDLE_PLANE_XY: "XY"}
        axes = (tonicGizmo.HANDLE_U, tonicGizmo.HANDLE_V, tonicGizmo.HANDLE_W)
        step = self._stepSize(sample)
        snapped = False
        grid = None
        if tool == "rotate":
            angle = (self._snappedDegrees if self._snappedDegrees is not None
                     else self._gizmo.dragAngle())
            text = "Rotate %.1f°" % angle
            snapped = self._snappedDegrees is not None
        elif tool == "scale":
            where = _AXIS_LETTERS.get(handle) or planes.get(handle) or ""
            text = "Scale %s%.2f" % (where + " " if where else "",
                                     self._lastScaleFactor)
            snapped = step > 0.0
        elif handle == tonicGizmo.HANDLE_RING:
            text = "Scale %.2f" % self._appliedScale
        elif handle == tonicGizmo.HANDLE_W and self.subMode() == "ring":
            text = "Twist %.1f°" % math.degrees(self._appliedTwist)
        else:
            delta = self._applied
            length = math.sqrt(sum(v * v for v in delta))
            # Ctrl on an axis is the plane whose normal is that axis
            # (parity G07), so the readout names the plane.
            ctrl = sample.has("ctrl") and handle in axes
            if self._constrain is not None:
                text = "Move normal %.3f" % sum(
                    delta[i] * self._constrain[i] for i in range(3))
            elif handle in axes and not ctrl:
                axis = self._unit(self._gizmo.axis(handle))
                along = (sum(delta[i] * axis[i] for i in range(3))
                         if axis is not None else length)
                text = "Move %s %.3f" % (_AXIS_LETTERS[handle], along)
            elif ctrl:
                normalPlane = {tonicGizmo.HANDLE_U: "YZ",
                               tonicGizmo.HANDLE_V: "XZ",
                               tonicGizmo.HANDLE_W: "XY"}
                text = "Move %s %.3f" % (normalPlane[handle], length)
            elif handle in planes:
                text = "Move %s %.3f" % (planes[handle], length)
            else:
                text = "Move %.3f" % length
            if sample.has("grid"):
                grid = float(self._gizmoSettings().gridSize)
            else:
                snapped = step > 0.0
        parts = [text]
        if self._precision:
            parts.append("Shift precision")
        if grid is not None:
            parts.append("grid %g" % grid)
        elif snapped:
            parts.append("snap step %g" % step)
        return " · ".join(parts)

    def _reportReadout(self):
        """The readout on the status line, at most every 50 ms."""
        if not self._readout:
            return
        now = time.monotonic()
        if self._readoutAt is not None and \
                now - self._readoutAt < READOUT_STATUS_INTERVAL:
            return
        self._readoutAt = now
        self._status("Tonic Tube: " + self._readout)

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
            elif abs(sample.x - x0) + abs(sample.y - y0) < \
                    tonicGizmo.clickSlopPixels(sample.camera):
                # Logical slop x the display ratio (G24): a one-logical-
                # pixel wobble on a 200 % display is still a click.
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
        if self._tweak and not self._pendingEdit and \
                self._tweakRemembered is not None:
            # A tweak released without travel was a selection click: the
            # centre it drove was never grabbed, so it neither stays
            # yellow nor arms the middle-drag repeat.
            self._gizmo.restoreRemembered(self._tweakRemembered)
        self._tweakRemembered = None
        self._gizmo.push(self.session)
        self._dragging = False
        # A tweak or a Ctrl/Shift centre press released without an edit was
        # a click, and a pending click candidate now selects.  Any drag that
        # edited nothing (a tweak click, a handle press without travel)
        # leaves no undo step (SS-02): its bracket is cancelled.
        click = not self._pendingEdit and (self._tweak or
                                           self._clickCandidate is not None)
        candidate = self._clickCandidate if click else None
        self._tweak = False
        self._clickCandidate = None
        if self._bracketOpen:
            self.session.endGestureIfChanged(self._pendingEdit)
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
        if candidate is not None:
            # The click table (selectModeFor): Ctrl removes, Shift toggles;
            # the centre handle no longer hides the component.
            self._selectItem(candidate[0], candidate[1])
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
            self._status(self._selectionStatus())
        self._placeGizmo(sample.camera)
        return True

    def cancel(self):
        """Escape: the press-time shape comes back, bit for bit."""
        self._tweak = False
        self._clickCandidate = None
        self._gizmo.setHoverHandle(tonicGizmo.HANDLE_NONE)
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
        if sample.camera is not None:
            self._lastCamera = sample.camera
        # The handle a press would grab prehighlights first (RigExec
        # _UpdateHover), and it hides the component hover under it: the
        # press goes to the handle, so a lit CV there would be a lie.
        handle = tonicGizmo.HANDLE_NONE
        if self._gizmo.visible and self.transformTool() != "select":
            handle = self._gizmo.handleAt(sample.camera, sample.x, sample.y)
        self._gizmo.setHoverHandle(handle)
        if handle != tonicGizmo.HANDLE_NONE:
            if self.session.setHover(0, -1, -1, -1):
                self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
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
        self._gizmo.resetHandles()
        self._gizmo.push(self.session)
        self.session.setHover(0, -1, -1, -1)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION |
                             tonicLib.TONIC_DIRTY_GIZMO)
        return True

    def activate(self):
        """GZ-08: entering Tube shows the gizmo on what is already selected.

        The controller follows with a camera-sized refreshGizmo; placing
        here keeps the native gizmo right for a caller without a view.
        """
        placed = self._placeGizmo(None)
        if placed:
            self.session.publish(tonicLib.TONIC_DIRTY_GIZMO)
        return placed

    def doubleClick(self, sample):
        """GZ-08: grow the selection one step from the component clicked.

        centre CV -> its whole tube, section CV -> its ring, ring -> every
        ring of its tube (the host application's double-click loop/shell select).  Never a
        level change: Hierarchy owns navigation.  Declined (the controller
        replays an ordinary press) when no component is under the cursor.
        """
        if self.session.model is None or self._dragging or \
                self._marquee is not None:
            return False
        item = self._componentItem(sample)
        if item is None:
            return False
        tubeId, subId = int(item["id"]), int(item["subId"])
        kind = int(item["kind"])
        if kind == tonicLib.TONIC_PICK_CENTER_CV:
            kind, rows = tonicLib.TONIC_PICK_TUBE_VERT, [(tubeId, -1, -1)]
        elif kind == tonicLib.TONIC_PICK_SECTION_CV:
            kind, rows = tonicLib.TONIC_PICK_SECTION_RING, [(tubeId, subId,
                                                             -1)]
        elif kind == tonicLib.TONIC_PICK_SECTION_RING:
            rows = [(tubeId, ring, -1)
                    for ring in range(self._tubeRingCount(tubeId))]
            if not rows:
                rows = [(tubeId, subId, -1)]
        else:
            return False
        # The click table's Shift/Ctrl+Shift grow the selection; a plain
        # double-click replaces it, as its first click already did.
        if selectModeFor(sample) == tonicLib.TONIC_SELECT_SET:
            self.session.clearSelection(self._selectableMask())
            mode = tonicLib.TONIC_SELECT_SET
        else:
            self._clearIncompatibleComponentKinds(kind)
            mode = tonicLib.TONIC_SELECT_ADD
        self.session.select(kind, [row[0] for row in rows],
                            [row[1] for row in rows],
                            [row[2] for row in rows], mode)
        self._pickedCV = None
        self._placeGizmo(sample.camera)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        self._status(self._selectionStatus())
        return True

    # -- selection ---------------------------------------------------------

    def _selectItem(self, item, modifiers):
        mode = selectModeFor(modifiers)
        if mode == tonicLib.TONIC_SELECT_SET:
            # One kind per call, so a plain click has to drop the others
            # itself or a center CV would stay selected under a ring.
            self.session.clearSelection(self._selectableMask())
        elif mode != tonicLib.TONIC_SELECT_REMOVE:
            # Only a click that can add needs one selection domain; a
            # remove leaves everything else exactly as it was.
            self._clearIncompatibleComponentKinds(item["kind"])
        selectItems(self.session, item["kind"], [item], mode)
        # A Shift toggle or a Ctrl remove may have deselected the CV: its
        # soft span must not put it (and its neighbours) straight back.
        if item["kind"] == tonicLib.TONIC_PICK_CENTER_CV and \
                self._itemSelected(item):
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
        return self._applyArea(
            sample, lambda mask, mode: self.session.selectRect(
                sample.camera, x0, y0, sample.x, sample.y, mask, mode))

    def _moveLasso(self, sample):
        if len(self._lasso) < 3:
            return self._moveMarquee(sample)
        points = list(self._lasso)
        return self._applyArea(
            sample, lambda mask, mode: self.session.selectPolygon(
                sample.camera, points, mask, mode))

    def _applyArea(self, sample, query):
        """One box/lasso over this sub-mode's component kind.

        `query(kindMask, mode)` is the rect or polygon select; the modifier
        table (selectBand) decides what it does to the selection.
        """
        return self._areaSelect(selectModeFor(sample, band=True), query)

    def _areaSelect(self, mode, query, invert=False):
        """`_applyArea` for an explicit mode; `invert` flips instead (SL-03
        Ctrl+I), keeping this sub-mode's own kind the way Shift-add does."""
        self._prepareAreaComponentDomain(mode)
        if self.subMode() == "ring":
            kind = tonicLib.TONIC_PICK_SECTION_RING

            def band(bandMode):
                if bandMode == tonicLib.TONIC_SELECT_SET:
                    # Rings come from the section vertices the band hits;
                    # a band that hits none must still replace the old
                    # rings (and tell a Ctrl band it covered nothing).
                    self.session.clearSelection(kind)
                ok = query(tonicLib.TONIC_PICK_SECTION_CV, bandMode)
                self._normalizeRingAreaSelection(bandMode)
                return ok
        else:
            kind = self.componentMask

            def band(bandMode):
                return query(kind, bandMode)
        if invert:
            invertBand(self.session, kind, band)
        else:
            selectBand(self.session, kind, mode, band)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    # -- SL-03: Ctrl+A / Ctrl+Shift+A / Ctrl+I -------------------------------

    def selectAll(self, camera):
        """Every visible component of this sub-mode's kind (the F8-F11 row)."""
        if self.session.model is None or camera is None:
            return False
        self._pickedCV = None
        self._areaSelect(tonicLib.TONIC_SELECT_SET, self._viewQuery(camera))
        self._placeGizmo(camera)
        self._status(self._selectionStatus())
        return True

    def deselectAll(self, camera):
        if self.session.model is None:
            return False
        self._pickedCV = None
        # Every kind, as the old Escape did: a Graph node or guide selected
        # in another mode must not come back when the artist returns there.
        self.session.clearSelection(0)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        self._placeGizmo(camera)
        self._status(self._selectionStatus())
        return True

    def invertSelection(self, camera):
        if self.session.model is None or camera is None:
            return False
        self._pickedCV = None
        self._areaSelect(tonicLib.TONIC_SELECT_ADD, self._viewQuery(camera),
                         invert=True)
        self._placeGizmo(camera)
        self._status(self._selectionStatus())
        return True

    def _viewQuery(self, camera):
        """`query(mask, mode)` over the whole view (see viewBand)."""
        return lambda mask, mode: viewBand(self.session, camera, mask)(mode)

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
        elif mode == tonicLib.TONIC_SELECT_REMOVE:
            # A Ctrl band only takes away; clearing anything here would
            # remove what the band never touched.
            return
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
        if self._gizmo.visible and self._gizmo.locked:
            # GZ-06: the dimmed gizmo needs its reason on the same line.
            hint = PINNED_ROOT_STATUS
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
        self._lastScaleFactor = 1.0
        # GZ-07: precision integrates from the press; the readout starts
        # empty and the first sample reaches the status line at once.
        self._rawCursor = (float(sample.x), float(sample.y))
        self._effCursor = self._rawCursor
        self._precision = False
        self._readout = ""
        self._readoutAt = None
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
        """Ctrl on a screen-plane drag pins it to the tube's root normal.

        Not in Ring/Section (GZ-06): a section chart is two dimensional and
        the root normal is roughly the ring's own normal, so pinning to it
        would leave almost nothing for the chart to take -- Ctrl there
        stays the free in-plane move.
        """
        if handle != tonicGizmo.HANDLE_CENTER or not sample.has("ctrl"):
            return None
        if self.subMode() in ("ring", "section"):
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
        # Individual Origins (the default): each owner gets its own pivot.
        # This is why a selected L2 child bends/scales about its root
        # without moving a sibling or its parent.  Selection Centre (parity
        # G17): every owner takes the ONE point the gizmo was placed on
        # (_transformPivot's bounds midpoint), read back from the gizmo so
        # the drawn pivot is the pivot used.
        shared = (tuple(self._gizmo.pressOrigin) if self._centrePivot()
                  else None)
        for tubeId, (cvs, anchor) in sorted(self._centerDrag.items()):
            centers = self._centers(tubeId)
            if not centers:
                continue
            handles = self._centerHandles(tubeId)
            # The visible root core is the transform pivot advertised by the
            # gizmo. Center coordinates below remain raw authored values so
            # the native CV delta writes and soft weighting keep their ABI.
            pivot = shared if shared is not None else \
                (handles[0] if handles else centers[0])
            ids = set(int(cv) for cv in cvs if 0 <= int(cv) < len(centers))
            # The root stays on the scalp unless it is itself selected under
            # a shared pivot, where turning it about the centre IS the edit
            # (about its own root it would only drift by the core offset).
            pinRoot = shared is None or 0 not in ids
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
                                           "weights": weights,
                                           "pinRoot": pinRoot})

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
            # Selection Centre applies to the rings the artist picked
            # (Ring/Section).  A whole-tube Scale's rings keep their own
            # centroid: the centre curve already carries them about the
            # shared pivot, and scaling their charts about it too would
            # move them twice.  A rotation about an axis parallel to W
            # through any point keeps a chart in its plane.
            ringPivot = (shared if shared is not None and
                         self.subMode() in ("ring", "section")
                         else frame["origin"])
            frozen = tonicTubeTransforms.FrozenPoints(points, ringPivot,
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
                    if int(cv) == 0 and owner.get("pinRoot", True):
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

    # -- snapping (parity G11) ---------------------------------------------

    def _stepSize(self, sample=None):
        """The live tool's Step Size, or 0.0 when no step snap applies.

        Step Snap is the panel option (sticky) or `J` held during the drag
        (the controller adds "stepSnap" to the sample's modifiers), exactly
        RigExec's TranslateSnap/_SnapStep rule.
        """
        settings = self.toolSettings()
        held = sample is not None and sample.has("stepSnap")
        if not (bool(settings.stepSnap) or held):
            return 0.0
        step = float(settings.stepSize)
        return step if step > 0.0 else 0.0

    def _snapReference(self, handle, ctrl):
        """What GridPoint/ConstrainToHandle see for the live handle.

        RigExec hands them the vendored Handle; Tonic's root-normal
        constraint (Ctrl on the centre) is a line, so it snaps as an axis
        along that normal rather than as the free centre.
        """
        if self._constrain is not None:
            return types.SimpleNamespace(kind="axis",
                                         worldAxis=self._constrain,
                                         worldNormal=None)
        if handle in (tonicGizmo.HANDLE_U, tonicGizmo.HANDLE_V,
                      tonicGizmo.HANDLE_W):
            return types.SimpleNamespace(kind="axis",
                                         worldAxis=self._gizmo.axis(handle),
                                         worldNormal=None)
        normals = {tonicGizmo.HANDLE_PLANE_YZ: tonicGizmo.HANDLE_U,
                   tonicGizmo.HANDLE_PLANE_XZ: tonicGizmo.HANDLE_V,
                   tonicGizmo.HANDLE_PLANE_XY: tonicGizmo.HANDLE_W}
        if handle in normals:
            return types.SimpleNamespace(
                kind="plane", worldAxis=None,
                worldNormal=self._gizmo.axis(normals[handle]))
        return types.SimpleNamespace(kind="center", worldAxis=None,
                                     worldNormal=None)

    def _snapTranslate(self, sample, delta, ctrl):
        """A translate delta after the X grid hold or the step snap.

        `X` (held) lands the PIVOT on the world grid -- only along what the
        handle may move, via the vendored GridPoint + ConstrainToHandle --
        and wins over a step snap as in RigExec.  Step Snap / `J` is the host application's
        Discrete Move: the delta advances in whole steps along the gizmo's
        own axes, so a selection that began off the grid stays off it.
        """
        handle = self._gizmo.activeHandle
        if sample.has("grid"):
            grid = float(self._gizmoSettings().gridSize)
            reference = self._snapReference(handle, ctrl)
            pivot = self._gizmo.pressOrigin
            unsnapped = tuple(pivot[i] + delta[i] for i in range(3))
            target = tonicGizmoSnap.GridPoint(reference, pivot, unsnapped,
                                              grid, ctrl)
            target = tonicGizmoSnap.ConstrainToHandle(reference, pivot,
                                                      target, ctrl)
            return tuple(target[i] - pivot[i] for i in range(3))
        step = self._stepSize(sample)
        if step <= 0.0:
            return delta
        if self._constrain is not None:
            axis = self._constrain
            along = tonicGizmoScreen.SnapRelative(
                sum(delta[i] * axis[i] for i in range(3)), step)
            return tuple(axis[i] * along for i in range(3))
        axes = [self._unit(self._gizmo.axis(i)) for i in range(3)]
        if any(axis is None for axis in axes):
            return delta
        parts = tonicGizmoScreen.SnapRelative(
            tuple(sum(delta[i] * axis[i] for i in range(3))
                  for axis in axes), step)
        return tuple(sum(axes[k][i] * parts[k] for k in range(3))
                     for i in range(3))

    def _applyRotation(self, sample):
        self._snappedDegrees = None
        if not self._transformOwners:
            return False
        rotation = self._gizmo.rotationDrag(sample.camera, sample.x, sample.y)
        if rotation is None:
            return False
        axis, radians = rotation
        axis = self._unit(axis)
        if axis is None:
            return False
        step = self._stepSize(sample)
        if step > 0.0:
            # a DCC's Snap Rotate: the swept angle in whole steps (degrees).
            self._snappedDegrees = tonicGizmoScreen.SnapRelative(
                math.degrees(radians), step)
            radians = math.radians(self._snappedDegrees)
        # The wedge ends where the target ended (RigExec _DisplayAngle):
        # the snapped angle while a step applies, the raw sweep otherwise.
        self._gizmo.setDisplayAngle(self._snappedDegrees)
        # A ring chart has one valid rotation: its normal (W).  Center
        # curves may use every rotate axis, but a section never leaves plane.
        if (self._gizmo.activeHandle != tonicGizmo.HANDLE_W and any(
                owner["kind"] == "section" for owner in self._transformOwners)):
            return False
        return self._applyFrozenTargets(rotateAxis=axis, radians=radians)

    def _applyScale(self, sample):
        if not self._transformOwners:
            return False
        # Parity G08: dragging an axis or square onto the pivot gives 0 and
        # through it a mirrored negative factor, as in a DCC, unless Prevent
        # Negative Scale clamps it at MIN_SCALE_FACTOR.  A sample through
        # the pivot is applied, not dropped, so the drag never sticks.
        allowNegative = not bool(self.toolSettings().preventNegativeScale)
        factor = float(self._gizmo.scaleFactor(sample.camera, sample.x,
                                               sample.y,
                                               allowNegative=allowNegative))
        step = self._stepSize(sample)
        if step > 0.0:
            # Tonic has no scale channel: the step quantises the factor.
            factor = tonicGizmoScreen.SnapRelative(factor, step)
            if not allowNegative:
                factor = max(factor, tonicGizmoScreen.MIN_SCALE_FACTOR)
        if factor != factor:             # NaN from a degenerate projection
            return False
        self._lastScaleFactor = factor   # the GZ-07 readout
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
        """The world delta since the press, constrained and snapped.

        Ctrl is read on every sample (parity G07, RigExec DragState): on an
        axis handle it moves in the plane PERPENDICULAR to that axis, and
        letting go mid-drag returns to the axis.  Ctrl on the centre handle
        is Tonic's own root-normal constraint, fixed at the press.
        """
        handle = self._gizmo.activeHandle
        ctrl = sample.has("ctrl") and handle in (
            tonicGizmo.HANDLE_U, tonicGizmo.HANDLE_V, tonicGizmo.HANDLE_W)
        delta = self._gizmo.drag(sample.camera, sample.x, sample.y,
                                 ctrl=ctrl)
        axis = self._constrain
        if axis is not None:
            along = sum(delta[i] * axis[i] for i in range(3))
            delta = (axis[0] * along, axis[1] * along, axis[2] * along)
        return self._snapTranslate(sample, delta, ctrl)

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
        """Delete: the selected center CVs, else the selected rings, else
        the selected whole tubes (Whole Tube / F8, SL-03)."""
        if self.session.model is None or self._stage is None:
            return False
        centers = self.session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
        rings = self.session.readSelection(tonicLib.TONIC_PICK_SECTION_RING)
        if not centers and not rings:
            tubes = [t for t, _s, _ss in self.session.readSelection(
                tonicLib.TONIC_PICK_TUBE_VERT)]
            if not tubes:
                return False
            removed, roots, error = deleteWholeTubes(self.session, tubes)
            self._placeGizmo(None)
            self._status(deleteTubesStatus("Tonic Tube", removed, roots,
                                           error))
            return bool(removed)
        if not self.session.beginGesture("Tube delete"):
            # Someone else's bracket is open: deleting inside it would
            # land in that step and the endGesture below would seal it.
            self._status("Tonic Tube: delete could not start an undo step "
                         "-- nothing removed")
            return False
        removed = 0
        refusals = []
        # Descending, so an earlier removal cannot shift a later index.
        calls = ([(self._stage.deleteCenterCV, tubeId, cv)
                  for tubeId, cv, _ss in sorted(centers, reverse=True)] +
                 [(self._stage.removeSectionRing, tubeId, ring)
                  for tubeId, ring, _ss in sorted(rings, reverse=True)])
        for entry, tubeId, index in calls:
            try:
                entry(self.session.model, tubeId, index)
                removed += 1
            except RuntimeError as exc:
                refusals.append(str(exc))
        # Every removal refused (a 2-CV tube, a 2-ring tube): no empty
        # step, the selection stays, nothing to commit.
        if not self.session.endGestureIfChanged(removed > 0):
            self._status("Tonic Tube: nothing removed -- %s"
                         % (refusals[0] if refusals else "no removal ran"))
            return False
        self.session.clearSelection(self._selectableMask())
        self._placeGizmo(None)
        self.session.enqueueCommit()
        if refusals:
            self._status("Tonic Tube: removed %d, %d refused: %s"
                         % (removed, len(refusals), refusals[0]))
        else:
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
