# testUsdviewBrushStroke -- T3: the brush tool through REAL mouse events.
#
#   testusdview --testScript plugin/usdGenTools/testenv/testUsdviewBrushStroke.py \
#                 examples/test-plane.usda
#
# Proven here, all of it through the installed palette and viewport filter:
#
#   * bind: the Brush Palette command opens the dock, Bind reads usdview's
#     prim selection (/Plane) and defines a UsdGenPaintMap on it, and the
#     viewport controller installs itself;
#   * stroke: a press-drag-release over the plane records face/(u, v) dabs
#     from the CPU raycast -- no Hydra pick per move;
#   * preview: while the drag is live the map overlay shows the hot
#     stroke, and the edit target is untouched;
#   * live groom: with the per-move write held off, the viewport's
#     single-shot flush still writes the session scratch primvar before
#     the release, and its status reaches the palette (the container path
#     wires the viewport's status sink);
#   * bake: release commits the corner-sampled paint primvar into the edit
#     target (usdview's session layer) and leaves the baked map drawn;
#   * Escape mid-stroke drops the gesture with the baked primvar
#     byte-identical and the map display restored;
#   * the palette's Undo/Redo replay the bake, redrawing the map display;
#   * Flood fills the bound map with the brush value, undoably.
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


def typeEscape(view):
    from pxr.Usdviewq.qt import QtCore
    _qtTest().QTest.keyClick(view, QtCore.Qt.Key.Key_Escape)


class Mouse:
    """QtTest mouse events on the StageView, in physical pixels.

    The controller reads physical pixels while QtTest takes logical widget
    coordinates, so everything converts once, here. Moves during a drag go
    straight at the widget: QTest.mouseMove reports NoButton even after a
    press in this Qt build, and a drag must carry LeftButton (the
    testUsdviewPomadeGraph.Mouse arrangement).
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
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/BrushStrokeCamera"))
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
        "/BrushStrokeCamera")
    return view.getActiveSceneCamera() is not None


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

def run(appController):
    global failures
    try:
        import usdGenTools
        from usdGenTools import (brushAuthor, brushCamera, brushPreview,
                                 brushState)
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
    wait(50)

    # -- bind ---------------------------------------------------------
    dataModel.selection.setPrimPath("/Plane")
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
    bound = palette.bindFromSelection()
    check(bound, "Bind reads the /Plane selection")
    binding = state.binding
    check(binding is not None and str(binding.surfacePath) == "/Plane",
          "the loop is bound to /Plane")
    viewport = container.brushViewport
    check(viewport is not None and viewport.installed,
          "bind installs the viewport controller")
    if binding is None or viewport is None:
        return 1
    paintMap = stage.GetPrimAtPath(binding.mapPath)
    check(paintMap.GetTypeName() == "UsdGenPaintMap",
          "bind defined a UsdGenPaintMap at %s" % binding.mapPath)
    state.strength = 1.0
    state.radiusWorld = 0.1
    state.value = 0.0  # the density preset's unpainted default is 1.0

    # The container path wires the viewport's status into the palette
    # (it used to return before setStatusSink and drop every message).
    paletteSink = viewport._statusSink
    check(paletteSink is not None and paletteSink == palette.onLoopStatus,
          "bind wires the viewport status sink to the palette")
    sink = []

    def recordStatus(text, kind=None):
        sink.append((text, kind))
        if paletteSink is not None:
            paletteSink(text, kind)

    viewport.setStatusSink(recordStatus)
    check(palette._rows["liveGroom"][1].isChecked() and state.liveGroom,
          "the live groom toggle reads on")

    camera = brushCamera.resolve(view)
    check(camera is not None and camera.invertible,
          "the StageView camera resolves")
    if camera is None:
        return 1

    def pixelOf(point):
        projected = camera.worldToPixels(point)
        return (projected[0], projected[1])

    mouse = Mouse(view)
    pressAt = pixelOf((0.55, 0.0, 0.55))
    moveAt = pixelOf((0.95, 0.0, 0.55))
    midAt = pixelOf((0.75, 0.0, 0.55))
    info("press %.1f,%.1f move %.1f,%.1f (physical)"
         % (pressAt[0], pressAt[1], moveAt[0], moveAt[1]))

    # -- stroke + preview ---------------------------------------------
    mouse.press(pressAt)
    wait(20)
    if not loop.gestureActive():
        # QtTest's global-coordinate round trip can miss the child widget
        # on a window the harness never showed; the direct send is a real
        # QMouseEvent through the real filter either way.
        mouse.direct = True
        mouse.press(pressAt)
        wait(20)
    check(loop.gestureActive(), "press on the plane starts a stroke")
    if not loop.gestureActive():
        return 1
    first = state.gesture.first  # LiveStroke keeps no per-dab record
    check(0 <= first.face < 64,
          "the first dab addresses a plane face (%d)" % first.face)
    check(0.0 <= first.u <= 1.0 and 0.0 <= first.v <= 1.0,
          "with face-local uv (%.3f, %.3f)" % (first.u, first.v))
    check(brushPreview.HasPreview(stage, binding),
          "the drag raises the session preview")
    # The edit target layer, not the composed value: the press arms the
    # live-groom flush, which may already have scratched the SESSION layer.
    check(stage.GetEditTarget().GetLayer().GetAttributeAtPath(
        binding.surfacePath.AppendProperty("primvars:" + binding.primvar))
        is None,
        "and the edit target is untouched while it is live")

    dabs = loop.dabCount()
    mouse.move(moveAt)
    wait(20)
    check(loop.dabCount() > dabs,
          "the drag extends the trail (%d -> %d dabs)"
          % (dabs, loop.dabCount()))
    colors = shownColors(stage, binding, state)
    check(colors is not None and len(colors) == 4 * 64
          and max(c[0] for c in colors) > 0.5
          and max(c[1] for c in colors) > 0.5
          and min(c[1] for c in colors) < 0.5,
          "the preview overlay shows the painted spot")

    # -- live groom: the trailing flush --------------------------------
    # Hold the per-move write off so only the viewport's single-shot
    # flush can land the last move: the groom must follow a drag that
    # stops without a release.
    from pxr import Sdf
    from usdGenTools import brushLoop
    scratch = Sdf.Path("/Plane.primvars:usdGen:paint:density")
    saved = brushLoop.LIVE_GROOM_MIN_INTERVAL
    brushLoop.LIVE_GROOM_MIN_INTERVAL = 1e9
    try:
        writes = loop.liveGroomWrites
        flushes = viewport.groomFlushes
        mouse.move(midAt)
        check(loop.gestureActive() and loop.liveGroomWrites == writes,
              "a held-off move leaves the live groom pending")
        for _tick in range(20):
            wait(30)
            if viewport.groomFlushes > flushes:
                break
        check(viewport.groomFlushes > flushes
              and loop.liveGroomWrites > writes,
              "the single-shot flush writes the live groom within ~%d ms "
              "of the last move (%d flush(es))"
              % (brushLoop.LIVE_GROOM_FLUSH_MS,
                 viewport.groomFlushes - flushes))
        check(loop.gestureActive(), "all before the release")
        check(stage.GetSessionLayer().GetAttributeAtPath(scratch)
              is not None,
              "the session scratch primvar carries the drag")
        check(any(kind == "cooking" and text.startswith("live groom")
                  for text, kind in sink),
              "and the palette status reads cooking")
    finally:
        brushLoop.LIVE_GROOM_MIN_INTERVAL = saved

    # -- bake -----------------------------------------------------------
    mouse.release(moveAt)
    wait(20)
    check(not loop.gestureActive(), "release drops the stroke")
    values = brushAuthor.BakedValues(stage, binding)
    check(values is not None and len(values) == 4 * 64
          and max(values) == 1.0 and min(values) < 0.5,
          "release bakes painted and untouched corners (max %.3f, min %.3f)"
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

    # -- Escape -----------------------------------------------------------
    before = list(brushAuthor.BakedValues(stage, binding))
    shown = shownColors(stage, binding, state)
    mouse.press(pressAt)
    wait(20)
    check(loop.gestureActive(), "a second stroke starts")
    mouse.move(moveAt)
    wait(20)
    typeEscape(view)
    wait(20)
    check(not loop.gestureActive(), "Escape drops the live stroke")
    mouse.release(moveAt)
    wait(20)
    check(brushAuthor.BakedValues(stage, binding) == before,
          "and the baked primvar is byte-identical")
    check(brushPreview.HasPreview(stage, binding),
          "and the map display is restored")
    check(shownColors(stage, binding, state) == shown,
          "showing the pre-stroke map")

    # -- undo / redo ------------------------------------------------------
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

    # -- flood -------------------------------------------------------------
    state.value = 0.0
    check(palette.flood(), "the palette floods the map")
    check(brushAuthor.BakedValues(stage, binding) == [0.0] * (4 * 64),
          "every corner floods to the brush value")
    check(palette.undo(), "the palette undoes the flood")
    check(brushAuthor.BakedValues(stage, binding) == before,
          "undo puts the baked map back")

    viewport.uninstall()
    print("testUsdviewBrushStroke: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the stroke proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewBrushStroke needs testusdview (no live view)")
    sys.exit(0)
