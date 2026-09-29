# testUsdviewPomadeTube -- T3: Tube and Fill driven by REAL mouse events
# (plan/18 V4 exit).
#
#   testusdview --testScript plugin/usdGenPomadeTools/testenv/testUsdviewPomadeTube.py \
#               examples/pomade-graph-scalp.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the other T3 pomade scripts
# use, plus the staged usdGenPomadeTools package and the usdGenPomade DLL
# (via USDGENPOMADE_DLL or the build tree).
#
# The ABI-level script beside this one (testPomadeAbiTube.py) calls the tube
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
#     is /__usdGenPomade/centers/L1 and centerCVs/L1 arriving at Hydra;
#   * the whole drag is ONE undo step: Ctrl+Z puts the CV back;
#   * TN-1 re-measured through the real controller: 50 real mouse moves,
#     each timed end to end by the controller (model edit + publish +
#     updateGL request);
#   * Fill: the panel's density descriptor writes the selection's fill
#     params and the guide count follows.
#
# Reading the published geometry: this USD build exposes no terminal scene
# index to Python, and the Pomade prims have no USD origin for a pick to
# name, so the only way to see what Hydra got is the framebuffer -- the
# technique testUsdviewPomadeGraph.py and testUsdviewPomadePublish.py use.
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
# pomadeTube.DEFAULT_RINGS / DEFAULT_LENGTH: what the G14 stub builds with.
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
            "4": QtCore.Qt.Key.Key_4,
            "5": QtCore.Qt.Key.Key_5,
            "6": QtCore.Qt.Key.Key_6,
            "d": QtCore.Qt.Key.Key_D,
            "c": QtCore.Qt.Key.Key_C,
            "w": QtCore.Qt.Key.Key_W,
            "e": QtCore.Qt.Key.Key_E,
            "r": QtCore.Qt.Key.Key_R,
            "m": QtCore.Qt.Key.Key_M,
            "p": QtCore.Qt.Key.Key_P,
            "g": QtCore.Qt.Key.Key_G,
            "s": QtCore.Qt.Key.Key_S,
            "f8": QtCore.Qt.Key.Key_F8,
            "f9": QtCore.Qt.Key.Key_F9,
            "f10": QtCore.Qt.Key.Key_F10,
            "f11": QtCore.Qt.Key.Key_F11,
            "delete": QtCore.Qt.Key.Key_Delete}
    keys.update(_gizmoKeys())
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    _qtTest().QTest.keyClick(view, keys[name], mods)


def _gizmoKeys():
    """GZ-05 / parity G11, G13: the gizmo keys, and the J / X holds."""
    from pxr.Usdviewq.qt import QtCore
    return {"j": QtCore.Qt.Key.Key_J, "x": QtCore.Qt.Key.Key_X,
            "l": QtCore.Qt.Key.Key_L, "+": QtCore.Qt.Key.Key_Plus,
            "=": QtCore.Qt.Key.Key_Equal, "-": QtCore.Qt.Key.Key_Minus}


def holdKey(view, name, down):
    """Press (down=True) or release one gizmo hold key WITHOUT the other half.

    QTest.keyClick would press and release at once; a hold has to stay
    down across mouse moves, exactly as the artist's finger does.
    """
    from pxr.Usdviewq.qt import QtCore
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    key = _gizmoKeys()[name]
    if down:
        _qtTest().QTest.keyPress(view, key, mods)
    else:
        _qtTest().QTest.keyRelease(view, key, mods)


def wheel(view, physical, notches=1):
    """One real QWheelEvent (a dolly notch) sent straight at the StageView.

    The left button is reported held, as it is under a gizmo drag.  The
    (pos, globalPos, pixelDelta, angleDelta, buttons, modifiers, phase,
    inverted) constructor is the same in Qt 5.12+ and Qt 6.  Returns False
    when this binding cannot build the event.
    """
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
    try:
        ratio = float(view.devicePixelRatioF())
    except AttributeError:
        ratio = 1.0
    local = QtCore.QPointF(physical[0] / ratio, physical[1] / ratio)
    globalPos = QtCore.QPointF(view.mapToGlobal(local.toPoint()))
    try:
        event = QtGui.QWheelEvent(
            local, globalPos, QtCore.QPoint(0, 0),
            QtCore.QPoint(0, 120 * int(notches)),
            QtCore.Qt.MouseButton.LeftButton,
            QtCore.Qt.KeyboardModifier.NoModifier,
            QtCore.Qt.ScrollPhase.NoScrollPhase, False)
    except (TypeError, AttributeError):
        return False
    QtWidgets.QApplication.sendEvent(view, event)
    return True


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
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/PomadeTubeCamera"))
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
        "/PomadeTubeCamera")
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
    if session.dll.Pomade_GetCenterCV(session.model, int(cv), out) != 0:
        return None
    return (float(out[0]), float(out[1]), float(out[2]))


def centerHandle(session, cv):
    """The centered core point published and picked for one root CV."""
    out = (ctypes.c_float * 3)()
    if session.dll.Pomade_GetTubeCenterHandle(session.model, 0, int(cv), out) != 0:
        return None
    return (float(out[0]), float(out[1]), float(out[2]))


def centers(session):
    count = int(session.dll.Pomade_GetCenterCVCount(session.model))
    return [centerCV(session, cv) for cv in range(max(count, 0))]


def sectionCV(session, tubeId, ring, slot):
    """World point for an authored section CV, using the public frame ABI."""
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
    u, v = section[1][slot]
    u, v = u * scale.value, v * scale.value
    u, v = u * ct - v * st, u * st + v * ct
    # Pomade_GetTubeSectionFrame returns the ring centroid as origin.  Center
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
    session.dll.Pomade_GetGuideCounts(session.model, ctypes.byref(guides),
                                     None)
    return int(guides.value)


def guidePreview(session):
    """Immutable guide points, for observing a real section edit's refill."""
    guides = ctypes.c_int(0)
    cvs = ctypes.c_int(0)
    if session.dll.Pomade_GetGuideCounts(session.model, ctypes.byref(guides),
                                        ctypes.byref(cvs)) != 0:
        return ()
    if guides.value <= 0 or cvs.value <= 0:
        return ()
    xyz = (ctypes.c_float * (3 * guides.value * cvs.value))()
    counts = (ctypes.c_int * guides.value)()
    got = ctypes.c_int(0)
    if session.dll.Pomade_ReadGuidePreview(
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
    if session.dll.Pomade_GetGizmo(session.model, ctypes.byref(kind), origin,
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


def gizmoParity(stage, view, session, viewport, mouse):
    """GZ-01/GZ-03 and RigExec parity G03/G04 through real events.

    A side camera spaces the stub's center CVs about 55 px apart, so with
    CV 1 selected the next CV sits on the V axis of its 90 px gizmo (the
    live walkthrough's step 10).  The handle must win that press, hover
    must prehighlight it, the dragged handle stays remembered and a middle
    drag repeats it; an unselected CV off the gizmo tweak-drags in one
    gesture; Ctrl on the centre over the selected CV is a click when it
    does not travel and the root-normal constraint when it does.  Every
    click leaves no undo step, so exactly one Ctrl+Z per real drag puts
    the tube back.  The top camera is restored at the end.
    """
    from usdGenPomadeTools import pomadeCamera, pomadeGizmo, pomadeLib, pomadeLoops
    loop = viewport.loop
    gizmo = loop._gizmo
    cvCount = int(session.dll.Pomade_GetCenterCVCount(session.model))
    beforeParity = centers(session)
    tip = cvCount - 1

    # testusdview execs this script with no __file__; --testScript names it.
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
        for index, argument in enumerate(sys.argv):
            if argument == "--testScript" and index + 1 < len(sys.argv):
                here = os.path.dirname(os.path.abspath(sys.argv[index + 1]))
    if here and here not in sys.path:
        sys.path.insert(0, here)
    try:
        import pomadeT3
    except ImportError as exc:
        check(False, "pomadeT3 (middle-button driver) imports: %s" % exc)
        return

    def side(distance):
        pomadeT3.sideCamera(stage, view, eye=(2.0 + distance, 2.1, 2.0))
        view.setFocus()
        wait(40)
        return pomadeCamera.resolve(view)

    def px(camera, cv):
        point = centerHandle(session, cv)
        projected = camera.worldToPixels(point) if point else None
        return (projected[0], projected[1]) if projected else None

    # Dolly so the CV spacing is about 55 px whatever the window size.
    camera = side(13.0)
    p1, p2 = px(camera, 1), px(camera, 2)
    spacing = abs(p2[1] - p1[1]) if p1 and p2 else 0.0
    if spacing > 1.0:
        camera = side(13.0 * spacing / 55.0)
    p1, p2 = px(camera, 1), px(camera, 2)
    info("side camera CV spacing %.1f px" % abs(p2[1] - p1[1]))

    session.clearSelection()
    loop._placeGizmo(camera)
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    mouse.click(p1)
    check(session.readSelection(2) == [(0, 1, -1)] and gizmo.visible,
          "GZ-01: a click selects CV 1 and raises its gizmo %r"
          % (session.readSelection(2),))
    camera = pomadeCamera.resolve(view)
    p1, p2 = px(camera, 1), px(camera, 2)
    handle = gizmo.handleAt(camera, p2[0], p2[1])
    component = loop._componentItem(pomadeLoops.Sample(session, camera,
                                                      p2[0], p2[1]))
    check(handle != pomadeGizmo.HANDLE_NONE and component is not None and
          (component["id"], component["subId"]) == (0, 2),
          "CV 2's pixel is both on a handle (%r) and on the unselected CV "
          "(%r)" % (handle, component))

    # G03: hover prehighlights the handle, not the CV under it.
    mouse.unheldMove(p2)
    wait(10)
    records = gizmo.screenHandles(camera)
    hovered = [r for r in records if r.get("hovered")]
    check(gizmo.hoverHandle == pomadeGizmo.HANDLE_V and hovered and
          hovered[0]["handle"] == pomadeGizmo.HANDLE_V and
          hovered[0]["color"] == pomadeGizmo.HOVER_COLOR,
          "hovering CV 2's pixel prehighlights HANDLE_V in the hover colour "
          "(%r, %r)" % (gizmo.hoverHandle,
                        [r["handle"] for r in hovered]))
    from pxr.Usdviewq.qt import QtCore
    shapes = QtCore.Qt.CursorShape
    check(view.cursor().shape() == shapes.SizeAllCursor,
          "FB-02: the cursor over a gizmo handle is SizeAll (%r)"
          % (view.cursor().shape(),))

    # GZ-01: the press drags the V handle; CV 1 moves, CV 2 does not.
    before = centers(session)
    perPixel = camera.worldPerPixel(centerHandle(session, 1))
    travel = 50.0
    mouse.press(p2)
    check(viewport.gestureActive and gizmo.dragging and
          gizmo.activeHandle == pomadeGizmo.HANDLE_V,
          "the press on CV 2's pixel drags the V handle")
    check(view.cursor().shape() == shapes.ClosedHandCursor,
          "FB-02: a handle drag holds the closed-hand cursor (%r)"
          % (view.cursor().shape(),))
    for step in range(1, 6):
        mouse.move((p2[0], p2[1] - travel * step / 5.0))
    mouse.release((p2[0], p2[1] - travel))
    after = centers(session)
    moved = after[1][1] - before[1][1]
    want = travel * perPixel
    check(abs(moved - want) < 0.05 * want,
          "CV 1 rose by the cursor travel (%.4f vs %.4f)" % (moved, want))
    check(after[2] == before[2] and session.readSelection(2) ==
          [(0, 1, -1)],
          "CV 2 did not move and the selection is still CV 1 %r"
          % (session.readSelection(2),))

    # G04: the dragged handle stays remembered, and a middle drag in open
    # space repeats it.
    records = gizmo.screenHandles(camera)
    vRecord = [r for r in records if r["handle"] == pomadeGizmo.HANDLE_V]
    check(gizmo.selectedHandle == pomadeGizmo.HANDLE_V and vRecord and
          vRecord[0]["selected"] and
          vRecord[0]["color"] == pomadeGizmo.ACTIVE_COLOR,
          "after the release HANDLE_V stays selected, in yellow")
    middle = pomadeT3.Mouse(view)
    middle.direct = True
    camera = pomadeCamera.resolve(view)
    p1 = px(camera, 1)
    away = (p1[0] + 150.0, p1[1])
    check(gizmo.handleAt(camera, away[0], away[1]) == pomadeGizmo.HANDLE_NONE,
          "150 px right of the gizmo is open space")
    before = centers(session)
    perPixel = camera.worldPerPixel(centerHandle(session, 1))
    middle.press(away, button="middle")
    check(viewport.gestureActive and gizmo.dragging and
          gizmo.activeHandle == pomadeGizmo.HANDLE_V,
          "a middle press there repeats the V handle")
    for step in range(1, 4):
        middle.move((away[0], away[1] - 30.0 * step / 3.0))
    middle.release((away[0], away[1] - 30.0), button="middle")
    after = centers(session)
    moved = after[1][1] - before[1][1]
    want = 30.0 * perPixel
    check(not viewport.gestureActive and abs(moved - want) < 0.05 * want,
          "the middle drag moved CV 1 along V by its travel (%.4f vs %.4f)"
          % (moved, want))
    # Two drags, two undo steps (the selecting click made none).  Undo
    # them so CV 1's gizmo is back well below the tip.
    for _ in range(2):
        typeKey(view, "z", ("ctrl",))
        wait(10)
    check(all(abs(a - b) < 1e-4 for cv in range(cvCount)
              for a, b in zip(centers(session)[cv], beforeParity[cv])),
          "two Ctrl+Z undo the V and the middle drag: the click made no "
          "undo step")
    loop._placeGizmo(pomadeCamera.resolve(view))

    # GZ-03: an UNSELECTED tip off the gizmo tweak-drags in one gesture.
    camera = pomadeCamera.resolve(view)
    tipPixel = px(camera, tip)
    check(gizmo.handleAt(camera, tipPixel[0], tipPixel[1]) ==
          pomadeGizmo.HANDLE_NONE, "the tip CV is off the gizmo")
    before = centers(session)
    perPixel = camera.worldPerPixel(centerHandle(session, tip))
    mouse.drag([tipPixel] + [(tipPixel[0], tipPixel[1] + 60.0 * s / 6.0)
                             for s in range(1, 7)])
    after = centers(session)
    moved = before[tip][1] - after[tip][1]
    want = 60.0 * perPixel
    check(session.readSelection(2) == [(0, tip, -1)] and
          abs(moved - want) < 0.05 * want,
          "one press selected the tip and dragged it down (%.4f vs %.4f)"
          % (moved, want))
    typeKey(view, "z", ("ctrl",))
    wait(10)
    check(all(abs(a - b) < 1e-4 for a, b in
              zip(centers(session)[tip], before[tip])),
          "one Ctrl+Z undoes the whole tweak")

    # GZ-03: Ctrl-click on the selected tip (under the centre) deselects.
    loop._placeGizmo(pomadeCamera.resolve(view))
    camera = pomadeCamera.resolve(view)
    tipPixel = px(camera, tip)
    if session.readSelection(2) != [(0, tip, -1)]:
        mouse.click(tipPixel)
    before = centers(session)
    check(gizmo.handleAt(camera, tipPixel[0], tipPixel[1]) ==
          pomadeGizmo.HANDLE_CENTER, "the selected tip sits under the centre")
    mouse.click(tipPixel, ("ctrl",))
    check(session.readSelection(2) == [] and centers(session) == before,
          "a Ctrl click on it deselects it and moves nothing %r"
          % (session.readSelection(2),))

    # Ctrl-drag on the centre: the root-normal (+Y here) constraint.
    mouse.click(tipPixel)
    before = centers(session)
    mouse.drag([tipPixel] + [(tipPixel[0] + 8.0 * s, tipPixel[1] - 8.0 * s)
                             for s in range(1, 6)], ("ctrl",))
    after = centers(session)
    delta = [after[tip][i] - before[tip][i] for i in range(3)]
    check(delta[1] > 0.0 and abs(delta[0]) < 0.1 * delta[1] and
          abs(delta[2]) < 0.1 * delta[1],
          "a Ctrl drag on the centre moves the tip along the root normal "
          "(%r)" % (delta,))

    # One real drag remains (Ctrl); no click left a step.
    typeKey(view, "z", ("ctrl",))
    wait(10)
    restored = centers(session)
    check(all(abs(a - b) < 1e-4 for cv in range(cvCount)
              for a, b in zip(restored[cv], beforeParity[cv])),
          "one Ctrl+Z restores the tube: the clicks made no undo steps")

    frameScalp(stage, view)
    view.setFocus()
    wait(40)
    session.clearSelection()
    loop._placeGizmo(pomadeCamera.resolve(view))
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)


def gizmoLook(view, session, viewport, mouse, messages):
    """GZ-02 / parity G05, G10: the RigExec look through the real overlay.

    With the tip CV selected: Move draws cone tips, Scale cube tips, and a
    real rotate drag on the camera-facing ring reports its angle in the
    status line and grows a pie wedge.  Each state is painted through the
    real overlay (a paint exception would print a Traceback), and a red
    cone pixel is probed in the overlay's own grab.  The rotate drag is
    undone and the Move tool restored, so the tip stays selected under
    Move for the checks that follow.
    """
    from usdGenPomadeTools import pomadeCamera, pomadeGizmo
    loop = viewport.loop
    gizmo = loop._gizmo
    overlay = viewport._gizmoOverlay
    viewport.setPointerInside(True)
    view.setFocus()
    wait(10)
    selection = session.readSelection(2)
    before = centers(session)

    def paint():
        overlay.repaint()
        wait(5)

    def axisTips():
        return [record.get("tip") for record in viewport.gizmoScreenHandles()
                if record["kind"] == "axis"]

    tips = axisTips()
    check(toolOf(viewport) == "move" and len(tips) == 3 and
          all(tip == "cone" for tip in tips),
          "GZ-02: Move draws three cone-tipped axes %r" % (tips,))
    paint()
    uAxis = [record for record in viewport.gizmoScreenHandles()
             if record["handle"] == pomadeGizmo.HANDLE_U and
             record["grabbable"] and not record["hovered"] and
             not record["selected"]]
    if uAxis and overlay is not None:
        start, end = uAxis[0]["points"][0], uAxis[0]["points"][-1]
        length = ((end[0] - start[0]) ** 2 + (end[1] - start[1]) ** 2) ** 0.5
        cone = (pomadeGizmo.CONE_RADIUS * pomadeGizmo.CONE_LENGTH_RATIO *
                uAxis[0]["sizePx"])
        probe = (end[0] - (end[0] - start[0]) / length * cone * 0.4,
                 end[1] - (end[1] - start[1]) / length * cone * 0.4)
        image = overlay.grab().toImage()
        pixel = image.pixelColor(int(round(probe[0])), int(round(probe[1])))
        check(pixel.red() > 180 and pixel.green() < 90 and
              pixel.blue() < 90,
              "the U cone paints solid red in the overlay (rgba %r at %r)"
              % ((pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()),
                 probe))
    else:
        check(False, "a plain grabbable U axis to probe (%r)" % (uAxis,))

    typeKey(view, "r")
    wait(10)
    tips = axisTips()
    check(toolOf(viewport) == "scale" and gizmo.kind ==
          pomadeGizmo.GIZMO_SCALE and len(tips) == 3 and
          all(tip == "cube" for tip in tips),
          "GZ-02: R's Scale gizmo draws three cube-tipped axes %r" % (tips,))
    paint()

    # G10: a real rotate drag on the ring facing the camera.  Rotate starts
    # Local (parity G16), so L puts it on World first; the camera looks
    # straight down, so the ring about world Y (V) is the one facing it.
    typeKey(view, "e")
    wait(10)
    check(loop.transformOrientation() == "tube",
          "G16: E's Rotate starts in the Local (tube) orientation (%r)"
          % loop.transformOrientation())
    typeKey(view, "l")
    wait(10)
    check(loop.transformOrientation() == "world",
          "and L flips Rotate to World (%r)" % loop.transformOrientation())
    camera = pomadeCamera.resolve(view)
    loop._placeGizmo(camera)
    facing = pomadeGizmo.HANDLE_V
    rings = [record for record in viewport.gizmoScreenHandles()
             if record["handle"] == facing and
             record["kind"] == "ring"]
    check(toolOf(viewport) == "rotate" and rings,
          "E raises the Rotate gizmo with its camera-facing V ring")
    if rings:
        paint()
        centre = gizmo.origin
        cx, cy = camera.worldToPixels(centre)[:2]
        radius = 0.85 * rings[0]["sizePx"]
        import math

        def onRing(degrees):
            radians = math.radians(degrees)
            return (cx + radius * math.cos(radians),
                    cy - radius * math.sin(radians))

        press = onRing(45.0)
        check(gizmo.handleAt(camera, press[0], press[1]) == facing,
              "45 degrees round the V ring picks HANDLE_V")
        del messages[:]
        mouse.press(press)
        for step in range(1, 7):
            mouse.move(onRing(45.0 + 90.0 * step / 6.0))
        angle = gizmo.dragAngle()
        pie = gizmo.pieSlice()
        paint()                         # the pie through the real painter
        mouse.release(onRing(135.0))
        readouts = [line for line in messages
                    if "Rotate" in str(line) and "°" in str(line)]
        check(abs(angle) > 1.0 and abs(abs(angle) - 90.0) < 10.0,
              "a quarter turn on the W ring reads %.1f deg" % angle)
        check(pie is not None and len(pie[0]) > 3,
              "the drag grew a pie wedge (%r points)"
              % (len(pie[0]) if pie else None,))
        check(readouts, "the status line reads the live angle (%r)"
              % (readouts[-1:] or messages[-3:],))
        check(gizmo.pieSlice() is None, "the release takes the pie away")
        typeKey(view, "z", ("ctrl",))
        wait(10)
    typeKey(view, "w")
    wait(10)
    loop._placeGizmo(pomadeCamera.resolve(view))
    restored = centers(session)
    check(toolOf(viewport) == "move" and
          session.readSelection(2) == selection and
          all(abs(a - b) < 1e-4 for cv in range(len(before))
              for a, b in zip(restored[cv], before[cv])),
          "one Ctrl+Z undid the rotation; W is back on the selected tip")


def gizmoSettingsParity(stage, view, session, viewport, state, mouse,
                        messages, workspace=None):
    """GZ-05 and parity G07/G08/G11/G13/G14/G23 through real events.

    An oblique camera, so every world axis has screen length.  World is
    the default orientation (three grabbable axes >= 12 px); `+`/`-` resize
    the manipulator and `L` flips World <-> Tube; Ctrl mid-drag on an axis
    moves in the perpendicular plane; held J quantises, held X lands the
    pivot on the grid and letting go un-snaps, and usdview's own J never
    fires; undo refuses mid-drag and all three redo keys redo; Scale through
    the pivot mirrors unless Prevent Negative Scale; Free Rotate hides the
    ball.  Every drag is undone; the top camera is restored at the end.
    """
    from usdGenPomadeTools import (pomadeCamera, pomadeGizmo,
                                  pomadeGizmoSettings, pomadeLib, pomadePanels)
    try:
        import pomadeT3
    except ImportError as exc:
        check(False, "pomadeT3 (camera helper) imports: %s" % exc)
        return
    loop = viewport.loop
    gizmo = loop._gizmo
    settings = pomadeGizmoSettings.settingsFor(state)
    cvCount = int(session.dll.Pomade_GetCenterCVCount(session.model))
    tip = cvCount - 1
    baseline = centers(session)
    pomadeT3.sideCamera(stage, view, eye=(9.0, 8.0, 10.0),
                       target=(2.0, 3.0, 2.0),
                       primPath="/PomadeT3ObliqueCamera")
    view.setFocus()
    viewport.setPointerInside(True)
    wait(40)
    camera = pomadeCamera.resolve(view)
    ratio = pomadeGizmo.cameraPixelRatio(camera)

    def px(cv):
        projected = camera.worldToPixels(centerHandle(session, cv))
        return (projected[0], projected[1])

    def restored(what):
        typeKey(view, "z", ("ctrl",))
        wait(10)
        now = centers(session)
        check(all(abs(a - b) < 1e-4 for cv in range(cvCount)
                  for a, b in zip(now[cv], baseline[cv])),
              "one Ctrl+Z undoes the %s" % what)

    def axisRecords():
        return {record["handle"]: record
                for record in viewport.gizmoScreenHandles()
                if record["kind"] == "axis"}

    def length(record):
        start, end = record["points"][0], record["points"][-1]
        return math.hypot(end[0] - start[0], end[1] - start[1])

    typeKey(view, "w")
    session.clearSelection()
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    mouse.click(px(tip))
    camera = pomadeCamera.resolve(view)
    loop._placeGizmo(camera)
    axes = axisRecords()
    check(session.readSelection(2) == [(0, tip, -1)] and
          state.transformOrientation == "world" and
          gizmo.frame == pomadeGizmo.IDENTITY_FRAME,
          "GZ-05: the tip's Move gizmo is World-oriented by default")
    check(len(axes) == 3 and
          all(length(record) >= pomadeGizmo.MIN_AXIS_PIXELS * ratio and
              record["grabbable"] for record in axes.values()),
          "three grabbable world axis records >= 12 px long (%r)"
          % [round(length(record), 1) for record in axes.values()])

    # G13: +/=/- resize the manipulator by 10 %; L flips World <-> Tube.
    before = length(axes[pomadeGizmo.HANDLE_U])
    typeKey(view, "+")
    wait(10)
    grown = length(axisRecords()[pomadeGizmo.HANDLE_U])
    check(abs(settings.manipulatorSize - 99.0) < 1e-6 and
          abs(grown / before - 1.1) < 0.02,
          "'+' grows the manipulator 10%% (%g px, %.1f -> %.1f)"
          % (settings.manipulatorSize, before, grown))
    typeKey(view, "-")
    typeKey(view, "=")
    typeKey(view, "-")
    wait(10)
    check(abs(settings.manipulatorSize - 90.0) < 1e-6,
          "'-' and '=' step it back and forth to 90 (%g)"
          % settings.manipulatorSize)
    typeKey(view, "l")
    wait(10)
    check(state.transformOrientation == "tube" and
          abs(gizmo.frame[7] - 1.0) < 1e-3,
          "L flips to the Tube frame, w along the root normal (%r)"
          % (gizmo.frame[6:9],))
    typeKey(view, "l")
    wait(10)
    check(state.transformOrientation == "world" and
          gizmo.frame == pomadeGizmo.IDENTITY_FRAME, "and L flips back")

    # G07: Ctrl on an axis moves in the perpendicular plane, every sample.
    camera = pomadeCamera.resolve(view)
    u = axisRecords()[pomadeGizmo.HANDLE_U]
    mid = ((u["points"][0][0] + u["points"][-1][0]) * 0.5,
           (u["points"][0][1] + u["points"][-1][1]) * 0.5)
    mouse.press(mid)
    check(viewport.gestureActive and gizmo.activeHandle ==
          pomadeGizmo.HANDLE_U, "the press on the U midpoint drags U")
    for step in range(1, 4):
        mouse.move((mid[0] + 10.0 * step, mid[1] + 6.0 * step))
    plain = centers(session)[tip]
    mouse.move((mid[0] + 33.0, mid[1] + 18.0), ("ctrl",))
    ctrl = centers(session)[tip]
    mouse.move((mid[0] + 36.0, mid[1] + 18.0))
    back = centers(session)[tip]
    home = baseline[tip]
    check(plain[0] - home[0] > 1e-3 and abs(plain[1] - home[1]) < 1e-4 and
          abs(plain[2] - home[2]) < 1e-4,
          "a plain U drag moves along world X only (%r)" % (plain,))
    check(abs(ctrl[0] - home[0]) < 1e-3 and
          (abs(ctrl[1] - home[1]) > 1e-3 or abs(ctrl[2] - home[2]) > 1e-3),
          "G07: Ctrl mid-drag moves in the YZ plane, no X (%r)" % (ctrl,))
    check(back[0] - home[0] > 1e-3 and abs(back[1] - home[1]) < 1e-4 and
          abs(back[2] - home[2]) < 1e-4,
          "and letting Ctrl go returns to the axis (%r)" % (back,))

    # G14: undo refuses while the drag is live.
    typeKey(view, "z", ("ctrl",))
    wait(10)
    check(viewport.gestureActive and centers(session)[tip] == back,
          "Ctrl+Z during the drag is refused; the drag goes on")
    if workspace is not None:
        # ...and the dock header's buttons say so (G14): grey mid-drag.
        workspace.refresh()
        undoButton = workspace.button("file", "undo")
        redoButton = workspace.button("file", "redo")
        check(undoButton is not None and redoButton is not None and
              not undoButton.isEnabled() and not redoButton.isEnabled(),
              "G14: the dock's Undo and Redo grey out while the drag is "
              "live (%r, %r)" % (undoButton and undoButton.isEnabled(),
                                 redoButton and redoButton.isEnabled()))
    mouse.release((mid[0] + 36.0, mid[1] + 18.0))
    if workspace is not None:
        wait(10)
        workspace.refresh()
        check(workspace.button("file", "undo").isEnabled(),
              "and Undo comes back on the release")
    dragged = centers(session)
    restored("Ctrl-plane drag")
    typeKey(view, "z", ("ctrl", "shift"))
    wait(10)
    check(centers(session)[tip] == dragged[tip],
          "Ctrl+Shift+Z redoes it (%r)" % (centers(session)[tip],))
    typeKey(view, "z", ("ctrl",))
    wait(10)
    typeKey(view, "z", ("shift",))
    wait(10)
    check(centers(session)[tip] == dragged[tip], "and so does Shift+Z")
    restored("redone drag")

    # G11: J held = step snap, X held = world grid; release un-snaps.
    loop._placeGizmo(pomadeCamera.resolve(view))
    camera = pomadeCamera.resolve(view)
    settings.For("move").stepSize = 0.5
    centre = px(tip)
    viewProj = tuple(camera.viewProj)
    mouse.press(centre)
    for step in range(1, 5):
        mouse.move((centre[0] + 11.0 * step, centre[1] + 3.0 * step))
    raw = [centers(session)[tip][i] - home[i] for i in range(3)]
    holdKey(view, "j", True)
    wait(10)
    stepped = [centers(session)[tip][i] - home[i] for i in range(3)]
    check(viewport.holdActive("stepSnap") and
          all(abs(v / 0.5 - round(v / 0.5)) < 1e-3 for v in stepped) and
          stepped != raw,
          "holding J snaps the live drag to 0.5 steps at once (%r -> %r)"
          % ([round(v, 3) for v in raw], [round(v, 3) for v in stepped]))
    check(tuple(pomadeCamera.resolve(view).viewProj) == viewProj,
          "and usdview's own J (Toggle Framed View) did not fire")
    holdKey(view, "j", False)
    wait(10)
    unsnapped = [centers(session)[tip][i] - home[i] for i in range(3)]
    check(not viewport.holdActive("stepSnap") and
          all(abs(a - b) < 1e-4 for a, b in zip(unsnapped, raw)),
          "releasing J un-snaps at once")
    holdKey(view, "x", True)
    wait(10)
    check(viewport.holdActive("grid") and
          all(abs(v - round(v)) < 1e-3 for v in gizmo.origin),
          "holding X lands the pivot on the world grid (%r)"
          % (gizmo.origin,))
    mouse.release((centre[0] + 44.0, centre[1] + 12.0))
    check(not viewport.holdActive("grid"), "the drag's end clears the hold")
    holdKey(view, "x", False)
    restored("snapped drag")

    # G08: Scale through the pivot mirrors, unless Prevent Negative Scale.
    typeKey(view, "r")
    wait(10)
    rows = {row.id: row for row in pomadePanels.descriptors("tube", state)}

    def scaleThrough():
        cam = pomadeCamera.resolve(view)
        loop._placeGizmo(cam)
        v = axisRecords()[pomadeGizmo.HANDLE_V]
        start, end = v["points"][0], v["points"][-1]
        half = ((start[0] + end[0]) * 0.5, (start[1] + end[1]) * 0.5)
        mirror = (2.0 * start[0] - half[0], 2.0 * start[1] - half[1])
        mouse.press(half)
        grabbed = gizmo.activeHandle
        for step in range(1, 5):
            t = step / 4.0
            mouse.move((half[0] + (mirror[0] - half[0]) * t,
                        half[1] + (mirror[1] - half[1]) * t))
        mouse.release(mirror)
        return grabbed, centers(session)[tip]

    grabbed, flipped = scaleThrough()
    root = baseline[0]
    check(grabbed == pomadeGizmo.HANDLE_V and flipped[1] < root[1] - 1.0,
          "G08: Scale V dragged through the pivot mirrors the tip below "
          "the root (%r)" % (flipped,))
    restored("mirroring scale")
    rows["preventNegativeScale"].set(state, session, True)
    grabbed, clamped = scaleThrough()
    check(grabbed == pomadeGizmo.HANDLE_V and clamped[1] >= root[1] and
          clamped[1] - root[1] < 0.01,
          "with Prevent Negative Scale it stops at the pivot (%r)"
          % (clamped,))
    restored("clamped scale")
    rows["preventNegativeScale"].set(state, session, False)

    # G23: Free Rotate hides the ball.
    typeKey(view, "e")
    wait(10)
    loop._placeGizmo(pomadeCamera.resolve(view))
    kinds = [record["kind"] for record in viewport.gizmoScreenHandles()]
    rows["freeRotate"].set(state, session, False)
    wait(10)
    without = [record["kind"] for record in viewport.gizmoScreenHandles()]
    check("free" in kinds and "free" not in without,
          "G23: Free Rotate off takes the ball away at once (%r -> %r)"
          % (kinds.count("free"), without.count("free")))
    rows["freeRotate"].set(state, session, True)
    typeKey(view, "w")
    settings.For("move").stepSize = 1.0

    pomadeT3.frameScalp(stage, view)
    view.setFocus()
    wait(40)
    session.clearSelection()
    loop._placeGizmo(pomadeCamera.resolve(view))
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)


def toolOf(viewport):
    return getattr(viewport.loop, "transformTool", lambda: None)()


def dockIcons(view, session, viewport, state, workspace):
    """DK-04 / parity G21: icon shelves, the Q/W/E/R row, button hooks."""
    from pxr.Usdviewq.qt import QtWidgets
    from usdGenPomadeTools import (pomadeBridge, pomadeCamera, pomadeDockIds,
                                  pomadeGizmo, pomadeGizmoIcons, pomadeLib,
                                  pomadeModes)
    typeKey(view, "2")
    wait(10)
    workspace.refresh()
    shelves = [("mode", m.id, m.hotkey) for m in pomadeModes.MODES]
    for subs in pomadeDockIds.SUBMODES.values():
        shelves.extend(("sub", s.id, s.hotkey) for s in subs)
    shelves.extend(("comp", s.id, s.hotkey)
                   for s in pomadeModes.TUBE_SUBMODES)
    shelves.extend(("tool", t.id, t.hotkey)
                   for t in pomadeDockIds.TRANSFORM_TOOLS)
    missing, iconless, keyless, misnamed = [], [], [], []
    for kind, itemId, hotkey in shelves:
        btn = workspace.button(kind, itemId)
        if btn is None:
            missing.append((kind, itemId))
            continue
        if btn.icon().isNull():
            iconless.append((kind, itemId))
        if ("(%s)" % hotkey) not in btn.toolTip():
            keyless.append((kind, itemId, btn.toolTip()))
        if btn.objectName() != pomadeDockIds.objectName(kind, itemId):
            misnamed.append((kind, itemId, btn.objectName()))
    check(not missing, "workspace.button() finds every shelf button (%r)"
          % (missing,))
    check(not iconless, "every mode/sub-mode/comp/tool button has an icon "
          "(%r)" % (iconless,))
    check(not keyless, "every shelf tooltip names its hotkey (%r)"
          % (keyless[:3],))
    check(not misnamed, "every shelf button carries its pomade objectName "
          "(%r)" % (misnamed[:3],))
    names = [b.objectName() for b in
             workspace.findChildren(QtWidgets.QAbstractButton)
             if pomadeDockIds.parseObjectName(b.objectName()) is not None]
    check(len(names) == len(set(names)),
          "no two dock buttons share an objectName (%d named)" % len(names))
    for fileId, keys in (("undo", ("Ctrl+Z",)),
                         ("redo", ("Ctrl+Y", "Ctrl+Shift+Z", "Shift+Z"))):
        btn = workspace.button("file", fileId)
        check(btn is not None and all(k in btn.toolTip() for k in keys) and
              not btn.icon().isNull(),
              "the header's %s button has its glyph and names %s (%r)"
              % (fileId, ", ".join(keys), btn and btn.toolTip()))
    check(workspace.button("file", "settings") is not None,
          "the header has a Settings button")
    check(all(pomadeGizmoIcons.HasArt(n) for n in
              ("select", "move", "rotate", "scale", "undo", "redo",
               "settings")),
          "pomadeGizmoIcons finds the usdRig-family art for the row glyphs")
    check("transformTool" in workspace.parameterIds() and
          "transformTool" not in [d.id for d, _w in workspace._paramWidgets],
          "Tube's transform tool is the icon row, not a form combo")
    check(not workspace._transformRow.isHidden(),
          "the transform row shows in Tube")

    # A whole tube selected, so the row's clicks have a gizmo to change.
    typeKey(view, "f8")
    wait(10)
    tubes = pomadeBridge.readTubeIds(session.dll, session.model)
    tube = int(tubes[-1]) if tubes else 0
    session.clearSelection()
    session.select(pomadeLib.POMADE_PICK_TUBE_VERT, [tube], [-1], [-1])
    camera = pomadeCamera.resolve(view)
    viewport.loop._placeGizmo(camera)
    rotate = workspace.button("tool", "rotate")
    rotate.click()
    wait(10)
    gizmo = viewport.loop._gizmo
    check(state.transformTool == "rotate" and gizmo.visible and
          gizmo.kind == pomadeGizmo.GIZMO_ROTATE and rotate.isChecked(),
          "clicking the Rotate button raises the Rotate gizmo (%r, kind %r)"
          % (state.transformTool, gizmo.kind))
    typeKey(view, "w")
    wait(10)
    workspace.refresh()
    move = workspace.button("tool", "move")
    check(state.transformTool == "move" and move.isChecked() and
          not rotate.isChecked(),
          "W checks the Move button (%r)" % state.transformTool)

    # Parity G21/G16/G17: the Global/Local and group-pivot toggles.
    orientBtn = workspace.button("gizmo", "orientation")
    pivotBtn = workspace.button("gizmo", "groupPivot")
    check(orientBtn is not None and pivotBtn is not None and
          orientBtn.objectName() ==
          pomadeDockIds.objectName("gizmo", "orientation") and
          "(L)" in orientBtn.toolTip() and "(P)" in pivotBtn.toolTip() and
          not orientBtn.icon().isNull() and not pivotBtn.icon().isNull(),
          "the transform row carries the L and P toggles with glyphs")
    if orientBtn is not None and pivotBtn is not None:
        state.transformOrientation = "world"
        workspace.refresh()
        check(orientBtn.isEnabled() and not orientBtn.isChecked() and
              not pivotBtn.isEnabled(),
              "under Move: Global shown, group pivot greyed (no choice)")
        orientBtn.click()
        wait(10)
        check(state.transformOrientation == "tube" and
              orientBtn.isChecked() and orientBtn.text() == "Local" and
              gizmo.frame != pomadeGizmo.IDENTITY_FRAME,
              "clicking the orientation toggle flips Move to Local and "
              "re-orients the gizmo (%r, %r)"
              % (state.transformOrientation, orientBtn.text()))
        orientBtn.click()
        wait(10)
        check(state.transformOrientation == "world" and
              not orientBtn.isChecked() and
              gizmo.frame == pomadeGizmo.IDENTITY_FRAME,
              "and back to Global")
        typeKey(view, "e")
        wait(10)
        workspace.refresh()
        settings = state.gizmoSettings
        pivotWas = settings.For("rotate").groupPivot
        if pivotWas != "individual":
            settings.For("rotate").groupPivot = "individual"
        workspace.refresh()
        viewport.loop._placeGizmo(pomadeCamera.resolve(view))
        eachOrigin = tuple(gizmo.origin)
        check(pivotBtn.isEnabled() and not pivotBtn.isChecked(),
              "under Rotate the pivot toggle is live, on Individual")
        typeKey(view, "p")
        wait(10)
        workspace.refresh()
        bounds = viewport.loop._selectionBounds()
        middle = tuple(0.5 * (bounds[0][i] + bounds[1][i])
                       for i in range(3)) if bounds else None
        check(settings.For("rotate").groupPivot == "centre" and
              pivotBtn.isChecked() and middle is not None and
              all(abs(gizmo.origin[i] - middle[i]) < 1e-4
                  for i in range(3)),
              "P switches Rotate to Selection Centre and the gizmo moves "
              "to the selection's middle (%r vs %r)"
              % (tuple(gizmo.origin), middle))
        pivotBtn.click()
        wait(10)
        check(settings.For("rotate").groupPivot == "individual" and
              not pivotBtn.isChecked() and
              all(abs(gizmo.origin[i] - eachOrigin[i]) < 1e-4
                  for i in range(3)),
              "clicking the pivot toggle cycles back to Individual, the "
              "gizmo back on the root (%r)" % (tuple(gizmo.origin),))
        typeKey(view, "w")
        wait(10)

    # Hierarchy: the row waits for a tube, then jumps like its W/E/R keys.
    typeKey(view, "4")
    wait(10)
    session.clearSelection()
    workspace.refresh()
    scale = workspace.button("tool", "scale")
    check(not workspace._transformRow.isHidden() and not scale.isEnabled(),
          "in Hierarchy the transform row waits for a selected tube")
    session.select(pomadeLib.POMADE_PICK_TUBE_VERT, [tube], [-1], [-1])
    workspace.refresh()
    check(scale.isEnabled(), "a selected tube enables the Hierarchy row")
    scale.click()
    wait(10)
    check(state.activeMode == "tube" and state.transformTool == "scale" and
          session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) ==
          [(tube, -1, -1)],
          "Hierarchy's Scale button jumps to Tube/Scale with the tube "
          "(%r, %r, %r)" % (state.activeMode, state.transformTool,
                            session.readSelection(
                                pomadeLib.POMADE_PICK_TUBE_VERT)))
    typeKey(view, "w")
    wait(10)


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
        import usdGenPomadeTools
        from usdGenPomadeTools import (pomadeBridge, pomadeCamera, pomadeGizmo,
                                      pomadeHierarchy, pomadeLib, pomadeModes,
                                      pomadePanels)
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

    check(frameScalp(stage, view), "the scene camera looks down at the scalp")
    view.setFocus()
    wait(50)

    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenPomadeTools.openWorkspace").run()
    messages = []
    registry.getCommandPlugin("usdGenPomadeTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.pomadeState
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

    def clickControl(actionId):
        """Invoke a dock action through its Qt button (DK-04 hook)."""
        button = workspace.button("action", actionId)
        if button is None:
            return False
        button.click()
        wait(15)
        return True

    # The region tint mesh is coincident with /Scalp, so leaving both on
    # would z-fight the pixel probes below. The model copied the scalp at
    # bind time, so the mouse still hits it through K1.
    stage.GetPrimAtPath("/Scalp").SetActive(False)

    camera = pomadeCamera.resolve(view)
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
    # FB-02: a Graph point tool shows a cross, and the HUD names the tool.
    from pxr.Usdviewq.qt import QtCore
    shapes = QtCore.Qt.CursorShape
    check(view.cursor().shape() == shapes.CrossCursor,
          "FB-02: Graph Draw shows the cross cursor (%r)"
          % (view.cursor().shape(),))
    hud = viewport._hudOverlay
    check(hud is not None and hud.isVisible() and "Graph" in hud.title and
          "Draw" in hud.title and hud.hint,
          "FB-02: the viewport HUD reads the Graph tool and its hint (%r)"
          % (hud.text() if hud is not None else None,))
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
    cvCount = int(session.dll.Pomade_GetCenterCVCount(session.model))
    check(cvCount == STUB_CVS,
          "G14: the closed region got a tube stub on the graph release "
          "(%d center CVs, status %r)" % (cvCount, messages[-2:]))
    check(int(session.dll.Pomade_GetTubeRegionId(session.model)) == 0,
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
    check(bool(stage.GetPrimAtPath("/PomadeGroom")),
          "the committed groom reached the stage")

    # -- Tube mode ---------------------------------------------------------
    typeKey(view, "2")
    check(state.activeMode == "tube", "the 2 hotkey selects Tube mode (%r)"
          % state.activeMode)
    check(viewport.loop is not None and viewport.loop.modeId == "tube",
          "and the controller built the Tube loop")
    check(state.tubeSubMode == "center",
          "which opens in the Center sub-mode (%r)" % state.tubeSubMode)
    hudText = viewport._hudOverlay.text() \
        if viewport._hudOverlay is not None else ""
    check("Tube" in hudText and "Center" in hudText and
          viewport._hudOverlay.hint == pomadeModes.hintFor("tube", "center"),
          "FB-02: after 2 the HUD reads Tube > Center and its hint (%r)"
          % hudText)

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
    selected = session.readSelection(2)          # PomadePick_CenterCV
    check(selected and selected[0][1] == cvCount - 1,
          "one click selected the tip center CV (%r)" % (selected,))
    kind, origin = gizmoKind(session, viewport)
    nativeKind, _ = gizmoKind(session)
    overlay = getattr(viewport, "_gizmoOverlay", None)
    check(kind == pomadeGizmo.GIZMO_TRANSLATE and nativeKind == pomadeGizmo.GIZMO_NONE
          and overlay is not None and overlay.isVisible(),
          "the Qt translate overlay is live while the native fallback is suppressed")
    check(origin is not None and abs(origin[1] - tipHandle[1]) < 1e-3,
          "at the selected core handle (%r vs %r)" % (origin, tipHandle))
    whiteAfter = whiteFraction(view, camera, tipHandle)
    info("white at the tip CV after selecting: %.2f" % whiteAfter)
    check(whiteAfter > whiteBefore + 0.05,
          "and the published CV dot turned white: %.2f -> %.2f"
          % (whiteBefore, whiteAfter))

    # GZ-08: picking the current mode again is a no-op -- the loop, its
    # gizmo and the Qt overlay all survive a redundant `2`.
    loopBefore = viewport.loop
    typeKey(view, "2")
    check(viewport.loop is loopBefore and state.activeMode == "tube",
          "GZ-08: a redundant 2 keeps the live Tube loop")
    check(overlay is not None and overlay.isVisible() and
          viewport.loop._gizmo.visible,
          "GZ-08: and its gizmo and overlay stay up")

    # -- GZ-02: cone/cube tips, the painted overlay, the rotate readout ----
    gizmoLook(view, session, viewport, mouse, messages)

    # Qt sends MouseButtonDblClick instead of the second press.  It is not
    # hierarchy navigation in Tube mode (GZ-08): a double-click on a centre
    # CV grows the selection to its whole tube, keeps the focused level and
    # leaves no gesture behind, however often it repeats.
    focusBeforeDouble = state.activeLevel
    mouse.direct = True
    for _click in range(3):
        mouse.doubleClick(tipPixel)
    wholeTube = session.readSelection(1)         # PomadePick_TubeVert
    check(state.activeLevel == focusBeforeDouble and
          wholeTube == [(0, -1, -1)] and
          not session.readSelection(2) and
          not viewport.gestureActive,
          "GZ-08: Tube double-clicks on a CV select its whole tube and keep "
          "focus %r" % ({"level": state.activeLevel, "tubes": wholeTube,
                         "gesture": viewport.gestureActive},))
    check(viewport.loop._gizmo.visible and overlay.isVisible(),
          "GZ-08: the gizmo re-placed on the whole tube")
    # Back to the tip CV the steps below drag, through the dock's route
    # (a programmatic selection followed by refreshGizmo).
    session.clearSelection(1)
    session.select(2, [0], [cvCount - 1], [-1], 0)
    viewport.refreshGizmo()
    check(session.readSelection(2) == [(0, cvCount - 1, -1)] and
          viewport.loop._gizmo.visible and
          abs(viewport.loop._gizmo.origin[1] - tipHandle[1]) < 1e-3,
          "GZ-08: refreshGizmo puts the gizmo back on the tip CV (%r)"
          % (tuple(viewport.loop._gizmo.origin),))

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
    originBeforeDrag = tuple(viewport.loop._gizmo.origin)
    mark = len(messages)
    mouse.press(tipPixel)
    check(viewport.gestureActive, "the press took the gizmo's free handle")
    for step in range(1, 7):
        mouse.move((tipPixel[0] + travelPx * step / 6.0, tipPixel[1]))
        if step == 2:
            # GZ-07: the live readout, on the status line and in a label
            # the Qt overlay paints beside the gizmo centre.
            overlay = viewport._gizmoOverlay
            overlay.repaint()
            wait(5)
            readout = viewport.loop.dragReadout()
            check(readout.startswith("Move"),
                  "GZ-07: the drag has a live readout (%r)" % readout)
            check(overlay.isVisible() and
                  getattr(overlay, "readoutText", "") == readout,
                  "GZ-07: the overlay painted it as a label (%r)"
                  % getattr(overlay, "readoutText", None))
            check(any("Move" in str(line) for line in messages[mark:]),
                  "GZ-07: a status recorded mid-drag reads Move (%r)"
                  % messages[-2:])
        if step == 3:
            # GZ-08: a wheel dolly mid-drag would move the camera the drag
            # was projected with; the controller swallows it, so neither
            # the overlay nor the camera changes and the CV delta below
            # still matches the cursor travel.
            recordsBefore = viewport.gizmoScreenHandles()
            viewBefore = pomadeCamera.resolve(view).viewProj
            check(wheel(view, (tipPixel[0] + travelPx * 0.5, tipPixel[1])),
                  "GZ-08: a QWheelEvent reached the view mid-drag")
            wait(10)
            check(viewport.gizmoScreenHandles() == recordsBefore and
                  pomadeCamera.resolve(view).viewProj == viewBefore,
                  "GZ-08: the wheel neither dollied nor moved the overlay")
    mouse.release(target)
    originAfterDrag = tuple(viewport.loop._gizmo.origin)
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
    # GZ-08: the gizmo follows each step at once -- no wait, no mouse event.
    def originNear(want):
        got = viewport.loop._gizmo.origin
        return max(abs(got[i] - want[i]) for i in range(3)) < 1e-4
    typeKey(view, "z", ("ctrl",))
    check(originNear(originAfterDrag),
          "GZ-08: the first Ctrl+Z puts the gizmo back where the 60 px drag "
          "left it (%r vs %r)" % (tuple(viewport.loop._gizmo.origin),
                                  originAfterDrag))
    typeKey(view, "z", ("ctrl",))
    check(originNear(originBeforeDrag),
          "GZ-08: the second Ctrl+Z puts it on the pre-drag tip (%r vs %r)"
          % (tuple(viewport.loop._gizmo.origin), originBeforeDrag))
    restored = centers(session)
    check(abs(restored[cvCount - 1][0] - before[cvCount - 1][0]) < 1e-4,
          "two Ctrl+Z (one per drag) put the tip CV back (%r vs %r)"
          % (restored[cvCount - 1], before[cvCount - 1]))

    # -- GZ-01/GZ-03/G03/G04: handle priority, hover, tweak, repeat ---------
    gizmoParity(stage, view, session, viewport, mouse)

    # -- GZ-05 / G07/G08/G11/G13/G14/G23: orientation, keys, snaps ---------
    gizmoSettingsParity(stage, view, session, viewport, state, mouse,
                        messages, workspace)

    # -- Fill --------------------------------------------------------------
    typeKey(view, "3")
    check(state.activeMode == "fill" and viewport.loop is not None and
          viewport.loop.modeId == "fill",
          "the 3 hotkey selects Fill mode and its loop (%r)"
          % state.activeMode)
    session.select(1, [0], None, None, 0)        # the tube, for the panel
    descriptors = {d.id: d for d in pomadePanels.descriptors("fill", state)}
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

    # SS-02 (walkthrough step 12): a panel density edit is its own undo
    # step. Ctrl+Z used to skip it and restore a snapshot from before the
    # guides were grown -- 38 guides to 0 -- leaving the density at 32.
    check(session.status()["undoLabel"] == "Fill params",
          "the density edit is a 'Fill params' undo step (%r)"
          % session.status()["undoLabel"])
    typeKey(view, "z", ("ctrl",))
    wait(10)
    check(int(round(descriptors["density"].get(state, session))) == 8 and
          guideCount(session) == sparse,
          "Ctrl+Z restores the density and its guides (%g, %d guides, "
          "want 8 and %d)" % (descriptors["density"].get(state, session),
                              guideCount(session), sparse))
    typeKey(view, "y", ("ctrl",))
    wait(10)
    check(int(round(descriptors["density"].get(state, session))) == 32 and
          guideCount(session) == dense,
          "Ctrl+Y re-applies it (%g, %d guides, want 32 and %d)"
          % (descriptors["density"].get(state, session), guideCount(session),
             dense))

    check(session.enqueueCommit() and pumpUntilCommitted(viewport, session),
          "the idle pump drains the committer after the fill")
    check(bool(stage.GetPrimAtPath("/PomadeGroom/Guides")),
          "and the committed guides reached the stage")

    # -- Ring scale and Section CV editing on a non-root tube -------------
    # Feed selection through the same live session the shelf uses, then
    # drive the visible gizmos with real StageView mouse events.  A child is
    # deliberate: root-only tests cannot catch an operation that silently
    # falls back to tube 0.
    # Select the visible root and subdivide through the Hierarchy dock.  This
    # exercises the same active-cut expansion path as an artist; directly
    # creating children leaves the parent frontier collapsed.
    workspace.button("mode", "hierarchy").click()
    wait(15)
    rootPoint = sectionCV(session, 0, 1, 0)
    rootPixel = pixel(*rootPoint) if rootPoint is not None else None
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
    check(bool(children), "the root subdivides for non-root Tube editing")
    if children:
        entered = clickControl("enterLevel")
        check(entered and int(state.activeLevel) >= 2,
              "the Hierarchy dock enters the created child level")
        child = int(children[0])
        ring = 1
        beforeRing = pomadeBridge.tubeSection(session.dll, session.model,
                                             child, ring)
        # This is the artist workflow: enter the descendant level before
        # selecting its rings, so L2 is the focused editable display.
        workspace.button("mode", "tube").click()
        wait(15)
        if workspace.button("comp", "ring") is not None:
            workspace.button("comp", "ring").click()
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
        tubeRows = {row.id: row for row in pomadePanels.descriptors("tube",
                                                                     state)}
        previewBeforeScale = guidePreview(session)
        numericScale = beforeRing[2] * 1.25
        tubeRows["uniformScale"].set(state, session, numericScale)
        numericRing = pomadeBridge.tubeSection(session.dll, session.model,
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
              ringGizmo.kind == pomadeGizmo.GIZMO_SCALE and scaleCenters,
              "R raises the explicit Scale gizmo on the focused child ring")
        if scaleCenters:
            ringPixel = scaleCenters[0]["points"][0]
            target = (ringPixel[0] + 32.0, ringPixel[1])
            check(ringGizmo.handleAt(camera, ringPixel[0], ringPixel[1]) ==
                  pomadeGizmo.HANDLE_CENTER,
                  "the Scale gizmo centre accepts uniform child-ring scale")
            mouse.press(ringPixel)
            check(viewport.gestureActive and ringGizmo.dragging and
                  ringGizmo.activeHandle == pomadeGizmo.HANDLE_CENTER,
                  "the real press begins a child Scale gesture")
            mouse.move(target)
            mouse.release(target)
            afterRing = pomadeBridge.tubeSection(session.dll, session.model,
                                                child, ring)
            guidesAfterRing = guideCount(session)
            factor = sectionUniformFactor(beforeRing, afterRing)
            check(factor is not None and factor > 1.01 and guidesAfterRing > 0,
                  "a real Scale drag uniformly scales child T%d "
                  "and retains its guide preview (factor %.3f, %d guides)"
                  % (child, factor or 0.0, guidesAfterRing))

        beforeSection = pomadeBridge.tubeSection(session.dll, session.model,
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
              and cvGizmo.visible and kind == pomadeGizmo.GIZMO_TRANSLATE and
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
            afterSection = pomadeBridge.tubeSection(session.dll, session.model,
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

    # -- DK-02: hotkey truth and the dock's instruction/status lines ------
    # The shelf used to print C/R/E for Tube sub-modes while Q/W/E/R were
    # eaten by the transform tool; these checks hold the keys the dock
    # advertises to what the keys actually do, through real key events.
    view.setFocus()
    viewport.setPointerInside(True)
    wait(15)

    def dockLines():
        workspace.refresh()
        return (workspace._instructionLabel.text(),
                workspace._toolStatusLabel.text())

    typeKey(view, "2")
    typeKey(view, "f9")
    wait(10)
    check(state.activeMode == "tube" and state.tubeSubMode == "center",
          "F9 picks the Center CV component (%r/%r)"
          % (state.activeMode, state.tubeSubMode))
    subBefore = state.tubeSubMode
    typeKey(view, "r")
    wait(10)
    check(state.transformTool == "scale" and state.tubeSubMode == subBefore,
          "R is the Scale tool and leaves the component kind alone "
          "(%r, %r)" % (state.transformTool, state.tubeSubMode))
    typeKey(view, "f10")
    wait(10)
    check(state.tubeSubMode == "ring",
          "the F10 key picks the Ring component (%r)" % state.tubeSubMode)
    # DK-04: the label is the component's name; the key is in the tooltip.
    workspace.refresh()
    ringButton = workspace.button("comp", "ring")
    check(ringButton is not None and ringButton.text() == "Ring" and
          "(F10)" in ringButton.toolTip() and ringButton.isChecked(),
          "the dock's Ring button is checked and tooltips F10 (%r)"
          % (ringButton.toolTip() if ringButton is not None else None,))

    # Hierarchy W jumps to Tube/Move and says so in the status line.
    statusLines = []
    printStatus = viewport._status
    viewport._status = lambda text: (statusLines.append(text),
                                      printStatus(text))
    typeKey(view, "4")
    typeKey(view, "w")
    wait(10)
    viewport._status = printStatus
    check(state.activeMode == "tube" and state.transformTool == "move" and
          any("Pomade Tube: Move (from Hierarchy)" in line
              for line in statusLines),
          "Hierarchy W jumps to Tube/Move and reports it (%r)"
          % (statusLines[-3:],))

    instructions = {}
    for key in ("1", "2", "3", "4", "6", "5"):
        typeKey(view, key)
        wait(10)
        instructions[key] = dockLines()[0]
    check(all(text.strip() for text in instructions.values()),
          "the dock shows an instruction line for every mode key 1-6 (%r)"
          % ([k for k, t in instructions.items() if not t.strip()],))
    tubeLine = instructions["2"]
    check("Q/W/E/R" in tubeLine and "F8-F11" in tubeLine and
          "Shift toggles" in tubeLine,
          "Tube's line names its real keys and the selection modifiers")

    # "5" was last, so Sculpt is active: its tool line is the loop's own.
    sculptStatus = dockLines()[1]
    check("Sculpt" in sculptStatus,
          "the dock's status line reads the Sculpt loop after 5 (%r)"
          % sculptStatus)
    typeKey(view, "g")
    wait(10)
    grabLine = dockLines()[0]
    typeKey(view, "s")
    wait(10)
    smoothLine = dockLines()[0]
    check(state.sculptSubMode == "smooth" and grabLine != smoothLine and
          grabLine.startswith("Grab") and smoothLine.startswith("Smooth"),
          "the instruction line follows the Sculpt brush (%r -> %r)"
          % (grabLine, smoothLine))
    typeKey(view, "1")
    typeKey(view, "d")
    wait(10)
    drawLine = dockLines()[0]
    typeKey(view, "p")
    wait(10)
    placeLine = dockLines()[0]
    check(state.graphSubMode == "place" and drawLine != placeLine,
          "the instruction line follows the Graph tool (%r -> %r)"
          % (drawLine, placeLine))
    typeKey(view, "escape")
    wait(10)

    dockIcons(view, session, viewport, state, workspace)

    # -- tear down cleanly -------------------------------------------------
    viewport.uninstall()
    session.deactivate()
    print("testUsdviewPomadeTube: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeTube needs testusdview (no live view)")
    sys.exit(0)
