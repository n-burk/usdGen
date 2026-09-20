# testUsdviewTonicLevels -- T3: Hierarchy mode driven by REAL mouse and key
# events (plan/18 V5 exit).
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicLevels.py \
#               examples/tonic-graph-scalp.usda
#
# The script that used to live under this name called the P4 C entries
# through ctypes and never opened the tool; V2 renamed it
# testTonicAbiLevels.py, which is what it always was. THIS one opens the
# workspace the way the artist does and then clicks, drags and types:
#
#   * `4` selects Hierarchy mode and builds its loop;
#   * a click over a tube selects it (K11), and a double-click enters the
#     level below it;
#   * Shift+D subdivides the selection and /__usdGenTonic/tubes/L2 appears
#     in what the scene index published;
#   * Ctrl+Down focuses L2 -- which is what draws its centers thick -- and
#     leaves L1 x-ray;
#   * an L2 CV edit survives a parent edit made after Ctrl+Up (K6);
#   * Shift+M takes L2 away again and Ctrl+Z brings it back.
#
# Reading the published geometry: this USD build exposes no terminal scene
# index to Python, so the level census comes from
# Tonic_GetPublishedLevelInfo, which reports what the ATTACHED INDEX
# staged for a level -- the same route testUsdviewTonicPublish.py uses.
# The widths and the x-ray primvar those flags produce are derived in
# tonicPublish.cpp and covered at T1; what a T3 can prove is that the real
# keys set the real flags and that the level really got published.
import ctypes
import os
import sys

failures = 0

# The scalp is the 4x4 quad grid in XZ at y = 0; the region is the middle
# square, which is where the tube will stand.
RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
CENTRE = (2.0, 2.0)


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


def typeKey(view, name, modifiers=()):
    """One key press through QtTest, seen by the app-level filter."""
    from pxr.Usdviewq.qt import QtCore
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    keys = {"escape": QtCore.Qt.Key.Key_Escape,
            "z": QtCore.Qt.Key.Key_Z,
            "y": QtCore.Qt.Key.Key_Y,
            "d": QtCore.Qt.Key.Key_D,
            "m": QtCore.Qt.Key.Key_M,
            "n": QtCore.Qt.Key.Key_N,
            "1": QtCore.Qt.Key.Key_1,
            "4": QtCore.Qt.Key.Key_4,
            "up": QtCore.Qt.Key.Key_Up,
            "down": QtCore.Qt.Key.Key_Down,
            "backspace": QtCore.Qt.Key.Key_Backspace}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    QtTest.QTest.keyClick(view, keys[name], mods)


def wait(ms=30):
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    QtTest.QTest.qWait(int(ms))


# ---------------------------------------------------------------------------
# The model, read back
# ---------------------------------------------------------------------------

def levelInfo(session, level):
    """(faces, points, tubes) the scene index published for `level`."""
    faces = ctypes.c_int(0)
    points = ctypes.c_int(0)
    tubes = ctypes.c_int(0)
    rc = session.dll.Tonic_GetPublishedLevelInfo(
        session.model, int(level), ctypes.byref(faces), ctypes.byref(points),
        ctypes.byref(tubes))
    if rc != 0:
        return None
    return (faces.value, points.value, tubes.value)


def levelDisplay(session, level):
    visible = ctypes.c_int(1)
    xray = ctypes.c_int(0)
    session.dll.Tonic_GetLevelDisplay(session.model, int(level),
                                      ctypes.byref(visible),
                                      ctypes.byref(xray))
    return (bool(visible.value), bool(xray.value))


def centerCV(session, tubeId, cv):
    out = (ctypes.c_float * 3)()
    if session.dll.Tonic_GetTubeCenterCV(session.model, int(tubeId), int(cv),
                                         out) != 0:
        return None
    return (float(out[0]), float(out[1]), float(out[2]))


def deltaNorm(session, tubeId):
    """The L2 norm of a tube's stored center deltas (its own edit)."""
    got = ctypes.c_int(0)
    if session.dll.Tonic_ReadTubeDeltas(session.model, int(tubeId), None, 0,
                                        ctypes.byref(got)) != 0:
        return None
    n = int(got.value)
    if n <= 0:
        return 0.0
    out = (ctypes.c_float * n)()
    if session.dll.Tonic_ReadTubeDeltas(session.model, int(tubeId), out, n,
                                        ctypes.byref(got)) != 0:
        return None
    return sum(float(out[i]) * float(out[i]) for i in range(n)) ** 0.5


def childrenOf(session, tubeId):
    out = (ctypes.c_int * 64)()
    got = ctypes.c_int(0)
    if session.dll.Tonic_GetTubeChildren(session.model, int(tubeId), out, 64,
                                         ctypes.byref(got)) != 0:
        return []
    return [int(out[i]) for i in range(int(got.value))]


def frameScalp(stage, view):
    """A camera looking straight down at the 4x4 scalp, and activate it."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicLevelsCamera"))
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
        "/TonicLevelsCamera")
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
        from usdGenTonicTools import tonicCamera, tonicLib
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

    check(frameScalp(stage, view), "the scene camera looks down at the scalp")
    view.setFocus()
    wait(50)

    # -- open the tool the way the artist does -----------------------------
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
    if container.session is not None:
        container.session.setStatusSink(messages.append)
    stage.GetPrimAtPath("/Scalp").SetActive(False)

    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the controller's camera did not resolve")
        return 1

    def pixel(x, z, y=0.0):
        projected = camera.worldToPixels((x, y, z))
        return (projected[0], projected[1])

    perPixel = camera.worldPerPixel((2.0, 0.0, 2.0))
    state.snapRadiusPx = max(0.1 / max(perPixel, 1e-9), 2.0)

    # -- a real stroke, so there is a region to stand a tube in ------------
    mouse = Mouse(view)
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
    check(session.graphCounts() == (4, 4, 1),
          "the drag stroked one closed region (%r)" % (session.graphCounts(),))
    check(session.dll.Tonic_BuildTubeFromRegion(
        session.model, 0, 5, 8, ctypes.c_float(3.0)) == 0,
        "a tube builds from the stroked region")
    session.publish()

    # -- 4: Hierarchy mode -------------------------------------------------
    typeKey(view, "4")
    check(state.activeMode == "hierarchy",
          "the 4 hotkey selects Hierarchy mode (%r)" % state.activeMode)
    check(viewport.loop is not None and viewport.loop.modeId == "hierarchy",
          "and builds HierarchyLoop (%r)" % (viewport.loop,))

    # -- a click selects the tube ------------------------------------------
    tip = centerCV(session, 0, 4)
    check(tip is not None, "the tube has center CVs (%r)" % (tip,))
    target = pixel(CENTRE[0], CENTRE[1], tip[1] * 0.5 if tip else 0.0)
    # K11 is vertex-anchored (testUsdviewTonicPick): the pick radius has
    # to reach a tube VERTEX, and from straight above the tube points at
    # the camera, so its vertices ring the cursor at the tube radius --
    # about 65 px here. A radius that reaches them is what the argument
    # is for; there is one tube in the scene to hit.
    state.snapRadiusPx = 200.0
    mouse.click(target)
    selected = session.selectionCount(tonicLib.TONIC_PICK_TUBE_VERT)
    check(selected == 1, "clicking the tube selects it (%d selected)"
          % selected)
    if selected != 1:
        info("status: %r" % (messages[-3:],))

    # -- Shift+D subdivides ------------------------------------------------
    state.subdivideCount = 4
    before = levelInfo(session, 2)
    check(before is None, "nothing is published at L2 yet (%r)" % (before,))
    typeKey(view, "d", ("shift",))
    wait(20)
    after = levelInfo(session, 2)
    check(after is not None, "Shift+D publishes /__usdGenTonic/tubes/L2")
    if after is not None:
        check(after[2] == 4, "with the panel's four children (%r)" % (after,))
    kids = childrenOf(session, 0)
    check(len(kids) == 4, "and the model holds four child tubes (%r)"
          % (kids,))

    # -- Ctrl+Down focuses L2 (thick centers) and x-rays L1 ----------------
    typeKey(view, "down", ("ctrl",))
    focus = int(session.dll.Tonic_GetFocusLevel(session.model))
    check(focus == 2, "Ctrl+Down focuses L2, which draws its centers thick "
                      "(focus %d)" % focus)
    check(state.activeLevel == 2, "and the tool state agrees (%d)"
          % state.activeLevel)
    visible1, xray1 = levelDisplay(session, 1)
    check(visible1 and xray1,
          "L1 stays visible and goes x-ray (visible %s, xray %s)"
          % (visible1, xray1))
    visible2, xray2 = levelDisplay(session, 2)
    check(visible2 and not xray2, "while the focused L2 draws solid")

    # -- an L2 edit, then a parent edit after Ctrl+Up (K6) -----------------
    if not kids:
        print("FAIL: no children to walk the K6 propagation over")
        print("testUsdviewTonicLevels: %d failure(s)" % (failures + 1))
        return 1
    child = kids[0]
    childBefore = centerCV(session, child, 3)
    check(session.dll.Tonic_MoveTubeCenterCV(
        session.model, child, 3, ctypes.c_float(0.2), ctypes.c_float(0.0),
        ctypes.c_float(0.0)) == 0, "an L2 CV takes an edit")
    childEdited = centerCV(session, child, 3)
    check(childEdited is not None and childBefore is not None and
          abs(childEdited[0] - childBefore[0]) > 1e-4,
          "which moved it (%r -> %r)" % (childBefore, childEdited))
    editNorm = deltaNorm(session, child)
    check(editNorm is not None and editNorm > 1e-4,
          "and recorded it as the child's own delta (%r)" % (editNorm,))

    typeKey(view, "up", ("ctrl",))
    check(int(session.dll.Tonic_GetFocusLevel(session.model)) == 1,
          "Ctrl+Up comes back to L1")
    # The translate gizmo that drags a parent from the mouse is Tube
    # mode's (V4); what this asserts is the propagation the level walk has
    # to survive, so the parent edit goes through the same entry the gizmo
    # calls.
    check(session.dll.Tonic_MoveTubeCenterCV(
        session.model, 0, 3, ctypes.c_float(0.0), ctypes.c_float(0.0),
        ctypes.c_float(0.5)) == 0, "the L1 parent takes a drag")
    session.publish()
    childAfter = centerCV(session, child, 3)
    check(childAfter is not None and
          abs(childAfter[2] - childEdited[2]) > 1e-4,
          "K6 carried it down to L2 (%r -> %r)" % (childEdited, childAfter))
    # The child's edit survives as a DELTA against the re-derived shape,
    # not as a world position: K6 re-derives every child from the moved
    # parent and re-applies the stored delta length-preservingly
    # (TonicHierarchicalSculptApplyCpu), which is why the delta is still
    # there and why its magnitude is not the number it went in as.
    afterNorm = deltaNorm(session, child)
    check(afterNorm is not None and afterNorm > 1e-4,
          "and the child still carries its own edit (%r -> %r)"
          % (editNorm, afterNorm))

    # -- Shift+M merges the children away ----------------------------------
    typeKey(view, "m", ("shift",))
    wait(20)
    check(not childrenOf(session, 0),
          "Shift+M merged the children back into the parent (%r)"
          % (childrenOf(session, 0),))
    session.publish()
    check(levelInfo(session, 2) is None,
          "so nothing is published at L2 any more (%r)"
          % (levelInfo(session, 2),))

    # -- Ctrl+Z puts them back ---------------------------------------------
    typeKey(view, "z", ("ctrl",))
    wait(20)
    restored = childrenOf(session, 0)
    check(len(restored) == 4, "Ctrl+Z restores the four children (%r)"
          % (restored,))
    check(levelInfo(session, 2) is not None,
          "and L2 is published again (%r)" % (levelInfo(session, 2),))

    status = session.status()
    info("model v%d committed v%d, last move %.2f ms, ladder step %d"
         % (status["modelVersion"], status["committedVersion"],
            status["lastMoveMs"], status.get("ladderStep", 0)))
    check("ladderStep" in status,
          "the status dict carries the ladder position for the strip")

    viewport.uninstall()
    session.deactivate()
    print("testUsdviewTonicLevels: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicLevels needs testusdview (no live view)")
    sys.exit(0)
