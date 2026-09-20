# usdGenTonicTools.tonicLoops -- one tool loop per mode (plan/18 section
# 3.1, section 3.6).
#
# Qt-free by rule: the loops see pixels, modifiers and a camera, never a
# QEvent. tonicViewport resolves the camera once per gesture and hands each
# loop a Sample; the loop does the model work through TonicSession and says
# whether it claimed the event.
#
# Two things changed from the P2 GraphController this replaces:
#   * every surface sample is a K1 Tonic_Raycast, not a Hydra
#     StageView.pick (plan/18 finding F6: ~1.3 ms per sample, once per
#     press at most by plan/08 section 6.1);
#   * every node / edge / region hit is a K11 Tonic_PickItem, which is
#     screen-space and occlusion-aware, instead of the world-space snap
#     queries that could not tell a node behind the head from one in front.
from __future__ import annotations

import ctypes

from . import tonicLib
from . import tonicModes

# The pick radius floor, so a zero snap radius still catches something.
MIN_PICK_RADIUS_PX = 2.0


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

    def deactivate(self):
        """The mode is being left: drop anything this loop put on screen.

        A gizmo or a brush ring is model state (plan/18 section 2.4), so
        it outlives the loop that set it unless the loop takes it away.
        """
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
    pickMask = (tonicLib.TONIC_PICK_GRAPH_NODE |
                tonicLib.TONIC_PICK_GRAPH_EDGE |
                tonicLib.TONIC_PICK_REGION)
    subModes = tonicModes.GRAPH_SUBMODES
    defaultSubMode = "draw"

    def __init__(self, session, state):
        super(GraphLoop, self).__init__(session, state)
        self._stroke = []          # (face, u, v) samples of the live stroke
        self._dragNode = -1
        self._dropPoint = None
        self._clickNode = -1
        self._clickRegion = -1
        self._marquee = None       # (x0, y0) while a rubber band is live
        self._active = False
        self._bracketOpen = False  # an undo bracket is open on the model
        self._pendingEdit = False  # the gesture changed the model
        self._lastStatus = ""

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return self.state.graphSubMode or self.defaultSubMode

    def setSubMode(self, subId):
        status = tonicModes.SetActiveGraphSubMode(self.state, subId)
        if status:
            self._clickNode = -1
            self._clickRegion = -1
        return status

    # -- status ------------------------------------------------------------

    def _status(self, text):
        self._lastStatus = text
        self.session.report(text)

    def statusLine(self):
        from .tonicGraph import hudStatus
        nodes, edges, regions = self.session.graphCounts()
        _r, uncovered, intersected = self.session.regionStats()
        return hudStatus((nodes, edges, regions),
                         (regions, uncovered, intersected),
                         self.state.mapVersion, self.state.bakedVersion,
                         fallbackReason=self.session.fallbackReason())

    # -- geometry helpers --------------------------------------------------

    def _bracket(self, label, action):
        """Run one mutating click as a single undo step."""
        self.session.beginGesture(label)
        try:
            return action()
        finally:
            self.session.endGesture()
            self._pendingEdit = True

    def _snapRest(self, sample):
        """The snap radius in rest units at the sampled depth.

        The panel's number is pixels; the ABI's is rest units. The camera
        converts the two with a measured world-per-pixel at the point the
        artist is actually pointing at, so the same 8 px means the same
        thing zoomed in and zoomed out.
        """
        hit = sample.surface()
        point = hit["point"] if hit else None
        if point is None or sample.camera is None:
            return float(self.session.dll.Tonic_GetSnapRadius(
                self.session.model))
        perPixel = sample.camera.worldPerPixel(point)
        if perPixel <= 0.0:
            return float(self.session.dll.Tonic_GetSnapRadius(
                self.session.model))
        return max(perPixel * float(self.state.snapRadiusPx), 1e-6)

    def _nodeAt(self, sample):
        item = sample.item(tonicLib.TONIC_PICK_GRAPH_NODE,
                           self.pickRadiusPx())
        return item["id"] if item else -1

    def _edgeAt(self, sample):
        item = sample.item(tonicLib.TONIC_PICK_GRAPH_EDGE,
                           self.pickRadiusPx())
        return item["id"] if item else -1

    def _regionAt(self, sample):
        """The region under the cursor, by K11 and then by face lookup."""
        item = sample.item(tonicLib.TONIC_PICK_REGION, self.pickRadiusPx())
        if item:
            return item["id"]
        hit = sample.surface()
        if not hit:
            return -1
        dll = self.session.dll
        count = ctypes.c_int(0)
        dll.Tonic_ReadFaceRegionIds(self.session.model, None, 0,
                                    ctypes.byref(count))
        n = max(int(count.value), 0)
        if hit["face"] < 0 or hit["face"] >= n:
            return -1
        ids = (ctypes.c_int * n)()
        got = ctypes.c_int(0)
        if dll.Tonic_ReadFaceRegionIds(self.session.model, ids, n,
                                       ctypes.byref(got)) \
                != tonicLib.TONIC_OK:
            return -1
        return int(ids[hit["face"]])

    # -- gesture -----------------------------------------------------------

    def press(self, sample):
        if self.session.model is None:
            return False
        if sample.has("shift"):
            return self._pressMarquee(sample)
        sub = self.subMode()
        self._active = True
        self._pendingEdit = False
        # A drag holds one bracket open for its whole life. A click mode
        # cannot know at press time whether it will mutate anything, and a
        # bracket opened on every arming click would litter the history
        # with steps that undo to the state they were taken from, so those
        # wrap their own mutation instead (_bracket below).
        if sub in ("draw", "place"):
            self.session.beginGesture("Graph %s" % sub)
            self._bracketOpen = True
        if sub == "draw":
            hit = sample.surface()
            self._stroke = ([(hit["face"], hit["u"], hit["v"])] if hit
                            else [])
            return True
        if sub == "place":
            return self._pressPlace(sample)
        if sub in ("connect", "weld"):
            return self._pressClickNode(sample, sub)
        if sub == "unweld":
            return self._pressUnweld(sample)
        if sub == "delete":
            return self._pressDelete(sample)
        if sub == "link":
            return self._pressLink(sample)
        self._active = False
        return False

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
        if sub == "place" and self._dragNode >= 0:
            hit = sample.surface()
            if not hit:
                return True
            self._dropPoint = hit["point"]
            self.session.dll.Tonic_GraphMoveNode(
                self.session.model, self._dragNode, hit["face"],
                ctypes.c_float(hit["u"]), ctypes.c_float(hit["v"]))
            self.session.publish()
            return True
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
        elif sub == "place" and self._dragNode >= 0:
            self._finishPlaceDrag(sample)
        self._stroke = []
        self._dragNode = -1
        self._dropPoint = None
        # Seal the undo bracket FIRST: the committer snapshots the model,
        # and a snapshot taken mid-bracket would carry half a gesture.
        if self._bracketOpen:
            self.session.endGesture()
            self._bracketOpen = False
        self._active = False
        if self._pendingEdit:
            self.endOfEdit()
        self._pendingEdit = False
        return True

    def cancel(self):
        """Escape: drop the live stroke and restore the press-time base."""
        if self._marquee is not None:
            self._marquee = None
            return True
        if not self._active:
            return False
        self._stroke = []
        self._dragNode = -1
        self._dropPoint = None
        self._clickNode = -1
        self._clickRegion = -1
        self._pendingEdit = False
        dirty = 0
        if self._bracketOpen:
            dirty = self.session.cancelGesture()
            self._bracketOpen = False
        self._active = False
        self.session.publish(dirty)
        self._status("Tonic Graph: cancelled")
        return True

    def hover(self, sample):
        """Highlight whatever a click would act on."""
        if self.session.model is None:
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

    def endOfEdit(self):
        """Rasterise, enqueue the commit and the bake, refresh the HUD."""
        if not self.session.rasterise():
            return False
        # plan/17 section 5.1: a closed region gets a tube stub. It has to
        # happen after K3 (the stub is built from the faces the region
        # claims) and before the commit (a groom with no tube cannot be
        # committed at all). plan/18 section 7 G14.
        self.session.ensureRegionTubes()
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
        mode = (tonicLib.TONIC_SELECT_ADD if sample.has("ctrl")
                else tonicLib.TONIC_SELECT_SET)
        self.session.selectRect(sample.camera, x0, y0, sample.x, sample.y,
                                tonicLib.TONIC_PICK_GRAPH_NODE, mode)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    def _releaseMarquee(self, sample):
        self._moveMarquee(sample)
        count = self.session.selectionCount(tonicLib.TONIC_PICK_GRAPH_NODE)
        self._marquee = None
        self._status("Tonic Graph: %d node(s) selected" % count)
        return True

    # -- sub-mode actions --------------------------------------------------

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
        if dll.Tonic_GraphStroke(
                self.session.model, faces, uvs, len(self._stroke),
                ctypes.c_float(snapRest), ctypes.c_float(eps), out, 4096,
                ctypes.byref(count), ctypes.byref(closed),
                ctypes.byref(weldStart),
                ctypes.byref(weldEnd)) != tonicLib.TONIC_OK:
            self._status("Tonic Graph: " + self.session.lastError())
            return False
        self._status("Tonic Graph: stroke of %d node(s)%s"
                     % (count.value, " (closed)" if closed.value else ""))
        return True

    def _pressPlace(self, sample):
        dll = self.session.dll
        node = self._nodeAt(sample)
        if node >= 0:
            self._dragNode = node
            self._dropPoint = None
            self._pendingEdit = True
            return True
        hit = sample.surface()
        if hit is None:
            return True
        self._dropPoint = hit["point"]
        nodeId = ctypes.c_int(-1)
        if dll.Tonic_GraphAddNode(self.session.model, hit["face"],
                                  ctypes.c_float(hit["u"]),
                                  ctypes.c_float(hit["v"]),
                                  ctypes.byref(nodeId)) == tonicLib.TONIC_OK:
            self._dragNode = nodeId.value
            self._pendingEdit = True
        self.session.publish()
        return True

    def _finishPlaceDrag(self, sample):
        """A drag that ends on another node or edge welds (plan/17 5.1)."""
        dll = self.session.dll
        other = self._nodeAt(sample)
        if other >= 0 and other != self._dragNode:
            dll.Tonic_GraphWeld(self.session.model, other, self._dragNode)
            self._dragNode = other
            return
        edge = self._edgeAt(sample)
        if edge < 0:
            return
        hit = sample.surface()
        if hit is None:
            return
        mid = ctypes.c_int(-1)
        if dll.Tonic_GraphSplitEdge(self.session.model, edge, hit["face"],
                                    ctypes.c_float(hit["u"]),
                                    ctypes.c_float(hit["v"]),
                                    ctypes.byref(mid)) == tonicLib.TONIC_OK:
            dll.Tonic_GraphWeld(self.session.model, mid.value,
                                self._dragNode)
            self._dragNode = mid.value

    def _pressClickNode(self, sample, action):
        dll = self.session.dll
        node = self._nodeAt(sample)
        if node < 0:
            self._status("Tonic Graph %s: no node here" % action)
            return True
        if self._clickNode < 0:
            self._clickNode = node
            self._status("Tonic Graph %s: node %d ..." % (action, node))
            return True
        first, self._clickNode = self._clickNode, -1
        if first == node:
            return True
        if action == "connect":
            def connect():
                edge = ctypes.c_int(-1)
                return dll.Tonic_GraphConnect(
                    self.session.model, first, node,
                    ctypes.byref(edge)) == tonicLib.TONIC_OK
            ok = self._bracket("Graph connect", connect)
            self._status("Tonic Graph: connected (%s)"
                         % ("ok" if ok else self.session.lastError()))
        else:
            ok = self._bracket(
                "Graph weld",
                lambda: dll.Tonic_GraphWeld(self.session.model, first, node)
                == tonicLib.TONIC_OK)
            self._status("Tonic Graph: welded (%s)"
                         % ("ok" if ok else self.session.lastError()))
        return True

    def _pressUnweld(self, sample):
        node = self._nodeAt(sample)
        if node < 0:
            self._status("Tonic Graph unweld: no node here")
            return True
        out = (ctypes.c_int * 64)()
        count = ctypes.c_int(0)
        self._bracket(
            "Graph unweld",
            lambda: self.session.dll.Tonic_GraphUnweld(
                self.session.model, node, out, 64, ctypes.byref(count)))
        self._status("Tonic Graph: unwelded into %d" % (count.value + 1))
        return True

    def _pressDelete(self, sample):
        dll = self.session.dll
        node = self._nodeAt(sample)
        if node >= 0:
            ok = self._bracket(
                "Graph delete node",
                lambda: dll.Tonic_GraphDeleteNode(self.session.model, node)
                == tonicLib.TONIC_OK)
            self._status("Tonic Graph: node deleted (%s)"
                         % ("ok" if ok else self.session.lastError()))
            return True
        edge = self._edgeAt(sample)
        if edge >= 0:
            ok = self._bracket(
                "Graph delete edge",
                lambda: dll.Tonic_GraphDeleteEdge(self.session.model, edge)
                == tonicLib.TONIC_OK)
            self._status("Tonic Graph: edge deleted, regions merged (%s)"
                         % ("ok" if ok else self.session.lastError()))
            return True
        return True

    def _pressLink(self, sample):
        region = self._regionAt(sample)
        if region < 0:
            self._status("Tonic Graph link: no region here")
            return True
        if self._clickRegion < 0:
            self._clickRegion = region
            self._status("Tonic Graph link: region %d ..." % region)
            return True
        first, self._clickRegion = self._clickRegion, -1
        if first == region:
            return True
        ok = self._bracket(
            "Graph link",
            lambda: self.session.dll.Tonic_GraphLinkRegions(
                self.session.model, first, region) == tonicLib.TONIC_OK)
        self._status("Tonic Graph: linked (%s)"
                     % ("ok" if ok else self.session.lastError()))
        return True

    # -- keys --------------------------------------------------------------

    def deleteSelection(self):
        """Delete every selected node, then every selected edge."""
        dll = self.session.dll
        nodes = [i for i, _s, _ss in self.session.readSelection(
            tonicLib.TONIC_PICK_GRAPH_NODE)]
        edges = [i for i, _s, _ss in self.session.readSelection(
            tonicLib.TONIC_PICK_GRAPH_EDGE)]
        if not nodes and not edges:
            return False
        self.session.beginGesture("Graph delete")
        for edgeId in edges:
            dll.Tonic_GraphDeleteEdge(self.session.model, edgeId)
        for nodeId in nodes:
            dll.Tonic_GraphDeleteNode(self.session.model, nodeId)
        self.session.endGesture()
        self.session.clearSelection(tonicLib.TONIC_PICK_GRAPH_NODE |
                                    tonicLib.TONIC_PICK_GRAPH_EDGE)
        self._status("Tonic Graph: deleted %d node(s), %d edge(s)"
                     % (len(nodes), len(edges)))
        self.endOfEdit()
        return True

    def adjustRadius(self, delta):
        """`[` / `]`: the Graph snap radius in pixels."""
        value = max(1.0, min(128.0, float(self.state.snapRadiusPx) +
                             float(delta)))
        self.state.snapRadiusPx = value
        status = "Tonic Graph: snap radius %.0f px" % value
        self._status(status)
        return status

    # -- one-shot actions the dock drives ----------------------------------

    def weldAll(self):
        dll = self.session.dll
        welds = ctypes.c_int(0)
        radius = float(dll.Tonic_GetSnapRadius(self.session.model))
        if dll.Tonic_GraphWeldAll(self.session.model, ctypes.c_float(radius),
                                  ctypes.byref(welds)) == tonicLib.TONIC_OK:
            self._status("Tonic Graph: %d weld(s)" % welds.value)
            self.endOfEdit()
            return True
        self._status("Tonic Graph: " + self.session.lastError())
        return False

    def toggleMirrorX(self):
        on = not self.state.mirrorX
        self.state.mirrorX = on
        self.session.dll.Tonic_SetMirrorX(self.session.model, 1 if on else 0)
        self._status("Tonic Graph: mirror-X %s" % ("on" if on else "off"))
        return on

    def weldSelected(self):
        """Shift+W: weld the two selected nodes into one."""
        nodes = [i for i, _s, _ss in self.session.readSelection(
            tonicLib.TONIC_PICK_GRAPH_NODE)]
        if len(nodes) != 2:
            self._status("Tonic Graph: weld wants exactly two nodes "
                         "(%d selected)" % len(nodes))
            return False
        self.session.beginGesture("Graph weld")
        ok = self.session.dll.Tonic_GraphWeld(
            self.session.model, nodes[0], nodes[1]) == tonicLib.TONIC_OK
        self.session.endGesture()
        self._status("Tonic Graph: welded (%s)"
                     % ("ok" if ok else self.session.lastError()))
        self.endOfEdit()
        return ok

    def unweldSelected(self):
        """Shift+U: split the one selected shared node per region."""
        nodes = [i for i, _s, _ss in self.session.readSelection(
            tonicLib.TONIC_PICK_GRAPH_NODE)]
        if len(nodes) != 1:
            self._status("Tonic Graph: unweld wants exactly one node "
                         "(%d selected)" % len(nodes))
            return False
        out = (ctypes.c_int * 64)()
        count = ctypes.c_int(0)
        self.session.beginGesture("Graph unweld")
        self.session.dll.Tonic_GraphUnweld(self.session.model, nodes[0], out,
                                           64, ctypes.byref(count))
        self.session.endGesture()
        self._status("Tonic Graph: unwelded into %d" % (count.value + 1))
        self.endOfEdit()
        return True


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
    "tube": ("tonicLoopsTube", "TubeLoop"),
    "fill": ("tonicLoopsFill", "FillLoop"),
    "hierarchy": ("tonicLoopsHierarchy", "HierarchyLoop"),
    "sculpt": ("tonicLoopsSculpt", "SculptLoop"),
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
        "graph": tonicModes.GRAPH_SUBMODES,
        "tube": tonicModes.TUBE_SUBMODES,
        "fill": tonicModes.FILL_SUBMODES,
        "hierarchy": tonicModes.HIERARCHY_SUBMODES,
        "sculpt": tonicModes.SCULPT_SUBMODES,
    }.get(str(modeId), ())


def setSubModeOn(state, modeId, subId):
    """Record a sub-mode for any mode; returns the status line."""
    setter = {
        "graph": tonicModes.SetActiveGraphSubMode,
        "tube": tonicModes.SetActiveTubeSubMode,
        "fill": tonicModes.SetActiveFillSubMode,
        "hierarchy": tonicModes.SetActiveHierarchySubMode,
        "sculpt": tonicModes.SetActiveSculptSubMode,
    }.get(str(modeId))
    return setter(state, subId) if setter is not None else ""
