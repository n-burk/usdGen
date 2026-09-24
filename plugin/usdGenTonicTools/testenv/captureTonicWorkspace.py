# captureTonicWorkspace -- writes renders/tonic-workspace.png (plan/18 V3
# exit, V7 deliverable) and renders/tonic-first-run.png (the unbound dock,
# docs/tonic-tool.md "First run").
#
#   powershell -File bin/launch_usdview.ps1 -TestScript \
#       plugin/usdGenTonicTools/testenv/captureTonicWorkspace.py \
#       examples/tonic-graph-scalp.usda
#
# The launcher builds the same PXR_PLUGINPATH_NAME / PYTHONPATH the T3
# tonic tests use. The stroke below is drawn at x/z 1..3, so it needs the
# 4x4 graph-scalp fixture (tonic-single-quad.usda spans only -1..1).
#
# NOT a test -- nothing here asserts a budget or a pixel, and it writes
# into the source tree, which is why its name does not start with "test"
# and ctest does not run it. It is the documented way to refresh the
# screenshot the plan and the docs point at.
#
# What the shot has to show (plan/18 section 1): the workspace dock open
# beside the viewport, Tube mode active on the shelf, a tube selected
# with its translate gizmo up, the parameter rows of the active mode, and
# the status strip. The frame comes from the T3 harness's own frame grab
# (the StageView renders offscreen under testusdview, so the widget's own
# paint can be empty) composited into a grab of the whole main window.
import ctypes
import os
import sys

RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
CENTRE = (2.0, 2.0)
# The viewport's logical size. testusdview pins the StageView with
# setFixedSize (SetPhysicalWindowSize, testusdview line 53) so its own
# scripts render a fixed frame, and that pin is what decides the window:
# the shot re-pins it once, before anything renders, and then sizes the
# window around it and the dock.
VIEW = (1180, 980)
DOCK_WIDTH = 470
# What the window needs beyond the viewport and the dock: usdview's menu
# bar and path field above, its timeline and status bar below, and the
# frame borders either side. The height is a floor -- the collapsed
# attribute browser and the timeline have minimums of their own, which
# is why VIEW is tall enough to fill what they leave.
CHROME = (60, 210)
# Where the shot camera stands and what it looks at. The tube is
# about three units tall over a five-unit scalp, so the eye sits back
# far enough for both to fit with room around them.
EYE = (9.5, 8.0, 11.0)
TARGET = (2.0, 1.5, 2.0)


def info(text):
    print("info: %s" % text)


def testenvDir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for i, arg in enumerate(list(sys.argv)):
        if arg == "--testScript" and i + 1 < len(sys.argv):
            return os.path.dirname(os.path.abspath(sys.argv[i + 1]))
        if arg.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(arg.split("=", 1)[1]))
    return ""


def typeKey(view, name):
    import importlib
    from pxr.Usdviewq.qt import QtCore, PySideModule
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    keys = {"2": QtCore.Qt.Key.Key_2, "d": QtCore.Qt.Key.Key_D}
    QtTest.QTest.keyClick(view, keys[name],
                          QtCore.Qt.KeyboardModifier.NoModifier)


def wait(ms):
    import importlib
    from pxr.Usdviewq.qt import PySideModule
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    QtTest.QTest.qWait(int(ms))


def frameScalp(stage, view):
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicShotCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(*EYE)
    target = Gf.Vec3d(*TARGET)
    zAxis = (eye - target).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0, 1, 0), zAxis).GetNormalized()
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
        "/TonicShotCamera")
    return view.getActiveSceneCamera() is not None


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import usdGenTonicTools
    from usdGenTonicTools import tonicCamera
    from testUsdviewTonicGraph import Mouse
    from pxr.Usdviewq.qt import QtCore

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = appController._plugRegistry
    container = usdGenTonicTools.container()
    if view is None or registry is None or container is None:
        print("FAIL: no stage view / plugin registry / container")
        return 1

    mainWindow = appController._mainWindow
    # The shot is about the tool, so usdview's own prim browser and
    # property editor collapse the way its viewport-only layout collapses
    # them, and the viewport and the Tonic dock share the whole window.
    ui = appController._ui
    ui.primStageSplitter.setSizes([0, 1])
    ui.topBottomSplitter.setSizes([1, 0])
    # The viewport is re-pinned here, before the first frame: resizing it
    # after it has drawn leaves the previous frame in the widget's
    # framebuffer and the grab below picks up both.
    ratio = float(view.devicePixelRatioF())
    view.SetPhysicalWindowSize(int(VIEW[0] * ratio), int(VIEW[1] * ratio))
    mainWindow.resize(VIEW[0] + DOCK_WIDTH + CHROME[0],
                      VIEW[1] + CHROME[1])
    wait(50)

    if not frameScalp(stage, view):
        print("FAIL: the shot camera did not activate")
        return 1
    view.setFocus()

    root = os.path.normpath(os.path.join(here, "..", "..", ".."))
    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    # The first-run shot (docs/tonic-tool.md "First run"): the dock open
    # with nothing bound -- the Bind button, the Step 1 hint and the
    # gated shelf -- before the bind below changes any of it.
    firstRun = container.workspace
    if firstRun is not None:
        firstRun.setMinimumWidth(DOCK_WIDTH)
        mainWindow.resize(VIEW[0] + DOCK_WIDTH + CHROME[0],
                          mainWindow.height())
        firstRun.refresh()
        wait(50)
        view.update()
        view.repaint()
        early = mainWindow.grab()
        earlyOut = os.path.join(root, "renders", "tonic-first-run.png")
        os.makedirs(os.path.dirname(earlyOut), exist_ok=True)
        if early.save(earlyOut, "PNG"):
            print("wrote %s (%dx%d)" % (earlyOut, early.width(),
                                        early.height()))
        else:
            print("FAIL: could not write %s" % earlyOut)
            return 1
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    workspace = container.workspace
    if session is None or session.model is None or workspace is None:
        print("FAIL: the workspace did not open on a live model")
        return 1

    # QMainWindow hands the dock whatever is left after the central
    # widget, and the central widget's width is now fixed by the pinned
    # viewport, so resizeDocks alone leaves the dock at a sliver however
    # wide the window gets. A minimum width is the one thing the layout
    # cannot take back.
    workspace.setMinimumWidth(DOCK_WIDTH)
    mainWindow.resize(VIEW[0] + DOCK_WIDTH + CHROME[0], mainWindow.height())
    wait(50)

    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the controller's camera did not resolve")
        return 1

    def pixel(x, z, y=0.0):
        projected = camera.worldToPixels((x, y, z))
        return (projected[0], projected[1])

    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)

    # A real stroke, so the shot shows a real region and a real tube.
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
        mouse.direct = True
        mouse.press(path[0])
    for point in path[1:]:
        mouse.move(point)
    mouse.release(path[-1])
    info("graph: %r" % (session.graphCounts(),))

    # Tube mode with the tube selected: the shelf shows 2 Tube, the dock
    # shows Tube's parameter rows, and the viewport shows the gizmo.
    typeKey(view, "2")
    state.snapRadiusPx = 200.0
    cvCount = int(session.dll.Tonic_GetCenterCVCount(session.model))
    tipOut = (ctypes.c_float * 3)()
    session.dll.Tonic_GetCenterCV(session.model, max(cvCount - 1, 0), tipOut)
    tip = (float(tipOut[0]), float(tipOut[1]), float(tipOut[2]))
    mouse.click(pixel(tip[0], tip[2], tip[1]))
    session.publish()
    # Let the idle pump land the commit and the baked map before the
    # grab, or the status strip in the shot reports a groom that is
    # still mid-flight (stage v0, swap 0.0 ms) and the guides the
    # committer authors are not drawn yet.
    for _ in range(200):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            break
        wait(25)
    # Bind took the scalp from usdview's selection; keeping it selected
    # afterwards paints usdview's yellow highlight over the whole scalp
    # mesh, which is what made the 2026-09-19 shot show an olive head
    # instead of the region colours underneath it.
    dataModel.selection.clear()
    # With nothing selected usdview draws the whole stage's bounding box
    # instead, which is the same white wireframe through the groom.
    dataModel.viewSettings.showBBoxes = False
    workspace.refresh()
    info("mode %r, selection %r, gizmo tube %r"
         % (state.activeMode, session.readSelection(2),
            session.selectionCount(0)))
    wait(50)

    # usdview draws unconditional RGB origin axes after every frame
    # (stageView.DrawAxis: pure-green +Y, scaled by camera distance, with
    # no toggle). Through x-rayed tubes the Y axis reads as a bright
    # green line through the groom — the V9 "green sliver", which
    # bisected to this and nothing else (probeGreen.py). Viewer chrome,
    # not tool content, so the shot turns it off.
    if hasattr(view, "DrawAxis"):
        view.DrawAxis = lambda *args, **kwargs: None

    # The window grab carries the dock, the shelf and the status strip;
    # the StageView is an offscreen QOpenGLWidget under testusdview, so
    # its own area in that grab can be blank. Paint the frame the harness
    # grabs from the renderer over it.
    view.update()
    view.repaint()
    shot = mainWindow.grab()
    info("window %dx%d, view %dx%d, dock %dpx, shot %dx%d (dpr %.2f)"
         % (mainWindow.width(), mainWindow.height(), view.width(),
            view.height(), workspace.width(), shot.width(), shot.height(),
            ratio))

    root = os.path.normpath(os.path.join(here, "..", "..", ".."))
    out = os.path.join(root, "renders", "tonic-workspace.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if not shot.save(out, "PNG"):
        print("FAIL: could not write %s" % out)
        return 1
    print("wrote %s (%dx%d)" % (out, shot.width(), shot.height()))

    viewport.uninstall()
    session.deactivate()
    return 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: captureTonicWorkspace needs testusdview")
    sys.exit(0)
