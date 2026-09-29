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
                     "Tonic_GetTubeSection", "Tonic_GetTubeSectionCount"):
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

    def Tonic_GetTubeSectionCount(self, _model, tubeId):
        # Every fixture tube carries the census's ring count; GZ-04 walks
        # them to convert a centre CV into the rings stationed on it.
        return self.sectionCount if int(tubeId) in self.centers else -1

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
                      "size": _f(size), "handle": int(handle),
                      "mask": 0xFFFFFFFF}
        self._record("Tonic_SetGizmo", (int(kind), int(handle)))
        return 0

    def Tonic_SetGizmoEx(self, model, kind, origin, frame, size, handle,
                         mask):
        # GZ-06: the same record plus the Hydra fallback's handle mask;
        # recorded under the plain name so call counts stay comparable.
        self.Tonic_SetGizmo(model, kind, origin, frame, size, handle)
        self.gizmo["mask"] = int(getattr(mask, "value", mask))
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

    def endGestureIfChanged(self, changed):
        # tonicSession.endGestureIfChanged: keep the step only on a change.
        if changed:
            return self.endGesture()
        dirty = self.cancelGesture()
        if dirty:
            self.publish(dirty)
        return False

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


def lookAtCamera(tonicCamera, eye, target=(0.0, 0.0, -5.0), up=(0.0, 1.0,
                                                                  0.0),
                 pixelRatio=1.0):
    """An orthographic 400 x 400 view over [-2, 2] aimed from `eye`.

    Row-vector convention (p * view * proj), as tonicCamera takes it: the
    view's columns are the camera right, up and backward axes.
    """
    def sub(a, b):
        return tuple(a[i] - b[i] for i in range(3))

    def unit(v):
        length = sum(c * c for c in v) ** 0.5
        return tuple(c / length for c in v)

    def cross(a, b):
        return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0])

    def dot(a, b):
        return sum(a[i] * b[i] for i in range(3))

    f = unit(sub(target, eye))
    r = unit(cross(f, up))
    u = cross(r, f)
    view = (r[0], u[0], -f[0], 0.0,
            r[1], u[1], -f[1], 0.0,
            r[2], u[2], -f[2], 0.0,
            -dot(r, eye), -dot(u, eye), dot(f, eye), 1.0)
    a = -2.0 / 20.0
    b = -1.0 + a * 1.0
    proj = (0.5, 0.0, 0.0, 0.0,
            0.0, 0.5, 0.0, 0.0,
            0.0, 0.0, a, 0.0,
            0.0, 0.0, b, 1.0)
    return tonicCamera.TonicCamera(tonicCamera.matMul(view, proj), 400, 400,
                                   pixelRatio)


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
    roundRings(dll)
    return dll, session, state, loop


# Fill's ramp keys its knot by the radius under the press, so its fixture
# rings are real circles: radius 0.5 world units, 50 px on the ortho camera.
FILL_RING_RADIUS = 0.5


def roundRings(dll, radius=FILL_RING_RADIUS):
    """Every ring of every fixture tube a circle of `radius` (chart units)."""
    def section(_model, tubeId, ring, t, uv, uvLen, count, scale, twist):
        dll._record("Tonic_GetTubeSection", (int(tubeId), int(ring)))
        _deref(count).value = 8
        if t is not None:
            _deref(t).value = 0.5 * int(ring)
            _deref(scale).value = 1.0
            _deref(twist).value = 0.0
            for i in range(min(uvLen // 2, 8)):
                angle = 2.0 * math.pi * i / 8.0
                uv[2 * i] = radius * math.cos(angle)
                uv[2 * i + 1] = radius * math.sin(angle)
        return 0
    dll.Tonic_GetTubeSection = section


def guideRoots(dll, rootsRU):
    """Answer Tonic_ReadGuideRoots with these normalised (ru, rv) roots."""
    def read(_model, faceIds, xyz, ru, maxRoots, outCount):
        _deref(outCount).value = len(rootsRU)
        if ru is not None:
            for i, (u, v) in enumerate(rootsRU[:maxRoots]):
                ru[2 * i] = u
                ru[2 * i + 1] = v
        return 0
    dll.Tonic_ReadGuideRoots = read
    dll.guideCount = len(rootsRU)


def sample(mods, session, camera, x, y, modifiers=()):
    return mods["tonicLoops"].Sample(session, camera, x, y,
                                     frozenset(modifiers))


def click(mods, session, camera, loop, x, y, modifiers=()):
    """A no-travel press and release, as the controller delivers a click.

    With Move a selecting press already holds a tweak drag (GZ-03), so a
    test that selects and then presses a handle must release in between,
    exactly as a real mouse does.
    """
    loop.press(sample(mods, session, camera, x, y, modifiers))
    loop.release(sample(mods, session, camera, x, y, modifiers))


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
    # DK-02: the component kinds are the F8--F11 row; Q/W/E/R are the
    # transform tool and must not also name a sub-mode.
    check(loop.subModeForHotkey("F10") == "ring" and
          loop.subModeForHotkey("F11") == "section" and
          loop.subModeForHotkey("R") == "" and
          loop.subModeForHotkey("E") == "",
          "F10/F11 reach the Ring and Section sub-modes, R/E do not")
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
    # GZ-03 tweak: with Move the same press already holds a drag on what
    # it selected, through the camera-plane centre handle.
    check(loop._dragging and
          loop._gizmo.activeHandle == mods["tonicGizmo"].HANDLE_CENTER and
          session.gestureStack == ["Tube center"],
          "with Move the selecting press holds a tweak drag (%r)"
          % session.gestureStack)
    loop.release(sample(mods, session, cam, 200.0, 150.0))
    check(not session.gestureStack and ("cancel", None) in session.events and
          ("end", None) not in session.events and
          not dll.argsOf("Tonic_MoveTubeCenterCV"),
          "a no-travel release is a plain selection: its bracket is "
          "cancelled, no undo step, no move (%r)" % (session.events,))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "and the selection survives the cancelled tweak")

    # SL-01's click table: Shift toggles (so it adds an unselected CV),
    # Ctrl removes and never adds -- off every handle.  CV 2's own pixel
    # (200, 75) is within the V axis tip's tolerance, and a Shift press on
    # a handle now drags it (a DCC), so these clicks land in open space.
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 2,
        "subSubId": -1}
    check(loop._gizmo.handleAt(cam, 330.0, 40.0) ==
          mods["tonicGizmo"].HANDLE_NONE, "(330, 40) is off every handle")
    loop.press(sample(mods, session, cam, 330.0, 40.0, ("shift",)))
    loop.release(sample(mods, session, cam, 330.0, 40.0, ("shift",)))
    check(len(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)) == 2,
          "Shift adds the second CV")
    loop.press(sample(mods, session, cam, 360.0, 75.0, ("ctrl",)))
    loop.release(sample(mods, session, cam, 360.0, 75.0, ("ctrl",)))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "Ctrl removes it again (%r)"
          % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))
    loop.press(sample(mods, session, cam, 360.0, 75.0, ("ctrl",)))
    loop.release(sample(mods, session, cam, 360.0, 75.0, ("ctrl",)))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "Ctrl on an unselected CV does not add it (%r)"
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

    # GZ-05: the Axis Orientation.  World is the default, so red/green/blue
    # really are world X/Y/Z even under a tilted camera, and all three axes
    # can be grabbed (the old screen frame left blue pointing at the eye).
    tonicGizmo = mods["tonicGizmo"]
    dll, session, state, loop = newTube(mods)
    tilted = lookAtCamera(mods["tonicCamera"], (6.0, 7.0, 3.0))
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    tip = tilted.worldToPixels((0.0, 0.0, -5.0))
    click(mods, session, tilted, loop, tip[0], tip[1])
    gizmo = loop._gizmo
    axes = [record for record in gizmo.screenHandles(tilted)
            if record["kind"] == "axis"]
    check(state.transformOrientation == "world" and
          gizmo.frame == tonicGizmo.IDENTITY_FRAME,
          "World under a tilted camera is the identity frame (%r)"
          % (gizmo.frame,))
    check(len(axes) == 3 and all(record["grabbable"] for record in axes),
          "and all three world axes are grabbable (%r)"
          % [record["grabbable"] for record in axes])
    state.transformOrientation = "screen"
    loop.refreshGizmo(tilted)
    check(gizmo.frame == tonicGizmo.screenFrame(tilted, gizmo.origin) and
          gizmo.frame != tonicGizmo.IDENTITY_FRAME,
          "Screen lines the handles up with the camera plane")
    state.transformOrientation = "tube"
    loop.refreshGizmo(tilted)
    check(all(near(gizmo.frame[6 + i], (0.0, 1.0, 0.0)[i])
              for i in range(3)),
          "Tube points w along tube 0's root normal (%r)"
          % (gizmo.frame[6:9],))
    check(loop.toggleOrientation(tilted) == "world" and
          gizmo.frame == tonicGizmo.IDENTITY_FRAME and
          loop.toggleOrientation(tilted) == "tube" and
          state.transformOrientation == "tube",
          "L flips Tube <-> World and re-places the gizmo at once")
    state.transformOrientation = "world"

    # GZ-05 HiDPI: every pixel constant is LOGICAL.  On a 2x display the
    # gizmo keeps its on-screen size (twice the world size per physical
    # pixel measured) and a 14 px miss still lands on an axis.
    cam = orthoCamera(mods["tonicCamera"])
    hidpi = mods["tonicCamera"].TonicCamera(cam.viewProj, 400, 300, 2.0)
    loop.refreshGizmo(cam)
    single = gizmo.sizeWorld
    loop.refreshGizmo(hidpi)
    check(near(gizmo.sizeWorld, 2.0 * single),
          "a 2x camera doubles sizeWorld (%g -> %g)"
          % (single, gizmo.sizeWorld))
    check(gizmo.handleAt(hidpi, 300.0, 164.0) == tonicGizmo.HANDLE_U and
          gizmo.handleAt(cam, 300.0, 164.0) == tonicGizmo.HANDLE_NONE,
          "a 14 px miss grabs U at 2x but not at 1x")
    check(near(loop.componentPickRadiusPx(hidpi), 16.0) and
          near(loop.componentPickRadiusPx(cam), 8.0) and
          near(loop.componentPickRadiusPx(), 8.0),
          "the CV pick radius is logical too (%g at 2x)"
          % loop.componentPickRadiusPx(hidpi))
    # The manipulator size (`+` / `-`) is logical pixels as well.
    loop.refreshGizmo(cam)
    size = loop.scaleManipulator(1.1, cam)
    check(near(size, 99.0) and near(gizmo.sizeWorld, 0.99),
          "'+' grows the manipulator 10%% (%r px, %g world)"
          % (size, gizmo.sizeWorld))


def testGizmoParity(mods):
    """Parity G07, G08, G11, G23: Ctrl+axis, the host application scale, snaps, free ball."""
    print("-- Gizmo parity: Ctrl plane, Maya scale, snaps ------------")
    tonicLib = mods["tonicLib"]
    tonicGizmo = mods["tonicGizmo"]
    tonicGizmoSettings = mods["tonicGizmoSettings"]

    # G07: Ctrl on an axis moves in the plane PERPENDICULAR to it, read on
    # every sample.  An oblique camera, so the x = const plane is not
    # edge-on and the ray/plane intersection is the real one.
    dll, session, state, loop = newTube(mods)
    oblique = lookAtCamera(mods["tonicCamera"], (6.0, 7.0, 3.0))
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    origin = oblique.worldToPixels((0.0, 0.0, -5.0))
    click(mods, session, oblique, loop, origin[0], origin[1])
    session.pickFn = lambda mask, x, y: None
    gizmo = loop._gizmo
    uTip = oblique.worldToPixels((0.5 * gizmo.sizeWorld, 0.0, -5.0))
    check(gizmo.handleAt(oblique, uTip[0], uTip[1]) == tonicGizmo.HANDLE_U,
          "the U axis is under its own midpoint")
    loop.press(sample(mods, session, oblique, uTip[0], uTip[1]))
    loop.move(sample(mods, session, oblique, uTip[0] + 30.0, uTip[1] + 20.0))
    cv = dll.centers[0][1]
    check(cv[0] > 0.05 and near(cv[1], 0.0) and near(cv[2], -5.0),
          "a plain axis drag moves along X only (%r)" % (cv,))
    loop.move(sample(mods, session, oblique, uTip[0] + 30.0, uTip[1] + 20.0,
                     ("ctrl",)))
    cv = dll.centers[0][1]
    check(near(cv[0], 0.0, 1e-5) and
          (abs(cv[1]) > 1e-3 or abs(cv[2] + 5.0) > 1e-3),
          "Ctrl mid-drag: the perpendicular plane, no X at all (%r)" % (cv,))
    loop.move(sample(mods, session, oblique, uTip[0] + 30.0, uTip[1] + 21.0))
    cv = dll.centers[0][1]
    check(cv[0] > 0.05 and near(cv[1], 0.0, 1e-5) and
          near(cv[2], -5.0, 1e-5),
          "letting Ctrl go returns to the axis on the next sample (%r)"
          % (cv,))
    loop.release(sample(mods, session, oblique, uTip[0] + 30.0,
                        uTip[1] + 21.0))

    # G11: Step Snap (sticky, or J held) is a RELATIVE step in the gizmo's
    # axes; X held lands the pivot on the world grid.
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    click(mods, session, cam, loop, 200.0, 150.0)
    session.pickFn = lambda mask, x, y: None
    settings = tonicGizmoSettings.settingsFor(state)
    settings.For("move").stepSize = 0.25
    settings.For("move").stepSnap = True
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 237.0, 150.0))
    check(near(dll.centers[0][1][0], 0.25),
          "Step Snap: 0.37 of travel moves one 0.25 step (%r)"
          % (dll.centers[0][1],))
    loop.release(sample(mods, session, cam, 237.0, 150.0))
    settings.For("move").stepSnap = False
    loop.press(sample(mods, session, cam, 225.0, 150.0))
    loop.move(sample(mods, session, cam, 287.0, 150.0, ("stepSnap",)))
    check(near(dll.centers[0][1][0], 0.75),
          "J held: 0.62 of travel snaps to 0.5 more (%r)"
          % (dll.centers[0][1],))
    loop.move(sample(mods, session, cam, 287.0, 150.0))
    check(near(dll.centers[0][1][0], 0.87),
          "releasing J un-snaps at once (%r)" % (dll.centers[0][1],))
    loop.move(sample(mods, session, cam, 287.0, 150.0, ("grid",)))
    check(near(dll.centers[0][1][0], 1.0) and near(dll.centers[0][1][1], 0.0)
          and near(dll.centers[0][1][2], -5.0),
          "X held lands the pivot on the world grid (%r)"
          % (dll.centers[0][1],))
    loop.release(sample(mods, session, cam, 287.0, 150.0, ("grid",)))

    # G08: a DCC scale -- through the pivot mirrors, unless Prevent
    # Negative Scale clamps.  A whole tube scales about its root.
    def scaleDrag(prevent):
        dll, session, state, loop = newTube(mods)
        state.transformTool = "scale"
        tonicGizmoSettings.settingsFor(state).For("scale") \
            .preventNegativeScale = prevent
        session.select(tonicLib.TONIC_PICK_TUBE_VERT, [0], [-1], [-1], 0)
        loop.refreshGizmo(cam)
        pivot = cam.worldToPixels(loop._gizmo.origin)
        tipY = cam.worldToPixels((0.0, -1.0 + loop._gizmo.sizeWorld,
                                  -5.0))[1]
        half = (pivot[0], 0.5 * (pivot[1] + tipY))
        check(loop._gizmo.handleAt(cam, half[0], half[1]) ==
              tonicGizmo.HANDLE_V, "the Scale V axis is under its midpoint")
        loop.press(sample(mods, session, cam, half[0], half[1]))
        through = (pivot[0], pivot[1] + (pivot[1] - half[1]))
        loop.move(sample(mods, session, cam, through[0], through[1]))
        centers = [tuple(p) for p in dll.centers[0]]
        loop.release(sample(mods, session, cam, through[0], through[1]))
        return centers

    mirrored = scaleDrag(False)
    check(near(mirrored[1][1], -2.0) and near(mirrored[2][1], -3.0) and
          near(mirrored[0][1], -1.0),
          "dragging V through the pivot mirrors the tube (factor -1, %r)"
          % (mirrored,))
    clamped = scaleDrag(True)
    check(near(clamped[1][1], -1.0, 1e-3) and clamped[1][1] > -1.0 and
          near(clamped[2][1], -1.0, 1e-3),
          "Prevent Negative Scale clamps at MIN_SCALE_FACTOR (%r)"
          % (clamped,))

    # G11 rotate: the swept angle in whole steps; G23: the free ball.
    dll, session, state, loop = newTube(mods)
    state.transformTool = "rotate"
    session.select(tonicLib.TONIC_PICK_TUBE_VERT, [0], [-1], [-1], 0)
    loop.refreshGizmo(cam)
    free = [record for record in loop._gizmo.screenHandles(cam)
            if record["kind"] == "free"]
    check(len(free) == 1, "Free Rotate on: the ball is offered")
    settings = tonicGizmoSettings.settingsFor(state)
    settings.For("rotate").freeRotate = False
    loop.refreshGizmo(cam)
    free = [record for record in loop._gizmo.screenHandles(cam)
            if record["kind"] == "free"]
    check(not free and loop._gizmo.handleAt(
        cam, *cam.worldToPixels(loop._gizmo.origin)[:2]) !=
        tonicGizmo.HANDLE_FREE,
          "Free Rotate off: no ball to draw or grab")
    settings.For("rotate").stepSnap = True
    pivot = cam.worldToPixels(loop._gizmo.origin)
    rx = 0.85 * loop._gizmo.sizeWorld * 100.0
    ry = 0.85 * loop._gizmo.sizeWorld * 75.0

    def onRing(degrees):
        return (pivot[0] + rx * math.cos(math.radians(degrees)),
                pivot[1] - ry * math.sin(math.radians(degrees)))

    press = onRing(45.0)
    check(loop._gizmo.handleAt(cam, press[0], press[1]) ==
          tonicGizmo.HANDLE_W, "45 degrees round the W ring picks W")
    loop.press(sample(mods, session, cam, press[0], press[1]))
    loop.move(sample(mods, session, cam, *onRing(82.0)))
    snapped = loop._snappedDegrees
    check(snapped is not None and abs(snapped) > 1.0 and
          near(math.fmod(abs(snapped), 15.0), 0.0),
          "Step Snap turns in whole 15 degree steps (%r)" % (snapped,))
    check(session.statuses and "Rotate" in session.statuses[-1] and
          "°" in session.statuses[-1] and
          "step 15" in session.statuses[-1],
          "and the readout says so (%r)" % (session.statuses[-1:],))
    # The wedge ends where the target ended (RigExec _DisplayAngle): the
    # snapped angle, not the raw 37 degree sweep the hand made.
    gizmo = loop._gizmo
    wedge = gizmo.pieSlice()
    expected = mods["tonicGizmoScreen"].PiePolygon(
        gizmo._pressHandle, gizmo.startParameter, snapped)
    check(abs(gizmo.dragAngle() - snapped) > 0.5 and
          near(gizmo.displayAngle(), snapped) and wedge is not None and
          len(wedge[0]) == len(expected) and
          all(near(wedge[0][i][0], expected[i][0]) and
              near(wedge[0][i][1], expected[i][1])
              for i in range(len(expected))),
          "the rotation wedge draws the snapped %r degrees, not the raw "
          "%.2f" % (snapped, gizmo.dragAngle()))
    loop.release(sample(mods, session, cam, *onRing(82.0)))
    check(gizmo.displayAngle() == 0.0 and gizmo.pieSlice() is None,
          "and the release forgets it")


def testGizmoPriority(mods):
    print("-- Gizmo: a handle wins the press, hover prehighlights ----")
    tonicLib = mods["tonicLib"]
    tonicGizmo = mods["tonicGizmo"]
    tonicGizmoScreen = mods["tonicGizmoScreen"]
    cam = orthoCamera(mods["tonicCamera"])
    cv1 = {"kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
           "subSubId": -1}
    cv2 = {"kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 2,
           "subSubId": -1}
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: cv1
    click(mods, session, cam, loop, 200.0, 150.0)
    gizmo = loop._gizmo
    # The gizmo sits on CV 1 at (200, 150); its U axis runs 90 px right.
    # An UNSELECTED CV 2 answers every pick from here on, standing in for
    # a dot drawn under the axis.
    session.pickFn = lambda mask, x, y: cv2
    check(gizmo.handleAt(cam, 260.0, 150.0) == tonicGizmo.HANDLE_U,
          "(260, 150) is on the U axis")

    # Hover: the handle prehighlights and hides the component hover.
    session.hoverItems = []
    loop.hover(sample(mods, session, cam, 260.0, 150.0))
    check(gizmo.hoverHandle == tonicGizmo.HANDLE_U,
          "hovering the axis prehighlights HANDLE_U (%r)" % gizmo.hoverHandle)
    check(session.hoverItems and session.hoverItems[-1] == (0, -1, -1, -1),
          "and clears the component hover under it (%r)"
          % (session.hoverItems[-1:],))
    records = gizmo.screenHandles(cam)
    hovered = [r["handle"] for r in records if r["hovered"]]
    check(hovered == [tonicGizmo.HANDLE_U],
          "exactly the U record is marked hovered (%r)" % (hovered,))
    uRecord = [r for r in records if r["handle"] == tonicGizmo.HANDLE_U][0]
    check(uRecord["color"] == tonicGizmo.HOVER_COLOR,
          "in the RigExec hover colour (%r)" % (uRecord["color"],))
    loop.hover(sample(mods, session, cam, 330.0, 40.0))
    check(gizmo.hoverHandle == tonicGizmo.HANDLE_NONE and
          session.hoverItems[-1][:2] == (tonicLib.TONIC_PICK_CENTER_CV, 0),
          "off the gizmo the component hover comes back (%r)"
          % (session.hoverItems[-1:],))

    # Press: the handle wins over the unselected CV under it.
    dll.reset()
    session.events = []
    picks = len(session.picks)
    loop.press(sample(mods, session, cam, 260.0, 150.0))
    check(loop._dragging and gizmo.activeHandle == tonicGizmo.HANDLE_U,
          "a press on the axis over an unselected CV drags the axis")
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "and selects nothing (%r)"
          % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))
    check(len(session.picks) == picks,
          "the handle won before any component query")
    loop.move(sample(mods, session, cam, 310.0, 170.0))
    step = dll.argsOf("Tonic_MoveTubeCenterCV")
    check(len(step) == 1 and step[0][:2] == (0, 1) and
          near(step[0][2], 0.5) and near(step[0][3], 0.0),
          "the drag moves CV 1 along U only (%r)" % (step,))
    loop.release(sample(mods, session, cam, 310.0, 170.0))
    check(("end", None) in session.events and not session.gestureStack,
          "one sealed bracket (%r)" % (session.events,))

    # G04: the dragged handle stays remembered (yellow) after the release.
    check(gizmo.selectedHandle == tonicGizmo.HANDLE_U and
          gizmo.activeHandle == tonicGizmo.HANDLE_NONE,
          "the last-dragged handle persists after the release")
    uRecord = [r for r in gizmo.screenHandles(cam)
               if r["handle"] == tonicGizmo.HANDLE_U][0]
    check(uRecord["selected"] and uRecord["color"] ==
          tonicGizmo.ACTIVE_COLOR,
          "and its record reads selected, in yellow (%r)" % (uRecord,))

    # G04: a middle drag anywhere repeats it, no hit test.
    dll.reset()
    session.events = []
    check(gizmo.handleAt(cam, 40.0, 280.0) == tonicGizmo.HANDLE_NONE,
          "(40, 280) is far from the gizmo")
    check(loop.press(sample(mods, session, cam, 40.0, 280.0, ("middle",))),
          "a middle press in open space is claimed")
    check(loop._dragging and gizmo.activeHandle == tonicGizmo.HANDLE_U,
          "it repeats the remembered U handle")
    loop.move(sample(mods, session, cam, 80.0, 250.0, ("middle",)))
    step = dll.argsOf("Tonic_MoveTubeCenterCV")
    check(len(step) == 1 and near(step[0][2], 0.4) and near(step[0][3], 0.0),
          "moving CV 1 along U by the travel (%r)" % (step,))
    loop.release(sample(mods, session, cam, 80.0, 250.0, ("middle",)))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "a middle drag never selects")
    state.transformTool = "rotate"
    loop._placeGizmo(cam)
    check(gizmo.selectedHandle == tonicGizmo.HANDLE_NONE and
          not loop.press(sample(mods, session, cam, 40.0, 280.0,
                                ("middle",))),
          "another manipulator forgets it: the middle press is declined")
    state.transformTool = "move"
    loop._placeGizmo(cam)

    # Shift over a handle drags too (a DCC); Shift off it is a marquee.
    dll.reset()
    uTip = cam.worldToPixels(gizmo.axisEndpoint(tonicGizmo.HANDLE_U))
    loop.press(sample(mods, session, cam, uTip[0] - 20.0, uTip[1],
                      ("shift",)))
    check(loop._dragging and loop._marquee is None and
          gizmo.activeHandle == tonicGizmo.HANDLE_U,
          "Shift on the axis starts the axis drag, not a marquee")
    loop.cancel()

    # Off every handle the same unselected CV is an ordinary pick.
    check(gizmo.handleAt(cam, 330.0, 40.0) == tonicGizmo.HANDLE_NONE,
          "(330, 40) is off every handle")
    loop.press(sample(mods, session, cam, 330.0, 40.0))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 2, -1)], "off the gizmo the press selects CV 2 (%r)"
          % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))
    check(gizmo.activeHandle in (tonicGizmo.HANDLE_NONE,
                                 tonicGizmo.HANDLE_CENTER),
          "with no axis drag (only the Move tweak's centre) (%r)"
          % gizmo.activeHandle)
    loop.release(sample(mods, session, cam, 330.0, 40.0))

    # G02: the tolerance is logical pixels times the device pixel ratio,
    # and the vendored HitTest resolves by kind before distance.
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: cv1
    click(mods, session, cam, loop, 200.0, 150.0)
    gizmo = loop._gizmo
    check(gizmo.handleAt(cam, 260.0, 164.0) == tonicGizmo.HANDLE_NONE,
          "14 px off the U axis misses at ratio 1")
    check(gizmo.handleAt(cam, 260.0, 164.0, pixelRatio=2.0) ==
          tonicGizmo.HANDLE_U, "and hits at ratio 2")
    hiDpi = mods["tonicCamera"].TonicCamera(cam.viewProj, 400, 300, 2.0)
    check(gizmo.handleAt(hiDpi, 260.0, 164.0) == tonicGizmo.HANDLE_U,
          "the ratio comes from the resolved camera too")
    check(gizmo.handleAt(cam, 204.0, 146.0) == tonicGizmo.HANDLE_CENTER,
          "the centre outranks the axes that start inside it")
    handles = gizmo.handles(cam)
    wAxis = [h for h in handles if h.handleId == tonicGizmo.HANDLE_W]
    check(wAxis and not wAxis[0].grabbable,
          "the view-aligned W axis is foreshortened, so ungrabbable")
    hit = tonicGizmoScreen.HitTest(handles, 200.0, 150.0, 8.0)
    check(hit is not None and hit.kind == "center",
          "the vendored HitTest takes kind order first (%r)" % (hit,))


def testTweakDrag(mods):
    print("-- Center: tweak -- select and move in one gesture --------")
    tonicLib = mods["tonicLib"]
    cam = orthoCamera(mods["tonicCamera"])
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    check(not loop._gizmo.visible, "nothing is selected yet")
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 250.0, 150.0))
    step = dll.argsOf("Tonic_MoveTubeCenterCV")
    check(len(step) == 1 and step[0][:2] == (0, 1) and
          near(step[0][2], 0.5) and near(step[0][3], 0.0),
          "the press that selects CV 1 also moves it 50 px (%r)" % (step,))
    loop.release(sample(mods, session, cam, 250.0, 150.0))
    check(session.events.count(("begin", "Tube center")) == 1 and
          session.events.count(("end", None)) == 1 and
          ("cancel", None) not in session.events and
          not session.gestureStack,
          "in exactly one sealed undo bracket (%r)" % (session.events,))
    check(("enqueueCommit", None) in session.events,
          "and the tweak commits like any drag")
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "CV 1 stays selected")

    # Scale and Rotate do not tweak: the press is a plain selection.
    dll, session, state, loop = newTube(mods)
    state.transformTool = "scale"
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 2,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 75.0))
    check(not loop._dragging and not session.gestureStack,
          "with Scale a selecting press holds no drag")
    loop.release(sample(mods, session, cam, 200.0, 75.0))


def testClickAndMiddleRepeatTruth(mods):
    """A plain click lights no handle; middle repeat needs a grabbable one.

    RigExec remembers only a handle a press hit (gizmoUI._OnPress), and
    its middle repeat refuses a remembered handle that is not grabbable.
    """
    print("-- Gizmo: clicks remember nothing, middle needs grabbable -")
    tonicLib = mods["tonicLib"]
    tonicGizmo = mods["tonicGizmo"]
    cam = orthoCamera(mods["tonicCamera"])
    cv1 = {"kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
           "subSubId": -1}
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: cv1
    gizmo = loop._gizmo
    # A plain Move click on a CV is a tweak press released without travel.
    click(mods, session, cam, loop, 200.0, 150.0)
    check(gizmo.visible and gizmo.selectedHandle == tonicGizmo.HANDLE_NONE,
          "a plain click selects but marks no handle as last dragged (%r)"
          % gizmo.selectedHandle)
    centre = [r for r in gizmo.screenHandles(cam)
              if r["handle"] == tonicGizmo.HANDLE_CENTER]
    check(centre and not centre[0]["selected"] and
          centre[0]["color"] != tonicGizmo.ACTIVE_COLOR,
          "so the centre square is not yellow (%r)" % (centre[:1],))
    check(not loop.middleRepeatAvailable(cam) and
          not loop.press(sample(mods, session, cam, 40.0, 280.0,
                                ("middle",))),
          "and a middle press has nothing to repeat")

    # A real axis drag is remembered; a later click keeps THAT one.
    session.pickFn = lambda mask, x, y: None
    loop.press(sample(mods, session, cam, 260.0, 150.0))
    loop.move(sample(mods, session, cam, 290.0, 150.0))
    loop.release(sample(mods, session, cam, 290.0, 150.0))
    check(gizmo.selectedHandle == tonicGizmo.HANDLE_U,
          "an axis drag is remembered (%r)" % gizmo.selectedHandle)
    session.pickFn = lambda mask, x, y: cv1
    origin = cam.worldToPixels(gizmo.origin)
    click(mods, session, cam, loop, origin[0] + 70.0, origin[1] + 60.0)
    check(gizmo.selectedHandle == tonicGizmo.HANDLE_U,
          "a later tweak click leaves the remembered U alone (%r)"
          % gizmo.selectedHandle)

    # The camera turns until U points at it: U is drawn dimmed and HitTest
    # skips it, so the middle repeat declines rather than drive it blind.
    edgeOn = lookAtCamera(mods["tonicCamera"],
                          (gizmo.origin[0] + 10.0, gizmo.origin[1],
                           gizmo.origin[2]), target=gizmo.origin)
    uBuilt = [h for h in gizmo.handles(edgeOn)
              if h.handleId == tonicGizmo.HANDLE_U]
    check(uBuilt and not uBuilt[0].grabbable,
          "looking down U, the U axis is ungrabbable")
    check(loop.middleRepeatAvailable(cam),
          "at the original camera the middle repeat is offered")
    check(not loop.middleRepeatAvailable(edgeOn) and
          not loop.press(sample(mods, session, edgeOn, 40.0, 280.0,
                                ("middle",))) and not loop._dragging,
          "edge-on, the middle press is declined, as RigExec does")
    check(loop.press(sample(mods, session, cam, 40.0, 280.0, ("middle",)))
          and gizmo.activeHandle == tonicGizmo.HANDLE_U,
          "back at the first camera it repeats U again")
    loop.cancel()

    # The Tube loop's click-or-band slop is logical pixels x the ratio
    # (parity G24): 3 physical px is a click on a 200 % display.
    hiDpi = mods["tonicCamera"].TonicCamera(cam.viewProj, 400, 300, 2.0)
    check(tonicGizmo.clickSlopPixels(cam) == tonicGizmo.CLICK_SLOP_PX and
          tonicGizmo.clickSlopPixels(hiDpi) == 2.0 * tonicGizmo.CLICK_SLOP_PX,
          "the click slop scales with the display ratio")
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: None
    state.transformTool = "select"
    loop.press(sample(mods, session, hiDpi, 100.0, 100.0))
    session.pickFn = lambda mask, x, y: cv1
    loop.release(sample(mods, session, hiDpi, 102.0, 101.0))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)],
          "a 3 physical px wobble at 2x is a click that picks the CV (%r)"
          % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))


def testWholeTubeTweak(mods):
    print("-- Whole tube: tweak -- select and move the body at once ---")
    tonicLib = mods["tonicLib"]
    tonicGizmo = mods["tonicGizmo"]
    TUBE = tonicLib.TONIC_PICK_TUBE_VERT
    cam = orthoCamera(mods["tonicCamera"])
    body = {"kind": TUBE, "id": 0, "subId": -1, "subSubId": -1}

    def bodyPicker(mask, _x, _y):
        return body if mask & TUBE else None

    # A plain Move press on an unselected body selects it and the same
    # gesture translates the whole tube (F8's counterpart of testTweakDrag).
    dll, session, state, loop = newTube(mods, "tube")
    session.pickFn = bodyPicker
    check(not loop._gizmo.visible, "Whole tube: nothing is selected yet")
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(session.readSelection(TUBE) == [(0, -1, -1)],
          "the body press selects tube 0 (%r)" % (session.readSelection(TUBE),))
    check(loop._dragging and
          loop._gizmo.activeHandle == tonicGizmo.HANDLE_CENTER and
          session.gestureStack == ["Tube tube"],
          "and holds a centre tweak drag in one bracket (%r)"
          % (session.gestureStack,))
    loop.move(sample(mods, session, cam, 250.0, 150.0))
    check(dll.argsOf("Tonic_TranslateTube") == [(0, 0.5, 0.0, 0.0)],
          "the same press-drag translates the whole tube 50 px (%r)"
          % (dll.argsOf("Tonic_TranslateTube"),))
    loop.release(sample(mods, session, cam, 250.0, 150.0))
    check(session.events.count(("begin", "Tube tube")) == 1 and
          session.events.count(("end", None)) == 1 and
          ("cancel", None) not in session.events and
          not session.gestureStack and
          ("enqueueCommit", None) in session.events,
          "one sealed undo step that commits (%r)" % (session.events,))
    check(session.readSelection(TUBE) == [(0, -1, -1)],
          "tube 0 stays selected after the tweak")

    # No travel: a plain selection, and no undo step (SS-02).
    dll, session, state, loop = newTube(mods, "tube")
    session.pickFn = bodyPicker
    click(mods, session, cam, loop, 200.0, 150.0)
    check(session.readSelection(TUBE) == [(0, -1, -1)] and
          not dll.argsOf("Tonic_TranslateTube") and
          ("cancel", None) in session.events and
          ("end", None) not in session.events and
          ("enqueueCommit", None) not in session.events and
          not session.gestureStack,
          "a no-travel body click selects with no move and no undo step "
          "(%r)" % (session.events,))
    check(loop._gizmo.visible, "and leaves the gizmo on the tube")

    # Ctrl on the centre handle, which sits on the selected body: a click
    # there keeps the table's meaning (Ctrl removes), not an empty drag.
    session.events = []
    check(loop._gizmo.handleAt(cam, 200.0, 150.0) ==
          tonicGizmo.HANDLE_CENTER, "(200, 150) is the centre handle")
    click(mods, session, cam, loop, 200.0, 150.0, ("ctrl",))
    check(session.readSelection(TUBE) == [] and
          ("end", None) not in session.events and
          not session.gestureStack and
          not dll.argsOf("Tonic_TranslateTube"),
          "Ctrl-click on the centre over the selected body removes the tube, "
          "no undo step (%r, %r)" % (session.readSelection(TUBE),
                                     session.events))

    # Ctrl off every handle over the selected body: the same click, no drag.
    click(mods, session, cam, loop, 200.0, 150.0)
    session.events = []
    check(loop._gizmo.handleAt(cam, 330.0, 40.0) == tonicGizmo.HANDLE_NONE,
          "(330, 40) is off every handle")
    loop.press(sample(mods, session, cam, 330.0, 40.0, ("ctrl",)))
    check(not loop._dragging and not session.gestureStack,
          "a Ctrl body press holds no drag")
    loop.release(sample(mods, session, cam, 330.0, 40.0, ("ctrl",)))
    check(session.readSelection(TUBE) == [] and
          ("end", None) not in session.events,
          "and Ctrl-click removes the tube (%r)"
          % (session.readSelection(TUBE),))

    # Shift off every handle: a toggle click (the reserved marquee path).
    loop.press(sample(mods, session, cam, 330.0, 40.0, ("shift",)))
    check(not loop._dragging and not session.gestureStack,
          "a Shift body press holds no drag")
    loop.release(sample(mods, session, cam, 330.0, 40.0, ("shift",)))
    check(session.readSelection(TUBE) == [(0, -1, -1)],
          "and Shift-click toggles the tube back in (%r)"
          % (session.readSelection(TUBE),))

    # Empty space is still a marquee; Scale's body press is a plain select.
    session.clearSelection()
    session.pickFn = lambda mask, x, y: None
    loop.press(sample(mods, session, cam, 380.0, 40.0))
    check(loop._marquee is not None and not loop._dragging and
          not session.gestureStack, "an empty Whole tube press is a marquee")
    loop.release(sample(mods, session, cam, 380.0, 40.0))
    dll, session, state, loop = newTube(mods, "tube")
    session.pickFn = bodyPicker
    state.transformTool = "scale"
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(session.readSelection(TUBE) == [(0, -1, -1)] and
          not loop._dragging and not session.gestureStack,
          "with Scale a body press selects and holds no drag")
    loop.release(sample(mods, session, cam, 200.0, 150.0))


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
    click(mods, session, cam, loop, 200.0, 150.0)
    dll.reset()
    session.events = []

    # The gizmo sits at pixel (200, 150); pressing there takes the free
    # (screen-plane) handle.
    claimed = loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(claimed and session.gestureStack == ["Tube center"],
          "the press on the handle opened one bracket (%r)"
          % (session.gestureStack,))
    check(len(session.picks) == 1,
          "the handle wins the press before any component query (%d picks)"
          % len(session.picks))
    check(dll.argsOf("Tonic_SetPreviewFraction") == [(0.25,)],
          "the drag dropped the guides to the preview fraction (%r)"
          % (dll.argsOf("Tonic_SetPreviewFraction"),))

    loop.move(sample(mods, session, cam, 250.0, 150.0))
    loop.move(sample(mods, session, cam, 300.0, 150.0))
    check(len(session.picks) == 1,
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
    click(mods, session, cam, loop, 200.0, 150.0)
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
    click(mods, session, cam, loop, 200.0, 150.0)

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
    click(mods, session, cam, loop, 200.0, 150.0)
    dll.reset()
    loop.press(sample(mods, session, cam, 200.0, 150.0, ("ctrl",)))
    loop.move(sample(mods, session, cam, 300.0, 50.0))
    step = dll.argsOf("Tonic_MoveTubeCenterCV")[-1]
    check(near(step[2], 0.0) and step[3] > 0.5,
          "Ctrl constrains the drag to the root normal (%r)" % (step,))
    loop.release(sample(mods, session, cam, 300.0, 50.0))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(0, 1, -1)], "a Ctrl DRAG on the centre keeps the selection")

    # GZ-03: the centre handle covers the selected CV, so a Ctrl click
    # there (no travel) is that CV's selection click: it deselects, and
    # its empty bracket is cancelled rather than sealed.
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    click(mods, session, cam, loop, 200.0, 150.0)
    dll.reset()
    session.events = []
    check(loop._gizmo.handleAt(cam, 200.0, 150.0) ==
          mods["tonicGizmo"].HANDLE_CENTER,
          "the selected CV sits under the centre handle")
    click(mods, session, cam, loop, 200.0, 150.0, ("ctrl",))
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) == [],
          "a Ctrl click on the selected CV under the centre deselects it "
          "(%r)" % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))
    check(("cancel", None) in session.events and
          ("end", None) not in session.events and
          not dll.argsOf("Tonic_MoveTubeCenterCV") and
          not session.gestureStack,
          "and leaves no undo step and no move (%r)" % (session.events,))
    # A Shift click there is the component's Shift click (whatever
    # _selectItem makes of Shift), never a drag or an undo step.
    click(mods, session, cam, loop, 200.0, 150.0)
    session.events = []
    click(mods, session, cam, loop, 200.0, 150.0, ("shift",))
    check(not loop._dragging and not session.gestureStack and
          ("end", None) not in session.events and
          not dll.argsOf("Tonic_MoveTubeCenterCV"),
          "a Shift click on the centre is a selection click (%r)"
          % (session.events,))

    # Escape restores the model through the bracket and puts the soft
    # selection and the preview fraction back.
    dll, session, state, loop = newTube(mods)
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    click(mods, session, cam, loop, 200.0, 150.0)
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
    click(mods, session, cam, loop, 200.0, 150.0)
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
    click(mods, session, cam, loop, 200.0, 225.0)
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
    click(mods, session, cam, loop, 200.0, 150.0)
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
    click(mods, session, cam, loop, 200.0, 150.0)
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

    # GZ-06: Ctrl on the ring's centre used to pin the move to the tube's
    # root normal -- this ring's own w -- which a two dimensional chart
    # cannot take, so the ring barely moved.  In Ring it stays free.
    stage.calls = []
    centre = cam.worldToPixels(loop._gizmo.origin)
    loop.press(sample(mods, session, cam, centre[0], centre[1], ("ctrl",)))
    loop.move(sample(mods, session, cam, centre[0] + 50.0,
                     centre[1] - 50.0, ("ctrl",)))
    moved = stage.argsOf("moveSectionRing")
    check(moved and (abs(moved[-1][2]) > 1e-3 or abs(moved[-1][3]) > 1e-3),
          "a Ctrl diagonal drag on the ring centre moves it in its plane "
          "(%r)" % (moved,))
    loop.release(sample(mods, session, cam, centre[0] + 50.0,
                        centre[1] - 50.0, ("ctrl",)))


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
    click(mods, session, cam, loop, 200.0, 150.0)
    gizmo = mods["tonicGizmo"]
    # GZ-06: under Move the ringTRS gizmo offers its scale circle and its
    # W twist arrow, and the Hydra fallback receives the same whitelist.
    records = loop._gizmo.screenHandles(cam)
    handles = {record["handle"] for record in records}
    check(gizmo.HANDLE_RING in handles and gizmo.HANDLE_W in handles,
          "Ring Move exposes the scale circle and the W twist (%r)" % handles)
    mask = dll.gizmo.get("mask", 0xFFFFFFFF)
    check(mask != 0xFFFFFFFF and mask & (1 << gizmo.HANDLE_RING) and
          mask & (1 << gizmo.HANDLE_W) and
          not mask & (1 << gizmo.HANDLE_PLANE_YZ),
          "and the pushed record carries that mask (%#x)" % mask)
    ring = [record for record in records
            if record["handle"] == gizmo.HANDLE_RING]
    centre = cam.worldToPixels(loop._gizmo.origin)
    onRing = ring[0]["points"][6] if ring else (0.0, 0.0)
    check(loop._gizmo.handleAt(cam, onRing[0], onRing[1]) ==
          gizmo.HANDLE_RING, "a press on the circle's polyline grabs it")
    outward = (centre[0] + 1.5 * (onRing[0] - centre[0]),
               centre[1] + 1.5 * (onRing[1] - centre[1]))
    loop.press(sample(mods, session, cam, onRing[0], onRing[1]))
    loop.move(sample(mods, session, cam, outward[0], outward[1]))
    scaled = stage.argsOf("scaleSectionRing")
    check(scaled and scaled[-1][:2] == (0, 1) and scaled[-1][2] > 1.2,
          "dragging it outward calls the ring-scale ABI (%r)" % scaled)
    check("Scale 1.5" in loop.dragReadout(),
          "and the readout names the factor (%r)" % loop.dragReadout())
    loop.release(sample(mods, session, cam, outward[0], outward[1]))
    check(not session.gestureStack, "the ring-scale bracket closed")
    wTip = cam.worldToPixels(loop._gizmo.axisEndpoint(gizmo.HANDLE_W))
    if loop._gizmo.handleAt(cam, wTip[0], wTip[1]) == gizmo.HANDLE_W:
        loop.press(sample(mods, session, cam, wTip[0], wTip[1]))
        loop.move(sample(mods, session, cam, wTip[0] + 60.0, wTip[1]))
        check(stage.argsOf("twistSectionRing"),
              "dragging the W arrow twists the ring through its ABI")
        loop.release(sample(mods, session, cam, wTip[0] + 60.0, wTip[1]))
    else:
        check(False, "the W twist arrow is grabbable at its tip")
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
    click(mods, session, cam, loop, 200.0, 150.0)
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

    # Every removal refused (a tube at its 2-CV / 2-ring minimum): no
    # empty "Tube delete" step, the selection stays, nothing committed.
    realDelete, realRemove = stage.deleteCenterCV, stage.removeSectionRing

    def refuse(*_args):
        raise RuntimeError("DeleteTubeCenterCV: need >= 2 CVs left")
    stage.deleteCenterCV = refuse
    stage.removeSectionRing = refuse
    try:
        session.select(tonicLib.TONIC_PICK_CENTER_CV, [0], [1], [-1],
                       tonicLib.TONIC_SELECT_SET)
        session.events = []
        check(not loop.deleteSelection(),
              "a Delete whose every removal is refused is not a success")
        names = [name for name, _a in session.events]
        check("cancel" in names and "end" not in names and
              "enqueueCommit" not in names and not session.gestureActive,
              "and cancels its bracket instead of sealing an empty step "
              "(%r)" % (names,))
        check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),
              "the refused CV stays selected")
        check(session.statuses and "nothing removed" in
              session.statuses[-1] and "2 CVs" in session.statuses[-1],
              "the status gives the refusal (%r)" % session.statuses[-1:])

        # A refused Begin: nothing runs and no foreign bracket is closed.
        stage.deleteCenterCV = realDelete
        before = len(stage.argsOf("deleteCenterCV"))
        realBegin = session.beginGesture
        session.beginGesture = lambda label: False
        session.events = []
        try:
            check(not loop.deleteSelection(),
                  "a refused Begin refuses the Delete")
        finally:
            session.beginGesture = realBegin
        names = [name for name, _a in session.events]
        check(len(stage.argsOf("deleteCenterCV")) == before and
              "end" not in names and "cancel" not in names,
              "without removing anything or closing a bracket (%r)"
              % (names,))
    finally:
        stage.deleteCenterCV, stage.removeSectionRing = realDelete, \
            realRemove

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


def testComponentConversion(mods):
    print("-- GZ-04: F8-F11 convert the selection ------------------")
    tonicLib = mods["tonicLib"]
    TUBE = tonicLib.TONIC_PICK_TUBE_VERT
    CENTER = tonicLib.TONIC_PICK_CENTER_CV
    RING = tonicLib.TONIC_PICK_SECTION_RING
    SECTION = tonicLib.TONIC_PICK_SECTION_CV
    stage = FakeStage()
    dll, session, state, loop = newTube(mods, "section", stage)
    # Three rings on three centre CVs: the fake stations ring r at
    # t = 0.5 r, so ring r hangs on CV round(t * 2) = r.
    dll.sectionCount = 3
    cam = orthoCamera(mods["tonicCamera"])
    loop._placeGizmo(cam)

    def selection():
        return {kind: session.readSelection(kind)
                for kind in (TUBE, CENTER, RING, SECTION)
                if session.readSelection(kind)}

    session.select(SECTION, [0], [1], [3], tonicLib.TONIC_SELECT_SET)
    status = loop.setSubMode("ring")
    check(selection() == {RING: [(0, 1, -1)]},
          "F10: a section CV becomes its ring %r" % (selection(),))
    check("converted 1 item(s)" in status,
          "and the status line says what happened (%r)" % status)
    check(loop._gizmo.visible, "the gizmo follows the converted ring")
    loop.setSubMode("center")
    check(selection() == {CENTER: [(0, 1, -1)]},
          "F9: the ring becomes the centre CV that owns it %r"
          % (selection(),))
    loop.setSubMode("tube")
    check(selection() == {TUBE: [(0, -1, -1)]},
          "F8: the centre CV becomes its whole tube %r" % (selection(),))

    # Whole tube -> a component kind keeps the tube as the owner set, and
    # component picks answer for that tube only.
    loop.setSubMode("center")
    check(selection() == {TUBE: [(0, -1, -1)]},
          "F9 after a whole tube keeps it selected as the owner set %r"
          % (selection(),))
    other = {"kind": CENTER, "id": 1, "subId": 0, "subSubId": -1}
    own = {"kind": CENTER, "id": 0, "subId": 2, "subSubId": -1}
    session.pickFn = lambda mask, x, y: other
    check(loop._componentItem(sample(mods, session, cam, 10.0, 10.0))
          is None, "a component of another tube is ignored while owners exist")
    session.pickFn = lambda mask, x, y: own
    check(loop._componentItem(sample(mods, session, cam, 10.0, 10.0)) == own,
          "the owner tube's own components still answer")
    loop.setSubMode("ring")
    check(selection() == {TUBE: [(0, -1, -1)]} and not loop._gizmo.visible,
          "F10 keeps the owner set and hides a gizmo that has no ring to drag")
    loop.setSubMode("tube")
    session.clearSelection()
    session.pickFn = lambda mask, x, y: other
    check(loop.setSubMode("center") and
          loop._componentItem(sample(mods, session, cam, 10.0, 10.0))
          == other, "with no owner set every tube's components answer")

    # Down the ladder: a centre CV -> the rings stationed on it -> their
    # section CVs.
    session.select(CENTER, [0], [2], [-1], tonicLib.TONIC_SELECT_SET)
    loop.setSubMode("ring")
    check(selection() == {RING: [(0, 2, -1)]},
          "F10 from a centre CV takes the ring stationed on it %r"
          % (selection(),))
    status = loop.setSubMode("section")
    check(selection() == {SECTION: [(0, 2, slot) for slot in range(8)]},
          "F11 from a ring takes every one of its section CVs %r"
          % (selection(),))
    check("converted 8 item(s)" in status, "counted (%r)" % status)
    loop.setSubMode("section")
    check(len(session.readSelection(SECTION)) == 8,
          "choosing the current kind again changes nothing")
    session.clearSelection()
    status = loop.setSubMode("center")
    check(status and "converted" not in status and not selection(),
          "an empty selection converts to nothing, quietly (%r)" % status)


def testActivateAndDoubleClick(mods):
    print("-- GZ-08: activate places, double-click grows ----------")
    tonicLib = mods["tonicLib"]
    TUBE = tonicLib.TONIC_PICK_TUBE_VERT
    CENTER = tonicLib.TONIC_PICK_CENTER_CV
    RING = tonicLib.TONIC_PICK_SECTION_RING
    SECTION = tonicLib.TONIC_PICK_SECTION_CV
    stage = FakeStage()
    dll, session, state, loop = newTube(mods, "center", stage)
    dll.sectionCount = 3
    cam = orthoCamera(mods["tonicCamera"])
    session.select(CENTER, [0], [2], [-1], tonicLib.TONIC_SELECT_SET)
    check(loop.activate() and loop._gizmo.visible,
          "entering Tube with a CV selected raises its gizmo at once")
    session.clearSelection()
    session.select(tonicLib.TONIC_PICK_GRAPH_NODE, [0], None, None,
                   tonicLib.TONIC_SELECT_SET)
    check(not loop.activate() and not loop._gizmo.visible,
          "a Graph-only selection raises no Tube gizmo")
    session.clearSelection()

    session.pickFn = lambda mask, x, y: (
        {"kind": CENTER, "id": 0, "subId": 1, "subSubId": -1}
        if mask & CENTER else None)
    check(loop.doubleClick(sample(mods, session, cam, 200.0, 150.0)) and
          session.readSelection(TUBE) == [(0, -1, -1)] and
          not session.readSelection(CENTER) and loop._gizmo.visible,
          "double-clicking a centre CV selects its whole tube (%r)"
          % (session.readSelection(TUBE),))
    check(dll.count("Tonic_SetActiveLevel") == 0 and
          dll.count("Tonic_EnterLevel") == 0,
          "and never navigates a level")

    loop.setSubMode("section")
    session.clearSelection()
    section = {"kind": SECTION, "id": 0, "subId": 1, "subSubId": 3}
    session.pickFn = lambda mask, x, y: section if mask & SECTION else None
    check(loop.doubleClick(sample(mods, session, cam, 200.0, 150.0)) and
          session.readSelection(RING) == [(0, 1, -1)] and
          not session.readSelection(SECTION),
          "double-clicking a section CV selects its ring (%r)"
          % (session.readSelection(RING),))

    loop.setSubMode("ring")
    session.clearSelection()
    check(loop.doubleClick(sample(mods, session, cam, 200.0, 150.0)) and
          session.readSelection(RING) ==
          [(0, 0, -1), (0, 1, -1), (0, 2, -1)],
          "double-clicking a ring selects every ring of its tube (%r)"
          % (session.readSelection(RING),))
    session.pickFn = lambda mask, x, y: None
    check(not loop.doubleClick(sample(mods, session, cam, 10.0, 10.0)),
          "a double-click over nothing is declined (a plain press replays)")


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


# ---------------------------------------------------------------------------
# SL-01: one selection-modifier table for every loop
# ---------------------------------------------------------------------------

class MatrixSession(FakeSession):
    """FakeSession whose bands select, as Tonic_SelectRect does.

    A band SET clears its mask and adds `bandHits`; ADD/TOGGLE combine
    them. Every mode an ABI call receives is kept, so the test can prove
    the Python-only REMOVE never reached one.
    """

    def __init__(self, dll, stage=None):
        super(MatrixSession, self).__init__(dll, stage)
        self.bandHits = {}
        self.abiModes = []

    def select(self, kind, ids, subIds=None, subSubIds=None, mode=0):
        self.abiModes.append(int(mode))
        if int(mode) == 0 and not ids:
            return True          # Tonic_SelectSet of nothing is a no-op
        return super(MatrixSession, self).select(kind, ids, subIds,
                                                 subSubIds, mode)

    def _band(self, kindMask, mode):
        self.abiModes.append(int(mode))
        if int(mode) == 0:
            self.clearSelection(kindMask)
        for kind, rows in self.bandHits.items():
            if kind & kindMask and rows:
                super(MatrixSession, self).select(
                    kind, [r[0] for r in rows], [r[1] for r in rows],
                    [r[2] for r in rows], 1 if int(mode) == 0 else mode)
        return True

    def selectRect(self, camera, x0, y0, x1, y1, kindMask, mode):
        self.rects.append((x0, y0, x1, y1, int(kindMask), int(mode)))
        return self._band(kindMask, mode)

    def selectPolygon(self, camera, points, kindMask, mode):
        self.polygons.append((tuple(points), int(kindMask), int(mode)))
        return self._band(kindMask, mode)

    def readSelection(self, kind):
        return sorted(self.dll.selection.get(int(kind), []))


# Start from {A, B} selected and C not; a click lands on A or C, a band
# covers A and C. Click none/Shift/Ctrl/Ctrl+Shift = SET/TOGGLE/REMOVE/ADD,
# band = SET/ADD/REMOVE/ADD (backlog "Selection-modifier convention").
MATRIX_MODIFIERS = ((), ("shift",), ("ctrl",), ("ctrl", "shift"))
MATRIX_CLICK = {
    ((), "A"): "A", ((), "C"): "C",
    (("shift",), "A"): "B", (("shift",), "C"): "ABC",
    (("ctrl",), "A"): "B", (("ctrl",), "C"): "AB",
    (("ctrl", "shift"), "A"): "AB", (("ctrl", "shift"), "C"): "ABC",
}
MATRIX_BAND = {(): "AC", ("shift",): "ABC", ("ctrl",): "B",
               ("ctrl", "shift"): "ABC"}


def runSelectionMatrix(label, names, reset, read, click, band):
    for mods in MATRIX_MODIFIERS:
        spelled = "+".join(mods) or "none"
        for target in ("A", "C"):
            reset()
            click(names[target], mods)
            want = sorted(names[c] for c in MATRIX_CLICK[(mods, target)])
            got = read()
            check(got == want, "%s: %s-click on %s -> %s (%r)"
                  % (label, spelled, target, MATRIX_CLICK[(mods, target)],
                     got))
        reset()
        band(mods)
        want = sorted(names[c] for c in MATRIX_BAND[mods])
        got = read()
        check(got == want, "%s: %s-band over A and C -> %s (%r)"
              % (label, spelled, MATRIX_BAND[mods], got))


def testTubeSelectionMatrix(mods):
    print("-- SL-01: Tube and Fill modifier matrix ------------------")
    tonicLib = mods["tonicLib"]
    cam = orthoCamera(mods["tonicCamera"])
    kind = tonicLib.TONIC_PICK_CENTER_CV
    names = {"A": (0, 0, -1), "B": (0, 1, -1), "C": (0, 2, -1)}
    for tool in ("select", "move"):
        dll = FakeDll()
        session = MatrixSession(dll)
        state = mods["TonicToolState"]()
        state.snapRadiusPx = 8.0
        state.tubeSubMode = "center"
        state.transformTool = tool
        loop = mods["tonicLoopsTube"].TubeLoop(session, state)
        session.bandHits = {kind: [names["A"], names["C"]]}

        def reset():
            dll.selection = {kind: [names["A"], names["B"]]}
            # The matrix is about the table, not about a gizmo handle
            # the previous row left over the click pixel (GZ-01).
            loop._gizmo.clear()

        def read():
            return session.readSelection(kind)

        def click(entry, modifiers):
            session.pickFn = (
                lambda mask, x, y: {"kind": kind, "id": entry[0],
                                    "subId": entry[1], "subSubId": -1}
                if mask & kind else None)
            loop.press(sample(mods, session, cam, 200.0, 150.0, modifiers))
            loop.release(sample(mods, session, cam, 200.0, 150.0,
                                modifiers))

        def band(modifiers):
            session.pickFn = lambda mask, x, y: None
            loop.press(sample(mods, session, cam, 20.0, 20.0, modifiers))
            loop.move(sample(mods, session, cam, 120.0, 120.0, modifiers))
            loop.release(sample(mods, session, cam, 140.0, 140.0,
                                modifiers))

        runSelectionMatrix("Tube/%s" % tool, names, reset, read, click, band)
        check(not session.gestureStack and
              ("end", None) not in session.events,
              "Tube/%s: no selection click left an undo step (%r)"
              % (tool, session.events))
        check(set(session.abiModes) <= {0, 1, 2},
              "Tube/%s: only real Tonic_Select* modes reached the ABI (%r)"
              % (tool, sorted(set(session.abiModes))))

    # A Ctrl band keeps the rest of the selection domain: the whole tube
    # selected beside the CVs is not cleared by a band that only removes.
    dll = FakeDll()
    session = MatrixSession(dll)
    state = mods["TonicToolState"]()
    state.tubeSubMode = "center"
    state.transformTool = "select"
    loop = mods["tonicLoopsTube"].TubeLoop(session, state)
    session.bandHits = {kind: [names["C"]]}
    dll.selection = {kind: [names["A"]]}
    loop.press(sample(mods, session, cam, 20.0, 20.0, ("shift",)))
    loop.move(sample(mods, session, cam, 120.0, 120.0, ("shift",)))
    loop.release(sample(mods, session, cam, 120.0, 120.0, ("shift",)))
    check(session.readSelection(kind) == [names["A"], names["C"]],
          "a Shift band adds the CV it covers (%r)"
          % (session.readSelection(kind),))
    loop.press(sample(mods, session, cam, 20.0, 20.0, ("ctrl",)))
    loop.move(sample(mods, session, cam, 120.0, 120.0, ("ctrl",)))
    loop.release(sample(mods, session, cam, 120.0, 120.0, ("ctrl",)))
    check(session.readSelection(kind) == [names["A"]],
          "a Ctrl band after a Shift band removes only the CV it covers "
          "(%r)" % (session.readSelection(kind),))

    # Ring: the band hits section vertices and the loop turns them into
    # their rings, so a Ctrl band must subtract rings, not vertices.
    ring = tonicLib.TONIC_PICK_SECTION_RING
    state.tubeSubMode = "ring"
    dll.selection = {ring: [(0, 0, -1), (0, 1, -1)]}
    session.bandHits = {tonicLib.TONIC_PICK_SECTION_CV: [(0, 1, 0),
                                                         (0, 1, 1)]}
    loop.press(sample(mods, session, cam, 20.0, 20.0, ("ctrl",)))
    loop.move(sample(mods, session, cam, 120.0, 120.0, ("ctrl",)))
    loop.release(sample(mods, session, cam, 120.0, 120.0, ("ctrl",)))
    check(session.readSelection(ring) == [(0, 0, -1)] and
          not session.readSelection(tonicLib.TONIC_PICK_SECTION_CV),
          "a Ctrl band in Ring removes the ring whose vertices it covers "
          "(%r)" % (dll.selection,))
    session.bandHits = {}
    loop.press(sample(mods, session, cam, 20.0, 20.0))
    loop.move(sample(mods, session, cam, 120.0, 120.0))
    loop.release(sample(mods, session, cam, 120.0, 120.0))
    check(not session.readSelection(ring),
          "a plain Ring band over nothing still replaces the rings")

    # Fill selects whole tubes by click and band through the same table.
    tube = tonicLib.TONIC_PICK_TUBE_VERT
    names = {"A": (0, -1, -1), "B": (1, -1, -1), "C": (2, -1, -1)}
    dll = FakeDll()
    session = MatrixSession(dll, FakeStage())
    state = mods["TonicToolState"]()
    state.snapRadiusPx = 8.0
    state.fillSubMode = "params"
    loop = mods["tonicLoopsFill"].FillLoop(session, state)
    session.bandHits = {tube: [names["A"], names["C"]]}

    def resetFill():
        dll.selection = {tube: [names["A"], names["B"]]}

    def readFill():
        return session.readSelection(tube)

    def clickFill(entry, modifiers):
        session.pickFn = (
            lambda mask, x, y: {"kind": tube, "id": entry[0], "subId": -1,
                                "subSubId": -1}
            if mask & tube else None)
        loop.press(sample(mods, session, cam, 200.0, 150.0, modifiers))
        loop.release(sample(mods, session, cam, 200.0, 150.0, modifiers))

    def bandFill(modifiers):
        session.pickFn = lambda mask, x, y: None
        loop.press(sample(mods, session, cam, 20.0, 20.0, modifiers))
        loop.move(sample(mods, session, cam, 120.0, 120.0, modifiers))
        loop.release(sample(mods, session, cam, 140.0, 140.0, modifiers))

    runSelectionMatrix("Fill", names, resetFill, readFill, clickFill,
                       bandFill)
    check(set(session.abiModes) <= {0, 1, 2},
          "Fill: only real Tonic_Select* modes reached the ABI (%r)"
          % sorted(set(session.abiModes)))


def testFillRampDrag(mods):
    print("-- Fill: the length-ramp drag -----------------------------")
    tonicLib = mods["tonicLib"]
    stage = FakeStage()
    dll, session, state, loop = newFill(mods, "preview", stage)
    cam = orthoCamera(mods["tonicCamera"])
    dll.previewFraction = 1.0
    state.previewFraction = 0.25
    state.freezeRoots = True
    # The kernels clamp a guide's length fraction to [0, 1], so "up is
    # longer" is only observable where the profile is below full length:
    # start from a uniform half-length ramp.
    dll.fill["profile"] = [0.0, 0.5, 1.0, 0.5]
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_TUBE_VERT, "id": 0, "subId": -1,
        "subSubId": -1}
    # x = 225 px is 0.25 world units right of the tube axis (x = 200 px):
    # half way to the wall of the 0.5-radius ring.
    check(loop.press(sample(mods, session, cam, 225.0, 150.0)),
          "a press on the tube starts a ramp drag")
    check(session.gestureStack == ["Fill length profile"],
          "inside one bracket (%r)" % (session.gestureStack,))
    check(dll.argsOf("Tonic_SetPreviewFraction") == [(0.25,)],
          "at the panel's preview fraction (%r)"
          % (dll.argsOf("Tonic_SetPreviewFraction"),))
    check(loop._ramp is not None and near(loop._ramp[1], 0.5) and
          near(loop._ramp[3], 0.5),
          "the knot is the radius under the press, r = 0.5, starting at "
          "the profile's 0.5 there (%r)" % (loop._ramp,))

    before = len(session.statuses)
    loop.move(sample(mods, session, cam, 225.0, 30.0))
    written = stage.argsOf("setFillParams")
    check(written, "the drag wrote fill params (%r)" % (written,))
    profile = written[-1][5]
    check(profile[:2] == [0.0, 0.5] and near(profile[2], 0.5) and
          near(profile[3], 1.0) and profile[4:] == [1.0, 0.5],
          "dragging up 120 px (half of RAMP_PIXELS) lifts the knot at "
          "r = 0.5 from 0.5 to full length (%r)" % (profile,))
    check(any("length ramp r=0.50: 1.00 (full length)" in text
              for text in session.statuses[before:]),
          "the readout names the radius and the length fraction (%r)"
          % (session.statuses[before:],))
    check(dll.argsOf("Tonic_RefillGuides")[-1] == (0.25,),
          "and refilled at preview density (%r)"
          % (dll.argsOf("Tonic_RefillGuides"),))
    count = len(written)
    loop.move(sample(mods, session, cam, 225.0, 10.0))
    check(len(stage.argsOf("setFillParams")) == count,
          "past full length the knot is pinned at 1 and nothing is "
          "rewritten (%r)" % (stage.argsOf("setFillParams")[count:],))
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

    # Down is shorter: the same knot drops toward 0 and never below it.
    stage = FakeStage()
    dll, session, state, loop = newFill(mods, "preview", stage)
    dll.fill["profile"] = [0.0, 0.5, 1.0, 0.5]
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_TUBE_VERT, "id": 0, "subId": -1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 225.0, 150.0))
    loop.move(sample(mods, session, cam, 225.0, 210.0))
    profile = stage.argsOf("setFillParams")[-1][5]
    check(near(profile[2], 0.5) and near(profile[3], 0.25),
          "dragging down 60 px drops the knot at r = 0.5 by a quarter (%r)"
          % (profile,))
    loop.move(sample(mods, session, cam, 225.0, 290.0))
    profile = stage.argsOf("setFillParams")[-1][5]
    check(near(profile[3], 0.0),
          "and it stops at 0, the shortest a guide can be (%r)" % (profile,))
    loop.release(sample(mods, session, cam, 225.0, 290.0))
    check(("end", None) in session.events,
          "the shortening drag is one undo step (%r)" % (session.events,))

    # A full-length ramp cannot grow: a drag up writes nothing and leaves
    # no undo step, and the readout says the guides are already full.
    stage = FakeStage()
    dll, session, state, loop = newFill(mods, "preview", stage)
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_TUBE_VERT, "id": 0, "subId": -1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 200.0, 60.0))
    loop.release(sample(mods, session, cam, 200.0, 60.0))
    check(not stage.argsOf("setFillParams") and
          ("end", None) not in session.events and
          ("cancel", None) in session.events and
          ("enqueueCommit", None) not in session.events,
          "up on a full-length ramp writes nothing and leaves no step (%r)"
          % (session.events,))
    check(any("r=0.00: 1.00 (full length)" in text
              for text in session.statuses),
          "the readout says the knot is already at full length (%r)"
          % (session.statuses,))

    # Up then back to the press height is a round trip: no undo step.
    stage = FakeStage()
    dll, session, state, loop = newFill(mods, "preview", stage)
    dll.fill["profile"] = [0.0, 0.5, 1.0, 0.5]
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_TUBE_VERT, "id": 0, "subId": -1,
        "subSubId": -1}
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    loop.move(sample(mods, session, cam, 200.0, 120.0))
    loop.move(sample(mods, session, cam, 200.0, 150.0))
    loop.release(sample(mods, session, cam, 200.0, 150.0))
    check(len(stage.argsOf("setFillParams")) == 2 and
          ("cancel", None) in session.events and
          ("end", None) not in session.events and
          ("enqueueCommit", None) not in session.events,
          "a drag that returns to its start cancels instead of keeping an "
          "empty step (%r)" % (session.events,))
    check(dll.argsOf("Tonic_RefillGuides")[-1] == (1.0,),
          "and still refills the restored model at full density (%r)"
          % (dll.argsOf("Tonic_RefillGuides"),))


class RefusingStage(FakeStage):
    """A stage library whose every per-tube fill write is refused."""

    def setFillParams(self, _ctx, tubeId, *args, **kwargs):
        self._record("setFillParams", (int(tubeId),))
        raise RuntimeError("Tonic_SetTubeFillParams: refused")


def testFillRampTargets(mods):
    print("-- Fill: the ramp edits the pressed tube, honestly ---------")
    tonicLib = mods["tonicLib"]
    tubeVert = tonicLib.TONIC_PICK_TUBE_VERT

    def twoTubeFill(stage):
        # Tube 1 stands one unit right of tube 0: its centers project to
        # x = 300 px, tube 0's to x = 200 px.
        dll, session, state, loop = newFill(mods, "preview", stage)
        dll.centers[1] = [(p[0] + 1.0, p[1], p[2]) for p in CENTERS]
        dll.previewFraction = 1.0
        state.previewFraction = 0.25
        # Half-length ramps on both tubes, so a drag up has room to grow
        # (the kernels clamp at full length).
        dll.fill["profile"] = [0.0, 0.5, 1.0, 0.5]
        stage.fill[1] = {"density": 8.0, "cvCount": 8, "seed": 3,
                         "edgeBias": 0.0, "profileCount": 4}
        session.pickFn = lambda mask, x, y: {
            "kind": tubeVert, "id": 1 if x > 250.0 else 0, "subId": -1,
            "subSubId": -1}
        return dll, session, state, loop

    cam = orthoCamera(mods["tonicCamera"])

    # Tube 0 is selected; the press lands on tube 1.
    stage = FakeStage()
    dll, session, state, loop = twoTubeFill(stage)
    dll.selection[tubeVert] = [(0, -1, -1)]
    check(loop.press(sample(mods, session, cam, 300.0, 150.0)),
          "a press on tube 1 is claimed")
    check(session.readSelection(tubeVert) == [(1, -1, -1)],
          "the press selected the tube under the cursor (%r)"
          % (session.readSelection(tubeVert),))
    before = len(session.statuses)
    loop.move(sample(mods, session, cam, 300.0, 30.0))
    written = [w[0] for w in stage.argsOf("setFillParams")]
    check(written == [1],
          "the ramp was written to tube 1 alone, not the old selection (%r)"
          % (written,))
    moveStatuses = session.statuses[before:]
    check(any("length ramp r=0.00: 1.00" in text for text in moveStatuses),
          "a status during the move reads the radius and length (%r)"
          % (moveStatuses,))
    loop.release(sample(mods, session, cam, 300.0, 30.0))
    check(("end", None) in session.events and
          ("enqueueCommit", None) in session.events,
          "the edited drag sealed its bracket and committed (%r)"
          % (session.events,))

    # A tube that is already selected keeps the multi-tube selection.
    stage = FakeStage()
    dll, session, state, loop = twoTubeFill(stage)
    dll.selection[tubeVert] = [(0, -1, -1), (1, -1, -1)]
    loop.press(sample(mods, session, cam, 300.0, 150.0))
    loop.move(sample(mods, session, cam, 300.0, 90.0))
    loop.release(sample(mods, session, cam, 300.0, 90.0))
    check(sorted(w[0] for w in stage.argsOf("setFillParams")) == [0, 1],
          "pressing a selected tube shapes the whole selection (%r)"
          % (stage.argsOf("setFillParams"),))

    # A click with no travel leaves no undo step and no lowered preview.
    stage = FakeStage()
    dll, session, state, loop = twoTubeFill(stage)
    loop.press(sample(mods, session, cam, 200.0, 150.0))
    check(near(dll.previewFraction, 0.25),
          "the press lowered the preview fraction for the drag")
    loop.move(sample(mods, session, cam, 200.0, 150.0))
    loop.release(sample(mods, session, cam, 200.0, 150.0))
    check(near(dll.previewFraction, 1.0),
          "a no-travel click restored the preview fraction (%r)"
          % dll.previewFraction)
    check(("cancel", None) in session.events and
          ("end", None) not in session.events,
          "and cancelled its gesture instead of sealing an empty step (%r)"
          % (session.events,))
    check(not stage.argsOf("setFillParams") and
          ("enqueueCommit", None) not in session.events and
          not dll.argsOf("Tonic_RefillGuides"),
          "a click writes, refills and commits nothing")
    check(not session.gestureStack, "no bracket is left open")

    # A refused write is reported once per gesture, not once per sample.
    stage = RefusingStage()
    dll, session, state, loop = twoTubeFill(stage)
    loop.press(sample(mods, session, cam, 300.0, 150.0))
    before = len(session.statuses)
    for y in (130.0, 110.0, 90.0, 70.0, 50.0):
        loop.move(sample(mods, session, cam, 300.0, y))
    check(len(stage.argsOf("setFillParams")) == 5,
          "every move tried the write (%r)" % (stage.argsOf("setFillParams"),))
    moveStatuses = session.statuses[before:]
    check(len(moveStatuses) == 1 and "refused" in moveStatuses[0],
          "five refused moves print one status (%r)" % (moveStatuses,))
    check(not dll.argsOf("Tonic_RefillGuides"),
          "a refused write does not refill as if it had landed")
    loop.release(sample(mods, session, cam, 300.0, 50.0))
    check(("cancel", None) in session.events and
          ("enqueueCommit", None) not in session.events,
          "a drag whose writes were all refused leaves no undo step (%r)"
          % (session.events,))

    # A failed bracket refuses the drag outright.
    stage = FakeStage()
    dll, session, state, loop = twoTubeFill(stage)
    session.beginGesture = lambda label: False
    loop.press(sample(mods, session, cam, 300.0, 150.0))
    check(loop._ramp is None and not loop._bracketOpen,
          "no drag starts when the gesture bracket is refused")
    check(not loop.move(sample(mods, session, cam, 300.0, 30.0)) and
          not stage.argsOf("setFillParams"),
          "and a move after it writes nothing")
    check(near(dll.previewFraction, 1.0),
          "the preview fraction was never lowered")

    # A guide names no tube: the click says so instead of guessing.
    stage = FakeStage()
    dll, session, state, loop = twoTubeFill(stage)
    dll.selection[tubeVert] = [(0, -1, -1)]
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_GUIDE, "id": 7, "subId": -1,
        "subSubId": -1}
    check(loop.press(sample(mods, session, cam, 300.0, 150.0)),
          "a press on a guide is claimed")
    check(loop._ramp is None and not session.gestureStack,
          "but starts no ramp on a guessed tube")
    check(session.statuses and "click the tube" in session.statuses[-1],
          "it tells the artist to click the tube (%r)"
          % (session.statuses[-1:],))
    check(session.readSelection(tubeVert) == [(0, -1, -1)],
          "and leaves the selection alone")

    # A strand over a tube: the tube is edited, at the strand's own root
    # radius (guide 7's root sits at |(0.3, 0.4)| = 0.5).
    roots = [(0.0, 0.0)] * 7 + [(0.3, 0.4)] + [(0.9, 0.0)] * 4

    def strandPick(mask, x, y):
        if mask & tonicLib.TONIC_PICK_GUIDE:
            return {"kind": tonicLib.TONIC_PICK_GUIDE, "id": 7, "subId": -1,
                    "subSubId": -1}
        return {"kind": tubeVert, "id": 0, "subId": -1, "subSubId": -1}

    for bias, want in ((0.0, 0.5), (0.5, 0.6)):
        stage = FakeStage()
        dll, session, state, loop = twoTubeFill(stage)
        dll.fill["edgeBias"] = bias
        guideRoots(dll, roots)
        session.pickFn = strandPick
        check(loop.press(sample(mods, session, cam, 200.0, 150.0)) and
              loop._ramp is not None and near(loop._ramp[1], want),
              "edge bias %+.1f: a strand press keys the knot at its root "
              "radius 0.5 remapped to %.2f (%r)" % (bias, want, loop._ramp))
        loop.cancel()

    # When the root census disagrees with the guide count the strand
    # cannot be trusted to name its root: the tube-body projection answers.
    stage = FakeStage()
    dll, session, state, loop = twoTubeFill(stage)
    guideRoots(dll, roots)
    dll.guideCount = len(roots) + 1
    session.pickFn = strandPick
    loop.press(sample(mods, session, cam, 240.0, 150.0))
    check(loop._ramp is not None and near(loop._ramp[1], 0.8),
          "a stale root census falls back to the body hit, 40 px off a "
          "50 px ring = 0.8 (%r)" % (loop._ramp,))
    loop.cancel()


def testFillActivateRefill(mods):
    print("-- Fill: opening Fill grows guides on a bare groom --------")
    dll, session, state, loop = newFill(mods, "params")
    dll.guideCount = 0
    loop.activate()
    check(dll.argsOf("Tonic_RefillGuides") == [(1.0,)],
          "a groom with no guides is refilled at full density (%r)"
          % (dll.argsOf("Tonic_RefillGuides"),))
    dll, session, state, loop = newFill(mods, "params")
    dll.guideCount = 64
    loop.activate()
    check(not dll.argsOf("Tonic_RefillGuides"),
          "a groom that has guides is left alone")


def testRampMaths(mods):
    print("-- Fill: the ramp knot rule -------------------------------")
    tonicLoopsFill = mods["tonicLoopsFill"]
    check(tonicLoopsFill.snapKnot(0.42) == 0.4 and
          tonicLoopsFill.snapKnot(-3.0) == 0.0 and
          tonicLoopsFill.snapKnot(9.0) == 1.0,
          "knots snap to the grid and stay inside [0, 1]")
    first = tonicLoopsFill.setProfileKnot([], 0.5, 0.6)
    check(first == [0.0, 1.0, 0.5, 0.6, 1.0, 1.0],
          "the first edit lays the ends down too, so the tube keeps its "
          "shape away from the knot (%r)" % (first,))
    again = tonicLoopsFill.setProfileKnot(first, 0.5, 0.3)
    check(again == [0.0, 1.0, 0.5, 0.3, 1.0, 1.0],
          "editing the same knot replaces it (%r)" % (again,))
    clamped = tonicLoopsFill.setProfileKnot(first, 0.5, 99.0)
    check(tonicLoopsFill.RAMP_MAX == 1.0 and near(clamped[3], 1.0),
          "and the value is clamped to full length, the kernels' ceiling "
          "(%r)" % (clamped,))
    floor = tonicLoopsFill.setProfileKnot(first, 0.5, -3.0)
    check(tonicLoopsFill.RAMP_MIN == 0.0 and near(floor[3], 0.0),
          "and never below zero length (%r)" % (floor,))
    other = tonicLoopsFill.setProfileKnot(first, 0.25, 0.5)
    check([other[i] for i in range(0, len(other), 2)] == [0.0, 0.25, 0.5,
                                                          1.0],
          "a new knot sorts into place (%r)" % (other,))
    position = tonicLoopsFill.profilePosition
    check(near(position(0.5, 0.0), 0.5) and
          near(position(0.5, 0.5), 0.5 ** 0.75) and
          near(position(0.5, -1.0), 0.5 ** 1.5) and
          position(0.0, 1.0) == 0.0 and near(position(3.0, 0.0), 1.0),
          "a radius maps to the ramp through the kernels' r^(1-bias/2)")
    ring = [(0.5 * math.cos(a), 0.5 * math.sin(a))
            for a in (2.0 * math.pi * i / 8.0 for i in range(8))]
    check(near(tonicLoopsFill.chartRadius(ring), 0.5) and
          near(tonicLoopsFill.chartRadius([(u + 3.0, v) for u, v in ring]),
               0.5),
          "the chart radius is the mean distance from the centroid")
    across = tonicLoopsFill.rayAxisDistance((0.3, 0.0, 5.0), (0.0, 0.0, -1.0),
                                            (0.0, 2.0, 0.0), (0.0, 1.0, 0.0))
    down = tonicLoopsFill.rayAxisDistance((0.3, 0.4, 5.0), (0.0, 0.0, -1.0),
                                          (0.0, 0.0, 0.0), (0.0, 0.0, 2.0))
    check(near(across, 0.3) and near(down, 0.5),
          "the cursor radius is its offset across the tube and its distance "
          "from the axis looking down it (%r, %r)" % (across, down))


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
    loop._gizmo.scaleFactor = lambda _c, _x, _y, **_kw: 2.0
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


def testToolOrientationDefaults(mods):
    print("-- parity G16: per-tool Axis Orientation ------------------")
    settingsMod = mods["tonicGizmoSettings"]
    dll, session, state, loop = newTube(mods)
    check(state.transformTool == "move" and
          state.transformOrientation == "world",
          "a fresh session starts on Move in World")
    check(loop.setTransformTool("rotate") and
          state.transformOrientation == "tube" and
          loop.transformOrientation() == "tube",
          "Rotate defaults to Local, the tube frame (%r)"
          % state.transformOrientation)
    check(loop.toggleOrientation() == "world" and
          state.transformOrientation == "world",
          "L flips the live tool (Rotate) to World")
    loop.setTransformTool("scale")
    check(state.transformOrientation == "world",
          "Scale defaults to World (%r)" % state.transformOrientation)
    loop.setTransformTool("move")
    loop.setTransformOrientation("screen")
    loop.setTransformTool("rotate")
    check(state.transformOrientation == "world",
          "Rotate remembers the World its L chose (%r)"
          % state.transformOrientation)
    loop.setTransformTool("move")
    check(state.transformOrientation == "screen",
          "and Move gets its own Screen back (%r)"
          % state.transformOrientation)
    # The dock row goes through the descriptor, not the loop: same swap.
    rows = {d.id: d for d in mods["tonicPanels"].descriptors("tube", state)}
    rows["transformTool"].set(state, session, "scale")
    check(state.transformTool == "scale" and
          state.transformOrientation == "world",
          "the transformTool row swaps the orientation too (%r)"
          % state.transformOrientation)
    rows["transformTool"].set(state, session, "move")
    check(state.transformOrientation == "screen",
          "and back to Move's own (%r)" % state.transformOrientation)
    orientation = rows["transformOrientation"]
    check(tuple(orientation.choiceLabels) ==
          tuple(settingsMod.OrientationLabel(o)
                for o in orientation.choices) and
          "tube" not in orientation.choiceLabels,
          "the Axis orientation combo shows human names (%r)"
          % (orientation.choiceLabels,))
    # A fresh state switched by a plain descriptor write also defaults.
    fresh = mods["TonicToolState"]()
    rows = {d.id: d for d in mods["tonicPanels"].descriptors("tube", fresh)}
    rows["transformTool"].set(fresh, None, "rotate")
    check(fresh.transformOrientation == "tube",
          "a first Rotate from the dock starts Local")
    # Reset transform tool puts the live tool's orientation back too.
    fresh.transformOrientation = "screen"

    class _Container(object):
        tonicState = fresh
    reset = {a.id: a for a in mods["tonicPanels"].actions("tube")}
    reset["resetTransformTool"].handler(_Container())
    check(fresh.transformOrientation == "tube",
          "Reset transform tool restores Rotate's Local")


def testGroupPivot(mods):
    print("-- parity G17: Individual Origins / Selection Centre -------")
    tonicLib = mods["tonicLib"]
    gizmo = mods["tonicGizmo"]
    settingsMod = mods["tonicGizmoSettings"]
    cam = orthoCamera(mods["tonicCamera"])

    def rotateQuarter(loop, session):
        """One real press on W, a stubbed 90 degree turn about +Z."""
        origin = cam.worldToPixels(loop._gizmo.origin)
        press = sample(mods, session, cam, origin[0], origin[1])
        if not loop._beginDrag(press, gizmo.HANDLE_W):
            return False
        loop._gizmo.rotationDrag = lambda _c, _x, _y: ((0.0, 0.0, 1.0),
                                                       math.pi / 2.0)
        applied = loop._applyRotation(press)
        loop.release(press)
        return applied

    def setUp(pivot, cvs, tubes=None):
        dll, session, state, loop = newTube(mods)
        if tubes:
            dll.centers.update(tubes)
        loop.setTransformTool("rotate")
        # World, so the gizmo frame is the identity and only the pivot
        # differs between the two modes.
        loop.setTransformOrientation("world")
        check(loop.setGroupPivot(pivot), "setGroupPivot(%r) accepted" % pivot)
        session.select(tonicLib.TONIC_PICK_CENTER_CV,
                       [t for t, _cv in cvs], [cv for _t, cv in cvs],
                       [-1] * len(cvs), tonicLib.TONIC_SELECT_SET)
        loop.refreshGizmo(cam)
        return dll, session, state, loop

    # Two CVs on one tube: CV 1 at (0, 0) and CV 2 at (0, 1).
    dll, session, state, loop = setUp(settingsMod.GROUP_PIVOT_INDIVIDUAL,
                                      [(0, 1), (0, 2)])
    check(state.gizmoSettings.For("rotate").groupPivot == "individual",
          "Individual Origins is Tonic's default group pivot")
    check(all(near(loop._gizmo.origin[i], (0.0, -1.0, -5.0)[i])
              for i in range(3)),
          "Individual draws the gizmo at the lead tube's root (%r)"
          % (loop._gizmo.origin,))
    check(rotateQuarter(loop, session), "Individual: the turn applies")
    check(near(dll.centers[0][1][0], -1.0) and
          near(dll.centers[0][1][1], -1.0) and
          near(dll.centers[0][2][0], -2.0) and
          near(dll.centers[0][2][1], -1.0),
          "Individual turns both CVs about the root (%r)"
          % (dll.centers[0],))

    dll, session, state, loop = setUp(settingsMod.GROUP_PIVOT_CENTRE,
                                      [(0, 1), (0, 2)])
    check(all(near(loop._gizmo.origin[i], (0.0, 0.5, -5.0)[i])
              for i in range(3)),
          "Centre draws the gizmo at the selection's middle (%r)"
          % (loop._gizmo.origin,))
    check(rotateQuarter(loop, session), "Centre: the turn applies")
    check(near(dll.centers[0][1][0], 0.5) and
          near(dll.centers[0][1][1], 0.5) and
          near(dll.centers[0][2][0], -0.5) and
          near(dll.centers[0][2][1], 0.5),
          "Centre turns both CVs about their shared middle (%r)"
          % (dll.centers[0],))
    check(near(dll.centers[0][0][1], -1.0),
          "the unselected root does not move (%r)" % (dll.centers[0][0],))

    # Root and tip under Centre: the selected root turns with the rest.
    dll, session, state, loop = setUp(settingsMod.GROUP_PIVOT_CENTRE,
                                      [(0, 0), (0, 2)])
    rotateQuarter(loop, session)
    check(near(dll.centers[0][0][0], 1.0) and
          near(dll.centers[0][0][1], 0.0) and
          near(dll.centers[0][2][0], -1.0) and
          near(dll.centers[0][2][1], 0.0),
          "a selected root turns about the centre too (%r)"
          % (dll.centers[0],))
    dll, session, state, loop = setUp(settingsMod.GROUP_PIVOT_INDIVIDUAL,
                                      [(0, 0), (0, 2)])
    rotateQuarter(loop, session)
    check(near(dll.centers[0][0][0], 0.0) and
          near(dll.centers[0][0][1], -1.0),
          "Individual keeps the root on the scalp (%r)"
          % (dll.centers[0][0],))

    # Two roots on two tubes: pinned alone, a real turn under Centre.
    second = {1: [[2.0, -1.0, -5.0], [2.0, 0.0, -5.0], [2.0, 1.0, -5.0]]}
    dll, session, state, loop = setUp(settingsMod.GROUP_PIVOT_INDIVIDUAL,
                                      [(0, 0), (1, 0)], second)
    check(loop._pinnedRootOnly() and loop._gizmo.locked,
          "Individual: two roots alone are pinned")
    dll, session, state, loop = setUp(settingsMod.GROUP_PIVOT_CENTRE,
                                      [(0, 0), (1, 0)], second)
    check(not loop._pinnedRootOnly() and not loop._gizmo.locked,
          "Centre: two roots turn about their middle, so not pinned")
    rotateQuarter(loop, session)
    check(near(dll.centers[0][0][0], 1.0) and
          near(dll.centers[0][0][1], -2.0) and
          near(dll.centers[1][0][0], 1.0) and
          near(dll.centers[1][0][1], 0.0),
          "the two roots swing about (1, -1) (%r, %r)"
          % (dll.centers[0][0], dll.centers[1][0]))

    # P: Rotate and Scale cycle their own; Move/Select have no choice.
    dll, session, state, loop = newTube(mods)
    loop.setTransformTool("rotate")
    check(loop.cycleGroupPivot() == "centre" and
          loop.groupPivot() == "centre" and
          loop.cycleGroupPivot() == "individual",
          "P cycles Rotate: Individual -> Centre -> Individual")
    check(session.statuses and "Individual Origins" in session.statuses[-1],
          "and names the pivot in the status line (%r)"
          % (session.statuses[-1:],))
    loop.cycleGroupPivot()
    loop.setTransformTool("scale")
    check(loop.groupPivot() == "individual",
          "each tool keeps its own group pivot (Scale still Individual)")
    loop.setTransformTool("move")
    check(loop.cycleGroupPivot() is None and
          "Rotate and Scale" in session.statuses[-1],
          "P under Move is refused with a reason (%r)"
          % (session.statuses[-1:],))
    loop.setTransformTool("rotate")
    loop._dragging = True
    check(loop.cycleGroupPivot() is None and loop.groupPivot() == "centre",
          "P is refused mid-drag")
    loop._dragging = False
    check(settingsMod.NextGroupPivot("centre", "rotate") == "individual" and
          settingsMod.NextGroupPivot("centre", "move") == "centre" and
          settingsMod.GroupPivotChoices("select") == (),
          "NextGroupPivot wraps and leaves toolless modes alone")


def testPinnedRoot(mods):
    print("-- GZ-06: a lone root CV under Rotate/Scale ---------------")
    tonicLib = mods["tonicLib"]
    gizmo = mods["tonicGizmo"]
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 0,
        "subSubId": -1}
    click(mods, session, cam, loop, 200.0, 225.0)
    check(not loop._gizmo.locked,
          "Move on the root stays a live manipulator")
    for tool in ("rotate", "scale"):
        session.statuses = []
        loop.setTransformTool(tool)
        loop.refreshGizmo(cam)
        records = loop._gizmo.screenHandles(cam)
        check(records and all(not r["grabbable"] for r in records),
              "%s on the root: the gizmo stays, every handle ungrabbable "
              "(%d records)" % (tool, len(records)))
        check(any("pinned" in text for text in session.statuses),
              "and the status says the root is pinned (%r)"
              % (session.statuses[-1:],))
        origin = cam.worldToPixels(loop._gizmo.origin)
        check(loop._gizmo.handleAt(cam, origin[0], origin[1]) ==
              gizmo.HANDLE_NONE and not loop.middleRepeatAvailable(),
              "nothing on it can be grabbed or repeated")
    loop.setTransformTool("move")
    check(not loop._gizmo.locked, "back to Move, the root is live again")


def testRingHint(mods):
    print("-- GZ-06: the Ring hint names real handles ----------------")
    from usdGenTonicTools.tonicTube import tubeEditHint
    tonicLib = mods["tonicLib"]
    gizmo = mods["tonicGizmo"]
    frame = {"origin": (0.0, 0.0, -5.0), "u": (1.0, 0.0, 0.0),
             "v": (0.0, 0.8, 0.6), "w": (0.0, -0.6, 0.8),
             "scale": 1.0, "twist": 0.0}
    stage = FakeStage({(0, 1): frame})
    dll, session, state, loop = newTube(mods, "ring", stage)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_SECTION_RING, "id": 0, "subId": 1,
        "subSubId": -1}
    click(mods, session, cam, loop, 200.0, 150.0)
    hint = tubeEditHint("ring")
    handles = {record["handle"] for record in loop._gizmo.screenHandles(cam)}
    # Each phrase of the hint and the handle that has to exist for it.
    promised = (("in its plane", gizmo.HANDLE_PLANE_XY),
                ("outer circle", gizmo.HANDLE_RING),
                ("W arrow", gizmo.HANDLE_W))
    check(all(phrase in hint for phrase, _h in promised),
          "the Ring hint names the plane move, circle and W arrow (%r)"
          % hint)
    check(all(handle in handles for _p, handle in promised),
          "and every handle it names is on screen under Move (%r)" % handles)
    check(hint in session.statuses[-1],
          "the selection status carries it (%r)" % session.statuses[-1:])


def testDragReadout(mods):
    print("-- GZ-07: readout, Shift precision, Ctrl plane ------------")
    tonicLib = mods["tonicLib"]
    gizmo = mods["tonicGizmo"]
    dll, session, state, loop = newTube(mods)
    cam = orthoCamera(mods["tonicCamera"])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    click(mods, session, cam, loop, 200.0, 150.0)
    check(loop.dragReadout() == "", "no drag, no readout")
    dll.reset()
    session.statuses = []
    # The u axis runs right from the centre; 10 px is 0.1 world units.
    loop.press(sample(mods, session, cam, 240.0, 150.0))
    loop.move(sample(mods, session, cam, 250.0, 150.0))
    loop.move(sample(mods, session, cam, 260.0, 150.0))
    readout = loop.dragReadout()
    check(readout.startswith("Move X") and "0.200" in readout,
          "two samples on U read 'Move X' and the along-axis 0.2 (%r)"
          % readout)
    check(any(text.startswith("Tonic Tube: Move X") for text in
              session.statuses),
          "the status line carries the readout (%r)" % session.statuses)
    before = len(dll.argsOf("Tonic_MoveTubeCenterCV"))
    loop.move(sample(mods, session, cam, 270.0, 150.0, ("shift",)))
    steps = dll.argsOf("Tonic_MoveTubeCenterCV")[before:]
    check(len(steps) == 1 and near(steps[0][2], 0.01),
          "Shift on the 3rd sample moves a tenth of the travel (%r)"
          % (steps,))
    readout = loop.dragReadout()
    check("0.210" in readout and "Shift precision" in readout,
          "and the readout says precision (%r)" % readout)
    loop.move(sample(mods, session, cam, 280.0, 150.0))
    steps = dll.argsOf("Tonic_MoveTubeCenterCV")[before:]
    check(len(steps) == 2 and near(steps[1][2], 0.1),
          "letting Shift go carries on at full speed, no jump (%r)"
          % (steps,))
    loop.release(sample(mods, session, cam, 280.0, 150.0))
    check(loop.dragReadout() == "", "the release clears the readout")

    # Ctrl at the press on an axis moves in the plane whose normal is
    # that axis (a DCC).  An oblique camera, so that plane is not edge-on.
    dll, session, state, loop = newTube(mods)
    cam = lookAtCamera(mods["tonicCamera"], (4.0, 3.0, -1.0))
    cv = cam.worldToPixels(CENTERS[1])
    session.pickFn = lambda mask, x, y: {
        "kind": tonicLib.TONIC_PICK_CENTER_CV, "id": 0, "subId": 1,
        "subSubId": -1}
    click(mods, session, cam, loop, cv[0], cv[1])
    loop.refreshGizmo(cam)
    origin = cam.worldToPixels(loop._gizmo.origin)
    tip = cam.worldToPixels(loop._gizmo.axisEndpoint(gizmo.HANDLE_U))
    grab = (0.5 * (origin[0] + tip[0]), 0.5 * (origin[1] + tip[1]))
    check(loop._gizmo.handleAt(cam, grab[0], grab[1]) == gizmo.HANDLE_U,
          "the U arrow is grabbable under the oblique camera")
    dll.reset()
    loop.press(sample(mods, session, cam, grab[0], grab[1], ("ctrl",)))
    loop.move(sample(mods, session, cam, grab[0] + 30.0, grab[1] - 40.0,
                     ("ctrl",)))
    steps = dll.argsOf("Tonic_MoveTubeCenterCV")
    check(steps and near(steps[-1][2], 0.0, 1e-4) and
          (abs(steps[-1][3]) + abs(steps[-1][4])) > 1e-3,
          "Ctrl on U: the delta has no U component (%r)" % (steps[-1:],))
    check(loop.dragReadout().startswith("Move YZ"),
          "and the readout names the YZ plane (%r)" % loop.dragReadout())
    loop.release(sample(mods, session, cam, grab[0] + 30.0, grab[1] - 40.0,
                        ("ctrl",)))


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
                                  tonicPanels, tonicGizmoScreen,
                                  tonicGizmoSettings)
    from usdGenTonicTools.tonicToolState import TonicToolState
    mods = {"tonicCamera": tonicCamera, "tonicGizmo": tonicGizmo,
            "tonicGizmoScreen": tonicGizmoScreen,
            "tonicGizmoSettings": tonicGizmoSettings,
            "tonicLib": tonicLib, "tonicLibStage": tonicLibStage,
            "tonicLoops": tonicLoops, "tonicLoopsFill": tonicLoopsFill,
            "tonicLoopsTube": tonicLoopsTube,
            "tonicTubeTransforms": tonicTubeTransforms,
            "tonicPanels": tonicPanels,
            "TonicToolState": TonicToolState}
    testShelf(mods)
    testSelectAndGizmo(mods)
    testGizmoParity(mods)
    testGizmoPriority(mods)
    testTweakDrag(mods)
    testClickAndMiddleRepeatTruth(mods)
    testWholeTubeTweak(mods)
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
    testComponentConversion(mods)
    testActivateAndDoubleClick(mods)
    testFillSelection(mods)
    testTubeSelectionMatrix(mods)
    testFillRampDrag(mods)
    testFillRampTargets(mods)
    testFillActivateRefill(mods)
    testRampMaths(mods)
    testFrozenTransformMaths(mods)
    testLoopRotateScaleBaselines(mods)
    testToolOrientationDefaults(mods)
    testGroupPivot(mods)
    testPinnedRoot(mods)
    testRingHint(mods)
    testDragReadout(mods)
    print("testUsdGenTonicToolsLoopsTube: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
