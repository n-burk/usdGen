# testUsdviewTonicTube -- T3: Tube and Fill driven by REAL mouse events
# (plan/18 V4 exit).
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicTube.py \
#               examples/tonic-graph-scalp.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the other T3 tonic scripts
# use, plus the staged usdGenTonicTools package and the usdGenTonic DLL
# (via USDGENTONIC_DLL or the build tree).
#
# The ABI-level script beside this one (testTonicAbiTube.py) calls the tube
# entry points directly and never opens the tool. THIS script opens the
# tool the way the artist does and then presses, drags and releases the
# mouse over the StageView with QtTest.
#
# Proven here:
#   * G14: a stroke that closes a region leaves that region with a TUBE,
#     built by the session on the graph release -- so a groom that is only
#     a graph still commits;
#   * Tube mode selects a center CV with one K11 pick and raises a
#     translate gizmo on it, and the published CV dot turns WHITE where it
#     was the clump colour;
#   * dragging that gizmo moves the published center: the white dot is
#     gone from where it was and present where the cursor left it, which
#     is /__usdGenTonic/centers/L1 and centerCVs/L1 arriving at Hydra;
#   * the whole drag is ONE undo step: Ctrl+Z puts the CV back;
#   * TN-1 re-measured through the real controller: 50 real mouse moves,
#     each timed end to end by the controller (model edit + publish +
#     updateGL request);
#   * Fill: the panel's density descriptor writes the selection's fill
#     params and the guide count follows.
#
# Reading the published geometry: this USD build exposes no terminal scene
# index to Python, and the Tonic prims have no USD origin for a pick to
# name, so the only way to see what Hydra got is the framebuffer -- the
# technique testUsdviewTonicGraph.py and testUsdviewTonicPublish.py use.
# The camera looks straight DOWN the tube: the tube has no end cap, so the
# center CVs are visible through the open end and a probe on the axis
# reads the CV dot rather than the tube wall.
import ctypes
import os
import sys

failures = 0

# The scalp is the 4x4 quad grid in XZ at y = 0; the stroke is the
# x in [1, 3], z in [1, 3] square, which covers the four middle faces.
RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
CENTRE = (2.0, 2.0)
# tonicTube.DEFAULT_RINGS / DEFAULT_LENGTH: what the G14 stub builds with.
STUB_CVS = 5
STUB_LENGTH = 4.0


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

def _qtTest():
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    return importlib.import_module("%s.QtTest" % PySideModule)


class Mouse:
    """QtTest mouse events on the StageView, in its own pixel space."""

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

        The fallback path, for a window the harness never showed: QtTest's
        global-coordinate round trip can miss the child widget entirely,
        and a missed press would look like a tool bug. QPointF is
        QtCore's, in PySide2 and PySide6 alike.
        """
        point = self._point(physical)
        local = self._QtCore.QPointF(point)
        globalPos = self._QtCore.QPointF(self._view.mapToGlobal(point))
        event = self._QtGui.QMouseEvent(kind, local, globalPos, button,
                                        button, mods)
        self._QtWidgets.QApplication.sendEvent(self._view, event)

    def press(self, physical, modifiers=()):
        from pxr.Usdviewq.qt import QtCore
        button = QtCore.Qt.MouseButton.LeftButton
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonPress, physical, mods,
                       button)
        else:
            _qtTest().QTest.mousePress(self._view, button, mods,
                                       self._point(physical))

    def move(self, physical, modifiers=()):
        from pxr.Usdviewq.qt import QtCore
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseMove, physical, mods,
                       QtCore.Qt.MouseButton.LeftButton)
        else:
            _qtTest().QTest.mouseMove(self._view, self._point(physical))

    def release(self, physical, modifiers=()):
        from pxr.Usdviewq.qt import QtCore
        button = QtCore.Qt.MouseButton.LeftButton
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonRelease, physical, mods,
                       button)
        else:
            _qtTest().QTest.mouseRelease(self._view, button, mods,
                                         self._point(physical))

    def click(self, physical, modifiers=()):
        self.press(physical, modifiers)
        self.release(physical, modifiers)

    def drag(self, points, modifiers=()):
        self.press(points[0], modifiers)
        for point in points[1:]:
            self.move(point, modifiers)
        self.release(points[-1], modifiers)


def typeKey(view, name, modifiers=()):
    """One key press through QtTest, seen by the app-level filter."""
    from pxr.Usdviewq.qt import QtCore
    keys = {"escape": QtCore.Qt.Key.Key_Escape,
            "z": QtCore.Qt.Key.Key_Z,
            "y": QtCore.Qt.Key.Key_Y,
            "1": QtCore.Qt.Key.Key_1,
            "2": QtCore.Qt.Key.Key_2,
            "3": QtCore.Qt.Key.Key_3,
            "c": QtCore.Qt.Key.Key_C,
            "r": QtCore.Qt.Key.Key_R,
            "delete": QtCore.Qt.Key.Key_Delete}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    _qtTest().QTest.keyClick(view, keys[name], mods)


def wait(ms=30):
    _qtTest().QTest.qWait(int(ms))


# ---------------------------------------------------------------------------
# Camera and pixels
# ---------------------------------------------------------------------------

def frameScalp(stage, view):
    """A camera looking straight down at the 4x4 scalp, and activate it.

    Straight down is also straight along the tube the stub builds (the
    region's mean normal is +Y), which is what makes the center CVs
    readable: the tube carries no end cap, so the axis is open to the sky.
    """
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicTubeCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(2.0, 12.0, 2.0)
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
        "/TonicTubeCamera")
    return view.getActiveSceneCamera() is not None


def frame(view):
    """Redraw and hand back the framebuffer image."""
    view.update()
    view.repaint()
    view.updateGL()
    return view.grabFrameBuffer()


def whiteFraction(view, camera, point, halfPx=6):
    """Fraction of near-white pixels in a small window around `point`.

    A selected CV dot publishes as white (plan/18 section 2.4a); every
    other thing in this frame -- the clump colour, the region tint, the
    grey backdrop -- is either coloured or dark, so "white" is the one
    unambiguous reading.
    """
    image = frame(view)
    projected = camera.worldToPixels(point)
    if projected is None:
        return 0.0
    width, height = image.width(), image.height()
    white = 0
    total = 0
    for dy in range(-halfPx, halfPx + 1):
        for dx in range(-halfPx, halfPx + 1):
            px = int(min(max(projected[0] + dx, 0), width - 1))
            py = int(min(max(projected[1] + dy, 0), height - 1))
            rgb = image.pixel(px, py)
            r = (rgb >> 16) & 0xFF
            g = (rgb >> 8) & 0xFF
            b = rgb & 0xFF
            total += 1
            if min(r, g, b) > 200 and (max(r, g, b) - min(r, g, b)) < 30:
                white += 1
    return float(white) / float(total) if total else 0.0


# ---------------------------------------------------------------------------
# Model reads
# ---------------------------------------------------------------------------

def centerCV(session, cv):
    out = (ctypes.c_float * 3)()
    if session.dll.Tonic_GetCenterCV(session.model, int(cv), out) != 0:
        return None
    return (float(out[0]), float(out[1]), float(out[2]))


def centers(session):
    count = int(session.dll.Tonic_GetCenterCVCount(session.model))
    return [centerCV(session, cv) for cv in range(max(count, 0))]


def guideCount(session):
    guides = ctypes.c_int(0)
    session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides),
                                     None)
    return int(guides.value)


def gizmoKind(session):
    kind = ctypes.c_int(0)
    origin = (ctypes.c_float * 3)()
    frameArr = (ctypes.c_float * 9)()
    size = ctypes.c_float(0.0)
    handle = ctypes.c_int(0)
    if session.dll.Tonic_GetGizmo(session.model, ctypes.byref(kind), origin,
                                  frameArr, ctypes.byref(size),
                                  ctypes.byref(handle)) != 0:
        return (0, None)
    return (int(kind.value),
            (float(origin[0]), float(origin[1]), float(origin[2])))


def pumpUntilCommitted(viewport, session, tries=60):
    for _ in range(tries):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            return True
        wait(25)
    return False


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

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
        from usdGenTonicTools import tonicCamera, tonicPanels
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools: %s" % exc)
        return 1

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
    wait(50)

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

    # The region tint mesh is coincident with /Scalp, so leaving both on
    # would z-fight the pixel probes below. The model copied the scalp at
    # bind time, so the mouse still hits it through K1.
    stage.GetPrimAtPath("/Scalp").SetActive(False)

    camera = tonicCamera.resolve(view)
    check(camera is not None, "the controller's camera resolves")
    if camera is None:
        return 1

    def pixel(x, y, z):
        projected = camera.worldToPixels((x, y, z))
        return (projected[0], projected[1])

    perPixel = camera.worldPerPixel((2.0, 0.0, 2.0))
    state.snapRadiusPx = max(0.1 / max(perPixel, 1e-9), 2.0)
    info("world per pixel at the scalp: %.5f" % perPixel)

    # -- Graph: a real stroke, and the G14 tube stub behind it -------------
    mouse = Mouse(view)
    path = []
    for k in range(len(RECT)):
        x0, z0 = RECT[k]
        x1, z1 = RECT[(k + 1) % len(RECT)]
        for i in range(5):
            t = float(i) / 5.0
            path.append(pixel(x0 + (x1 - x0) * t, 0.0,
                              z0 + (z1 - z0) * t))
    path.append(pixel(RECT[0][0], 0.0, RECT[0][1]))

    mouse.press(path[0])
    if not viewport.gestureActive:
        info("QtTest press did not land; using direct QMouseEvent delivery")
        mouse.direct = True
        mouse.press(path[0])
    check(viewport.gestureActive, "the press opened a graph gesture")
    for point in path[1:]:
        mouse.move(point)
    mouse.release(path[-1])

    nodes, edges, regions = session.graphCounts()
    check((nodes, edges, regions) == (4, 4, 1),
          "the drag stroked one closed region (got %d/%d/%d)"
          % (nodes, edges, regions))
    cvCount = int(session.dll.Tonic_GetCenterCVCount(session.model))
    check(cvCount == STUB_CVS,
          "G14: the closed region got a tube stub on the graph release "
          "(%d center CVs, status %r)" % (cvCount, messages[-2:]))
    check(int(session.dll.Tonic_GetTubeRegionId(session.model)) == 0,
          "and the stub is rooted in that region")
    root = centerCV(session, 0)
    tip = centerCV(session, cvCount - 1)
    check(root is not None and abs(root[0] - 2.0) < 0.3 and
          abs(root[2] - 2.0) < 0.3,
          "its root sits in the middle of the region (%r)" % (root,))
    check(tip is not None and abs(tip[1] - STUB_LENGTH) < 1e-3,
          "and it grows along the region normal (%r)" % (tip,))
    check(session.enqueueCommit() and pumpUntilCommitted(viewport, session),
          "a graph-only groom now commits, because it has a tube")
    check(bool(stage.GetPrimAtPath("/TonicGroom")),
          "the committed groom reached the stage")

    # -- Tube mode ---------------------------------------------------------
    typeKey(view, "2")
    check(state.activeMode == "tube", "the 2 hotkey selects Tube mode (%r)"
          % state.activeMode)
    check(viewport.loop is not None and viewport.loop.modeId == "tube",
          "and the controller built the Tube loop")
    check(state.tubeSubMode == "center",
          "which opens in the Center sub-mode (%r)" % state.tubeSubMode)

    tipPixel = pixel(*tip)
    whiteBefore = whiteFraction(view, camera, tip)
    info("white at the tip CV before selecting: %.2f" % whiteBefore)
    check(whiteBefore < 0.05,
          "the tip CV draws in the clump colour, not white (%.2f)"
          % whiteBefore)

    mouse.click(tipPixel)
    selected = session.readSelection(2)          # TonicPick_CenterCV
    check(selected and selected[0][1] == cvCount - 1,
          "one click selected the tip center CV (%r)" % (selected,))
    kind, origin = gizmoKind(session)
    check(kind == 1, "a translate gizmo went to the model (kind %r)" % kind)
    check(origin is not None and abs(origin[1] - tip[1]) < 1e-3,
          "at the selected CV (%r vs %r)" % (origin, tip))
    whiteAfter = whiteFraction(view, camera, tip)
    info("white at the tip CV after selecting: %.2f" % whiteAfter)
    check(whiteAfter > whiteBefore + 0.05,
          "and the published CV dot turned white: %.2f -> %.2f"
          % (whiteBefore, whiteAfter))

    # -- the gizmo drag ----------------------------------------------------
    # The camera is 12 above the scalp and 8 above the tip, so a pixel is
    # worth less up there: the drag is unprojected at the CV's own depth,
    # and so is what this test expects of it.
    travelPx = 60.0
    tipPerPixel = camera.worldPerPixel(tip)
    info("world per pixel at the tip CV: %.5f" % tipPerPixel)
    target = (tipPixel[0] + travelPx, tipPixel[1])
    before = centers(session)
    version = session.modelVersion
    mouse.press(tipPixel)
    check(viewport.gestureActive, "the press took the gizmo's free handle")
    for step in range(1, 7):
        mouse.move((tipPixel[0] + travelPx * step / 6.0, tipPixel[1]))
    mouse.release(target)
    after = centers(session)
    moved = after[cvCount - 1][0] - before[cvCount - 1][0]
    want = travelPx * tipPerPixel
    info("the tip CV moved %.3f in x (%.1f px at %.5f per px = %.3f)"
         % (moved, travelPx, tipPerPixel, want))
    check(abs(moved - want) < 0.05 * want,
          "the drag moved the CV by what the cursor travelled (%.3f vs "
          "%.3f)" % (moved, want))
    check(after[0] == before[0],
          "and left the root alone (%r)" % (after[0],))
    check(session.modelVersion > version,
          "the model version advanced (%d -> %d)"
          % (version, session.modelVersion))

    movedTip = after[cvCount - 1]
    check(whiteFraction(view, camera, movedTip) > 0.05,
          "the published centers followed: the white dot is where the "
          "cursor left it (%.2f)"
          % whiteFraction(view, camera, movedTip))
    check(whiteFraction(view, camera, tip) < 0.05,
          "and gone from where it was (%.2f)"
          % whiteFraction(view, camera, tip))

    # -- TN-1: 50 real moves through the real controller -------------------
    samples = []
    mouse.press(pixel(*movedTip))
    for step in range(50):
        offset = 4.0 * (1 if step % 2 == 0 else -1) * (1 + step % 5)
        mouse.move((pixel(*movedTip)[0] + offset, pixel(*movedTip)[1]))
        samples.append(float(state.lastMoveMs))
    mouse.release(pixel(*movedTip))
    samples = [s for s in samples if s > 0.0]
    if samples:
        samples.sort()
        median = samples[len(samples) // 2]
        info("TN-1 through the controller: %d moves, median %.3f ms, "
             "worst %.3f ms (budget 8)" % (len(samples), median, samples[-1]))
        check(median < 8.0,
              "the median move is inside the TN-1 budget (%.3f ms)" % median)
        check(samples[-1] < 16.7,
              "and the worst still fits a frame (%.3f ms)" % samples[-1])
    else:
        check(False, "the controller timed no moves at all")

    # -- Ctrl+Z ------------------------------------------------------------
    typeKey(view, "z", ("ctrl",))
    typeKey(view, "z", ("ctrl",))
    restored = centers(session)
    check(abs(restored[cvCount - 1][0] - before[cvCount - 1][0]) < 1e-4,
          "two Ctrl+Z (one per drag) put the tip CV back (%r vs %r)"
          % (restored[cvCount - 1], before[cvCount - 1]))

    # -- Fill --------------------------------------------------------------
    typeKey(view, "3")
    check(state.activeMode == "fill" and viewport.loop is not None and
          viewport.loop.modeId == "fill",
          "the 3 hotkey selects Fill mode and its loop (%r)"
          % state.activeMode)
    session.select(1, [0], None, None, 0)        # the tube, for the panel
    descriptors = {d.id: d for d in tonicPanels.descriptors("fill", state)}
    check("density" in descriptors, "the Fill panel generates a density row")
    descriptors["density"].set(state, session, 8.0)
    sparse = guideCount(session)
    descriptors["density"].set(state, session, 32.0)
    dense = guideCount(session)
    info("guides at density 8: %d, at density 32: %d" % (sparse, dense))
    check(dense > sparse > 0,
          "the panel's density reached the tube and the guides followed "
          "(%d -> %d)" % (sparse, dense))
    check(int(round(descriptors["density"].get(state, session))) == 32,
          "and the panel reads back what it wrote")

    check(session.enqueueCommit() and pumpUntilCommitted(viewport, session),
          "the idle pump drains the committer after the fill")
    check(bool(stage.GetPrimAtPath("/TonicGroom/Guides")),
          "and the committed guides reached the stage")

    # -- tear down cleanly -------------------------------------------------
    viewport.uninstall()
    session.deactivate()
    print("testUsdviewTonicTube: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicTube needs testusdview (no live view)")
    sys.exit(0)
