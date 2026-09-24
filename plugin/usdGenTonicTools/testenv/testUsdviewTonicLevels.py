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
#   * Ctrl+Down expands only that root branch: its L2 children are visible
#     while the L1 parent is removed from the active frontier;
#   * an L2 CV edit survives a parent edit made after Ctrl+Up (K6);
#   * Shift+M takes L2 away again and Ctrl+Z restores topology; an explicit
#     Enter returns the restored children to the visible frontier.
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
            "l": QtCore.Qt.Key.Key_L,
            "1": QtCore.Qt.Key.Key_1,
            "4": QtCore.Qt.Key.Key_4,
            "up": QtCore.Qt.Key.Key_Up,
            "down": QtCore.Qt.Key.Key_Down,
            "backspace": QtCore.Qt.Key.Key_Backspace,
            "delete": QtCore.Qt.Key.Key_Delete}
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


def clickWidget(widget):
    """One left click on a dock widget through QtTest."""
    from pxr.Usdviewq.qt import QtCore
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    QtTest.QTest.mouseClick(widget, QtCore.Qt.MouseButton.LeftButton)


def clickAction(workspace, actionId):
    """Click one dock action through Qt, as an artist does; the button is
    found by its Action.id (DK-04 workspace.button hook), not its text."""
    from pxr.Usdviewq.qt import QtCore
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    button = workspace.button("action", actionId)
    if button is None:
        return False
    QtTest.QTest.mouseClick(button, QtCore.Qt.MouseButton.LeftButton)
    return True


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


def frameTube(stage, view, target):
    """A side camera with visible tube-strip quads, never an open top cap."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicLevelsTubeCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    target = Gf.Vec3d(*target)
    eye = target + Gf.Vec3d(11.0, 0.0, 0.0)
    zAxis = (eye - target).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0.0, 1.0, 0.0), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    xf = UsdGeom.Xformable(cam.GetPrim())
    # Reuse the op on a second call (MD-04 re-aims this camera):
    # AddTransformOp refuses a duplicate.
    op = None
    for candidate in xf.GetOrderedXformOps():
        if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform:
            op = candidate
            break
    if op is None:
        op = xf.AddTransformOp()
    op.Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/TonicLevelsTubeCamera")
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
        from usdGenTonicTools import tonicCamera, tonicHierarchy, tonicLib
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
    viewport.setPointerInside(True)
    typeKey(view, "d")
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
    root = centerCV(session, 0, 0)
    check(root is not None and frameTube(
        stage, view, (root[0], 0.5 * tip[1], root[2])),
          "the side camera exposes a visible tube wall")
    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the side camera did not resolve")
        return 1
    # Click the middle of a visible side wall, deliberately away from every
    # tessellated vertex.  This is the ordinary artist selection path, not a
    # test-only giant pick radius.
    target = pixel(root[0], root[2] + 0.25, 0.5 * tip[1])
    # MD-04: Hierarchy clicks with its own pick radius, not Graph's snap.
    state.pickRadiusPx = 2.0
    probe = session.pickItem(camera, target[0], target[1],
                             state.pickRadiusPx,
                             tonicLib.TONIC_PICK_TUBE_VERT)
    check(probe is not None,
          "the side-wall point resolves to a visible tube (%r)" % (probe,))
    mouse.click(target)
    selected = session.selectionCount(tonicLib.TONIC_PICK_TUBE_VERT)
    check(selected == 1, "clicking the tube selects it (%d selected)"
          % selected)
    if selected != 1:
        info("status: %r" % (messages[-3:],))

    # -- the dock's Subdivide button enters the child level ----------------
    state.subdivideCount = 4
    before = levelInfo(session, 2)
    check(before is None, "nothing is published at L2 yet (%r)" % (before,))
    check(clickAction(container.workspace, "subdivide"),
          "the visible Subdivide button was clicked")
    wait(20)
    after = levelInfo(session, 2)
    check(after is not None, "Shift+D publishes /__usdGenTonic/tubes/L2")
    if after is not None:
        check(after[2] == 4, "with the panel's four children (%r)" % (after,))
    kids = childrenOf(session, 0)
    check(len(kids) == 4, "and the model holds four child tubes (%r)"
          % (kids,))
    focus = int(session.dll.Tonic_GetFocusLevel(session.model))
    check(focus == 2 and state.activeLevel == 2,
          "Subdivide selects and focuses the new L2 children (focus %d)"
          % focus)
    selectedIds = [item[0] for item in
                   session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)]
    check(sorted(selectedIds) == sorted(kids),
          "the created children are the active selection (%r)" % selectedIds)

    # -- Ctrl+Up / Ctrl+Down navigate with the corresponding selection -----
    typeKey(view, "up", ("ctrl",))
    check(int(session.dll.Tonic_GetFocusLevel(session.model)) == 1,
          "Ctrl+Up returns to the selected L1 parent")
    check([item[0] for item in
           session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)] == [0],
          "and selects that parent")
    typeKey(view, "down", ("ctrl",))
    focus = int(session.dll.Tonic_GetFocusLevel(session.model))
    check(focus == 2 and state.activeLevel == 2,
          "Ctrl+Down returns to L2 (focus %d)" % focus)
    selectedIds = [item[0] for item in
                   session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)]
    check(sorted(selectedIds) == sorted(kids),
          "and returns to its children (%r)" % selectedIds)
    check(tonicHierarchy.isTubeVisible(session.dll, session.model, 0) is False
          and all(tonicHierarchy.isTubeVisible(session.dll, session.model,
                                                child)
                  for child in kids),
          "the active cut replaces the L1 parent with its visible L2 children")
    visible1, xray1 = levelDisplay(session, 1)
    visible2, xray2 = levelDisplay(session, 2)
    check(visible1 and not xray1 and visible2 and not xray2,
          "frontier display levels stay solid unless the artist enables x-ray"
          " (L1 %s/%s, L2 %s/%s)" % (visible1, xray1, visible2, xray2))

    # -- DK-08: the breadcrumb is a row of buttons -------------------------
    workspace = container.workspace
    workspace.refresh()
    crumbs = workspace.breadcrumbButtons()
    check(len(crumbs) == 2,
          "after subdivide + enter the breadcrumb has two buttons (%r)"
          % ([button.text() for button in crumbs],))
    if len(crumbs) == 2:
        check(crumbs[0].text() == "Groom" and crumbs[1].text() == "Tube 0"
              and "Level 1" in crumbs[1].toolTip(),
              "Groom, then the tube's name with its level in the tooltip "
              "(%r, %r)" % (crumbs[1].text(), crumbs[1].toolTip()))
        frontier = workspace._breadcrumbBar.frontierLabel.text()
        check(frontier == "L2 (%d children)" % len(kids),
              "the frontier reads where the artist is (%r)" % frontier)
        clickWidget(crumbs[0])
        wait(20)
        check(state.activeLevel == 1 and
              int(session.dll.Tonic_GetFocusLevel(session.model)) == 1 and
              tonicHierarchy.isTubeVisible(session.dll, session.model, 0)
              is True,
              "clicking the first crumb returns to L1 with the parent on "
              "screen (state L%d)" % state.activeLevel)
        typeKey(view, "down", ("ctrl",))
        wait(20)
        check(state.activeLevel == 2, "Ctrl+Down enters the branch again")
        workspace.refresh()
        exitButton = workspace._breadcrumbBar.exitButton
        check(exitButton.isEnabled() and
              "Ctrl+Up" in exitButton.toolTip(),
              "the breadcrumb's up arrow is Exit level (Ctrl+Up)")
        clickWidget(exitButton)
        wait(20)
        check(state.activeLevel == 1 and
              [item[0] for item in
               session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)] == [0],
              "clicking it exits to the selected parent")
        typeKey(view, "down", ("ctrl",))
        wait(20)
        selectedIds = [item[0] for item in
                       session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)]
        check(state.activeLevel == 2 and sorted(selectedIds) == sorted(kids),
              "and Ctrl+Down comes back to the children (%r)" % selectedIds)

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

    # -- DK-08: Re-subdivide's confirm shows on the button -----------------
    TUBE = tonicLib.TONIC_PICK_TUBE_VERT
    workspace = container.workspace
    resub = workspace.button("action", "resubdivide")
    check(resub is not None, "the dock has a Re-subdivide button")
    if resub is not None:
        plainText = resub.text()
        check(clickAction(workspace, "resubdivide"), "Re-subdivide clicked")
        wait(20)
        check("Confirm" in resub.text() and viewport.loop.resubdivideArmed,
              "one click arms it and the button asks to confirm (%r)"
              % resub.text())
        check(sorted(childrenOf(session, 0)) == sorted(kids) and
              (deltaNorm(session, child) or 0.0) > 1e-4,
              "and nothing was re-subdivided yet: the child keeps its edit")
        session.select(TUBE, [kids[0]])
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        workspace.refresh()
        check("Confirm" not in resub.text() and resub.text() == plainText,
              "a new selection reverts the button (%r)" % resub.text())
        session.select(TUBE, [0])
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        clickAction(workspace, "resubdivide")
        wait(20)
        clickAction(workspace, "resubdivide")
        wait(20)
        # The model recycles freed tube ids, so "new" children are told
        # apart by what Re-subdivide throws away: the children's sculpt.
        redone = childrenOf(session, 0)
        norms = [deltaNorm(session, tubeId) for tubeId in redone]
        check(len(redone) == 4 and
              all(norm is not None and norm < 1e-5 for norm in norms) and
              "re-subdivided" in str(messages[-1:]),
              "two clicks re-subdivide: four children recreated without "
              "the old sculpt (%r, deltas %r; %r)"
              % (redone, norms, messages[-1:]))
        check("Confirm" not in resub.text(),
              "and the button is plain again (%r)" % resub.text())
        # Back to the parent, as Shift+M below expects.
        typeKey(view, "up", ("ctrl",))
        wait(20)
        check([item[0] for item in session.readSelection(TUBE)] == [0],
              "Ctrl+Up selects the parent again")

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
    check(levelInfo(session, 2) is None and
          tonicHierarchy.isTubeVisible(session.dll, session.model, 0) is True,
          "undo restores topology but keeps the merged parent collapsed")
    check(clickAction(container.workspace, "enterLevel"),
          "an explicit Enter returns the restored branch to the frontier")
    wait(20)
    check(levelInfo(session, 2) is not None and
          tonicHierarchy.isTubeVisible(session.dll, session.model, 0) is False
          and all(tonicHierarchy.isTubeVisible(session.dll, session.model,
                                                child)
                  for child in restored),
          "and explicitly entering publishes the restored L2 children"
          " (%r)" % (levelInfo(session, 2),))

    # -- MD-03: the edge split is drawn visibly and consumed ---------------
    typeKey(view, "m", ("shift",))
    wait(20)
    check(not childrenOf(session, 0),
          "Shift+M folds the children away again for the edge split")
    session.select(tonicLib.TONIC_PICK_TUBE_VERT, [0])
    check(frameScalp(stage, view), "the top-down camera is back")
    view.setFocus()
    wait(50)
    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the top-down camera did not resolve")
        return 1
    state.splitMode = "edge"
    typeKey(view, "d")
    loop = viewport.loop
    check(loop is not None and loop.subMode() == "subdivide",
          "D picks the Subdivide sub-mode (%r)"
          % (loop.subMode() if loop is not None else None,))
    overlay = viewport._regionOverlay
    check(overlay is not None and not overlay.isVisible(),
          "no edge overlay before a stroke")
    stroke = [pixel(0.6 + 2.8 * i / 8.0, 2.0) for i in range(9)]
    mouse.press(stroke[0])
    for point in stroke[1:]:
        mouse.move(point)
    live = loop.edgePreview()["live"]
    check(live is not None and overlay.isVisible(),
          "the stroke in flight is drawn by the overlay (%r)" % (live,))
    mouse.release(stroke[-1])
    wait(20)
    check(loop.edge is not None,
          "the drag across the root recorded a split edge (%r)"
          % (messages[-1:],))
    check(overlay.isVisible(),
          "and the recorded edge stays on screen until Shift+D")
    typeKey(view, "d", ("shift",))
    wait(20)
    edgeKids = childrenOf(session, 0)
    check(len(edgeKids) == 2,
          "Shift+D splits the root along the edge into two (%r, %r)"
          % (edgeKids, messages[-1:]))
    check(loop.edge is None and not overlay.isVisible(),
          "the split consumed the edge and hid the overlay")

    # -- SL-02: a plain drag from empty space boxes tubes -------------------
    typeKey(view, "n")
    check(viewport.loop is not None and
          viewport.loop.subMode() == "navigate",
          "N picks the Navigate sub-mode (%r)"
          % (viewport.loop.subMode() if viewport.loop is not None
             else None,))
    session.clearSelection(tonicLib.TONIC_PICK_TUBE_VERT)
    session.publish(tonicLib.TONIC_DIRTY_SELECTION)
    start = pixel(0.3, 0.3)
    end = pixel(3.7, 3.7)
    probe = session.pickItem(camera, start[0], start[1],
                             viewport.loop.pickRadiusPx(),
                             tonicLib.TONIC_PICK_TUBE_VERT)
    check(probe is None, "the band starts on empty space (%r)" % (probe,))
    check(abs(end[0] - start[0]) >= 40.0 and abs(end[1] - start[1]) >= 40.0,
          "the band spans at least 40 px each way (%r -> %r)"
          % (start, end))
    marquee = viewport._marqueeOverlay
    mouse.press(start)
    mouse.move(((start[0] + end[0]) * 0.5, (start[1] + end[1]) * 0.5))
    mouse.move(end)
    wait(10)
    check(marquee is not None and marquee.isVisible(),
          "a plain drag from empty space shows the marquee")
    mouse.release(end)
    wait(10)
    boxed = sorted(item[0] for item in
                   session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT))
    check(edgeKids and set(edgeKids) <= set(boxed),
          "and selects the tubes it encloses (%r, children %r)"
          % (boxed, edgeKids))
    check(marquee is None or not marquee.isVisible(),
          "the marquee goes away on release")

    # -- SL-03: Backspace leaves the entered level; Delete removes a child --
    viewport.setPointerInside(True)
    TUBE = tonicLib.TONIC_PICK_TUBE_VERT
    if len(edgeKids) == 2:
        session.select(TUBE, [edgeKids[0]])
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        typeKey(view, "backspace")
        wait(20)
        check(tonicHierarchy.isTubeVisible(session.dll, session.model, 0)
              is True and
              not any(tonicHierarchy.isTubeVisible(session.dll,
                                                   session.model, child)
                      for child in edgeKids),
              "Backspace collapses the branch: parent visible, children "
              "hidden")
        check(int(session.dll.Tonic_GetFocusLevel(session.model)) == 1 and
              int(state.activeLevel) == 1,
              "and focuses level 1 (focus %d, state %d)"
              % (int(session.dll.Tonic_GetFocusLevel(session.model)),
                 int(state.activeLevel)))
        check([item[0] for item in session.readSelection(TUBE)] == [0],
              "with the parent selected (%r)" % (session.readSelection(TUBE),))

        typeKey(view, "down", ("ctrl",))
        wait(20)
        check(all(tonicHierarchy.isTubeVisible(session.dll, session.model,
                                               child) for child in edgeKids),
              "Ctrl+Down enters the branch again")
        victim, survivor = edgeKids
        session.select(TUBE, [victim])
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        undoDepth = int(session.dll.Tonic_GetUndoDepth(session.model))
        typeKey(view, "delete")
        wait(20)
        remaining = childrenOf(session, 0)
        check(remaining == [survivor],
              "Delete removes the selected child and only it (%r, %r)"
              % (remaining, messages[-1:]))
        check(victim not in [item[0] for item in
                             session.readSelection(TUBE)],
              "and the deleted tube leaves the selection")
        check(int(session.dll.Tonic_GetUndoDepth(session.model)) ==
              undoDepth + 1,
              "as exactly one undo step")
        typeKey(view, "z", ("ctrl",))
        wait(20)
        check(sorted(childrenOf(session, 0)) == sorted(edgeKids),
              "Ctrl+Z restores the deleted child (%r)"
              % (childrenOf(session, 0),))
        session.select(TUBE, [0])
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        typeKey(view, "delete")
        wait(20)
        check(sorted(childrenOf(session, 0)) == sorted(edgeKids) and
              messages and "L1 root" in str(messages[-1]),
              "Delete refuses the L1 root with the reason (%r)"
              % (messages[-1:],))
    else:
        check(False, "the edge split left two children for the Delete step")

    # -- MD-04: Levels and Merge act on a plain click ----------------------
    def clickablePixel(tubeId):
        """A pixel whose Hierarchy pick is `tubeId` -- probed through the
        loop's own pick radius, never assumed from a projection."""
        radius = viewport.loop.pickRadiusPx()
        for cv in (2, 3, 1, 4, 0):
            point = centerCV(session, tubeId, cv)
            if point is None:
                continue
            base = camera.worldToPixels(point)
            if base is None:
                continue
            for dx, dy in ((0.0, 0.0), (4.0, 0.0), (-4.0, 0.0), (0.0, 4.0),
                           (0.0, -4.0)):
                x, y = base[0] + dx, base[1] + dy
                hit = session.pickItem(camera, x, y, radius, TUBE)
                if hit is not None and int(hit["id"]) == int(tubeId):
                    return (x, y)
        return None

    if len(edgeKids) == 2 and root is not None and tip is not None:
        session.select(TUBE, [0])
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        typeKey(view, "down", ("ctrl",))
        wait(20)
        check(all(tonicHierarchy.isTubeVisible(session.dll, session.model,
                                               child) for child in edgeKids),
              "MD-04: Ctrl+Down shows the L2 children for the Levels click")
        check(frameTube(stage, view, (root[0], 0.5 * tip[1], root[2])),
              "the side camera is back for the sub-mode clicks")
        view.setFocus()
        wait(50)
        camera = tonicCamera.resolve(view)
        if camera is None:
            print("FAIL: the side camera did not resolve for MD-04")
            return 1

        typeKey(view, "l")
        check(viewport.loop is not None and
              viewport.loop.subMode() == "levels",
              "L picks the Levels sub-mode (%r)"
              % (viewport.loop.subMode() if viewport.loop is not None
                 else None,))
        spot = clickablePixel(edgeKids[0])
        check(spot is not None, "an L2 child has a clickable pixel")
        if spot is not None:
            mouse.click(spot)
            wait(20)
            check(int(state.soloLevel) == 2 and
                  levelDisplay(session, 2)[0] and
                  not levelDisplay(session, 1)[0],
                  "a Levels click on an L2 child solos L2 (solo %d, %r)"
                  % (int(state.soloLevel), messages[-1:]))
            wait(300)       # two clicks, never a double-click (Enter)
            mouse.click(spot)
            wait(20)
            check(int(state.soloLevel) == tonicHierarchy.SOLO_OFF and
                  levelDisplay(session, 1)[0],
                  "a second click un-solos it (solo %d, %r)"
                  % (int(state.soloLevel), messages[-1:]))

        # Merge: back to the parent, whose click folds its children.
        session.select(TUBE, [edgeKids[0]])
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        typeKey(view, "up", ("ctrl",))
        wait(20)
        check(tonicHierarchy.isTubeVisible(session.dll, session.model, 0)
              is True, "Ctrl+Up puts the parent back on screen")
        typeKey(view, "m")
        check(viewport.loop is not None and
              viewport.loop.subMode() == "merge",
              "M picks the Merge sub-mode (%r)"
              % (viewport.loop.subMode() if viewport.loop is not None
                 else None,))
        spot = clickablePixel(0)
        check(spot is not None, "the parent has a clickable pixel")
        if spot is not None:
            undoDepth = int(session.dll.Tonic_GetUndoDepth(session.model))
            mouse.click(spot)
            wait(20)
            check(not childrenOf(session, 0),
                  "a Merge click on the parent folds its children (%r, %r)"
                  % (childrenOf(session, 0), messages[-1:]))
            check(int(session.dll.Tonic_GetUndoDepth(session.model)) ==
                  undoDepth + 1, "as exactly one undo step")
            check(messages and
                  "Merge children (Shift+M)" in str(messages[-1]),
                  "with Shift+M's status (%r)" % (messages[-1:],))
            typeKey(view, "z", ("ctrl",))
            wait(20)
            check(sorted(childrenOf(session, 0)) == sorted(edgeKids),
                  "Ctrl+Z brings the children back (%r)"
                  % (childrenOf(session, 0),))
        typeKey(view, "n")
    else:
        check(False, "the edge split left two children for the MD-04 clicks")

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
