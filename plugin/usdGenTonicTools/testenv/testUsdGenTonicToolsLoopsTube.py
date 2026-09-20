#!/usr/bin/env python
# testUsdGenTonicToolsLoopsTube -- T0 for the V4 Tube and Fill loops
# (plan/18 section 4 "T0", section 3.6).
#
#   python plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsLoopsTube.py
#
# No usdview, no Qt, no DLL: TubeLoop and FillLoop are driven over a fake
# session that records every C call, with a camera whose arithmetic anyone
# can check by hand. What this proves that a T3 cannot:
#
#   * a press picks ONCE (K11) and a drag re-picks never: the whole gesture
#     runs off the press-time selection, camera and ring frames;
#   * one gesture is one undo bracket -- Begin at press, End at release,
#     Cancel on Escape -- and Escape puts the soft selection and the
#     preview fraction back exactly as it found them;
#   * a world drag reaches the section chart correctly: the frame's twist
#     and scale are undone in the right order, which is the whole reason
#     Tonic_GetTubeSectionFrame exists;
#   * a soft-radius drag moves ONE anchor CV per tube and lets the model
#     spread it, instead of moving every CV of the span with a falloff
#     each and counting the neighbours twice;
#   * Fill drags at the panel's preview fraction and releases at full
#     density, and never touches the artist's freeze-roots switch.
import math
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


def _deref(ref):
    """The object behind a ctypes.byref() the fake DLL was handed."""
    return getattr(ref, "_obj", ref)


def _f(v):
    """A float argument, whether it arrived raw or as a ctypes.c_float."""
    return float(getattr(v, "value", v))


def _plain(method):
    """A bound method as a PLAIN function.

    tonicHierarchy and tonicBridge set `entry.argtypes = [...]` on the
    entry they are about to call, the way they would on a real ctypes
    function. A bound method refuses attributes, so the entries those two
    modules reach have to be ordinary functions on the instance.
    """
    def entry(*args):
        return method(*args)
    return entry


# ---------------------------------------------------------------------------
# Fakes
# ---------------------------------------------------------------------------

# The fixture tube: three center CVs up the +Y axis at z = -5, so the
# camera below projects them to pixels (200, 225), (200, 150), (200, 75).
CENTERS = [(0.0, -1.0, -5.0), (0.0, 0.0, -5.0), (0.0, 1.0, -5.0)]


class FakeDll:
    """Every Tonic_* entry the two loops touch, recorded."""

    def __init__(self):
        self.calls = []
        self.centers = {0: [list(p) for p in CENTERS]}
        self.selection = {}          # kind -> [(id, subId, subSubId)]
        self.softCenter = 0.25
        self.softRadius = 0.0
        self.previewFraction = 1.0
        self.guideCount = 64
        self.gizmo = None
        self.fill = {"density": 8.0, "cvCount": 8, "seed": 3,
                     "edgeBias": 0.0, "profile": []}
        self.sectionCount = 2
        self.regionId = 0
        for name in ("Tonic_GetTubeCenterCount", "Tonic_GetTubeCenterCV",
                     "Tonic_MoveTubeCenterCV", "Tonic_GetTubeSection"):
            setattr(self, name, _plain(getattr(self, name)))

    # -- bookkeeping -------------------------------------------------------

    def _record(self, name, args=()):
        self.calls.append((name, args))

    def names(self):
        return [name for name, _args in self.calls]

    def count(self, name):
        return sum(1 for entry, _args in self.calls if entry == name)

    def argsOf(self, name):
        return [args for entry, args in self.calls if entry == name]

    def reset(self):
        self.calls = []

    # -- census ------------------------------------------------------------

    def Tonic_GetCenterCVCount(self, _model):
        return len(self.centers.get(0, []))

    def Tonic_GetSectionCount(self, _model):
        return self.sectionCount

    def Tonic_GetTubeRegionId(self, _model):
        return self.regionId

    def Tonic_GetTubeCenterCount(self, _model, tubeId):
        return len(self.centers.get(int(tubeId), []))

    def Tonic_GetTubeCenterCV(self, _model, tubeId, cv, out3):
        point = self.centers[int(tubeId)][int(cv)]
        for i in range(3):
            out3[i] = point[i]
        return 0

    def Tonic_MoveTubeCenterCV(self, _model, tubeId, cv, dx, dy, dz):
        self._record("Tonic_MoveTubeCenterCV",
                     (int(tubeId), int(cv), _f(dx), _f(dy), _f(dz)))
        point = self.centers[int(tubeId)][int(cv)]
        for i, d in enumerate((dx, dy, dz)):
            point[i] += _f(d)
        return 0

    def Tonic_GetTubeSection(self, _model, tubeId, ring, t, uv, uvLen, count,
                             scale, twist):
        self._record("Tonic_GetTubeSection", (int(tubeId), int(ring)))
        _deref(count).value = 8
        if t is not None:
            _deref(t).value = 0.5 * int(ring)
            _deref(scale).value = 1.0
            _deref(twist).value = 0.0
            for i in range(min(uvLen, 16)):
                uv[i] = 0.5
        return 0

    # -- selection ---------------------------------------------------------

    def _items(self, kind):
        return self.selection.get(int(kind), [])

    def Tonic_ReadSelection(self, _model, kind, ids, subIds, subSubIds, cap,
                            count):
        items = self._items(getattr(kind, "value", kind))
        _deref(count).value = len(items)
        if ids is not None:
            for i, item in enumerate(items[:cap]):
                ids[i] = item[0]
                if subIds is not None:
                    subIds[i] = item[1]
                if subSubIds is not None:
                    subSubIds[i] = item[2]
        return 0

    def Tonic_GetSelectionBounds(self, _model, lo, hi):
        points = []
        for tubeId, _s, _ss in self._items(1):          # TubeVert
            points.extend(self.centers.get(tubeId, []))
        for tubeId, cv, _ss in self._items(1 << 1):     # CenterCV
            points.append(self.centers[tubeId][cv])
        for tubeId, _ring, _ss in self._items(1 << 7):  # SectionRing
            points.append(self.centers[tubeId][1])
        for tubeId, _ring, _slot in self._items(1 << 2):  # SectionCV
            points.append(self.centers[tubeId][1])
        if not points:
            return 1
        for a in range(3):
            lo[a] = min(p[a] for p in points)
            hi[a] = max(p[a] for p in points)
        return 0

    # -- soft selection, preview, guides -----------------------------------

    def Tonic_GetSoftSelection(self, _model, center, radius):
        _deref(center).value = self.softCenter
        _deref(radius).value = self.softRadius
        return 0

    def Tonic_SetSoftSelection(self, _model, center, radius):
        self._record("Tonic_SetSoftSelection", (_f(center), _f(radius)))
        self.softCenter = _f(center)
        self.softRadius = _f(radius)
        return 0

    def Tonic_GetPreviewFraction(self, _model):
        return self.previewFraction

    def Tonic_SetPreviewFraction(self, _model, fraction):
        self._record("Tonic_SetPreviewFraction", (_f(fraction),))
        self.previewFraction = _f(fraction)
        return 0

    def Tonic_GetGuideCounts(self, _model, guides, cvs):
        if guides is not None:
            _deref(guides).value = self.guideCount
        if cvs is not None:
            _deref(cvs).value = 8
        return 0

    def Tonic_RefillGuides(self, _model, fraction):
        self._record("Tonic_RefillGuides", (_f(fraction),))
        return 0

    # -- fill params -------------------------------------------------------

    def Tonic_GetFillParams(self, _model, density, cvCount, seed, edgeBias,
                            profile, maxFloats, got):
        _deref(density).value = self.fill["density"]
        _deref(cvCount).value = self.fill["cvCount"]
        _deref(seed).value = self.fill["seed"]
        _deref(edgeBias).value = self.fill["edgeBias"]
        values = self.fill["profile"]
        _deref(got).value = len(values)
        for i, value in enumerate(values[:maxFloats]):
            profile[i] = value
        return 0

    def Tonic_SetFillParams(self, _model, density, cvCount, seed, edgeBias,
                            profile, count):
        pairs = [float(profile[i]) for i in range(count)] if profile else []
        self._record("Tonic_SetFillParams",
                     (_f(density), int(cvCount), int(seed), _f(edgeBias),
                      pairs))
        self.fill = {"density": _f(density), "cvCount": int(cvCount),
                     "seed": int(seed), "edgeBias": _f(edgeBias),
                     "profile": pairs}
        return 0

    # -- overlays ----------------------------------------------------------

    def Tonic_SetGizmo(self, _model, kind, origin, frame, size, handle):
        self.gizmo = {"kind": int(kind),
                      "origin": tuple(origin[i] for i in range(3)),
                      "frame": tuple(frame[i] for i in range(9)),
                      "size": _f(size), "handle": int(handle)}
        self._record("Tonic_SetGizmo", (int(kind), int(handle)))
        return 0

    # -- everything else ---------------------------------------------------

    def __getattr__(self, name):
        if name.startswith("_") or not name.startswith("Tonic_"):
            raise AttributeError(name)

        def entry(*args):
            self._record(name, args)
            return 0
        return entry


class FakeStage:
    """The tonicLibStage.StageLibrary surface, recorded.

    worldToChart is the REAL method: the conversion from a world delta to
    the (du, dv) the ABI takes is the thing under test, so a fake of it
    would prove nothing.
    """

    def __init__(self, frames=None):
        self.calls = []
        self.frames = frames or {}
        self.fill = {}

    def _record(self, name, args):
        self.calls.append((name,) + tuple(args))

    def names(self):
        return [entry[0] for entry in self.calls]

    def argsOf(self, name):
        return [entry[1:] for entry in self.calls if entry[0] == name]

    def sectionFrame(self, _ctx, tubeId, ring):
        return self.frames.get((int(tubeId), int(ring)))

    def worldToChart(self, ring, delta):
        """The REAL conversion: it is the thing under test."""
        from usdGenTonicTools.tonicLibStage import StageLibrary
        return StageLibrary.worldToChart(self, ring, delta)

    def moveSectionRing(self, _ctx, tubeId, ring, du, dv):
        self._record("moveSectionRing", (int(tubeId), int(ring), du, dv))

    def scaleSectionRing(self, _ctx, tubeId, ring, scale):
        self._record("scaleSectionRing", (int(tubeId), int(ring), scale))

    def twistSectionRing(self, _ctx, tubeId, ring, radians):
        self._record("twistSectionRing", (int(tubeId), int(ring), radians))

    def moveSectionCV(self, _ctx, tubeId, ring, slot, du, dv):
        self._record("moveSectionCV",
                     (int(tubeId), int(ring), int(slot), du, dv))

    def deleteCenterCV(self, _ctx, tubeId, index):
        self._record("deleteCenterCV", (int(tubeId), int(index)))

    def removeSectionRing(self, _ctx, tubeId, ring):
        self._record("removeSectionRing", (int(tubeId), int(ring)))

    def setFillParams(self, _ctx, tubeId, density, cvCount, seed, edgeBias,
                      lengthProfile=()):
        self._record("setFillParams",
                     (int(tubeId), float(density), int(cvCount), int(seed),
                      float(edgeBias), list(lengthProfile)))
        self.fill[int(tubeId)] = {"density": float(density),
                                  "cvCount": int(cvCount), "seed": int(seed),
                                  "edgeBias": float(edgeBias),
                                  "profileCount": len(lengthProfile)}

    def fillParams(self, _ctx, tubeId):
        return self.fill.get(int(tubeId),
                             {"density": 8.0, "cvCount": 8, "seed": 3,
                              "edgeBias": 0.0, "profileCount": 0})


class FakeSession:
    """The TonicSession surface the loops use, recorded."""

    def __init__(self, dll, stage=None):
        self.dll = dll
        self.stageLib = stage
        self.model = "model"
        self.ctx = self.model
        self.events = []
        self.statuses = []
        self.picks = []
        self.hovers = []
        self.rects = []
        self.published = []
        self.gestureStack = []
        self.pickFn = lambda mask, x, y: None

    # -- status ------------------------------------------------------------

    def report(self, text):
        self.statuses.append(text)

    def lastError(self):
        return "fake error"

    # -- gestures ----------------------------------------------------------

    def beginGesture(self, label):
        self.events.append(("begin", label))
        self.gestureStack.append(label)
        return True

    def endGesture(self):
        self.events.append(("end", None))
        if self.gestureStack:
            self.gestureStack.pop()
        return True

    def cancelGesture(self):
        self.events.append(("cancel", None))
        if self.gestureStack:
            self.gestureStack.pop()
        return 1

    @property
    def gestureActive(self):
        return bool(self.gestureStack)

    # -- publish / commit --------------------------------------------------

    def publish(self, dirtyMask=0):
        self.published.append(int(dirtyMask))
        return 1

    def enqueueCommit(self):
        self.events.append(("enqueueCommit", None))
        return True

    def rebake(self):
        self.events.append(("rebake", None))
        return True

    # -- picking and selection ---------------------------------------------

    def pickItem(self, camera, x, y, radiusPx, kindMask):
        self.picks.append((int(kindMask), float(x), float(y)))
        return self.pickFn(int(kindMask), float(x), float(y))

    def setHover(self, kind=0, ident=-1, subId=-1, subSubId=-1):
        self.hovers.append((int(kind), int(ident)))
        return True

    def selectRect(self, camera, x0, y0, x1, y1, kindMask, mode):
        self.rects.append((x0, y0, x1, y1, int(kindMask), int(mode)))
        return True

    def select(self, kind, ids, subIds=None, subSubIds=None, mode=0):
        rows = [(int(ids[i]),
                 int(subIds[i]) if subIds else -1,
                 int(subSubIds[i]) if subSubIds else -1)
                for i in range(len(ids))]
        store = self.dll.selection
        if mode == 0:            # SET
            store[int(kind)] = rows
        elif mode == 1:          # ADD
            existing = store.setdefault(int(kind), [])
            for row in rows:
                if row not in existing:
                    existing.append(row)
        else:                    # TOGGLE
            existing = store.setdefault(int(kind), [])
            for row in rows:
                if row in existing:
                    existing.remove(row)
                else:
                    existing.append(row)
        return True

    def clearSelection(self, kindMask=0):
        for kind in list(self.dll.selection):
            if not kindMask or (kind & kindMask):
                self.dll.selection.pop(kind)
        return True

    def readSelection(self, kind):
        return list(self.dll.selection.get(int(kind), []))

    def selectionCount(self, kindMask=0):
        return sum(len(v) for k, v in self.dll.selection.items()
                   if not kindMask or (k & kindMask))


# ---------------------------------------------------------------------------
# The camera: 400 x 300 pixels over x, y in [-2, 2], looking down -Z.
# One world unit is 100 px across and 75 px down; one pixel is 0.01 across.
# ---------------------------------------------------------------------------

def orthoCamera(tonicCamera):
    a = -2.0 / 9.0
    b = -11.0 / 9.0
    viewProj = (0.5, 0.0, 0.0, 0.0,
                0.0, 0.5, 0.0, 0.0,
                0.0, 0.0, a, 0.0,
                0.0, 0.0, b, 1.0)
    return tonicCamera.TonicCamera(viewProj, 400, 300)


def newTube(mods, subMode="center", stage=None):
    dll = FakeDll()
    session = FakeSession(dll, stage)
    state = mods["TonicToolState"]()
    state.snapRadiusPx = 8.0
    loop = mods["tonicLoopsTube"].TubeLoop(session, state)
    state.tubeSubMode = subMode
    return dll, session, state, loop


def newFill(mods, subMode="params", stage=None):
    dll = FakeDll()
    session = FakeSession(dll, stage)
    state = mods["TonicToolState"]()
    state.snapRadiusPx = 8.0
    loop = mods["tonicLoopsFill"].FillLoop(session, state)
    state.fillSubMode = subMode
    return dll, session, state, loop


def sample(mods, session, camera, x, y, modifiers=()):
    return mods["tonicLoops"].Sample(session, camera, x, y,
                                     frozenset(modifiers))


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def testShelf(mods):
    print("-- Tube and Fill are registered --------------------------")
    tonicLoops = mods["tonicLoops"]
    check(tonicLoops.loopClass("tube") is mods["tonicLoopsTube"].TubeLoop,
          "Tube mode's loop is TubeLoop")
    check(tonicLoops.loopClass("fill") is mods["tonicLoopsFill"].FillLoop,
          "Fill mode's loop is FillLoop")
    check("tube" not in tonicLoops.PANEL_ONLY_MODES and
          "fill" not in tonicLoops.PANEL_ONLY_MODES,
          "and neither is a panel-only mode")
    check(len(tonicLoops.subModesFor("tube")) == 3 and
          len(tonicLoops.subModesFor("fill")) == 2,
          "their sub-mode shelves come from tonicModes")
    dll, _session, state, loop = newTube(mods)
    check(loop.subModeForHotkey("R") == "ring" and
          loop.subModeForHotkey("E") == "section",
          "the R and E hotkeys reach the Ring and Section sub-modes")
    check(loop.pickMask == (mods["tonicLib"].TONIC_PICK_CENTER_CV |
                            mods["tonicLib"].TONIC_PICK_TUBE_VERT),
          "Center mode asks K11 for center CVs and the tube surface")
    state.tubeSubMode = "ring"
    check(loop.pickMask & mods["tonicLib"].TONIC_PICK_SECTION_RING,
          "Ring mode asks for rings")
    state.tubeSubMode = "section"
    check(loop.pickMask & mods["tonicLib"].TONIC_PICK_SECTION_CV,
          "Section mode asks for section CVs")
    check(dll.count("Tonic_MoveTubeCenterCV") == 0,
          "and none of that touched the model")


def testSelectAndGizmo(mods):
    print("-- Center: a click selects and raises the gizmo ----------")
    tonicLib = mods["tonicLib"]
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(len(session.picks) == 1,
          "the press picked once (%d)" % len(session.picks))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "center CV 1 of tube 0 is selected (%r)"
          % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))
    check(dll.gizmo is not None and
          dll.gizmo["kind"] == tonicLib.TONIC_GIZMO_TRANSLATE,
          "a translate gizmo went to the model (%r)" % (dll.gizmo,))
    check(near(dll.gizmo["origin"][1], 0.0) and
          near(dll.gizmo["origin"][2], -5.0),
          "at the selection bounds (%r)" % (dll.gizmo["origin"],))
    gizmoPx = mods["tonicLoopsTube"].GIZMO_PIXELS
    check(near(dll.gizmo["size"], 0.01 * gizmoPx),
          "sized to %g px at this zoom (%r)"
          % (gizmoPx, dll.gizmo["size"]))
    check(session.published and session.published[-1] ==
          tonicLib.TONIC_DIRTY_SELECTION,
          "and only the selection locator was republished (%r)"
          % (session.published,))
    check(not session.gestureStack,
          "selecting is not an undoable gesture (%r)" % session.gestureStack)

    # Shift adds, Ctrl toggles.
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 2,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 75.0, ("shift",)))
    check(len(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)) == 2,
          "Shift adds the second CV")
    # Away from the gizmo's axes: within their tolerance Ctrl means the
    # normal constraint, not a toggle.
    loop.press(sample(mods, session, cam, 260.0, 75.0, ("ctrl",)))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "Ctrl toggles it back off (%r)"
          % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))

    # A miss is a marquee, not a deselect-and-drag.
    session.pickFn = lambda mask, x, y: None
    loop.press(sample(mods, session, cam, 380.0, 40.0))
    loop.move(sample(mods, session, cam, 300.0, 100.0))
    loop.release(sample(mods, session, cam, 300.0, 100.0))
    check(session.rects and session.rects[-1][:4] == (380.0, 40.0, 300.0,
                                                      100.0),
          "a press on nothing rubber-bands (%r)" % (session.rects[-1:],))
    check(session.rects[-1][4] == loop.pickMask,
          "over the sub-mode's kinds (%r)" % (session.rects[-1],))


def testCenterDrag(mods):
    print("-- Center: the gizmo drag ---------------------------------")
    tonicLib = mods["tonicLib"]
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    dll.reset()
    session.events = []

    # The gizmo sits at pixel (200, 150); pressing there takes the free
    # (screen-plane) handle.
    claimed = loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(claimed and session.gestureStack == ["Tube center"],
          "the press on the handle opened one bracket (%r)"
          % (session.gestureStack,))
    check(len(session.picks) == 1,
          "and did NOT pick again (%d picks in the whole gesture)"
          % len(session.picks))
    check(dll.argsOf("Tonic_SetPreviewFraction") == [(0.25,)],
          "the drag dropped the guides to the preview fraction (%r)"
          % (dll.argsOf("Tonic_SetPreviewFraction"),))

    loop.move(sample(mods, session, cam, 250.0, 150.0))
    loop.move(sample(mods, session, cam, 300.0, 150.0))
    steps = dll.argsOf("Tonic_MoveTubeCenterCV")
    check(len(steps) == 2, "two moves, two ABI calls (%r)" % (steps,))
    check(steps[0][:2] == (0, 1) and near(steps[0][2], 0.5),
          "the first move is 50 px = 0.5 world units (%r)" % (steps[0],))
    check(near(steps[1][2], 0.5),
          "the second is the INCREMENT, not the whole travel (%r)"
          % (steps[1],))
    check(near(dll.centers[0][1][0], 1.0),
          "so the CV ends where the cursor is (%r)" % (dll.centers[0][1],))
    check(dll.argsOf("Tonic_RefillGuides") == [(0.25,), (0.25,)],
          "each move refilled at preview density (%r)"
          % (dll.argsOf("Tonic_RefillGuides"),))
    check(near(dll.gizmo["origin"][0], 1.0),
          "the gizmo followed the drag (%r)" % (dll.gizmo["origin"],))

    loop.release(sample(mods, session, cam, 300.0, 150.0))
    check(session.gestureStack == [], "the release sealed the bracket")
    check(dll.argsOf("Tonic_SetPreviewFraction")[-1] == (1.0,),
          "the stored preview fraction came back (%r)"
          % (dll.argsOf("Tonic_SetPreviewFraction"),))
    check(dll.argsOf("Tonic_RefillGuides")[-1] == (1.0,),
          "and the guides refilled at full density (%r)"
          % (dll.argsOf("Tonic_RefillGuides")[-1],))
    check(("enqueueCommit", None) in session.events,
          "the release enqueued the commit (%r)" % (session.events,))
    order = [name for name in dll.names()
             if name in ("Tonic_MoveTubeCenterCV", "Tonic_RefillGuides",
                         "Tonic_SetPreviewFraction")]
    check(order[0] == "Tonic_SetPreviewFraction" and
          order[-1] == "Tonic_RefillGuides",
          "preview first, full refill last (%r)" % (order,))


def testConstraintsAndEscape(mods):
    print("-- Center: axis, normal and Escape ------------------------")
    tonicLib = mods["tonicLib"]
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))

    # The u axis runs right from the centre, GIZMO_PIXELS of it.
    dll.reset()
    loop.press(sample(mods, session, cam, 240.0, 150.0))
    loop.move(sample(mods, session, cam, 290.0, 100.0))
    step = dll.argsOf("Tonic_MoveTubeCenterCV")[-1]
    check(near(step[2], 0.5) and near(step[3], 0.0) and near(step[4], 0.0),
          "an axis drag keeps only the along-axis travel (%r)" % (step,))
    loop.release(sample(mods, session, cam, 290.0, 100.0))

    # Ctrl on the free handle pins the drag to the tube's root normal,
    # which for this fixture is +Y. A fresh tube, because the drag above
    # left the one before it bent (and its root normal with it).
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    dll.reset()
    loop.press(sample(mods, session, cam, 200.0, 150.0, ("ctrl",)))
    loop.move(sample(mods, session, cam, 300.0, 50.0))
    step = dll.argsOf("Tonic_MoveTubeCenterCV")[-1]
    check(near(step[2], 0.0) and step[3] > 0.5,
          "Ctrl constrains the drag to the root normal (%r)" % (step,))
    loop.release(sample(mods, session, cam, 300.0, 50.0))

    # Escape restores the model through the bracket and puts the soft
    # selection and the preview fraction back.
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    dll.reset()
    session.events = []
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 260.0, 150.0))
    check(loop.cancel(), "Escape claimed the live drag")
    check(("cancel", None) in session.events and not session.gestureStack,
          "it cancelled the bracket instead of sealing it (%r)"
          % (session.events,))
    check(("enqueueCommit", None) not in session.events,
          "a cancelled drag commits nothing (%r)" % (session.events,))
    check(near(dll.previewFraction, 1.0),
          "the preview fraction is back (%r)" % dll.previewFraction)
    check(near(dll.softRadius, 0.0) and near(dll.softCenter, 0.25),
          "and so is the panel's soft selection (%r, %r)"
          % (dll.softCenter, dll.softRadius))
    check(not loop.cancel(), "a second Escape is not ours to claim")


def testSoftSpan(mods):
    print("-- Center: the soft span ----------------------------------")
    tonicLib = mods["tonicLib"]
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    state.softRadius = 0.6
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    selected = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
    check(sorted(cv for _t, cv, _s in selected) == [0, 1, 2],
          "the span the falloff reaches shows as selected CVs (%r)"
          % (selected,))

    dll.reset()
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    pushed = dll.argsOf("Tonic_SetSoftSelection")[0]
    check(near(pushed[0], 0.5) and near(pushed[1], 0.6, 1e-6),
          "the drag told the model the anchor's t and the panel radius "
          "(%r)" % (pushed,))
    loop.move(sample(mods, session, cam, 250.0, 150.0))
    moved = dll.argsOf("Tonic_MoveTubeCenterCV")
    check(len(moved) == 1 and moved[0][1] == 1,
          "a soft drag moves ONE anchor and lets the model spread it "
          "(%r)" % (moved,))
    loop.release(sample(mods, session, cam, 250.0, 150.0))
    check(near(dll.softRadius, 0.0) and near(dll.softCenter, 0.25),
          "and the panel's own soft selection is restored after (%r, %r)"
          % (dll.softCenter, dll.softRadius))

    # With no radius every selected CV moves by the same delta. A fresh
    # tube: the soft drag above left this one bent, and with it the
    # bounds the gizmo sits on.
    dll, session, state, loop = newTube(mods)
    state.softRadius = 0.0
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 0,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 225.0))
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 2,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 75.0, ("shift",)))
    dll.reset()
    # The two CVs bracket the origin, so their gizmo is back at (200, 150).
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 250.0, 150.0))
    moved = dll.argsOf("Tonic_MoveTubeCenterCV")
    check(sorted(m[1] for m in moved) == [0, 2],
          "a hard drag moves every selected CV (%r)" % (moved,))
    check(all(near(m[2], 0.5) for m in moved),
          "each by the same delta (%r)" % (moved,))
    loop.release(sample(mods, session, cam, 250.0, 150.0))


def testWholeTubeDrag(mods):
    print("-- Center: dragging a whole tube --------------------------")
    tonicLib = mods["tonicLib"]
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    state.softRadius = 0.5
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_TUBE_VERT, "id": 0, "subId": -1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) ==
          [(0, -1, -1)], "clicking the surface selects the tube")
    dll.reset()
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(dll.argsOf("Tonic_SetSoftSelection")[0] == (0.0, 0.0),
          "a whole-tube drag turns the falloff OFF: translating a curve "
          "must not bend it (%r)" % (dll.argsOf("Tonic_SetSoftSelection"),))
    loop.move(sample(mods, session, cam, 250.0, 150.0))
    moved = dll.argsOf("Tonic_MoveTubeCenterCV")
    check(sorted(m[1] for m in moved) == [0, 1, 2],
          "every center CV of the tube moved (%r)" % (moved,))
    check(all(near(m[2], 0.5) for m in moved),
          "by the same delta (%r)" % (moved,))
    loop.release(sample(mods, session, cam, 250.0, 150.0))


def testRingPlane(mods):
    print("-- Ring: the chart conversion -----------------------------")
    tonicLib = mods["tonicLib"]
    # A ring whose plane is x/z, scaled by 2 and twisted a quarter turn:
    # a world step along +x must come out as (du, dv) = (0, -0.5).
    frame = {"origin": (0.0, 0.0, -5.0), "u": (1.0, 0.0, 0.0),
             "v": (0.0, 0.0, 1.0), "w": (0.0, 1.0, 0.0), "scale": 2.0,
             "twist": math.pi / 2.0}
    stage = FakeStage({(0, 1): frame})
    dll, session, state, loop = newTube(mods, "ring", stage)
    cam = orthoCamera(mods["tonicCamera"])
    du, dv = stage.worldToChart(frame, (1.0, 0.0, 0.0))
    check(near(du, 0.0) and near(dv, -0.5),
          "worldToChart undoes the twist and the scale (%r, %r)" % (du, dv))
    check(stage.worldToChart(frame, (0.0, 1.0, 0.0)) == (0.0, 0.0),
          "and drops the tangent component: a chart is two dimensional")

    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_SECTION_RING, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(dll.gizmo["kind"] == tonicLib.TONIC_GIZMO_RING_TRS,
          "Ring mode raises the ringTRS gizmo (%r)" % (dll.gizmo,))
    check(near(dll.gizmo["frame"][0], 1.0) and near(dll.gizmo["frame"][5],
                                                    1.0),
          "oriented by the ring's own frame, not the screen (%r)"
          % (dll.gizmo["frame"],))

    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 300.0, 150.0))
    moved = stage.argsOf("moveSectionRing")
    check(len(moved) == 1 and moved[0][0] == 0 and moved[0][1] == 1,
          "the drag moved the selected ring through the per-tube ABI (%r)"
          % (moved,))
    check(near(moved[0][2], 0.0) and near(moved[0][3], -0.5),
          "with the world delta resolved into the ring plane (%r)"
          % (moved[0],))
    loop.release(sample(mods, session, cam, 300.0, 150.0))
    check(not session.gestureStack, "and the bracket closed")


def testRingScaleTwist(mods):
    print("-- Ring: scale and twist ----------------------------------")
    tonicLib = mods["tonicLib"]
    frame = {"origin": (0.0, 0.0, -5.0), "u": (1.0, 0.0, 0.0),
             "v": (0.0, 0.0, 1.0), "w": (0.0, 1.0, 0.0), "scale": 1.0,
             "twist": 0.0}
    stage = FakeStage({(0, 1): frame})
    dll, session, state, loop = newTube(mods, "ring", stage)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_SECTION_RING, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))

    # The scale ring sits at RING_FRACTION of the gizmo's pixel length;
    # press it off-axis so no axis handle claims the pixel first.
    offset = (mods["tonicGizmo"].RING_FRACTION *
              mods["tonicLoopsTube"].GIZMO_PIXELS) / math.sqrt(2.0)
    loop.press(sample(mods, session, cam, 200.0 + offset, 150.0 + offset))
    loop.move(sample(mods, session, cam, 200.0 + 2 * offset,
                     150.0 + 2 * offset))
    scaled = stage.argsOf("scaleSectionRing")
    check(len(scaled) == 1 and near(scaled[0][2], 2.0),
          "dragging the circle handle out doubles the ring (%r)"
          % (scaled,))
    loop.move(sample(mods, session, cam, 200.0 + 3 * offset,
                     150.0 + 3 * offset))
    scaled = stage.argsOf("scaleSectionRing")
    check(len(scaled) == 2 and near(scaled[1][2], 1.5),
          "the second sample sends the INCREMENT (3/2), not 3 (%r)"
          % (scaled[1],))
    loop.release(sample(mods, session, cam, 200.0 + 3 * offset,
                        150.0 + 3 * offset))

    # The w axis is the tangent, which a 2D chart cannot translate along,
    # so it twists instead.
    loop.press(sample(mods, session, cam, 200.0, 120.0))
    loop.move(sample(mods, session, cam, 230.0, 150.0))
    twisted = stage.argsOf("twistSectionRing")
    check(len(twisted) == 1 and near(twisted[0][2], -math.pi / 2.0),
          "the w handle twists by the swept screen angle (%r)" % (twisted,))
    loop.release(sample(mods, session, cam, 230.0, 150.0))
    check(stage.argsOf("moveSectionRing") == [],
          "and never translated the ring while doing it")


def testSectionCV(mods):
    print("-- Section: a CV moves in the ring plane ------------------")
    tonicLib = mods["tonicLib"]
    frame = {"origin": (0.0, 0.0, -5.0), "u": (1.0, 0.0, 0.0),
             "v": (0.0, 0.0, 1.0), "w": (0.0, 1.0, 0.0), "scale": 1.0,
             "twist": 0.0}
    stage = FakeStage({(0, 1): frame})
    dll, session, state, loop = newTube(mods, "section", stage)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_SECTION_CV, "id": 0, "subId": 1,
        "subSubId": 3}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(session.readSelection(tonicLib.TONIC_PICK_SECTION_CV) ==
          [(0, 1, 3)], "ring 1 slot 3 is selected (%r)"
          % (session.readSelection(tonicLib.TONIC_PICK_SECTION_CV),))
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 300.0, 50.0))
    moved = stage.argsOf("moveSectionCV")
    check(len(moved) == 1 and moved[0][:3] == (0, 1, 3),
          "the drag moved that CV through the per-tube ABI (%r)" % (moved,))
    check(near(moved[0][3], 1.0) and near(moved[0][4], 0.0),
          "the screen-vertical travel is dropped: the CV stays in its "
          "ring (%r)" % (moved[0],))
    loop.release(sample(mods, session, cam, 300.0, 50.0))


def testDeleteAndDeactivate(mods):
    print("-- Delete, hover and leaving the mode ---------------------")
    tonicLib = mods["tonicLib"]
    stage = FakeStage()
    dll, session, state, loop = newTube(mods, "center", stage)
    cam = orthoCamera(mods["tonicCamera"])
    check(not loop.deleteSelection(), "Delete with nothing selected is "
                                      "not ours")
    session.select(tonicLib.TONIC_PICK_CENTER_CV, [0, 0], [0, 2], [-1, -1],
                   tonicLib.TONIC_SELECT_SET)
    check(loop.deleteSelection(), "Delete acts on the selection")
    check([a[1] for a in stage.argsOf("deleteCenterCV")] == [2, 0],
          "removing CVs runs descending so no index shifts (%r)"
          % (stage.argsOf("deleteCenterCV"),))
    check(("enqueueCommit", None) in session.events,
          "and the delete is committed")

    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 2,
        "subSubId": -1}
    loop.hover(sample(mods, session, cam, 200.0, 75.0))
    check(session.hovers[-1] == (tonicLib.TONIC_PICK_CENTER_CV, 0),
          "hovering highlights what a click would take (%r)"
          % (session.hovers[-1],))

    loop.deactivate()
    check(dll.gizmo["kind"] == tonicLib.TONIC_GIZMO_NONE,
          "leaving Tube mode takes the gizmo with it (%r)" % (dll.gizmo,))
    check(session.hovers[-1] == (0, -1), "and the hover")


def testFillSelection(mods):
    print("-- Fill: selection and the panel --------------------------")
    tonicLib = mods["tonicLib"]
    stage = FakeStage()
    dll, session, state, loop = newFill(mods, "params", stage)
    cam = orthoCamera(mods["tonicCamera"])
    check(loop.selectedTubes() == [0],
          "with nothing selected Fill means the primary tube")
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_TUBE_VERT, "id": 0, "subId": -1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) ==
          [(0, -1, -1)], "a click selects the tube")
    check(session.picks[-1][0] & tonicLib.TONIC_PICK_GUIDE,
          "and guides are pickable too: a dense tube hides behind its hair")

    # The panel edits the selection through the per-tube ABI.
    tonicPanels = mods["tonicPanels"]
    dll.selection[tonicLib.TONIC_PICK_TUBE_VERT] = [(0, -1, -1),
                                                    (3, -1, -1)]
    descs = {d.id: d for d in tonicPanels.descriptors("fill", state)}
    descs["density"].set(state, session, 24.0)
    written = stage.argsOf("setFillParams")
    check([w[0] for w in written] == [0, 3],
          "the density reached every selected tube (%r)"
          % ([w[0] for w in written],))
    check(near(written[0][1], 24.0), "with the value the panel was given")
    check(dll.argsOf("Tonic_RefillGuides")[-1] == (1.0,),
          "and the guides refilled at full density after it (%r)"
          % (dll.argsOf("Tonic_RefillGuides"),))
    check(dll.count("Tonic_SetFreezeRoots") == 0,
          "the panel never touched freeze roots on its own")


def testFillRampDrag(mods):
    print("-- Fill: the length-ramp drag -----------------------------")
    tonicLib = mods["tonicLib"]
    stage = FakeStage()
    dll, session, state, loop = newFill(mods, "preview", stage)
    cam = orthoCamera(mods["tonicCamera"])
    dll.previewFraction = 1.0
    state.previewFraction = 0.25
    state.freezeRoots = True
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_GUIDE, "id": 4, "subId": -1,
        "subSubId": -1}
    check(loop.press(sample(mods, session, cam, 200.0, 150.0)),
          "a press on the hair starts a ramp drag")
    check(session.gestureStack == ["Fill length profile"],
          "inside one bracket (%r)" % (session.gestureStack,))
    check(dll.argsOf("Tonic_SetPreviewFraction") == [(0.25,)],
          "at the panel's preview fraction (%r)"
          % (dll.argsOf("Tonic_SetPreviewFraction"),))

    loop.move(sample(mods, session, cam, 200.0, 30.0))
    written = stage.argsOf("setFillParams")
    check(written, "the drag wrote fill params (%r)" % (written,))
    profile = written[-1][5]
    check(profile[:2] == [0.0, 1.0] and near(profile[2], 0.5) and
          near(profile[3], 1.5) and profile[4:] == [1.0, 1.0],
          "dragging up lifts the ramp at the t under the cursor (%r)"
          % (profile,))
    check(dll.argsOf("Tonic_RefillGuides")[-1] == (0.25,),
          "and refilled at preview density (%r)"
          % (dll.argsOf("Tonic_RefillGuides"),))

    loop.release(sample(mods, session, cam, 200.0, 30.0))
    check(not session.gestureStack, "the release sealed the bracket")
    check(dll.argsOf("Tonic_SetPreviewFraction")[-1] == (1.0,),
          "the stored fraction came back (%r)"
          % (dll.argsOf("Tonic_SetPreviewFraction"),))
    check(dll.argsOf("Tonic_RefillGuides")[-1] == (1.0,),
          "and the guides went to full density (%r)"
          % (dll.argsOf("Tonic_RefillGuides"),))
    check(dll.count("Tonic_SetFreezeRoots") == 0 and state.freezeRoots,
          "freeze roots is the artist's switch and the loop never moved it")
    check(("enqueueCommit", None) in session.events,
          "the release enqueued the commit")

    # Escape mid-ramp restores instead of committing.
    session.events = []
    dll.reset()
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 200.0, 90.0))
    check(loop.cancel(), "Escape claimed the ramp drag")
    check(("cancel", None) in session.events and
          ("enqueueCommit", None) not in session.events,
          "it cancelled the bracket and committed nothing (%r)"
          % (session.events,))
    check(near(dll.previewFraction, 1.0),
          "with the preview fraction restored (%r)" % dll.previewFraction)


def testRampMaths(mods):
    print("-- Fill: the ramp knot rule -------------------------------")
    tonicLoopsFill = mods["tonicLoopsFill"]
    check(tonicLoopsFill.snapKnot(0.42) == 0.4 and
          tonicLoopsFill.snapKnot(-3.0) == 0.0 and
          tonicLoopsFill.snapKnot(9.0) == 1.0,
          "knots snap to the grid and stay inside [0, 1]")
    first = tonicLoopsFill.setProfileKnot([], 0.5, 1.5)
    check(first == [0.0, 1.0, 0.5, 1.5, 1.0, 1.0],
          "the first edit lays the ends down too, so the tube keeps its "
          "shape away from the knot (%r)" % (first,))
    again = tonicLoopsFill.setProfileKnot(first, 0.5, 2.0)
    check(again == [0.0, 1.0, 0.5, 2.0, 1.0, 1.0],
          "editing the same knot replaces it (%r)" % (again,))
    clamped = tonicLoopsFill.setProfileKnot(first, 0.5, 99.0)
    check(near(clamped[3], tonicLoopsFill.RAMP_MAX),
          "and the value is clamped (%r)" % (clamped,))
    other = tonicLoopsFill.setProfileKnot(first, 0.25, 0.5)
    check([other[i] for i in range(0, len(other), 2)] == [0.0, 0.25, 0.5,
                                                          1.0],
          "a new knot sorts into place (%r)" % (other,))


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
    from usdGenTonicTools import (tonicCamera, tonicGizmo, tonicLib,
                                  tonicLibStage, tonicLoops,
                                  tonicLoopsFill, tonicLoopsTube,
                                  tonicPanels)
    from usdGenTonicTools.tonicToolState import TonicToolState
    mods = {"tonicCamera": tonicCamera, "tonicGizmo": tonicGizmo,
            "tonicLib": tonicLib, "tonicLibStage": tonicLibStage,
            "tonicLoops": tonicLoops, "tonicLoopsFill": tonicLoopsFill,
            "tonicLoopsTube": tonicLoopsTube, "tonicPanels": tonicPanels,
            "TonicToolState": TonicToolState}
    testShelf(mods)
    testSelectAndGizmo(mods)
    testCenterDrag(mods)
    testConstraintsAndEscape(mods)
    testSoftSpan(mods)
    testWholeTubeDrag(mods)
    testRingPlane(mods)
    testRingScaleTwist(mods)
    testSectionCV(mods)
    testDeleteAndDeactivate(mods)
    testFillSelection(mods)
    testFillRampDrag(mods)
    testRampMaths(mods)
    print("testUsdGenTonicToolsLoopsTube: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
