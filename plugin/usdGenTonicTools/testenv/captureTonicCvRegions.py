# captureTonicCvRegions -- refresh renders/tonic-cv-regions.png.
#
# This deliberately remains outside CTest: it writes a review artifact.  Run
# it with testusdview and the same environment as testUsdviewTonicCvRegions.
import os
import sys

VIEW = (1180, 920)
DOCK_WIDTH = 480


def info(text):
    print("info: %s" % text)


def run(appController):
    # testusdview execs capture scripts from its own module namespace, so
    # make our sibling helper importable before importing it.
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if not here:
        for index, arg in enumerate(list(sys.argv)):
            if arg == "--testScript" and index + 1 < len(sys.argv):
                here = os.path.dirname(os.path.abspath(sys.argv[index + 1]))
                break
            if arg.startswith("--testScript="):
                here = os.path.dirname(os.path.abspath(
                    arg.split("=", 1)[1]))
                break
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    from testUsdviewTonicCvRegions import (LEFT, RIGHT, _bindGeometryFromDock,
                                            _setPtexDensity, frameScalp,
                                            typeKey, wait)
    import usdGenTonicTools
    from usdGenTonicTools import tonicCamera
    from testUsdviewTonicGraph import Mouse
    from pxr.Usdviewq.qt import QtCore, QtGui

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    mainWindow = appController._mainWindow
    registry = appController._plugRegistry
    container = usdGenTonicTools.container()
    if view is None or mainWindow is None or registry is None or container is None:
        print("FAIL: no usdview window / registry / Tonic container")
        return 1

    # Give the dock enough room to show Geometry, the Region shelf and the
    # explicit 128 Ptex setting alongside a recognisable single-quad scalp.
    ui = appController._ui
    ui.primStageSplitter.setSizes([0, 1])
    ui.topBottomSplitter.setSizes([1, 0])
    ratio = float(view.devicePixelRatioF())
    view.SetPhysicalWindowSize(int(VIEW[0] * ratio), int(VIEW[1] * ratio))
    mainWindow.resize(VIEW[0] + DOCK_WIDTH + 60, VIEW[1] + 210)
    if not frameScalp(stage, view):
        print("FAIL: the capture camera did not activate")
        return 1
    view.setFocus()
    wait(40)

    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    workspace = container.workspace
    if workspace is None or not _bindGeometryFromDock(workspace):
        print("FAIL: capture could not bind /Scalp through the dock")
        return 1
    workspace.setMinimumWidth(DOCK_WIDTH)
    mainWindow.resize(VIEW[0] + DOCK_WIDTH + 60, mainWindow.height())
    wait(40)

    session = container.session
    viewport = container.viewport
    state = container.tonicState
    if session is None or session.model is None or viewport is None:
        print("FAIL: capture has no live Tonic model")
        return 1
    workspace.refresh()
    if _setPtexDensity(workspace) != "128":
        print("FAIL: capture could not select Ptex 128")
        return 1
    view.setFocus()                    # release the Ptex combo's text focus
    wait(10)

    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: no capture camera")
        return 1
    state.snapRadiusPx = max(
        0.05 / max(camera.worldPerPixel((0.0, 0.0, 0.0)), 1e-9), 2.0)

    def pixel(point):
        projected = camera.worldToPixels((point[0], 0.0, point[1]))
        return (projected[0], projected[1])

    mouse = Mouse(view)
    mouse.direct = True
    # Save the authoring state before closure as well.  The draft is a Qt
    # child overlay (not Hydra data), so keep its own grab and restore it
    # after compositing the renderer framebuffer into the window shot.
    for point in LEFT:
        mouse.click(pixel(point))
    mouse.move(pixel((-0.12, 0.48)))
    wait(30)

    def compositeShot(includeDraft=False):
        view.update()
        view.repaint()
        frame = view.grabFrameBuffer()
        overlay = getattr(viewport, "_regionOverlay", None)
        overlayImage = overlay.grab() if includeDraft and overlay is not None \
            else None
        shot = mainWindow.grab()
        painter = QtGui.QPainter(shot)
        try:
            origin = view.mapTo(mainWindow, QtCore.QPoint(0, 0))
            target = QtCore.QRect(origin, view.size())
            painter.drawImage(target, frame)
            if overlayImage is not None:
                painter.drawPixmap(origin, overlayImage)
        finally:
            painter.end()
        return shot

    root = os.path.normpath(os.path.join(here, "..", "..", ".."))
    draftOut = os.path.join(root, "renders", "tonic-cv-draft.png")
    os.makedirs(os.path.dirname(draftOut), exist_ok=True)
    draft = compositeShot(includeDraft=True)
    if not draft.save(draftOut, "PNG"):
        print("FAIL: could not write %s" % draftOut)
        return 1
    print("wrote %s (%dx%d); draft %d CVs" %
          (draftOut, draft.width(), draft.height(),
           len(viewport.regionDraftPreview()["points"])))

    typeKey(view, "return")
    for point in RIGHT:
        mouse.click(pixel(point))
    mouse.click(pixel(RIGHT[0]))
    workspace.refresh()
    for _ in range(120):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            break
        wait(20)
    dataModel.selection.clear()
    dataModel.viewSettings.showBBoxes = False
    workspace.refresh()
    wait(50)

    # Window grabs contain the dock but testusdview's offscreen StageView
    # can be blank.  Composite its renderer framebuffer back into the exact
    # widget rectangle so the saved shot always displays the real regions.
    shot = compositeShot()
    out = os.path.join(root, "renders", "tonic-cv-regions.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if not shot.save(out, "PNG"):
        print("FAIL: could not write %s" % out)
        return 1
    print("wrote %s (%dx%d); graph %r, Ptex %d" %
          (out, shot.width(), shot.height(), session.graphCounts(),
           session.bakeTexelResolution))
    viewport.uninstall()
    session.deactivate()
    return 0


def testUsdviewInputFunction(appController):
    return run(appController)


if __name__ == "__main__":
    print("SKIP: captureTonicCvRegions needs testusdview")
    sys.exit(0)
