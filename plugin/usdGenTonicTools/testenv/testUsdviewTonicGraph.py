# testUsdviewTonicGraph -- T3: the Graph tool driven by REAL mouse events
# (plan/18 V2 exit).
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicGraph.py \
#               examples/tonic-graph-scalp.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the other T3 tonic scripts
# use, plus the staged usdGenTonicTools package and the usdGenTonic DLL
# (via USDGENTONIC_DLL or the build tree).
#
# The script that used to live here called Tonic_GraphStroke through ctypes
# and never opened the tool; it is now testTonicAbiGraph.py, which is what
# it always was. THIS script opens the tool the way the artist does -- the
# usdGen -> Tonic menu commands, registered in usdview's own plugin
# registry -- and then presses, drags and releases the mouse over the
# StageView with QtTest, types the hotkeys, and asserts on what came out.
#
# Proven here:
#   * "Bind scalp (selection)" activates a session and installs the
#     viewport controller on the real StageView (plan/18 finding F3: the
#     P2 controller read usdviewApi.stageView, which does not exist, so
#     its filter was never installed at all);
#   * a press-drag-release over the scalp strokes a closed region: the
#     model gains four nodes and one region, and the frame gains the
#     region's clump colour where the rectangle is, which is the scene
#     index publishing graphRegions;
#   * Escape during a live stroke cancels it and authors nothing;
#   * a Weld click pair merges two nodes, and Ctrl+Z puts the node back;
#   * after the idle pump the committed groom -- ScalpGraph and all -- is
#     on the stage, having never been touched during a gesture.
#
# Reading the published geometry: this USD build exposes no terminal scene
# index to Python (UsdImagingGL.Engine has no GetTerminalSceneIndex), and
# the Tonic prims have no USD origin for a pick to name, so the only way to
# see what Hydra got is the framebuffer -- the same technique
# testUsdviewTonicPublish.py uses, and the reason plan/18 section 3.2
# routes tool picking through K11 instead of view.pick().
import os
import sys

failures = 0

# The scalp is the 4x4 quad grid in XZ at y = 0; the stroke is the
# x in [1, 3], z in [1, 3] square, which covers the four middle faces.
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


# ---------------------------------------------------------------------------
# Driving the real widget
# ---------------------------------------------------------------------------

class Mouse:
    """QtTest mouse events on the StageView, in its own pixel space.

    The controller reads physical pixels (event position times the device
    pixel ratio, the same scaling StageView's own handlers do), while
    QtTest takes logical widget coordinates, so everything below converts
    once, here.
    """

    def __init__(self, view):
        from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
        self._view = view
        self._QtCore = QtCore
        self._QtGui = QtGui
        self._QtWidgets = QtWidgets
        try:
            self._ratio = float(view.devicePixelRatioF())
        except AttributeError:
            self._ratio = 1.0
        self.direct = False          # set when QtTest delivery does not land

    def _point(self, physical):
        return self._QtCore.QPoint(int(round(physical[0] / self._ratio)),
                                   int(round(physical[1] / self._ratio)))

    def _modifiers(self, names):
        mods = self._QtCore.Qt.KeyboardModifier.NoModifier
        table = {"shift": self._QtCore.Qt.KeyboardModifier.ShiftModifier,
                 "ctrl": self._QtCore.Qt.KeyboardModifier.ControlModifier}
        for name in names:
            mods |= table[name]
        return mods

    def _send(self, kind, physical, mods, button):
        """One synthetic event straight at the widget.

        The fallback path: on a window the test harness never showed,
        QtTest's global-coordinate round trip can miss the child widget
        entirely, and a missed press would look like a tool bug. The event
        is a real QMouseEvent through the real filter either way.
        """
        QtCore = self._QtCore
        point = self._point(physical)
        # QPointF is QtCore's, in PySide2 and PySide6 alike: this path
        # had never run, because the graph stroke's press lands through
        # QtTest, and it raised an AttributeError the first time a hover
        # needed it (a hover has no grab, so QtTest can miss the widget).
        local = self._QtCore.QPointF(point)
        globalPos = self._QtCore.QPointF(self._view.mapToGlobal(point))
        event = self._QtGui.QMouseEvent(kind, local, globalPos, button,
                                        button, mods)
        self._QtWidgets.QApplication.sendEvent(self._view, event)

    def press(self, physical, modifiers=()):
        from pxr.Usdviewq.qt import QtCore
        QtTest = _qtTest()
        button = QtCore.Qt.MouseButton.LeftButton
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonPress, physical, mods,
                       button)
        else:
            QtTest.QTest.mousePress(self._view, button, mods,
                                    self._point(physical))

    def move(self, physical, modifiers=()):
        from pxr.Usdviewq.qt import QtCore
        QtTest = _qtTest()
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseMove, physical, mods,
                       QtCore.Qt.MouseButton.LeftButton)
        else:
            QtTest.QTest.mouseMove(self._view, self._point(physical))

    def release(self, physical, modifiers=()):
        from pxr.Usdviewq.qt import QtCore
        QtTest = _qtTest()
        button = QtCore.Qt.MouseButton.LeftButton
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonRelease, physical, mods,
                       button)
        else:
            QtTest.QTest.mouseRelease(self._view, button, mods,
                                      self._point(physical))

    def click(self, physical, modifiers=()):
        self.press(physical, modifiers)
        self.release(physical, modifiers)

    def drag(self, points, modifiers=()):
        self.press(points[0], modifiers)
        for point in points[1:]:
            self.move(point, modifiers)
        self.release(points[-1], modifiers)


def _qtTest():
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    return importlib.import_module("%s.QtTest" % PySideModule)


def typeKey(view, name, modifiers=()):
    """One key press through QtTest, seen by the app-level filter."""
    from pxr.Usdviewq.qt import QtCore
    QtTest = _qtTest()
    keys = {"escape": QtCore.Qt.Key.Key_Escape,
            "z": QtCore.Qt.Key.Key_Z,
            "y": QtCore.Qt.Key.Key_Y,
            "1": QtCore.Qt.Key.Key_1,
            "2": QtCore.Qt.Key.Key_2,
            "d": QtCore.Qt.Key.Key_D,
            "w": QtCore.Qt.Key.Key_W,
            "p": QtCore.Qt.Key.Key_P,
            "delete": QtCore.Qt.Key.Key_Delete,
            "]": QtCore.Qt.Key.Key_BracketRight}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    QtTest.QTest.keyClick(view, keys[name], mods)


def wait(ms=30):
    QtTest = _qtTest()
    QtTest.QTest.qWait(int(ms))


# ---------------------------------------------------------------------------
# Camera and pixels
# ---------------------------------------------------------------------------

def frameScalp(stage, view):
    """A camera looking straight down at the 4x4 scalp, and activate it."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicGraphCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    # High enough that the default 35 mm / 15.29 mm aperture (a 24.6-degree
    # vertical field) still shows the whole 4 x 4 scalp after usdview
    # conforms the frustum to the window.
    eye = Gf.Vec3d(2.0, 12.0, 2.0)
    # Camera-to-world by hand: looking down -Y, with +Z up the screen.
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
        "/TonicGraphCamera")
    return view.getActiveSceneCamera() is not None


def scalpFraction(view, x, z, half=0.25, steps=12):
    """Fraction of a small scalp patch carrying region colour 0 (#2F6BFF).

    The region tint uses the same palette as the tubes rooted in it
    (plan/18 section 2.4a), so region 0 is a strongly blue-dominant patch
    against the grey scalp and the grey backdrop.
    """
    from usdGenTonicTools import tonicCamera
    view.update()
    view.repaint()
    view.updateGL()
    camera = tonicCamera.resolve(view)
    image = view.grabFrameBuffer()
    width, height = image.width(), image.height()
    blue = 0
    total = 0
    for iz in range(steps):
        for ix in range(steps):
            wx = x - half + 2.0 * half * (ix + 0.5) / steps
            wz = z - half + 2.0 * half * (iz + 0.5) / steps
            projected = camera.worldToPixels((wx, 0.0, wz))
            if projected is None:
                continue
            px = int(min(max(projected[0], 0), width - 1))
            py = int(min(max(projected[1], 0), height - 1))
            rgb = image.pixel(px, py)
            r = (rgb >> 16) & 0xFF
            g = (rgb >> 8) & 0xFF
            b = rgb & 0xFF
            total += 1
            if b > r + 40 and b > g + 40:
                blue += 1
    return float(blue) / float(total) if total else 0.0


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

def graphCounts(session):
    return session.graphCounts()


def pumpUntilCommitted(viewport, session, tries=60):
    """Idle-pump until the committer has nothing left, or give up."""
    for _ in range(tries):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            return True
        wait(25)
    return False


def run(appController):
    global failures
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if here:
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import tonicCamera, tonicModes, tonicPanels
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    check(view is not None, "usdview built a StageView")
    check(registry is not None, "the plugin registry is reachable")
    if view is None or registry is None:
        return 1

    container = usdGenTonicTools.container()
    check(container is not None, "the Tonic plugin container registered")
    if container is None:
        return 1

    # -- the menu, exactly five commands -----------------------------------
    names = [name for name, _l, _d in container.COMMANDS]
    check(names == ["openWorkspace", "bindScalp", "saveGroom",
                    "exportCenterCurves", "importCurves"],
          "usdGen -> Tonic carries the five plan/18 commands (%r)" % (names,))
    for name in names:
        check(registry.getCommandPlugin("usdGenTonicTools.%s" % name)
              is not None, "%s is registered with usdview" % name)

    check(frameScalp(stage, view), "the scene camera looks down at the scalp")
    view.setFocus()
    wait(50)

    # -- open the tool the way the artist does -----------------------------
    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    check(container.workspace is not None,
          "Open workspace built the dock and stored it on the container")
    check(container.tonicState.workspaceOpen,
          "the dock reports itself open, which is what arms the hotkeys")
    # Route the tool's status lines here: usdview sends them to a widget,
    # and a failure below is only readable if its reason came with it.
    messages = []
    if container.session is not None:
        container.session.setStatusSink(messages.append)
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    check(session is not None and session.model is not None,
          "Bind scalp created a live model (status: %r)" % (messages[-3:],))
    check(viewport is not None and viewport.installed,
          "the viewport controller installed itself on the StageView")
    if session is None or viewport is None or session.model is None:
        return 1
    check(viewport.view is view,
          "and on the STAGE VIEW, not on some other widget")
    check(container.tonicState.activeMode == "graph" and
          container.tonicState.graphSubMode == "draw",
          "the tool opens in Graph/Draw (%r/%r)"
          % (container.tonicState.activeMode,
             container.tonicState.graphSubMode))
    check(session.publish() >= 1,
          "the model publishes to at least one scene index")

    # The region tint mesh the index publishes is COINCIDENT with /Scalp
    # (same points, same topology), so leaving both on would z-fight and
    # the pixel probe below would read whichever won. The model copied the
    # scalp at bind time, so the mouse still hits it through K1; what goes
    # away is only the stage's own draw of the same surface.
    stage.GetPrimAtPath("/Scalp").SetActive(False)

    camera = tonicCamera.resolve(view)
    check(camera is not None and camera.width > 200,
          "the controller's camera resolves (%dx%d)"
          % (camera.width if camera else 0, camera.height if camera else 0))
    if camera is None:
        return 1

    def pixel(x, z):
        projected = camera.worldToPixels((x, 0.0, z))
        return (projected[0], projected[1])

    perPixel = camera.worldPerPixel((2.0, 0.0, 2.0))
    info("world per pixel at the scalp: %.5f" % perPixel)
    # Aim the panel's snap radius at 0.1 rest units, which is the value the
    # ABI-level regression uses, so both scripts weld on the same distance.
    container.tonicState.snapRadiusPx = max(0.1 / max(perPixel, 1e-9), 2.0)
    info("snap radius: %.1f px" % container.tonicState.snapRadiusPx)

    before = scalpFraction(view, *CENTRE)
    info("region colour over the square before the stroke: %.2f" % before)
    check(before < 0.2, "the scalp starts with no region there")

    # -- a real press-drag-release ----------------------------------------
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
        # QtTest's delivery did not reach the child widget on this window;
        # go straight at it instead. Still a real QMouseEvent, still
        # through the installed filter.
        info("QtTest press did not land; using direct QMouseEvent delivery")
        mouse.direct = True
        mouse.press(path[0])
    check(viewport.gestureActive,
          "the press opened a gesture in the controller")
    for point in path[1:]:
        mouse.move(point)
    mouse.release(path[-1])
    check(not viewport.gestureActive, "the release closed it")

    nodes, edges, regions = graphCounts(session)
    check((nodes, edges, regions) == (4, 4, 1),
          "the drag stroked one closed 4-node region (got %d/%d/%d)"
          % (nodes, edges, regions))
    _r, uncovered, intersected = session.regionStats()
    check(uncovered == 12 and intersected == 0,
          "it claims the four middle faces (12 uncovered, got %d)"
          % uncovered)

    after = scalpFraction(view, *CENTRE)
    info("region colour over the square after the stroke: %.2f" % after)
    check(after > 0.5,
          "Hydra draws the stroked region in its clump colour: %.2f -> %.2f"
          % (before, after))
    # The scalp tint is what this check is about, so the L1 tube the stroke
    # just seeded is hidden for the measurement: since V6 its root section
    # is fitted to the region boundary (plan/18 section 7 G12), so the tube
    # is wide enough to project over a patch of scalp well outside its own
    # region and would read as tint that is not there.
    session.dll.Tonic_SetLevelDisplay(session.model, 1, 0, 0)
    session.publishAll()
    outside = scalpFraction(view, 0.5, 0.5)
    session.dll.Tonic_SetLevelDisplay(session.model, 1, 1, 0)
    session.publishAll()
    info("region colour outside the square: %.2f" % outside)
    check(outside < 0.2, "and only inside it (%.2f)" % outside)

    # -- Escape cancels a live stroke --------------------------------------
    mouse.press(pixel(0.3, 0.3))
    mouse.move(pixel(0.7, 0.3))
    mouse.move(pixel(0.7, 0.7))
    check(viewport.gestureActive, "a second stroke is live")
    typeKey(view, "escape")
    check(not viewport.gestureActive, "Escape closed the gesture")
    mouse.release(pixel(0.7, 0.7))
    check(graphCounts(session) == (4, 4, 1),
          "the cancelled stroke authored nothing (got %r)"
          % (graphCounts(session),))

    # -- Place a node, then Weld it onto a corner --------------------------
    typeKey(view, "p")
    check(container.tonicState.graphSubMode == "place",
          "the P hotkey switched to the Place sub-mode (%r)"
          % container.tonicState.graphSubMode)
    mouse.click(pixel(*CENTRE))
    nodes, edges, regions = graphCounts(session)
    check(nodes == 5, "Place added a fifth node (got %d)" % nodes)

    typeKey(view, "w")
    check(container.tonicState.graphSubMode == "weld",
          "the W hotkey switched to the Weld sub-mode (%r)"
          % container.tonicState.graphSubMode)
    # Corner first: Tonic_GraphWeld keeps the first node and drops the
    # second, so the region's own corner survives and the loose centre node
    # is the one absorbed.
    mouse.click(pixel(*RECT[0]))
    mouse.click(pixel(*CENTRE))
    weldedNodes, _e, _r = graphCounts(session)
    check(weldedNodes == 4,
          "the weld merged the pair back to four nodes (got %d)"
          % weldedNodes)

    # -- Ctrl+Z ------------------------------------------------------------
    typeKey(view, "z", ("ctrl",))
    undoneNodes, _e, _r = graphCounts(session)
    check(undoneNodes == 5,
          "Ctrl+Z put the welded node back (got %d)" % undoneNodes)
    typeKey(view, "y", ("ctrl",))
    redoneNodes, _e, _r = graphCounts(session)
    check(redoneNodes == 4,
          "Ctrl+Y welds it again (got %d)" % redoneNodes)

    # -- mode keys ---------------------------------------------------------
    typeKey(view, "2")
    check(container.tonicState.activeMode == "tube",
          "the 2 hotkey selects Tube mode (%r)"
          % container.tonicState.activeMode)
    check(viewport.loop is not None and viewport.loop.modeId == "tube",
          "and the controller swapped in Tube's own loop (V4)")
    # The dock did not make this switch, so it has to notice it: the
    # shelf button, the sub-mode shelf and the generated parameter rows
    # all belong to the mode the artist is actually in.
    workspace = container.workspace
    workspace.refresh()
    tubeRows = [d.id for d in tonicPanels.descriptors("tube",
                                                      container.tonicState)]
    check(workspace.activeMode == "tube",
          "the dock followed the hotkey into Tube (%r)"
          % workspace.activeMode)
    check(sorted(workspace.parameterIds()) == sorted(tubeRows),
          "and rebuilt Tube's parameter rows (%r)"
          % (workspace.parameterIds(),))
    check(sorted(workspace.subModeIds()) ==
          sorted(m.id for m in tonicModes.TUBE_SUBMODES),
          "and Tube's sub-mode shelf (%r)" % (workspace.subModeIds(),))
    typeKey(view, "1")
    check(container.tonicState.activeMode == "graph",
          "and 1 comes back to Graph")
    workspace.refresh()
    check(workspace.activeMode == "graph" and
          sorted(workspace.subModeIds()) ==
          sorted(m.id for m in tonicModes.GRAPH_SUBMODES),
          "and the dock comes back with it (%r, %r)"
          % (workspace.activeMode, workspace.subModeIds()))

    # -- the idle pump puts it on the stage --------------------------------
    #
    # A groom with no tube has nothing to commit -- TonicBuildCommitLayer
    # refuses a snapshot that holds no tubes -- and Tube mode's loop is V4,
    # so the tube for the stroked region is built through the ABI here.
    # Everything around it (the graph, the enqueue, the swap) is the
    # controller's own work.
    import ctypes
    check(session.dll.Tonic_BuildTubeFromRegion(
        session.model, 0, 5, 8, ctypes.c_float(3.0)) == 0,
        "a tube builds from the stroked region")
    check(session.enqueueCommit(), "a commit enqueues")
    check(pumpUntilCommitted(viewport, session),
          "the idle pump drains the committer")
    groom = stage.GetPrimAtPath("/TonicGroom")
    check(bool(groom), "the committed groom reached the stage")
    graphPrim = stage.GetPrimAtPath("/TonicGroom/ScalpGraph")
    check(bool(graphPrim), "with its ScalpGraph")
    if graphPrim:
        faceIds = graphPrim.GetAttribute("usdGen:tonic:nodeFaceIds").Get()
        check(faceIds is not None and len(faceIds) == 4,
              "and four committed nodes (got %r)"
              % (None if faceIds is None else len(faceIds),))
    check(bool(stage.GetPrimAtPath("/TonicGroom/Guides")),
          "and the committed guides")
    status = session.status()
    info("status: model v%d committed v%d swap %.2f ms, last move %.2f ms"
         % (status["modelVersion"], status["committedVersion"],
            status["lastSwapMs"], status["lastMoveMs"]))
    check(status["committedVersion"] > 0,
          "the committer reports a committed version")
    check(status["lastMoveMs"] > 0.0,
          "the controller timed its moves with the dock refreshing on "
          "every publish (%.3f ms, budget 8)" % status["lastMoveMs"])
    check(status["lastMoveMs"] < 8.0,
          "which leaves the TN-1 move budget intact")

    # The workspace reads these off the session as numbers, by these
    # names, through the status dict or straight off the object.
    for name in ("modelVersion", "committedVersion", "pendingVersion",
                 "lastSwapMs", "committerDetached"):
        value = getattr(session, name, None)
        check(name in status and value is not None and not callable(value),
              "the dock can read %s as a value (status %r, attribute %r)"
              % (name, status.get(name), value))

    # -- V6 / G14: a second region gets its own L1 tube --------------------
    #
    # plan/17 section 5.1: "each closed region gets a tube stub". Until V6
    # the model held ONE L1 tube, so the second region an artist drew was
    # unrepresentable -- and silently so. This strokes a second closed
    # region in the corner of the scalp (far enough from the first that
    # nothing welds), and asserts the whole chain through the real
    # controller: two roots in the model, two tubes in the published
    # /__usdGenTonic/tubes/L1, and two tube prims committed to the stage.
    typeKey(view, "d")
    check(container.tonicState.graphSubMode == "draw",
          "D puts Graph back in the Draw sub-mode (%r)"
          % container.tonicState.graphSubMode)
    SECOND = ((0.1, 0.1), (0.8, 0.1), (0.8, 0.8), (0.1, 0.8))
    secondPath = []
    for k in range(len(SECOND)):
        x0, z0 = SECOND[k]
        x1, z1 = SECOND[(k + 1) % len(SECOND)]
        for i in range(5):
            t = float(i) / 5.0
            secondPath.append(pixel(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t))
    secondPath.append(pixel(*SECOND[0]))
    mouse.press(secondPath[0])
    check(viewport.gestureActive, "the second region's press opens a gesture")
    for point in secondPath[1:]:
        mouse.move(point)
    mouse.release(secondPath[-1])
    check(not viewport.gestureActive, "and the release closes it")

    nodes2, edges2, regions2 = graphCounts(session)
    check((nodes2, edges2, regions2) == (8, 8, 2),
          "the scalp now carries two closed regions (got %d/%d/%d)"
          % (nodes2, edges2, regions2))

    ids = (ctypes.c_int * 16)()
    count = ctypes.c_int(0)
    rc = session.dll.Tonic_ReadL1TubeIds(session.model, ids, 16,
                                         ctypes.byref(count))
    roots = [ids[i] for i in range(count.value)] if rc == 0 else []
    check(len(roots) == 2 and len(set(roots)) == 2,
          "the release gave each region its own L1 root (%r)" % (roots,))
    check(0 in roots, "the first root is still tube 0 (%r)" % (roots,))

    session.publishAll()
    faces = ctypes.c_int(0)
    points = ctypes.c_int(0)
    tubes = ctypes.c_int(0)
    rc = session.dll.Tonic_GetPublishedLevelInfo(
        session.model, 1, ctypes.byref(faces), ctypes.byref(points),
        ctypes.byref(tubes))
    check(rc == 0 and tubes.value == 2,
          "/__usdGenTonic/tubes/L1 publishes two tubeId values (got %d)"
          % tubes.value)
    info("tubes/L1 after the second region: %d faces / %d points / %d tubes"
         % (faces.value, points.value, tubes.value))

    check(session.enqueueCommit(), "the two-root groom enqueues")
    check(pumpUntilCommitted(viewport, session),
          "and the idle pump drains it")
    committed = []
    tubesPrim = stage.GetPrimAtPath("/TonicGroom/Tubes")
    if tubesPrim:
        committed = [child.GetName() for child in tubesPrim.GetChildren()]
    check(len(committed) == 2,
          "both L1 tubes committed to the stage (%r)" % (committed,))
    regionIds = []
    for name in committed:
        prim = stage.GetPrimAtPath("/TonicGroom/Tubes/" + name)
        attr = prim.GetAttribute("usdGen:tonic:regionId")
        regionIds.append(attr.Get() if attr else None)
    check(len(set(regionIds)) == 2 and None not in regionIds,
          "with distinct channel-0 region ids (%r)" % (regionIds,))

    # -- the hotkeys belong to the workspace -------------------------------
    container.tonicState.workspaceOpen = False
    typeKey(view, "2")
    check(container.tonicState.activeMode == "graph",
          "with the workspace closed the number keys go back to usdview "
          "(mode is %r)" % container.tonicState.activeMode)
    container.tonicState.workspaceOpen = True
    typeKey(view, "2")
    check(container.tonicState.activeMode == "tube",
          "and come back when it reopens (%r)"
          % container.tonicState.activeMode)
    typeKey(view, "1")

    # -- tear down cleanly -------------------------------------------------
    viewport.uninstall()
    session.deactivate()
    check(not container.tonicState.activated,
          "deactivating the session clears the tool state")

    print("testUsdviewTonicGraph: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicGraph needs testusdview (no live view)")
    sys.exit(0)
