# testUsdviewBrushSetup -- T3: description setup through REAL mouse events.
#
#   testusdview --testScript plugin/usdGenTools/testenv/testUsdviewBrushSetup.py \
#                 examples/test-plane.usda
#
# Proven here, all of it through the installed palette and viewport filter:
#
#   * setup: the palette's Setup description reads usdview's prim
#     selection (/Plane) and defines a ready-to-cook UsdGenDescription
#     with its Scatter -> Grow -> Clump -> Curl -> Width chain, and
#     Paint-to binds the active preset's PaintMap under the Maps scope;
#   * rings: hovering the bound surface shows the cursor ring where the
#     CPU raycast says the dab lands, sized by the picked face, with the
#     inner (hardness) ring at hardness * radius; leaving the view hides it;
#   * hotkeys: F over the bound surface drag-adjusts the radius (Enter
#     confirms, Esc cancels), [ steps it, and F is left to usdview's Frame
#     Selected while strokes are disarmed;
#   * eye: the palette's preview toggle hides and re-shows the map overlay;
#   * mask row: flipping the mask-preset buttons rebinds the map, primvar
#     and grow wiring and redraws the newly bound map (and back);
#   * stroke + bake: a press-drag-release paints the Maps primvar into
#     the edit target with the live map overlay, like the bare bind;
#   * silence: the run emits no operatorOrder / operator-node / commit
#     engine errors (the CTest FAIL_REGULAR_EXPRESSION asserts that).
import os
import sys

failures = 0


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
    the path it was given is on the command line.
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


def shownColors(stage, binding, state):
    """The preview colours on `binding`'s surface, on either preview path.

    Session path (no live render chain): the composed displayColor.
    Overlay path (brushPreview pushed to the Hydra preview index, which is
    write-only from Python): brushPreview.ShownColors when the backend
    records what it pushed, else the colours SetPreview computes for the
    grid it shows -- the live gesture's working grid mid-stroke, the baked
    map otherwise."""
    from usdGenTools import brushAuthor, brushPreview
    if binding is None:
        return None
    getter = getattr(brushPreview, "ShownColors", None)
    if getter is not None:
        return getter(stage, binding)
    overlaid = getattr(brushPreview, "_OVERLAID", ())
    if str(binding.surfacePath) not in overlaid:
        return brushPreview.PreviewColors(stage, binding)
    gesture = getattr(state, "gesture", None)
    if gesture is not None:
        grid = gesture.working
    else:
        grid, _error = brushAuthor.BaseGridFromStage(stage, binding)
    if grid is None:
        return None
    rgb = brushPreview._gridColors(grid, state.colorMap, state.valueRange)
    if rgb is None:
        return None
    return [tuple(float(x) for x in row) for row in rgb.tolist()]


def wait(ms=30):
    _qtTest().QTest.qWait(int(ms))


def _qtWidgets():
    from pxr.Usdviewq.qt import QtWidgets
    return QtWidgets


def typeKey(view, key):
    """A real key click on the view: ShortcutOverride, then KeyPress."""
    _qtTest().QTest.keyClick(view, key)


def shortcutOverride(key):
    """An unaccepted ShortcutOverride for `key`, as Qt's shortcut map sends."""
    from pxr.Usdviewq.qt import QtCore, QtGui
    event = QtGui.QKeyEvent(QtCore.QEvent.Type.ShortcutOverride, key,
                            QtCore.Qt.KeyboardModifier.NoModifier)
    event.ignore()
    return event


class Mouse:
    """QtTest mouse events on the StageView, in physical pixels.

    The controller reads physical pixels while QtTest takes logical widget
    coordinates, so everything converts once, here. Moves during a drag go
    straight at the widget: QTest.mouseMove reports NoButton even after a
    press in this Qt build, and a drag must carry LeftButton (the
    testUsdviewBrushStroke.Mouse arrangement).
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
        self.direct = False
        self._leftHeld = False

    def _point(self, physical):
        return self._QtCore.QPoint(int(round(physical[0] / self._ratio)),
                                   int(round(physical[1] / self._ratio)))

    def _send(self, kind, physical, button, buttons):
        QtCore = self._QtCore
        point = self._point(physical)
        local = QtCore.QPointF(point)
        globalPos = QtCore.QPointF(self._view.mapToGlobal(point))
        event = self._QtGui.QMouseEvent(
            kind, local, globalPos, button, buttons,
            QtCore.Qt.KeyboardModifier.NoModifier)
        self._QtWidgets.QApplication.sendEvent(self._view, event)

    def press(self, physical):
        QtCore = self._QtCore
        button = QtCore.Qt.MouseButton.LeftButton
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonPress, physical,
                       button, button)
        else:
            _qtTest().QTest.mousePress(
                self._view, button,
                QtCore.Qt.KeyboardModifier.NoModifier,
                self._point(physical))
        self._leftHeld = True

    def move(self, physical):
        QtCore = self._QtCore
        if self._leftHeld:
            self._send(QtCore.QEvent.Type.MouseMove, physical,
                       QtCore.Qt.MouseButton.NoButton,
                       QtCore.Qt.MouseButton.LeftButton)
        elif self.direct:
            self._send(QtCore.QEvent.Type.MouseMove, physical,
                       QtCore.Qt.MouseButton.NoButton,
                       QtCore.Qt.MouseButton.NoButton)
        else:
            _qtTest().QTest.mouseMove(self._view, self._point(physical))

    def release(self, physical):
        QtCore = self._QtCore
        button = QtCore.Qt.MouseButton.LeftButton
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonRelease, physical,
                       button, QtCore.Qt.MouseButton.NoButton)
        else:
            _qtTest().QTest.mouseRelease(
                self._view, button,
                QtCore.Qt.KeyboardModifier.NoModifier,
                self._point(physical))
        self._leftHeld = False


def aimCamera(stage, view, eye, target):
    """A scene camera at `eye` looking at `target`, +Y up, activated."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/BrushSetupCamera"))
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
        "/BrushSetupCamera")
    return view.getActiveSceneCamera() is not None


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

def run(appController):
    global failures
    try:
        import usdGenTools
        from usdGenTools import (brushAuthor, brushCamera, brushPanels,
                                 brushPick, brushPreview, brushState)
    except ImportError as exc:
        print("FAIL: cannot import usdGenTools: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    if view is None or registry is None:
        print("FAIL: no stage view / plugin registry")
        return 1
    container = usdGenTools.container()
    check(container is not None,
          "the usdGenTools plugin container registered")
    if container is None:
        return 1

    # Straight down at the plane: every pick is unambiguous.
    check(aimCamera(stage, view, (0.0, 8.0, 0.0001), (0.0, 0.0, 0.0)),
          "the scene camera looks down at the plane")
    view.setFocus()
    # Hovers warp the real cursor, so they land outside an inactive or
    # occluded window (button drags grab past that); front the window.
    view.window().raise_()
    view.window().activateWindow()
    wait(50)

    # -- palette --------------------------------------------------------
    registry.getCommandPlugin("usdGenTools.showBrushPalette").run()
    wait(50)
    palette = container.brushPalette
    check(palette is not None, "the Brush Palette command opens the dock")
    if palette is None:
        return 1
    state = container.brushState
    loop = container.brushLoop
    check(isinstance(state, brushState.BrushToolState),
          "the container owns the brush state")
    check(loop.state is state, "and the gesture loop over it")

    # -- setup ----------------------------------------------------------
    dataModel.selection.setPrimPath("/Plane")
    check(palette.setupDescription(),
          "Setup description reads the /Plane selection")
    descPath = state.activeDescription
    check(descPath.startswith("/Groom/Description"),
          "the new description is the paint target (%s)" % descPath)
    desc = stage.GetPrimAtPath(descPath)
    check(desc.IsValid() and desc.GetTypeName() == "UsdGenDescription",
          "setup defined a UsdGenDescription at %s" % descPath)
    grow = stage.GetPrimAtPath(descPath + "/Ops/grow")
    scatter = stage.GetPrimAtPath(descPath + "/Ops/scatter")
    clump = stage.GetPrimAtPath(descPath + "/Ops/clump")
    curl = stage.GetPrimAtPath(descPath + "/Ops/curl")
    width = stage.GetPrimAtPath(descPath + "/Ops/width")
    check(grow.IsValid() and grow.GetTypeName() == "UsdGenGrow"
          and scatter.IsValid() and scatter.GetTypeName() == "UsdGenScatter"
          and clump.IsValid() and clump.GetTypeName() == "UsdGenClump"
          and curl.IsValid() and curl.GetTypeName() == "UsdGenCurl"
          and width.IsValid() and width.GetTypeName() == "UsdGenWidth",
          "with the Scatter -> Grow -> Clump -> Curl -> Width chain")
    check([c.GetName() for c in stage.GetPrimAtPath(
        descPath + "/Ops").GetChildren()]
        == ["width", "curl", "clump", "grow", "scatter"],
        "ordered terminal-first, so bottom-up is the pipeline")
    binding = state.binding
    check(binding is not None and str(binding.surfacePath) == "/Plane",
          "and the loop is bound to /Plane")
    paintMap = stage.GetPrimAtPath(binding.mapPath)
    check(paintMap.GetTypeName() == "UsdGenPaintMap"
          and str(binding.mapPath).startswith(descPath + "/Maps/"),
          "through the preset's PaintMap at %s" % binding.mapPath)
    viewport = container.brushViewport
    check(viewport is not None and viewport.installed,
          "setup installs the viewport controller")
    if binding is None or viewport is None:
        return 1
    check(palette.paintToDescription(),
          "Paint-to re-selects the one description without a dialog")
    check(state.activeDescription == descPath,
          "and the paint target is unchanged")
    state.maskPreset = "length"
    check(palette.paintToDescription(),
          "paint-to binds the length preset without a dialog either")
    expr = stage.GetPrimAtPath(descPath + "/Expressions/lengthScale")
    check(expr.IsValid() and expr.GetTypeName() == "UsdGenExpression",
          "length paint-to wires a live expression")
    check(list(grow.GetAttribute("usdGen:length").GetConnections())
          == [expr.GetPath()],
          "and grow length connects to it")
    check(grow.GetAttribute("usdGen:length").GetCustomDataByKey(
        "usdGen:evaluation") == "primitive",
        "evaluating at primitive domain, where ptex() is legal")
    state.maskPreset = "density"
    check(palette.paintToDescription(),
          "painting density again needs no dialog either")

    state.strength = 1.0
    state.radiusWorld = 0.1
    state.value = 0.0  # density's unpainted default is 1.0: paint the gap

    camera = brushCamera.resolve(view)
    check(camera is not None and camera.invertible,
          "the StageView camera resolves")
    if camera is None:
        return 1

    def pixelOf(point):
        projected = camera.worldToPixels(point)
        return (projected[0], projected[1])

    mouse = Mouse(view)
    hoverAt = pixelOf((0.2, 0.0, 0.2))
    pressAt = pixelOf((0.55, 0.0, 0.55))
    moveAt = pixelOf((0.95, 0.0, 0.55))
    info("hover %.1f,%.1f press %.1f,%.1f move %.1f,%.1f (physical)"
         % (hoverAt[0], hoverAt[1], pressAt[0], pressAt[1],
            moveAt[0], moveAt[1]))

    # -- ring -------------------------------------------------------------
    # The disarmed hover primes from ELSEWHERE: a QTest warp to the
    # cursor's current spot delivers no move, and a previous run leaves
    # the cursor at the armed target -- so the armed hover below must
    # always travel. Either way this one reads None (no event, or the
    # disarmed clear).
    check(view.hasMouseTracking(),
          "install enables mouse tracking for real hovers")
    # Hovers go direct: a QTest warp needs window-system hit-testing,
    # which an inactive or occluded window fails, while a direct send
    # is the same QMouseEvent through the same filter either way.
    # Presses and drags stay on QTest (grab-routed, occlusion-proof).
    mouse.direct = True
    state.strokesArmed = False
    mouse.move(moveAt)
    wait(50)
    check(viewport.overlayRing() is None,
          "no ring while strokes are disarmed")
    state.strokesArmed = True
    mouse.move(hoverAt)
    wait(50)
    ring = viewport.overlayRing()
    check(ring is not None, "hovering the bound surface shows the ring")
    if ring is None:
        return 1
    surface = stage.GetPrimAtPath("/Plane")
    snapshot, error = brushPick.snapshotMesh(surface)
    check(snapshot is not None, "the hover snapshot reads (%s)" % error)
    expected = brushPick.ringPixels(snapshot, camera, hoverAt[0], hoverAt[1],
                                    state.radiusWorld)
    check(expected is not None, "the hover pick hits the plane")
    ratio = max(float(view.devicePixelRatioF()), 1.0)
    logical = (hoverAt[0] / ratio, hoverAt[1] / ratio)
    check(abs(ring[0] - logical[0]) < 2.0
          and abs(ring[1] - logical[1]) < 2.0,
          "the ring centres on the cursor (%.1f, %.1f)" % (ring[0], ring[1]))
    check(abs(ring[2] - expected[2] / ratio) < 1.5,
          "sized by the picked face (%.1f px, want %.1f)"
          % (ring[2], expected[2] / ratio))
    rings = viewport.overlayRings()
    check(rings is not None
          and abs(rings[3] - rings[2] * state.hardness) < 0.5,
          "the inner ring is hardness * radius (%.1f of %.1f px)"
          % (rings[3] if rings else -1.0, rings[2] if rings else -1.0))

    # -- F drag-adjust (a DCC style) -----------------------------------
    from pxr.Usdviewq.qt import QtCore
    Key = QtCore.Qt.Key
    radiusBefore = state.radiusWorld
    # Disarmed: probe the key filter with a ShortcutOverride rather than a
    # real F, which would run usdview's Frame Selected and move the camera
    # every later pick depends on.
    state.strokesArmed = False
    probe = shortcutOverride(Key.Key_F)
    check(not viewport.keyEvent(probe, True) and not probe.isAccepted()
          and viewport.adjusting is None,
          "F is left to usdview (Frame Selected) while strokes are "
          "disarmed")
    state.strokesArmed = True
    mouse.move(hoverAt)  # the pointer is over the view again
    wait(20)
    view.setFocus()
    typeKey(view, Key.Key_F)
    wait(20)
    check(viewport.adjusting == "adjustRadius",
          "F over the bound surface starts the radius drag-adjust")
    doubling = brushPanels.ADJUST_RADIUS_PIXELS_PER_DOUBLING
    mouse.move((hoverAt[0] + doubling * ratio, hoverAt[1]))
    wait(20)
    check(abs(state.radiusWorld - 2.0 * radiusBefore) < 0.02 * radiusBefore,
          "dragging right one doubling doubles the radius live "
          "(%.4f -> %.4f)" % (radiusBefore, state.radiusWorld))
    check(viewport.overlayRing() is not None,
          "and the rings follow the adjust")
    typeKey(view, Key.Key_Return)
    wait(20)
    check(viewport.adjusting is None
          and abs(state.radiusWorld - 2.0 * radiusBefore)
          < 0.02 * radiusBefore,
          "Enter confirms the adjusted radius")
    typeKey(view, Key.Key_F)
    wait(20)
    mouse.move((hoverAt[0] + 3.0 * doubling * ratio, hoverAt[1]))
    wait(20)
    typeKey(view, Key.Key_Escape)
    wait(20)
    check(viewport.adjusting is None
          and abs(state.radiusWorld - 2.0 * radiusBefore)
          < 0.02 * radiusBefore,
          "Esc cancels a second adjust back to the confirmed radius")
    typeKey(view, Key.Key_BracketLeft)
    wait(20)
    check(abs(state.radiusWorld - 1.8 * radiusBefore) < 0.02 * radiusBefore,
          "[ shrinks the radius by 10%")
    state.radiusWorld = radiusBefore
    mouse.move(hoverAt)
    wait(20)

    # -- focus trap: a palette spinbox keeps focus (usdview's jealous
    # focus); the pointer over the view, or the key itself, takes it back.
    QtWidgets = _qtWidgets()
    strengthSpin = palette._rows["strength"][1]
    _qtTest().QTest.mouseClick(strengthSpin, QtCore.Qt.MouseButton.LeftButton)
    wait(20)
    held = QtWidgets.QApplication.focusWidget()
    info("after clicking Strength, focus is %s"
         % (type(held).__name__ if held is not None else None))
    viewport.hoverLeave()
    viewport._pointer = None  # the next move is a fresh entry
    mouse.move(hoverAt)
    wait(20)
    check(QtWidgets.QApplication.focusWidget() is not strengthSpin,
          "moving over the view takes focus back from the Strength spinbox")
    radiusNow = state.radiusWorld
    typeKey(view, Key.Key_BracketRight)
    wait(20)
    check(abs(state.radiusWorld - 1.1 * radiusNow) < 0.01 * radiusNow,
          "] over the view grows the radius after a palette edit "
          "(%.4f -> %.4f)" % (radiusNow, state.radiusWorld))
    # Without leaving the view: focus back in the spinbox, key straight in.
    strengthSpin.setFocus()
    wait(20)
    radiusNow = state.radiusWorld
    typeKey(view, Key.Key_BracketRight)
    wait(20)
    check(abs(state.radiusWorld - 1.1 * radiusNow) < 0.01 * radiusNow
          and QtWidgets.QApplication.focusWidget() is not strengthSpin,
          "] still claims while the spinbox holds focus, and takes it")
    state.radiusWorld = radiusBefore

    viewport.hoverLeave()
    check(viewport.overlayRing() is None,
          "leaving the view hides the ring")

    # -- stroke + preview ---------------------------------------------
    # The groom counters (process-wide, from the groom scene index through
    # the brush ABI) settle first, so a later advance is the drag's doing.
    from usdGenTools import brushApi
    state.liveGroom = True

    def groomCounts():
        return (brushApi.groomCookCount(), brushApi.groomPublishCount())

    counts = groomCounts()
    for _tick in range(40):
        wait(100)
        now = groomCounts()
        if now == counts and counts[1] > 0:
            break
        counts = now
    check(counts[0] > 0 and counts[1] > 0,
          "the description cooked and published before the stroke "
          "(%d cooks, %d publications)" % counts)
    mouse.direct = False
    mouse.press(pressAt)
    wait(20)
    check(loop.gestureActive(), "press on the plane starts a stroke")
    if not loop.gestureActive():
        return 1
    first = state.gesture.first  # LiveStroke keeps no per-dab record
    check(0 <= first.face < 64,
          "the first dab addresses a plane face (%d)" % first.face)
    check(brushPreview.HasPreview(stage, binding),
          "the drag raises the session preview")
    check(brushPreview.overlayActive(),
          "inside usdview the preview rides the Hydra overlay index "
          "(%d preview indices)" % brushApi.previewIndexCount())
    check(stage.GetSessionLayer().GetAttributeAtPath(
        "/Plane.primvars:displayColor") is None,
        "so no session displayColor is authored")
    # The edit target layer, not the composed value: the press arms the
    # live-groom flush, which may already have scratched the SESSION layer.
    check(stage.GetEditTarget().GetLayer().GetAttributeAtPath(
        binding.surfacePath.AppendProperty("primvars:" + binding.primvar))
        is None,
        "and the edit target is untouched while it is live")
    check(viewport.overlayRing() is not None,
          "and the ring tracks through the drag")

    dabs = loop.dabCount()
    mouse.move(moveAt)
    wait(20)
    check(loop.dabCount() > dabs,
          "the drag extends the trail (%d -> %d dabs)"
          % (dabs, loop.dabCount()))
    from pxr import Sdf
    sessionSpec = None
    for _tick in range(20):
        wait(50)
        sessionSpec = stage.GetSessionLayer().GetAttributeAtPath(Sdf.Path(
            "/Plane.primvars:usdGen:paint:density"))
        if sessionSpec is not None:
            break
    check(sessionSpec is not None,
          "the paused drag scratches the session primvar for the live groom")
    # The live groom re-cooks DURING the drag, not only on release.
    wait(300)
    during = groomCounts()
    for _tick in range(20):
        if during[0] > counts[0] and during[1] > counts[1]:
            break
        wait(100)
        during = groomCounts()
    check(loop.gestureActive() and during[0] > counts[0]
          and during[1] > counts[1],
          "the drag re-cooks and republishes the groom before release "
          "(cooks %d -> %d, publications %d -> %d)"
          % (counts[0], during[0], counts[1], during[1]))
    colors = shownColors(stage, binding, state)
    check(colors is not None and len(colors) == 4 * 64
          and max(c[0] for c in colors) > 0.5
          and max(c[1] for c in colors) > 0.5
          and min(c[1] for c in colors) < 0.5,
          "the preview displayColor shows the painted spot")

    # -- bake -----------------------------------------------------------
    mouse.release(moveAt)
    wait(20)
    check(not loop.gestureActive(), "release drops the stroke")
    after = groomCounts()
    for _tick in range(30):
        if after[1] > during[1]:
            break
        wait(100)
        after = groomCounts()
    check(after[0] > during[0] and after[1] > during[1],
          "the bake cooks and publishes the groom again "
          "(cooks %d -> %d, publications %d -> %d)"
          % (during[0], after[0], during[1], after[1]))
    values = brushAuthor.BakedValues(stage, binding)
    check(values is not None and len(values) == 4 * 64
          and max(values) == 1.0 and min(values) < 0.5,
          "release bakes painted and default corners (max %.3f, min %.3f)"
          % (max(values) if values else -1.0,
             min(values) if values else -1.0))
    check(brushPreview.HasPreview(stage, binding),
          "release leaves the baked map drawn")
    drawn = shownColors(stage, binding, state)
    check(drawn is not None and len(drawn) == 4 * 64
          and max(c[0] for c in drawn) > 0.5
          and max(c[1] for c in drawn) > 0.5
          and min(c[1] for c in drawn) < 0.5,
          "and the display shows the baked spot")

    # -- undo / redo ------------------------------------------------------
    before = list(brushAuthor.BakedValues(stage, binding))
    shown = shownColors(stage, binding, state)
    check(palette.undo(), "the palette undoes the bake")
    check(brushAuthor.BakedValues(stage, binding) is None,
          "undo takes the baked primvar back off")
    undone = shownColors(stage, binding, state)
    check(undone is not None and min(c[0] for c in undone) == 1.0,
          "and the display falls back to the unpainted default (white)")
    check(palette.redo(), "the palette redoes the bake")
    check(brushAuthor.BakedValues(stage, binding) == before,
          "redo puts the baked primvar back")
    check(shownColors(stage, binding, state) == shown,
          "and the display redraws the bake back")

    # -- preview toggle (the eye) ----------------------------------------
    eye = palette._rows["previewMap"][1]
    check(eye.isChecked() and state.previewMap,
          "the eye toggle reads the overlay as shown")
    eye.click()
    wait(20)
    check(not state.previewMap and not eye.isChecked(),
          "clicking the eye hides the map")
    check(not brushPreview.HasPreview(stage, binding),
          "and the overlay leaves the surface")
    eye.click()
    wait(20)
    check(state.previewMap and brushPreview.HasPreview(stage, binding),
          "clicking it again shows the overlay")
    check(shownColors(stage, binding, state) == shown,
          "drawing the baked map again")

    # -- mask-preset row --------------------------------------------------
    combo = palette._rows["maskPreset"][1]
    lengthAt = None
    densityAt = None
    for index in range(combo.count()):
        if combo.itemData(index) == "length":
            lengthAt = index
        if combo.itemData(index) == "density":
            densityAt = index
    check(lengthAt is not None and densityAt is not None,
          "the mask-preset row offers Length and Density")
    combo.setCurrentIndex(lengthAt)
    wait(20)
    check(state.maskPreset == "length"
          and state.binding.primvar == "usdGen:paint:length"
          and str(state.binding.mapPath) == descPath + "/Maps/lengthPaint",
          "the row switches the bound map and primvar")
    flipped = shownColors(stage, state.binding, state)
    check(flipped is not None and flipped != shown
          and min(c[0] for c in flipped) == 1.0,
          "and the unpainted length map is drawn (white)")
    expr = stage.GetPrimAtPath(descPath + "/Expressions/lengthScale")
    check(expr.IsValid()
          and list(grow.GetAttribute("usdGen:length").GetConnections())
          == [expr.GetPath()],
          "with the length wiring live on grow")

    # -- a length stroke alone re-grows the groom ------------------------
    # User report: painting length did not affect the groom until some
    # other map was painted. A rebuilt Grow restarted its buffer stamps,
    # so Clump/Curl kept their stale captures. One stroke at 0.3, no
    # other edit, must shorten the published strands.
    def settledCounts():
        last = groomCounts()
        for _tick in range(40):
            wait(100)
            now = groomCounts()
            if now == last:
                return now
            last = now
        return last

    settledCounts()
    lengthBefore = brushApi.groomCurveStats(descPath)
    check(lengthBefore is not None and lengthBefore[0] > 0
          and lengthBefore[1] > 0.0,
          "the groom publishes strands before the length stroke (%r)"
          % (lengthBefore,))
    strokeCounts = groomCounts()
    state.value = 0.3
    state.radiusWorld = 0.5
    mouse.press(pressAt)
    wait(20)
    check(loop.gestureActive()
          and state.binding.primvar == "usdGen:paint:length",
          "a press starts a length stroke")
    mouse.move(moveAt)
    wait(50)
    mouse.release(moveAt)
    wait(20)
    check(not loop.gestureActive(), "release bakes the length stroke")
    lengthValues = brushAuthor.BakedValues(stage, state.binding)
    check(lengthValues is not None and min(lengthValues) < 0.5,
          "the bake writes the 0.3 length paint (min %.3f)"
          % (min(lengthValues) if lengthValues else -1.0))
    lengthAfter = lengthBefore
    for _tick in range(50):
        wait(100)
        now = groomCounts()
        lengthAfter = brushApi.groomCurveStats(descPath)
        if (now[1] > strokeCounts[1] and lengthAfter is not None
                and lengthBefore is not None
                and lengthAfter[1] < lengthBefore[1] * 0.995):
            break
    check(lengthBefore is not None and lengthAfter is not None
          and lengthAfter[0] == lengthBefore[0]
          and lengthAfter[1] < lengthBefore[1] * 0.995,
          "the length stroke alone shortens the published strands "
          "(%d curves, total length %.4f -> %.4f)"
          % (lengthBefore[0] if lengthBefore else -1,
             lengthBefore[1] if lengthBefore else -1.0,
             lengthAfter[1] if lengthAfter else -1.0))
    state.value = 0.0
    state.radiusWorld = 0.1
    combo.setCurrentIndex(densityAt)
    wait(20)
    check(state.binding.primvar == "usdGen:paint:density",
          "switching back rebinds density")
    check(shownColors(stage, binding, state) == shown,
          "and the baked density map is drawn back")

    viewport.uninstall()
    print("testUsdviewBrushSetup: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the setup proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewBrushSetup needs testusdview (no live view)")
    sys.exit(0)
