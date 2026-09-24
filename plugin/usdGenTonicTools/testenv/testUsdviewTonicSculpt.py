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
#   * the idle ring faces the camera, sits exactly under the cursor even
#     between CVs, and leaves with the pointer (QEvent.Leave);
#   * `[` and `]` scale the brush by 1.15, and an F-drag reports the live
#     radius before its release;
#   * Escape in the middle of a stroke restores the press-time centers
#     BIT-EXACTLY, which is the whole point of the gesture bracket;
#   * a click without a drag leaves no undo step, and a press off every
#     tube over the active USD scalp neither opens a gesture nor reaches
#     usdview's picker (MD-02);
#   * TS-03: Twist keeps the root and the arc length; a Brush strength of 0
#     typed into the dock moves nothing and leaves no undo step; and on a
#     four-sibling fixture centred on x = 0 the dock's Mirror X box repeats
#     a Grab stroke on the stroked tube's mirror twin, and only there.
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


def QtCoreType(name):
    from pxr.Usdviewq.qt import QtCore
    return getattr(QtCore.QEvent.Type, name)


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


def brushRingNormal(session):
    """The normal of the ring the viewport draws (None on error)."""
    active = ctypes.c_int(0)
    centre = (ctypes.c_float * 3)()
    normal = (ctypes.c_float * 3)()
    radius = ctypes.c_float(0.0)
    if session.dll.Tonic_GetBrushRing(session.model, ctypes.byref(active),
                                      centre, normal,
                                      ctypes.byref(radius)) != 0:
        return None
    return (float(normal[0]), float(normal[1]), float(normal[2]))


def hoverOf(session):
    """(kind, id, subId) of the model's hover prehighlight."""
    kind = ctypes.c_uint(0)
    ident = ctypes.c_int(-1)
    subId = ctypes.c_int(-1)
    subSubId = ctypes.c_int(-1)
    if session.dll.Tonic_GetHover(session.model, ctypes.byref(kind),
                                  ctypes.byref(ident), ctypes.byref(subId),
                                  ctypes.byref(subSubId)) != 0:
        return None
    return (int(kind.value), int(ident.value), int(subId.value))


def sendViewEvent(view, kind):
    """One plain QEvent (Enter/Leave) through the installed view filter."""
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    QtWidgets.QApplication.sendEvent(view, QtCore.QEvent(kind))


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

    # -- MD-02: the ring faces the camera and follows the cursor exactly --
    forwardRay = camera.rayThrough(camera.width * 0.5, camera.height * 0.5)
    ringNormal = brushRingNormal(session)
    facing = (abs(sum(ringNormal[k] * forwardRay[1][k] for k in range(3)))
              if ringNormal is not None and forwardRay is not None else 0.0)
    check(facing > 0.99,
          "side-on, the hover ring faces the camera (|dot| %.4f, normal %r)"
          % (facing, ringNormal))
    cvPixels = [pixelOf(point) for point in base]
    betweenCVs = []
    for a, b, t, dx in ((1, 2, 0.5, 6.0), (2, 3, 0.35, -5.0)):
        betweenCVs.append((cvPixels[a][0] + (cvPixels[b][0] -
                                             cvPixels[a][0]) * t + dx,
                           cvPixels[a][1] + (cvPixels[b][1] -
                                             cvPixels[a][1]) * t))
    for target in betweenCVs:
        delivered = mouse._point(target)
        cursor = (float(delivered.x()) * mouse._ratio,
                  float(delivered.y()) * mouse._ratio)
        mouse.move(target)
        wait(20)
        active, ringCentre, ringRadius = brushRing(session)
        projected = (camera.worldToPixels(ringCentre)
                     if active and ringCentre is not None else None)
        check(projected is not None and
              abs(projected[0] - cursor[0]) <= 1.0 and
              abs(projected[1] - cursor[1]) <= 1.0,
              "between CVs the ring centre projects under the cursor "
              "(%r vs %r)"
              % (None if projected is None else (projected[0], projected[1]),
                 cursor))
        hovered = hoverOf(session)
        check(hovered is not None and
              hovered[0] == tonicLibProbe.TONIC_PICK_CENTER_CV and
              hovered[1] == 0,
              "and the anchor centre CV is the hover prehighlight (%r)"
              % (hovered,))
    sendViewEvent(view, QtCoreType("Leave"))
    wait(20)
    active, _centre, ringRadius = brushRing(session)
    check(not active and ringRadius == 0.0,
          "QEvent.Leave takes the idle ring away (active %r, r %g)"
          % (active, ringRadius))
    sendViewEvent(view, QtCoreType("Enter"))
    viewport.setPointerInside(True)
    mouse.move(tipPixel)
    wait(20)
    check(brushRing(session)[0], "and re-entering brings it back")

    # -- `[` / `]` --------------------------------------------------------
    state.brushRadiusPx = 60.0
    typeKey(view, "]")
    ratio = state.brushRadiusPx / 60.0
    check(abs(ratio - 1.15) < 1e-3, "] scales the brush by 1.15 (%g -> %g)"
          % (60.0, state.brushRadiusPx))
    typeKey(view, "[")
    check(abs(state.brushRadiusPx - 60.0) < 1e-3,
          "[ divides it back (%g)" % state.brushRadiusPx)
    typeKey(view, "[")
    check(abs(60.0 / state.brushRadiusPx - 1.15) < 1e-3,
          "[ shrinks it by the same ratio (%g)" % state.brushRadiusPx)

    # -- F + LMB width drag ------------------------------------------------
    state.brushRadiusPx = 40.0
    beforeResize = centers(session)
    heldKey(view, "f", True)
    check(viewport._brushResizeArmed,
          "holding F arms Sculpt brush-width drag")
    mouse.press(tipPixel)
    check(viewport.gestureActive and viewport._brushResizeActive,
          "F-LMB captures a resize gesture instead of a sculpt stroke")
    resizeMark = len(messages)
    mouse.move((tipPixel[0] + 80.0, tipPixel[1] + 120.0))
    grownRadius = loop.brushRadiusPx()
    liveReadout = [str(m) for m in messages[resizeMark:]
                   if "brush radius" in str(m)]
    check(bool(liveReadout),
          "the F-drag reports the live brush radius before release (%r)"
          % (liveReadout[-1:],))
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

    # -- TS-03: Twist rotates about the chord, root pinned, length kept ---
    typeKey(view, "t")
    check(loop.brush() == "twist", "T selects the Twist brush (%r)"
          % loop.brush())
    beforeTwist = centers(session)
    twistStart = pixelOf(beforeTwist[CV_COUNT - 2])
    depthTwist = int(session.undoDepth())
    mouse.press(twistStart)
    for step in range(1, 5):
        mouse.move((twistStart[0] + 20.0 * step, twistStart[1]))
    mouse.release((twistStart[0] + 80.0, twistStart[1]))
    twisted = centers(session)
    check(changed(beforeTwist, twisted),
          "a rightward Twist drag turns the bent curve")
    check(all(abs(twisted[0][k] - beforeTwist[0][k]) < 1e-5
              for k in range(3)),
          "with the root pinned (%r -> %r)" % (beforeTwist[0], twisted[0]))
    twistRelative = (abs(arcLength(twisted) - arcLength(beforeTwist)) /
                     max(arcLength(beforeTwist), 1e-9))
    check(twistRelative <= 1e-3,
          "and the arc length kept (relative change %.2e)" % twistRelative)
    check(int(session.undoDepth()) == depthTwist + 1,
          "as one undo step (%d -> %d)" % (depthTwist,
                                           int(session.undoDepth())))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(centers(session) == beforeTwist,
          "Ctrl+Z restores the Twist exactly")
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

    # -- MD-02: a click without a drag leaves no undo step ----------------
    preStroke = centers(session)
    start = pixelOf(preStroke[CV_COUNT - 2])
    mouse.press(start)
    mouse.move((start[0] + 20.0, start[1]))
    mouse.move((start[0] + 40.0, start[1]))
    mouse.release((start[0] + 40.0, start[1]))
    afterStroke = centers(session)
    check(changed(preStroke, afterStroke),
          "a Grab stroke before the click moved the CVs")
    clickAt = pixelOf(afterStroke[CV_COUNT - 2])
    clickMark = len(messages)
    mouse.press(clickAt)
    mouse.release(clickAt)
    check(not viewport.gestureActive and not session.gestureActive,
          "a click without a drag leaves no gesture open")
    check(centers(session) == afterStroke,
          "and changes nothing")
    check(any("nothing changed" in str(m) for m in messages[clickMark:]),
          "and says so (%r)" % ([str(m) for m in messages[clickMark:]][-2:],))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(centers(session) == preStroke,
          "one Ctrl+Z after the click restores the pre-stroke centers")

    # -- TS-03: Brush strength 0 (typed into the dock) moves nothing -------
    workspace = container.workspace
    workspace.refresh()
    strengthRow = None
    for descriptor, widget in getattr(workspace, "_paramWidgets", ()):
        if descriptor.id == "sculptStrength":
            strengthRow = widget
    check(strengthRow is not None and strengthRow.minimum() == 0.0,
          "the Sculpt form has a Brush strength row reaching 0 (%r)"
          % (None if strengthRow is None else strengthRow.minimum(),))

    def typeStrength(text):
        from pxr.Usdviewq.qt import QtCore as _QtCore
        strengthRow.setFocus()
        wait(10)
        strengthRow.selectAll()
        _qtTest().QTest.keyClicks(strengthRow, text)
        _qtTest().QTest.keyClick(strengthRow, _QtCore.Qt.Key.Key_Return)
        wait(20)
        view.setFocus()
        viewport.setPointerInside(True)

    if strengthRow is not None:
        typeStrength("0")
        check(float(state.sculptStrength) == 0.0,
              "typing 0 + Enter sets the brush strength to 0 (%r)"
              % state.sculptStrength)
        zeroBase = centers(session)
        zeroDepth = int(session.undoDepth())
        zeroMark = len(messages)
        zeroStart = pixelOf(zeroBase[CV_COUNT - 2])
        mouse.press(zeroStart)
        for step in range(1, 5):
            mouse.move((zeroStart[0] + 15.0 * step,
                        zeroStart[1] - 10.0 * step))
        mouse.release((zeroStart[0] + 60.0, zeroStart[1] - 40.0))
        check(centers(session) == zeroBase,
              "a Grab stroke at strength 0 moves nothing")
        check(int(session.undoDepth()) == zeroDepth and
              not session.gestureActive,
              "and leaves no undo step (%d -> %d)"
              % (zeroDepth, int(session.undoDepth())))
        check(any("nothing changed" in str(m) for m in messages[zeroMark:]),
              "and says so (%r)"
              % ([str(m) for m in messages[zeroMark:]][-1:],))
        typeKey(view, "s")
        smoothBase = centers(session)
        mouse.press(zeroStart)
        mouse.move((zeroStart[0] + 30.0, zeroStart[1]))
        mouse.release((zeroStart[0] + 30.0, zeroStart[1]))
        check(centers(session) == smoothBase and
              int(session.undoDepth()) == zeroDepth,
              "a Smooth stroke at strength 0 moves nothing either")
        typeKey(view, "g")
        workspace.refresh()
        for descriptor, widget in getattr(workspace, "_paramWidgets", ()):
            if descriptor.id == "sculptStrength":
                strengthRow = widget
        typeStrength("1")
        check(abs(float(state.sculptStrength) - 1.0) < 1e-6,
              "typing 1 puts the strength back (%r)" % state.sculptStrength)

    # -- MD-02: a miss over the active USD scalp is honest ----------------
    state.brushRadiusPx = 8.0
    session.clearSelection()
    stage.GetPrimAtPath("/Scalp").SetActive(True)
    wait(50)
    camera = tonicCamera.resolve(view)
    api = viewport._api
    missMark = len(messages)
    farScalp = camera.worldToPixels((0.3, 0.0, 0.3))
    for label, pixel in (("the background", (12.0, 12.0)),
                         ("the scalp far from the tube",
                          (farScalp[0], farScalp[1]) if farScalp else None)):
        if pixel is None:
            check(False, "%s projects into the view" % label)
            continue
        # A prim nowhere under the cursor, so any usdview pick changes it
        # (a background pick clears it, a scalp pick selects /Scalp).
        dataModel.selection.setPrimPath("/TonicSculptCamera")
        wait(10)
        before = [str(path) for path in (api.selectedPaths or [])]
        mouse.press(pixel)
        wait(10)
        check(not viewport.gestureActive and not session.gestureActive,
              "a Sculpt press on %s opens no gesture" % label)
        mouse.release(pixel)
        wait(20)
        after = [str(path) for path in (api.selectedPaths or [])]
        check(after == before,
              "and never reaches usdview's picker (%r -> %r)"
              % (before, after))
    check(any("no tube under the brush" in str(m)
              for m in messages[missMark:]),
          "the miss is reported in the status (%r)"
          % ([str(m) for m in messages[missMark:]][-1:],))
    stage.GetPrimAtPath("/Scalp").SetActive(False)
    wait(50)
    camera = tonicCamera.resolve(view)

    status = session.status()
    info("model v%d, last move %.3f ms, ladder step %d"
         % (status["modelVersion"], status["lastMoveMs"],
            status.get("ladderStep", 0)))
    check(status["lastMoveMs"] > 0.0,
          "the controller timed its sculpt moves (%.3f ms, budget 8)"
          % status["lastMoveMs"])

    # -- F tap frames (FB-03) ----------------------------------------------
    # Down and up with no width drag between is a tap: it frames the groom
    # (Sculpt has no selection of its own) instead of arming a resize.  Last,
    # because the framed free camera invalidates every pixel above.
    check(state.activeMode == "sculpt", "still in Sculpt for the F tap")
    cameraBefore = tonicCamera.resolve(view).viewProj
    heldKey(view, "f", True)
    heldKey(view, "f", False)
    wait(30)
    check(not viewport._brushResizeArmed and not viewport.gestureActive,
          "an F tap leaves no resize arm or gesture behind")
    check(tonicCamera.resolve(view).viewProj != cameraBefore,
          "an F tap with no mouse frames the groom (the camera moved)")

    typeKey(view, "1")
    check(state.activeMode == "graph", "1 leaves Sculpt for Graph")
    check(not brushRing(session)[0],
          "and the brush ring leaves with the mode")

    mirrorXFixture(appController, container, stage, view, Mouse)

    viewport = container.viewport
    session = container.session
    viewport.uninstall()
    session.deactivate()
    print("testUsdviewTonicSculpt: %d failure(s)" % failures)
    return 1 if failures else 0


# ---------------------------------------------------------------------------
# TS-03: Mirror X on a four-sibling fixture
# ---------------------------------------------------------------------------

# Two mirror pairs of regions on a scalp centred on x = 0: A/B across the
# plane at the front, C/D at the back.
# B and D are A and C reflected point for point, stroke order included, so
# the simplified region loops (and the roots fitted to them) are twins.
MIRROR_QUADS = {
    "A": ((-1.8, 0.2), (-0.2, 0.2), (-0.2, 1.8), (-1.8, 1.8)),
    "C": ((-1.8, 2.2), (-0.2, 2.2), (-0.2, 3.8), (-1.8, 3.8)),
}
MIRROR_QUADS["B"] = tuple((-x, z) for x, z in MIRROR_QUADS["A"])
MIRROR_QUADS["D"] = tuple((-x, z) for x, z in MIRROR_QUADS["C"])


def defineMirrorScalp(stage):
    """/MirrorScalp: the fixture's 4x4 grid moved to span x in [-2, 2]."""
    from pxr import Gf, Sdf, UsdGeom, Vt
    mesh = UsdGeom.Mesh.Define(stage, Sdf.Path("/MirrorScalp"))
    points = [Gf.Vec3f(float(i - 2), 0.0, float(j))
              for i in range(5) for j in range(5)]
    counts = [4] * 16
    indices = []
    for i in range(4):
        for j in range(4):
            indices.extend([i * 5 + j, i * 5 + j + 1, (i + 1) * 5 + j + 1,
                            (i + 1) * 5 + j])
    mesh.CreatePointsAttr(Vt.Vec3fArray(points))
    mesh.CreateFaceVertexCountsAttr(Vt.IntArray(counts))
    mesh.CreateFaceVertexIndicesAttr(Vt.IntArray(indices))
    mesh.CreateExtentAttr(Vt.Vec3fArray([Gf.Vec3f(-2.5, -0.5, -0.5),
                                         Gf.Vec3f(2.5, 0.5, 4.5)]))
    return mesh.GetPrim()


def rootOf(session, tubeId):
    out = centers(session, tubeId)
    return out[0] if out else None


def l1Roots(session):
    ids = (ctypes.c_int * 64)()
    count = ctypes.c_int(0)
    if session.dll.Tonic_ReadL1TubeIds(session.model, ids, 64,
                                       ctypes.byref(count)) != 0:
        return []
    return [int(ids[i]) for i in range(count.value)]


def mirrorXFixture(appController, container, stage, view, Mouse):
    """Mirror X: a stroke on one sibling repeats on its mirror twin only.

    A second bind (replacing the first model) onto a scalp centred on
    x = 0, four closed regions drawn with real Draw strokes -- four L1
    siblings in two mirror pairs -- then Sculpt Grab strokes on sibling A
    with the dock's Mirror X box off and on.
    """
    from usdGenTonicTools import tonicCamera
    from pxr.Usdviewq.qt import QtCore
    import tonicT3
    prim = defineMirrorScalp(stage)
    api = container.viewport._api
    bound = container.bindGeometry(api, "/MirrorScalp", replace=True)
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    check(bool(bound) and session is not None and session.model is not None
          and viewport is not None and viewport.installed,
          "Mirror X: /MirrorScalp (x in [-2, 2]) binds in place of /Scalp "
          "(%r)" % (bound,))
    if not bound or session is None or session.model is None:
        return
    messages = []
    session.setStatusSink(messages.append)
    prim.SetActive(False)
    tonicT3.frameScalp(stage, view, eye=(0.0, 12.0, 2.0),
                       primPath="/TonicMirrorTopCamera")
    view.setFocus()
    wait(50)
    mouse = Mouse(view)
    mouse.direct = True
    viewport.setPointerInside(True)
    typeKey(view, "1")
    typeKey(view, "d")
    # The mode switch can re-lay the dock out; project only after the view
    # has settled, with the camera the controller will resolve.
    view.update()
    wait(50)
    camera = tonicCamera.resolve(view)
    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((0.0, 0.0, 2.0)), 1e-9), 2.0)
    roots = {}
    for name in ("A", "B", "C", "D"):
        quad = MIRROR_QUADS[name]
        path = []
        for k in range(4):
            x0, z0 = quad[k]
            x1, z1 = quad[(k + 1) % 4]
            for i in range(5):
                t = float(i) / 5.0
                hit = camera.worldToPixels((x0 + (x1 - x0) * t, 0.0,
                                            z0 + (z1 - z0) * t))
                path.append((hit[0], hit[1]))
        first = camera.worldToPixels((quad[0][0], 0.0, quad[0][1]))
        path.append((first[0], first[1]))
        before = set(l1Roots(session))
        mouse.press(path[0])
        for point in path[1:]:
            mouse.move(point)
        mouse.release(path[-1])
        grown = sorted(set(l1Roots(session)) - before)
        roots[name] = grown[0] if len(grown) == 1 else None
    check(session.graphCounts()[2] == 4 and
          all(roots[n] is not None for n in roots),
          "four separate strokes make four regions, each with its own L1 "
          "sibling (%r, %r, %r)" % (session.graphCounts(), roots,
                                    messages[-2:]))
    if not all(roots[n] is not None for n in roots):
        return
    a, b = rootOf(session, roots["A"]), rootOf(session, roots["B"])
    check(a is not None and b is not None and
          abs(a[0] + b[0]) < 0.1 and abs(a[2] - b[2]) < 0.1,
          "A and B root as mirror twins across x = 0 (%r, %r)" % (a, b))

    # Front on, looking down +Z, so a sideways drag moves A along X.
    tonicT3.sideCamera(stage, view, eye=(0.0, 1.6, -12.0),
                       target=(0.0, 1.6, 2.0),
                       primPath="/TonicMirrorFrontCamera")
    view.setFocus()
    wait(50)
    camera = tonicCamera.resolve(view)
    typeKey(view, "5")
    typeKey(view, "g")
    loop = viewport.loop
    check(loop is not None and loop.modeId == "sculpt" and
          loop.brush() == "grab", "5 then G: Sculpt Grab on the siblings")
    state.brushRadiusPx = 10.0
    state.brushTRadius = 0.0
    state.sculptStrength = 1.0

    def mirrorBox():
        workspace = container.workspace
        workspace.refresh()
        for descriptor, widget in getattr(workspace, "_paramWidgets", ()):
            if descriptor.id == "sculptMirrorX":
                return widget
        return None

    def everyTube():
        return {name: centers(session, roots[name]) for name in roots}

    def strokeA():
        tip = centers(session, roots["A"])[CV_COUNT - 2]
        start = camera.worldToPixels(tip)
        start = (start[0], start[1])
        mouse.press(start)
        for step in range(1, 5):
            mouse.move((start[0] + 10.0 * step, start[1]))
        mouse.release((start[0] + 40.0, start[1]))

    def delta(before, after, cv):
        return tuple(after[cv][k] - before[cv][k] for k in range(3))

    def clickBox(box):
        _qtTest().QTest.mouseClick(box, QtCore.Qt.MouseButton.LeftButton,
                                   QtCore.Qt.KeyboardModifier.NoModifier,
                                   QtCore.QPoint(6, box.height() // 2))
        wait(20)
        view.setFocus()
        viewport.setPointerInside(True)

    box = mirrorBox()
    check(box is not None and not box.isChecked(),
          "the dock's Mirror X box starts unticked")
    if box is None:
        return
    base = everyTube()
    strokeA()
    after = everyTube()
    moved = delta(base["A"], after["A"], CV_COUNT - 2)
    check(abs(moved[0]) > 1e-3,
          "Mirror X off: the Grab stroke moves A sideways (%r)" % (moved,))
    check(after["B"] == base["B"] and after["C"] == base["C"] and
          after["D"] == base["D"],
          "and no other sibling")
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(everyTube() == base, "Ctrl+Z restores A")

    clickBox(box)
    check(bool(state.sculptMirrorX),
          "a click on the Mirror X box turns it on (%r)" % state.sculptMirrorX)
    depth = int(session.undoDepth())
    strokeA()
    after = everyTube()
    movedA = delta(base["A"], after["A"], CV_COUNT - 2)
    movedB = delta(base["B"], after["B"], CV_COUNT - 2)
    info("Mirror X on: A moved %r, B moved %r" % (movedA, movedB))
    scale = max(abs(movedA[0]), 1e-6)
    check(abs(movedA[0]) > 1e-3 and
          abs(movedB[0] + movedA[0]) <= 0.1 * scale and
          abs(movedB[1] - movedA[1]) <= 0.1 * scale + 1e-4 and
          abs(movedB[2] - movedA[2]) <= 0.1 * scale + 1e-4,
          "Mirror X on: B, A's twin across x = 0, gets the mirrored stroke "
          "(A %r, B %r)" % (movedA, movedB))
    check(after["C"] == base["C"] and after["D"] == base["D"],
          "while the other pair stays put")
    check(int(session.undoDepth()) == depth + 1,
          "the symmetric stroke is one undo step (%d -> %d)"
          % (depth, int(session.undoDepth())))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(everyTube() == base, "and one Ctrl+Z restores both twins")
    box = mirrorBox()
    if box is not None and box.isChecked():
        clickBox(box)
    typeKey(view, "1")


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicSculpt needs testusdview (no live view)")
    sys.exit(0)
