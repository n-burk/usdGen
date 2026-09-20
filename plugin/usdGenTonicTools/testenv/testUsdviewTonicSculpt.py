# testUsdviewTonicSculpt -- T3: Sculpt mode driven by REAL mouse and key
# events (plan/18 V5 exit).
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicSculpt.py \
#               examples/tonic-graph-scalp.usda
#
# Proven here, all of it through the installed event filters:
#
#   * `5` selects Sculpt mode and builds SculptLoop;
#   * moving the mouse over the tube puts the brush ring under the cursor
#     (Tonic_SetBrushRing, which is what the viewport draws the ring from);
#   * a press-drag-release with the Grab brush moves the center CVs, and
#     the curve's arc length survives it to 1e-4 relative -- the
#     length-preserving default of plan/17 section 5.5, now enforced in
#     C++ by Tonic_SculptStrokeShaped rather than by the caller;
#   * `[` and `]` resize the brush;
#   * Escape in the middle of a stroke restores the press-time centers
#     BIT-EXACTLY, which is the whole point of the gesture bracket.
import ctypes
import math
import os
import sys

failures = 0

RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
TUBE_LENGTH = 3.0
CV_COUNT = 5


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
    """The directory this script lives in.

    testusdview EXECS the script, so `__file__` is not defined inside it;
    the path it was given is on the command line, which is where the other
    T3 scripts' `except NameError` branch gives up. This one looks.
    """
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


def _qtTest():
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    return importlib.import_module("%s.QtTest" % PySideModule)


def typeKey(view, name, modifiers=()):
    from pxr.Usdviewq.qt import QtCore
    QtTest = _qtTest()
    keys = {"escape": QtCore.Qt.Key.Key_Escape,
            "5": QtCore.Qt.Key.Key_5,
            "1": QtCore.Qt.Key.Key_1,
            "g": QtCore.Qt.Key.Key_G,
            "s": QtCore.Qt.Key.Key_S,
            "[": QtCore.Qt.Key.Key_BracketLeft,
            "]": QtCore.Qt.Key.Key_BracketRight}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    QtTest.QTest.keyClick(view, keys[name], mods)


def wait(ms=30):
    _qtTest().QTest.qWait(int(ms))


# ---------------------------------------------------------------------------
# The model, read back
# ---------------------------------------------------------------------------

def centers(session, tubeId=0):
    count = int(session.dll.Tonic_GetTubeCenterCount(session.model,
                                                     int(tubeId)))
    out = []
    for cv in range(max(count, 0)):
        xyz = (ctypes.c_float * 3)()
        if session.dll.Tonic_GetTubeCenterCV(session.model, int(tubeId), cv,
                                             xyz) != 0:
            return []
        out.append((float(xyz[0]), float(xyz[1]), float(xyz[2])))
    return out


def arcLength(points):
    total = 0.0
    for i in range(1, len(points)):
        a, b = points[i - 1], points[i]
        total += math.sqrt(sum((b[k] - a[k]) ** 2 for k in range(3)))
    return total


def brushRing(session):
    """(active, centre, radius) of the ring the viewport draws."""
    active = ctypes.c_int(0)
    centre = (ctypes.c_float * 3)()
    normal = (ctypes.c_float * 3)()
    radius = ctypes.c_float(0.0)
    if session.dll.Tonic_GetBrushRing(session.model, ctypes.byref(active),
                                      centre, normal,
                                      ctypes.byref(radius)) != 0:
        return (False, None, 0.0)
    return (bool(active.value),
            (float(centre[0]), float(centre[1]), float(centre[2])),
            float(radius.value))


def aimCamera(stage, view, eye, target):
    """A scene camera at `eye` looking at `target`, +Y up, activated."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicSculptCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(*eye)
    zAxis = (eye - Gf.Vec3d(*target)).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0.0, 1.0, 0.0), zAxis).GetNormalized()
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
        "/TonicSculptCamera")
    return view.getActiveSceneCamera() is not None


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

def run(appController):
    global failures
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import tonicCamera
        from usdGenTonicTools import tonicLib as tonicLibProbe
        from testUsdviewTonicGraph import Mouse
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    if view is None or registry is None:
        print("FAIL: no stage view / plugin registry")
        return 1
    container = usdGenTonicTools.container()
    if container is None:
        print("FAIL: the Tonic plugin container did not register")
        return 1

    # Down at the scalp first: the region has to be stroked from above.
    check(aimCamera(stage, view, (2.0, 14.0, 2.0001), (2.0, 0.0, 2.0)),
          "the scene camera looks down at the scalp")
    view.setFocus()
    wait(50)

    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    messages = []
    if container.session is not None:
        container.session.setStatusSink(messages.append)
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    check(session is not None and session.model is not None,
          "Bind scalp created a live model (status %r)" % (messages[-2:],))
    check(viewport is not None and viewport.installed,
          "the viewport controller installed itself")
    if session is None or viewport is None or session.model is None:
        return 1
    session.setStatusSink(messages.append)
    stage.GetPrimAtPath("/Scalp").SetActive(False)

    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the controller's camera did not resolve")
        return 1
    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)

    mouse = Mouse(view)
    path = []
    for k in range(len(RECT)):
        x0, z0 = RECT[k]
        x1, z1 = RECT[(k + 1) % len(RECT)]
        for i in range(5):
            t = float(i) / 5.0
            hit = camera.worldToPixels((x0 + (x1 - x0) * t, 0.0,
                                        z0 + (z1 - z0) * t))
            path.append((hit[0], hit[1]))
    first = camera.worldToPixels((RECT[0][0], 0.0, RECT[0][1]))
    path.append((first[0], first[1]))
    mouse.press(path[0])
    if not viewport.gestureActive:
        info("QtTest press did not land; using direct QMouseEvent delivery")
        mouse.direct = True
        mouse.press(path[0])
    for point in path[1:]:
        mouse.move(point)
    mouse.release(path[-1])
    check(session.graphCounts() == (4, 4, 1),
          "the drag stroked one closed region (%r)"
          % (session.graphCounts(),))
    check(session.dll.Tonic_BuildTubeFromRegion(
        session.model, 0, CV_COUNT, 8, ctypes.c_float(TUBE_LENGTH)) == 0,
        "a tube builds from the stroked region")
    session.publish()

    # -- side on, so the tube spans the frame ------------------------------
    check(aimCamera(stage, view, (13.0, 1.5, 2.0), (2.0, 1.5, 2.0)),
          "the camera moves round to the side of the tube")
    wait(50)
    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the side camera did not resolve")
        return 1

    base = centers(session)
    check(len(base) == CV_COUNT, "the tube has %d center CVs (%d)"
          % (CV_COUNT, len(base)))
    baseLength = arcLength(base)
    info("arc length before the stroke: %.6f" % baseLength)

    def pixelOf(point):
        projected = camera.worldToPixels(point)
        return (projected[0], projected[1])

    # -- 5: Sculpt mode ----------------------------------------------------
    typeKey(view, "5")
    check(state.activeMode == "sculpt",
          "the 5 hotkey selects Sculpt mode (%r)" % state.activeMode)
    loop = viewport.loop
    check(loop is not None and loop.modeId == "sculpt",
          "and builds SculptLoop (%r)" % (loop,))
    check(loop is not None and loop.brush() == "grab",
          "opening in the Grab brush (%r)"
          % (loop.brush() if loop else None,))

    # -- the ring follows the cursor --------------------------------------
    state.brushRadiusPx = 60.0
    tipPixel = pixelOf(base[CV_COUNT - 2])
    # A hover has no mouse grab, so QtTest's global-coordinate round trip
    # delivers it to whatever sits under the REAL cursor rather than to
    # the view; the direct QMouseEvent goes to the widget and through the
    # same installed filter (the reason Mouse._send exists at all).
    mouse.direct = True
    mouse.move(tipPixel)
    wait(20)
    probe = session.pickItem(camera, tipPixel[0], tipPixel[1], 60.0,
                             tonicLibProbe.TONIC_PICK_CENTER_CV)
    info("probe pick at the cursor: %r (camera %dx%d)"
         % (probe, camera.width, camera.height))
    hitRay = camera.rayThrough(tipPixel[0], tipPixel[1])
    info("probe raycast: %r" % (session.raycast(hitRay[0], hitRay[1])
                                if hitRay else None,))
    active, ringCentre, ringRadius = brushRing(session)
    check(active and ringRadius > 0.0,
          "a hover puts the brush ring under the cursor (r = %.4f world)"
          % ringRadius)
    if ringCentre is not None:
        projected = camera.worldToPixels(ringCentre)
        check(projected is not None and
              abs(projected[0] - tipPixel[0]) < 40.0 and
              abs(projected[1] - tipPixel[1]) < 40.0,
              "and it sits where the cursor is (%r vs %r)"
              % (None if projected is None else (projected[0], projected[1]),
                 tipPixel))

    # -- `[` / `]` --------------------------------------------------------
    state.brushRadiusPx = 60.0
    typeKey(view, "]")
    check(state.brushRadiusPx > 60.0, "] grows the brush (%g)"
          % state.brushRadiusPx)
    typeKey(view, "[")
    typeKey(view, "[")
    check(state.brushRadiusPx < 60.0, "[ shrinks it (%g)"
          % state.brushRadiusPx)
    state.brushRadiusPx = 60.0
    state.sculptPreserveLength = True
    state.sculptMirrorX = False
    state.brushTRadius = 0.0

    # -- a real grab stroke ------------------------------------------------
    start = pixelOf(base[CV_COUNT - 2])
    mouse.press(start)
    if not viewport.gestureActive:
        info("the sculpt press did not open a gesture; status %r"
             % (messages[-3:],))
    check(viewport.gestureActive, "the press opened a sculpt gesture")
    for step in range(1, 5):
        mouse.move((start[0] + 12.0 * step, start[1]))
    mouse.release((start[0] + 48.0, start[1]))
    check(not viewport.gestureActive, "the release closed it")

    moved = centers(session)
    check(len(moved) == CV_COUNT, "the tube still has its CVs")
    delta = max(abs(moved[i][k] - base[i][k])
                for i in range(len(moved)) for k in range(3))
    check(delta > 1e-4, "the stroke moved the center CVs (max %.6f)" % delta)
    check(all(abs(moved[0][k] - base[0][k]) < 1e-4 for k in range(3)),
          "with the root pinned (%r -> %r)" % (base[0], moved[0]))
    movedLength = arcLength(moved)
    relative = abs(movedLength - baseLength) / max(baseLength, 1e-9)
    info("arc length after the stroke: %.6f (relative change %.2e)"
         % (movedLength, relative))
    check(relative <= 1e-4,
          "and preserved the arc length to 1e-4 relative (%.2e)" % relative)

    # -- Escape mid-stroke restores bit-exactly ---------------------------
    before = centers(session)
    start = pixelOf(before[CV_COUNT - 2])
    mouse.press(start)
    check(viewport.gestureActive, "a second stroke is live")
    mouse.move((start[0] + 20.0, start[1]))
    mouse.move((start[0] + 40.0, start[1]))
    during = centers(session)
    check(any(abs(during[i][k] - before[i][k]) > 1e-6
              for i in range(len(during)) for k in range(3)),
          "the CVs moved while the drag was live")
    typeKey(view, "escape")
    check(not viewport.gestureActive, "Escape closed the gesture")
    mouse.release((start[0] + 40.0, start[1]))
    restored = centers(session)
    check(restored == before,
          "and restored the press-time centers BIT-exactly")

    status = session.status()
    info("model v%d, last move %.3f ms, ladder step %d"
         % (status["modelVersion"], status["lastMoveMs"],
            status.get("ladderStep", 0)))
    check(status["lastMoveMs"] > 0.0,
          "the controller timed its sculpt moves (%.3f ms, budget 8)"
          % status["lastMoveMs"])

    typeKey(view, "1")
    check(state.activeMode == "graph", "1 leaves Sculpt for Graph")
    check(not brushRing(session)[0],
          "and the brush ring leaves with the mode")

    viewport.uninstall()
    session.deactivate()
    print("testUsdviewTonicSculpt: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicSculpt needs testusdview (no live view)")
    sys.exit(0)
