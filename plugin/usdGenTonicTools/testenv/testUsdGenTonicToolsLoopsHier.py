#!/usr/bin/env python
# testUsdGenTonicToolsLoopsHier -- T0 for the V5 loops (plan/18 section 4
# "T0"): HierarchyLoop, SculptLoop and the fallback ladder driven over a
# fake session that records every C call.
#
#   python plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsLoopsHier.py
#
# No usdview, no pxr, no DLL. The fakes come from its V2 sibling
# testUsdGenTonicToolsLoops.py, so there is one recording DLL in the suite
# and one place to teach it a new entry point.
#
# What it proves that a T3 cannot:
#
#   * a hierarchy action over a selection is ONE undo bracket, and the
#     calls inside it come in the order the plan describes (merge before
#     the split that re-subdivide is made of, the group before its
#     selection);
#   * edge split mode records the drawn stroke and hands its two WORLD
#     points to Tonic_SubdivideTubeEdge -- the coefficients the audit found
#     hard-coded to (1, 0, 0) now come from the artist;
#   * Re-subdivide confirms on the status line and does nothing until the
#     second call (plan/18 section 6: no modal);
#   * a sculpt move is exactly one Tonic_SculptStrokeShaped carrying the
#     brush radius, the t window, mirror-X and length-preserving -- the
#     loop never weights a CV itself (section 7 G13);
#   * the ladder steps on the THIRD consecutive slow move and not before,
#     walks preview -> segments -> centers-only -> hover-off, and puts
#     every value back on release.
import os
import sys

failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def near(a, b, eps=1e-4):
    return abs(float(a) - float(b)) <= eps


# ---------------------------------------------------------------------------
# Fakes: the V2 recording DLL plus what V5 adds
# ---------------------------------------------------------------------------

def _deref(ref):
    return getattr(ref, "_obj", ref)


class Entry:
    """A recording entry point that takes ctypes' argtypes/restype.

    tonicLibStage.bindV5 writes those onto the handle it is given, exactly
    as it does for the real DLL, and a bound method cannot carry them.
    """

    def __init__(self, fn):
        self._fn = fn
        self.argtypes = None
        self.restype = None

    def __call__(self, *args, **kwargs):
        return self._fn(*args, **kwargs)


def makeFakes(base):
    """(FakeDll, FakeSession) subclasses for the V5 entry points."""

    class Dll(base.FakeDll):
        def __init__(self):
            base.FakeDll.__init__(self)
            self.previewFraction = 0.25
            self.displaySegments = 4
            self.levelModes = {}       # level -> (visible, xray, centers)
            self.childIds = [1, 2]
            self.children = {}         # tube id -> child ids
            self.parents = {}          # child id -> parent id
            self.levels = {}           # tube id -> hierarchy level
            self.activeCutEnabled = False
            self.expanded = set()
            for name in ("Tonic_SculptStrokeShaped",
                         "Tonic_SubdivideTubeEdge", "Tonic_SetLevelDrawMode",
                         "Tonic_GetLevelDrawMode", "Tonic_GetTubeChildren",
                         "Tonic_GetTubeCount", "Tonic_GetTubeLevel",
                         "Tonic_GetTubeParent", "Tonic_ReadTubeIds",
                         "Tonic_SetActiveCutEnabled",
                         "Tonic_GetActiveCutEnabled", "Tonic_SetTubeExpanded",
                         "Tonic_GetTubeExpanded", "Tonic_IsTubeVisible"):
                setattr(self, name, Entry(getattr(self, "_" + name[6].lower()
                                                  + name[7:])))

        # -- reads the V5 code asks for --------------------------------
        def Tonic_GetPreviewFraction(self, _ctx):
            self._record("Tonic_GetPreviewFraction")
            return self.previewFraction

        def Tonic_SetPreviewFraction(self, _ctx, fraction):
            value = getattr(fraction, "value", fraction)
            self._record("Tonic_SetPreviewFraction", (float(value),))
            self.previewFraction = float(value)
            return 0

        def Tonic_GetDisplaySegments(self, _ctx):
            self._record("Tonic_GetDisplaySegments")
            return self.displaySegments

        def Tonic_SetDisplaySegments(self, _ctx, segments):
            self._record("Tonic_SetDisplaySegments", (int(segments),))
            self.displaySegments = int(segments)
            return 0

        def _getTubeCount(self, _ctx):
            self._record("Tonic_GetTubeCount")
            return max(len(self.children) + 1, 1)

        def _getTubeChildren(self, _ctx, tubeId, out, cap, count):
            kids = self.children.get(int(tubeId), [])
            self._record("Tonic_GetTubeChildren", (int(tubeId),))
            for i, value in enumerate(kids[:cap]):
                out[i] = value
            _deref(count).value = min(len(kids), cap)
            return 0

        def _getTubeLevel(self, _ctx, tubeId):
            tubeId = int(tubeId)
            self._record("Tonic_GetTubeLevel", (tubeId,))
            return self.levels.get(tubeId, 1)

        def _getTubeParent(self, _ctx, tubeId, parent, childIndex):
            tubeId = int(tubeId)
            self._record("Tonic_GetTubeParent", (tubeId,))
            value = self.parents.get(tubeId)
            _deref(parent).value = -1 if value is None else int(value)
            _deref(childIndex).value = -1 if value is None else 0
            return 0

        def _readTubeIds(self, _ctx, out, cap, count):
            ids = sorted(self.levels)
            self._record("Tonic_ReadTubeIds")
            _deref(count).value = len(ids)
            if out is not None:
                for i, value in enumerate(ids[:cap]):
                    out[i] = value
            return 0

        def _setActiveCutEnabled(self, _ctx, enabled):
            self.activeCutEnabled = bool(int(enabled))
            self._record("Tonic_SetActiveCutEnabled",
                         (1 if self.activeCutEnabled else 0,))
            return 0

        def _getActiveCutEnabled(self, _ctx):
            self._record("Tonic_GetActiveCutEnabled")
            return 1 if self.activeCutEnabled else 0

        def _setTubeExpanded(self, _ctx, tubeId, expanded):
            tubeId = int(tubeId)
            if tubeId not in self.children or not self.children[tubeId]:
                return 1
            if int(expanded):
                self.expanded.add(tubeId)
            else:
                self.expanded.discard(tubeId)
                # Native collapse clears nested expansion records too.
                for candidate in tuple(self.expanded):
                    current = self.parents.get(candidate)
                    while current is not None:
                        if current == tubeId:
                            self.expanded.discard(candidate)
                            break
                        current = self.parents.get(current)
            self._record("Tonic_SetTubeExpanded", (tubeId, int(expanded)))
            return 0

        def _getTubeExpanded(self, _ctx, tubeId):
            self._record("Tonic_GetTubeExpanded", (int(tubeId),))
            return 1 if int(tubeId) in self.expanded else 0

        def _isTubeVisible(self, _ctx, tubeId):
            tubeId = int(tubeId)
            self._record("Tonic_IsTubeVisible", (tubeId,))
            if not self.activeCutEnabled:
                return 1
            if tubeId in self.expanded:
                return 0
            parent = self.parents.get(tubeId)
            while parent is not None:
                if parent not in self.expanded:
                    return 0
                parent = self.parents.get(parent)
            return 1

        def _getLevelDrawMode(self, _ctx, level, visible, xray,
                              centers):
            self._record("Tonic_GetLevelDrawMode", (int(level),))
            mode = self.levelModes.get(int(level), (1, 0, 0))
            _deref(visible).value = mode[0]
            _deref(xray).value = mode[1]
            _deref(centers).value = mode[2]
            return 0

        def _setLevelDrawMode(self, _ctx, level, visible, xray,
                              centers):
            self._record("Tonic_SetLevelDrawMode",
                         (int(level), int(visible), int(xray),
                          int(centers)))
            self.levelModes[int(level)] = (int(visible), int(xray),
                                           int(centers))
            return 0

        def _subdivideTubeEdge(self, _ctx, tubeId, worldA, worldB, seed,
                               out, cap, count):
            self._record("Tonic_SubdivideTubeEdge",
                         (int(tubeId),
                          (worldA[0], worldA[1], worldA[2]),
                          (worldB[0], worldB[1], worldB[2]), int(seed)))
            for i, value in enumerate(self.childIds[:cap]):
                out[i] = value
            _deref(count).value = min(len(self.childIds), cap)
            return 0

        def _sculptStrokeShaped(self, _ctx, tubeId, brush, _viewProj,
                                w, h, x, y, radiusPx, delta, amount,
                                tCenter, tRadius, preserveLength,
                                mirrorX, touched):
            self._record("Tonic_SculptStrokeShaped", {
                "tube": int(tubeId), "brush": brush.decode("ascii"),
                "w": int(w), "h": int(h),
                "x": float(getattr(x, "value", x)),
                "y": float(getattr(y, "value", y)),
                "radiusPx": float(getattr(radiusPx, "value", radiusPx)),
                "delta": (delta[0], delta[1], delta[2]),
                "amount": float(getattr(amount, "value", amount)),
                "tCenter": float(getattr(tCenter, "value", tCenter)),
                "tRadius": float(getattr(tRadius, "value", tRadius)),
                "preserveLength": int(preserveLength),
                "mirrorX": int(mirrorX)})
            _deref(touched).value = 3
            return 0

    class Session(base.FakeSession):
        def __init__(self, dll):
            base.FakeSession.__init__(self, dll)
            self.selects = []

        def select(self, kind, ids, subIds=None, subSubIds=None, mode=0):
            self.selects.append((int(kind), list(ids), int(mode)))
            self.selection[int(kind)] = list(ids)
            return True

    return Dll, Session


# ---------------------------------------------------------------------------
# HierarchyLoop
# ---------------------------------------------------------------------------

def testHierarchySelection(mods):
    print("-- HierarchyLoop: selection and navigation ---------------")
    tonicLib = mods["tonicLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)

    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_TUBE_VERT, "id": 7,
                       "subId": -1, "subSubId": -1})
    check(loop.press(sample(100.0, 100.0)), "a press claims the event")
    check(session.selects and
          session.selects[-1][0] == tonicLib.TONIC_PICK_TUBE_VERT and
          session.selects[-1][1] == [7],
          "clicking a tube selects it (%r)" % (session.selects[-1:],))
    loop.release(sample(100.0, 100.0))
    check(not session.gestureStack,
          "a selection click opens no undo bracket")

    session.rects = []
    loop.press(sample(50.0, 50.0, frozenset(["shift"])))
    loop.move(sample(300.0, 300.0, frozenset(["shift"])))
    loop.release(sample(300.0, 300.0, frozenset(["shift"])))
    check(len(session.rects) == 2 and
          session.rects[-1][4] == tonicLib.TONIC_PICK_TUBE_VERT,
          "Shift-drag marquees over tubes (%r)" % (session.rects[-1:],))

    dll.reset()
    check(loop.doubleClick(sample(100.0, 100.0)),
          "a double-click over a tube is claimed")
    check(dll.count("Tonic_SetFocusLevel") == 1,
          "and pushes the new focus level to the model")
    check(state.activeLevel >= 1, "the focus level is a real level (%d)"
          % state.activeLevel)


def testHierarchyActions(mods):
    print("-- HierarchyLoop: subdivide / merge / group --------------")
    tonicLib = mods["tonicLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    session.selection = {tonicLib.TONIC_PICK_TUBE_VERT: [0, 3]}

    state.subdivideCount = 4
    state.splitMode = "kmeans"
    check(loop.subdivideSelection(), "Subdivide runs over the selection")
    names = [n for n in dll.names() if n.startswith("Tonic_Subdivide")]
    check(names == ["Tonic_SubdivideTube", "Tonic_SubdivideTube"],
          "one Tonic_SubdivideTube per selected tube (%r)" % (names,))
    events = [name for name, _a in session.events]
    check(events.count("begin") == 1 and events.count("end") == 1,
          "the whole selection is ONE undo bracket (%r)" % (events,))
    order = [name for name, _a in session.events]
    check(order.index("end") < order.index("enqueueCommit"),
          "the bracket is sealed before the commit is enqueued")

    args = dll.argsOf("Tonic_SubdivideTube")
    check(args and args[0][2] == 4 and args[0][3] == b"kmeans",
          "the panel's count and split mode reach the ABI (%r)" % (args[0],))

    dll.reset()
    session.events = []
    check(loop.mergeSelected(), "Merge selected folds the siblings")
    check(dll.count("Tonic_MergeSelected") == 1,
          "through one Tonic_MergeSelected")

    dll.reset()
    session.events = []
    check(loop.group(), "Group builds an on-the-fly parent")
    check(dll.count("Tonic_GroupTubes") == 1, "through Tonic_GroupTubes")
    check([n for n, _a in session.events][:2] == ["begin", "end"],
          "inside one bracket (%r)" % (session.events,))

    dll.reset()
    session.events = []
    check(loop.makePersistent(), "Make persistent keeps the group")
    check(dll.count("Tonic_MakePersistent") == 1,
          "over the group the previous action selected, once")

    dll.reset()
    session.selection = {}
    check(not loop.subdivideSelection(),
          "Subdivide with nothing selected does nothing")
    check(dll.count("Tonic_SubdivideTube") == 0, "and calls no ABI")


def testHierarchyEdgeSplit(mods):
    print("-- HierarchyLoop: the drawn edge -------------------------")
    tonicLib = mods["tonicLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    session.selection = {tonicLib.TONIC_PICK_TUBE_VERT: [0]}
    state.splitMode = "edge"
    loop.setSubMode("subdivide")

    check(not loop.subdivideSelection(),
          "edge mode refuses to split before the stroke is drawn")
    check(dll.count("Tonic_SubdivideTubeEdge") == 0, "and calls no ABI")

    session.pickFn = lambda mask, x, y: None
    loop.press(sample(100.0, 200.0))
    loop.move(sample(200.0, 200.0))
    loop.release(sample(300.0, 200.0))
    check(loop.edge is not None, "the stroke is recorded as an edge")

    check(loop.subdivideSelection(), "Subdivide then splits along it")
    args = dll.argsOf("Tonic_SubdivideTubeEdge")
    check(len(args) == 1, "through one Tonic_SubdivideTubeEdge (%r)"
          % (args,))
    if args:
        tubeId, worldA, worldB, _seed = args[0]
        check(tubeId == 0, "over the selected tube")
        # The top-down fixture camera maps x pixels to world x directly.
        check(near(worldA[0], 1.0, 1e-3) and near(worldB[0], 3.0, 1e-3),
              "carrying the two WORLD ends of the stroke (%r -> %r)"
              % (worldA, worldB))
        check(near(worldA[1], 0.0, 1e-3), "on the scalp plane")


def testHierarchyResubdivide(mods):
    print("-- HierarchyLoop: Re-subdivide confirms ------------------")
    tonicLib = mods["tonicLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    session.selection = {tonicLib.TONIC_PICK_TUBE_VERT: [0]}
    dll.children = {0: [1, 2]}

    check(not loop.resubdivide(), "the first call only arms it")
    check(dll.count("Tonic_MergeChildren") == 0,
          "nothing is merged yet")
    check(any("Re-subdivide" in s for s in session.statuses),
          "and the confirm text goes to the status line, not a modal (%r)"
          % (session.statuses[-1:],))
    check(loop.resubdivide(), "the second call carries it out")
    order = [n for n in dll.names()
             if n in ("Tonic_MergeChildren", "Tonic_SubdivideTube")]
    check(order == ["Tonic_MergeChildren", "Tonic_SubdivideTube"],
          "as a merge followed by a split (%r)" % (order,))


def testHierarchyLevels(mods):
    print("-- HierarchyLoop: level display --------------------------")
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    state.activeLevel = 2
    dll.reset()
    check(loop.setSolo(2), "Solo reaches the model")
    calls = [c[1:] for c in dll.argsOf("Tonic_SetLevelDisplay")]
    check(calls and all(len(c) == 3 for c in calls),
          "through Tonic_SetLevelDisplay (%r)" % (calls,))
    check(any(level == 2 and visible for level, visible, _x in calls),
          "the soloed level stays visible (%r)" % (calls,))

    dll.reset()
    loop.setShowMaxLevel(1)
    calls = [c[1:] for c in dll.argsOf("Tonic_SetLevelDisplay")]
    check(calls, "Show <= n reaches the model too (%r)" % (calls,))

    dll.reset()
    loop.setLevelXray(1, True)
    calls = [c[1:] for c in dll.argsOf("Tonic_SetLevelDisplay")]
    check(any(level == 1 and xray for level, _v, xray in calls),
          "x-ray on L1 reaches the model (%r)" % (calls,))


def testHierarchyActiveCut(mods):
    print("-- HierarchyLoop: per-branch active cut ------------------")
    tonicLib = mods["tonicLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    # Two unrelated roots, each with its own L2 frontier.  This is the
    # regression global activeLevel could never express.
    dll.children = {0: [1, 2], 1: [3], 10: [11, 12]}
    dll.parents = {1: 0, 2: 0, 3: 1, 11: 10, 12: 10}
    dll.levels = {0: 1, 10: 1, 1: 2, 2: 2, 3: 3, 11: 2, 12: 2}
    session.selection = {tonicLib.TONIC_PICK_TUBE_VERT: [1],
                         tonicLib.TONIC_PICK_CENTER_CV: [1]}

    check(loop.activate() and dll.activeCutEnabled,
          "activating Hierarchy opts into the native active cut")
    check(session.selection.get(tonicLib.TONIC_PICK_TUBE_VERT) == [] and
          tonicLib.TONIC_PICK_CENTER_CV not in session.selection and
          session.hovers[-1:] == [(0, -1)],
          "activation drops hidden component owners and hover")
    check(dll._isTubeVisible(session.model, 0) == 1 and
          dll._isTubeVisible(session.model, 1) == 0 and
          dll._isTubeVisible(session.model, 10) == 1,
          "an empty cut starts at every root frontier")

    session.selection = {tonicLib.TONIC_PICK_TUBE_VERT: [0, 10]}
    check(loop.enterLevel(), "Enter expands every selected root branch")
    check(dll.expanded == {0, 10} and
          session.selection[tonicLib.TONIC_PICK_TUBE_VERT] == [1, 2, 11, 12],
          "Enter selects direct children without hiding the other branch")
    check(dll._isTubeVisible(session.model, 0) == 0 and
          dll._isTubeVisible(session.model, 1) == 1 and
          dll._isTubeVisible(session.model, 10) == 0 and
          dll._isTubeVisible(session.model, 11) == 1,
          "each expanded parent is replaced only by its own children")
    session.selection = {tonicLib.TONIC_PICK_CENTER_CV: [1]}
    check(loop.activate() and
          session.selection.get(tonicLib.TONIC_PICK_TUBE_VERT) == [1] and
          tonicLib.TONIC_PICK_CENTER_CV not in session.selection,
          "returning from a visible CV edit promotes its owner for Exit")
    check(state.focusParentId == 0 and state.focusAncestorIds == (0,) and
          state.activeLevel == 2,
          "one selected branch supplies compatibility focus and breadcrumb")
    check(dll.levelModes.get(1, (0, 0, 0))[1] == 0 and
          dll.levelModes.get(2, (0, 0, 0))[1] == 0,
          "mixed active-cut frontier levels stay opaque unless explicitly x-rayed")

    # A mixed-depth request selects A's L2 owner and its L3 child.  Exit
    # must collapse A only once; A's hidden child may not survive selected.
    check(loop._setExpanded(1, True), "a nested selected branch can expand")
    session.selection = {tonicLib.TONIC_PICK_TUBE_VERT: [1, 3],
                         tonicLib.TONIC_PICK_CENTER_CV: [3]}
    check(loop.exitLevel(), "Exit collapses the parent of each selected child")
    check(dll.expanded == {10} and
          session.selection[tonicLib.TONIC_PICK_TUBE_VERT] == [0] and
          tonicLib.TONIC_PICK_CENTER_CV not in session.selection and
          dll._isTubeVisible(session.model, 0) == 1 and
          dll._isTubeVisible(session.model, 1) == 0 and
          dll._isTubeVisible(session.model, 10) == 0 and
          dll._isTubeVisible(session.model, 11) == 1,
          "Exit collapses only the shallowest requested branch")

    session.selection = {tonicLib.TONIC_PICK_TUBE_VERT: [11]}
    check(loop.exitLevel() and dll.expanded == set() and
          session.selection[tonicLib.TONIC_PICK_TUBE_VERT] == [10] and
          dll._isTubeVisible(session.model, 10) == 1 and
          dll._isTubeVisible(session.model, 11) == 0,
          "a separate Exit restores B without reviving A descendants")
    check(loop.focusTube(0) and
          session.selection[tonicLib.TONIC_PICK_TUBE_VERT] == [0],
          "a tube-id breadcrumb targets one branch, never a global level")


# ---------------------------------------------------------------------------
# SculptLoop
# ---------------------------------------------------------------------------

def testSculptStroke(mods):
    print("-- SculptLoop: one shaped stroke per move ----------------")
    tonicLib = mods["tonicLib"]
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.brushRadiusPx = 32.0
    state.brushTRadius = 0.4
    state.sculptMirrorX = True
    state.sculptPreserveLength = True
    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 5,
                       "subId": 2, "subSubId": -1}
                      if mask == tonicLib.TONIC_PICK_CENTER_CV else None)

    check(loop.press(sample(200.0, 200.0)), "the press claims the event")
    check(session.gestureStack == ["Sculpt grab"],
          "and opens one gesture bracket (%r)" % (session.gestureStack,))
    check(dll.count("Tonic_SetBrushRing") >= 1,
          "the brush ring is placed on press")
    check(all(near(r, 32.0) for m, _x, _y, r in session.picks
              if m == tonicLib.TONIC_PICK_CENTER_CV),
          "the footprint pick uses the brush radius (%r)" % (session.picks,))

    loop.move(sample(240.0, 200.0))
    strokes = dll.argsOf("Tonic_SculptStrokeShaped")
    check(len(strokes) == 1, "one move is one stroke call (%r)"
          % (len(strokes),))
    if strokes:
        call = strokes[0]
        check(call["tube"] == 5 and call["brush"] == "grab",
              "carrying the picked tube and the brush (%r)" % (call,))
        check(near(call["radiusPx"], 32.0) and near(call["tRadius"], 0.4),
              "and the falloff inputs: radius %g, t radius %g"
              % (call["radiusPx"], call["tRadius"]))
        check(call["mirrorX"] == 1 and call["preserveLength"] == 1,
              "and mirror-X and length-preserving from the panel")
        check(abs(call["delta"][0]) > 1e-6,
              "the world delta follows the cursor (%r)" % (call["delta"],))
    loop.move(sample(280.0, 200.0))
    check(len(dll.argsOf("Tonic_SculptStrokeShaped")) == 2,
          "a second move is a second stroke, not a re-send")

    loop.release(sample(280.0, 200.0))
    check(not session.gestureStack, "release seals the bracket")
    check(("enqueueCommit", None) in session.events,
          "and enqueues the commit (%r)" % (session.events,))

    # Escape mid-stroke.
    session.events = []
    dll.reset()
    loop.press(sample(200.0, 200.0))
    loop.move(sample(230.0, 210.0))
    check(loop.cancel(), "Escape cancels a live stroke")
    check(("cancel", None) in session.events,
          "through Tonic_CancelGesture (%r)" % (session.events,))


def testSculptBrushes(mods):
    print("-- SculptLoop: the brush scalars -------------------------")
    tonicLib = mods["tonicLib"]
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.sculptStrength = 1.0
    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0,
                       "subId": 1, "subSubId": -1})
    loop.press(sample(200.0, 200.0))

    loop.setSubMode("twist")
    delta, amount = loop.strokeParams(cam, (200.0, 200.0), (300.0, 200.0))
    check(delta == (0.0, 0.0, 0.0) and amount > 0.0,
          "twist turns a rightward drag into radians (%g)" % amount)
    loop.setSubMode("lengthen")
    _d, grow = loop.strokeParams(cam, (200.0, 200.0), (200.0, 100.0))
    check(grow > 0.0, "dragging up lengthens (%g)" % grow)
    _d, shrink = loop.strokeParams(cam, (200.0, 200.0), (200.0, 300.0))
    check(shrink < 0.0, "and dragging down shortens (%g)" % shrink)
    loop.setSubMode("smooth")
    _d, strength = loop.strokeParams(cam, (200.0, 200.0), (300.0, 200.0))
    check(near(strength, 1.0), "smooth passes the strength (%g)" % strength)
    loop.setSubMode("comb")
    delta, push = loop.strokeParams(cam, (200.0, 200.0), (300.0, 200.0))
    check(push > 0.0 and delta != (0.0, 0.0, 0.0),
          "comb passes a direction and a fixed push (%g)" % push)

    loop.setSubMode("grab")
    state.sculptStrength = 0.25
    delta, amount = loop.strokeParams(cam, (200.0, 200.0),
                                      (300.0, 200.0))
    check(near(delta[0], 0.25) and near(amount, 0.0),
          "grab applies the brush strength to its world delta (%r)" %
          (delta,))
    state.sculptStrength = 1.0
    state.brushRadiusPx = 24.0
    loop.adjustRadius(8.0)
    check(near(state.brushRadiusPx, 32.0), "] grows the brush radius")
    loop.adjustRadius(-1000.0)
    check(near(state.brushRadiusPx, 2.0),
          "[ shrinks it and clamps (%g)" % state.brushRadiusPx)


def testSculptRadiusResize(mods):
    print("-- SculptLoop: F-drag brush radius ----------------------")
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.brushRadiusPx = 24.0
    check(loop.beginRadiusResize(sample(100.0, 180.0)),
          "F-LMB begins a UI-only radius drag")
    check(not session.gestureStack and
          not dll.argsOf("Tonic_SculptStrokeShaped"),
          "resizing opens no sculpt undo bracket or stroke")
    check(loop.resizeRadius(sample(160.0, 20.0)) and
          near(loop.brushRadiusPx(), 54.0) and near(state.brushRadiusPx, 24.0),
          "horizontal travel previews radius without a partial UI commit (%g)"
          % loop.brushRadiusPx())
    check(loop.endRadiusResize(sample(180.0, 700.0)) and
          near(state.brushRadiusPx, 64.0) and not loop.resizingRadius,
          "LMB release commits one final radius without sculpting (%g)"
          % state.brushRadiusPx)
    check(not session.gestureStack and
          not dll.argsOf("Tonic_SculptStrokeShaped"),
          "the completed width change authored no stroke")
    check(loop.beginRadiusResize(sample(200.0, 100.0)) and
          loop.resizeRadius(sample(-2000.0, 100.0)) and
          near(loop.brushRadiusPx(), 2.0),
          "left F-drag clamps at the minimum radius")
    check(loop.cancelRadiusResize() and near(state.brushRadiusPx, 64.0),
          "Escape/capture cancel restores the press-time radius")


def testSculptViewPlane(mods):
    print("-- SculptLoop: frozen view-plane stroke -----------------")
    tonicLib = mods["tonicLib"]
    sculpt = mods["tonicLoopsSculpt"]
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.brushRadiusPx = 40.0
    # No geometry item exists at this press.  The existing explicit tube
    # selection is nevertheless close enough in screen space to begin a
    # sculpt gesture; no unselected/default tube may be guessed.
    session.selection = {tonicLib.TONIC_PICK_TUBE_VERT: [9]}
    session.pickFn = lambda _mask, _x, _y: None
    oldCount = sculpt.tonicHierarchy.tubeCenterCount
    oldPoint = sculpt.tonicHierarchy.tubeCenterHandle
    sculpt.tonicHierarchy.tubeCenterCount = lambda *_args: 5
    sculpt.tonicHierarchy.tubeCenterHandle = (
        lambda *_args: (2.0, 0.0, 2.0))  # projects to (200, 200)
    try:
        press = sample(224.0, 216.0)
        check(loop.press(press),
              "selected tube starts a near-empty-space sculpt press")
        check(loop._tube == 9 and loop._cv > 0,
              "the selected owner resolves to an editable center CV")
        picksAtPress, raysAtPress = len(session.picks), len(session.rays)
        loop.move(sample(360.0, 300.0))
        check(len(session.picks) == picksAtPress and
              len(session.rays) == raysAtPress,
              "active drag never repicks or raycasts geometry")
        strokes = dll.argsOf("Tonic_SculptStrokeShaped")
        check(strokes and near(strokes[-1]["x"], 200.0) and
              near(strokes[-1]["y"], 200.0),
              "Grab keeps the projected press footprint, not the trail")
        rings = dll.argsOf("Tonic_SetBrushRing")
        if rings:
            centre, normal = rings[-1][1], rings[-1][2]
            projected = cam.worldToPixels((centre[0], centre[1], centre[2]))
            check(projected is not None and near(projected[0], 360.0) and
                  near(projected[1], 300.0),
                  "the active brush ring follows the cursor on its view plane")
            check(abs(float(normal[1])) > 0.99,
                  "the frozen ring normal is camera-forward, not a surface normal")
        else:
            check(False, "the active view-plane brush ring is published")
        check(loop.cancel(), "Escape cancels the background view-plane drag")
    finally:
        sculpt.tonicHierarchy.tubeCenterCount = oldCount
        sculpt.tonicHierarchy.tubeCenterHandle = oldPoint


# ---------------------------------------------------------------------------
# The fallback ladder
# ---------------------------------------------------------------------------

def testLadder(mods):
    print("-- the fallback ladder -----------------------------------")
    tonicLadder = mods["tonicLadder"]
    Dll, Session = mods["fakes"]
    dll = Dll()
    session = Session(dll)
    state = mods["TonicToolState"]()
    ladder = tonicLadder.FallbackLadder(session, state)

    ladder.arm(editedLevel=1, levels=(1, 2))
    check(ladder.armed and ladder.step == 0,
          "a press arms it at full fidelity")
    check(near(dll.previewFraction, 0.25),
          "and changes nothing by itself")

    check(not ladder.noteMove(2.0), "a fast move does not step it")
    check(not ladder.noteMove(20.0), "nor does one slow move")
    check(not ladder.noteMove(20.0), "nor two")
    check(ladder.noteMove(20.0), "the third consecutive slow move steps it")
    check(ladder.step == 1 and near(dll.previewFraction, 0.25),
          "to the 25%% preview (step %d, %g)"
          % (ladder.step, dll.previewFraction))
    check(near(ladder.firstTriggerMs, 20.0),
          "recording the move time that triggered it (%g ms)"
          % ladder.firstTriggerMs)

    ladder.noteMove(20.0)
    ladder.noteMove(2.0)             # a fast move resets the run
    ladder.noteMove(20.0)
    ladder.noteMove(20.0)
    check(ladder.step == 1,
          "one fast move in the middle resets the count (step %d)"
          % ladder.step)
    check(ladder.noteMove(20.0), "three more step it again")
    check(ladder.step == 2 and near(dll.previewFraction, 0.10),
          "to the 10%% preview (%g)" % dll.previewFraction)

    for _ in range(3):
        ladder.noteMove(20.0)
    check(ladder.step == 3 and near(dll.previewFraction, 0.0),
          "then the preview goes off (%g)" % dll.previewFraction)
    for _ in range(3):
        ladder.noteMove(20.0)
    check(ladder.step == 4 and dll.displaySegments == 2,
          "then the display segments halve, 4 -> %d" % dll.displaySegments)
    for _ in range(3):
        ladder.noteMove(20.0)
    check(ladder.step == 5, "then the centers-only rung (step %d)"
          % ladder.step)
    check(dll.levelModes.get(2, (1, 0, 0))[2] == 1,
          "the non-edited level draws centers only (%r)" % (dll.levelModes,))
    check(dll.levelModes.get(1, (1, 0, 0))[2] == 0,
          "and the edited level keeps its full draw (%r)" % (dll.levelModes,))
    check(not ladder.hoverSuppressed, "hover is still on")
    for _ in range(3):
        ladder.noteMove(20.0)
    check(ladder.step == 6 and ladder.hoverSuppressed,
          "the last rung turns hover off (step %d)" % ladder.step)
    check(session.hovers and session.hovers[-1] == (0, -1),
          "clearing the hover it had (%r)" % (session.hovers[-1:],))
    for _ in range(6):
        ladder.noteMove(20.0)
    check(ladder.step == 6, "and the ladder stops at the bottom")
    check(int(state.ladderStep) == 6,
          "the step is on the tool state for the status strip (%r)"
          % state.ladderStep)

    session.published = []
    check(ladder.restore(), "release restores it")
    check(near(dll.previewFraction, 0.25) and dll.displaySegments == 4,
          "preview and segments come back (%g, %d)"
          % (dll.previewFraction, dll.displaySegments))
    check(dll.levelModes.get(2, (1, 0, 0))[2] == 0,
          "and so does the level draw mode (%r)" % (dll.levelModes,))
    check(session.published and
          session.published[-1] == mods["tonicLib"].TONIC_DIRTY_ALL,
          "republishing everything at full fidelity (%r)"
          % (session.published[-1:],))
    check(ladder.step == 0 and not ladder.armed and
          int(state.ladderStep) == 0,
          "with the ladder back at the top")
    check(not ladder.restore(), "a second restore is a no-op")


# ---------------------------------------------------------------------------
# Wiring
# ---------------------------------------------------------------------------

def newHierarchy(mods):
    Dll, Session = mods["fakes"]
    dll = Dll()
    session = Session(dll)
    state = mods["TonicToolState"]()
    loop = mods["tonicLoopsHierarchy"].HierarchyLoop(session, state)
    cam = mods["base"].topDownCamera(mods["tonicCamera"])
    Sample = mods["tonicLoops"].Sample

    def sample(x, y, mods_=frozenset()):
        return Sample(session, cam, x, y, mods_)

    return loop, dll, session, state, cam, sample


def newSculpt(mods):
    Dll, Session = mods["fakes"]
    dll = Dll()
    session = Session(dll)
    state = mods["TonicToolState"]()
    loop = mods["tonicLoopsSculpt"].SculptLoop(session, state)
    cam = mods["base"].topDownCamera(mods["tonicCamera"])
    Sample = mods["tonicLoops"].Sample

    def sample(x, y, mods_=frozenset()):
        return Sample(session, cam, x, y, mods_)

    return loop, dll, session, state, cam, sample


def main():
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import tonicTestPackage
    tonicTestPackage.install()
    import testUsdGenTonicToolsLoops as base
    from usdGenTonicTools import (tonicCamera, tonicLadder, tonicLib,
                                  tonicLoops, tonicLoopsHierarchy,
                                  tonicLoopsSculpt)
    from usdGenTonicTools.tonicToolState import TonicToolState
    Dll, Session = makeFakes(base)
    mods = {"base": base, "fakes": (Dll, Session), "tonicCamera": tonicCamera,
            "tonicLadder": tonicLadder, "tonicLib": tonicLib,
            "tonicLoops": tonicLoops,
            "tonicLoopsHierarchy": tonicLoopsHierarchy,
            "tonicLoopsSculpt": tonicLoopsSculpt,
            "TonicToolState": TonicToolState}
    check(tonicLoops.makeLoop("hierarchy", None, None) is not None,
          "Hierarchy has a loop from V5 on")
    check(tonicLoops.makeLoop("sculpt", None, None) is not None,
          "and so does Sculpt")
    testHierarchySelection(mods)
    testHierarchyActions(mods)
    testHierarchyEdgeSplit(mods)
    testHierarchyResubdivide(mods)
    testHierarchyLevels(mods)
    testHierarchyActiveCut(mods)
    testSculptStroke(mods)
    testSculptBrushes(mods)
    testSculptRadiusResize(mods)
    testSculptViewPlane(mods)
    testLadder(mods)
    print("testUsdGenTonicToolsLoopsHier: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
