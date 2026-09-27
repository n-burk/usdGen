# T1 -- the brush tool's UI half: tool state defaults, the generated panel
# descriptors and actions, the status text, the viewport hotkey / ring
# arithmetic, the loop's live-groom cadence and hardness plumbing (against
# stubbed backends, so it runs whatever state the C++ brush lane is in),
# and -- on the offscreen Qt platform -- the palette widgets and the
# viewport controller's hotkey routing.
#
# Run as:  python testUsdGenToolsBrushUI.py [path to test-plane.usda]
# CMake registers it as testUsdGenToolsBrushUI (labels T1;tools;brush).
# Nothing is written to disk. The Qt half SKIPs (does not fail) when no Qt
# binding imports.

import contextlib
import os
import sys
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from usdGenTools import brushLoop, brushPanels, brushState

FAILURES = []


def check(condition, message):
    if condition:
        print("ok: " + message)
    else:
        print("FAIL: " + message)
        FAILURES.append(message)


def near(a, b, tolerance=1e-6):
    return abs(float(a) - float(b)) <= tolerance


@contextlib.contextmanager
def patched(target, name, value):
    had = hasattr(target, name)
    old = getattr(target, name, None)
    setattr(target, name, value)
    try:
        yield value
    finally:
        if had:
            setattr(target, name, old)
        else:
            delattr(target, name)


# -- state ----------------------------------------------------------------

def testStateDefaults():
    state = brushState.BrushToolState()
    check(state.hardness == 0.5, "hardness defaults to 0.5")
    check(state.previewMap is True, "the map overlay defaults on")
    check(state.resolutionAuto is True and state.resolution == 32,
          "resolution defaults to auto with a manual 32")
    check(state.liveGroom is True and state.strokesArmed is True,
          "live groom and viewport strokes default on")
    check(not hasattr(state, "previewLive"),
          "previewLive is gone (previewMap replaces it)")
    check(not hasattr(state, "displayPrior"),
          "the overlay no longer stashes a displayColor prior")
    check("erase" in brushState.BRUSH_IDS
          and brushState.BRUSH_MODES.get("erase") == "erase",
          "the shelf offers Erase, dabbing in erase mode")
    check(brushState.BRUSH_IDS == ("paint", "add", "smooth", "erase"),
          "shelf order: Paint, Add, Smooth, Erase")
    check(set(brushState.BRUSH_MODES) == set(brushState.BRUSH_IDS),
          "every shelf brush has a dab mode")


# -- descriptors / actions / status -------------------------------------

def testDescriptors():
    state = brushState.BrushToolState()
    descs = brushPanels.descriptors(state)
    rows = dict((d.id, d) for d in descs)
    check(set(rows) >= {"maskPreset", "radiusWorld", "strength", "hardness",
                        "falloff", "value", "channel", "previewMap",
                        "colorMap", "liveGroom", "strokesArmed",
                        "resolutionAuto", "resolution"},
          "descriptors cover every palette row")
    check("previewLive" not in rows, "no previewLive row")
    check(all(d.tooltip for d in descs), "every row carries a tooltip")
    groups = set(g for g, _label in brushPanels.GROUPS)
    check(all(d.group in groups for d in descs),
          "every row belongs to a palette section")
    check("F drag" in rows["radiusWorld"].tooltip
          and "[ ]" in rows["radiusWorld"].tooltip,
          "the radius tooltip names F drag and [ ]")
    check("Shift+F" in rows["strength"].tooltip,
          "the strength tooltip names Shift+F")
    check("Ctrl+F" in rows["hardness"].tooltip,
          "the hardness tooltip names Ctrl+F")
    check(rows["previewMap"].icons == ("preview_on", "preview_off"),
          "the eye toggle has on/off glyphs")
    check(rows["maskPreset"].icons[0] == "preset_density"
          and len(rows["maskPreset"].icons)
          == len(rows["maskPreset"].choices),
          "every mask preset has an icon")
    rows["hardness"].set(state, 1.7)
    check(state.hardness == 1.0, "hardness clamps high")
    rows["hardness"].set(state, -0.2)
    check(state.hardness == 0.0, "hardness clamps low")
    rows["hardness"].set(state, "nope")
    check(state.hardness == 0.0, "a non-number hardness falls to 0")
    rows["previewMap"].set(state, 0)
    check(state.previewMap is False, "previewMap toggles")
    rows["resolution"].set(state, 33)
    check(state.resolution == 32, "resolution snaps to a power of two")
    rows["resolution"].set(state, 1000)
    check(state.resolution == 256, "resolution clamps at 256")
    rows["resolution"].set(state, 1)
    check(state.resolution == 4, "resolution floors at 4")
    rows["resolutionAuto"].set(state, False)
    check(state.resolutionAuto is False, "resolutionAuto toggles")
    check(brushPanels.resolutionChoices()
          == [4, 8, 16, 32, 64, 128, 256], "power-of-two choices 4..256")
    rows["radiusWorld"].set(state, -1.0)
    check(state.radiusWorld == 0.0, "radius still clamps at 0")
    check(rows["radiusWorld"].max is None, "radius stays unbounded")


def testActions():
    actions = brushPanels.actions()
    ids = [a.id for a in actions]
    check(ids == ["setupDescription", "paintToDescription", "bind",
                  "flood", "undo", "redo"],
          "actions: setup, paint-to, bind, flood, undo, redo")
    check("clearPreview" not in ids, "no clearPreview action")
    check(all(a.icon and a.tooltip for a in actions),
          "every action has an icon name and a tooltip")
    check(all(a.icon in brushPanels.ICON_NAMES for a in actions),
          "action icons are from the icon set")
    byId = dict((a.id, a) for a in actions)
    check(byId["undo"].hotkeyLabel == "" and byId["redo"].hotkeyLabel == "",
          "undo/redo claim no hotkey (usdview owns Ctrl+Z/Ctrl+Y)")

    calls = []

    class Palette(object):
        def __getattr__(self, name):
            return lambda: calls.append(name)

    palette = Palette()
    for action in actions:
        action.handler(palette)
    check(calls == ["setupDescription", "paintToDescription",
                    "bindFromSelection", "flood", "undo", "redo"],
          "each action handler reaches its palette method")


def testStatus():
    state = brushState.BrushToolState()
    text = brushPanels.statusText(state)
    check("unbound" in text and "h=0.50" in text,
          "the status names the unbound state and the hardness")

    class Binding(object):
        surfacePath = "/Plane"
        mapPath = "/BrushMaps/densityPaint"
        primvar = "usdGen:paint:density"
        resolution = 16
        resolutionAuto = True
        channels = 1

    state.binding = Binding()
    check("bound: /Plane" in brushPanels.statusText(state, "x"),
          "and the bound state")
    check(brushPanels.statusKind(state) == "idle",
          "no gesture reads idle")
    check(brushPanels.statusKind(state, "cooking") == "cooking",
          "a cooking notice reads cooking")
    check(brushPanels.statusKind(state, "bogus") == "idle",
          "an unknown kind reads idle")

    class Gesture(object):
        class stroke(object):
            @staticmethod
            def dabCount():
                return 3

    state.gesture = Gesture()
    check(brushPanels.statusKind(state, "idle") == "stroke",
          "a live gesture reads stroke")
    check(brushPanels.statusKind(state, "error") == "error",
          "but an error still reads error")
    check(all(k in brushPanels.STATUS_COLORS
              for k in brushPanels.STATUS_KINDS),
          "every status kind has a dot colour")
    readout = brushPanels.resolutionText(
        state, "32 px/face (median edge 0.5) -> 0.1 MTexel")
    sep = brushPanels.READOUT_SEPARATOR
    check(readout == sep.join(("bound 16 px/face",
                               "auto suggests 32 px/face", "0.1 MTexel")),
          "the resolution readout is short: bound, suggestion, texels (%s)"
          % readout)
    check(brushPanels.resolutionText(state, "garbled")
          == sep.join(("bound 16 px/face", "auto suggests garbled")),
          "an unparsable suggestion still shows")
    state.binding.resolutionInfo = "8 px/face (median edge 1) -> 0.2 MTexel"
    check(brushPanels.resolutionText(state).endswith(
        sep.join(("auto suggests 8 px/face", "0.2 MTexel"))),
          "without a suggestion the binding's own info shows")
    check(brushPanels.resolutionText(brushState.BrushToolState())
          == "unbound", "an unbound readout says so")


# -- hotkey / ring arithmetic --------------------------------------------

def testHotkeys():
    table = [
        (("F",), {}, "adjustRadius"),
        (("f",), {}, "adjustRadius"),
        (("F",), {"shift": True}, "adjustStrength"),
        (("F",), {"ctrl": True}, "adjustHardness"),
        (("F",), {"ctrl": True, "shift": True}, None),
        (("F",), {"alt": True}, None),
        (("[",), {}, "radiusDown"),
        (("]",), {}, "radiusUp"),
        (("[",), {"shift": True}, "hardnessDown"),
        (("]",), {"shift": True}, "hardnessUp"),
        (("{",), {"shift": True}, "hardnessDown"),
        (("}",), {"shift": True}, "hardnessUp"),
        (("[",), {"ctrl": True}, None),
        (("Z",), {"ctrl": True}, None),
        (("Y",), {"ctrl": True}, None),
        (("G",), {}, None),
    ]
    for args, kwargs, want in table:
        got = brushPanels.hotkeyFor(*args, **kwargs)
        check(got == want, "hotkey %s %s -> %s" % (args[0], kwargs, want))
    P, N, F = (brushPanels.FOCUS_PALETTE, brushPanels.FOCUS_NONE,
               brushPanels.FOCUS_FOREIGN)
    decisions = [
        # (pointerOver, bound, armed, busy, focus) -> (claim, steal, hint)
        ((True, True, True, False, N), (True, False, "")),
        ((True, True, True, False, P), (True, True, "")),
        ((True, True, True, False, F), (False, False, "")),
        ((False, True, True, False, P), (False, False, "")),
        ((True, True, True, True, P), (False, False, "")),
        ((True, False, True, False, N),
         (False, False, brushPanels.HINT_UNBOUND)),
        ((True, False, True, False, P),
         (False, False, brushPanels.HINT_UNBOUND)),
        ((True, True, False, False, N),
         (False, False, brushPanels.HINT_DISARMED)),
        ((False, False, True, False, N), (False, False, "")),
        ((True, False, True, False, F), (False, False, "")),
    ]
    for args, want in decisions:
        got = brushPanels.hotkeyDecision(*args)
        check(got == want, "hotkey decision %s -> %s" % (args, want[:2]))

    check(set(brushPanels.ADJUST_FIELDS.values())
          == {"radiusWorld", "strength", "hardness"},
          "the drag-adjusts edit radius, strength and hardness")

    check(near(brushPanels.stepRadius(0.1, 1), 0.11), "] is +10%")
    check(near(brushPanels.stepRadius(0.1, -1), 0.09), "[ is -10%")
    check(brushPanels.stepRadius(0.0, -1) == brushPanels.RADIUS_FLOOR,
          "[ never reaches zero")
    check(near(brushPanels.stepHardness(0.5, 1), 0.6), "Shift+] is +0.1")
    check(near(brushPanels.stepHardness(0.5, -1), 0.4), "Shift+[ is -0.1")
    check(brushPanels.stepHardness(0.95, 1) == 1.0, "hardness caps at 1")
    check(brushPanels.stepHardness(0.05, -1) == 0.0, "and floors at 0")
    value = 0.0
    for _ in range(3):
        value = brushPanels.stepHardness(value, 1)
    check(value == 0.3, "repeated steps stay on the 0.1 grid (%r)" % value)

    px = brushPanels.ADJUST_RADIUS_PIXELS_PER_DOUBLING
    check(near(brushPanels.adjustValue("adjustRadius", 0.2, px), 0.4),
          "F drag right one doubling doubles the radius")
    check(near(brushPanels.adjustValue("adjustRadius", 0.2, -px), 0.1),
          "and left halves it")
    check(near(brushPanels.adjustValue("adjustRadius", 0.2, 0.0), 0.2),
          "no travel keeps the value")
    check(brushPanels.adjustValue("adjustRadius", 0.0, -1e6)
          == brushPanels.RADIUS_FLOOR, "the adjust never zeroes the radius")
    unit = brushPanels.ADJUST_UNIT_PIXELS
    check(near(brushPanels.adjustValue("adjustStrength", 0.5, unit * 0.25),
               0.75), "Shift+F drag is linear")
    check(brushPanels.adjustValue("adjustStrength", 0.5, unit * 5) == 1.0,
          "and clamps at 1")
    check(brushPanels.adjustValue("adjustHardness", 0.5, -unit * 5) == 0.0,
          "Ctrl+F drag clamps at 0")
    check(near(brushPanels.adjustValue("adjustHardness", 0.5, float("nan")),
               0.5), "a NaN travel is ignored")

    check(brushPanels.ringRadii(40.0, 0.5) == (40.0, 20.0),
          "the inner ring is hardness * outer")
    check(brushPanels.ringRadii(40.0, 0.0) == (40.0, 0.0),
          "hardness 0 draws no inner ring")
    check(brushPanels.ringRadii(40.0, 3.0) == (40.0, 40.0),
          "hardness clamps to the outer ring")
    check(brushPanels.ringRadii(-3.0, 0.5) == (0.0, 0.0),
          "a negative ring collapses")

    for radius in (0.001, 0.05, 0.15, 1.0, 10.0):
        back = brushPanels.sliderToRadius(brushPanels.radiusToSlider(radius))
        check(abs(back - radius) / radius < 0.01,
              "the radius slider round-trips %g (%g)" % (radius, back))
    check(brushPanels.radiusToSlider(0.0) == 0
          and brushPanels.radiusToSlider(1e6) == 1000,
          "the radius slider clamps at its ends")


def testIcons():
    dirs = brushPanels.iconDirectories()
    check(any(d.replace("\\", "/").endswith("resources/icons") for d in dirs),
          "icons resolve under the plugin resources dir")
    check(brushPanels.iconPath("no_such_icon_name") is None,
          "a missing icon resolves to None (text fallback)")
    check(brushPanels.iconPath("") is None, "an empty icon name is None")
    found = [n for n in brushPanels.ICON_NAMES if brushPanels.iconPath(n)]
    print("info: %d of %d icons present" % (len(found),
                                            len(brushPanels.ICON_NAMES)))


# -- loop cadence (stubbed backends) -------------------------------------

class _Live(object):
    """A LiveStroke stand-in: records calls, reports touched faces."""

    native = False

    def __init__(self, touched=(2,)):
        self.calls = []
        self.touchedPerDab = list(touched)
        self._pending = set()
        self.aborted = False
        self.closed = False
        self.commits = 0
        self.grid = object()

    def dab(self, *args, **kwargs):
        self.calls.append((args, kwargs))
        self._pending |= set(self.touchedPerDab)
        return (1, "")

    def takeTouched(self):
        out = sorted(self._pending)
        self._pending = set()
        return out

    def dabCount(self):
        return len(self.calls)

    def workingGrid(self):
        return self.grid

    def commitGrid(self):
        self.commits += 1
        return self.grid

    def abort(self):
        self.aborted = True

    def close(self):
        self.closed = True


class _Undo(object):
    def __init__(self):
        self.undos = 0

    def undo(self):
        self.undos += 1
        return (True, "")

    def redo(self):
        return (True, "")

    def clear(self):
        pass


def _stubbedLoop():
    state = brushState.BrushToolState()
    state.undoStack = _Undo()
    loop = brushLoop.BrushLoop(state)
    state.binding = object()
    gesture = brushLoop.BrushGesture(
        camera=object(), snapshot=object(), base=object(), live=_Live(),
        prior=None, first=brushLoop.FirstDab(2, 0.5, 0.5))
    gesture.lastAttempt = (0.0, 0.0)
    state.gesture = gesture
    return state, loop, gesture


# LiveStroke.dab positional layout (the cpp contract).
_ARG_RADIUS, _ARG_HARDNESS, _ARG_MODE = 3, 4, 8


def testLoopCadence():
    writes = []

    def patch(stage, binding, grid, faces):
        writes.append(set(faces))
        return (True, "")

    with contextlib.ExitStack() as stack:
        stack.enter_context(patched(brushLoop.brushAuthor,
                                    "PatchLivePrimvar", patch))
        stack.enter_context(patched(brushLoop.brushPick, "pickPixels",
                                    lambda s, c, x, y: (2, 0.5, 0.5,
                                                        (0.0, 0.0, 0.0))))
        stack.enter_context(patched(brushLoop.brushPick, "faceEdgeLen",
                                    lambda s, f: 1.0))
        state, loop, gesture = _stubbedLoop()
        live = gesture.live
        state.previewMap = False  # no overlay traffic in this test
        state.hardness = 0.3
        state.activeBrush = "erase"
        stage = object()

        ok, info = loop.move(stage, 10.0, 0.0)
        check(ok and info.endswith("live groom"),
              "the first move writes the live groom (%s)" % info)
        check(len(writes) == 1 and writes[0] == {2}
              and gesture.liveWrites == 1 and loop.liveGroomWrites == 1,
              "one live write of the touched faces, counted on the "
              "gesture and the loop")
        args, kwargs = live.calls[-1]
        check(len(live.calls) == 1 and kwargs.get("isMove") is True
              and kwargs.get("spacing") == brushLoop.MOVE_SPACING,
              "a move is ONE driver call (isMove, spacing), no per-stamp "
              "Python")
        check(args[_ARG_HARDNESS] == 0.3,
              "the dab carries the state hardness")
        check(args[_ARG_MODE] == "erase", "Erase dabs in erase mode")
        check(args[_ARG_RADIUS] >= brushLoop.brushPick.MIN_FACE_RADIUS,
              "the loop floors radiusUV to the corner reach")
        ok, info = loop.move(stage, 20.0, 0.0)
        check(ok and not info.endswith("live groom") and len(writes) == 1,
              "a move inside LIVE_GROOM_MIN_INTERVAL does not write (%s)"
              % info)
        check(gesture.liveDirty == {2}, "but leaves its faces dirty")
        written, info = loop.flushLiveGroom(stage)
        check(written and len(writes) == 2 and not gesture.liveDirty,
              "flushLiveGroom lands the trailing move (%s)" % info)
        written, info = loop.flushLiveGroom(stage)
        check(not written and info == "clean",
              "a clean flush writes nothing (%s)" % info)
        ok, info = loop.move(stage, 30.0, 0.0)
        cooked, info = loop.pollLiveGroom(stage)
        check(not cooked and info == "dragging",
              "polling right after a write waits (%s)" % info)
        gesture.lastLive -= brushLoop.LIVE_GROOM_MIN_INTERVAL + 0.01
        cooked, info = loop.pollLiveGroom(stage)
        check(cooked and len(writes) == 3,
              "polling past the interval flushes (%s)" % info)
        ok, info = loop.move(stage, 40.0, 0.0)
        gesture.lastMoveTime = time.time() - brushLoop.LIVE_GROOM_PAUSE - 0.01
        gesture.lastLive = time.time()
        cooked, info = loop.pollLiveGroom(stage)
        check(cooked and len(writes) == 4,
              "polling after a pause flushes too (%s)" % info)
        state.liveGroom = False
        gesture.lastLive = 0.0
        ok, info = loop.move(stage, 50.0, 0.0)
        check(len(writes) == 4 and not info.endswith("live groom"),
              "no live write while live groom is off")
        written, info = loop.flushLiveGroom(stage)
        check(not written and info == "live groom is off",
              "and the flush refuses (%s)" % info)
        state.liveGroom = True
        state.hardness = 7.0
        loop.move(stage, 60.0, 0.0)
        check(live.calls[-1][0][_ARG_HARDNESS] == 1.0,
              "a hardness past 1 reaches the dab clamped")
        ok, info = loop.move(stage, 61.0, 0.0)
        check(ok and info == "throttled",
              "sub-pixel moves stay throttled (%s)" % info)
        check(brushLoop.LIVE_GROOM_MIN_INTERVAL == 0.05
              and brushLoop.LIVE_GROOM_FLUSH_MS == 60,
              "cadence: 50 ms per-move interval, 60 ms trailing flush")
        check(loop.dabCount() == len(live.calls),
              "the loop's dab count is the driver's")

        # A write that raises is contained (the stroke stays live).
        def boom(*_args):
            raise RuntimeError("scratch failed")

        with patched(brushLoop.brushAuthor, "PatchLivePrimvar", boom):
            gesture.liveDirty = {1}
            written, info = loop.flushLiveGroom(stage)
        check(not written and "internal error" in info
              and state.gesture is gesture,
              "a raising live write is reported, not raised (%s)" % info)

        # A driver that raises mid-move drops the stroke, closed.
        def dabBoom(*_args, **_kwargs):
            raise RuntimeError("native dab failed")

        with contextlib.ExitStack() as inner:
            inner.enter_context(patched(live, "dab", dabBoom))
            inner.enter_context(patched(brushLoop.brushAuthor,
                                        "ClearLivePrimvar",
                                        lambda *a: (True, "")))
            inner.enter_context(patched(brushLoop.brushPreview,
                                        "ClearPreview",
                                        lambda *a: (True, "")))
            ok, info = loop.move(stage, 90.0, 0.0)
        check(not ok and "internal error" in info
              and state.gesture is None and live.aborted and live.closed,
              "a raising driver drops, aborts and closes the stroke (%s)"
              % info)


class _Snapshot(object):
    def faceCount(self):
        return 4


class _Base(object):
    def numFaces(self):
        return 4


def testLoopPressRelease():
    """press -> move -> release/cancel through LiveStroke.create."""
    made = []

    class LiveStroke(object):
        @staticmethod
        def create(snapshot, base):
            live = _Live()
            made.append((snapshot, base, live))
            return (live, "")

    baked = []

    class Surface(object):
        pass

    class Stage(object):
        def GetPrimAtPath(self, path):
            return Surface()

    class Camera(object):
        invertible = True

    snapshot, base = _Snapshot(), _Base()
    with contextlib.ExitStack() as stack:
        for module, name, value in (
                (brushLoop.brushMap, "LiveStroke", LiveStroke),
                (brushLoop.brushPick, "snapshotMesh",
                 lambda prim, faces=None: (snapshot, "")),
                (brushLoop.brushPick, "pickPixels",
                 lambda s, c, x, y: (1, 0.25, 0.75, (0.0, 0.0, 0.0))),
                (brushLoop.brushPick, "faceEdgeLen", lambda s, f: 1.0),
                (brushLoop.brushAuthor, "BaseGridFromStage",
                 lambda stage, binding: (base, "")),
                (brushLoop.brushAuthor, "CaptureLivePrior",
                 lambda stage, binding: None),
                (brushLoop.brushAuthor, "ClearLivePrimvar",
                 lambda *a: (True, "")),
                (brushLoop.brushAuthor, "PatchLivePrimvar",
                 lambda *a: (True, "")),
                (brushLoop.brushAuthor, "BakeStroke",
                 lambda stage, binding, grid, undoStack=None:
                 baked.append(grid) or (True, "")),
                (brushLoop.brushPreview, "CapturePrior",
                 lambda stage, binding: None),
                (brushLoop.brushPreview, "ClearPreview",
                 lambda *a: (True, "")),
        ):
            stack.enter_context(patched(module, name, value))
        state = brushState.BrushToolState()
        loop = brushLoop.BrushLoop(state)
        loop.showBoundMap = lambda stage: (True, "")
        state.binding = type("B", (), {"surfacePath": "/Plane"})()
        state.previewMap = False
        stage = Stage()
        captured, info = loop.press(stage, Camera(), 5.0, 5.0)
        check(captured and len(made) == 1 and made[0][:2] == (snapshot, base),
              "press opens one LiveStroke over the snapshot and base (%s)"
              % info)
        live = made[0][2]
        check(len(live.calls) == 1
              and live.calls[0][1].get("isMove") is False,
              "the press dab is not a move")
        first = state.gesture.first
        check((first.face, first.u, first.v) == (1, 0.25, 0.75),
              "the gesture records where the stroke started")
        check(state.gesture.working is live.grid,
              "the working grid is the driver's")
        loop.move(stage, 25.0, 5.0)
        check(len(live.calls) == 2, "the move is one more driver call")
        ok, info = loop.release(stage)
        check(ok and baked == [live.grid] and live.commits == 1,
              "release bakes live.commitGrid() (%s)" % info)
        check(live.closed and state.gesture is None,
              "and closes the driver")

        loop.press(stage, Camera(), 5.0, 5.0)
        live = made[-1][2]
        check(loop.cancel(stage) and live.aborted and live.closed
              and state.gesture is None,
              "cancel aborts then closes the driver")

        loop.press(stage, Camera(), 5.0, 5.0)
        live = made[-1][2]
        loop.setStage(stage)
        check(live.closed and state.gesture is None,
              "a stage replace closes a live driver")

        class Refusing(object):
            @staticmethod
            def create(snapshot, base):
                live = _Live()
                live.dab = lambda *a, **k: (-1, "bad dab")
                made.append((snapshot, base, live))
                return (live, "")

        with patched(brushLoop.brushMap, "LiveStroke", Refusing):
            captured, info = loop.press(stage, Camera(), 5.0, 5.0)
        check(not captured and info == "bad dab" and made[-1][2].closed
              and state.gesture is None,
              "a refused press dab closes the driver and captures nothing")


def testLoopPreviewAndUndo():
    cleared = []
    shown = []

    def clearPreview(stage, binding, prior=None):
        cleared.append((stage, binding, prior))
        return (True, "")

    with contextlib.ExitStack() as stack:
        stack.enter_context(patched(brushLoop.brushPreview, "ClearPreview",
                                    clearPreview))
        stack.enter_context(patched(brushLoop.brushAuthor,
                                    "ClearLivePrimvar",
                                    lambda *a: (True, "")))
        state, loop, gesture = _stubbedLoop()
        loop.showBoundMap = lambda stage: shown.append(stage) or (True, "")
        check(not hasattr(loop, "clearPreview"),
              "loop.clearPreview is gone")
        stage = object()
        state.gesture = None
        ok, _info = loop.setPreviewMap(stage, False)
        check(ok and state.previewMap is False and len(cleared) == 1
              and cleared[0][0] is stage and cleared[0][2] is None,
              "the eye toggle off clears the overlay, with no prior")
        ok, _info = loop.setPreviewMap(stage, True)
        check(ok and state.previewMap is True and shown == [stage],
              "the eye toggle on redraws the bound map")
        other = object()
        ok, _info = loop.undo(other)
        check(ok and state.undoStack.undos == 1 and shown[-1] is other,
              "undo redraws on the stage the palette passes")
        ok, _info = loop.redo(stage)
        check(ok and shown[-1] is stage, "and so does redo")
        loop.undo()
        check(shown[-1] is stage, "a bare undo reuses the last stage")

        state.gesture = gesture
        check(loop.cancel(stage) and state.gesture is None
              and gesture.live.aborted and gesture.live.closed,
              "cancel drops, aborts and closes the stroke")
        check(shown[-1] is stage,
              "and redraws the baked map with the overlay on")
        state.previewMap = False
        gesture = brushLoop.BrushGesture(object(), object(), object(),
                                         _Live(), None)
        state.gesture = gesture
        before = len(cleared)
        loop.cancel(stage)
        check(len(cleared) == before + 1,
              "with the overlay off cancel clears it instead")

        check(loop._bindResolution() is None, "auto binds pass None")
        state.resolutionAuto = False
        state.resolution = 64
        check(loop._bindResolution() == 64, "manual binds pass the size")


def testLoopBindResolution(stagePath):
    try:
        from pxr import Usd, UsdGeom
    except ImportError:
        print("SKIP: pxr unavailable for the bind-resolution check")
        return
    stage = Usd.Stage.CreateInMemory()
    mesh = UsdGeom.Mesh.Define(stage, "/Plane")
    mesh.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    seen = []

    def bindSurface(stage, path, **kwargs):
        seen.append(kwargs)
        return (None, "stubbed")

    with patched(brushLoop.brushAuthor, "BindSurface", bindSurface):
        state = brushState.BrushToolState()
        loop = brushLoop.BrushLoop(state)
        loop.bindFromSelection(stage, ["/Plane"])
        check(seen and seen[-1].get("resolution", "x") is None,
              "an auto bind asks BindSurface for the suggested size")
        state.resolutionAuto = False
        state.resolution = 16
        loop.bindFromSelection(stage, ["/Plane"])
        check(seen[-1].get("resolution") == 16,
              "a manual bind passes the manual size")
        loop.bindFromSelection(stage, ["/Plane"], resolution=8)
        check(seen[-1].get("resolution") == 8,
              "an explicit resolution wins")


# -- Qt: palette + viewport controller (offscreen) -----------------------

def _qt():
    try:
        from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
    except Exception as exc:  # no Qt binding in this interpreter
        print("SKIP: Qt unavailable (%s)" % exc)
        return None
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication(
        ["testUsdGenToolsBrushUI"])
    return QtCore, QtGui, QtWidgets, app


class _Api(object):
    def __init__(self, QtWidgets, view=None):
        self.qMainWindow = QtWidgets.QMainWindow()
        self.dataModel = None
        self.stageView = view
        self.updates = 0

    def UpdateViewport(self):
        self.updates += 1


def testPalette():
    qt = _qt()
    if qt is None:
        return
    QtCore, QtGui, QtWidgets, app = qt
    from usdGenTools import brushPalette
    api = _Api(QtWidgets)
    palette = brushPalette.BrushPaletteDock(api)
    state = palette._state
    ids = set(d.id for d in brushPanels.descriptors(state))
    check(set(palette._rows) == ids,
          "every descriptor has an editing widget in _rows")
    root = palette.widget()
    check(root.objectName() == brushPalette.OBJECT_NAME
          and "#%s" % brushPalette.OBJECT_NAME in root.styleSheet(),
          "the stylesheet is scoped to the dock's root")
    check(len(palette._shelfButtons) == 4
          and "erase" in palette._shelfButtons,
          "the shelf has Paint, Add, Smooth and Erase")
    texts = [b.text() for b in root.findChildren(QtWidgets.QAbstractButton)]
    check(not any("clear" in t.lower() and "preview" in t.lower()
                  for t in texts), "no Clear preview button anywhere")
    missing = [b.text() for b in root.findChildren(QtWidgets.QAbstractButton)
               if not b.toolTip()]
    check(not missing, "every button has a tooltip (%s)" % missing)
    missingRows = [i for i, (_d, w) in palette._rows.items()
                   if not w.toolTip()]
    check(not missingRows, "every row widget has a tooltip (%s)"
          % missingRows)

    palette._shelfButtons["erase"].click()
    check(state.activeBrush == "erase", "the Erase shelf button selects it")

    presets = palette._rows["maskPreset"][1]
    check(presets.count() == 5 and presets.itemData(1) == "length"
          and presets.currentData() == "density",
          "the mask row offers the presets, density current")
    presets.setCurrentIndex(presets.findData("length"))
    check(state.maskPreset == "density"
          and presets.currentData() == "density",
          "a refused rebind (no stage) reverts state and row")
    check(palette.statusKind() == "error",
          "and the refusal lights the error dot")

    state.radiusWorld = 0.42
    state.strength = 0.25
    state.hardness = 0.8
    state.previewMap = False
    state.liveGroom = False
    state.falloff = "linear"
    state.resolutionAuto = False
    state.resolution = 128
    palette.refresh()
    rows = palette._rows
    check(near(rows["radiusWorld"][1].value(), 0.42, 1e-3)
          and near(rows["strength"][1].value(), 0.25, 1e-3)
          and near(rows["hardness"][1].value(), 0.8, 1e-3),
          "refresh pushes state into the spinboxes")
    check(palette._sliders["hardness"].value() == 800
          and palette._sliders["radiusWorld"].value()
          == brushPanels.radiusToSlider(0.42),
          "and into the sliders")
    check(not rows["previewMap"][1].isChecked()
          and not rows["liveGroom"][1].isChecked(),
          "and into the toggles")
    check(rows["falloff"][1].currentData() == "linear",
          "and into the combos")
    check(rows["resolution"][1].value() == 128
          and rows["resolution"][1].isEnabled()
          and not rows["resolutionAuto"][1].isChecked(),
          "and into the resolution row (manual: enabled)")

    palette._sliders["strength"].setValue(600)
    check(near(state.strength, 0.6), "the strength slider writes state")
    check(near(rows["strength"][1].value(), 0.6, 1e-3),
          "and the spinbox follows")
    rows["hardness"][1].setValue(0.35)
    check(near(state.hardness, 0.35), "the hardness spinbox writes state")
    check(palette._sliders["hardness"].value() == 350,
          "and the slider follows")
    rows["resolution"][1].stepBy(1)
    check(state.resolution == 256, "the texel spin doubles per step")
    rows["resolution"][1].stepBy(-2)
    check(state.resolution == 64, "and halves")
    rows["resolutionAuto"][1].setChecked(True)
    check(state.resolutionAuto and not rows["resolution"][1].isEnabled(),
          "Auto disables the manual texel spin")

    eye = rows["previewMap"][1]
    eye.click()
    check(state.previewMap is True and eye.isChecked(),
          "the eye toggle shows the map")
    eye.click()
    check(state.previewMap is False, "and hides it")

    palette.onLoopStatus("stroke: 3 dabs", "stroke")
    check(palette._message.text() == "stroke: 3 dabs"
          and palette.statusKind() == "stroke",
          "viewport status reaches the strip with its kind")
    palette.onLoopStatus("plain message")
    check(palette.statusKind() == "idle", "a kindless message reads idle")

    check(not palette._actionButtons["undo"].isEnabled(),
          "undo is disabled with nothing to undo")

    palette.resize(280, 900)
    hint = root.minimumSizeHint().width()
    check(hint <= 280, "the dock fits a 280 px width (min %d)" % hint)
    palette.deleteLater()
    app.processEvents()


def testViewportController():
    qt = _qt()
    if qt is None:
        return
    QtCore, QtGui, QtWidgets, app = qt
    from usdGenTools import brushViewport
    window = QtWidgets.QWidget()
    window.resize(400, 300)
    view = QtWidgets.QWidget(window)
    view.resize(400, 300)
    api = _Api(QtWidgets, view)
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    controller = brushViewport.BrushViewportController(api, state, loop)
    messages = []
    controller.setStatusSink(lambda text, kind=None:
                             messages.append((text, kind)))
    check(controller.install() and controller.installed,
          "the controller installs on a stand-in view")
    check(not controller.hotkeysClaimable(),
          "hotkeys are not claimed with nothing bound")
    state.binding = object()
    controller._pointer = None
    # The real cursor is nowhere near an offscreen view.
    check(not controller.hotkeysClaimable() or controller.pointerOverView(),
          "hotkeys need the pointer over the view")
    controller._pointer = (100.0, 100.0)
    check(controller.hotkeysClaimable(),
          "bound + armed + pointer over the view claims the hotkeys")
    state.strokesArmed = False
    check(not controller.hotkeysClaimable(), "disarmed strokes claim none")
    state.strokesArmed = True

    Key = QtCore.Qt.Key
    Mod = QtCore.Qt.KeyboardModifier
    Type = QtCore.QEvent.Type

    def key(kind, code, mods=Mod.NoModifier):
        return QtGui.QKeyEvent(kind, code, mods)

    override = key(Type.ShortcutOverride, Key.Key_F)
    override.ignore()
    check(controller.keyEvent(override, True) and override.isAccepted(),
          "F's ShortcutOverride is accepted (Frame Selected suppressed)")
    radius = state.radiusWorld
    check(controller.keyEvent(key(Type.KeyPress, Key.Key_F), False)
          and controller.adjusting == "adjustRadius",
          "F starts the radius drag-adjust")
    controller.adjustTo(100.0 + brushPanels.ADJUST_RADIUS_PIXELS_PER_DOUBLING)
    check(near(state.radiusWorld, radius * 2.0),
          "dragging right doubles the radius live")
    rings = controller.overlayRings()
    check(rings is not None and near(rings[3], rings[2] * state.hardness),
          "the adjust draws both rings (inner = hardness * outer)")
    check(controller.keyEvent(key(Type.KeyPress, Key.Key_Escape), False)
          and controller.adjusting is None
          and near(state.radiusWorld, radius),
          "Esc cancels and restores the radius")
    controller.keyEvent(key(Type.KeyPress, Key.Key_F, Mod.ShiftModifier),
                        False)
    check(controller.adjusting == "adjustStrength", "Shift+F adjusts strength")
    controller.adjustTo(100.0 + brushPanels.ADJUST_UNIT_PIXELS * 0.1)
    controller.keyEvent(key(Type.KeyPress, Key.Key_Return), False)
    check(controller.adjusting is None and near(state.strength, 0.6),
          "Enter confirms the strength (%.3f)" % state.strength)
    controller.keyEvent(key(Type.KeyPress, Key.Key_F, Mod.ControlModifier),
                        False)
    check(controller.adjusting == "adjustHardness", "Ctrl+F adjusts hardness")
    controller.adjustTo(100.0 - brushPanels.ADJUST_UNIT_PIXELS * 0.2)
    press = QtGui.QMouseEvent(
        Type.MouseButtonPress, QtCore.QPointF(100.0, 100.0),
        QtCore.QPointF(100.0, 100.0), QtCore.Qt.MouseButton.LeftButton,
        QtCore.Qt.MouseButton.LeftButton, Mod.NoModifier)
    check(controller.viewEvent(view, press) and controller.adjusting is None
          and near(state.hardness, 0.3),
          "a left click confirms the hardness (%.3f)" % state.hardness)
    release = QtGui.QMouseEvent(
        Type.MouseButtonRelease, QtCore.QPointF(100.0, 100.0),
        QtCore.QPointF(100.0, 100.0), QtCore.Qt.MouseButton.LeftButton,
        QtCore.Qt.MouseButton.NoButton, Mod.NoModifier)
    check(controller.viewEvent(view, release),
          "and its release is swallowed")

    radius = state.radiusWorld
    controller.keyEvent(key(Type.KeyPress, Key.Key_BracketRight), False)
    check(near(state.radiusWorld, radius * 1.1), "] grows the radius 10%")
    controller.keyEvent(key(Type.KeyPress, Key.Key_BracketLeft), False)
    check(near(state.radiusWorld, radius * 1.1 * 0.9),
          "[ shrinks it 10%")
    hardness = state.hardness
    controller.keyEvent(key(Type.KeyPress, Key.Key_BraceRight,
                            Mod.ShiftModifier), False)
    check(near(state.hardness, hardness + 0.1), "Shift+] adds 0.1 hardness")

    controller._pointer = None
    view.move(5000, 5000)
    passed = key(Type.ShortcutOverride, Key.Key_F)
    passed.ignore()
    claimable = controller.hotkeysClaimable()
    handled = controller.keyEvent(passed, True)
    check(handled == claimable and passed.isAccepted() == claimable,
          "with the pointer away F passes through to usdview")
    controller._pointer = (10.0, 10.0)
    state.binding = None
    passed = key(Type.ShortcutOverride, Key.Key_F)
    passed.ignore()
    check(not controller.keyEvent(passed, True) and not passed.isAccepted(),
          "with nothing bound F passes through to usdview")
    check(not controller.keyEvent(key(Type.KeyPress, Key.Key_Escape), False),
          "Escape passes through with no gesture live")

    class Broken(object):
        def type(self):
            raise RuntimeError("wrapped C++ object deleted")

    check(controller._viewFilter.eventFilter(view, Broken()) is False,
          "an exception inside the view filter passes the event through")
    check(controller._keyFilter.eventFilter(view, Broken()) is False,
          "and inside the key filter")
    check(messages and messages[-1][1] == "error",
          "and is reported to the status sink as an error")

    # -- palette focus trap: usdview's jealous focus keeps a spinbox ------
    palette = QtWidgets.QWidget(window)
    palette.setObjectName("usdGenBrushPaletteDock")
    spin = QtWidgets.QDoubleSpinBox(palette)
    foreign = QtWidgets.QLineEdit(window)
    state.binding = object()
    state.strokesArmed = True
    view.move(0, 0)
    window.show()
    window.activateWindow()
    spin.setFocus()
    app.processEvents()
    if QtWidgets.QApplication.focusWidget() is not spin:
        print("SKIP: offscreen window took no focus; focus-trap checks "
              "need an active window")
    else:
        controller._pointer = (10.0, 10.0)
        check(controller.hotkeyDecision() == (True, True, ""),
              "palette-held focus is claimed with a focus steal")
        radius = state.radiusWorld
        check(controller.keyEvent(key(Type.KeyPress, Key.Key_BracketRight),
                                  False)
              and near(state.radiusWorld, radius * 1.1),
              "] works while a palette spinbox holds focus")
        check(QtWidgets.QApplication.focusWidget() is not spin,
              "and focus left the palette spinbox")
        spin.setFocus()
        app.processEvents()
        controller._pointer = None
        enter = QtCore.QEvent(Type.Enter)
        controller.viewEvent(view, enter)
        check(QtWidgets.QApplication.focusWidget() is not spin,
              "the pointer entering the view takes focus from the palette")
        foreign.setFocus()
        app.processEvents()
        controller._pointer = (10.0, 10.0)
        blocked = key(Type.ShortcutOverride, Key.Key_BracketRight)
        blocked.ignore()
        check(not controller.keyEvent(blocked, True)
              and not blocked.isAccepted()
              and QtWidgets.QApplication.focusWidget() is foreign,
              "a text field outside the palette still blocks, keeping focus")
        controller.viewEvent(view, enter)
        check(QtWidgets.QApplication.focusWidget() is foreign,
              "and hovering the view does not steal from it")
        view.setFocus()
    before = len(messages)
    state.binding = None
    controller._pointer = (10.0, 10.0)
    check(not controller.keyEvent(key(Type.KeyPress, Key.Key_BracketRight),
                                  False)
          and len(messages) == before + 1
          and messages[-1][0] == brushPanels.HINT_UNBOUND,
          "unbound: the key passes through with a bind hint")
    state.binding = object()
    state.strokesArmed = False
    controller.keyEvent(key(Type.KeyPress, Key.Key_F), False)
    check(messages[-1][0] == brushPanels.HINT_DISARMED
          and controller.adjusting is None,
          "disarmed: F passes through with an arm hint")
    state.strokesArmed = True

    controller.onStageReplaced()
    check(controller.installed and controller._pointer is None
          and controller._hoverSnapshot is None,
          "stage replaced reinstalls and resets the hover cache")
    view.deleteLater()
    window.deleteLater()
    app.processEvents()
    check(controller.viewEvent(view, press) is False
          or not controller._viewAlive(),
          "a deleted StageView is never touched")
    controller.uninstall()
    check(not controller.installed, "uninstall")


def main():
    stagePath = sys.argv[1] if len(sys.argv) > 1 else None
    testStateDefaults()
    testDescriptors()
    testActions()
    testStatus()
    testHotkeys()
    testIcons()
    testLoopCadence()
    testLoopPressRelease()
    testLoopPreviewAndUndo()
    testLoopBindResolution(stagePath)
    testPalette()
    testViewportController()
    if FAILURES:
        print("testUsdGenToolsBrushUI: %d failure(s)" % len(FAILURES))
        return 1
    print("testUsdGenToolsBrushUI: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
