#!/usr/bin/env python
# testUsdGenPomadeToolsLoopsHier -- T0 for the V5 loops (plan/18 section 4
# "T0"): HierarchyLoop, SculptLoop and the fallback ladder driven over a
# fake session that records every C call.
#
#   python plugin/usdGenPomadeTools/testenv/testUsdGenPomadeToolsLoopsHier.py
#
# No usdview, no pxr, no DLL. The fakes come from its V2 sibling
# testUsdGenPomadeToolsLoops.py, so there is one recording DLL in the suite
# and one place to teach it a new entry point.
#
# What it proves that a T3 cannot:
#
#   * a hierarchy action over a selection is ONE undo bracket, and the
#     calls inside it come in the order the plan describes (merge before
#     the split that re-subdivide is made of, the group before its
#     selection);
#   * edge split mode records the drawn stroke and hands its two WORLD
#     points to Pomade_SubdivideTubeEdge -- the coefficients the audit found
#     hard-coded to (1, 0, 0) now come from the artist;
#   * Re-subdivide confirms on the status line and does nothing until the
#     second call (plan/18 section 6: no modal);
#   * a sculpt move is exactly one Pomade_SculptStrokeShaped carrying the
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

    pomadeLibStage.bindV5 writes those onto the handle it is given, exactly
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
            for name in ("Pomade_SculptStrokeShaped",
                         "Pomade_SubdivideTubeEdge", "Pomade_SetLevelDrawMode",
                         "Pomade_GetLevelDrawMode", "Pomade_GetTubeChildren",
                         "Pomade_GetTubeCount", "Pomade_GetTubeLevel",
                         "Pomade_GetTubeParent", "Pomade_ReadTubeIds",
                         "Pomade_SetActiveCutEnabled",
                         "Pomade_GetActiveCutEnabled", "Pomade_SetTubeExpanded",
                         "Pomade_GetTubeExpanded", "Pomade_IsTubeVisible"):
                setattr(self, name, Entry(getattr(self, "_" + name[6].lower()
                                                  + name[7:])))

        # -- reads the V5 code asks for --------------------------------
        def Pomade_GetPreviewFraction(self, _ctx):
            self._record("Pomade_GetPreviewFraction")
            return self.previewFraction

        def Pomade_SetPreviewFraction(self, _ctx, fraction):
            value = getattr(fraction, "value", fraction)
            self._record("Pomade_SetPreviewFraction", (float(value),))
            self.previewFraction = float(value)
            return 0

        def Pomade_GetDisplaySegments(self, _ctx):
            self._record("Pomade_GetDisplaySegments")
            return self.displaySegments

        def Pomade_SetDisplaySegments(self, _ctx, segments):
            self._record("Pomade_SetDisplaySegments", (int(segments),))
            self.displaySegments = int(segments)
            return 0

        def _getTubeCount(self, _ctx):
            self._record("Pomade_GetTubeCount")
            return max(len(self.children) + 1, 1)

        def _getTubeChildren(self, _ctx, tubeId, out, cap, count):
            kids = self.children.get(int(tubeId), [])
            self._record("Pomade_GetTubeChildren", (int(tubeId),))
            for i, value in enumerate(kids[:cap]):
                out[i] = value
            _deref(count).value = min(len(kids), cap)
            return 0

        def _getTubeLevel(self, _ctx, tubeId):
            tubeId = int(tubeId)
            self._record("Pomade_GetTubeLevel", (tubeId,))
            return self.levels.get(tubeId, 1)

        def _getTubeParent(self, _ctx, tubeId, parent, childIndex):
            tubeId = int(tubeId)
            self._record("Pomade_GetTubeParent", (tubeId,))
            value = self.parents.get(tubeId)
            _deref(parent).value = -1 if value is None else int(value)
            _deref(childIndex).value = -1 if value is None else 0
            return 0

        def _readTubeIds(self, _ctx, out, cap, count):
            ids = sorted(self.levels)
            self._record("Pomade_ReadTubeIds")
            _deref(count).value = len(ids)
            if out is not None:
                for i, value in enumerate(ids[:cap]):
                    out[i] = value
            return 0

        def _setActiveCutEnabled(self, _ctx, enabled):
            self.activeCutEnabled = bool(int(enabled))
            self._record("Pomade_SetActiveCutEnabled",
                         (1 if self.activeCutEnabled else 0,))
            return 0

        def _getActiveCutEnabled(self, _ctx):
            self._record("Pomade_GetActiveCutEnabled")
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
            self._record("Pomade_SetTubeExpanded", (tubeId, int(expanded)))
            return 0

        def _getTubeExpanded(self, _ctx, tubeId):
            self._record("Pomade_GetTubeExpanded", (int(tubeId),))
            return 1 if int(tubeId) in self.expanded else 0

        def _isTubeVisible(self, _ctx, tubeId):
            tubeId = int(tubeId)
            self._record("Pomade_IsTubeVisible", (tubeId,))
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
            self._record("Pomade_GetLevelDrawMode", (int(level),))
            mode = self.levelModes.get(int(level), (1, 0, 0))
            _deref(visible).value = mode[0]
            _deref(xray).value = mode[1]
            _deref(centers).value = mode[2]
            return 0

        def _setLevelDrawMode(self, _ctx, level, visible, xray,
                              centers):
            self._record("Pomade_SetLevelDrawMode",
                         (int(level), int(visible), int(xray),
                          int(centers)))
            self.levelModes[int(level)] = (int(visible), int(xray),
                                           int(centers))
            return 0

        def _subdivideTubeEdge(self, _ctx, tubeId, worldA, worldB, seed,
                               out, cap, count):
            self._record("Pomade_SubdivideTubeEdge",
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
            self._record("Pomade_SculptStrokeShaped", {
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
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)

    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_TUBE_VERT, "id": 7,
                       "subId": -1, "subSubId": -1})
    check(loop.press(sample(100.0, 100.0)), "a press claims the event")
    check(session.selects and
          session.selects[-1][0] == pomadeLib.POMADE_PICK_TUBE_VERT and
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
          session.rects[-1][4] == pomadeLib.POMADE_PICK_TUBE_VERT,
          "Shift-drag marquees over tubes (%r)" % (session.rects[-1:],))

    dll.reset()
    check(loop.doubleClick(sample(100.0, 100.0)),
          "a double-click over a tube is claimed")
    check(dll.count("Pomade_SetFocusLevel") == 1,
          "and pushes the new focus level to the model")
    check(state.activeLevel >= 1, "the focus level is a real level (%d)"
          % state.activeLevel)


def testHierarchySelectionMatrix(mods):
    """SL-01: Hierarchy clicks and bands read the shared modifier table."""
    print("-- HierarchyLoop: click and band modifier matrix ---------")
    pomadeLib = mods["pomadeLib"]
    base = mods["base"]
    Dll, _Session = mods["fakes"]
    dll = Dll()
    session = base.MatrixSession(dll)
    state = mods["PomadeToolState"]()
    loop = mods["pomadeLoopsHierarchy"].HierarchyLoop(session, state)
    cam = base.topDownCamera(mods["pomadeCamera"])
    Sample = mods["pomadeLoops"].Sample
    kind = pomadeLib.POMADE_PICK_TUBE_VERT
    names = {"A": (7, -1, -1), "B": (8, -1, -1), "C": (9, -1, -1)}
    session.bandHits = {kind: [names["A"], names["C"]]}

    def reset():
        session.items = {kind: [names["A"], names["B"]]}

    def read():
        return session.readSelection(kind)

    def click(entry, modifiers):
        session.pickFn = (lambda mask, x, y:
                          {"kind": kind, "id": entry[0], "subId": -1,
                           "subSubId": -1} if mask & kind else None)
        loop.press(Sample(session, cam, 100.0, 100.0, modifiers))
        loop.release(Sample(session, cam, 100.0, 100.0, modifiers))

    def band(modifiers):
        session.pickFn = lambda mask, x, y: None
        loop.press(Sample(session, cam, 50.0, 50.0, modifiers))
        loop.move(Sample(session, cam, 250.0, 250.0, modifiers))
        loop.release(Sample(session, cam, 300.0, 300.0, modifiers))

    # SL-02: every row, the plain band included -- a plain press on empty
    # space is a marquee in every sub-mode but the edge stroke.
    base.runSelectionMatrix("Hierarchy", names, reset, read, click, band)
    for sub in ("navigate", "subdivide", "merge", "group", "levels"):
        loop.setSubMode(sub)
        state.splitMode = "kmeans"
        session.rects = []
        reset()
        session.pickFn = lambda mask, x, y: None
        loop.press(Sample(session, cam, 50.0, 50.0))
        check(loop.marqueeRect() == (50.0, 50.0),
              "%s: a plain press on empty space starts a marquee" % sub)
        loop.move(Sample(session, cam, 250.0, 250.0))
        check(session.rects and
              session.rects[-1][5] == pomadeLib.POMADE_SELECT_SET,
              "%s: moving it calls selectRect (%r)"
              % (sub, session.rects[-1:]))
        dll.reset()
        loop.release(Sample(session, cam, 250.0, 250.0))
        if sub == "group":
            # MD-04: Group's plain band groups exactly what it boxed.
            groups = dll.argsOf("Pomade_GroupTubes")
            check(len(groups) == 1 and
                  list(groups[0][1][:groups[0][2]]) == [7, 9],
                  "group: the band grouped the tubes it caught (%r)"
                  % ([list(g[1][:g[2]]) for g in groups],))
            continue
        check(read() == sorted([names["A"], names["C"]]),
              "%s: the band replaced the selection (%r)" % (sub, read()))
        check(dll.count("Pomade_GroupTubes") == 0,
              "%s: and grouped nothing" % sub)
    # The edge stroke keeps its plain press in subdivide + edge.
    loop.setSubMode("subdivide")
    state.splitMode = "edge"
    loop.press(Sample(session, cam, 50.0, 50.0))
    check(loop.marqueeRect() is None,
          "subdivide + edge: a plain press draws the edge, not a band")
    loop.cancel()
    state.splitMode = "kmeans"
    loop.setSubMode("navigate")
    # A plain click without travel on empty space deselects on release.
    reset()
    session.bandHits = {}
    loop.press(Sample(session, cam, 50.0, 50.0))
    loop.release(Sample(session, cam, 50.0, 50.0))
    check(read() == [], "a plain empty click deselects (%r)" % (read(),))
    check(set(session.abiModes) <= {pomadeLib.POMADE_SELECT_SET,
                                    pomadeLib.POMADE_SELECT_ADD,
                                    pomadeLib.POMADE_SELECT_TOGGLE},
          "only real Pomade_Select* modes reached the ABI (%r)"
          % sorted(set(session.abiModes)))
    check(not session.gestureStack,
          "no selection gesture opened an undo bracket")


def testHierarchyActions(mods):
    print("-- HierarchyLoop: subdivide / merge / group --------------")
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0, 3]}

    state.subdivideCount = 4
    state.splitMode = "kmeans"
    check(loop.subdivideSelection(), "Subdivide runs over the selection")
    names = [n for n in dll.names() if n.startswith("Pomade_Subdivide")]
    check(names == ["Pomade_SubdivideTube", "Pomade_SubdivideTube"],
          "one Pomade_SubdivideTube per selected tube (%r)" % (names,))
    events = [name for name, _a in session.events]
    check(events.count("begin") == 1 and events.count("end") == 1,
          "the whole selection is ONE undo bracket (%r)" % (events,))
    order = [name for name, _a in session.events]
    check(order.index("end") < order.index("enqueueCommit"),
          "the bracket is sealed before the commit is enqueued")

    args = dll.argsOf("Pomade_SubdivideTube")
    check(args and args[0][2] == 4 and args[0][3] == b"kmeans",
          "the panel's count and split mode reach the ABI (%r)" % (args[0],))

    dll.reset()
    session.events = []
    check(loop.mergeSelected(), "Merge selected folds the siblings")
    check(dll.count("Pomade_MergeSelected") == 1,
          "through one Pomade_MergeSelected")

    dll.reset()
    session.events = []
    check(loop.group(), "Group builds an on-the-fly parent")
    check(dll.count("Pomade_GroupTubes") == 1, "through Pomade_GroupTubes")
    check([n for n, _a in session.events][:2] == ["begin", "end"],
          "inside one bracket (%r)" % (session.events,))

    dll.reset()
    session.events = []
    check(loop.makePersistent(), "Make persistent keeps the group")
    check(dll.count("Pomade_MakePersistent") == 1,
          "over the group the previous action selected, once")

    dll.reset()
    session.selection = {}
    check(not loop.subdivideSelection(),
          "Subdivide with nothing selected does nothing")
    check(dll.count("Pomade_SubdivideTube") == 0, "and calls no ABI")


def testHierarchyEdgeSplit(mods):
    print("-- HierarchyLoop: the drawn edge -------------------------")
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0]}
    state.splitMode = "edge"
    loop.setSubMode("subdivide")

    check(not loop.subdivideSelection(),
          "edge mode refuses to split before the stroke is drawn")
    check(dll.count("Pomade_SubdivideTubeEdge") == 0, "and calls no ABI")

    session.pickFn = lambda mask, x, y: None
    loop.press(sample(100.0, 200.0))
    loop.move(sample(200.0, 200.0))
    loop.release(sample(300.0, 200.0))
    check(loop.edge is not None, "the stroke is recorded as an edge")

    check(loop.subdivideSelection(), "Subdivide then splits along it")
    args = dll.argsOf("Pomade_SubdivideTubeEdge")
    check(len(args) == 1, "through one Pomade_SubdivideTubeEdge (%r)"
          % (args,))
    if args:
        tubeId, worldA, worldB, _seed = args[0]
        check(tubeId == 0, "over the selected tube")
        # The top-down fixture camera maps x pixels to world x directly.
        check(near(worldA[0], 1.0, 1e-3) and near(worldB[0], 3.0, 1e-3),
              "carrying the two WORLD ends of the stroke (%r -> %r)"
              % (worldA, worldB))
        check(near(worldA[1], 0.0, 1e-3), "on the scalp plane")

    # MD-03: the split consumes the stroke, so a second Shift+D cannot cut
    # the new children along a line drawn for their parent.
    check(loop.edge is None, "the split consumed the recorded edge")
    check(loop.edgePreview() == {"live": None, "recorded": None},
          "and the overlay has nothing left to draw (%r)"
          % (loop.edgePreview(),))
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0]}
    check(not loop.subdivideSelection(), "a second Shift+D is refused")
    check(len(dll.argsOf("Pomade_SubdivideTubeEdge")) == 1,
          "and splits nothing (%d edge splits)"
          % len(dll.argsOf("Pomade_SubdivideTubeEdge")))


def testHierarchyEdgeOverlay(mods):
    print("-- HierarchyLoop: the edge preview and its lifetime ------")
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0]}
    session.pickFn = lambda mask, x, y: None
    state.splitMode = "kmeans"
    loop.setSubMode("subdivide")
    check(loop.edgePreview() == {"live": None, "recorded": None},
          "no edge preview outside edge split mode")
    state.splitMode = "edge"

    loop.press(sample(100.0, 200.0))
    loop.move(sample(250.0, 200.0))
    preview = loop.edgePreview()
    check(preview["live"] is not None and preview["recorded"] is None,
          "the stroke in flight is previewed live (%r)" % (preview,))
    if preview["live"] is not None:
        check(near(preview["live"][1][0], 2.5, 1e-3),
              "its free end follows the cursor (%r)" % (preview["live"],))
    loop.release(sample(300.0, 200.0))
    preview = loop.edgePreview()
    check(preview["live"] is None and preview["recorded"] is not None,
          "release turns it into the recorded edge (%r)" % (preview,))

    # A too-short re-stroke keeps the recorded edge and its tube.
    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_TUBE_VERT, "id": 3,
                       "subId": -1, "subSubId": -1})
    loop.press(sample(100.0, 100.0))
    loop.release(sample(101.0, 100.0))
    check(loop.edge is not None and loop._edgeTube == 0,
          "a rejected short stroke leaves the recorded edge on T0 (T%d)"
          % loop._edgeTube)
    session.pickFn = lambda mask, x, y: None

    # The edge is drawn for ONE tube: any other selection is refused.
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0, 3]}
    dll.reset()
    check(not loop.subdivideSelection(),
          "Shift+D over two tubes with a one-tube edge is refused")
    check(dll.count("Pomade_SubdivideTubeEdge") == 0 and
          not session.gestureStack, "and opens no bracket, calls no ABI")
    check("select only T0" in session.statuses[-1],
          "the status names the stroked tube (%r)" % session.statuses[-1:])
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [3]}
    check(not loop.subdivideSelection(), "so is a different single tube")
    check("draw the split across T3" in session.statuses[-1],
          "which is told to draw the split across T3 (%r)"
          % session.statuses[-1:])

    # Escape: first the idle recorded edge goes, with no selection change.
    check(loop.cancel(), "Escape with a recorded edge is consumed")
    check(loop.edge is None, "and forgets the edge")
    check(not loop.cancel(), "a second Escape has nothing left to cancel")

    # A sub-mode change drops it too.
    loop.press(sample(100.0, 200.0))
    loop.release(sample(300.0, 200.0))
    check(loop.edge is not None, "a fresh edge is recorded")
    loop.setSubMode("navigate")
    check(loop.edge is None, "leaving Subdivide drops the recorded edge")
    loop.setSubMode("subdivide")
    loop.press(sample(100.0, 200.0))
    loop.release(sample(300.0, 200.0))
    loop.deactivate()
    check(loop.edge is None, "leaving Hierarchy drops it as well")


def testHierarchyHonestFailures(mods):
    print("-- HierarchyLoop: failures are reported, not overwritten --")
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0, 3]}
    dll.children = {0: [1, 2], 3: [4, 5]}

    def mergeChildren(_ctx, tubeId):
        dll._record("Pomade_MergeChildren", (int(tubeId),))
        return 7 if int(tubeId) == 3 else 0
    dll.Pomade_MergeChildren = Entry(mergeChildren)
    session.events = []
    check(not loop.mergeChildrenOfSelection(),
          "Merge children with one failed tube reports failure")
    final = session.statuses[-1] if session.statuses else ""
    check("1 done, 1 failed" in final and "T3" in final,
          "the final status says what failed (%r)" % final)
    events = [name for name, _a in session.events]
    check(events.count("begin") == 1 and events.count("end") == 1,
          "the tube that merged is still one undo step (%r)" % (events,))

    # Every subdivide failing rolls the bracket back: no empty undo step.
    def failSubdivide(*_args):
        dll._record("Pomade_SubdivideTube")
        return 5
    dll.Pomade_SubdivideTube = Entry(failSubdivide)
    state.splitMode = "kmeans"
    session.events = []
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0, 3]}
    check(not loop.subdivideSelection(), "an all-failed Subdivide is False")
    events = [name for name, _a in session.events]
    check("cancel" in events and "end" not in events and
          "enqueueCommit" not in events,
          "and cancels its gesture instead of committing (%r)" % (events,))
    check("0 done, 2 failed" in session.statuses[-1],
          "with the count in the status (%r)" % session.statuses[-1:])

    # A refused bracket means nothing runs at all.
    session.beginGesture = lambda label: False
    dll.reset()
    check(not loop.makePersistent(), "a refused undo bracket is a refusal")
    check(dll.count("Pomade_MakePersistent") == 0,
          "and the action never reaches the ABI")
    check("undo step" in session.statuses[-1],
          "the status says why (%r)" % session.statuses[-1:])


def testHierarchyResubdivide(mods):
    print("-- HierarchyLoop: Re-subdivide confirms ------------------")
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0]}
    dll.children = {0: [1, 2]}

    check(not loop.resubdivideArmed, "nothing is armed before the first call")
    check(not loop.resubdivide(), "the first call only arms it")
    check(loop.resubdivideArmed,
          "resubdivideArmed says so, for the dock's confirm button (DK-08)")
    check(dll.count("Pomade_MergeChildren") == 0,
          "nothing is merged yet")
    check(any("Re-subdivide" in s for s in session.statuses),
          "and the confirm text goes to the status line, not a modal (%r)"
          % (session.statuses[-1:],))
    # MD-03: Escape disarms, so the next press re-prompts instead of
    # silently discarding the children's sculpt deltas.
    check(loop.cancel(), "Escape over an armed Re-subdivide is consumed")
    check(not loop.resubdivideArmed, "and cancel() disarms it")
    session.statuses = []
    check(not loop.resubdivide(), "after Escape Re-subdivide re-prompts")
    check(loop.resubdivideArmed, "which arms it again")
    check(dll.count("Pomade_MergeChildren") == 0 and
          any("confirm" in s for s in session.statuses),
          "with the confirm text again, merging nothing (%r)"
          % (session.statuses[-1:],))
    check(loop.resubdivide(), "the second call carries it out")
    order = [n for n in dll.names()
             if n in ("Pomade_MergeChildren", "Pomade_SubdivideTube")]
    check(order == ["Pomade_MergeChildren", "Pomade_SubdivideTube"],
          "as a merge followed by a split (%r)" % (order,))
    check(not loop.resubdivideArmed, "carrying it out disarms it")

    # DK-08: the confirm belongs to the selection it was armed over.
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0]}
    dll.children = {0: [1, 2]}
    dll.reset()
    check(not loop.resubdivide() and loop.resubdivideArmed,
          "a fresh press arms it over tube 0")
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [5]}
    check(not loop.resubdivideArmed,
          "selecting something else lapses the confirm")
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0]}
    check(not loop.resubdivideArmed and not loop.resubdivide() and
          dll.count("Pomade_MergeChildren") == 0,
          "and going back re-prompts instead of discarding the children")

    # A parent whose merge went through but whose re-split refused must
    # not be sealed merged-flat beside a good parent: the whole action
    # rolls back and the status says so.
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0, 3]}
    dll.children = {0: [1, 2], 3: [4, 5]}

    def splitRefusesT3(_ctx, tubeId, count, mode, seed, out, cap, got):
        dll._record("Pomade_SubdivideTube", (int(tubeId),))
        if int(tubeId) == 3:
            return 5
        _deref(got).value = 0
        return 0
    realSplit = dll.__dict__.get("Pomade_SubdivideTube")
    dll.Pomade_SubdivideTube = Entry(splitRefusesT3)
    try:
        dll.reset()
        check(not loop.resubdivide() and loop.resubdivideArmed,
              "a two-parent Re-subdivide arms first")
        session.events = []
        check(not loop.resubdivide(),
              "a half-applied Re-subdivide reports failure")
        events = [name for name, _a in session.events]
        check(events.count("begin") == 1 and "cancel" in events and
              "end" not in events and "enqueueCommit" not in events and
              not session.gestureActive,
              "and rolls the whole bracket back, the good parent too (%r)"
              % (events,))
        final = session.statuses[-1] if session.statuses else ""
        check("rolled back" in final and "T3" in final and
              "nothing changed" in final,
              "the status names the parent and says nothing changed (%r)"
              % (final,))
    finally:
        if realSplit is None:
            del dll.Pomade_SubdivideTube
        else:
            dll.Pomade_SubdivideTube = realSplit


def testHierarchyLevels(mods):
    print("-- HierarchyLoop: level display --------------------------")
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    state.activeLevel = 2
    dll.reset()
    check(loop.setSolo(2), "Solo reaches the model")
    calls = [c[1:] for c in dll.argsOf("Pomade_SetLevelDisplay")]
    check(calls and all(len(c) == 3 for c in calls),
          "through Pomade_SetLevelDisplay (%r)" % (calls,))
    check(any(level == 2 and visible for level, visible, _x in calls),
          "the soloed level stays visible (%r)" % (calls,))

    dll.reset()
    loop.setShowMaxLevel(1)
    calls = [c[1:] for c in dll.argsOf("Pomade_SetLevelDisplay")]
    check(calls, "Show <= n reaches the model too (%r)" % (calls,))

    dll.reset()
    loop.setLevelXray(1, True)
    calls = [c[1:] for c in dll.argsOf("Pomade_SetLevelDisplay")]
    check(any(level == 1 and xray for level, _v, xray in calls),
          "x-ray on L1 reaches the model (%r)" % (calls,))


def testHierarchySubModeActions(mods):
    """MD-04: Merge, Group and Levels act; Navigate/Subdivide select."""
    print("-- HierarchyLoop: sub-modes that act (MD-04) -------------")
    pomadeLib = mods["pomadeLib"]
    from usdGenPomadeTools import pomadeHierarchy, pomadeLoopsFill
    TUBE = pomadeLib.POMADE_PICK_TUBE_VERT
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    dll.children = {0: [1, 2]}
    dll.parents = {1: 0, 2: 0}
    dll.levels = {0: 1, 1: 2, 2: 2}

    def over(tubeId):
        session.pickFn = (lambda mask, x, y:
                          {"kind": TUBE, "id": tubeId, "subId": -1,
                           "subSubId": -1} if mask & TUBE else None)

    def click(x=100.0, y=100.0, modifiers=frozenset()):
        loop.press(sample(x, y, modifiers))
        loop.release(sample(x, y, modifiers))

    def merges():
        return [int(args[-1]) for args in dll.argsOf("Pomade_MergeChildren")]

    # Navigate and Subdivide (k-means): a click on a parent only selects.
    for sub in ("navigate", "subdivide"):
        loop.setSubMode(sub)
        state.splitMode = "kmeans"
        dll.reset()
        session.events = []
        over(0)
        click()
        check(session.selection.get(TUBE) == [0] and not merges() and
              not dll.argsOf("Pomade_SetLevelDisplay") and
              not session.events,
              "%s: a click on a parent selects it and does nothing else "
              "(%r)" % (sub, session.events))

    # Merge: a plain click on a parent folds its children, as Shift+M.
    loop.setSubMode("merge")
    dll.reset()
    session.events = []
    session.statuses = []
    over(0)
    click()
    check(merges() == [0],
          "merge: clicking a parent merges its children (%r)" % merges())
    events = [name for name, _a in session.events]
    check(events.count("begin") == 1 and events.count("end") == 1,
          "as one undo step (%r)" % (events,))
    check(session.statuses and session.statuses[-1] ==
          pomadeHierarchy.mergeChildrenStatus("1 tube(s)"),
          "with Shift+M's status (%r)" % session.statuses[-1:])
    # A visible child folds its siblings back into their parent.
    dll.reset()
    over(1)
    click()
    check(merges() == [0],
          "merge: clicking a child merges its parent's children (%r)"
          % merges())
    # A press that travels off the tube is a cancelled click.
    dll.reset()
    over(0)
    loop.press(sample(100.0, 100.0))
    loop.release(sample(130.0, 100.0))
    check(not merges(), "merge: a press dragged off the tube merges nothing")
    # A modifier click only edits the selection.
    dll.reset()
    click(modifiers=frozenset(["shift"]))
    check(not merges(), "merge: a Shift-click only toggles the selection")
    # Escape between press and release drops the pending click.
    dll.reset()
    loop.press(sample(100.0, 100.0))
    loop.cancel()
    loop.release(sample(100.0, 100.0))
    check(not merges(), "merge: Escape mid-click merges nothing")

    # Levels: a click solos the clicked tube's level; again un-solos.
    loop.setSubMode("levels")
    dll.reset()
    session.events = []
    over(1)
    click()
    check(state.soloLevel == 2,
          "levels: clicking an L2 tube solos L2 (%d)" % state.soloLevel)
    calls = [c[1:] for c in dll.argsOf("Pomade_SetLevelDisplay")]
    check(any(level == 1 and not visible for level, visible, _x in calls) and
          any(level == 2 and visible for level, visible, _x in calls),
          "and the model shows L2 alone (%r)" % (calls,))
    check("solo L2" in session.statuses[-1],
          "the status says so (%r)" % session.statuses[-1:])
    check(not session.events, "soloing opens no undo step")
    over(2)
    click()
    check(state.soloLevel == pomadeHierarchy.SOLO_OFF,
          "levels: a second L2 click un-solos (%d)" % state.soloLevel)
    over(0)
    click()
    check(state.soloLevel == 1, "an L1 click solos L1 (%d)" % state.soloLevel)
    over(1)
    click()
    check(state.soloLevel == 2,
          "and an L2 click moves the solo to L2 (%d)" % state.soloLevel)
    loop.setSolo(pomadeHierarchy.SOLO_OFF)

    # Group: a plain band groups what it boxed.
    loop.setSubMode("group")
    band = []

    def selectRect(camera, x0, y0, x1, y1, kindMask, mode):
        session.rects.append((x0, y0, x1, y1, int(kindMask), int(mode)))
        session.selection[TUBE] = list(band)
        return True
    session.selectRect = selectRect
    session.pickFn = lambda mask, x, y: None

    def drag(modifiers=frozenset()):
        loop.press(sample(50.0, 50.0, modifiers))
        loop.move(sample(200.0, 200.0, modifiers))
        loop.release(sample(250.0, 250.0, modifiers))

    band[:] = [1, 2]
    dll.reset()
    session.events = []
    drag()
    groups = dll.argsOf("Pomade_GroupTubes")
    check(len(groups) == 1 and list(groups[0][1][:groups[0][2]]) == [1, 2],
          "group: a plain band groups the tubes it boxed (%r)"
          % ([list(g[1][:g[2]]) for g in groups],))
    events = [name for name, _a in session.events]
    check(events.count("begin") == 1 and events.count("end") == 1,
          "as one undo step (%r)" % (events,))
    dll.reset()
    drag(frozenset(["shift"]))
    check(dll.count("Pomade_GroupTubes") == 0,
          "group: a Shift band only adds to the selection")
    band[:] = [1]
    dll.reset()
    drag()
    check(dll.count("Pomade_GroupTubes") == 0 and
          "two or more" in session.statuses[-1],
          "group: a band over one tube says why it did not group (%r)"
          % session.statuses[-1:])
    dll.reset()
    click(50.0, 50.0)
    check(dll.count("Pomade_GroupTubes") == 0,
          "group: an empty click without travel groups nothing")

    # [ / ] change Hierarchy's own pick radius, never Graph's weld snap.
    state.snapRadiusPx = 8.0
    state.pickRadiusPx = 8.0
    loop.adjustRadius(4.0)
    check(near(state.pickRadiusPx, 12.0) and near(state.snapRadiusPx, 8.0),
          "] widens pickRadiusPx and leaves snapRadiusPx (%g, %g)"
          % (state.pickRadiusPx, state.snapRadiusPx))
    loop.setSubMode("navigate")
    session.picks = []
    over(0)
    click()
    check(session.picks and near(session.picks[-1][3], 12.0),
          "Hierarchy picks with pickRadiusPx (%r)" % (session.picks[-1:],))
    state.snapRadiusPx = 40.0
    fill = pomadeLoopsFill.FillLoop(session, state)
    check(near(fill.pickRadiusPx(), 12.0),
          "Fill picks with pickRadiusPx, not the snap radius (%g)"
          % fill.pickRadiusPx())


def testHierarchyActiveCut(mods):
    print("-- HierarchyLoop: per-branch active cut ------------------")
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newHierarchy(mods)
    # Two unrelated roots, each with its own L2 frontier.  This is the
    # regression global activeLevel could never express.
    dll.children = {0: [1, 2], 1: [3], 10: [11, 12]}
    dll.parents = {1: 0, 2: 0, 3: 1, 11: 10, 12: 10}
    dll.levels = {0: 1, 10: 1, 1: 2, 2: 2, 3: 3, 11: 2, 12: 2}
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [1],
                         pomadeLib.POMADE_PICK_CENTER_CV: [1]}

    check(loop.activate() and dll.activeCutEnabled,
          "activating Hierarchy opts into the native active cut")
    check(session.selection.get(pomadeLib.POMADE_PICK_TUBE_VERT) == [] and
          pomadeLib.POMADE_PICK_CENTER_CV not in session.selection and
          session.hovers[-1:] == [(0, -1)],
          "activation drops hidden component owners and hover")
    check(dll._isTubeVisible(session.model, 0) == 1 and
          dll._isTubeVisible(session.model, 1) == 0 and
          dll._isTubeVisible(session.model, 10) == 1,
          "an empty cut starts at every root frontier")

    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [0, 10]}
    check(loop.enterLevel(), "Enter expands every selected root branch")
    check(dll.expanded == {0, 10} and
          session.selection[pomadeLib.POMADE_PICK_TUBE_VERT] == [1, 2, 11, 12],
          "Enter selects direct children without hiding the other branch")
    check(dll._isTubeVisible(session.model, 0) == 0 and
          dll._isTubeVisible(session.model, 1) == 1 and
          dll._isTubeVisible(session.model, 10) == 0 and
          dll._isTubeVisible(session.model, 11) == 1,
          "each expanded parent is replaced only by its own children")
    session.selection = {pomadeLib.POMADE_PICK_CENTER_CV: [1]}
    check(loop.activate() and
          session.selection.get(pomadeLib.POMADE_PICK_TUBE_VERT) == [1] and
          pomadeLib.POMADE_PICK_CENTER_CV not in session.selection,
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
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [1, 3],
                         pomadeLib.POMADE_PICK_CENTER_CV: [3]}
    check(loop.exitLevel(), "Exit collapses the parent of each selected child")
    check(dll.expanded == {10} and
          session.selection[pomadeLib.POMADE_PICK_TUBE_VERT] == [0] and
          pomadeLib.POMADE_PICK_CENTER_CV not in session.selection and
          dll._isTubeVisible(session.model, 0) == 1 and
          dll._isTubeVisible(session.model, 1) == 0 and
          dll._isTubeVisible(session.model, 10) == 0 and
          dll._isTubeVisible(session.model, 11) == 1,
          "Exit collapses only the shallowest requested branch")

    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [11]}
    check(loop.exitLevel() and dll.expanded == set() and
          session.selection[pomadeLib.POMADE_PICK_TUBE_VERT] == [10] and
          dll._isTubeVisible(session.model, 10) == 1 and
          dll._isTubeVisible(session.model, 11) == 0,
          "a separate Exit restores B without reviving A descendants")
    check(loop.focusTube(0) and
          session.selection[pomadeLib.POMADE_PICK_TUBE_VERT] == [0],
          "a tube-id breadcrumb targets one branch, never a global level")


# ---------------------------------------------------------------------------
# SculptLoop
# ---------------------------------------------------------------------------

def testSculptStroke(mods):
    print("-- SculptLoop: one shaped stroke per move ----------------")
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.brushRadiusPx = 32.0
    state.brushTRadius = 0.4
    state.sculptMirrorX = True
    state.sculptPreserveLength = True
    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_CENTER_CV, "id": 5,
                       "subId": 2, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_CENTER_CV else None)

    check(loop.press(sample(200.0, 200.0)), "the press claims the event")
    check(session.gestureStack == ["Sculpt Grab"],
          "and opens one gesture bracket labelled for the undo menu (%r)"
          % (session.gestureStack,))
    check(dll.count("Pomade_SetBrushRing") >= 1,
          "the brush ring is placed on press")
    check(all(near(r, 32.0) for m, _x, _y, r in session.picks
              if m == pomadeLib.POMADE_PICK_CENTER_CV),
          "the footprint pick uses the brush radius (%r)" % (session.picks,))

    loop.move(sample(240.0, 200.0))
    strokes = dll.argsOf("Pomade_SculptStrokeShaped")
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
    check(len(dll.argsOf("Pomade_SculptStrokeShaped")) == 2,
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
          "through Pomade_CancelGesture (%r)" % (session.events,))


def testSculptBrushes(mods):
    print("-- SculptLoop: the brush scalars -------------------------")
    pomadeLib = mods["pomadeLib"]
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.sculptStrength = 1.0
    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_CENTER_CV, "id": 0,
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
    state.brushRadiusPx = 40.0
    loop.adjustRadius(1.0)
    check(near(state.brushRadiusPx, 46.0),
          "] scales the brush radius by 1.15 (%g)" % state.brushRadiusPx)
    loop.adjustRadius(-1.0)
    check(near(state.brushRadiusPx, 40.0),
          "[ divides it back (%g)" % state.brushRadiusPx)
    loop.adjustRadius(0.5)
    check(near(state.brushRadiusPx, 42.0),
          "the fine step scales by 1.05 (%g)" % state.brushRadiusPx)
    state.brushRadiusPx = 3.0
    loop.adjustRadius(1.0)
    check(near(state.brushRadiusPx, 4.0),
          "a small brush still moves by at least 1 px (%g)"
          % state.brushRadiusPx)
    state.brushRadiusPx = 2.5
    loop.adjustRadius(-1.0)
    check(near(state.brushRadiusPx, 2.0),
          "[ clamps at BRUSH_RADIUS_MIN_PX (%g)" % state.brushRadiusPx)
    state.brushRadiusPx = 500.0
    loop.adjustRadius(1.0)
    check(near(state.brushRadiusPx, 512.0),
          "] clamps at BRUSH_RADIUS_MAX_PX (%g)" % state.brushRadiusPx)
    check(mods["pomadeLoopsSculpt"].BRUSH_RADIUS_MIN_PX == 2.0 and
          mods["pomadeLoopsSculpt"].BRUSH_RADIUS_MAX_PX == 512.0,
          "the loop's radius range is pomadeSculpt's")
    check("brush radius 512 px" in session.statuses[-1],
          "each key reports the radius (%r)" % (session.statuses[-1:],))
    loop.setSubMode("smooth")
    state.sculptStrength = 2.5
    line = loop.statusLine()
    check("strength 1.00" in line and "capped" in line,
          "the Smooth status shows the strength it applies (%r)" % line)
    loop.setSubMode("grab")
    state.sculptStrength = 1.0


def testSculptRadiusResize(mods):
    print("-- SculptLoop: F-drag brush radius ----------------------")
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.brushRadiusPx = 24.0
    check(loop.beginRadiusResize(sample(100.0, 180.0)),
          "F-LMB begins a UI-only radius drag")
    check(not session.gestureStack and
          not dll.argsOf("Pomade_SculptStrokeShaped"),
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
          not dll.argsOf("Pomade_SculptStrokeShaped"),
          "the completed width change authored no stroke")
    check(loop.beginRadiusResize(sample(200.0, 100.0)) and
          loop.resizeRadius(sample(-2000.0, 100.0)) and
          near(loop.brushRadiusPx(), 2.0),
          "left F-drag clamps at the minimum radius")
    session.statuses = []
    loop.resizeRadius(sample(400.0, 100.0))
    check(session.statuses and
          "brush radius" in session.statuses[-1] and
          loop.resizingRadius,
          "every resize sample reports the live radius (%r)"
          % (session.statuses[-1:],))
    check(loop.cancelRadiusResize() and near(state.brushRadiusPx, 64.0),
          "Escape/capture cancel restores the press-time radius")


def testSculptHonesty(mods):
    print("-- SculptLoop: facing ring, misses, clicks --------------")
    pomadeLib = mods["pomadeLib"]
    sculpt = mods["pomadeLoopsSculpt"]
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.brushRadiusPx = 40.0
    oldCount = sculpt.pomadeHierarchy.tubeCenterCount
    oldPoint = sculpt.pomadeHierarchy.tubeCenterHandle
    sculpt.pomadeHierarchy.tubeCenterCount = lambda *_args: 5
    sculpt.pomadeHierarchy.tubeCenterHandle = (
        lambda *_args: (2.0, 0.5, 2.0))
    try:
        session.pickFn = (lambda mask, x, y:
                          {"kind": pomadeLib.POMADE_PICK_CENTER_CV, "id": 5,
                           "subId": 2, "subSubId": -1}
                          if mask == pomadeLib.POMADE_PICK_CENTER_CV else None)
        forward = cam.rayThrough(cam.width * 0.5, cam.height * 0.5)[1]
        for cursor in ((210.0, 205.0), (190.0, 214.0)):
            dll.reset()
            loop.hover(sample(*cursor))
            rings = dll.argsOf("Pomade_SetBrushRing")
            check(bool(rings), "a hover over a tube places the ring")
            if not rings:
                continue
            centre, normal = rings[-1][1], rings[-1][2]
            dot = sum(float(normal[k]) * forward[k] for k in range(3))
            check(abs(dot) > 0.99,
                  "the idle ring faces the camera (dot %.3f)" % dot)
            projected = cam.worldToPixels(
                (centre[0], centre[1], centre[2]))
            check(projected is not None and
                  abs(projected[0] - cursor[0]) < 1.0 and
                  abs(projected[1] - cursor[1]) < 1.0,
                  "and sits under the cursor, not on the CV (%r vs %r)"
                  % (projected, cursor))
        check(session.hovers and
              session.hovers[-1] == (pomadeLib.POMADE_PICK_CENTER_CV, 5),
              "the anchor CV is the hover prehighlight (%r)"
              % (session.hovers[-1:],))
        hoverCalls = len(session.hovers)
        loop.hover(sample(215.0, 205.0))
        check(len(session.hovers) == hoverCalls,
              "the same anchor CV is not re-hovered on every move")

        dll.reset()
        check(loop.clearHover(), "leaving the view clears an idle ring")
        rings = dll.argsOf("Pomade_SetBrushRing")
        check(rings and float(getattr(rings[-1][3], "value",
                                      rings[-1][3])) == 0.0,
              "with a zero-radius ring (%r)" % (rings[-1:],))
        check(session.hovers[-1] == (0, -1), "and drops the CV hover")

        # A no-travel click leaves no undo step.
        session.events = []
        check(loop.press(sample(200.0, 200.0)), "a press on the tube claims")
        check(loop.release(sample(200.0, 200.0)),
              "the release closes it")
        check(("cancel", None) in session.events and
              ("end", None) not in session.events and
              ("enqueueCommit", None) not in session.events,
              "a click without a drag cancels its bracket (%r)"
              % (session.events,))
        check(not session.gestureStack, "no bracket is left open")
        check("nothing changed" in session.statuses[-1],
              "and says so (%r)" % (session.statuses[-1:],))

        # An honest miss: a status, no gesture, and the ring off-target.
        session.pickFn = lambda _mask, _x, _y: None
        session.selection = {}
        session.surfaceMisses = True
        session.events = []
        check(not loop.press(sample(20.0, 20.0)),
              "a press off every tube opens nothing")
        check(not session.gestureStack and
              not any(e[0] == "begin" for e in session.events),
              "no gesture bracket on a miss")
        check("no tube under the brush" in session.statuses[-1],
              "the miss is reported (%r)" % (session.statuses[-1:],))
    finally:
        sculpt.pomadeHierarchy.tubeCenterCount = oldCount
        sculpt.pomadeHierarchy.tubeCenterHandle = oldPoint


def testSculptViewPlane(mods):
    print("-- SculptLoop: frozen view-plane stroke -----------------")
    pomadeLib = mods["pomadeLib"]
    sculpt = mods["pomadeLoopsSculpt"]
    loop, dll, session, state, cam, sample = newSculpt(mods)
    state.brushRadiusPx = 40.0
    # No geometry item exists at this press.  The existing explicit tube
    # selection is nevertheless close enough in screen space to begin a
    # sculpt gesture; no unselected/default tube may be guessed.
    session.selection = {pomadeLib.POMADE_PICK_TUBE_VERT: [9]}
    session.pickFn = lambda _mask, _x, _y: None
    oldCount = sculpt.pomadeHierarchy.tubeCenterCount
    oldPoint = sculpt.pomadeHierarchy.tubeCenterHandle
    sculpt.pomadeHierarchy.tubeCenterCount = lambda *_args: 5
    sculpt.pomadeHierarchy.tubeCenterHandle = (
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
        strokes = dll.argsOf("Pomade_SculptStrokeShaped")
        check(strokes and near(strokes[-1]["x"], 200.0) and
              near(strokes[-1]["y"], 200.0),
              "Grab keeps the projected press footprint, not the trail")
        rings = dll.argsOf("Pomade_SetBrushRing")
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
        sculpt.pomadeHierarchy.tubeCenterCount = oldCount
        sculpt.pomadeHierarchy.tubeCenterHandle = oldPoint


# ---------------------------------------------------------------------------
# The fallback ladder
# ---------------------------------------------------------------------------

def testLadder(mods):
    print("-- the fallback ladder -----------------------------------")
    pomadeLadder = mods["pomadeLadder"]
    Dll, Session = mods["fakes"]
    dll = Dll()
    session = Session(dll)
    state = mods["PomadeToolState"]()
    ladder = pomadeLadder.FallbackLadder(session, state)

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
          session.published[-1] == mods["pomadeLib"].POMADE_DIRTY_ALL,
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
    state = mods["PomadeToolState"]()
    loop = mods["pomadeLoopsHierarchy"].HierarchyLoop(session, state)
    cam = mods["base"].topDownCamera(mods["pomadeCamera"])
    Sample = mods["pomadeLoops"].Sample

    def sample(x, y, mods_=frozenset()):
        return Sample(session, cam, x, y, mods_)

    return loop, dll, session, state, cam, sample


def newSculpt(mods):
    Dll, Session = mods["fakes"]
    dll = Dll()
    session = Session(dll)
    state = mods["PomadeToolState"]()
    loop = mods["pomadeLoopsSculpt"].SculptLoop(session, state)
    cam = mods["base"].topDownCamera(mods["pomadeCamera"])
    Sample = mods["pomadeLoops"].Sample

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
    import pomadeTestPackage
    pomadeTestPackage.install()
    import testUsdGenPomadeToolsLoops as base
    from usdGenPomadeTools import (pomadeCamera, pomadeLadder, pomadeLib,
                                  pomadeLoops, pomadeLoopsHierarchy,
                                  pomadeLoopsSculpt)
    from usdGenPomadeTools.pomadeToolState import PomadeToolState
    Dll, Session = makeFakes(base)
    mods = {"base": base, "fakes": (Dll, Session), "pomadeCamera": pomadeCamera,
            "pomadeLadder": pomadeLadder, "pomadeLib": pomadeLib,
            "pomadeLoops": pomadeLoops,
            "pomadeLoopsHierarchy": pomadeLoopsHierarchy,
            "pomadeLoopsSculpt": pomadeLoopsSculpt,
            "PomadeToolState": PomadeToolState}
    check(pomadeLoops.makeLoop("hierarchy", None, None) is not None,
          "Hierarchy has a loop from V5 on")
    check(pomadeLoops.makeLoop("sculpt", None, None) is not None,
          "and so does Sculpt")
    testHierarchySelection(mods)
    testHierarchySelectionMatrix(mods)
    testHierarchyActions(mods)
    testHierarchyEdgeSplit(mods)
    testHierarchyEdgeOverlay(mods)
    testHierarchyHonestFailures(mods)
    testHierarchyResubdivide(mods)
    testHierarchyLevels(mods)
    testHierarchySubModeActions(mods)
    testHierarchyActiveCut(mods)
    testSculptStroke(mods)
    testSculptBrushes(mods)
    testSculptRadiusResize(mods)
    testSculptHonesty(mods)
    testSculptViewPlane(mods)
    testLadder(mods)
    print("testUsdGenPomadeToolsLoopsHier: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
