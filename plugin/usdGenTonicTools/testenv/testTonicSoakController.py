# testTonicSoakController -- TN-5 through the REAL viewport controller
# (plan/18 V7, plan/17 section 7 TN-5).
#
#   testusdview --testScript \
#       plugin/usdGenTonicTools/testenv/testTonicSoakController.py \
#       examples/tonic-graph-scalp.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the other T3 tonic
# scripts use, plus the staged usdGenTonicTools package and the
# usdGenTonic DLL.
#
# testTonicSoak.py beside this one drives the C ABI: it soaks the ENGINE.
# This one drives QtTest mouse and key gestures at the StageView, so every
# op takes the artist's whole path -- event filter, mode loop, K11 pick,
# model edit, publish, viewport refresh, idle pump -- and the budget it
# gates is the one the artist would feel. Both are kept: an engine
# regression and a controller regression are different bugs.
#
# Environment (ctest sets the first two; all have defaults):
#   USDGENTONIC_SOAK_MINUTES   wall-clock budget, default 30
#   USDGENTONIC_SOAK_SEED      RNG seed, default 7
#   USDGENTONIC_SOAK_CONTROL   0 drops the scheduler control, which is
#                              how a run proves the control is not the
#                              thing perturbing it (default 1)
#
# Gates (exit 0 PASS / 1 FAIL):
#   * no op over 33 ms ATTRIBUTABLE TO THE TOOL. An op is a whole
#     gesture -- a mode key, a press, four moves and a release -- plus
#     the event-loop work all of them posted, drained once at the end.
#     That is Qt's compressed case, the one a real session takes when
#     input outruns the painter, and it is the reading this gate keeps.
#     USDGENTONIC_SOAK_PACE=1 drains after EVERY input event instead and
#     reports per-frame figures beside it, which is the uncompressed
#     case and the unit TN-5 words its budget in; the two bracket the
#     truth and the gate does not move with the switch (plan/17 s7, V9).
#     Attribution uses the same
#     reasoning testTonicSoak.py settled on: an out-of-process scheduler
#     control samples its own sleep overshoot on the same box (no shared
#     GIL, no shared model), and an over-budget op whose timestamp falls
#     inside a control stall was descheduled, not slow. Windows' thread
#     CPU clock has a 15.6 ms quantum and cannot resolve a 33 ms frame,
#     which is why the control exists instead.
#   * no device-memory growth over the run (cudaMemGetInfo, after a
#     warm-up pass that touches every op so first-touch caches are inside
#     the baseline).
#   * no committer swap over TN-4's 5 ms in any idle slot.
#   * zero hard failures. Engine rejections (TONIC_ERROR with a clean
#     rollback) are artist-handled -- undo and carry on -- and capped at
#     25 per cent of iterations, as in testTonicSoak.py.
import collections
import ctypes
import gc
import glob
import os
import random
import subprocess
import sys
import threading
import time

failures = 0

FRAME_BUDGET_MS = 33.0
# How often the run says where it is. Frequent enough to show a trend
# inside a short debugging run, rare enough not to be part of the
# measurement.
REPORT_EVERY_S = 15.0
SWAP_BUDGET_MS = 5.0             # TN-4
# A control stall this many seconds either side of an over-budget op
# explains it. The control samples every ~5 ms, so the window only has to
# cover one op plus the control's own sampling gap.
CONTROL_WINDOW_S = 0.25
# The scalp is the 4x4 quad grid in XZ at y = 0; the region is the middle
# square, which is where the tube stands.
RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
CENTRE = (2.0, 2.0)

# An out-of-process sleeper. It shares the box and nothing else, so an
# overshoot here is the OS or the driver, never this tool.
CONTROL_SOURCE = (
    "import sys, time\n"
    "while True:\n"
    "    t0 = time.perf_counter()\n"
    "    time.sleep(0.005)\n"
    "    over = (time.perf_counter() - t0) * 1000.0 - 5.0\n"
    "    if over > 15.0:\n"
    "        sys.stdout.write('%.6f %.3f\\n' % (time.time(), over))\n"
    "        sys.stdout.flush()\n"
)


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def info(text):
    print("info: %s" % text)


def testenvDir():
    """The directory this script lives in (testusdview EXECs it)."""
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    argv = list(sys.argv)
    for i, arg in enumerate(argv):
        if arg == "--testScript" and i + 1 < len(argv):
            return os.path.dirname(os.path.abspath(argv[i + 1]))
        if arg.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(arg.split("=", 1)[1]))
    return ""


def deviceMemUsed():
    """Device bytes in use, or None when unmeasurable."""
    for cuda in ("cudart64_12.dll", "cudart64_11.dll"):
        try:
            runtime = ctypes.CDLL(cuda)
            break
        except OSError:
            runtime = None
    if runtime is not None:
        try:
            free = ctypes.c_size_t(0)
            total = ctypes.c_size_t(0)
            if runtime.cudaMemGetInfo(ctypes.byref(free),
                                      ctypes.byref(total)) == 0:
                return total.value - free.value
        except (OSError, AttributeError):
            pass
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=memory.used",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=30, check=True)
        return int(out.stdout.splitlines()[0].strip()) * 1024 * 1024
    except (OSError, subprocess.SubprocessError, ValueError, IndexError):
        return None


class SchedulerControl:
    """The out-of-process stall witness; empty when it cannot start."""

    def __init__(self):
        self.stalls = []
        self._proc = None
        self._thread = None
        # USDGENTONIC_SOAK_CONTROL=0 runs without it. The control is an
        # attribution device, not a measurement, so being able to take it
        # out is how a run proves the control is not itself the thing
        # perturbing the numbers.
        if os.environ.get("USDGENTONIC_SOAK_CONTROL", "1") == "0":
            return
        try:
            self._proc = subprocess.Popen(
                [sys.executable, "-c", CONTROL_SOURCE],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        except OSError:
            return
        self._thread = threading.Thread(target=self._drain, daemon=True)
        self._thread.start()

    def _drain(self):
        for line in self._proc.stdout:
            parts = line.split()
            if len(parts) == 2:
                try:
                    self.stalls.append((float(parts[0]), float(parts[1])))
                except ValueError:
                    pass

    @property
    def running(self):
        return self._proc is not None and self._proc.poll() is None

    def explains(self, stamp):
        """True when the control also stalled around `stamp`."""
        for when, _over in self.stalls:
            if abs(when - stamp) <= CONTROL_WINDOW_S:
                return True
        return False

    def stop(self):
        if self._proc is None:
            return
        try:
            self._proc.terminate()
            self._proc.wait(timeout=10)
        except (OSError, subprocess.SubprocessError):
            pass


def frameScalp(stage, view):
    """A camera looking straight down at the 4x4 scalp, and activate it."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicSoakCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(2.0, 14.0, 2.0)
    zAxis = Gf.Vec3d(0.0, 1.0, 0.0)
    xAxis = Gf.Vec3d(1.0, 0.0, 0.0)
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = None
    for candidate in xf.GetOrderedXformOps():
        if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform:
            op = candidate
            break
    if op is None:
        op = xf.AddTransformOp()
    op.Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/TonicSoakCamera")
    return view.getActiveSceneCamera() is not None


def sendKey(view, name, modifiers=()):
    """One key press through QtTest, seen by the app-level filter.

    `run()` shadows this with a paced `typeKey` that drains the event
    loop straight after, so the key and the work it causes are one
    frame; this raw form is what that wrapper calls.
    """
    import importlib
    from pxr.Usdviewq.qt import QtCore, PySideModule
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    keys = {"1": QtCore.Qt.Key.Key_1, "2": QtCore.Qt.Key.Key_2,
            "3": QtCore.Qt.Key.Key_3, "4": QtCore.Qt.Key.Key_4,
            "5": QtCore.Qt.Key.Key_5, "6": QtCore.Qt.Key.Key_6,
            "d": QtCore.Qt.Key.Key_D, "m": QtCore.Qt.Key.Key_M,
            "z": QtCore.Qt.Key.Key_Z, "y": QtCore.Qt.Key.Key_Y,
            "up": QtCore.Qt.Key.Key_Up, "down": QtCore.Qt.Key.Key_Down,
            "[": QtCore.Qt.Key.Key_BracketLeft,
            "]": QtCore.Qt.Key.Key_BracketRight,
            "escape": QtCore.Qt.Key.Key_Escape}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    QtTest.QTest.keyClick(view, keys[name], mods)


def childrenOf(session, tubeId):
    out = (ctypes.c_int * 64)()
    got = ctypes.c_int(0)
    if session.dll.Tonic_GetTubeChildren(session.model, int(tubeId), out, 64,
                                         ctypes.byref(got)) != 0:
        return []
    return [int(out[i]) for i in range(int(got.value))]


def tubeCount(session):
    return int(session.dll.Tonic_GetTubeCount(session.model))


def pct(data, quantile):
    if not data:
        return 0.0
    ordered = sorted(data)
    return ordered[min(len(ordered) - 1, int(quantile * len(ordered)))]


# ---- growth probes --------------------------------------------------------
#
# The V7 run found throughput falling 5x and the committer swap reaching
# 26 ms while device memory, the Qt object count, the tube count and the
# undo depth were all flat. "Something host-side grows" is not a finding,
# so the run now samples the candidates by name every REPORT_EVERY_S and
# prints them as a row: whatever climbs with the curve is the cause.

def processPrivateBytes():
    """Windows private working set, or 0 where unavailable."""
    try:
        class _Counters(ctypes.Structure):
            _fields_ = [("cb", ctypes.c_uint32),
                        ("PageFaultCount", ctypes.c_uint32),
                        ("PeakWorkingSetSize", ctypes.c_size_t),
                        ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t),
                        ("PeakPagefileUsage", ctypes.c_size_t),
                        ("PrivateUsage", ctypes.c_size_t)]
        counters = _Counters()
        counters.cb = ctypes.sizeof(_Counters)
        psapi = ctypes.WinDLL("psapi.dll")
        kernel32 = ctypes.WinDLL("kernel32.dll")
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p,
                                               ctypes.c_void_p,
                                               ctypes.c_uint32]
        handle = kernel32.GetCurrentProcess()
        if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters),
                                      counters.cb):
            return int(counters.PrivateUsage)
    except (AttributeError, OSError, ValueError):
        pass
    return 0


def layerSpecCount(layer):
    """Prims + properties in an Sdf layer, counted by traversal."""
    if layer is None:
        return 0
    total = [0]

    def walk(prim):
        total[0] += 1 + len(prim.properties)
        for child in prim.nameChildren:
            walk(child)
    try:
        for root in layer.rootPrims:
            walk(root)
    except (AttributeError, RuntimeError):
        return -1
    return total[0]


def mapFileCount(session):
    """Versioned region-map files the bake has left on disk."""
    directory = getattr(session, "bakeDir", "") or ""
    if not directory:
        return -1
    return len(glob.glob(os.path.join(directory, "*.v*.ptx")))


def qtObjectCount(window):
    from pxr.Usdviewq.qt import QtCore
    try:
        return len(window.findChildren(QtCore.QObject))
    except (AttributeError, RuntimeError):
        return -1


def listItemCount(container):
    """Rows the workspace's warnings list is holding."""
    workspace = getattr(container, "workspace", None)
    widget = getattr(workspace, "_warningsList", None)
    try:
        return int(widget.count()) if widget is not None else -1
    except (AttributeError, RuntimeError):
        return -1


class Meter:
    """Call count and wall time for one wrapped callable.

    Problem 2 of the V7 soak is "roughly 20 ms of deferred work per op
    that is not yet named". Naming it needs the deferred queue broken
    down by who is in it, so the three candidates -- the dock refresh,
    the Storm draw and the idle pump -- are each wrapped and counted.
    """

    def __init__(self, label):
        self.label = label
        self.calls = 0
        self.ms = 0.0

    def wrap(self, fn):
        def wrapped(*args, **kwargs):
            t0 = time.perf_counter()
            try:
                return fn(*args, **kwargs)
            finally:
                self.calls += 1
                self.ms += (time.perf_counter() - t0) * 1000.0
        return wrapped

    def take(self):
        calls, ms = self.calls, self.ms
        self.calls, self.ms = 0, 0.0
        return calls, ms


class GcMeter:
    """Wall time Python's cyclic collector spends, by generation.

    A leak that grows the tracked-object count does not only cost memory:
    every generation-2 pass walks every live object, and it lands
    wherever the interpreter happens to be -- inside a Usd change notice
    during the committer's swap as readily as inside an op. Timing the
    collector is how the swap's degradation is tied to the leak rather
    than asserted.
    """

    def __init__(self):
        self.calls = 0
        self.ms = 0.0
        self.gen2 = 0
        self._t0 = 0.0
        gc.callbacks.append(self._on)

    def _on(self, phase, info):
        if phase == "start":
            self._t0 = time.perf_counter()
            return
        self.calls += 1
        self.ms += (time.perf_counter() - self._t0) * 1000.0
        if int(info.get("generation", 0)) >= 2:
            self.gen2 += 1

    def stop(self):
        try:
            gc.callbacks.remove(self._on)
        except ValueError:
            pass

    def take(self):
        calls, ms, gen2 = self.calls, self.ms, self.gen2
        self.calls, self.ms, self.gen2 = 0, 0.0, 0
        return calls, ms, gen2


class Probe:
    """One row of the growth table, sampled on the progress cadence."""

    FIELDS = ("pyObjects", "privateMB", "liveSpecs", "ptxFiles", "usedLayers",
              "sessionSublayers", "qtObjects", "warnRows", "swapMs")

    def __init__(self, stage, container, window):
        self._stage = stage
        self._container = container
        self._window = window
        self.rows = []

    def census(self):
        """Live Python objects by type name.

        `gc.get_objects()` climbing while device memory, the Qt object
        count, the tube count and the undo depth are flat is the V7
        soak's unexplained host-side growth; the type that climbs with it
        names the leak.
        """
        counts = collections.Counter()
        for obj in gc.get_objects():
            counts[type(obj).__name__] += 1
        return counts

    def sample(self, elapsed, swapMs):
        session = getattr(self._container, "session", None)
        live = getattr(session, "_liveLayer", None)
        try:
            used = len(self._stage.GetUsedLayers())
            subs = len(self._stage.GetSessionLayer().subLayerPaths)
        except (AttributeError, RuntimeError):
            used, subs = -1, -1
        row = {
            "t": elapsed,
            "pyObjects": len(gc.get_objects()),
            "privateMB": processPrivateBytes() / 1048576.0,
            "liveSpecs": layerSpecCount(live),
            "ptxFiles": mapFileCount(session),
            "usedLayers": used,
            "sessionSublayers": subs,
            "qtObjects": qtObjectCount(self._window),
            "warnRows": listItemCount(self._container),
            "swapMs": swapMs,
        }
        self.rows.append(row)
        return row

    @staticmethod
    def format(row):
        return ("probe: py=%-8d priv=%7.1f MB  liveSpecs=%-6d ptx=%-5d "
                "usedLayers=%-3d subLayers=%-2d qtObj=%-6d warnRows=%-3d "
                "swap=%6.3f ms"
                % (row["pyObjects"], row["privateMB"], row["liveSpecs"],
                   row["ptxFiles"], row["usedLayers"],
                   row["sessionSublayers"], row["qtObjects"],
                   row["warnRows"], row["swapMs"]))

    def growth(self):
        """First-to-last delta per field, for the end-of-run table."""
        if len(self.rows) < 2:
            return []
        first, last = self.rows[0], self.rows[-1]
        return [(name, first[name], last[name]) for name in self.FIELDS]


def run(appController):
    global failures
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import tonicCamera, tonicPanels
        from testUsdviewTonicGraph import Mouse
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools: %s" % exc)
        return 1

    minutes = float(os.environ.get("USDGENTONIC_SOAK_MINUTES", "30"))
    seed = int(os.environ.get("USDGENTONIC_SOAK_SEED", "7"))
    budget = minutes * 60.0
    rng = random.Random(seed)
    info("soak: %.1f minute(s), seed %d" % (minutes, seed))

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    container = usdGenTonicTools.container()
    check(view is not None and registry is not None and container is not None,
          "usdview built a StageView and the Tonic plugin registered")
    if view is None or registry is None or container is None:
        return 1

    check(frameScalp(stage, view), "the scene camera looks down at the scalp")
    view.setFocus()

    # -- open the tool the way the artist does -----------------------------
    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    messages = []
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    check(session is not None and session.model is not None,
          "Bind scalp created a live model")
    check(viewport is not None and viewport.installed,
          "the viewport controller installed itself on the StageView")
    if session is None or viewport is None or session.model is None:
        return 1
    session.setStatusSink(messages.append)
    # The region tint mesh is coincident with /Scalp; deactivating the
    # stage mesh keeps the two from z-fighting for thirty minutes. The
    # model copied the scalp at bind time, so K1 still hits it.
    stage.GetPrimAtPath("/Scalp").SetActive(False)

    camera = tonicCamera.resolve(view)
    check(camera is not None, "the controller's camera resolves")
    if camera is None:
        return 1

    def pixel(x, z, y=0.0):
        projected = camera.worldToPixels((x, y, z))
        return (projected[0], projected[1])

    perPixel = camera.worldPerPixel((2.0, 0.0, 2.0))
    state.snapRadiusPx = max(0.1 / max(perPixel, 1e-9), 2.0)

    # -- frames ------------------------------------------------------------
    #
    # TN-5 words its budget per FRAME, and the first version of this
    # script charged a whole multi-sample gesture -- a key, a press, four
    # moves and a release -- to one 33 ms frame, plus everything the
    # event loop owed all of them. A press and its consequences ARE a
    # frame; seven of them are seven. So every synthetic input event
    # drains the loop straight after it, which is also what the artist's
    # running loop does between two mouse samples, and each one is timed
    # on its own. The op-level numbers are still recorded (an op's budget
    # is its frame count times the frame budget), so nothing is hidden.
    from pxr.Usdviewq.qt import QtWidgets
    application = QtWidgets.QApplication.instance()
    frames = []                 # every frame's wall ms
    frameDrains = []            # the drain half of each frame
    frameOver = []              # (stamp, op, ms) over FRAME_BUDGET_MS
    opFrames = [0]              # frames this op has spent
    opName = [""]
    recording = [False]

    # USDGENTONIC_SOAK_PACE=1 drains after EVERY input event, so each
    # one is a frame of its own. It is off by default and the gate does
    # not change with it, because the two readings bracket the truth
    # rather than replacing each other: Qt compresses a burst of mouse
    # moves into one paint when input outruns the painter (the default,
    # one drain per gesture) and paints each sample when it does not
    # (paced). Turning it on is how a run answers "what does ONE frame
    # cost", which is the unit TN-5 words its budget in.
    paced = os.environ.get("USDGENTONIC_SOAK_PACE", "0") != "0"

    def pace(fn):
        """Run one input event; when paced, drain what it posted too."""
        if not paced:
            return fn

        def wrapped(*args, **kwargs):
            t0 = time.perf_counter()
            try:
                return fn(*args, **kwargs)
            finally:
                t1 = time.perf_counter()
                application.processEvents()
                t2 = time.perf_counter()
                if recording[0]:
                    ms = (t2 - t0) * 1000.0
                    frames.append(ms)
                    frameDrains.append((t2 - t1) * 1000.0)
                    opFrames[0] += 1
                    if ms > FRAME_BUDGET_MS:
                        frameOver.append((time.time(), opName[0], ms))
        return wrapped

    def typeKey(v, name, modifiers=()):
        """The paced key press every op below uses."""
        pace(sendKey)(v, name, modifiers)

    # -- a real stroke, so there is a region and a tube to edit ------------
    mouse = Mouse(view)
    viewport.setPointerInside(True)
    typeKey(view, "d")
    for _method in ("press", "move", "release"):
        setattr(mouse, _method, pace(getattr(mouse, _method)))
    path = []
    for k in range(len(RECT)):
        x0, z0 = RECT[k]
        x1, z1 = RECT[(k + 1) % len(RECT)]
        for i in range(5):
            t = float(i) / 5.0
            path.append(pixel(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t))
    path.append(pixel(*RECT[0]))
    mouse.press(path[0])
    if not viewport.gestureActive:
        info("QtTest press did not land; using direct QMouseEvent delivery")
        mouse.direct = True
        mouse.press(path[0])
    for point in path[1:]:
        mouse.move(point)
    mouse.release(path[-1])
    check(session.graphCounts()[2] == 1,
          "the drag stroked one closed region (%r)" % (session.graphCounts(),))
    check(int(session.dll.Tonic_GetCenterCVCount(session.model)) > 0,
          "and the release left a tube stub in it (G14)")

    # A two-level groom: the soak's subdivide/merge pair churns L2 under
    # this L1, so the fanout stays bounded while topology keeps moving.
    state.snapRadiusPx = 200.0
    state.subdivideCount = 4
    typeKey(view, "4")
    tipHeight = 0.0
    cvOut = (ctypes.c_float * 3)()
    if session.dll.Tonic_GetCenterCV(session.model, 4, cvOut) == 0:
        tipHeight = float(cvOut[1])
    tubePixel = pixel(CENTRE[0], CENTRE[1], tipHeight * 0.5)
    mouse.click(tubePixel)
    typeKey(view, "d", ("shift",))
    check(len(childrenOf(session, 0)) == 4,
          "Shift+D built the L2 fanout the soak churns (%r)"
          % (childrenOf(session, 0),))
    baseTubes = tubeCount(session)
    info("starting fanout: %d tube(s)" % baseTubes)

    # -- the op table ------------------------------------------------------
    # Every op is a real Qt gesture or a real panel write; each is timed
    # end to end, which includes the publish and the viewport refresh the
    # controller schedules.
    swaps = []

    def offsetPixel(spread):
        return (tubePixel[0] + rng.uniform(-spread, spread),
                tubePixel[1] + rng.uniform(-spread, spread))

    def opModeSwitch():
        typeKey(view, rng.choice(("1", "2", "3", "4", "5", "6")))

    def opHover():
        mouse.move(offsetPixel(40.0))

    def opPick():
        typeKey(view, "2")
        mouse.click(offsetPixel(30.0))

    def opTubeDrag():
        typeKey(view, "2")
        start = offsetPixel(20.0)
        mouse.press(start)
        for step in range(1, 5):
            mouse.move((start[0] + 6.0 * step, start[1] + 2.0 * step))
        mouse.release((start[0] + 24.0, start[1] + 8.0))

    def opSculptStroke():
        typeKey(view, "5")
        state.brushRadiusPx = 60.0
        start = offsetPixel(20.0)
        mouse.press(start)
        for step in range(1, 5):
            mouse.move((start[0] + 8.0 * step, start[1]))
        mouse.release((start[0] + 32.0, start[1]))

    def opBrushResize():
        typeKey(view, "5")
        typeKey(view, "]" if rng.random() < 0.5 else "[")

    def opGraphStroke():
        # Kept bounded: every stroke is undone straight away, so thirty
        # minutes of drawing does not grow the graph without limit.
        typeKey(view, "1")
        start = pixel(0.4 + rng.uniform(0.0, 0.2), 0.4 + rng.uniform(0.0, 0.2))
        mouse.press(start)
        for step in range(1, 4):
            mouse.move((start[0] + 10.0 * step, start[1] + 6.0 * step))
        mouse.release((start[0] + 30.0, start[1] + 18.0))
        typeKey(view, "z", ("ctrl",))

    def opLevelWalk():
        typeKey(view, "4")
        typeKey(view, "down" if rng.random() < 0.5 else "up", ("ctrl",))

    def opTopology():
        # Merge the children away and split them back: topology churn plus
        # the undo of topology, the op testTonicSoak.py found the engine's
        # one-way-door class in.
        typeKey(view, "4")
        mouse.click(tubePixel)
        if childrenOf(session, 0):
            typeKey(view, "m", ("shift",))
        else:
            typeKey(view, "d", ("shift",))

    def opFillDensity():
        typeKey(view, "3")
        session.select(1, [0], None, None, 0)
        descriptors = {d.id: d for d in tonicPanels.descriptors("fill", state)}
        density = descriptors.get("density")
        if density is not None:
            density.set(state, session, rng.choice((8.0, 16.0, 32.0)))

    def opUndoRedo():
        typeKey(view, "z" if rng.random() < 0.6 else "y", ("ctrl",))

    def opPump():
        viewport.pumpOnce()
        swaps.append(float(session.lastSwapMs))

    ops = {
        "mode-switch": opModeSwitch,
        "hover": opHover,
        "pick": opPick,
        "tube-drag": opTubeDrag,
        "sculpt-stroke": opSculptStroke,
        "brush-resize": opBrushResize,
        "graph-stroke": opGraphStroke,
        "level-walk": opLevelWalk,
        "topology": opTopology,
        "fill-density": opFillDensity,
        "undo-redo": opUndoRedo,
        "pump": opPump,
    }

    # Warm up every op before the memory baseline, so first-touch device
    # caches are inside it and not read later as a leak.
    for name in sorted(ops):
        try:
            ops[name]()
        except RuntimeError:
            if session.dll.Tonic_GetUndoDepth(session.model) > 0:
                session.undo()
    # The warm-up's topology op may have left the L1 childless; put the
    # fanout back and PROVE it, because a soak that starts degraded proves
    # nothing.
    typeKey(view, "4")
    mouse.click(tubePixel)
    if not childrenOf(session, 0):
        typeKey(view, "d", ("shift",))
    check(len(childrenOf(session, 0)) == 4,
          "the warm-up ends at the full fanout (%r)" % (childrenOf(session, 0),))

    control = SchedulerControl()
    info("scheduler control %s" % ("running" if control.running
                                   else "unavailable (attribution is "
                                        "wall-clock only)"))
    mem0 = deviceMemUsed()
    counts = {name: 0 for name in ops}
    rejects = {name: 0 for name in ops}
    samples = {name: [] for name in ops}
    # TN-5 words its budget per FRAME, so the gesture itself and the
    # deferred work the event loop owes it are timed apart and reported
    # apart: `samples` is the whole op (what the 33 ms gate reads),
    # `gestureMs` the Qt gestures alone, `deferredMs` the drain.
    gestureSamples = {name: [] for name in ops}
    deferredSamples = {name: [] for name in ops}
    worst = {name: 0.0 for name in ops}
    frameCount = {name: [] for name in ops}
    overEvents = []
    hard = []
    commits = []
    probe = Probe(stage, container,
                  getattr(appController, "_mainWindow", None) or view.window())
    throughput = []

    # Name the deferred work (problem 2). Each wrapper is an instance
    # attribute, so nothing outside this script sees them.
    meters = []
    workspace = getattr(container, "workspace", None)
    if workspace is not None:
        refreshMeter = Meter("dock-refresh")
        workspace.refresh = refreshMeter.wrap(workspace.refresh)
        meters.append(refreshMeter)
        warnMeter = Meter("dock-warnings")
        workspace._refreshWarnings = warnMeter.wrap(workspace._refreshWarnings)
        meters.append(warnMeter)
        paramMeter = Meter("dock-params")
        workspace._refreshParams = paramMeter.wrap(workspace._refreshParams)
        meters.append(paramMeter)
        stripMeter = Meter("dock-strip")
        workspace._refreshStatusStrip = stripMeter.wrap(
            workspace._refreshStatusStrip)
        meters.append(stripMeter)
    # Every event the application dispatches passes two PYTHON event
    # filters (usdview's AppEventFilter and the Tonic key filter), so the
    # count itself is a cost. Histogram it by type, so "409 events per
    # op" becomes a list of names.
    eventCounts = collections.Counter()
    eventFilter = None
    if os.environ.get("USDGENTONIC_SOAK_EVENTS", "0") != "0":
        from pxr.Usdviewq.qt import QtCore as _QtCore

        receiverCounts = collections.Counter()
        watched = {int(_QtCore.QEvent.Type.Move),
                   int(_QtCore.QEvent.Type.Resize),
                   int(_QtCore.QEvent.Type.Paint),
                   int(_QtCore.QEvent.Type.Create)}

        class _Counter(_QtCore.QObject):
            def eventFilter(self, obj, event):
                try:
                    kind = int(event.type())
                    eventCounts[kind] += 1
                    if kind in watched:
                        receiverCounts[(kind, type(obj).__name__)] += 1
                except (RuntimeError, ValueError):
                    pass
                return False

        eventFilter = _Counter()
        application.installEventFilter(eventFilter)

    statusMeter = Meter("abi-status")
    session.status = statusMeter.wrap(session.status)
    meters.append(statusMeter)
    paintMeter = Meter("storm-paint")
    view.paintGL = paintMeter.wrap(view.paintGL)
    meters.append(paintMeter)
    pumpMeter = Meter("idle-pump")
    session.pump = pumpMeter.wrap(session.pump)
    meters.append(pumpMeter)
    publishMeter = Meter("publish")
    session.publish = publishMeter.wrap(session.publish)
    meters.append(publishMeter)
    meterTotals = {m.label: [0, 0.0] for m in meters}

    # USDGENTONIC_SOAK_PROFILE=1 puts cProfile around the deferred drain
    # for the whole run. It distorts the absolute numbers, so it is off
    # for a gate run and on for a "what is IN the drain" run.
    profiler = None
    if os.environ.get("USDGENTONIC_SOAK_PROFILE", "0") != "0":
        import cProfile
        profiler = cProfile.Profile()
        info("cProfile is on: timings are inflated, attribution is not")

    census0 = probe.census()
    recording[0] = True
    gcMeter = GcMeter()
    gcTotal = [0, 0.0, 0]
    start = time.perf_counter()
    iterations = 0
    lastReportIt = 0
    nextReport = start + REPORT_EVERY_S
    while time.perf_counter() - start < budget:
        iterations += 1
        # A 30-minute test that prints nothing until the end tells you
        # nothing when it dies in the middle, and nothing about whether a
        # cost is constant or climbing.
        now = time.perf_counter()
        if now >= nextReport:
            nextReport = now + REPORT_EVERY_S
            throughput.append(iterations - lastReportIt)
            lastReportIt = iterations
            info("progress: %6.1f s  it=%-7d (+%d) tubes=%-3d undo=%-3d "
                 "worst-so-far %7.3f ms (%s)  device %s"
                 % (now - start, iterations, throughput[-1],
                    tubeCount(session),
                    int(session.dll.Tonic_GetUndoDepth(session.model)),
                    max(worst.values()),
                    max(worst, key=lambda k: worst[k]),
                    "%.1f MB" % (deviceMemUsed() / (1024.0 * 1024.0))
                    if deviceMemUsed() is not None else "n/a"))
            info(Probe.format(probe.sample(now - start,
                                           float(session.lastSwapMs))))
            window = max(1, throughput[-1])
            parts = []
            for meter in meters:
                calls, ms = meter.take()
                meterTotals[meter.label][0] += calls
                meterTotals[meter.label][1] += ms
                parts.append("%s %.2f/op x%.2f"
                             % (meter.label, ms / window,
                                float(calls) / window))
            gcCalls, gcMs, gcGen2 = gcMeter.take()
            gcTotal[0] += gcCalls
            gcTotal[1] += gcMs
            gcTotal[2] += gcGen2
            parts.append("gc %.2f/op x%.2f (%d gen2)"
                         % (gcMs / window, float(gcCalls) / window, gcGen2))
            info("deferred: " + "  ".join(parts))
            # The probe itself is not part of any op's measurement.
            nextReport = time.perf_counter() + REPORT_EVERY_S

        name = rng.choice(sorted(ops))
        opName[0] = name
        opFrames[0] = 0
        t0 = time.perf_counter()
        tGesture = t0
        try:
            ops[name]()
            tGesture = time.perf_counter()
            # An op posts as well as sends: the idle pump's timer, a
            # repaint request, a dock refresh. usdview drains those on
            # the next turn of its event loop, but a script runs BETWEEN
            # turns, so without this the whole backlog of every op since
            # the last one lands inside whichever op next spins the loop
            # (QTest's mouse move does) -- and that op is charged for all
            # of it. Draining here, inside this op's own timing, is both
            # what the artist's running loop does and the only way each
            # op's number is its own.
            if profiler is None:
                application.processEvents()
            else:
                profiler.enable()
                application.processEvents()
                profiler.disable()
        except RuntimeError as exc:
            # Engine rejection: artist handling is undo-and-continue, and
            # its cost belongs to this op's frame.
            if int(session.dll.Tonic_GetUndoDepth(session.model)) <= 0:
                hard.append("%s: %s (no undo to recover)" % (name, exc))
                continue
            session.undo()
            rejects[name] += 1
        except Exception as exc:                          # noqa: BLE001
            hard.append("%s: HARD %r [%s]" % (name, exc, session.lastError()))
            continue
        else:
            counts[name] += 1
        end = time.perf_counter()
        wallMs = (end - t0) * 1000.0
        samples[name].append(wallMs)
        gestureSamples[name].append((tGesture - t0) * 1000.0)
        deferredSamples[name].append((end - tGesture) * 1000.0)
        if wallMs > worst[name]:
            worst[name] = wallMs
        # An op's budget is its frame count times the frame budget: a
        # gesture of a key, a press, four moves and a release is seven
        # frames, and charging seven frames of work to one is what made
        # the V7 run read 61 per cent of ops as over budget.
        frameCount[name].append(opFrames[0])
        if wallMs > FRAME_BUDGET_MS:
            overEvents.append((time.time(), name, wallMs, iterations))
    elapsed = time.perf_counter() - start
    gcCalls, gcMs, gcGen2 = gcMeter.take()
    gcTotal[0] += gcCalls
    gcTotal[1] += gcMs
    gcTotal[2] += gcGen2
    gcMeter.stop()
    mem1 = deviceMemUsed()
    control.stop()

    # -- what happened -----------------------------------------------------
    nReject = sum(rejects.values())
    info("soak: %d iterations in %.1f s (%d rejects, %d hard failures)"
         % (iterations, elapsed, nReject, len(hard)))
    for name in sorted(counts):
        info("%-13s x%-6d +%3d rejects  p50 %6.3f  p99 %6.3f  p999 %6.3f  "
             "worst %7.3f ms   [gesture p50 %6.3f / deferred p50 %6.3f, "
             "deferred p99 %6.3f]"
             % (name, counts[name], rejects[name], pct(samples[name], 0.50),
                pct(samples[name], 0.99), pct(samples[name], 0.999),
                worst[name], pct(gestureSamples[name], 0.50),
                pct(deferredSamples[name], 0.50),
                pct(deferredSamples[name], 0.99)))
    allDeferred = [ms for name in ops for ms in deferredSamples[name]]
    info("deferred work per op: p50 %.3f  p99 %.3f  worst %.3f ms"
         % (pct(allDeferred, 0.50), pct(allDeferred, 0.99),
            max(allDeferred) if allDeferred else 0.0))
    if frames:
        info("frames per op: %.2f (%d frames over %d ops)"
             % (float(len(frames)) / max(1, iterations), len(frames),
                iterations))
    if profiler is not None:
        import io
        import pstats
        buffer = io.StringIO()
        pstats.Stats(profiler, stream=buffer).sort_stats("cumulative") \
            .print_stats(25)
        for line in buffer.getvalue().splitlines():
            if line.strip():
                info("profile| %s" % line.rstrip())
    if eventFilter is not None:
        from pxr.Usdviewq.qt import QtCore as _QtCore
        application.removeEventFilter(eventFilter)
        total = sum(eventCounts.values())
        info("application events: %d total, %.1f per op"
             % (total, float(total) / max(1, iterations)))
        for value, count in eventCounts.most_common(15):
            try:
                label = _QtCore.QEvent.Type(value).name
            except ValueError:
                label = "type%d" % value
            info("event %-26s x%-9d %7.1f per op"
                 % (label, count, float(count) / max(1, iterations)))
        for (value, cls), count in receiverCounts.most_common(15):
            try:
                label = _QtCore.QEvent.Type(value).name
            except ValueError:
                label = "type%d" % value
            info("receiver %-12s %-30s %7.1f per op"
                 % (label, cls, float(count) / max(1, iterations)))
    info("cyclic GC: %d collections (%d gen2), %.1f ms total, %.3f ms/op"
         % (gcTotal[0], gcTotal[2], gcTotal[1],
            gcTotal[1] / max(1, iterations)))
    if throughput:
        info("throughput per %.0f s: first %d, last %d, min %d, max %d"
             % (REPORT_EVERY_S, throughput[0], throughput[-1],
                min(throughput), max(throughput)))
    for label in sorted(meterTotals):
        calls, ms = meterTotals[label]
        info("deferred %-14s x%-8d %9.1f ms total  %6.3f ms/op  %5.2f calls/op"
             % (label, calls, ms, ms / max(1, iterations),
                float(calls) / max(1, iterations)))
    for field, first, last in probe.growth():
        info("growth %-17s %12s -> %12s"
             % (field, ("%.1f" % first) if isinstance(first, float)
                else str(first),
                ("%.1f" % last) if isinstance(last, float) else str(last)))
    if census0 is not None:
        census1 = probe.census()
        deltas = sorted(((census1[k] - census0.get(k, 0), k)
                         for k in census1), reverse=True)
        for delta, name in deltas[:12]:
            if delta <= 0:
                break
            info("census %-28s +%-8d (%.2f per op)"
                 % (name, delta, float(delta) / max(1, iterations)))
    if commits:
        info("commit enqueue x%d worst %.3f ms (release work, informational)"
             % (len(commits), max(commits)))
    if swaps:
        info("committer swap x%d worst %.3f ms (TN-4 budget %.1f)"
             % (len(swaps), max(swaps), SWAP_BUDGET_MS))

    # -- TN-5's own unit: the frame (USDGENTONIC_SOAK_PACE only) --------
    if frames:
        info("frames: %d, p50 %.3f  p99 %.3f  p999 %.3f  worst %.3f ms"
             % (len(frames), pct(frames, 0.50), pct(frames, 0.99),
                pct(frames, 0.999), max(frames)))
        info("frame drain (the event loop's share): p50 %.3f  p99 %.3f  "
             "worst %.3f ms"
             % (pct(frameDrains, 0.50), pct(frameDrains, 0.99),
                max(frameDrains)))
        frameAttributable = []
        for stamp, name, ms in frameOver:
            if not control.explains(stamp):
                frameAttributable.append((name, ms))
        for name, ms in frameAttributable[:10]:
            info("over frame budget: %-13s %8.3f ms (TOOL)" % (name, ms))
        check(not frameAttributable,
              "no FRAME over %.1f ms attributable to the tool (%d "
              "over-budget frame(s) of %d, %d explained by the scheduler "
              "control)"
              % (FRAME_BUDGET_MS, len(frameOver), len(frames),
                 len(frameOver) - len(frameAttributable)))

    attributable = []
    for stamp, name, ms, at in overEvents:
        explained = control.explains(stamp)
        info("over budget: %-13s %8.3f ms at it=%d %s"
             % (name, ms, at, "(scheduler stall, not tool-attributable)"
                if explained else "(TOOL)"))
        if not explained:
            attributable.append((name, ms, at))
    check(not attributable,
          "no op over %.1f ms attributable to the tool (%d over-budget "
          "event(s), %d explained by the scheduler control)"
          % (FRAME_BUDGET_MS, len(overEvents),
             len(overEvents) - len(attributable)))
    check(not hard, "no hard failures (%d)" % len(hard))
    if hard:
        for text in hard[:5]:
            info("hard: %s" % text)
    check(nReject <= iterations // 4,
          "rejects stay under a quarter of %d iterations (%d)"
          % (iterations, nReject))
    check(not swaps or max(swaps) <= SWAP_BUDGET_MS,
          "no committer swap over TN-4's %.1f ms (worst %.3f)"
          % (SWAP_BUDGET_MS, max(swaps) if swaps else 0.0))
    if mem0 is None or mem1 is None:
        info("device memory unmeasurable here (no cudart / nvidia-smi)")
    else:
        info("device used %.1f MB -> %.1f MB"
             % (mem0 / 1048576.0, mem1 / 1048576.0))
        # 4 MB is the same allowance testTonicSoak.py uses: below one
        # CUDA suballocation, far under anything a per-op leak would
        # reach over a 30-minute run.
        check(mem1 <= mem0 + 4 * 1048576,
              "device memory did not grow (%.1f MB)"
              % ((mem1 - mem0) / 1048576.0))

    viewport.uninstall()
    session.deactivate()
    print("testTonicSoakController: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the soak needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testTonicSoakController needs testusdview (no live view)")
    sys.exit(0)
