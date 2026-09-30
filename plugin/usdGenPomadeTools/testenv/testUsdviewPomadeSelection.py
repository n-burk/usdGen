# testUsdviewPomadeSelection -- T3 selection acceptance through the real Qt
# StageView filter.  This deliberately never calls session.select(): every
# selection below is produced by the mouse path an artist uses.
import ctypes
import math
import os
import sys

failures = 0

# A clockwise square in the middle of the 4x4 XZ scalp fixture.
RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
STUB_CVS = 4


def frameComponentTargets(stage, view):
    """Frame an open tube almost end-on, with its center CVs separable.

    ``frameScalp`` intentionally looks exactly along the stub's +Y center
    line.  That remains useful for the later framebuffer assertion, but it
    overlays the center dots in screen space.  This slight oblique view still
    sees through the open tip while giving each displayed CV its own target.
    """
    from pxr import Gf, Sdf, UsdGeom

    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/PomadeSelectionCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(5.5, 12.0, 2.0)
    target = Gf.Vec3d(2.0, 2.0, 2.0)
    zAxis = (eye - target).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0.0, 0.0, 1.0), zAxis).GetNormalized()
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
    if op is None:
        op = xf.AddTransformOp()
    op.Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/PomadeSelectionCamera")
    return view.getActiveSceneCamera() is not None


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def wait(ms=30):
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    importlib.import_module("%s.QtTest" % PySideModule).QTest.qWait(int(ms))


def testenvDir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for index, argument in enumerate(sys.argv):
        if argument == "--testScript" and index + 1 < len(sys.argv):
            return os.path.dirname(os.path.abspath(sys.argv[index + 1]))
        if argument.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(argument.split("=", 1)[1]))
    return ""


def hover(session):
    """The model's current highlight, set only by the real hover event."""
    kind = ctypes.c_uint(0)
    ident = ctypes.c_int(-1)
    sub = ctypes.c_int(-1)
    subsub = ctypes.c_int(-1)
    rc = session.dll.Pomade_GetHover(session.model, ctypes.byref(kind),
                                    ctypes.byref(ident), ctypes.byref(sub),
                                    ctypes.byref(subsub))
    if rc != 0:
        return None
    return (int(kind.value), int(ident.value), int(sub.value),
            int(subsub.value))


def sectionCV(session, tubeId, ring, slot):
    """World position of one live section CV through the public C ABI."""
    from usdGenPomadeTools import pomadeBridge
    section = pomadeBridge.tubeSection(session.dll, session.model, tubeId,
                                      ring)
    if slot < 0 or slot >= len(section[1]):
        return None
    entry = session.dll.Pomade_GetTubeSectionFrame
    cfloat3 = ctypes.POINTER(ctypes.c_float)
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, cfloat3,
                      cfloat3, cfloat3, cfloat3]
    entry.restype = ctypes.c_int
    origin = (ctypes.c_float * 3)()
    frame = (ctypes.c_float * 9)()
    scale = ctypes.c_float(1.0)
    twist = ctypes.c_float(0.0)
    if entry(session.model, int(tubeId), int(ring), origin, frame,
             ctypes.byref(scale), ctypes.byref(twist)) != 0:
        return None
    ct, st = math.cos(twist.value), math.sin(twist.value)

    def placed(pair):
        u, v = pair[0] * scale.value, pair[1] * scale.value
        return (u * ct - v * st, u * st + v * ct)

    placedRing = [placed(pair) for pair in section[1]]
    # Pomade_GetTubeSectionFrame returns the ring centroid as origin.  Center
    # the transformed authored UVs before applying that frame.
    meanU = sum(pair[0] for pair in placedRing) / len(placedRing)
    meanV = sum(pair[1] for pair in placedRing) / len(placedRing)
    u, v = placedRing[slot]
    return tuple(origin[axis] + frame[axis] * (u - meanU) +
                 frame[axis + 3] * (v - meanV) for axis in range(3))


def sectionRingCenter(session, tubeId, ring):
    """The public frame API's origin is the pickable ring centroid."""
    entry = session.dll.Pomade_GetTubeSectionFrame
    cfloat3 = ctypes.POINTER(ctypes.c_float)
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, cfloat3,
                      cfloat3, cfloat3, cfloat3]
    entry.restype = ctypes.c_int
    origin = (ctypes.c_float * 3)()
    frame = (ctypes.c_float * 9)()
    if entry(session.model, int(tubeId), int(ring), origin, frame, None,
             None) != 0:
        return None
    return tuple(float(value) for value in origin)


def sendMouse(view, kind, physical, button, buttons, modifiers=()):
    """One direct QMouseEvent, with the Alt/Meta names the Tube helper lacks."""
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
    try:
        ratio = float(view.devicePixelRatioF())
    except AttributeError:
        ratio = 1.0
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier,
             "alt": QtCore.Qt.KeyboardModifier.AltModifier,
             "meta": QtCore.Qt.KeyboardModifier.MetaModifier}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    for name in modifiers:
        mods |= table[name]
    point = QtCore.QPoint(int(round(physical[0] / ratio)),
                          int(round(physical[1] / ratio)))
    event = QtGui.QMouseEvent(kind, QtCore.QPointF(point),
                              QtCore.QPointF(view.mapToGlobal(point)),
                              button, buttons, mods)
    QtWidgets.QApplication.sendEvent(view, event)


def _inBox(point, a, b):
    return (min(a[0], b[0]) <= point[0] <= max(a[0], b[0]) and
            min(a[1], b[1]) <= point[1] <= max(a[1], b[1]))


def checkGraphModifiers(session, mouse, pixel):
    """SL-01 in Graph: modified node clicks and a Ctrl band.

    Right after the Draw stroke, under the top-down camera: the four
    corner nodes sit on the RECT corners. A Shift or Ctrl press is a node
    selection in every Graph tool (never a stroke), a click without travel
    and a band with it, all through the one modifier table.
    """
    from usdGenPomadeTools import pomadeLib
    kind = pomadeLib.POMADE_PICK_GRAPH_NODE

    def nodes():
        return session.readSelection(kind)

    corners = [pixel((x, 0.0, z)) for x, z in RECT]
    session.clearSelection()
    mouse.click(corners[0], ("shift",))
    first = nodes()
    check(len(first) == 1,
          "SL-01 Graph: a Shift-click on a node selects it %r" % (first,))
    mouse.click(corners[1], ("shift",))
    both = nodes()
    check(len(both) == 2 and set(first) < set(both),
          "SL-01 Graph: Shift-click extends the node selection %r" % (both,))
    mouse.click(corners[1], ("ctrl",))
    check(nodes() == first,
          "SL-01 Graph: Ctrl-click removes the node %r" % (nodes(),))
    mouse.click(corners[1], ("ctrl",))
    check(nodes() == first,
          "SL-01 Graph: a Ctrl-click never adds an unselected node %r"
          % (nodes(),))
    mouse.click(corners[1], ("ctrl", "shift"))
    check(nodes() == both,
          "SL-01 Graph: Ctrl+Shift-click adds it back %r" % (nodes(),))
    start = (corners[1][0] - 12.0, corners[1][1] - 12.0)
    finish = (corners[1][0] + 12.0, corners[1][1] + 12.0)
    mouse.drag([start, finish], ("ctrl",))
    check(nodes() == first,
          "SL-01 Graph: a Ctrl band removes the node it covers %r"
          % (nodes(),))
    check(session.graphCounts() == (4, 4, 1),
          "SL-01 Graph: no modified press authored anything %r"
          % (session.graphCounts(),))
    session.clearSelection()


def checkTubeModifiers(session, viewport, mouse, cvPixels, outward):
    """SL-01 in Tube Center: modified CV clicks, then Shift and Ctrl bands.

    `cvPixels` are the root's center CVs under the oblique camera (tip
    last), `outward` the unit screen direction from the prior CV to the
    tip. The Select tool shows no gizmo, so every press is a selection
    press (a Move gizmo's handle rightly wins a press over it, GZ-01).
    """
    from usdGenPomadeTools import pomadeLib
    kind = pomadeLib.POMADE_PICK_CENTER_CV
    loop = viewport.loop
    toolWas = loop.transformTool()
    loop.setTransformTool("select")
    tip = (0, STUB_CVS - 1, -1)
    prior = (0, STUB_CVS - 2, -1)

    def cvs():
        return session.readSelection(kind)

    try:
        session.clearSelection()
        mouse.click(cvPixels[-1])
        mouse.click(cvPixels[-2], ("shift",))
        check(cvs() == [prior, tip],
              "SL-01 Tube: Shift-click extends the CV selection %r" % (cvs(),))
        mouse.click(cvPixels[-1], ("ctrl",))
        check(cvs() == [prior],
              "SL-01 Tube: Ctrl-click removes the tip CV %r" % (cvs(),))
        mouse.click(cvPixels[-1], ("ctrl",))
        check(cvs() == [prior],
              "SL-01 Tube: a Ctrl-click never adds an unselected CV %r"
              % (cvs(),))
        mouse.click(cvPixels[-1], ("ctrl", "shift"))
        check(cvs() == [prior, tip],
              "SL-01 Tube: Ctrl+Shift-click adds the tip CV %r" % (cvs(),))
        mouse.click(cvPixels[-1], ("ctrl", "shift"))
        check(cvs() == [prior, tip],
              "SL-01 Tube: Ctrl+Shift-click adds, it never toggles %r"
              % (cvs(),))
        mouse.click(cvPixels[-2], ("shift",))
        check(cvs() == [tip],
              "SL-01 Tube: Shift-click on a selected CV toggles it off %r"
              % (cvs(),))

        # A Shift band over every CV, then a Ctrl band over only the tip.
        session.clearSelection()
        xs = [p[0] for p in cvPixels]
        ys = [p[1] for p in cvPixels]
        mouse.drag([(min(xs) - 30.0, min(ys) - 30.0),
                    (max(xs) + 30.0, max(ys) + 30.0)], ("shift",))
        every = [(0, cv, -1) for cv in range(STUB_CVS)]
        check(cvs() == every,
              "SL-01 Tube: a Shift band selects every root CV %r" % (cvs(),))
        # The Ctrl press starts beyond the tip, away from every other CV
        # and outside the 8 px CV target, so it is a band, not a click; the
        # box reaches 4 px back past the tip and so stops short of the
        # prior CV (> 16 px away).
        sx = 1.0 if outward[0] >= 0.0 else -1.0
        sy = 1.0 if outward[1] >= 0.0 else -1.0
        start = (cvPixels[-1][0] + 14.0 * sx, cvPixels[-1][1] + 14.0 * sy)
        finish = (cvPixels[-1][0] - 4.0 * sx, cvPixels[-1][1] - 4.0 * sy)
        inside = [cv for cv, p in enumerate(cvPixels)
                  if _inBox(p, start, finish)]
        check(inside == [STUB_CVS - 1],
              "SL-01 Tube: the Ctrl band's box encloses only the tip %r"
              % (inside,))
        mouse.drag([start, finish], ("ctrl",))
        check(cvs() == every[:-1],
              "SL-01 Tube: a Ctrl band after a Shift band removes the tip "
              "CV %r" % (cvs(),))
    finally:
        loop.setTransformTool(toolWas)
        session.clearSelection()


def checkHierarchyModifiers(session, mouse, childPixels):
    """SL-01 in Hierarchy: modified tube clicks at the entered child level.

    `childPixels` are pickable surface points of two child tubes (None for
    one that did not project).
    """
    from usdGenPomadeTools import pomadeLib
    kind = pomadeLib.POMADE_PICK_TUBE_VERT
    if len(childPixels) < 2 or None in childPixels:
        check(False, "SL-01 Hierarchy: two child tubes project to pixels %r"
              % (childPixels,))
        return

    def tubes():
        return session.readSelection(kind)

    session.clearSelection()
    mouse.click(childPixels[0])
    first = tubes()
    check(len(first) == 1,
          "SL-01 Hierarchy: a click selects one child tube %r" % (first,))
    mouse.click(childPixels[1], ("shift",))
    both = tubes()
    check(len(both) == 2 and set(first) < set(both),
          "SL-01 Hierarchy: a Shift-click keeps the previous tube %r"
          % (both,))
    mouse.click(childPixels[1], ("ctrl",))
    check(tubes() == first,
          "SL-01 Hierarchy: Ctrl-click removes the tube %r" % (tubes(),))
    mouse.click(childPixels[1], ("ctrl", "shift"))
    check(tubes() == both,
          "SL-01 Hierarchy: Ctrl+Shift-click adds it %r" % (tubes(),))
    mouse.click(childPixels[0], ("shift",))
    check(len(both) == 2 and tubes() == [t for t in both if t not in first],
          "SL-01 Hierarchy: a Shift-click on a selected tube toggles it "
          "off %r" % (tubes(),))
    session.clearSelection()


def checkInputHygiene(appController, context):
    """FB-01: hover, camera drags, brush misses, key focus and dock close.

    Every step goes through the real StageView event filter (or, with the
    dock hidden, its absence).  `context` carries the handles `run` built;
    `target` is a displayed section CV `(pixel, tube, ring)` under the
    end-on `frameScalp` camera, or None when the child section setup failed
    (reported there already).
    """
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    from usdGenPomadeTools import pomadeCamera, pomadeLib
    from testUsdviewPomadeTube import _qtTest, frameScalp

    view = context["view"]
    stage = context["stage"]
    dataModel = context["dataModel"]
    workspace = context["workspace"]
    session = context["session"]
    viewport = context["viewport"]
    state = context["state"]
    mouse = context["mouse"]
    target = context["target"]
    api = viewport._api
    LEFT = QtCore.Qt.MouseButton.LeftButton
    NONE = QtCore.Qt.MouseButton.NoButton

    def usdSelection():
        return [str(path) for path in (api.selectedPaths or [])]

    def selectCamera():
        # A prim nowhere under the cursor, so any usdview pick changes it.
        dataModel.selection.setPrimPath("/PomadeTubeCamera")
        wait(10)
        return usdSelection()

    def pumpIdle():
        for _ in range(200):
            if not session.hasPendingWork():
                return True
            viewport.pumpOnce()
            wait(25)
        return not session.hasPendingWork()

    check(frameScalp(stage, view), "FB-01: the end-on camera is restored")
    wait(30)
    camera = pomadeCamera.resolve(view)
    if camera is None or target is None:
        check(False, "FB-01: a displayed section CV target exists")
        return
    cvPixel, child, ring = target

    # 1. Press-less moves are Pomade's: usdview's Hydra rollover pick (and
    # its prim tooltip) must never run while the workspace is open.
    rollovers = []

    def onRollover(*_args):
        rollovers.append(1)

    view.signalPrimRollover.connect(onRollover)
    try:
        for index in range(20):
            offset = (index % 5) * 4.0 - 8.0
            mouse.unheldMove((cvPixel[0] + offset,
                              cvPixel[1] + (index // 5) * 30.0 - 45.0))
            wait(5)
    finally:
        view.signalPrimRollover.disconnect(onRollover)
    check(not rollovers,
          "FB-01: 20 hover moves never reach StageView's rollover pick (%d)"
          % len(rollovers))
    check(not QtWidgets.QToolTip.isVisible(),
          "FB-01: no usdview prim tooltip over a Pomade hover")

    # 2. A hidden dock gives the StageView back to usdview: a click over a
    # tube CV picks a USD prim and never touches the Pomade selection.
    stage.GetPrimAtPath("/Scalp").SetActive(True)
    wait(20)
    workspace.hide()
    wait(20)
    check(getattr(viewport, "suspended", False) and viewport.installed,
          "FB-01: hiding the dock suspends the controller, keeping the model")
    before = selectCamera()
    count = session.selectionCount(0)
    mouse.click(cvPixel)
    wait(20)
    check(session.selectionCount(0) == count,
          "FB-01: a closed-dock click leaves the Pomade selection alone")
    check(usdSelection() != before,
          "FB-01: a closed-dock click is usdview's prim pick %r -> %r"
          % (before, usdSelection()))

    # 3. Showing the dock resumes Pomade picking, and a Pomade click is no
    # longer a usdview pick.
    workspace.show()
    wait(20)
    check(not getattr(viewport, "suspended", True),
          "FB-01: showing the dock resumes the controller")
    before = selectCamera()
    session.clearSelection()
    mouse.click(cvPixel)
    wait(10)
    picked = session.readSelection(pomadeLib.POMADE_PICK_SECTION_CV)
    check((child, ring, 0) in picked,
          "FB-01: the re-shown dock picks the section CV again %r" % (picked,))
    check(usdSelection() == before,
          "FB-01: a Pomade click leaves the usdview selection alone")

    # 4. An Alt+LMB camera drag renders once per move: StageView's own
    # render, with no Pomade hover render on top, and no hover change.
    pumpIdle()
    mouse.unheldMove((cvPixel[0] + 40.0, cvPixel[1] + 40.0))
    wait(20)
    hoverBefore = hover(session)
    stageViewClass = type(view)
    originalPaint = stageViewClass.paintGL
    paints = [0]

    def countingPaint(self, *args, **kwargs):
        paints[0] += 1
        return originalPaint(self, *args, **kwargs)

    start = (cvPixel[0] + 40.0, cvPixel[1] + 40.0)
    sendMouse(view, QtCore.QEvent.Type.MouseButtonPress, start, LEFT, LEFT,
              ("alt",))
    QtWidgets.QApplication.processEvents()
    stageViewClass.paintGL = countingPaint
    try:
        for index in range(30):
            sendMouse(view, QtCore.QEvent.Type.MouseMove,
                      (start[0] + 3.0 * (index + 1), start[1]), NONE, LEFT,
                      ("alt",))
            QtWidgets.QApplication.processEvents()
    finally:
        stageViewClass.paintGL = originalPaint
    hoverAfter = hover(session)
    sendMouse(view, QtCore.QEvent.Type.MouseButtonRelease,
              (start[0] + 90.0, start[1]), LEFT, NONE, ("alt",))
    wait(20)
    check(paints[0] == 30,
          "FB-01: 30 Alt+LMB camera moves render exactly 30 times (%d)"
          % paints[0])
    check(hoverAfter == hoverBefore,
          "FB-01: a camera drag leaves the Pomade hover alone %r -> %r"
          % (hoverBefore, hoverAfter))
    check(frameScalp(stage, view), "FB-01: the scene camera is restored")
    wait(30)

    # 5. Hotkeys survive a dock edit: clicking the viewport takes the keys
    # back from a focused spin box.
    window = getattr(appController, "_mainWindow", None)
    if window is not None:
        window.activateWindow()
        window.raise_()
    spins = [spin for spin in workspace.findChildren(QtWidgets.QDoubleSpinBox)
             if spin.isVisible()]
    if not spins:
        spins = [spin for spin in
                 workspace.findChildren(QtWidgets.QAbstractSpinBox)
                 if spin.isVisible()]
    check(bool(spins), "FB-01: the dock shows a spin box to focus")
    if spins:
        spins[0].setFocus()
        wait(20)
        print("info: dock spin box holds the keys before the click: %s"
              % viewport._textFocus())
    mouse.click((cvPixel[0] + 150.0, cvPixel[1] + 150.0))
    wait(20)
    check(view.hasFocus(),
          "FB-01: a viewport click takes keyboard focus from the dock")
    check(not viewport._textFocus(),
          "FB-01: no dock text field holds the keys after the click")
    _qtTest().QTest.keyClick(view, QtCore.Qt.Key.Key_5)
    wait(20)
    check(state.activeMode == "sculpt",
          "FB-01: 5 reaches Sculpt after a dock field had focus (%s)"
          % state.activeMode)

    # 6. A Sculpt miss over the active USD scalp is still a Pomade click:
    # StageView must not pick /Scalp under it.
    if state.activeMode != "sculpt":
        viewport.setMode("sculpt")   # already reported above; test the miss
    session.clearSelection()
    camera = pomadeCamera.resolve(view)
    projected = camera.worldToPixels((0.15, 0.0, 3.85))
    corner = (projected[0], projected[1])
    before = selectCamera()
    mouse.press(corner)
    wait(10)
    check(not viewport.gestureActive,
          "FB-01: a Sculpt press off every tube starts no gesture")
    mouse.release(corner)
    wait(20)
    check(usdSelection() == before,
          "FB-01: a Sculpt miss never falls through to usdview's pick %r -> %r"
          % (before, usdSelection()))
    stage.GetPrimAtPath("/Scalp").SetActive(False)
    wait(20)


def checkNavigation(appController, context):
    """FB-03: MMB/RMB never pick, the host application MMB drives the camera, F frames.

    `context` is the one `checkInputHygiene` took; `target` is the child
    section CV `(pixel, tube, ring)` under the end-on `frameScalp` camera.
    """
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    from usdGenPomadeTools import pomadeBridge, pomadeCamera, pomadeLib
    from testUsdviewPomadeTube import _qtTest, frameScalp, typeKey

    view = context["view"]
    stage = context["stage"]
    session = context["session"]
    viewport = context["viewport"]
    state = context["state"]
    mouse = context["mouse"]
    target = context["target"]
    api = viewport._api
    settings = view._dataModel.viewSettings
    MIDDLE = QtCore.Qt.MouseButton.MiddleButton
    RIGHT = QtCore.Qt.MouseButton.RightButton
    NONE = QtCore.Qt.MouseButton.NoButton
    PRESS = QtCore.QEvent.Type.MouseButtonPress
    MOVE = QtCore.QEvent.Type.MouseMove
    RELEASE = QtCore.QEvent.Type.MouseButtonRelease
    def usdSelection():
        return [str(path) for path in (api.selectedPaths or [])]

    def frameTarget():
        """(pixel, selection-kind key, kind, world point of a picked item).

        The child section CV when the Hierarchy setup made one; otherwise
        (already reported there) the root tube's tip centre CV, so framing
        is still proven.
        """
        camera = pomadeCamera.resolve(view)
        if target is not None:
            pixelAt, child, ring = target
            return (pixelAt, "f11", pomadeLib.POMADE_PICK_SECTION_CV,
                    lambda item: sectionCV(session, item[0], item[1],
                                           item[2]))
        tip = pomadeBridge.tubeCenterHandle(session.dll, session.model, 0,
                                           STUB_CVS - 1)
        projected = camera.worldToPixels(tip)
        return ((projected[0], projected[1]), "f9",
                pomadeLib.POMADE_PICK_CENTER_CV,
                lambda item: pomadeBridge.tubeCenterHandle(
                    session.dll, session.model, item[0], item[1]))

    def drag(start, delta, button, modifiers=(), steps=10):
        sendMouse(view, PRESS, start, button, button, modifiers)
        QtWidgets.QApplication.processEvents()
        for index in range(steps):
            f = float(index + 1) / steps
            sendMouse(view, MOVE, (start[0] + delta[0] * f,
                                   start[1] + delta[1] * f),
                      NONE, button, modifiers)
            QtWidgets.QApplication.processEvents()
        end = (start[0] + delta[0], start[1] + delta[1])
        sendMouse(view, RELEASE, end, button, NONE, modifiers)
        wait(20)

    def offCentre(point):
        """|dx|, |dy| of a world point from the view centre, in view units."""
        camera = pomadeCamera.resolve(view)
        projected = camera.worldToPixels(point)
        return (abs(projected[0] - 0.5 * camera.width) / camera.width,
                abs(projected[1] - 0.5 * camera.height) / camera.height)

    # StageView's pick emits signalPrimSelected for every press it sees,
    # hit or miss, and a right pick would open usdview's prim context menu
    # (closed here, and counted, so a regression cannot hang the test).
    picks = []
    popups = []

    def onPick(*_args):
        picks.append(1)

    def closePopup():
        popup = QtWidgets.QApplication.activePopupWidget()
        if popup is not None:
            popups.append(type(popup).__name__)
            popup.close()

    closer = QtCore.QTimer()
    closer.setInterval(20)
    closer.timeout.connect(closePopup)
    closer.start()
    view.signalPrimSelected.connect(onPick)
    styleWas = state.navigationStyle
    try:
        check(frameScalp(stage, view), "FB-03: the end-on camera is restored")
        wait(30)
        cvPixel, kindKey, kind, positionOf = frameTarget()
        spot = (cvPixel[0] + 120.0, cvPixel[1] + 120.0)

        # 1. A DCC (the default): a plain MMB/RMB click and drag is Pomade's
        # and inert -- no prim pick, no context menu, no camera move.
        check(styleWas == "maya", "FB-03: Maya navigation is the default")
        if state.activeMode != "sculpt":
            viewport.setMode("sculpt")
        before = usdSelection()
        cameraBefore = pomadeCamera.resolve(view).viewProj
        drag(spot, (40.0, 0.0), MIDDLE)
        drag(spot, (40.0, 0.0), RIGHT)
        check(not picks,
              "FB-03: plain MMB/RMB never reach StageView's pick (%d)"
              % len(picks))
        check(not popups,
              "FB-03: a right click opens no usdview context menu %r"
              % (popups,))
        check(usdSelection() == before,
              "FB-03: the usdview prim selection is untouched %r -> %r"
              % (before, usdSelection()))
        check(pomadeCamera.resolve(view).viewProj == cameraBefore,
              "FB-03: a Maya-style plain MMB drag leaves the camera alone")

        # 2. A DCC: plain MMB orbits usdview's free camera.
        state.navigationStyle = "blender"
        sendMouse(view, PRESS, spot, MIDDLE, MIDDLE)
        QtWidgets.QApplication.processEvents()
        free = settings.freeCamera
        check(free is not None and settings.cameraPrim is None,
              "FB-03: a Blender MMB press switches to the free camera")
        rotBefore = ((free.rotTheta, free.rotPhi) if free is not None
                     else None)
        for index in range(10):
            sendMouse(view, MOVE, (spot[0] + 6.0 * (index + 1),
                                   spot[1] + 3.0 * (index + 1)),
                      NONE, MIDDLE)
            QtWidgets.QApplication.processEvents()
        free = settings.freeCamera
        rotAfter = ((free.rotTheta, free.rotPhi) if free is not None
                    else None)
        sendMouse(view, RELEASE, (spot[0] + 60.0, spot[1] + 30.0), MIDDLE,
                  NONE)
        wait(20)
        check(rotBefore is not None and rotAfter != rotBefore,
              "FB-03: a Blender MMB drag orbits the free camera %r -> %r"
              % (rotBefore, rotAfter))
        check(not picks and usdSelection() == before,
              "FB-03: the Blender orbit never picks a prim")
        check(not viewport.gestureActive,
              "FB-03: the orbit is no Pomade gesture")

        # 3. F frames the Pomade selection.  Select one CV with a real
        # click, pan it off centre with Shift+MMB, then F.
        check(frameScalp(stage, view), "FB-03: the end-on camera again")
        wait(30)
        typeKey(view, "2")
        typeKey(view, kindKey)
        wait(10)
        session.clearSelection()
        mouse.click(cvPixel)
        wait(10)
        picked = session.readSelection(kind)
        check(len(picked) == 1,
              "FB-03: a click selects one CV to frame %r" % (picked,))
        point = positionOf(picked[0]) if picked else None
        if point is None:
            check(False, "FB-03: the selected CV reads back")
            return
        drag(spot, (-260.0, 160.0), MIDDLE, ("shift",))
        away = offCentre(point)
        check(max(away) > 0.1,
              "FB-03: Shift+MMB pans the CV off centre (%.2f, %.2f)" % away)
        check(not picks,
              "FB-03: the Blender pan never picks a prim (%d)" % len(picks))
        _qtTest().QTest.keyClick(view, QtCore.Qt.Key.Key_F)
        wait(30)
        framed = offCentre(point)
        check(framed[0] < 0.1 and framed[1] < 0.1,
              "FB-03: F centres the selected CV (%.3f, %.3f of the view)"
              % framed)
        check(not viewport._brushResizeArmed,
              "FB-03: Tube F never arms the Sculpt brush resize")
    finally:
        state.navigationStyle = styleWas
        closer.stop()
        view.signalPrimSelected.disconnect(onPick)
    check(frameScalp(stage, view), "FB-03: the scene camera is restored")
    wait(30)


def checkComponentConversion(view, session, viewport, state, mouse,
                             cvPixel, child, ring, others):
    """GZ-04: F8-F11 convert the selection instead of clearing it.

    Starts in Tube Section with the child's section CV `(child, ring, 0)`
    under `cvPixel` (the end-on camera); leaves Section with that CV
    selected again, so the hygiene/navigation checks see what they did.
    `others` are the remaining child tubes, for the owner-set restriction.
    """
    from usdGenPomadeTools import (pomadeBridge, pomadeCamera, pomadeLib,
                                  pomadeLoops)
    from testUsdviewPomadeTube import typeKey
    TUBE = pomadeLib.POMADE_PICK_TUBE_VERT
    CENTER = pomadeLib.POMADE_PICK_CENTER_CV
    RING = pomadeLib.POMADE_PICK_SECTION_RING
    SECTION = pomadeLib.POMADE_PICK_SECTION_CV

    def key(name):
        typeKey(view, name)
        wait(15)

    def clickFresh():
        # An empty selection hides the gizmo, so the click is the CV's.
        session.clearSelection()
        viewport.refreshGizmo()
        mouse.click(cvPixel)

    clickFresh()
    check(session.readSelection(SECTION) == [(child, ring, 0)],
          "GZ-04: the child section CV is selected before converting %r"
          % (session.readSelection(SECTION),))
    key("f10")
    check(state.tubeSubMode == "ring" and
          (child, ring, -1) in session.readSelection(RING) and
          not session.readSelection(SECTION),
          "GZ-04: F10 converts the section CV into its ring %r"
          % (session.readSelection(RING),))
    count = pomadeBridge.tubeCenterCount(session.dll, session.model, child)
    t = pomadeBridge.tubeSection(session.dll, session.model, child, ring)[0]
    owner = max(0, min(count - 1, int(round(t * (count - 1)))))
    key("f9")
    check(state.tubeSubMode == "center" and
          (child, owner, -1) in session.readSelection(CENTER),
          "GZ-04: F9 converts the ring into the centre CV %d that owns it %r"
          % (owner, session.readSelection(CENTER)))
    key("f8")
    check(state.tubeSubMode == "tube" and
          session.readSelection(TUBE) == [(child, -1, -1)] and
          not session.readSelection(CENTER),
          "GZ-04: F8 converts the centre CV into its whole tube %r"
          % (session.readSelection(TUBE),))

    # A body click in Whole Tube, then F9: the tube stays selected as the
    # owner set and only its own components answer a click.
    snap = state.snapRadiusPx
    state.snapRadiusPx = 8.0
    clickFresh()
    state.snapRadiusPx = snap
    check(session.readSelection(TUBE) == [(child, -1, -1)],
          "GZ-04: a body click in Whole Tube selects the child tube %r"
          % (session.readSelection(TUBE),))
    key("f9")
    check(state.tubeSubMode == "center" and
          session.readSelection(TUBE) == [(child, -1, -1)],
          "GZ-04: F9 after a body click keeps the tube as the owner set %r"
          % (session.readSelection(TUBE),))
    camera = pomadeCamera.resolve(view)
    loop = viewport.loop
    probed = False
    for other in others:
        handle = pomadeBridge.tubeCenterHandle(session.dll, session.model,
                                              other, 0)
        projected = camera.worldToPixels(handle)
        sample = pomadeLoops.Sample(session, camera, projected[0],
                                   projected[1])
        raw = sample.item(CENTER, loop.componentPickRadiusPx(camera))
        if raw is None or int(raw["id"]) != other:
            continue
        probed = True
        check(loop._componentItem(sample) is None,
              "GZ-04: a centre CV of tube %d is ignored while tube %d owns "
              "the selection" % (other, child))
        break
    if not probed:
        check(False, "GZ-04: another tube's centre CV is pickable to probe "
              "the owner restriction (others %r)" % (list(others),))
    handle = pomadeBridge.tubeCenterHandle(session.dll, session.model, child,
                                          count - 1)
    projected = camera.worldToPixels(handle)
    own = loop._componentItem(pomadeLoops.Sample(session, camera,
                                                projected[0], projected[1]))
    check(own is not None and int(own["id"]) == child,
          "GZ-04: the owner tube's own centre CVs still answer %r" % (own,))

    # Back to Section with the child CV, as the caller left it.
    key("f11")
    clickFresh()
    check(state.tubeSubMode == "section" and
          session.readSelection(SECTION) == [(child, ring, 0)],
          "GZ-04: Section and the child CV are restored %r"
          % (session.readSelection(SECTION),))


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenPomadeTools
        from usdGenPomadeTools import (pomadeBridge, pomadeCamera,
                                      pomadeHierarchy, pomadeLib)
        # Reuse only the Qt event/camera/framebuffer mechanics.  This test
        # owns the selection assertions and does not execute testTube's run.
        from testUsdviewPomadeTube import (Mouse, _qtTest, frameScalp,
                                          typeKey, whiteFraction)
    except ImportError as exc:
        print("FAIL: cannot import Pomade selection helpers: %s" % exc)
        return 1

    def selectKey(name, modifiers=()):
        """Ctrl+A / Ctrl+Shift+A / Ctrl+I through QtTest (SL-03); the shared
        Tube key table has no A or I."""
        from pxr.Usdviewq.qt import QtCore
        keys = {"a": QtCore.Qt.Key.Key_A, "i": QtCore.Qt.Key.Key_I}
        table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
                 "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
        mods = QtCore.Qt.KeyboardModifier.NoModifier
        for modifier in modifiers:
            mods |= table[modifier]
        _qtTest().QTest.keyClick(view, keys[name], mods)
        wait(15)

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    container = usdGenPomadeTools.container()
    check(view is not None and registry is not None and container is not None,
          "usdview supplies a StageView and Pomade command registry")
    if view is None or registry is None or container is None:
        return 1

    def shutdown():
        viewport = getattr(container, "viewport", None)
        session = getattr(container, "session", None)
        try:
            if viewport is not None:
                viewport.uninstall()
        finally:
            if session is not None:
                session.deactivate()

    check(frameScalp(stage, view), "a stable top camera frames the scalp")
    view.setFocus()
    wait(30)
    try:
        rolloverBefore = bool(view.rolloverPicking)
    except AttributeError:
        rolloverBefore = None

    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenPomadeTools.openWorkspace").run()
    workspace = container.workspace
    registry.getCommandPlugin("usdGenPomadeTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.pomadeState
    check(workspace is not None and workspace.isVisible() and
          session is not None and session.model is not None and
          viewport is not None and viewport.installed,
          "opening the dock binds a live model and viewport controller")
    if session is None or session.model is None or viewport is None:
        shutdown()
        return 1

    # The controller owns usdview rollover while its workspace is visible:
    # Pomade's own hover/highlight must be the only mouse-over presentation.
    if rolloverBefore is not None:
        check(not bool(view.rolloverPicking),
              "the visible workspace suppresses StageView rollover picking")
    check(viewport._marqueeOverlay is not None and
          viewport._marqueeOverlay.testAttribute(
              viewport._marqueeOverlay.Qt.WA_TransparentForMouseEvents)
          if hasattr(viewport._marqueeOverlay, "Qt") else
          viewport._marqueeOverlay is not None,
          "the selection-band overlay is installed as a mouse-transparent child")

    # The scalp is copied at bind time. Hiding its USD prim avoids z fighting
    # without changing the model's K1 scalp used by the Graph stroke.
    stage.GetPrimAtPath("/Scalp").SetActive(False)
    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the live controller resolves a camera")
    if camera is None:
        shutdown()
        return 1

    def pixel(point):
        projected = camera.worldToPixels(point)
        return (projected[0], projected[1])

    def centerHandle(tubeId, cv):
        """The centered core point artists see and can click."""
        return pomadeBridge.tubeCenterHandle(session.dll, session.model,
                                            tubeId, cv)

    def clickControl(actionId):
        """Invoke a dock action through its Qt button (DK-04 hook)."""
        button = workspace.button("action", actionId)
        if button is None:
            return False
        button.click()
        wait(15)
        return True

    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)
    mouse = Mouse(view)
    # This is still a real QMouseEvent delivered through the installed Qt
    # filter; it avoids the hidden test window's global-coordinate routing.
    mouse.direct = True
    viewport.setPointerInside(True)

    # Create one visible tube using the ordinary Graph freehand workflow.
    typeKey(view, "d")
    path = []
    for index, (x0, z0) in enumerate(RECT):
        x1, z1 = RECT[(index + 1) % len(RECT)]
        for step in range(5):
            t = float(step) / 5.0
            path.append(pixel((x0 + (x1 - x0) * t, 0.0,
                               z0 + (z1 - z0) * t)))
    path.append(pixel((RECT[0][0], 0.0, RECT[0][1])))
    mouse.drag(path)
    check(session.graphCounts() == (4, 4, 1) and
          int(session.dll.Pomade_GetCenterCVCount(session.model)) == STUB_CVS,
          "a real Graph stroke creates the visible five-CV root tube")
    checkGraphModifiers(session, mouse, pixel)

    typeKey(view, "2")
    check(state.activeMode == "tube" and state.tubeSubMode == "center",
          "Tube Center owns the ensuing click, hover and marquee input")
    tip = centerHandle(0, STUB_CVS - 1)
    check(tip is not None, "the visible root-tip center CV is readable")
    if tip is None:
        shutdown()
        return 1

    # Graph Snap can be deliberately tiny without shrinking the direct
    # manipulation target of an already displayed CV.  Six physical pixels
    # is inside the fixed 8px component target and intentionally outside the
    # former 2px generic pick floor.  The normal scalp camera is deliberately
    # end-on, so use an almost-end-on view which separates the dots before
    # checking an exact CV identity.
    check(frameComponentTargets(stage, view),
          "an oblique open-tube camera makes center CV targets distinct")
    wait(30)
    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the component-target camera resolves")
    if camera is None:
        shutdown()
        return 1
    tipPixel = pixel(tip)
    priorPixel = pixel(centerHandle(0, STUB_CVS - 2))
    tipGap = ((tipPixel[0] - priorPixel[0]) ** 2 +
              (tipPixel[1] - priorPixel[1]) ** 2) ** 0.5
    check(tipGap > 16.0,
          "the visible tip has a separate screen target (%.2fpx)" % tipGap)
    outward = ((tipPixel[0] - priorPixel[0]) / max(tipGap, 1e-9),
               (tipPixel[1] - priorPixel[1]) / max(tipGap, 1e-9))
    state.snapRadiusPx = 0.25
    tipNearPixel = (tipPixel[0] + 6.0 * outward[0],
                    tipPixel[1] + 6.0 * outward[1])

    # A press-less move travels through the real hover route.  It must
    # identify the same center CV a click would take and clear off-target.
    mouse.move(tipNearPixel)
    wait(15)
    check(hover(session) == (pomadeLib.POMADE_PICK_CENTER_CV, 0,
                             STUB_CVS - 1, -1),
          "an 8px CV target prehighlights the exact per-tube tip under tiny Snap")
    mouse.move((10.0, 10.0))
    wait(15)
    check(hover(session) == (0, -1, -1, -1),
          "moving off the tube clears the Pomade highlight")

    mouse.click(tipNearPixel)
    nearSelection = session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV)
    check(nearSelection == [(0, STUB_CVS - 1, -1)] and
          not session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT),
          "the offset CV click takes the CV rather than its tube body %r" %
          (nearSelection,))
    # SL-03: Escape only cancels a live gesture; Ctrl+Shift+A deselects.
    typeKey(view, "escape")
    wait(10)
    check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) ==
          [(0, STUB_CVS - 1, -1)],
          "SL-03: Escape with nothing live keeps the clicked CV selected")
    selectKey("a", ("ctrl", "shift"))
    check(not session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV),
          "SL-03: Ctrl+Shift+A deselects it")
    checkTubeModifiers(session, viewport, mouse,
                       [pixel(centerHandle(0, cv)) for cv in range(STUB_CVS)],
                       outward)

    # Restore the end-on view for the framebuffer colour assertion below.
    check(frameScalp(stage, view),
          "the end-on camera is restored for the visual selection check")
    wait(30)
    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the restored selection camera resolves")
    if camera is None:
        shutdown()
        return 1
    tipPixel = pixel(tip)

    # Actual click, not a test-side selection call: it must select the tip
    # and publish the expected white visual selection highlight.
    whiteBefore = whiteFraction(view, camera, tip)
    mouse.click(tipPixel)
    selected = session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV)
    whiteAfter = whiteFraction(view, camera, tip)
    check(selected == [(0, STUB_CVS - 1, -1)],
          "a visible center-CV click selects exactly that tube and CV %r"
          % (selected,))
    check(not session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) and
          not session.readSelection(pomadeLib.POMADE_PICK_SECTION_RING),
          "the visible CV click does not retain a whole tube or ring")
    check(whiteAfter > whiteBefore + 0.05,
          "the click's selected CV is visibly highlighted %.2f -> %.2f"
          % (whiteBefore, whiteAfter))

    # SL-03: Escape never throws a selection away -- from the viewport or
    # from a focused dock field, which owns the key.
    tipOnly = [(0, STUB_CVS - 1, -1)]
    allCVs = [(0, index, -1) for index in range(STUB_CVS)]
    typeKey(view, "escape")
    wait(10)
    check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) == tipOnly,
          "SL-03: Escape keeps the clicked CV selected")
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    spins = [spin for spin in
             workspace.findChildren(QtWidgets.QAbstractSpinBox)
             if spin.isVisible()]
    check(bool(spins), "SL-03: the dock shows a spin box to focus")
    if spins:
        spins[0].setFocus()
        wait(20)
        fieldFocus = viewport._textFocus()
        _qtTest().QTest.keyClick(spins[0], QtCore.Qt.Key.Key_Escape)
        wait(10)
        check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) ==
              tipOnly,
              "SL-03: Escape in a focused dock spin box leaves the selection"
              " unchanged (field held the keys: %s)" % fieldFocus)
        view.setFocus()
        wait(20)
    check(not viewport._textFocus(),
          "SL-03: the keys are back with the viewport")

    # Ctrl+A takes every visible center CV of this sub-mode's kind, Ctrl+I
    # flips them, Ctrl+Shift+A drops them all.
    selectKey("a", ("ctrl",))
    check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) == allCVs,
          "SL-03: Ctrl+A selects every visible center CV %r"
          % (session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV),))
    check(not session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT),
          "SL-03: and no whole tube with them")
    # Deselect first: with all five selected the gizmo's centre handle sits
    # over the end-on tip, and a click there is the gizmo's (GZ-03).
    selectKey("a", ("ctrl", "shift"))
    mouse.click(tipPixel)
    check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) == tipOnly,
          "SL-03: a click re-selects just the tip before the invert")
    selectKey("i", ("ctrl",))
    check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) ==
          allCVs[:-1],
          "SL-03: Ctrl+I inverts the tip into the other center CVs %r"
          % (session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV),))
    selectKey("a", ("ctrl", "shift"))
    check(not session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV),
          "SL-03: Ctrl+Shift+A clears the selection before the marquee")

    # Shift always reserves a drag for the selection band, including when
    # the pointer crosses a tube.  Observe the actual QRubberBand mid-drag,
    # then require release to select the center candidates inside it.
    start = (tipPixel[0] - 90.0, tipPixel[1] - 90.0)
    finish = (tipPixel[0] + 90.0, tipPixel[1] + 90.0)
    mouse.press(start, ("shift",))
    mouse.move(finish, ("shift",))
    wait(10)
    check(viewport._marqueeOverlay.isVisible(),
          "a real Shift drag shows the mouse-transparent selection band")
    check(getattr(viewport._marqueeOverlay, "colourRecord", None) == "add",
          "FB-02: the Shift band is tinted 'add' (%r)"
          % (getattr(viewport._marqueeOverlay, "colourRecord", None),))
    # Ctrl joins mid-drag: the next move re-tints the same band 'remove'.
    mouse.move((finish[0] - 2.0, finish[1] - 2.0), ("ctrl",))
    wait(10)
    check(getattr(viewport._marqueeOverlay, "colourRecord", None) ==
          "remove",
          "FB-02: holding Ctrl re-tints the live band 'remove' (%r)"
          % (getattr(viewport._marqueeOverlay, "colourRecord", None),))
    from pxr.Usdviewq.qt import QtCore
    check(view.cursor().shape() == QtCore.Qt.CursorShape.CrossCursor,
          "FB-02: a live band shows the cross cursor (%r)"
          % (view.cursor().shape(),))
    mouse.move(finish, ("shift",))
    wait(10)
    mouse.release(finish, ("shift",))
    marqueeSelection = session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV)
    expected = [(0, index, -1) for index in range(STUB_CVS)]
    check(marqueeSelection == expected,
          "the real marquee selects every visible root center candidate %r"
          % (marqueeSelection,))
    check(not session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) and
          not session.readSelection(pomadeLib.POMADE_PICK_SECTION_RING),
          "the component marquee drops stale whole-tube and ring selection")
    check(not viewport._marqueeOverlay.isVisible(),
          "releasing the marquee removes its transient Qt overlay")

    # The explicit Lasso shape is a plain drag as well as Shift-add.  The
    # physical-pixel polygon below encloses the same displayed CVs as the
    # box, while the translucent child remains mouse-transparent to the
    # StageView event filter.
    selectKey("a", ("ctrl", "shift"))
    state.selectionShape = "lasso"
    lasso = [(tipPixel[0] - 90.0, tipPixel[1] - 90.0),
             (tipPixel[0] + 90.0, tipPixel[1] - 90.0),
             (tipPixel[0] + 90.0, tipPixel[1] + 90.0),
             (tipPixel[0] - 90.0, tipPixel[1] + 90.0)]
    mouse.press(lasso[0])
    for point in lasso[1:]:
        mouse.move(point)
    wait(10)
    check(viewport._lassoOverlay.isVisible(),
          "a real Lasso drag paints its mouse-transparent polygon")
    mouse.release(lasso[0])
    lassoSelection = session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV)
    check(lassoSelection == expected,
          "the real Lasso selects visible center CVs through SelectPolygon %r"
          % (lassoSelection,))
    check(not session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) and
          not session.readSelection(pomadeLib.POMADE_PICK_SECTION_RING),
          "the component Lasso never silently adds a whole tube or ring")
    check(not viewport._lassoOverlay.isVisible(),
          "releasing the Lasso removes its transient Qt overlay")
    state.selectionShape = "box"

    # Now exercise the exact same Qt routes against a child tube's ring and
    # section CV.  This is deliberately after the root proof: the flattened
    # K11 candidate stream has to preserve the child owner, rather than
    # quietly spelling every section item as tube 0.
    # Select and subdivide the visible root through the Hierarchy dock.  The
    # UI action expands the active-cut branch; native child creation followed
    # by a global level change leaves this branch hidden from Tube picking.
    workspace.button("mode", "hierarchy").click()
    wait(15)
    rootPoint = sectionCV(session, 0, 1, 0)
    rootPixel = pixel(rootPoint) if rootPoint is not None else None
    if rootPixel is not None:
        mouse.click(rootPixel)
    rootSelection = session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT)
    check(rootSelection == [(0, -1, -1)],
          "the visible root is selected before child subdivision %r"
          % (rootSelection,))
    didSubdivide = (rootSelection == [(0, -1, -1)] and
                    clickControl("subdivide"))
    children = ([tube for tube in pomadeBridge.readTubeIds(
        session.dll, session.model) if tube != 0] if didSubdivide else [])
    check(bool(children), "the root subdivides before child section picking")
    if children:
        entered = clickControl("enterLevel")
        check(entered and int(state.activeLevel) >= 2,
              "the Hierarchy dock enters the created child level")
        childPixels = []
        for tube in children[:2]:
            point = sectionCV(session, int(tube), 1, 0)
            childPixels.append(pixel(point) if point is not None else None)
        checkHierarchyModifiers(session, mouse, childPixels)
        child = int(children[0])
        ring = 1
        try:
            ringPoint = sectionRingCenter(session, child, ring)
            cvPoint = sectionCV(session, child, ring, 0)
        except (AttributeError, RuntimeError, TypeError, ValueError) as exc:
            ringPoint = cvPoint = None
            check(False, "the child section geometry reads through the ABI: %s"
                  % exc)
        workspace.button("mode", "tube").click()
        wait(15)
        if workspace.button("comp", "ring") is not None:
            workspace.button("comp", "ring").click()
            wait(10)
        else:
            viewport.setMode("tube")
            viewport.setSubMode("ring")
        session.clearSelection()
        if ringPoint is not None and cvPoint is not None:
            # Ring mode displays section vertices, while the native generic
            # ring candidate lives at the centroid.  The Tube resolver maps
            # this real displayed vertex back to its ring for hover/click.
            ringVertexPixel = pixel(cvPoint)
            mouse.move(ringVertexPixel)
            wait(15)
            check(hover(session) == (pomadeLib.POMADE_PICK_SECTION_RING,
                                     child, ring, -1),
                  "a displayed child section vertex prehighlights its owning ring")
            mouse.click(ringVertexPixel)
            ringSelection = session.readSelection(pomadeLib.POMADE_PICK_SECTION_RING)
            check(ringSelection == [(child, ring, -1)],
                  "a real child-ring vertex click preserves its tube and ring %r"
                  % (ringSelection,))
        else:
            check(False, "a child ring has projectable center and vertex controls")

        if workspace.button("comp", "section") is not None:
            workspace.button("comp", "section").click()
            wait(10)
        else:
            viewport.setSubMode("section")
        session.clearSelection()
        if cvPoint is not None:
            cvPixel = pixel(cvPoint)
            mouse.click(cvPixel)
            cvSelection = session.readSelection(pomadeLib.POMADE_PICK_SECTION_CV)
            check(cvSelection == [(child, ring, 0)],
                  "a real child section-CV click preserves tube/ring/slot %r"
                  % (cvSelection,))
            session.clearSelection()
            start = (cvPixel[0] - 3.0, cvPixel[1] - 3.0)
            finish = (cvPixel[0] + 3.0, cvPixel[1] + 3.0)
            mouse.drag([start, finish], ("shift",))
            marquee = session.readSelection(pomadeLib.POMADE_PICK_SECTION_CV)
            check((child, ring, 0) in marquee,
                  "a real child-CV marquee retains its tube/ring/slot %r"
                  % (marquee,))
            checkComponentConversion(view, session, viewport, state, mouse,
                                     cvPixel, child, ring,
                                     [int(tube) for tube in children[1:]])
        else:
            check(False, "a child section CV has a projectable pick center")

    hygieneTarget = None
    if children and cvPoint is not None:
        hygieneTarget = (pixel(cvPoint), int(children[0]), 1)
    navigationContext = {
        "view": view, "stage": stage, "dataModel": dataModel,
        "workspace": workspace, "session": session, "viewport": viewport,
        "state": state, "mouse": mouse, "target": hygieneTarget}
    checkInputHygiene(appController, navigationContext)
    checkNavigation(appController, navigationContext)

    capturePath = os.environ.get("USDGEN_POMADE_SELECTION_CAPTURE")
    if capturePath:
        window = getattr(appController, "_mainWindow", None)
        shot = window.grab() if window is not None else view.grab()
        check(shot.save(capturePath, "PNG"),
              "the optional selection acceptance screenshot was written")

    if rolloverBefore is not None:
        workspace.hide()
        wait(10)
        check(bool(view.rolloverPicking) == rolloverBefore,
              "hiding the workspace restores StageView rollover picking")
        workspace.show()

    shutdown()
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeSelection needs testusdview")
    sys.exit(0)
