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
#   * a press resolves the precise component pass before the body fallback,
#     and a drag never re-picks: the whole gesture runs off the press-time
#     selection, camera and ring frames;
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
        self.rejectCenterCVs = set()
        self.centerHandles = {}
        for name in ("Tonic_GetTubeCenterCount", "Tonic_GetTubeCenterCV",
                     "Tonic_GetTubeCenterHandle", "Tonic_MoveTubeCenterCV", "Tonic_TranslateTube",
                     "Tonic_GetTubeSection"):
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

    def Tonic_GetTubeCenterHandle(self, _model, tubeId, cv, out3):
        # Symmetric fixtures default to the authored center; a targeted test
        # can provide an off-centre displayed core without changing raw data.
        point = self.centerHandles.get((int(tubeId), int(cv)),
                                       self.centers[int(tubeId)][int(cv)])
        for i in range(3):
            out3[i] = point[i]
        return 0

    def Tonic_MoveTubeCenterCV(self, _model, tubeId, cv, dx, dy, dz):
        self._record("Tonic_MoveTubeCenterCV",
                     (int(tubeId), int(cv), _f(dx), _f(dy), _f(dz)))
        if int(cv) in self.rejectCenterCVs:
            return 1
        point = self.centers[int(tubeId)][int(cv)]
        for i, d in enumerate((dx, dy, dz)):
            point[i] += _f(d)
        return 0

    def Tonic_TranslateTube(self, _model, tubeId, dx, dy, dz):
        self._record("Tonic_TranslateTube",
                     (int(tubeId), _f(dx), _f(dy), _f(dz)))
        for point in self.centers[int(tubeId)]:
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
        self.hoverItems = []
        self.rects = []
        self.polygons = []
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
        self.picks.append((int(kindMask), float(x), float(y),
                           float(radiusPx)))
        return self.pickFn(int(kindMask), float(x), float(y))

    def setHover(self, kind=0, ident=-1, subId=-1, subSubId=-1):
        self.hovers.append((int(kind), int(ident)))
        self.hoverItems.append((int(kind), int(ident), int(subId),
                                int(subSubId)))
        return True

    def selectRect(self, camera, x0, y0, x1, y1, kindMask, mode):
        self.rects.append((x0, y0, x1, y1, int(kindMask), int(mode)))
        return True

    def selectPolygon(self, camera, points, kindMask, mode):
        self.polygons.append((tuple(points), int(kindMask), int(mode)))
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
    check(len(tonicLoops.subModesFor("tube")) == 4 and
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
          "an unselected component hit needs no body fallback (%d)" %
          len(session.picks))
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
    loop.release(sample(mods, session, cam, 200.0, 75.0, ("shift",)))
    check(len(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)) == 2,
          "Shift adds the second CV")
    # Away from the gizmo's axes: within their tolerance Ctrl means the
    # normal constraint, not a toggle.
    loop.press(sample(mods, session, cam, 360.0, 75.0, ("ctrl",)))
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
    check(session.rects[-1][4] == loop.componentMask,
          "over the sub-mode's editable components (%r)" %
          (session.rects[-1],))


def testComponentPriorityAndBodyFallback(mods):
    print("-- Tube components: priority and body fallback ------------")
    tonicLib = mods["tonicLib"]
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    state.snapRadiusPx = 0.25
    state.transformTool = "select"
    cv0 = {"kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0,
           "subId": 0, "subSubId": -1}
    cv2 = {"kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0,
           "subId": 2, "subSubId": -1}
    body = {"kind": tonicLib.TONIC_PICK_TUBE_VERT, "id": 0,
            "subId": -1, "subSubId": -1}

    def picker(mask, x, _y):
        # The exact component resolver owns the displayed dots.  The broad
        # Tube mask stands in for a dense body directly underneath them.
        if mask == tonicLib.TONIC_PICK_CENTER_CV:
            return cv0 if x < 150.0 else cv2 if x < 250.0 else None
        return body if mask & tonicLib.TONIC_PICK_TUBE_VERT else None

    session.pickFn = picker
    loop.hover(sample(mods, session, cam, 100.0, 150.0))
    check(session.hoverItems[-1] ==
          (tonicLib.TONIC_PICK_CENTER_CV, 0, 0, -1),
          "a visible CV prehighlights its exact tube and CV identity %r" %
          (session.hoverItems[-1],))
    check(near(session.picks[-1][3], 8.0),
          "component prehighlight keeps an 8px target when Snap is tiny %r" %
          (session.picks[-1],))

    loop.press(sample(mods, session, cam, 100.0, 150.0))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 0, -1)] and
          not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT),
          "the component press beats the body under it")
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 2, -1)] and
          not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT),
          "repeated plain CV clicks replace the exact component, never wedge a tube")
    loop.press(sample(mods, session, cam, 100.0, 150.0, ("shift",)))
    loop.release(sample(mods, session, cam, 100.0, 150.0, ("shift",)))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 2, -1), (0, 0, -1)],
          "Shift click adds the precise displayed CV")

    # In component tools a drag which begins over the tube body is still a
    # component area selection.  The body is considered only by a no-travel
    # release, preserving the intentional whole-tube click fallback.
    session.clearSelection()
    loop.press(sample(mods, session, cam, 320.0, 150.0))
    check(loop._marquee == (320.0, 150.0) and
          not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT),
          "component box starts over body without selecting the whole tube")
    loop.move(sample(mods, session, cam, 350.0, 180.0))
    loop.release(sample(mods, session, cam, 350.0, 180.0))
    check(session.rects[-1][4] == tonicLib.TONIC_PICK_CENTER_CV and
          not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT),
          "component box never turns its body start into a tube selection")

    loop.press(sample(mods, session, cam, 320.0, 150.0))
    loop.release(sample(mods, session, cam, 320.0, 150.0))
    check(session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) ==
          [(0, -1, -1)],
          "a no-travel body click remains the explicit whole-tube fallback")

    session.clearSelection()
    state.selectionShape = "lasso"
    loop.press(sample(mods, session, cam, 320.0, 150.0))
    loop.move(sample(mods, session, cam, 350.0, 150.0))
    loop.move(sample(mods, session, cam, 350.0, 180.0))
    loop.release(sample(mods, session, cam, 320.0, 150.0))
    check(session.polygons and
          session.polygons[-1][1] == tonicLib.TONIC_PICK_CENTER_CV and
          not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT),
          "component lasso also ignores a tube body at its press point")
    state.selectionShape = "box"

    loop.setSubMode("tube")
    session.clearSelection()
    loop.press(sample(mods, session, cam, 320.0, 150.0))
    check(session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) ==
          [(0, -1, -1)] and loop._marquee is None,
          "explicit Whole Tube mode keeps immediate body selection")

    # The same resolver owns inner section CVs; they retain all three
    # identity coordinates instead of degrading to the tube body.
    section = {"kind": tonicLib.TONIC_PICK_SECTION_CV, "id": 0,
               "subId": 1, "subSubId": 3}
    loop.setSubMode("section")
    session.clearSelection()
    session.pickFn = lambda mask, _x, _y: (
        section if mask == tonicLib.TONIC_PICK_SECTION_CV else
        body if mask & tonicLib.TONIC_PICK_TUBE_VERT else None)
    loop.hover(sample(mods, session, cam, 240.0, 150.0))
    loop.press(sample(mods, session, cam, 240.0, 150.0))
    check(session.hoverItems[-1] ==
          (tonicLib.TONIC_PICK_SECTION_CV, 0, 1, 3) and
          session.readSelection(tonicLib.TONIC_PICK_SECTION_CV) ==
          [(0, 1, 3)] and
          not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT),
          "an inner section CV has the same fixed-radius priority and identity")

    # Ring mode renders the same vertices but owns a whole ring. K11's
    # native ring candidate is its centroid, so the loop must normalize a
    # displayed vertex hit and its box/lasso candidates to that owner.
    loop.setSubMode("ring")
    session.clearSelection()
    def ringPicker(mask, x, _y):
        if mask == tonicLib.TONIC_PICK_SECTION_RING:
            return None                 # no centroid under this vertex
        if mask == tonicLib.TONIC_PICK_SECTION_CV and x < 250.0:
            return section
        return body if mask & tonicLib.TONIC_PICK_TUBE_VERT else None
    session.pickFn = ringPicker
    loop.hover(sample(mods, session, cam, 240.0, 150.0))
    loop.press(sample(mods, session, cam, 240.0, 150.0))
    check(session.hoverItems[-1] ==
          (tonicLib.TONIC_PICK_SECTION_RING, 0, 1, -1) and
          session.readSelection(tonicLib.TONIC_PICK_SECTION_RING) ==
          [(0, 1, -1)] and
          not session.readSelection(tonicLib.TONIC_PICK_SECTION_CV),
          "a displayed section vertex selects and prehighlights its owning ring")

    def rectVertices(camera, x0, y0, x1, y1, mask, mode):
        session.rects.append((x0, y0, x1, y1, int(mask), int(mode)))
        session.select(tonicLib.TONIC_PICK_SECTION_CV, [0, 0, 0],
                       [1, 1, 2], [0, 2, 1], mode)
        return True
    session.selectRect = rectVertices
    session.clearSelection()
    loop.press(sample(mods, session, cam, 320.0, 150.0))
    loop.move(sample(mods, session, cam, 350.0, 180.0))
    loop.release(sample(mods, session, cam, 350.0, 180.0))
    check(session.rects[-1][4] == tonicLib.TONIC_PICK_SECTION_CV and
          session.readSelection(tonicLib.TONIC_PICK_SECTION_RING) ==
          [(0, 1, -1), (0, 2, -1)] and
          not session.readSelection(tonicLib.TONIC_PICK_SECTION_CV),
          "ring box maps displayed vertices to unique rings without CV residue")

    def lassoVertices(camera, points, mask, mode):
        session.polygons.append((tuple(points), int(mask), int(mode)))
        session.select(tonicLib.TONIC_PICK_SECTION_CV, [0, 0], [2, 2],
                       [0, 2], mode)
        return True
    session.selectPolygon = lassoVertices
    session.clearSelection()
    state.selectionShape = "lasso"
    loop.press(sample(mods, session, cam, 320.0, 150.0))
    loop.move(sample(mods, session, cam, 350.0, 150.0))
    loop.move(sample(mods, session, cam, 350.0, 180.0))
    loop.release(sample(mods, session, cam, 320.0, 150.0))
    check(session.polygons[-1][1] == tonicLib.TONIC_PICK_SECTION_CV and
          session.readSelection(tonicLib.TONIC_PICK_SECTION_RING) ==
          [(0, 2, -1)] and
          not session.readSelection(tonicLib.TONIC_PICK_SECTION_CV),
          "ring lasso maps displayed vertices to their unique ring owner")
    state.selectionShape = "box"


def testPartialDragCleanup(mods):
    print("-- Tube drag: partial bracket cleanup --------------------")
    tonicLib = mods["tonicLib"]
    dll, session, _state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    session.select(tonicLib.TONIC_PICK_CENTER_CV, [0], [1], [-1],
                   tonicLib.TONIC_SELECT_SET)
    loop._placeGizmo(cam)
    session.pickFn = lambda _mask, _x, _y: None

    def failFreeze():
        raise RuntimeError("test press-time freeze failure")

    loop._freezeTransformBaseline = failFreeze
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(("begin", "Tube center") in session.events and
          ("cancel", None) in session.events and not session.gestureStack and
          not loop._bracketOpen and not loop._dragging,
          "a press-time setup failure closes the native bracket before dragging")
    cancelled = session.events.count(("cancel", None))
    loop.deactivate()
    check(session.events.count(("cancel", None)) == cancelled,
          "deactivate is idempotent after partial drag cleanup")

    # A live bracket is also cancelled before a component-domain change.
    dll, session, _state, loop = newTube(mods)
    session.select(tonicLib.TONIC_PICK_CENTER_CV, [0], [1], [-1],
                   tonicLib.TONIC_SELECT_SET)
    loop._placeGizmo(cam)
    session.pickFn = lambda _mask, _x, _y: None
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(loop._bracketOpen and loop._dragging,
          "the normal handle press opens a live Tube bracket")
    loop.setSubMode("section")
    check(("cancel", None) in session.events and not session.gestureStack and
          not loop._bracketOpen and not loop._dragging and
          loop.subMode() == "section",
          "a component-mode change cancels its active Tube drag first")


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
    check(len(session.picks) == 2,
          "the handle press performs one component-priority query (%d picks)"
          % len(session.picks))
    check(dll.argsOf("Tonic_SetPreviewFraction") == [(0.25,)],
          "the drag dropped the guides to the preview fraction (%r)"
          % (dll.argsOf("Tonic_SetPreviewFraction"),))

    loop.move(sample(mods, session, cam, 250.0, 150.0))
    loop.move(sample(mods, session, cam, 300.0, 150.0))
    check(len(session.picks) == 2,
          "drag moves do not re-pick after the press-time priority query")
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


def testRefusedCenterDrag(mods):
    print("-- Center: refused writes do not commit -------------------")
    tonicLib = mods["tonicLib"]
    cam = orthoCamera(mods["tonicCamera"])

    # A native refusal is not a visual edit: the drag may have opened its
    # bracket and lowered preview density, but it must not refill/commit an
    # unchanged model when released.
    dll, session, _state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    before = tuple(dll.centers[0][1])
    dll.rejectCenterCVs = {1}
    dll.reset()
    session.events = []
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 250.0, 150.0))
    check(tuple(dll.centers[0][1]) == before and not loop._pendingEdit,
          "a refused center write leaves the geometry and edit flag alone")
    check(not dll.argsOf("Tonic_RefillGuides"),
          "a refused move does not refill a changed-looking preview")
    check(session.statuses and "Tonic_MoveTubeCenterCV failed" in
          session.statuses[-1],
          "the native refusal remains visible in the status (%r)" %
          session.statuses[-1:])
    loop.release(sample(mods, session, cam, 250.0, 150.0))
    check(("enqueueCommit", None) not in session.events and
          not session.gestureStack,
          "releasing a fully refused drag closes its bracket without a "
          "commit (%r)" % (session.events,))

    # One refused owner must not discard a valid sibling write from the same
    # press-time selection.  That real change still previews and commits.
    dll, session, _state, loop = newTube(mods)
    session.select(tonicLib.TONIC_PICK_CENTER_CV, [0, 0], [0, 2], [-1, -1],
                   tonicLib.TONIC_SELECT_SET)
    loop._placeGizmo(cam)
    before0 = tuple(dll.centers[0][0])
    before2 = tuple(dll.centers[0][2])
    dll.rejectCenterCVs = {0}
    dll.reset()
    session.events = []
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 250.0, 150.0))
    check(tuple(dll.centers[0][0]) == before0 and
          dll.centers[0][2][0] > before2[0] and loop._pendingEdit,
          "a valid sibling write still marks the mixed batch as changed")
    check(dll.argsOf("Tonic_RefillGuides") == [(0.25,)],
          "the accepted sibling alone receives a preview refill (%r)" %
          dll.argsOf("Tonic_RefillGuides"))
    loop.release(sample(mods, session, cam, 250.0, 150.0))
    check(("enqueueCommit", None) in session.events,
          "the mixed batch commits its accepted write (%r)" %
          session.events)


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
    loop.release(sample(mods, session, cam, 200.0, 75.0, ("shift",)))
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
    moved = dll.argsOf("Tonic_TranslateTube")
    check(moved == [(0, 0.5, 0.0, 0.0)] and
          not dll.argsOf("Tonic_MoveTubeCenterCV"),
          "a whole-tube Move uses one atomic center-cage translation (%r)"
          % (moved,))
    check(all(near(point[0], source[0] + 0.5)
              for point, source in zip(dll.centers[0], CENTERS)),
          "the atomic call translates every center by the same delta (%r)"
          % (dll.centers[0],))
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
    print("-- Ring: Scale and Rotate tools ----------------------------")
    tonicLib = mods["tonicLib"]
    # Tilt v toward the camera.  The displayed scale handle is consequently
    # an ellipse; this guards against treating its visible contour as a
    # screen-space circle.
    frame = {"origin": (0.0, 0.0, -5.0), "u": (1.0, 0.0, 0.0),
             "v": (0.0, 0.8, 0.6), "w": (0.0, -0.6, 0.8),
             "scale": 1.0, "twist": 0.0}
    stage = FakeStage({(0, 1): frame})
    dll, session, state, loop = newTube(mods, "ring", stage)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_SECTION_RING, "id": 0, "subId": 1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    # An asymmetric chart makes the centroid pivot visible.  Ring selection
    # owns every slot; Scale applies the frozen target positions through the
    # section-CV ABI rather than the retired multiplicative ring operation.
    loop._section = lambda _tube, _ring: (
        0.5, [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)], 1.0, 0.0)
    loop.setTransformTool("scale")
    gizmo = mods["tonicGizmo"]
    handles = {record["handle"] for record in loop._gizmo.screenHandles(cam)}
    check(handles == {gizmo.HANDLE_U, gizmo.HANDLE_V, gizmo.HANDLE_CENTER,
                      gizmo.HANDLE_PLANE_XY},
          "Section Scale exposes only in-plane and uniform handles (%r)" %
          handles)
    centre = cam.worldToPixels(loop._gizmo.origin)
    uTip = cam.worldToPixels(loop._gizmo.axisEndpoint(gizmo.HANDLE_U))
    check(loop._gizmo.handleAt(cam, uTip[0], uTip[1]) == gizmo.HANDLE_U,
          "the visible U scale handle is pickable")
    loop.press(sample(mods, session, cam, uTip[0], uTip[1]))
    twice = (centre[0] + 2.0 * (uTip[0] - centre[0]),
             centre[1] + 2.0 * (uTip[1] - centre[1]))
    loop.move(sample(mods, session, cam, twice[0], twice[1]))
    scaled = stage.argsOf("moveSectionCV")
    check(len(scaled) == 4 and all(row[:2] == (0, 1) for row in scaled),
          "Scale writes every selected ring slot through the CV ABI (%r)" %
          scaled)
    check(any(abs(row[3]) > 1e-4 for row in scaled) and
          all(near(row[4], 0.0) for row in scaled),
          "the U scale stays in the section chart's U direction (%r)" %
          scaled)
    loop.release(sample(mods, session, cam, twice[0], twice[1]))

    # Rotate has only the section normal: view/free and U/V rings are both
    # hidden and unpickable, so a chart can never be rotated out of plane.
    loop.setTransformTool("rotate")
    handles = {record["handle"] for record in loop._gizmo.screenHandles(cam)}
    check(handles == {gizmo.HANDLE_W},
          "Section Rotate exposes only the normal ring (%r)" % handles)
    record = loop._gizmo.screenHandles(cam)[0]
    turnStart = record["points"][6]
    turnEnd = (centre[0] - (turnStart[1] - centre[1]),
               centre[1] + (turnStart[0] - centre[0]))
    loop.press(sample(mods, session, cam, turnStart[0], turnStart[1]))
    loop.move(sample(mods, session, cam, turnEnd[0], turnEnd[1]))
    rotated = stage.argsOf("moveSectionCV")
    check(len(rotated) == 8,
          "Rotate also updates the selected ring's frozen slots (%r)" %
          rotated)
    loop.release(sample(mods, session, cam, turnEnd[0], turnEnd[1]))


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
    check(all(call[:3] == (0, 1, 3) for call in moved),
          "the individual-CV drag did not move a section sibling (%r)"
          % (moved,))
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


def testFrozenTransformMaths(mods):
    print("-- Tube transform: frozen pivots -------------------------")
    transforms = mods["tonicTubeTransforms"]
    frame = (1.0, 0.0, 0.0,
             0.0, 1.0, 0.0,
             0.0, 0.0, 1.0)
    frozen = transforms.FrozenPoints(
        {"root": (0.0, 0.0, 0.0), "tip": (0.0, 2.0, 0.0)},
        (0.0, 0.0, 0.0), frame)
    rotated = frozen.absolute(rotateAxis=(0.0, 0.0, 1.0),
                              radians=math.pi / 2.0)
    check(all(near(rotated["root"][axis], 0.0) for axis in range(3)) and
          near(rotated["tip"][0], -2.0) and near(rotated["tip"][1], 0.0),
          "rotation uses the frozen root pivot (%r)" % rotated)
    scaled = frozen.absolute(scale=(2.0, 0.5, 1.0))
    check(near(scaled["tip"][1], 1.0),
          "frame-local nonuniform scale uses the frozen pivot (%r)" % scaled)
    moved = frozen.absolute(translation=(3.0, 0.0, 0.0))
    check(near(moved["root"][0], 3.0) and near(moved["tip"][0], 3.0),
          "whole-tube move translates every frozen target (%r)" % moved)
    first = frozen.absolute(translation=(1.0, 0.0, 0.0))
    second = frozen.absolute(translation=(2.5, 0.0, 0.0))
    steps = transforms.FrozenPoints.increments(first, second)
    check(near(steps["root"][0], 1.5) and near(steps["tip"][0], 1.5),
          "repeated samples are absolute-from-press increments (%r)" % steps)


def testLoopRotateScaleBaselines(mods):
    print("-- Tube transform: component application -----------------")
    tonicLib = mods["tonicLib"]
    gizmo = mods["tonicGizmo"]
    # Rotation starts at the tube root, freezes every whole-tube center and
    # applies the second sample from that baseline rather than re-rotating
    # the already-edited curve.
    dll, session, state, loop = newTube(mods)
    state.transformTool = "rotate"
    loop._centerDrag = {0: ([0, 1, 2], -1)}
    dll.centerHandles[(0, 0)] = (0.4, -1.0, -5.0)
    loop._gizmo.frame = mods["tonicTubeTransforms"].IDENTITY_FRAME
    loop._freezeTransformBaseline()
    pivot = loop._transformOwners[0]["frozen"].pivot
    check(all(near(pivot[i], (0.4, -1.0, -5.0)[i]) for i in range(3)),
          "whole-tube Rotate freezes at the displayed core root, not raw cage "
          "(%r)" % (pivot,))
    # Keep the pre-existing transform-math assertions symmetric; the check
    # above isolated the off-centre pivot contract.
    dll.centerHandles.clear()
    loop._transformOwners = []
    loop._freezeTransformBaseline()
    loop._gizmo.rotationDrag = lambda _c, _x, _y: ((0.0, 0.0, 1.0),
                                                   math.pi / 2.0)
    dragSample = sample(mods, session, orthoCamera(mods["tonicCamera"]),
                        200.0, 150.0)
    check(loop._applyRotation(dragSample), "whole-tube Rotate accepts a live axis")
    moved = dll.argsOf("Tonic_MoveTubeCenterCV")
    check([row[1] for row in moved] == [1, 2],
          "Rotate keeps the root CV pinned (%r)" % moved)
    check(near(dll.centers[0][1][0], -1.0) and
          near(dll.centers[0][2][0], -2.0),
          "Rotate bends offsets around the frozen root (%r)" % dll.centers[0])
    loop._gizmo.rotationDrag = lambda _c, _x, _y: ((0.0, 0.0, 1.0), math.pi)
    loop._applyRotation(dragSample)
    check(near(dll.centers[0][1][0], 0.0) and
          near(dll.centers[0][1][1], -2.0),
          "the next rotation is still absolute from press (%r)" %
          dll.centers[0][1])

    # A selected Section-CV gets only its chart-space U/V delta.  Scaling
    # another slot or moving along W would violate component isolation.
    frame = {"origin": (0.0, 0.0, -5.0), "u": (1.0, 0.0, 0.0),
             "v": (0.0, 1.0, 0.0), "w": (0.0, 0.0, 1.0),
             "scale": 1.0, "twist": 0.0}
    stage = FakeStage({(0, 1): frame})
    dll, session, state, loop = newTube(mods, "section", stage)
    state.transformTool = "scale"
    session.select(tonicLib.TONIC_PICK_SECTION_CV, [0], [1], [3],
                   tonicLib.TONIC_SELECT_SET)
    # An off-centre chart proves the transform pivots at the ring centroid,
    # not raw UV zero.  Slot 3 lies +.5/+ .5 from that centroid.
    loop._section = lambda _tube, _ring: (
        0.5, [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)], 1.0, 0.0)
    loop._gizmo.activeHandle = gizmo.HANDLE_U
    loop._freezeTransformBaseline()
    loop._gizmo.scaleFactor = lambda _c, _x, _y: 2.0
    dragSample = sample(mods, session, orthoCamera(mods["tonicCamera"]),
                        200.0, 150.0)
    check(loop._applyScale(dragSample), "section U scale accepts a positive factor")
    moves = stage.argsOf("moveSectionCV")
    check(len(moves) == 1 and moves[0][:3] == (0, 1, 3) and
          near(moves[0][3], 0.5) and near(moves[0][4], 0.0),
          "Section scale moves only the selected slot in its plane (%r)" %
          moves)
    loop._gizmo.activeHandle = gizmo.HANDLE_W
    check(not loop._applyScale(dragSample),
          "section normal scale is rejected before a chart write")


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
                                  tonicTubeTransforms,
                                  tonicPanels)
    from usdGenTonicTools.tonicToolState import TonicToolState
    mods = {"tonicCamera": tonicCamera, "tonicGizmo": tonicGizmo,
            "tonicLib": tonicLib, "tonicLibStage": tonicLibStage,
            "tonicLoops": tonicLoops, "tonicLoopsFill": tonicLoopsFill,
            "tonicLoopsTube": tonicLoopsTube,
            "tonicTubeTransforms": tonicTubeTransforms,
            "tonicPanels": tonicPanels,
            "TonicToolState": TonicToolState}
    testShelf(mods)
    testSelectAndGizmo(mods)
    testComponentPriorityAndBodyFallback(mods)
    testPartialDragCleanup(mods)
    testCenterDrag(mods)
    testRefusedCenterDrag(mods)
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
    testFrozenTransformMaths(mods)
    testLoopRotateScaleBaselines(mods)
    print("testUsdGenTonicToolsLoopsTube: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
