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
            "d": QtCore.Qt.Key.Key_D,
            "g": QtCore.Qt.Key.Key_G,
            "s": QtCore.Qt.Key.Key_S,
            "c": QtCore.Qt.Key.Key_C,
            "l": QtCore.Qt.Key.Key_L,
            "t": QtCore.Qt.Key.Key_T,
            "z": QtCore.Qt.Key.Key_Z,
            "[": QtCore.Qt.Key.Key_BracketLeft,
            "]": QtCore.Qt.Key.Key_BracketRight}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    QtTest.QTest.keyClick(view, keys[name], mods)


def heldKey(view, name, down):
    """Deliver one physical key transition through the real app filter."""
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
    keys = {"f": QtCore.Qt.Key.Key_F}
    event = QtGui.QKeyEvent(
        QtCore.QEvent.Type.KeyPress if down else QtCore.QEvent.Type.KeyRelease,
        keys[name], QtCore.Qt.KeyboardModifier.NoModifier, name)
    QtWidgets.QApplication.sendEvent(view, event)


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


def guideCount(session):
    guides = ctypes.c_int(0)
    cvs = ctypes.c_int(0)
    if session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides),
                                        ctypes.byref(cvs)) != 0:
        return 0
    return int(guides.value)


def changed(before, after, epsilon=1e-5):
    return (len(before) == len(after) and any(
        abs(before[i][axis] - after[i][axis]) > epsilon
        for i in range(len(before)) for axis in range(3)))


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


def graphStrokeDetail(session, samples):
    """Failure-only census for the region that seeds this sculpt fixture."""
    nodes, edges, regions = session.graphCounts()
    nodeRecords = []
    for nodeId in range(nodes + 8):
        face = ctypes.c_int(-1)
        uv = (ctypes.c_float * 2)()
        point = (ctypes.c_float * 3)()
        if session.dll.Tonic_GraphGetNode(
                session.model, nodeId, ctypes.byref(face), uv, point) == 0:
            nodeRecords.append((nodeId, int(face.value),
                                tuple(round(float(value), 5)
                                      for value in uv),
                                tuple(round(float(value), 5)
                                      for value in point)))
    edgeRecords = []
    for edgeId in range(edges + 8):
        endpoints = (ctypes.c_int * 2)()
        if session.dll.Tonic_GraphGetEdge(
                session.model, edgeId, endpoints) == 0:
            edgeRecords.append((edgeId, int(endpoints[0]), int(endpoints[1])))
    compactSamples = [tuple(round(float(value), 2) for value in point)
                      for point in samples]
    return ("counts=%r nodes=%r edges=%r samples=%r"
            % ((nodes, edges, regions), nodeRecords, edgeRecords,
               compactSamples))


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

    mouse = Mouse(view)
    # Keep the fixture's press, held moves and release on one delivery path.
    # Mixing QtTest endpoints with explicit held QMouseEvents can perturb the
    # final sample on an unshown StageView, adding a spurious graph CV.
    # Direct QMouseEvents still pass through the installed viewport filter.
    mouse.direct = True
    viewport.setPointerInside(True)
    typeKey(view, "d")
    # Opening the workspace, hiding the source prim and changing the shelf
    # can each settle the StageView layout on the following event turn.
    # Project only after that turn, matching the camera the controller will
    # resolve for the press.
    view.update()
    wait(50)
    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the controller's camera did not resolve")
        return 1
    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)
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
    beforeGraphCounts = session.graphCounts()
    delivered = mouse._point(path[0])
    deliveredXY = (float(delivered.x()) * mouse._ratio,
                   float(delivered.y()) * mouse._ratio)
    deliveredRay = camera.rayThrough(*deliveredXY)
    firstRayHit = (session.raycast(*deliveredRay)
                   if deliveredRay is not None else None)
    graphLoop = viewport.loop
    capturedStroke = []
    originalCommitStroke = graphLoop._commitStroke

    def captureCommitStroke(snapRest):
        capturedStroke[:] = list(graphLoop._stroke)
        return originalCommitStroke(snapRest)

    graphLoop._commitStroke = captureCommitStroke
    mouse.press(path[0])
    for point in path[1:]:
        mouse.move(point)
    mouse.release(path[-1])
    graphLoop._commitStroke = originalCommitStroke
    graphCounts = session.graphCounts()
    if graphCounts != (4, 4, 1):
        info("initial graph fixture mismatch: %s"
             % graphStrokeDetail(session, path))
        info("initial graph before=%r delivered=%r firstRay=%r stroke=%r"
             % (beforeGraphCounts, deliveredXY, firstRayHit,
                (None if not capturedStroke else
                 (capturedStroke[0], capturedStroke[-1],
                  len(capturedStroke)))))
        pressCamera = viewport.camera
        info("stroke camera=%dx%d controller camera=%s"
             % (camera.width, camera.height,
                None if pressCamera is None else
                "%dx%d" % (pressCamera.width, pressCamera.height)))
    check(graphCounts == (4, 4, 1),
          "the drag stroked one closed region (%r)" % (graphCounts,))
    buildRc = session.dll.Tonic_BuildTubeFromRegion(
        session.model, 0, CV_COUNT, 8, ctypes.c_float(TUBE_LENGTH))
    if buildRc != 0:
        info("initial tube build failed: %s" % session.lastError())
    check(buildRc == 0, "a tube builds from the stroked region")
    check(session.dll.Tonic_RefillGuides(session.model,
                                         ctypes.c_float(1.0)) == 0,
          "the fixture has a full guide preview before sculpting")
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

    # -- F + LMB width drag ------------------------------------------------
    state.brushRadiusPx = 40.0
    beforeResize = centers(session)
    heldKey(view, "f", True)
    check(viewport._brushResizeArmed,
          "holding F arms Sculpt brush-width drag")
    mouse.press(tipPixel)
    check(viewport.gestureActive and viewport._brushResizeActive,
          "F-LMB captures a resize gesture instead of a sculpt stroke")
    mouse.move((tipPixel[0] + 80.0, tipPixel[1] + 120.0))
    grownRadius = loop.brushRadiusPx()
    check(grownRadius > 40.0,
          "horizontal F-drag grows the live ring radius (%g)" % grownRadius)
    check(abs(state.brushRadiusPx - 40.0) < 1e-5,
          "the panel radius waits for the completed F-drag")
    mouse.release((tipPixel[0] + 80.0, tipPixel[1] + 120.0))
    heldKey(view, "f", False)
    check(not viewport.gestureActive and not viewport._brushResizeActive and
          not viewport._brushResizeArmed,
          "LMB/F release cleanly end resize capture and its arm")
    check(not changed(beforeResize, centers(session)),
          "F-drag changes brush width without sculpting geometry")
    check(abs(state.brushRadiusPx - grownRadius) < 1e-5,
          "LMB release commits one atomic brush-radius value")
    heldKey(view, "f", True)
    check(viewport._brushResizeArmed, "F can arm another width drag")
    typeKey(view, "escape")
    check(not viewport._brushResizeArmed and not viewport.gestureActive,
          "Escape clears a stuck F arm without cancelling a sculpt stroke")
    heldKey(view, "f", False)
    # Start from the tube wall, more than the brush radius from its centre
    # line.  This verifies the normal sculpt workflow after a body click;
    # it must not depend on landing exactly on a displayed centre CV.
    state.brushRadiusPx = 8.0
    state.sculptPreserveLength = True
    state.sculptMirrorX = False
    state.brushTRadius = 0.0

    # -- a real grab stroke ------------------------------------------------
    body = list(base[CV_COUNT - 2])
    body[2] += 0.25
    start = pixelOf(body)
    bodyPick = session.pickItem(camera, start[0], start[1],
                                state.brushRadiusPx,
                                tonicLibProbe.TONIC_PICK_TUBE_VERT)
    check(bodyPick is not None,
          "the broad tube-wall point resolves to its owning tube (%r)"
          % (bodyPick,))
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
    guidesBeforeBrushes = guideCount(session)
    check(guidesBeforeBrushes > 0,
          "the sculpt fixture has %d visible guides" % guidesBeforeBrushes)

    # -- selected empty-space view-plane Grab ----------------------------
    # An artist can keep dragging after leaving the rendered tube.  The
    # initial point is deliberately near, rather than on, a selected owner;
    # motion and the ring thereafter must use the press camera plane and no
    # longer depend on a scalp/body ray hit.
    state.brushRadiusPx = 120.0
    session.select(tonicLibProbe.TONIC_PICK_TUBE_VERT, [0])
    selectedBase = centers(session)
    selectedAnchor = pixelOf(selectedBase[CV_COUNT - 2])
    emptyStart = (selectedAnchor[0], selectedAnchor[1] + 105.0)
    emptyEnd = (emptyStart[0] + 150.0, emptyStart[1])
    mouse.press(emptyStart)
    check(viewport.gestureActive,
          "a selected nearby tube accepts a near-empty-space press")
    mouse.move(emptyEnd)
    active, planeCentre, planeRadius = brushRing(session)
    check(active and planeRadius > 0.0,
          "the brush ring remains visible through background")
    if planeCentre is not None:
        projected = camera.worldToPixels(planeCentre)
        check(projected is not None and
              abs(projected[0] - emptyEnd[0]) < 5.0 and
              abs(projected[1] - emptyEnd[1]) < 5.0,
              "the active ring follows the cursor on the frozen view plane")
    mouse.release(emptyEnd)
    selectedMoved = centers(session)
    check(changed(selectedBase, selectedMoved),
          "Grab continues to deform the selected owner after leaving it")
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(centers(session) == selectedBase,
          "one undo restores the view-plane Grab exactly")

    # Escape follows the same plane path and restores the gesture base.
    mouse.press(emptyStart)
    mouse.move(emptyEnd)
    check(changed(selectedBase, centers(session)),
          "the view-plane Grab is live before Escape")
    typeKey(view, "escape")
    check(not viewport.gestureActive,
          "Escape closes the view-plane Grab gesture")
    mouse.release(emptyEnd)
    check(centers(session) == selectedBase,
          "Escape restores the view-plane Grab press-time centers exactly")
    moved = selectedBase

    # -- the remaining brushes use the bent grab result ------------------
    # Each is a real mode hotkey plus press/move/release, then Ctrl+Z.  A
    # straight column is a bad Smooth/Twist fixture because those operations
    # can correctly be zero there; the body Grab above left this curve bent.
    def brushStroke(key, dx, dy):
        typeKey(view, key)
        beforeBrush = centers(session)
        startBrush = pixelOf(beforeBrush[CV_COUNT - 2])
        mouse.press(startBrush)
        check(viewport.gestureActive,
              "%s opens a sculpt gesture" % key.upper())
        mouse.move((startBrush[0] + dx, startBrush[1] + dy))
        mouse.release((startBrush[0] + dx, startBrush[1] + dy))
        afterBrush = centers(session)
        check(changed(beforeBrush, afterBrush),
              "%s changes the bent center curve" % key.upper())
        if guidesBeforeBrushes:
            check(guideCount(session) > 0,
                  "%s keeps the guide preview populated" % key.upper())
        typeKey(view, "z", ("ctrl",))
        wait(20)
        check(centers(session) == beforeBrush,
              "Ctrl+Z restores the %s stroke exactly" % key.upper())

    brushStroke("c", 40.0, 0.0)
    brushStroke("s", 20.0, 0.0)
    beforeLength = arcLength(centers(session))
    typeKey(view, "l")
    check(loop.brush() == "lengthen", "L selects the Lengthen brush")
    lengthStart = pixelOf(centers(session)[CV_COUNT - 2])
    mouse.press(lengthStart)
    mouse.move((lengthStart[0], lengthStart[1] - 70.0))
    mouse.release((lengthStart[0], lengthStart[1] - 70.0))
    lengthened = centers(session)
    check(arcLength(lengthened) > beforeLength + 1e-4,
          "Lengthen grows the selected span")
    if guidesBeforeBrushes:
        check(guideCount(session) > 0,
              "Lengthen keeps the guide preview populated")
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(centers(session) == moved,
          "Ctrl+Z restores the Lengthen stroke exactly")
    brushStroke("t", 60.0, 0.0)
    typeKey(view, "g")

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
