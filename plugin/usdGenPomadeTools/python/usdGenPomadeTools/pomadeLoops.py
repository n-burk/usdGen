# usdGenPomadeTools.pomadeLoops -- one tool loop per mode (plan/18 section
# 3.1, section 3.6).
#
# Qt-free by rule: the loops see pixels, modifiers and a camera, never a
# QEvent. pomadeViewport resolves the camera once per gesture and hands each
# loop a Sample; the loop does the model work through PomadeSession and says
# whether it claimed the event.
#
# Two things changed from the P2 GraphController this replaces:
#   * every surface sample is a K1 Pomade_Raycast, not a Hydra
#     StageView.pick (plan/18 finding F6: ~1.3 ms per sample, once per
#     press at most by plan/08 section 6.1);
#   * every node / edge / region hit is a K11 Pomade_PickItem, which is
#     screen-space and occlusion-aware, instead of the world-space snap
#     queries that could not tell a node behind the head from one in front.
from __future__ import annotations

import ctypes

from . import pomadeGizmo
from . import pomadeLib
from . import pomadeModes

# The pick radius floor, so a zero snap radius still catches something.
MIN_PICK_RADIUS_PX = 2.0
# Displayed component glyphs are direct-manipulation targets.  Their hit
# target does not inherit the graph Snap preference: a deliberately tiny
# weld radius must not make visible CVs effectively unclickable.
COMPONENT_PICK_RADIUS_PX = 8.0
# Region authoring selects published CV glyphs, rather than arbitrary graph
# primitives below them.  Keep that screen target usable when Snap is tiny;
# Snap still controls graph welding and the authoring radius in world space.
REGION_NODE_PICK_RADIUS_PX = COMPONENT_PICK_RADIUS_PX
# Reposition is direct manipulation, so its visible CV/edge affordances
# remain comfortably reachable even when the graph snap preference is tiny.
# Keep these fixed: snapping may intentionally be much larger or smaller.
REPOSITION_NODE_PICK_RADIUS_PX = 8.0
REPOSITION_EDGE_PICK_RADIUS_PX = 5.0
# Graph sub-modes whose press acts on an existing node/edge/region; a press
# on nothing there starts a node marquee. The authoring sub-modes (draw,
# region, place, reposition) press on empty scalp on purpose.
CLICK_SUBMODES = ("connect", "weld", "unweld", "delete", "link")


class Sample:
    """One mouse position, resolved lazily against the model.

    A press that only needs a node id never pays for a raycast, and a move
    that only needs the surface never pays for a pick. Both are cached for
    the lifetime of the sample, which is one event.
    """

    __slots__ = ("x", "y", "camera", "modifiers", "_session", "_surface",
                 "_surfaceDone", "_items")

    def __init__(self, session, camera, x, y, modifiers=frozenset()):
        self._session = session
        self.camera = camera
        self.x = float(x)
        self.y = float(y)
        self.modifiers = frozenset(modifiers)
        self._surface = None
        self._surfaceDone = False
        self._items = {}

    def surface(self):
        """The K1 scalp hit under the cursor, or None."""
        if not self._surfaceDone:
            self._surfaceDone = True
            ray = (self.camera.rayThrough(self.x, self.y)
                   if self.camera is not None else None)
            if ray is not None:
                self._surface = self._session.raycast(ray[0], ray[1])
        return self._surface

    def item(self, kindMask, radiusPx):
        """The K11 selection item under the cursor for `kindMask`."""
        key = (int(kindMask), round(float(radiusPx), 3))
        if key not in self._items:
            self._items[key] = self._session.pickItem(
                self.camera, self.x, self.y, radiusPx, kindMask)
        return self._items[key]

    def has(self, name):
        return name in self.modifiers


# -- the selection-modifier table ---------------------------------------------
#
# One convention for every loop, click and band, so a modifier means the
# same thing in Graph, Tube, Fill and Hierarchy (a DCC: Shift toggles, Ctrl
# deselects, Ctrl+Shift adds; a DCC: Shift extends, Ctrl-box subtracts):
#
#   gesture       none   Shift    Ctrl     Ctrl+Shift
#   click         SET    TOGGLE   REMOVE   ADD
#   box / lasso   SET    ADD      REMOVE   ADD
#
# The loops decide only WHEN a gesture is a click or a band; what the
# modifiers make of it lives here and nowhere else.

def selectModeFor(modifiers, band=False):
    """The POMADE_SELECT_* a click (or, with band=True, a box/lasso) means.

    `modifiers` is a Sample or any container of modifier names.
    """
    modifiers = getattr(modifiers, "modifiers", modifiers) or ()
    shift = "shift" in modifiers
    ctrl = "ctrl" in modifiers
    if ctrl and shift:
        return pomadeLib.POMADE_SELECT_ADD
    if ctrl:
        return pomadeLib.POMADE_SELECT_REMOVE
    if shift:
        return (pomadeLib.POMADE_SELECT_ADD if band
                else pomadeLib.POMADE_SELECT_TOGGLE)
    return pomadeLib.POMADE_SELECT_SET


def _selectionEntry(item):
    """(id, subId, subSubId) from a K11 pick dict or a selection tuple."""
    if isinstance(item, dict):
        return (int(item.get("id", -1)), int(item.get("subId", -1)),
                int(item.get("subSubId", -1)))
    entry = tuple(int(v) for v in item)
    return (entry + (-1, -1, -1))[:3]


def _selectionKey(kind, entry):
    # A whole tube is one thing however it was picked: only its id counts.
    if int(kind) == pomadeLib.POMADE_PICK_TUBE_VERT:
        return (int(entry[0]),)
    return tuple(int(v) for v in entry[:3])


def _kindsIn(kindMask):
    mask = int(kindMask)
    bit = 1
    while bit <= mask:
        if mask & bit:
            yield bit
        bit <<= 1


def _selectEntries(session, kind, entries, mode):
    return session.select(kind, [e[0] for e in entries],
                          [e[1] for e in entries], [e[2] for e in entries],
                          mode)


def applyRemove(session, kind, items):
    """Click-remove: deselect whichever of `items` is selected now.

    The C ABI has no subtract (POMADE_SELECT_REMOVE is Python-only), so this
    TOGGLEs exactly the selection's own entries that match: toggling an
    unselected item would ADD it, the opposite of what Ctrl means.
    """
    targets = {_selectionKey(kind, _selectionEntry(item)) for item in items}
    hit = [_selectionEntry(entry) for entry in session.readSelection(kind)
           if _selectionKey(kind, _selectionEntry(entry)) in targets]
    if not hit:
        return False
    return _selectEntries(session, kind, hit, pomadeLib.POMADE_SELECT_TOGGLE)


def selectItems(session, kind, items, mode):
    """Apply one click's items of one kind through the modifier table."""
    if int(mode) == pomadeLib.POMADE_SELECT_REMOVE:
        return applyRemove(session, kind, items)
    return _selectEntries(session, kind,
                          [_selectionEntry(item) for item in items], mode)


def selectBand(session, kindMask, mode, band):
    """Apply one box/lasso through the modifier table.

    `band(mode)` runs the rect/polygon query with a real Pomade_Select*
    mode. REMOVE runs it as SET to learn what the band covers (B), then
    puts back what was selected before (S) minus B, kind by kind.
    """
    if int(mode) != pomadeLib.POMADE_SELECT_REMOVE:
        return band(mode)
    if not int(kindMask):
        return False             # clearSelection(0) would mean "every kind"
    before = {kind: [_selectionEntry(entry)
                     for entry in session.readSelection(kind)]
              for kind in _kindsIn(kindMask)}
    if not band(pomadeLib.POMADE_SELECT_SET):
        # The query failed before touching the selection; reading it back
        # now would call everything "covered" and remove it all.
        return False
    covered = {kind: {_selectionKey(kind, _selectionEntry(entry))
                      for entry in session.readSelection(kind)}
               for kind in before}
    session.clearSelection(kindMask)
    for kind, entries in before.items():
        keep = [entry for entry in entries
                if _selectionKey(kind, entry) not in covered[kind]]
        if keep:
            _selectEntries(session, kind, keep, pomadeLib.POMADE_SELECT_ADD)
    return True


class ToolLoop:
    """What every mode's viewport loop must answer.

    press / move / release / cancel / hover return True when the loop
    claimed the event; the controller passes anything unclaimed through to
    usdview, which is how camera navigation keeps working.
    """

    modeId = ""
    label = ""
    pickMask = 0
    subModes = ()
    defaultSubMode = ""

    def __init__(self, session, state):
        self.session = session
        self.state = state

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return ""

    def setSubMode(self, subId):
        return ""

    def subModeForHotkey(self, letter):
        letter = str(letter).upper()
        for mode in self.subModes:
            if mode.hotkey.upper() == letter:
                return mode.id
        return ""

    # -- gesture -----------------------------------------------------------

    def press(self, sample):
        return False

    def move(self, sample):
        return False

    def release(self, sample):
        return False

    def cancel(self):
        return False

    def hover(self, sample):
        return False

    def clearHover(self):
        """The pointer left the view: drop loop-owned hover drawing."""
        return False

    def marqueeRect(self):
        """The live selection band's physical-pixel origin, if any."""
        return getattr(self, "_marquee", None)

    def deactivate(self):
        """The mode is being left: drop anything this loop put on screen.

        A gizmo or a brush ring is model state (plan/18 section 2.4), so
        it outlives the loop that set it unless the loop takes it away.
        """
        return False

    def activate(self):
        """The mode became current after construction (optional hook)."""
        return False

    # -- keys --------------------------------------------------------------

    def deleteSelection(self):
        return False

    def adjustRadius(self, delta):
        return ""

    def statusLine(self):
        return ""

    # -- helpers -----------------------------------------------------------

    def pickRadiusPx(self):
        return max(float(self.state.snapRadiusPx), MIN_PICK_RADIUS_PX)


class GraphLoop(ToolLoop):
    """Graph mode: draw, place, connect, weld, unweld, delete, link.

    Every behaviour of the P2 GraphController, with its two Hydra-era
    shortcuts replaced (see the module docstring) and the gesture bracket,
    Escape and marquee it never had.
    """

    modeId = "graph"
    label = "Graph"
    pickMask = (pomadeLib.POMADE_PICK_GRAPH_NODE |
                pomadeLib.POMADE_PICK_GRAPH_EDGE |
                pomadeLib.POMADE_PICK_REGION)
    subModes = pomadeModes.GRAPH_SUBMODES
    defaultSubMode = "region"

    def __init__(self, session, state):
        super(GraphLoop, self).__init__(session, state)
        self._stroke = []          # (face, u, v) samples of the live stroke
        self._dragNode = -1
        self._dropPoint = None
        self._clickNode = -1
        self._clickRegion = -1
        # Region creation deliberately keeps these CVs out of the model
        # until it closes.  Each record is a K1 hit plus the physical pixel
        # at which it was placed; the latter is only for clicking the first
        # draft CV to close and for the viewport's transient overlay.
        self._regionDraft = []
        self._regionHover = None
        # The hover sits on the first draft CV of a closable (3+ CV) draft:
        # the rubber band is snapped there and a click would close it.
        self._closeArmed = False
        self._regionCloseRequested = False
        self._marquee = None       # (x0, y0) while a rubber band is live
        self._active = False
        self._bracketOpen = False  # an undo bracket is open on the model
        self._pendingEdit = False  # the gesture changed the model
        self._editFinished = False  # _finishEdit's K3 ran; endOfEdit due
        self._lastStatus = ""
        # Reposition uses canonical scalp locations as a frozen press-time
        # baseline.  The visible graph glyph may be lifted from that scalp;
        # applying the cursor's surface delta to this baseline avoids a jump.
        self._repositionIds = ()
        self._repositionPoints = ()
        self._repositionAnchor = None
        self._repositionLastDelta = None

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return self.state.graphSubMode or self.defaultSubMode

    def setSubMode(self, subId):
        # A mode switch is allowed while the pointer is down.  Reposition
        # owns an open native bracket in that state, so restore it before
        # changing the mode that release() will inspect.
        if self._active and self.subMode() == "reposition":
            self.cancel()
        if (self.subMode() in ("region", "reposition") and
                subId != self.subMode()):
            self._setGraphHover(None)
        if subId != self.subMode():
            self._clearRegionDraft()
        status = pomadeModes.SetActiveGraphSubMode(self.state, subId)
        if status:
            self._disarm()
        return status

    def deactivate(self):
        """Drop a non-model region draft when Graph loses focus."""
        if self._active and self.subMode() == "reposition":
            self.cancel()
        if self.subMode() in ("region", "reposition"):
            self._setGraphHover(None)
        disarmed = self._disarm()
        return self._clearRegionDraft() or disarmed

    # -- the two-click pick (connect, weld, link) --------------------------

    def _armed(self):
        return self._clickNode >= 0 or self._clickRegion >= 0

    def _arm(self, kind, ident):
        """Hold the first pick of a two-click action and draw it.

        Hover follows the cursor, so it cannot mark a pick the artist has
        already made; the selection can, and it is what Escape clears.
        """
        if kind == pomadeLib.POMADE_PICK_REGION:
            self._clickRegion = int(ident)
        else:
            self._clickNode = int(ident)
        self.session.select(kind, [int(ident)], None, None,
                            pomadeLib.POMADE_SELECT_SET)
        self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)

    def _disarm(self, clearSelection=True):
        """Forget a pending first pick; True when one was armed.

        `clearSelection` also takes back the selection _arm drew it with.
        A modifier press keeps it: the armed node then simply stays one
        of the selected nodes the artist is extending.
        """
        kind = (pomadeLib.POMADE_PICK_GRAPH_NODE if self._clickNode >= 0 else
                pomadeLib.POMADE_PICK_REGION if self._clickRegion >= 0 else 0)
        self._clickNode = -1
        self._clickRegion = -1
        if not kind:
            return False
        if clearSelection and self.session.model is not None:
            self.session.clearSelection(kind)
            self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        return True

    def draftRegionPreview(self):
        """The transient contour consumed by pomadeViewport's Qt overlay."""
        return {"points": [self._draftDisplayPoint(entry)
                            for entry in self._regionDraft],
                "hover": self._regionHover,
                "closeArmed": self._closeArmed}

    def _clearRegionDraft(self):
        hadDraft = bool(self._regionDraft or self._regionHover or
                        self._regionCloseRequested)
        self._regionDraft = []
        self._regionHover = None
        self._closeArmed = False
        self._regionCloseRequested = False
        return hadDraft

    def _trackRegionHover(self, nodeId, hit, sample):
        """Move the draft's rubber band end to what a click here would add.

        Over the first CV the end snaps onto it, so the closing segment the
        artist is about to author is drawn exactly, not a few pixels off.
        `closeArmed` needs 3 CVs because a press there closes nothing
        before that (it says "region needs at least 3 CVs" instead).
        """
        if not self._regionDraft:
            self._regionHover = None
            self._closeArmed = False
            return
        if self._regionFirstMatches(nodeId, sample):
            self._regionHover = self._draftDisplayPoint(self._regionDraft[0])
            armed = len(self._regionDraft) >= 3
            if armed and not self._closeArmed:
                # Once per approach, not per mouse move.
                self._status("Pomade Graph: click to close the region (%d CVs)"
                             % len(self._regionDraft))
            self._closeArmed = armed
            return
        self._closeArmed = False
        self._regionHover = (None if hit is None else
                             (self._graphNodeDisplayPoint(nodeId) or
                              hit["point"]))

    # -- status ------------------------------------------------------------

    def _status(self, text):
        self._lastStatus = text
        self.session.report(text)

    def statusLine(self):
        from .pomadeGraph import hudStatus
        nodes, edges, regions = self.session.graphCounts()
        _r, uncovered, intersected = self.session.regionStats()
        return hudStatus((nodes, edges, regions),
                         (regions, uncovered, intersected),
                         self.state.mapVersion, self.state.bakedVersion,
                         fallbackReason=self.session.fallbackReason())

    # -- geometry helpers --------------------------------------------------

    def _bracket(self, label, action):
        """Run one mutating click as a single undo step (SS-02).

        The edit, the K3 pass after it and the tube stub of any region it
        closed all land inside this one bracket, so one Ctrl+Z takes the
        whole click back. `action()` answers whether the model changed; a
        refused or no-op action leaves no undo step at all. Returns that
        answer; the caller runs endOfEdit() once the bracket is sealed.
        """
        if not self.session.beginGesture(label):
            return False
        changed = False
        try:
            changed = bool(action())
            if changed:
                self._finishEdit()
        finally:
            self.session.endGestureIfChanged(changed)
        if changed and self._active:
            self._pendingEdit = True     # release() runs endOfEdit()
        return changed

    def _snapRest(self, sample):
        """The snap radius in rest units at the sampled depth."""
        from .pomadeGraph import snapRadiusRest
        hit = sample.surface()
        return snapRadiusRest(self.session, self.state, sample.camera,
                              hit["point"] if hit else None)

    def _nodeAt(self, sample):
        item = sample.item(pomadeLib.POMADE_PICK_GRAPH_NODE,
                           self.pickRadiusPx())
        return item["id"] if item else -1

    def _graphNodeHit(self, nodeId):
        """The exact scalp location of a stable graph node, or None.

        K11 names the node even where the CV dot's visible silhouette is a
        few pixels wider than the scalp hit below it.  Looking up that id
        gives the draft overlay and GraphCreateRegion the node's actual
        location instead of treating a nearby K1 hit as a new CV.
        """
        entry = getattr(self.session.dll, "Pomade_GraphGetNode", None)
        if nodeId < 0 or entry is None:
            return None
        face = ctypes.c_int(-1)
        uv = (ctypes.c_float * 2)()
        point = (ctypes.c_float * 3)()
        if entry(self.session.model, int(nodeId), ctypes.byref(face), uv,
                 point) != pomadeLib.POMADE_OK:
            return None
        canonical = (float(point[0]), float(point[1]), float(point[2]))
        # The committed glyph is lifted off the scalp.  Keep its display
        # position in the transient overlay too, while face/UV below remain
        # the canonical coordinates passed to GraphCreateRegion.
        return {"face": int(face.value), "u": float(uv[0]),
                "v": float(uv[1]), "point": canonical}

    def _graphNodeDisplayPoint(self, nodeId):
        """The published (lifted) graph glyph position, if this ABI has it."""
        entry = getattr(self.session.dll, "Pomade_GraphGetNodeDisplayPosition",
                        None)
        if nodeId < 0 or entry is None:
            return None
        point = (ctypes.c_float * 3)()
        if entry(self.session.model, int(nodeId), point) != pomadeLib.POMADE_OK:
            return None
        return (float(point[0]), float(point[1]), float(point[2]))

    def _regionHitAt(self, sample):
        """(stable node id, canonical hit) for one region click/hover."""
        # K11 sees the lifted published glyph.  Use the same bounded target
        # for hover and press so a click six pixels into a visible CV cannot
        # fall through to an edge/region or mint a duplicate node.
        item = sample.item(pomadeLib.POMADE_PICK_GRAPH_NODE,
                           REGION_NODE_PICK_RADIUS_PX)
        nodeId = item["id"] if item else -1
        if nodeId >= 0:
            return nodeId, self._graphNodeHit(nodeId)
        return -1, sample.surface()

    def _draftDisplayPoint(self, entry):
        """Published nodes use their lifted glyph; new draft CVs are raw."""
        return self._graphNodeDisplayPoint(entry[6]) or entry[3]

    def _edgeAt(self, sample):
        item = sample.item(pomadeLib.POMADE_PICK_GRAPH_EDGE,
                           self.pickRadiusPx())
        return item["id"] if item else -1

    def _repositionTarget(self, sample):
        """The direct-manipulation target under a Reposition cursor.

        K11 picks each component class independently.  Testing nodes first
        gives a CV within its usable screen handle priority over an edge that
        happens to pass directly below it.  Regions are never targets here.
        """
        item = sample.item(pomadeLib.POMADE_PICK_GRAPH_NODE,
                           REPOSITION_NODE_PICK_RADIUS_PX)
        if item:
            return item
        return sample.item(pomadeLib.POMADE_PICK_GRAPH_EDGE,
                           REPOSITION_EDGE_PICK_RADIUS_PX)

    def _setGraphHover(self, item):
        if item:
            changed = self.session.setHover(item["kind"], item["id"],
                                            item["subId"], item["subSubId"])
        else:
            changed = self.session.setHover(0, -1, -1, -1)
        if changed:
            self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        return changed

    def _regionAt(self, sample):
        """The exact sub-face region under the cursor, if any.

        A face-region map is deliberately not a fallback: a single scalp
        face may cross a contour, and its coarse id would select a region
        outside the actual click point.  Pomade_RegionAtSurface performs the
        polygon containment test from the K1 face/UV hit instead.
        """
        hit = sample.surface()
        entry = getattr(self.session.dll, "Pomade_RegionAtSurface", None)
        if hit is None or entry is None:
            return -1
        return int(entry(self.session.model, int(hit["face"]),
                         ctypes.c_float(hit["u"]),
                         ctypes.c_float(hit["v"])))

    # -- gesture -----------------------------------------------------------

    def press(self, sample):
        if self.session.model is None:
            return False
        # A selection modifier makes the press a node selection: a band on
        # travel, a click through the modifier table without (no Graph
        # authoring gesture reads Shift or Ctrl).
        if selectModeFor(sample) != pomadeLib.POMADE_SELECT_SET:
            self._disarm(clearSelection=False)
            return self._pressMarquee(sample)
        sub = self.subMode()
        target = None
        if sub in CLICK_SUBMODES:
            # The click sub-modes act on what is under the cursor, so a
            # press on nothing is free: it boxes nodes like any other
            # tool's empty drag. An armed first pick keeps the old miss
            # (a status line) so a slightly short second click does not
            # throw the pick away; Escape is how it is dropped.
            target = self._clickTarget(sample, sub)
            if target[1] < 0 and not self._armed():
                return self._pressMarquee(sample)
        self._active = True
        self._pendingEdit = False
        # A drag holds one bracket open for its whole life. A click mode
        # cannot know at press time whether it will mutate anything, and a
        # bracket opened on every arming click would litter the history
        # with steps that undo to the state they were taken from, so those
        # wrap their own mutation instead (_bracket below).
        if sub in ("draw", "place"):
            # A refused Begin means another owner's bracket is open; the
            # stroke must not run inside it, and release/Escape must not
            # later seal or roll back a bracket this press never opened.
            # The press is still claimed, like _pressReposition's refusal.
            if not self.session.beginGesture("Graph %s" % sub):
                self._active = False
                self._status("Pomade Graph %s: another edit is still open "
                             "-- nothing drawn" % sub)
                return True
            self._bracketOpen = True
        if sub == "reposition":
            return self._pressReposition(sample)
        if sub == "draw":
            hit = sample.surface()
            self._stroke = ([(hit["face"], hit["u"], hit["v"])] if hit
                            else [])
            return True
        if sub == "region":
            return self._pressRegion(sample)
        if sub == "place":
            return self._pressPlace(sample)
        if sub in ("connect", "weld"):
            return self._pressClickNode(target[1], sub)
        if sub == "unweld":
            return self._pressUnweld(target[1])
        if sub == "delete":
            return self._pressDelete(target)
        if sub == "link":
            return self._pressLink(target[1])
        self._active = False
        return False

    def _clickTarget(self, sample, sub):
        """(kind, id) a click sub-mode's press acts on; id -1 for nothing.

        One pick per press: a caller that probed and then picked again
        would ask K11 twice for the same pixel.
        """
        if sub == "link":
            return (pomadeLib.POMADE_PICK_REGION, self._regionAt(sample))
        node = self._nodeAt(sample)
        if node >= 0 or sub != "delete":
            return (pomadeLib.POMADE_PICK_GRAPH_NODE, node)
        return (pomadeLib.POMADE_PICK_GRAPH_EDGE, self._edgeAt(sample))

    def move(self, sample):
        if self.session.model is None:
            return False
        if self._marquee is not None:
            return self._moveMarquee(sample)
        if not self._active:
            return False
        sub = self.subMode()
        if sub == "draw":
            hit = sample.surface()
            if hit:
                self._stroke.append((hit["face"], hit["u"], hit["v"]))
            return True
        if sub == "region":
            nodeId, hit = self._regionHitAt(sample)
            self._trackRegionHover(nodeId, hit, sample)
            return True
        if sub == "place" and self._dragNode >= 0:
            hit = sample.surface()
            if not hit:
                return True
            self._dropPoint = hit["point"]
            # Only a move the model accepted makes the gesture an edit; a
            # press on a node that never moves is undone on release instead
            # of sealing an empty step and re-rasterising for nothing.
            if self.session.dll.Pomade_GraphMoveNode(
                    self.session.model, self._dragNode, hit["face"],
                    ctypes.c_float(hit["u"]),
                    ctypes.c_float(hit["v"])) == pomadeLib.POMADE_OK:
                self._pendingEdit = True
                self.session.publish()
            return True
        if sub == "reposition":
            return self._moveReposition(sample)
        return False

    def release(self, sample):
        if self.session.model is None:
            return False
        if self._marquee is not None:
            return self._releaseMarquee(sample)
        if not self._active:
            return False
        sub = self.subMode()
        if sub == "draw":
            hit = sample.surface()
            if hit:
                self._stroke.append((hit["face"], hit["u"], hit["v"]))
            self._pendingEdit = self._commitStroke(self._snapRest(sample))
        elif sub == "region":
            self._regionHover = None
            self._closeArmed = False
            if self._regionCloseRequested:
                self._regionCloseRequested = False
                self._pendingEdit = self.completeRegionDraft()
        elif sub == "place" and self._dragNode >= 0:
            if self._finishPlaceDrag(sample):
                self._pendingEdit = True
        # K3 has to see the moved graph and its transported attachment
        # subtrees while the single undo bracket is still live.  Reposition
        # carries existing root/child shapes coherently; it is not a fresh
        # tube fit from the contour.
        repositionFinalized = False
        repositionFinalizeFailed = False
        if sub == "reposition" and self._repositionIds:
            # Qt may coalesce motion events, so the release position is the
            # final drag sample.  _moveReposition is safe for a no-op click
            # and ignores an invalid last ray without losing prior edits.
            self._moveReposition(sample)
        if (sub == "reposition" and self._pendingEdit and
                not self._repositionAtBase):
            repositionFinalized = self._finalizeReposition()
            repositionFinalizeFailed = not repositionFinalized
        # An arming click, a rejected/missed drag, or a return to the press
        # baseline is not an edit to put on the undo stack or bake.  Likewise
        # a failed final K3 pass must restore the bracket snapshot rather
        # than ending it with a moved graph and stale maps.
        repositionRestore = (sub == "reposition" and
                             (not self._pendingEdit or
                              self._repositionAtBase or
                              repositionFinalizeFailed))
        # Place likewise: a press on a node that was never moved or dropped
        # (or an add the model refused) changed nothing.
        placeRestore = sub == "place" and not self._pendingEdit
        self._stroke = []
        self._dragNode = -1
        self._dropPoint = None
        self._resetReposition()
        # K3 and the stub of a region the edit closed belong to the same
        # undo step as the edit (Reposition ran them in _finalizeReposition
        # above). Then seal the bracket BEFORE the stage work: the committer
        # snapshots the model, and a snapshot taken mid-bracket would carry
        # half a gesture.
        if self._bracketOpen:
            if repositionRestore or placeRestore:
                dirty = self.session.cancelGesture()
                self._pendingEdit = False
                self.session.publish(dirty)
            else:
                if self._pendingEdit and sub != "reposition":
                    self._finishEdit()
                # A stroke the model refused changed nothing: no step.
                self.session.endGestureIfChanged(self._pendingEdit)
            self._bracketOpen = False
        self._active = False
        if self._pendingEdit and sub != "region":
            if sub == "reposition":
                if repositionFinalized:
                    self.session.enqueueCommit()
                    self.session.rebake()
                    self.session.publish()
                    self._status(self.statusLine())
            else:
                self.endOfEdit()
        self._pendingEdit = False
        return True

    def cancel(self):
        """Escape: drop the live stroke and restore the press-time base."""
        if self._marquee is not None:
            self._marquee = None
            return True
        if not self._active:
            if self._clearRegionDraft():
                self._status("Pomade Graph: region draft cancelled")
                return True
            # Between the two clicks of connect/weld/link nothing is held
            # down, but the first pick is still live and Escape backs out
            # of it (returning True keeps the controller from clearing the
            # rest of the selection as well).
            action = self.subMode()
            if self._disarm():
                self._setGraphHover(None)
                self._status("Pomade Graph: %s cancelled" % action)
                return True
            return False
        self._stroke = []
        self._clearRegionDraft()
        self._dragNode = -1
        self._dropPoint = None
        self._resetReposition()
        self._disarm()
        self._pendingEdit = False
        dirty = 0
        if self._bracketOpen:
            dirty = self.session.cancelGesture()
            self._bracketOpen = False
        self._active = False
        self.session.publish(dirty)
        self._status("Pomade Graph: cancelled")
        return True

    def hover(self, sample):
        """Highlight whatever a click would act on."""
        if self.session.model is None:
            return False
        sub = self.subMode()
        if sub == "region":
            nodeId, hit = self._regionHitAt(sample)
            self._trackRegionHover(nodeId, hit, sample)
            self._setGraphHover(
                {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": nodeId,
                 "subId": -1, "subSubId": -1} if nodeId >= 0 else None)
            return False
        if sub == "reposition":
            # Press installs the active target highlight; cursor motion while
            # a drag is live must not replace it with a nearby edge or CV.
            if self._active:
                return False
            self._setGraphHover(self._repositionTarget(sample))
            return False
        if sub in CLICK_SUBMODES:
            # Hover exactly what the press would act on (connect, weld and
            # unweld: nodes; delete: a node, else an edge; link: the region
            # containing the cursor), so a highlighted region under a
            # Delete cursor can never promise a delete that does not come.
            kind, ident = self._clickTarget(sample, sub)
            self._setGraphHover(
                {"kind": kind, "id": ident, "subId": -1, "subSubId": -1}
                if ident >= 0 else None)
            return False
        self._setGraphHover(sample.item(self.pickMask, self.pickRadiusPx()))
        return False

    def _finishEdit(self):
        """K3, then the tube stub of every newly closed region.

        Runs INSIDE the edit's undo bracket, so the stub (and the MD-01
        guide refill that comes with it) is part of the artist's one step
        rather than a second one Ctrl+Z would spend first. plan/17 section
        5.1: a closed region gets a tube stub; it has to follow K3 (the
        stub is built from the faces the region claims) and precede the
        commit (a groom with no tube cannot be committed at all).
        plan/18 section 7 G14.
        """
        self._editFinished = bool(self.session.rasterise())
        if self._editFinished:
            self.session.ensureRegionTubes()
        return self._editFinished

    def endOfEdit(self):
        """After the bracket is sealed: enqueue the commit and the bake,
        publish, refresh the HUD. False when _finishEdit's K3 failed."""
        finished = self._editFinished
        self._editFinished = False
        if not finished:
            return False
        self.session.enqueueCommit()
        self.session.rebake()
        self.session.publish()
        self._status(self.statusLine())
        return True

    # -- marquee -----------------------------------------------------------

    def _pressMarquee(self, sample):
        self._marquee = (sample.x, sample.y)
        return True

    def _moveMarquee(self, sample):
        x0, y0 = self._marquee
        selectBand(self.session, pomadeLib.POMADE_PICK_GRAPH_NODE,
                   selectModeFor(sample, band=True),
                   lambda mode: self.session.selectRect(
                       sample.camera, x0, y0, sample.x, sample.y,
                       pomadeLib.POMADE_PICK_GRAPH_NODE, mode))
        self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        return True

    def _releaseMarquee(self, sample):
        x0, y0 = self._marquee
        # Click-versus-band slop in the physical pixels a Sample carries
        # (logical CLICK_SLOP_PX x the view's devicePixelRatio).
        slop = pomadeGizmo.clickSlopPixels(sample.camera)
        nodeId = (self._nodeAt(sample)
                  if abs(sample.x - x0) + abs(sample.y - y0) < slop else -1)
        if nodeId >= 0:
            # No travel over a node: a click, which the table reads
            # differently from a band (Shift toggles instead of adding).
            selectItems(self.session, pomadeLib.POMADE_PICK_GRAPH_NODE,
                        [(nodeId, -1, -1)], selectModeFor(sample))
            self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        else:
            self._moveMarquee(sample)
        count = self.session.selectionCount(pomadeLib.POMADE_PICK_GRAPH_NODE)
        self._marquee = None
        sub = self.subMode()
        if (nodeId < 0 and sub in CLICK_SUBMODES and
                abs(sample.x - x0) + abs(sample.y - y0) < slop and
                selectModeFor(sample) == pomadeLib.POMADE_SELECT_SET):
            # A plain click that found nothing to act on: it only cleared
            # the selection, and the artist should hear why nothing
            # happened rather than a bare "0 node(s) selected".
            self._status(self._missStatus(sub))
            return True
        self._status("Pomade Graph: %d node(s) selected" % count)
        return True

    @staticmethod
    def _missStatus(sub):
        """What a click sub-mode's press on nothing says."""
        return "Pomade Graph %s: %s" % (sub, {
            "delete": "no node or edge here",
            "link": "no region here"}.get(sub, "no node here"))

    # -- sub-mode actions --------------------------------------------------

    def _pressRegion(self, sample):
        """Arm a close, or retain one clicked scalp CV without authoring."""
        nodeId, hit = self._regionHitAt(sample)
        # A press can arrive before any hover event.  Refresh from this
        # resolver result, including clearing a prior CV highlight for a new
        # scalp point; never let an old hover choose the authored identity.
        self._setGraphHover(
            {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": nodeId,
             "subId": -1, "subSubId": -1} if nodeId >= 0 else None)
        # A K11 id without its canonical graph record is never converted to
        # a fresh surface CV: that would silently create the duplicate this
        # path is meant to prevent.
        if nodeId >= 0 and hit is None:
            self._status("Pomade Graph: selected graph node is unavailable")
            return True
        if self._regionDraft and self._regionFirstMatches(nodeId, sample):
            if len(self._regionDraft) < 3:
                self._status("Pomade Graph: region needs at least 3 CVs")
            else:
                self._regionCloseRequested = True
            return True
        if hit is None:
            self._status("Pomade Graph: no scalp under cursor")
            return True
        # K11 supplies a stable graph node id.  Preserve it rather than
        # inferring sharing later from the K1 point: a click inside a CV dot
        # need not raycast to its mathematical centre, but it must still
        # reuse that exact node in the closed contour.
        self._regionDraft.append((hit["face"], hit["u"], hit["v"],
                                  hit["point"], sample.x, sample.y, nodeId))
        self._regionHover = None
        self._closeArmed = False
        self._status("Pomade Graph: region CV %d%s (Enter or first CV closes)"
                     % (len(self._regionDraft),
                        " shares node %d" % nodeId if nodeId >= 0 else ""))
        return True

    def _regionFirstAt(self, sample):
        """True when `sample` falls in the first transient CV's pick ring."""
        first = self._regionDraft[0]
        x, y = first[4], first[5]
        if sample.camera is not None:
            projected = sample.camera.worldToPixels(
                self._draftDisplayPoint(first))
            if projected is not None:
                x, y = projected[0], projected[1]
        dx, dy = sample.x - x, sample.y - y
        return dx * dx + dy * dy <= REGION_NODE_PICK_RADIUS_PX ** 2

    def _regionFirstMatches(self, nodeId, sample):
        """Close by identity for published CVs, proximity only for drafts."""
        firstId = self._regionDraft[0][6]
        if firstId >= 0:
            return nodeId == firstId
        # A published, distinct nearby node wins over the still-transient
        # first dot.  Otherwise a draft CV has no model id to compare.
        return nodeId < 0 and self._regionFirstAt(sample)

    def completeRegionDraft(self):
        """Commit the retained contour as one closed region undo step."""
        if self.subMode() != "region" or len(self._regionDraft) < 3:
            if self.subMode() == "region":
                self._status("Pomade Graph: region needs at least 3 CVs")
            return False
        if not self.session.beginGesture("Graph region"):
            return False
        ok = False
        try:
            ok = self._commitRegionDraft()
            if ok:
                ok = bool(self.session.rasterise())
            if ok:
                self.session.ensureRegionTubes()
        finally:
            if ok:
                self.session.endGesture()
            else:
                dirty = self.session.cancelGesture()
                self.session.publish(dirty)
        if not ok:
            self._status("Pomade Graph: " + self.session.lastError())
            return False
        count = len(self._regionDraft)
        self._clearRegionDraft()
        self.session.enqueueCommit()
        self.session.rebake()
        self.session.publish()
        self._status("Pomade Graph: closed region of %d CVs" % count)
        return True

    def discardRegionCV(self):
        """Backspace removes only the most-recent uncommitted draft CV."""
        if self.subMode() != "region" or not self._regionDraft:
            return False
        self._regionDraft.pop()
        self._regionHover = None
        self._closeArmed = False
        self._regionCloseRequested = False
        self._status("Pomade Graph: region CV removed (%d remaining)"
                     % len(self._regionDraft))
        return True

    def _commitRegionDraft(self):
        """Atomically author a closed chain with exact shared node ids."""
        dll = self.session.dll
        count = len(self._regionDraft)
        nodes = (ctypes.c_int * count)(
            *[entry[6] for entry in self._regionDraft])
        faces = (ctypes.c_int * count)(
            *[entry[0] for entry in self._regionDraft])
        uvs = (ctypes.c_float * (2 * count))(
            *[coordinate for entry in self._regionDraft
              for coordinate in (entry[1], entry[2])])
        region = ctypes.c_int(-1)
        status = dll.Pomade_GraphCreateRegion(
            self.session.model, nodes, faces, uvs, count,
            ctypes.byref(region))
        if status != pomadeLib.POMADE_OK:
            return False
        return True

    def _commitStroke(self, snapRest):
        """Author the stroke; True when it reached the model."""
        if len(self._stroke) < 2:
            return False
        dll = self.session.dll
        faces = (ctypes.c_int * len(self._stroke))(
            *[s[0] for s in self._stroke])
        uvs = (ctypes.c_float * (2 * len(self._stroke)))(
            *[c for s in self._stroke for c in (s[1], s[2])])
        out = (ctypes.c_int * 4096)()
        count = ctypes.c_int(0)
        closed = ctypes.c_int(0)
        weldStart = ctypes.c_int(0)
        weldEnd = ctypes.c_int(0)
        # The simplify tolerance rides the snap radius: both answer "how
        # close counts as the same place", and one number in the panel
        # should not need a second one beside it.
        eps = max(snapRest * 0.5, 1e-6)
        if dll.Pomade_GraphStroke(
                self.session.model, faces, uvs, len(self._stroke),
                ctypes.c_float(snapRest), ctypes.c_float(eps), out, 4096,
                ctypes.byref(count), ctypes.byref(closed),
                ctypes.byref(weldStart),
                ctypes.byref(weldEnd)) != pomadeLib.POMADE_OK:
            self._status("Pomade Graph: " + self.session.lastError())
            return False
        self._status("Pomade Graph: stroke of %d node(s)%s"
                     % (count.value, " (closed)" if closed.value else ""))
        return True

    def _pressPlace(self, sample):
        dll = self.session.dll
        node = self._nodeAt(sample)
        if node >= 0:
            # Grabbing a node is not an edit yet: move() marks one only when
            # Pomade_GraphMoveNode accepts a position, and release() undoes
            # the bracket of a grab that never moved or dropped anywhere.
            self._dragNode = node
            self._dropPoint = None
            return True
        hit = sample.surface()
        if hit is None:
            self._status("Pomade Graph place: no scalp under cursor")
            return True
        self._dropPoint = hit["point"]
        nodeId = ctypes.c_int(-1)
        if dll.Pomade_GraphAddNode(self.session.model, hit["face"],
                                  ctypes.c_float(hit["u"]),
                                  ctypes.c_float(hit["v"]),
                                  ctypes.byref(nodeId)) == pomadeLib.POMADE_OK:
            self._dragNode = nodeId.value
            self._pendingEdit = True
        self.session.publish()
        return True

    # -- reposition -------------------------------------------------------

    def _resetReposition(self):
        self._repositionIds = ()
        self._repositionPoints = ()
        self._repositionAnchor = None
        self._repositionAtBase = True
        self._repositionLastDelta = None

    def _pressReposition(self, sample):
        """Freeze one CV, or both endpoints of one edge, for a drag.

        This is intentionally separate from Place: Reposition never adds,
        welds, or splits graph topology.  A node wins over an edge where
        their visible pick shapes overlap.
        """
        self._resetReposition()
        target = self._repositionTarget(sample)
        nodeId = target["id"] if target and target["kind"] == \
            pomadeLib.POMADE_PICK_GRAPH_NODE else -1
        ids = (nodeId,) if nodeId >= 0 else ()
        pickedKind = target["kind"] if target else 0
        pickedId = target["id"] if target else -1
        if not ids:
            edgeId = (target["id"] if target and
                      target["kind"] == pomadeLib.POMADE_PICK_GRAPH_EDGE
                      else -1)
            if edgeId < 0:
                self._active = False
                self._setGraphHover(None)
                self._status("Pomade Graph reposition: no CV or edge here")
                return True
            endpoints = (ctypes.c_int * 2)()
            entry = getattr(self.session.dll, "Pomade_GraphGetEdge", None)
            if entry is None or entry(self.session.model, edgeId, endpoints) != pomadeLib.POMADE_OK:
                self._active = False
                self._status("Pomade Graph reposition: edge is unavailable")
                return True
            ids = (int(endpoints[0]), int(endpoints[1]))
            pickedKind = pomadeLib.POMADE_PICK_GRAPH_EDGE
            pickedId = edgeId
        anchor = sample.surface()
        if anchor is None:
            self._active = False
            self._status("Pomade Graph reposition: no scalp under cursor")
            return True
        records = [self._graphNodeHit(node) for node in ids]
        if any(record is None for record in records):
            self._active = False
            self._status("Pomade Graph reposition: graph target is unavailable")
            return True
        if not self.session.beginGesture("Graph reposition"):
            self._active = False
            return True
        self._bracketOpen = True
        self._repositionIds = ids
        self._repositionPoints = tuple(record["point"] for record in records)
        self._repositionAnchor = anchor["point"]
        self._repositionAtBase = True
        self._setGraphHover(target)
        self._status("Pomade Graph: repositioning %s (Esc cancels)" %
                     ("CV" if pickedKind == pomadeLib.POMADE_PICK_GRAPH_NODE
                      else "edge"))
        return True

    def _closestScalp(self, point):
        """K1 closest-point record for a target rest position, or None."""
        query = (ctypes.c_float * 3)(*point)
        hit = ctypes.c_int(0)
        face = ctypes.c_int(-1)
        uv = (ctypes.c_float * 2)()
        outPoint = (ctypes.c_float * 3)()
        normal = (ctypes.c_float * 3)()
        if self.session.dll.Pomade_ClosestPoint(
                self.session.model, query, ctypes.byref(hit),
                ctypes.byref(face), uv, outPoint, normal) != pomadeLib.POMADE_OK:
            return None
        if not hit.value:
            return None
        return {"face": int(face.value), "u": float(uv[0]),
                "v": float(uv[1])}

    def _moveReposition(self, sample):
        if not self._repositionIds or self._repositionAnchor is None:
            return True
        hit = sample.surface()
        if hit is None:
            # Do not advance the frozen baseline or alter the last valid
            # geometry when the cursor has no scalp ray.
            return True
        delta = tuple(float(hit["point"][axis]) -
                      float(self._repositionAnchor[axis])
                      for axis in range(3))
        atBase = all(abs(component) <= 1e-8 for component in delta)
        if atBase and not self._pendingEdit:
            return True
        if delta == self._repositionLastDelta:
            return True
        targets = [self._closestScalp(
            tuple(point[axis] + delta[axis] for axis in range(3)))
                   for point in self._repositionPoints]
        if any(target is None for target in targets):
            return True
        count = len(self._repositionIds)
        nodeIds = (ctypes.c_int * count)(*self._repositionIds)
        faces = (ctypes.c_int * count)(*[target["face"] for target in targets])
        uvs = (ctypes.c_float * (2 * count))(
            *[coordinate for target in targets
              for coordinate in (target["u"], target["v"])])
        entry = getattr(self.session.dll, "Pomade_GraphMoveNodes", None)
        if entry is None or entry(self.session.model, nodeIds, faces, uvs,
                                  count) != pomadeLib.POMADE_OK:
            # A batch rejection (including a topology collapse) leaves the
            # latest valid graph in place.  The next pointer sample still
            # derives from the original press baseline and can recover.
            self._status("Pomade Graph reposition: " + self.session.lastError())
            return True
        self._pendingEdit = True
        self._repositionAtBase = atBase
        self._repositionLastDelta = delta
        # The graph move transports every attached tube subtree in native
        # code. Publish its points, topology and guides with the contour so
        # the live viewport never combines a new region with stale tubes.
        self.session.publish(pomadeLib.POMADE_DIRTY_POINTS |
                             pomadeLib.POMADE_DIRTY_TOPOLOGY |
                             pomadeLib.POMADE_DIRTY_GRAPH |
                             pomadeLib.POMADE_DIRTY_REGIONS |
                             pomadeLib.POMADE_DIRTY_GUIDES)
        return True

    def _finalizeReposition(self):
        """Synchronize K3 while the reposition undo bracket remains open."""
        if not self.session.rasterise():
            self._status("Pomade Graph: " + self.session.lastError())
            return False
        # This only adds a missing root stub for a newly closed region; the
        # accepted move has already transported existing root/child shapes.
        self.session.ensureRegionTubes()
        return True

    def _finishPlaceDrag(self, sample):
        """A drag that ends on another node or edge welds (plan/17 5.1).

        True when the drop changed the graph.
        """
        dll = self.session.dll
        other = self._nodeAt(sample)
        if other >= 0 and other != self._dragNode:
            welded = dll.Pomade_GraphWeld(
                self.session.model, other,
                self._dragNode) == pomadeLib.POMADE_OK
            self._dragNode = other
            return welded
        edge = self._edgeAt(sample)
        if edge < 0:
            return False
        # The release pick can still see the edge under the node that was
        # dragged.  Splitting that incident edge and welding its new midpoint
        # back to the dragged node deletes/replaces the node during an
        # ordinary move.  Only an edge that does not own the dragged node is
        # a valid Place drop target.
        endpoints = (ctypes.c_int * 2)()
        getEdge = getattr(dll, "Pomade_GraphGetEdge", None)
        if (getEdge is not None and
                getEdge(self.session.model, edge, endpoints) ==
                pomadeLib.POMADE_OK and
                self._dragNode in (int(endpoints[0]), int(endpoints[1]))):
            return False
        hit = sample.surface()
        if hit is None:
            return False
        mid = ctypes.c_int(-1)
        if dll.Pomade_GraphSplitEdge(self.session.model, edge, hit["face"],
                                    ctypes.c_float(hit["u"]),
                                    ctypes.c_float(hit["v"]),
                                    ctypes.byref(mid)) != pomadeLib.POMADE_OK:
            return False
        # The split alone is an edit even if the weld after it is refused.
        dll.Pomade_GraphWeld(self.session.model, mid.value, self._dragNode)
        self._dragNode = mid.value
        return True

    def _pressClickNode(self, node, action):
        dll = self.session.dll
        if node < 0:
            self._status("Pomade Graph %s: no node here" % action)
            return True
        if self._clickNode < 0:
            self._arm(pomadeLib.POMADE_PICK_GRAPH_NODE, node)
            self._status("Pomade Graph %s: node %d ... (Esc cancels)"
                         % (action, node))
            return True
        # The pick is spent either way; a weld may even have deleted the
        # node its selection named.
        first = self._clickNode
        self._disarm()
        if first == node:
            return True
        if action == "connect":
            def connect():
                edge = ctypes.c_int(-1)
                return dll.Pomade_GraphConnect(
                    self.session.model, first, node,
                    ctypes.byref(edge)) == pomadeLib.POMADE_OK
            ok = self._bracket("Graph connect", connect)
            self._status("Pomade Graph: connected (%s)"
                         % ("ok" if ok else self.session.lastError()))
        else:
            ok = self._bracket(
                "Graph weld",
                lambda: dll.Pomade_GraphWeld(self.session.model, first, node)
                == pomadeLib.POMADE_OK)
            self._status("Pomade Graph: welded (%s)"
                         % ("ok" if ok else self.session.lastError()))
        return True

    def _pressUnweld(self, node):
        if node < 0:
            self._status("Pomade Graph unweld: no node here")
            return True
        self._unweld(node)
        return True

    def _unweld(self, node):
        """Split `node` per region as one undo step; True when it split.

        A node only one region uses has nothing to split: that leaves no
        undo step and says so. A split reports the pieces the node became
        and, when the split opened a region's boundary, the region count
        before and after (unwelding a corner whose third edge is shared
        leaves one closed region, not two).
        """
        dll = self.session.dll
        out = (ctypes.c_int * 64)()
        count = ctypes.c_int(0)
        before = self.session.graphCounts()
        result = {"ok": False}       # stays False if the bracket refused

        def unweld():
            result["ok"] = dll.Pomade_GraphUnweld(
                self.session.model, node, out, 64,
                ctypes.byref(count)) == pomadeLib.POMADE_OK
            return result["ok"] and count.value > 0

        if not self._bracket("Graph unweld", unweld):
            if result["ok"]:
                self._status("Pomade Graph unweld: node %d is not shared, "
                             "nothing to split" % node)
            else:
                self._status("Pomade Graph unweld: " +
                             self.session.lastError())
            return False
        after = self.session.graphCounts()
        text = "Pomade Graph: unwelded into %d nodes" % (count.value + 1)
        if after[2] != before[2]:
            text += " (regions %d -> %d)" % (before[2], after[2])
        self._status(text)
        return True

    def _pressDelete(self, target):
        """`target` is _clickTarget's: a node first, else the edge."""
        dll = self.session.dll
        kind, ident = target
        node = ident if kind == pomadeLib.POMADE_PICK_GRAPH_NODE else -1
        if node >= 0:
            ok = self._bracket(
                "Graph delete node",
                lambda: dll.Pomade_GraphDeleteNode(self.session.model, node)
                == pomadeLib.POMADE_OK)
            self._status("Pomade Graph: node deleted (%s)"
                         % ("ok" if ok else self.session.lastError()))
            return True
        edge = ident if kind == pomadeLib.POMADE_PICK_GRAPH_EDGE else -1
        if edge >= 0:
            ok = self._bracket(
                "Graph delete edge",
                lambda: dll.Pomade_GraphDeleteEdge(self.session.model, edge)
                == pomadeLib.POMADE_OK)
            self._status("Pomade Graph: edge deleted, regions merged (%s)"
                         % ("ok" if ok else self.session.lastError()))
            return True
        self._status(self._missStatus("delete"))
        return True

    def _pressLink(self, region):
        if region < 0:
            self._status("Pomade Graph link: no region here")
            return True
        if self._clickRegion < 0:
            self._arm(pomadeLib.POMADE_PICK_REGION, region)
            self._status("Pomade Graph link: region %d ... (Esc cancels)"
                         % region)
            return True
        first = self._clickRegion
        self._disarm()
        if first == region:
            return True
        ok = self._bracket(
            "Graph link",
            lambda: self.session.dll.Pomade_GraphLinkRegions(
                self.session.model, first, region) == pomadeLib.POMADE_OK)
        self._status("Pomade Graph: linked (%s)"
                     % ("ok" if ok else self.session.lastError()))
        return True

    # -- keys --------------------------------------------------------------

    def deleteSelection(self):
        """Delete every selected node, then every selected edge."""
        dll = self.session.dll
        nodes = [i for i, _s, _ss in self.session.readSelection(
            pomadeLib.POMADE_PICK_GRAPH_NODE)]
        edges = [i for i, _s, _ss in self.session.readSelection(
            pomadeLib.POMADE_PICK_GRAPH_EDGE)]
        if not nodes and not edges:
            return False
        gone = {"nodes": 0, "edges": 0}

        def delete():
            for edgeId in edges:
                if dll.Pomade_GraphDeleteEdge(
                        self.session.model, edgeId) == pomadeLib.POMADE_OK:
                    gone["edges"] += 1
            for nodeId in nodes:
                if dll.Pomade_GraphDeleteNode(
                        self.session.model, nodeId) == pomadeLib.POMADE_OK:
                    gone["nodes"] += 1
            return gone["nodes"] + gone["edges"] > 0

        changed = self._bracket("Graph delete", delete)
        self.session.clearSelection(pomadeLib.POMADE_PICK_GRAPH_NODE |
                                    pomadeLib.POMADE_PICK_GRAPH_EDGE)
        self._status("Pomade Graph: deleted %d node(s), %d edge(s)"
                     % (gone["nodes"], gone["edges"]))
        if changed:
            self.endOfEdit()
        return True

    def adjustRadius(self, delta):
        """`[` / `]`: the Graph snap radius in pixels."""
        value = max(1.0, min(128.0, float(self.state.snapRadiusPx) +
                             float(delta)))
        self.state.snapRadiusPx = value
        status = "Pomade Graph: snap radius %.0f px" % value
        self._status(status)
        return status

    # -- one-shot actions the dock drives ----------------------------------

    def weldAll(self, camera=None):
        """Weld every node pair closer than the snap radius, one step.

        The radius is the panel's pixels converted to rest units at the
        scalp through `camera` (pomadeGraph.snapRadiusRest), never the
        pixel number itself. No weld in range leaves no undo step.
        """
        from .pomadeGraph import snapRadiusRest
        dll = self.session.dll
        welds = ctypes.c_int(0)
        radius = snapRadiusRest(self.session, self.state, camera)
        result = {"ok": False}       # stays False if the bracket refused

        def weld():
            result["ok"] = dll.Pomade_GraphWeldAll(
                self.session.model, ctypes.c_float(radius),
                ctypes.byref(welds)) == pomadeLib.POMADE_OK
            return result["ok"] and welds.value > 0

        changed = self._bracket("Graph weld all", weld)
        if not result["ok"]:
            self._status("Pomade Graph: " + self.session.lastError())
            return False
        self._status("Pomade Graph: %d weld(s)" % welds.value)
        if changed:
            self.endOfEdit()
        return True

    def toggleMirrorX(self):
        on = not self.state.mirrorX
        self.state.mirrorX = on
        self.session.dll.Pomade_SetMirrorX(self.session.model, 1 if on else 0)
        self._status("Pomade Graph: mirror-X %s" % ("on" if on else "off"))
        return on

    def weldSelected(self):
        """Shift+W: weld the two selected nodes into one."""
        nodes = [i for i, _s, _ss in self.session.readSelection(
            pomadeLib.POMADE_PICK_GRAPH_NODE)]
        if len(nodes) != 2:
            self._status("Pomade Graph: weld wants exactly two nodes "
                         "(%d selected)" % len(nodes))
            return False
        ok = self._bracket(
            "Graph weld",
            lambda: self.session.dll.Pomade_GraphWeld(
                self.session.model, nodes[0], nodes[1]) == pomadeLib.POMADE_OK)
        self._status("Pomade Graph: welded (%s)"
                     % ("ok" if ok else self.session.lastError()))
        if ok:
            self.endOfEdit()
        return ok

    def unweldSelected(self):
        """Shift+U: split the one selected shared node per region."""
        nodes = [i for i, _s, _ss in self.session.readSelection(
            pomadeLib.POMADE_PICK_GRAPH_NODE)]
        if len(nodes) != 1:
            self._status("Pomade Graph: unweld wants exactly one node "
                         "(%d selected)" % len(nodes))
            return False
        ok = self._unweld(nodes[0])
        if ok:
            self.endOfEdit()
        return ok


# The modes that drive the viewport. Output is deliberately absent: it is
# a panel, not a gesture (plan/18 section 3.6), so it has no loop, no
# sub-mode shelf and no pointer behaviour of its own. Registering an empty
# loop for it would make the shelf lie about what the viewport does.
#
# GraphLoop lives in this module, so it is in the table from the start.
LOOPS = {GraphLoop.modeId: GraphLoop}

# The modes that answer None from makeLoop, and do so by design. The
# controller reports the mode's own status line for these; nothing is
# "not built yet".
PANEL_ONLY_MODES = ("output",)


# Every other loop is registered LAZILY, by module name: those modules
# import ToolLoop from here, so a table that asked for the class at import
# time would break whenever one of them was imported first -- the module
# would be half-built in sys.modules and its class not yet defined.
_LAZY_LOOPS = {
    "tube": ("pomadeLoopsTube", "TubeLoop"),
    "fill": ("pomadeLoopsFill", "FillLoop"),
    "hierarchy": ("pomadeLoopsHierarchy", "HierarchyLoop"),
    "sculpt": ("pomadeLoopsSculpt", "SculptLoop"),
}


def loopClass(modeId):
    """The loop class for `modeId`, or None when that mode has none."""
    modeId = str(modeId)
    factory = LOOPS.get(modeId)
    if factory is not None:
        return factory
    entry = _LAZY_LOOPS.get(modeId)
    if entry is None:
        return None
    import importlib
    factory = getattr(importlib.import_module("." + entry[0], __package__),
                      entry[1])
    LOOPS[modeId] = factory
    return factory


def makeLoop(modeId, session, state):
    """The loop for `modeId`, or None when that mode has none yet."""
    factory = loopClass(modeId)
    return factory(session, state) if factory is not None else None


def subModesFor(modeId):
    """The sub-mode shelf of `modeId` (empty for Output)."""
    return {
        "graph": pomadeModes.GRAPH_SUBMODES,
        "tube": pomadeModes.TUBE_SUBMODES,
        "fill": pomadeModes.FILL_SUBMODES,
        "hierarchy": pomadeModes.HIERARCHY_SUBMODES,
        "sculpt": pomadeModes.SCULPT_SUBMODES,
    }.get(str(modeId), ())


def setSubModeOn(state, modeId, subId):
    """Record a sub-mode for any mode; returns the status line."""
    setter = {
        "graph": pomadeModes.SetActiveGraphSubMode,
        "tube": pomadeModes.SetActiveTubeSubMode,
        "fill": pomadeModes.SetActiveFillSubMode,
        "hierarchy": pomadeModes.SetActiveHierarchySubMode,
        "sculpt": pomadeModes.SetActiveSculptSubMode,
    }.get(str(modeId))
    return setter(state, subId) if setter is not None else ""
