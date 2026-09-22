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
import math
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

    def _send(self, kind, physical, mods, button, buttons=None):
        """One synthetic event straight at the widget.

        The fallback path, for a window the harness never showed: QtTest's
        global-coordinate round trip can miss the child widget entirely,
        and a missed press would look like a tool bug. QPointF is
        QtCore's, in PySide2 and PySide6 alike.
        """
        point = self._point(physical)
        local = self._QtCore.QPointF(point)
        globalPos = self._QtCore.QPointF(self._view.mapToGlobal(point))
        if buttons is None:
            buttons = button
        event = self._QtGui.QMouseEvent(kind, local, globalPos, button,
                                        buttons, mods)
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
        # QTest.mouseMove reports NoButton even after QTest.mousePress in
        # this Qt build. A held drag must carry NoButton as the changed
        # button and LeftButton in the persistent button-state field.
        self._send(QtCore.QEvent.Type.MouseMove, physical, mods,
                   QtCore.Qt.MouseButton.NoButton,
                   QtCore.Qt.MouseButton.LeftButton)

    def unheldMove(self, physical, modifiers=()):
        """A real MouseMove reporting lost left-button capture."""
        from pxr.Usdviewq.qt import QtCore
        mods = self._modifiers(modifiers)
        self._send(QtCore.QEvent.Type.MouseMove, physical, mods,
                   QtCore.Qt.MouseButton.NoButton,
                   QtCore.Qt.MouseButton.NoButton)

    def focusOut(self):
        """Send a keyboard-focus event without ending a held mouse drag."""
        event = self._QtCore.QEvent(self._QtCore.QEvent.Type.FocusOut)
        self._QtWidgets.QApplication.sendEvent(self._view, event)

    def ungrabMouse(self):
        """Send the capture-loss event that has no matching release."""
        event = self._QtCore.QEvent(self._QtCore.QEvent.Type.UngrabMouse)
        self._QtWidgets.QApplication.sendEvent(self._view, event)

    def release(self, physical, modifiers=()):
        from pxr.Usdviewq.qt import QtCore
        button = QtCore.Qt.MouseButton.LeftButton
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonRelease, physical, mods,
                       button, QtCore.Qt.MouseButton.NoButton)
        else:
            _qtTest().QTest.mouseRelease(self._view, button, mods,
                                         self._point(physical))

    def click(self, physical, modifiers=()):
        self.press(physical, modifiers)
        self.release(physical, modifiers)

    def doubleClick(self, physical, modifiers=()):
        """Deliver Qt's distinct double-click press followed by release."""
        from pxr.Usdviewq.qt import QtCore
        button = QtCore.Qt.MouseButton.LeftButton
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonDblClick, physical,
                       mods, button)
            self.release(physical, modifiers)
        else:
            _qtTest().QTest.mouseDClick(self._view, button, mods,
                                        self._point(physical))

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
            "d": QtCore.Qt.Key.Key_D,
            "c": QtCore.Qt.Key.Key_C,
            "w": QtCore.Qt.Key.Key_W,
            "r": QtCore.Qt.Key.Key_R,
            "m": QtCore.Qt.Key.Key_M,
            "f10": QtCore.Qt.Key.Key_F10,
            "f11": QtCore.Qt.Key.Key_F11,
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
    """The authored center cage, retained for geometry assertions."""
    out = (ctypes.c_float * 3)()
    if session.dll.Tonic_GetCenterCV(session.model, int(cv), out) != 0:
        return None
    return (float(out[0]), float(out[1]), float(out[2]))


def centerHandle(session, cv):
    """The centered core point published and picked for one root CV."""
    out = (ctypes.c_float * 3)()
    if session.dll.Tonic_GetTubeCenterHandle(session.model, 0, int(cv), out) != 0:
        return None
    return (float(out[0]), float(out[1]), float(out[2]))


def centers(session):
    count = int(session.dll.Tonic_GetCenterCVCount(session.model))
    return [centerCV(session, cv) for cv in range(max(count, 0))]


def sectionCV(session, tubeId, ring, slot):
    """World point for an authored section CV, using the public frame ABI."""
    from usdGenTonicTools import tonicBridge
    section = tonicBridge.tubeSection(session.dll, session.model, tubeId,
                                      ring)
    if slot < 0 or slot >= len(section[1]):
        return None
    entry = session.dll.Tonic_GetTubeSectionFrame
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
    u, v = section[1][slot]
    u, v = u * scale.value, v * scale.value
    u, v = u * ct - v * st, u * st + v * ct
    # Tonic_GetTubeSectionFrame returns the ring centroid as origin.  Center
    # the transformed authored UVs before applying that frame.
    placed = []
    for rawU, rawV in section[1]:
        rawU, rawV = rawU * scale.value, rawV * scale.value
        placed.append((rawU * ct - rawV * st,
                       rawU * st + rawV * ct))
    meanU = sum(pair[0] for pair in placed) / len(placed)
    meanV = sum(pair[1] for pair in placed) / len(placed)
    return tuple(origin[axis] + frame[axis] * (u - meanU) +
                 frame[axis + 3] * (v - meanV) for axis in range(3))


def guideCount(session):
    guides = ctypes.c_int(0)
    session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides),
                                     None)
    return int(guides.value)


def guidePreview(session):
    """Immutable guide points, for observing a real section edit's refill."""
    guides = ctypes.c_int(0)
    cvs = ctypes.c_int(0)
    if session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides),
                                        ctypes.byref(cvs)) != 0:
        return ()
    if guides.value <= 0 or cvs.value <= 0:
        return ()
    xyz = (ctypes.c_float * (3 * guides.value * cvs.value))()
    counts = (ctypes.c_int * guides.value)()
    got = ctypes.c_int(0)
    if session.dll.Tonic_ReadGuidePreview(
            session.model, xyz, len(xyz), counts, len(counts),
            ctypes.byref(got)) != 0:
        return ()
    return tuple(float(value) for value in xyz[:3 * got.value * cvs.value])


def gizmoKind(session, viewport=None):
    """Read the controller-owned gizmo when the Qt overlay is active.

    The native scene-index record is deliberately cleared while usdview draws
    the unoccluded transparent overlay, so it is a suppression check rather
    than the source of an interactive gizmo's state.
    """
    loop = getattr(viewport, "loop", None) if viewport is not None else None
    gizmo = getattr(loop, "_gizmo", None)
    if gizmo is not None and gizmo.visible:
        return (int(gizmo.kind), tuple(float(value) for value in gizmo.origin))
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


def sectionUniformFactor(before, after):
    """The common local-UV scale factor, or None when it is not uniform."""
    old, new = before[1], after[1]
    if len(old) != len(new) or len(old) < 2:
        return None
    oldCenter = tuple(sum(point[axis] for point in old) / len(old)
                      for axis in range(2))
    newCenter = tuple(sum(point[axis] for point in new) / len(new)
                      for axis in range(2))
    factors = []
    for oldPoint, newPoint in zip(old, new):
        oldRadius = math.hypot(oldPoint[0] - oldCenter[0],
                               oldPoint[1] - oldCenter[1])
        newRadius = math.hypot(newPoint[0] - newCenter[0],
                               newPoint[1] - newCenter[1])
        if oldRadius > 1e-6:
            factors.append(newRadius / oldRadius)
    if not factors or max(factors) - min(factors) > 2e-3:
        return None
    return sum(factors) / len(factors)


def pumpUntilCommitted(viewport, session, tries=60):
    for _ in range(tries):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            return True
        wait(25)
    return False


def liveMarquee(loop):
    """Read a loop's live marquee across the base and Tube loop APIs."""
    getter = getattr(loop, "marqueeRect", None)
    if callable(getter):
        return getter()
    return getattr(loop, "_marquee", None)


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
        from usdGenTonicTools import (tonicBridge, tonicCamera, tonicGizmo,
                                      tonicHierarchy, tonicLib, tonicPanels)
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
    workspace = container.workspace
    check(session is not None and session.model is not None and
          workspace is not None,
          "Bind scalp created a live model")
    check(viewport is not None and viewport.installed,
          "the viewport controller installed itself on the StageView")
    if session is None or viewport is None or session.model is None or \
            workspace is None:
        return 1
    session.setStatusSink(messages.append)

    def clickControl(text):
        """Invoke a visible dock action through its Qt button."""
        from pxr.Usdviewq.qt import QtWidgets
        for button in workspace.findChildren(QtWidgets.QAbstractButton):
            if button.text().split(" (", 1)[0] == text:
                button.click()
                wait(15)
                return True
        return False

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
    viewport.setPointerInside(True)
    typeKey(view, "d")
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
    tipHandle = centerHandle(session, cvCount - 1)
    check(tipHandle is not None,
          "the visible tip has a centered core handle (%r)" % (tipHandle,))
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

    if tipHandle is None:
        shutdown()
        return 1
    tipPixel = pixel(*tipHandle)
    whiteBefore = whiteFraction(view, camera, tipHandle)
    info("white at the tip CV before selecting: %.2f" % whiteBefore)
    check(whiteBefore < 0.05,
          "the tip CV draws in the clump colour, not white (%.2f)"
          % whiteBefore)

    mouse.click(tipPixel)
    selected = session.readSelection(2)          # TonicPick_CenterCV
    check(selected and selected[0][1] == cvCount - 1,
          "one click selected the tip center CV (%r)" % (selected,))
    kind, origin = gizmoKind(session, viewport)
    nativeKind, _ = gizmoKind(session)
    overlay = getattr(viewport, "_gizmoOverlay", None)
    check(kind == tonicGizmo.GIZMO_TRANSLATE and nativeKind == tonicGizmo.GIZMO_NONE
          and overlay is not None and overlay.isVisible(),
          "the Qt translate overlay is live while the native fallback is suppressed")
    check(origin is not None and abs(origin[1] - tipHandle[1]) < 1e-3,
          "at the selected core handle (%r vs %r)" % (origin, tipHandle))
    whiteAfter = whiteFraction(view, camera, tipHandle)
    info("white at the tip CV after selecting: %.2f" % whiteAfter)
    check(whiteAfter > whiteBefore + 0.05,
          "and the published CV dot turned white: %.2f -> %.2f"
          % (whiteBefore, whiteAfter))

    # Qt sends MouseButtonDblClick instead of the second press.  It is not
    # hierarchy navigation in Tube mode: repeated direct QMouseEvents must
    # retain the focused level and leave the selected CV draggable.
    focusBeforeDouble = state.activeLevel
    mouse.direct = True
    for _click in range(3):
        mouse.doubleClick(tipPixel)
    selected = session.readSelection(2)
    check(state.activeLevel == focusBeforeDouble and
          selected == [(0, cvCount - 1, -1)] and
          not viewport.gestureActive,
          "repeated Tube double-clicks keep focus and selection usable %r"
          % ({"level": state.activeLevel, "selection": selected,
             "gesture": viewport.gestureActive},))

    # Missed releases used to leave the controller captured forever.  A
    # fresh real press must cancel the old no-travel bracket before arming
    # the new one, and an actual no-left-button move must recover a stale
    # marquee without treating an ordinary viewport Leave as cancellation.
    mouse.press(tipPixel)
    check(viewport.gestureActive, "the first recovery fixture press armed")
    mouse.press(tipPixel)                 # prior release intentionally lost
    check(viewport.gestureActive,
          "a fresh press recovers then arms a new Tube gesture")
    mouse.release(tipPixel)
    mouse.press(tipPixel)
    mouse.focusOut()
    mouse.move((tipPixel[0] + 4.0, tipPixel[1]))
    check(viewport.gestureActive,
          "keyboard FocusOut preserves a held Tube mouse capture")
    mouse.ungrabMouse()
    check(not viewport.gestureActive,
          "UngrabMouse cancels an otherwise release-less Tube gesture")
    blank = pixel(0.25, 0.0, 0.25)
    mouse.press(blank)
    mouse.move((blank[0] + 16.0, blank[1] + 12.0))
    check(viewport.gestureActive and liveMarquee(viewport.loop) is not None,
          "a blank Tube drag owns a live marquee before capture loss")
    mouse.unheldMove((blank[0] + 20.0, blank[1] + 14.0))
    check(not viewport.gestureActive and liveMarquee(viewport.loop) is None,
          "a no-left move cancels the stale marquee and recovers")
    mouse.drag([blank, (blank[0] + 18.0, blank[1] + 14.0)])
    check(not viewport.gestureActive,
          "the next marquee completes after recovery")
    mouse.click(tipPixel)
    check(session.readSelection(2) == [(0, cvCount - 1, -1)],
          "a CV remains selectable after interrupted marquee recovery")

    # -- the gizmo drag ----------------------------------------------------
    # The camera is 12 above the scalp and 8 above the tip, so a pixel is
    # worth less up there: the drag is unprojected at the CV's own depth,
    # and so is what this test expects of it.
    travelPx = 60.0
    tipPerPixel = camera.worldPerPixel(tipHandle)
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
    movedTipHandle = centerHandle(session, cvCount - 1)
    movedWhite = (whiteFraction(view, camera, movedTipHandle)
                  if movedTipHandle is not None else 0.0)
    check(movedTipHandle is not None and
          movedWhite > 0.05,
          "the published centers followed: the white dot is where the "
          "cursor left it (%.2f)"
          % movedWhite)
    check(whiteFraction(view, camera, tipHandle) < 0.05,
          "and gone from where it was (%.2f)"
          % whiteFraction(view, camera, tipHandle))

    # -- TN-1: 50 real moves through the real controller -------------------
    samples = []
    movedTipPixel = pixel(*movedTipHandle) if movedTipHandle is not None else None
    if movedTipPixel is None:
        check(False, "the moved center CV retains a visible core handle")
        shutdown()
        return 1
    mouse.press(movedTipPixel)
    for step in range(50):
        offset = 4.0 * (1 if step % 2 == 0 else -1) * (1 + step % 5)
        mouse.move((movedTipPixel[0] + offset, movedTipPixel[1]))
        samples.append(float(state.lastMoveMs))
    mouse.release(movedTipPixel)
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

    # -- Ring scale and Section CV editing on a non-root tube -------------
    # Feed selection through the same live session the shelf uses, then
    # drive the visible gizmos with real StageView mouse events.  A child is
    # deliberate: root-only tests cannot catch an operation that silently
    # falls back to tube 0.
    # Select the visible root and subdivide through the Hierarchy dock.  This
    # exercises the same active-cut expansion path as an artist; directly
    # creating children leaves the parent frontier collapsed.
    workspace._modeButtons["hierarchy"].click()
    wait(15)
    rootPoint = sectionCV(session, 0, 1, 0)
    rootPixel = pixel(*rootPoint) if rootPoint is not None else None
    if rootPixel is not None:
        mouse.click(rootPixel)
    rootSelection = session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)
    check(rootSelection == [(0, -1, -1)],
          "the visible root is selected before child subdivision %r"
          % (rootSelection,))
    didSubdivide = (rootSelection == [(0, -1, -1)] and
                    clickControl("Subdivide"))
    children = ([tube for tube in tonicBridge.readTubeIds(
        session.dll, session.model) if tube != 0] if didSubdivide else [])
    check(bool(children), "the root subdivides for non-root Tube editing")
    if children:
        entered = clickControl("Enter level")
        check(entered and int(state.activeLevel) >= 2,
              "the Hierarchy dock enters the created child level")
        child = int(children[0])
        ring = 1
        beforeRing = tonicBridge.tubeSection(session.dll, session.model,
                                             child, ring)
        # This is the artist workflow: enter the descendant level before
        # selecting its rings, so L2 is the focused editable display.
        workspace._modeButtons["tube"].click()
        wait(15)
        if "ring" in getattr(workspace, "_tubeSelectionButtons", {}):
            workspace._tubeSelectionButtons["ring"].click()
            wait(10)
        else:
            view.setFocus()
            typeKey(view, "f10")
        check(state.tubeSubMode == "ring",
              "F10 chooses the explicit Ring component mode")
        session.clearSelection()
        session.select(128, [child], [ring], [-1], 0)  # SectionRing
        # The visible numeric control is an absolute value, complementary to
        # the ring-handle's relative drag.  It must use the selected child,
        # not silently modify tube 0, and it must refill live guides.
        tubeRows = {row.id: row for row in tonicPanels.descriptors("tube",
                                                                     state)}
        previewBeforeScale = guidePreview(session)
        numericScale = beforeRing[2] * 1.25
        tubeRows["uniformScale"].set(state, session, numericScale)
        numericRing = tonicBridge.tubeSection(session.dll, session.model,
                                              child, ring)
        previewAfterScale = guidePreview(session)
        guidesAfterScale = guideCount(session)
        check(abs(numericRing[2] - numericScale) < 1e-4 and
              guidesAfterScale > 0 and previewAfterScale != previewBeforeScale,
              "the selected-section scale control changes child T%d and "
              "keeps a live guide preview" % child)
        beforeRing = numericRing
        typeKey(view, "r")
        viewport.loop._placeGizmo(camera)
        ringGizmo = viewport.loop._gizmo
        scaleCenters = [handle for handle in ringGizmo.screenHandles(camera)
                        if handle["kind"] == "center" and
                        handle.get("grabbable", False)]
        check(state.activeMode == "tube" and state.tubeSubMode == "ring" and
              state.transformTool == "scale" and ringGizmo.visible and
              ringGizmo.kind == tonicGizmo.GIZMO_SCALE and scaleCenters,
              "R raises the explicit Scale gizmo on the focused child ring")
        if scaleCenters:
            ringPixel = scaleCenters[0]["points"][0]
            target = (ringPixel[0] + 32.0, ringPixel[1])
            check(ringGizmo.handleAt(camera, ringPixel[0], ringPixel[1]) ==
                  tonicGizmo.HANDLE_CENTER,
                  "the Scale gizmo centre accepts uniform child-ring scale")
            mouse.press(ringPixel)
            check(viewport.gestureActive and ringGizmo.dragging and
                  ringGizmo.activeHandle == tonicGizmo.HANDLE_CENTER,
                  "the real press begins a child Scale gesture")
            mouse.move(target)
            mouse.release(target)
            afterRing = tonicBridge.tubeSection(session.dll, session.model,
                                                child, ring)
            guidesAfterRing = guideCount(session)
            factor = sectionUniformFactor(beforeRing, afterRing)
            check(factor is not None and factor > 1.01 and guidesAfterRing > 0,
                  "a real Scale drag uniformly scales child T%d "
                  "and retains its guide preview (factor %.3f, %d guides)"
                  % (child, factor or 0.0, guidesAfterRing))

        beforeSection = tonicBridge.tubeSection(session.dll, session.model,
                                                 child, ring)
        previewBeforeCV = guidePreview(session)
        typeKey(view, "f11")
        typeKey(view, "w")
        session.clearSelection()
        childCV = sectionCV(session, child, ring, 0)
        check(childCV is not None,
              "the child section CV has a public, projectable position")
        if childCV is not None:
            mouse.click(pixel(childCV[0], childCV[1], childCV[2]))
        cvSelection = session.readSelection(4)         # SectionCV
        check(cvSelection == [(child, ring, 0)],
              "a real child Section-CV click selects its tube/ring/slot %r"
              % (cvSelection,))
        viewport.loop._placeGizmo(camera)
        cvGizmo = viewport.loop._gizmo
        kind, cvOrigin = gizmoKind(session, viewport)
        check(state.tubeSubMode == "section" and state.transformTool == "move"
              and cvGizmo.visible and kind == tonicGizmo.GIZMO_TRANSLATE and
              cvOrigin is not None,
              "F11 then W raises Move on the focused child section CV")
        if cvOrigin is not None and cvGizmo.visible:
            cvPixel = camera.worldToPixels(cvOrigin)
            cvTarget = (cvPixel[0] + 24.0, cvPixel[1])
            mouse.press(cvPixel)
            check(viewport.gestureActive and cvGizmo.dragging,
                  "the real press begins a child section-CV gesture")
            mouse.move(cvTarget)
            mouse.release(cvTarget)
            afterSection = tonicBridge.tubeSection(session.dll, session.model,
                                                    child, ring)
            previewAfterCV = guidePreview(session)
            guidesAfterCV = guideCount(session)
            changed = (afterSection[1][0] != beforeSection[1][0])
            siblingSame = (afterSection[1][1] == beforeSection[1][1])
            previewChanged = (previewAfterCV != previewBeforeCV)
            info("child section drag: changed=%r siblingSame=%r "
                 "previewChanged=%r before=%r after=%r" %
                 (changed, siblingSame, previewChanged,
                  beforeSection[1][0], afterSection[1][0]))
            check(changed and siblingSame,
                  "a real Section drag moves only child T%d ring %d CV 0"
                  % (child, ring))
            check(guidesAfterCV > 0 and previewChanged,
                  "the child Section drag refills its live guides (%d)"
                  % guidesAfterCV)

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
