# testUsdviewPomadeBakeIsolation -- TN-7: a Tube-mode drag stays inside the
# TN-1 budget while a full bake of the REFERENCE SCALP is in flight
# (plan/17 section 7 TN-7, plan/18 V7).
#
#   testusdview --testScript \
#       plugin/usdGenPomadeTools/testenv/testUsdviewPomadeBakeIsolation.py \
#       examples/pomade-reference.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the other T3 pomade
# scripts use, plus the staged usdGenPomadeTools package and the
# usdGenPomade DLL.
#
# TN-7 reads: "a Tube-mode drag (TN-1 script) while a full bake of the
# reference scalp is in flight -- no move exceeds TN-1 by more than
# 0.5 ms; zero UI-thread time in the bake beyond the one-attribute swap".
# The bake worker owns its own thread and its own lowest-priority CUDA
# stream (plan/17 section 3.1a), so the claim under test is that neither
# steals from the gesture.
#
# The script therefore measures TWICE with the same gesture:
#
#   1. 50 real mouse moves with no bake running   -> the TN-1 baseline
#   2. the same 50 moves with a full reference-scalp bake in flight
#
# and holds the second against the first at the same statistic. It also
# checks after EVERY move that the bake was still in flight: a bake that
# finished early would make the comparison meaningless, so that is a
# failure, not a pass.
#
# The bake is made big enough to outlast the drag through the Output
# panel's texel resolution override (PomadeSession.setBakeTexelResolution):
# the stub reference scalp is only 64 faces and at the auto resolution the
# worker is done before the drag starts.
#
# The override has to be in force BEFORE the graph edit that triggers the
# bake, and this is the one ordering rule the script turns on. A graph
# gesture's release already enqueues a bake for the map version it just
# produced (GraphLoop.endOfEdit), and the worker tracks progress by map
# version alone: a second enqueue of the SAME version -- which is all a
# rebake() after the release can be -- leaves pending equal to completed
# the moment the release's own bake lands, so the worker can be baking
# hard while every in-flight test reads idle. Setting the resolution
# first makes the release's bake the big one, and then pending > completed
# means exactly what it says.
import ctypes
import os
import sys
import time

failures = 0

# How big the bake is forced to be: per-face texels per side. At the auto
# resolution the 64-face stub reference scalp bakes in well under the 50
# moves and the gate would pass without ever overlapping the drag.
# Measured on this scene: 128 -> 43 ms, 512 -> 287 ms, 1024 -> 993 ms,
# against a drag of about 40 ms, so 1024 gives the whole drag roughly a
# 20x margin of bake to run inside. MIN_OVERLAP below enforces that the
# overlap really happened rather than trusting the arithmetic.
BAKE_TEXELS = 1024
# Moves per drag, matching the TN-1 script in testUsdviewPomadeTube.py.
MOVES = 50
# TN-7's allowance over TN-1, and TN-1's own budget (plan/17 section 7).
ALLOWANCE_MS = 0.5
TN1_BUDGET_MS = 8.0
# How many of the drag's moves must coincide with the bake for the
# measurement to mean anything.
MIN_OVERLAP = 8
# How long the drain at the end waits for the forced bake to land. It is
# a bound on a spin over the idle pump, not a timing assumption: the test
# fails if the bake has not finished by then, because a bake still
# running at teardown would be joined by the worker's destructor with no
# cancellation point inside a 64-face chunk.
DRAIN_LIMIT_S = 120.0


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


def frameScalp(stage, view):
    """A camera looking straight down at the reference scalp."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/PomadeBakeCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.05, 500.0))
    eye = Gf.Vec3d(0.0, 6.0, 0.0)
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
        "/PomadeBakeCamera")
    return view.getActiveSceneCamera() is not None


def frameTube(stage, view):
    """Look across the vertical tube so its center handles do not overlap."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/PomadeBakeTubeCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.05, 500.0))
    eye = Gf.Vec3d(8.0, 1.5, 8.0)
    target = Gf.Vec3d(0.0, 1.5, 0.0)
    zAxis = (eye - target).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0.0, 1.0, 0.0), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = next((candidate for candidate in xf.GetOrderedXformOps()
               if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform),
              None)
    (op if op is not None else xf.AddTransformOp()).Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/PomadeBakeTubeCamera")
    return view.getActiveSceneCamera() is not None


def typeKey(view, name):
    import importlib
    from pxr.Usdviewq.qt import QtCore, PySideModule
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    keys = {"1": QtCore.Qt.Key.Key_1, "2": QtCore.Qt.Key.Key_2,
            "d": QtCore.Qt.Key.Key_D}
    QtTest.QTest.keyClick(view, keys[name],
                          QtCore.Qt.KeyboardModifier.NoModifier)


def wait(milliseconds=30):
    import importlib
    from pxr.Usdviewq.qt import PySideModule
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    QtTest.QTest.qWait(int(milliseconds))


def bakeVersions(session):
    """(pending, completed) map versions on the bake worker."""
    if session.bake is None:
        return (0, 0)
    return (int(session.dll.Pomade_BakePendingVersion(session.bake)),
            int(session.dll.Pomade_BakeCompletedVersion(session.bake)))


def bakeInFlight(session):
    pending, completed = bakeVersions(session)
    return pending > completed


def dragSamples(mouse, state, origin, witness=None):
    """One press / MOVES moves / release; the controller's per-move ms.

    `state.lastMoveMs` is what the controller measured end to end for the
    move it just handled -- model edit, publish and the updateGL request
    -- which is the quantity TN-1 is written against. `witness`, when
    given, is called after every move and its answers are returned
    beside the timings: that is how the loaded run proves the bake was
    running for the WHOLE drag and not just at its ends.
    """
    samples = []
    witnessed = []
    mouse.press(origin)
    for step in range(MOVES):
        offset = 4.0 * (1 if step % 2 == 0 else -1) * (1 + step % 5)
        mouse.move((origin[0] + offset, origin[1]))
        samples.append(float(state.lastMoveMs))
        if witness is not None:
            witnessed.append(bool(witness()))
    mouse.release(origin)
    return ([s for s in samples if s > 0.0], witnessed)


def median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2] if ordered else 0.0


def run(appController):
    global failures
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenPomadeTools
        from usdGenPomadeTools import pomadeCamera
        from testUsdviewPomadeGraph import Mouse
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    container = usdGenPomadeTools.container()
    check(view is not None and registry is not None and container is not None,
          "usdview built a StageView and the Pomade plugin registered")
    if view is None or registry is None or container is None:
        return 1

    scalpPath = "/World/Skin"
    check(bool(stage.GetPrimAtPath(scalpPath)),
          "the reference scene carries %s" % scalpPath)
    check(frameScalp(stage, view),
          "the scene camera looks down at the reference scalp")
    view.setFocus()

    dataModel.selection.setPrimPath(scalpPath)
    registry.getCommandPlugin("usdGenPomadeTools.openWorkspace").run()
    messages = []
    registry.getCommandPlugin("usdGenPomadeTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.pomadeState
    check(session is not None and session.model is not None,
          "Bind scalp created a live model on the reference scalp")
    check(viewport is not None and viewport.installed,
          "the viewport controller installed itself")
    if session is None or viewport is None or session.model is None:
        return 1
    session.setStatusSink(messages.append)
    check(session.bake is not None, "and started the bake worker")
    if session.bake is None:
        return 1
    # The other T3 scripts deactivate the stage scalp so the region tint
    # does not z-fight their pixel probes. This one reads no pixels, and
    # deactivating THIS scalp crashes the process at teardown -- see the
    # note below -- so it leaves the mesh alone.
    #
    # Deactivating /World/Skin on examples/pomade-reference.usda makes
    # usdview fault on the way out (0xC0000005) with no Pomade session in
    # the process at all: bind, deactivate the session, then
    # prim.SetActive(False) and let testusdview close. That is a
    # usdGenImaging teardown bug over a UsdGenRestAPI mesh the scene's
    # description references, not a tool bug, and it is out of this
    # script's scope.

    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the controller's camera resolves")
    if camera is None:
        return 1

    def pixel(x, z, y=0.0):
        projected = camera.worldToPixels((x, y, z))
        return (projected[0], projected[1])

    perPixel = camera.worldPerPixel((0.0, 0.0, 0.0))
    state.snapRadiusPx = max(0.05 / max(perPixel, 1e-9), 2.0)

    # -- a real stroke, so there is a region and a tube to drag -----------
    # The reference scalp spans [-1, 1] in x and z; the region is the
    # middle square, which is where the tube stands.
    rect = ((-0.5, -0.5), (0.5, -0.5), (0.5, 0.5), (-0.5, 0.5))
    mouse = Mouse(view)
    viewport.setPointerInside(True)
    typeKey(view, "d")
    path = []
    for k in range(len(rect)):
        x0, z0 = rect[k]
        x1, z1 = rect[(k + 1) % len(rect)]
        for i in range(5):
            t = float(i) / 5.0
            path.append(pixel(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t))
    path.append(pixel(*rect[0]))
    mouse.press(path[0])
    if not viewport.gestureActive:
        info("QtTest press did not land; using direct QMouseEvent delivery")
        mouse.direct = True
        mouse.press(path[0])
    for point in path[1:]:
        mouse.move(point)
    mouse.release(path[-1])
    nodes, edges, regions = session.graphCounts()
    check(regions == 1, "the drag stroked one closed region (%d/%d/%d)"
          % (nodes, edges, regions))
    cvCount = int(session.dll.Pomade_GetCenterCVCount(session.model))
    check(cvCount > 0, "and the release left a tube stub in it (G14)")
    if cvCount <= 0:
        return 1

    # -- Tube mode, and a CV under the cursor ------------------------------
    check(frameTube(stage, view),
          "the Tube gesture camera separates the vertical center handles")
    typeKey(view, "2")
    wait(30)
    check(state.activeMode == "tube", "the 2 hotkey selects Tube mode (%r)"
          % state.activeMode)

    def rawTip():
        """Authored cage state, retained as the edit witness."""
        out = (ctypes.c_float * 3)()
        if session.dll.Pomade_GetCenterCV(session.model, cvCount - 1, out) != 0:
            return None
        return (float(out[0]), float(out[1]), float(out[2]))

    def displayedTip():
        """The centered core handle shown to and picked by the artist."""
        out = (ctypes.c_float * 3)()
        if session.dll.Pomade_GetTubeCenterHandle(session.model, 0,
                                                 cvCount - 1, out) != 0:
            return None
        return (float(out[0]), float(out[1]), float(out[2]))

    def displayedTipPixel(handle):
        """Project against the live Tube-mode viewport, not Graph's layout."""
        liveCamera = pomadeCamera.resolve(view)
        if liveCamera is None or handle is None:
            return None
        projected = liveCamera.worldToPixels(handle)
        return ((projected[0], projected[1]) if projected is not None
                else None)

    tip = rawTip()
    tipHandle = displayedTip()
    check(tip is not None, "the tube's authored tip CV reads back")
    check(tipHandle is not None, "the tube's displayed tip core reads back")
    if tip is None or tipHandle is None:
        return 1
    # Opening Tube's dock changes the StageView rectangle.  The controller
    # resolves this camera on press, so stale Graph-mode pixels can land on
    # the adjacent CV rather than the displayed tip.
    tipPixel = displayedTipPixel(tipHandle)
    check(tipPixel is not None, "the Tube-mode viewport projects the tip")
    if tipPixel is None:
        return 1
    mouse.click(tipPixel)
    selected = session.readSelection(2)              # PomadePick_CenterCV
    check(selected == [(0, cvCount - 1, -1)],
          "one displayed-core click selected the tip center CV (%r)"
          % (selected,))

    # -- 1: the TN-1 baseline, nothing baking ------------------------------
    check(not bakeInFlight(session), "no bake is running for the baseline")
    baselineBefore = rawTip()
    baseline, _ = dragSamples(mouse, state, tipPixel)
    baselineAfter = rawTip()
    check(len(baseline) == MOVES,
          "the controller timed all %d baseline moves (%d)"
          % (MOVES, len(baseline)))
    check(baselineBefore is not None and baselineAfter is not None and
          any(abs(after - before) > 1e-5
              for before, after in zip(baselineBefore, baselineAfter)),
          "the baseline drag actually moved its selected authored CV")
    if not baseline:
        return 1
    baseMedian = median(baseline)
    info("TN-1 baseline: median %.3f ms, worst %.3f ms over %d moves"
         % (baseMedian, max(baseline), len(baseline)))

    # -- 2: the same drag with a full reference-scalp bake in flight -------
    # The forced resolution goes on FIRST (see the header): the graph
    # gesture below enqueues its own bake at release, and that is the
    # bake this half measures against.
    check(session.setBakeTexelResolution(BAKE_TEXELS) == BAKE_TEXELS,
          "the bake is forced to %d texels per face side (a full "
          "reference-scalp bake)" % BAKE_TEXELS)
    # Then a second region. The bake worker tracks progress by MAP
    # VERSION, so re-baking an unchanged graph enqueues the version that
    # is already completed and the pending/completed pair can never show
    # the work: it has to be a map the model has not baked yet. Drawing
    # another region is also what an artist does right before the rebake
    # they then keep working through, and it makes the bake bigger.
    # Graph remains a surface-space gesture, so restore its top-down camera
    # before projecting the second region.
    check(frameScalp(stage, view),
          "the second Graph stroke restores the scalp camera")
    typeKey(view, "1")
    wait(30)
    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the restored Graph camera resolves")
    if camera is None:
        return 1
    second = ((-0.9, 0.6), (-0.1, 0.6), (-0.1, 0.9), (-0.9, 0.9))
    secondPath = []
    for k in range(len(second)):
        x0, z0 = second[k]
        x1, z1 = second[(k + 1) % len(second)]
        for i in range(5):
            t = float(i) / 5.0
            secondPath.append(pixel(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t))
    secondPath.append(pixel(*second[0]))
    mapBefore = int(session.dll.Pomade_GetMapVersion(session.model))
    enqueuedAt = time.perf_counter()
    mouse.press(secondPath[0])
    for point in secondPath[1:]:
        mouse.move(point)
    mouse.release(secondPath[-1])
    mapAfter = int(session.dll.Pomade_GetMapVersion(session.model))
    check(mapAfter > mapBefore,
          "a second region advanced the map version (%d -> %d)"
          % (mapBefore, mapAfter))
    check(bakeInFlight(session),
          "the release enqueued it and the worker took it "
          "(pending %d > completed %d)" % bakeVersions(session))
    check(frameTube(stage, view),
          "the loaded Tube gesture camera separates the center handles")
    typeKey(view, "2")
    wait(30)
    # The baseline drag changed the authored cage, so refresh the visual
    # handle before the loaded gesture rather than reusing a stale cursor.
    tipHandle = displayedTip()
    if tipHandle is None:
        check(False, "the moved tip retains a displayed core handle")
        return 1
    tipPixel = displayedTipPixel(tipHandle)
    check(tipPixel is not None,
          "the refreshed Tube-mode viewport projects the moved tip")
    if tipPixel is None:
        return 1
    mouse.click(tipPixel)
    selected = session.readSelection(2)
    check(selected == [(0, cvCount - 1, -1)],
          "the refreshed core click selected the same tip before loaded timing")
    loadedBefore = rawTip()
    loaded, running = dragSamples(mouse, state, tipPixel,
                                  witness=lambda: bakeInFlight(session))
    loadedAfter = rawTip()
    check(len(loaded) == MOVES,
          "the controller timed all %d loaded moves (%d)"
          % (MOVES, len(loaded)))
    check(loadedBefore is not None and loadedAfter is not None and
          any(abs(after - before) > 1e-5
              for before, after in zip(loadedBefore, loadedAfter)),
          "the loaded drag actually moved its selected authored CV")
    # Only the moves that COINCIDED with the bake are evidence about the
    # bake. At BAKE_TEXELS the bake outlasts the whole drag on this box,
    # so this is normally every move; keeping the filter means a faster
    # box shortens the evidence instead of silently weakening the gate.
    overlapped = [ms for ms, wasBaking in zip(loaded, running) if wasBaking]
    check(len(overlapped) >= MIN_OVERLAP,
          "at least %d moves ran with the bake in flight (%d of %d) -- "
          "fewer would prove nothing; raise BAKE_TEXELS"
          % (MIN_OVERLAP, len(overlapped), len(running)))
    if not loaded or not overlapped:
        return 1
    loadedMedian = median(overlapped)
    info("TN-7 with the bake in flight: median %.3f ms, worst %.3f ms "
         "over %d overlapping moves (whole drag: median %.3f, worst %.3f "
         "over %d)"
         % (loadedMedian, max(overlapped), len(overlapped), median(loaded),
            max(loaded), len(loaded)))
    info("TN-7 delta against the baseline: median %+.3f ms, worst %+.3f ms "
         "(allowance %.1f)"
         % (loadedMedian - baseMedian, max(overlapped) - max(baseline),
            ALLOWANCE_MS))
    # Two readings of "no move exceeds TN-1 by more than 0.5 ms", both
    # gated. The absolute one is TN-1's own budget from plan/17 section 7
    # (8 ms tool-side per move) plus the allowance, and it is the stable
    # gate. The relative one holds this run's median against the same
    # run's unloaded median: comparing single worst moves of two
    # 50-sample sets of a sub-millisecond quantity would gate on the
    # spread of the baseline, not on the bake.
    check(max(overlapped) <= TN1_BUDGET_MS + ALLOWANCE_MS,
          "no move with the bake in flight exceeds TN-1's %.1f ms by more "
          "than %.1f (worst %.3f)"
          % (TN1_BUDGET_MS, ALLOWANCE_MS, max(overlapped)))
    check(loadedMedian <= baseMedian + ALLOWANCE_MS,
          "and the median move is within %.1f ms of the unloaded median "
          "(%.3f vs %.3f)" % (ALLOWANCE_MS, loadedMedian, baseMedian))

    # The one UI-thread cost TN-7 allows is the single-attribute swap the
    # pump does when the bake lands; nothing else may run on this thread.
    # The drain also has to finish before teardown: the worker's
    # destructor joins its thread, and a 64-face bake is one chunk with
    # no cancellation point inside it, so a bake still running here would
    # block the process exit for as long as it had left.
    drained = False
    while time.perf_counter() - enqueuedAt < DRAIN_LIMIT_S:
        viewport.pumpOnce()
        if not bakeInFlight(session):
            drained = True
            break
    bakeMs = (time.perf_counter() - enqueuedAt) * 1000.0
    check(drained, "the forced bake finished inside %.0f s (%.1f ms)"
          % (DRAIN_LIMIT_S, bakeMs))
    # And the pump does the one thing TN-7 allows it on this thread:
    # author the new map file on the RegionMap prim.
    while (int(state.bakedVersion) < mapAfter and
           time.perf_counter() - enqueuedAt < DRAIN_LIMIT_S):
        viewport.pumpOnce()
    check(int(state.bakedVersion) == mapAfter,
          "and the idle pump swapped it in (map v%d)"
          % int(state.bakedVersion))
    info("the forced bake ran %.1f ms against a %.1f ms drag, so every "
         "move above had it in flight" % (bakeMs, sum(loaded)))
    # Back to the bake's own plan, so the deactivate below is not racing
    # another forced bake.
    session.setBakeTexelResolution(0)

    viewport.uninstall()
    session.deactivate()
    print("testUsdviewPomadeBakeIsolation: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the TN-7 gate needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeBakeIsolation needs testusdview")
    sys.exit(0)
