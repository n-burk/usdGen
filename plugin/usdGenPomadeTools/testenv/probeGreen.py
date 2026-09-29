# One-off diagnostic: which published prim draws the pure-green sliver?
# Runs under testusdview like capturePomadeBraidL2.py and samples the frame
# after hiding one family at a time.
import ctypes
import os
import sys

GROOM = "/PomadeGroom"
FOCUS_LEVEL = 2


def testenvDir():
    for i, arg in enumerate(list(sys.argv)):
        if arg == "--testScript" and i + 1 < len(sys.argv):
            return os.path.dirname(os.path.abspath(sys.argv[i + 1]))
    return os.path.dirname(os.path.abspath(__file__))


def run(appController):
    here = testenvDir()
    sys.path.insert(0, here)
    sys.path.insert(0, os.path.normpath(os.path.join(here, "..", "python")))
    import usdGenPomadeTools
    import capturePomadeBraidL2 as cap

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = appController._plugRegistry
    container = usdGenPomadeTools.container()
    ui = appController._ui
    ui.primStageSplitter.setSizes([0, 1])
    ui.topBottomSplitter.setSizes([1, 0])
    dataModel.selection.clear()
    dataModel.viewSettings.showBBoxes = False
    ratio = float(view.devicePixelRatioF())
    view.SetPhysicalWindowSize(int(cap.VIEW[0] * ratio),
                               int(cap.VIEW[1] * ratio))
    cap.wait(50)

    def firstFrame():
        view.update()
        view.repaint()
        view.updateGL()
        img = view.grabFrameBuffer()
        n = 0
        first = None
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                if (img.pixel(x, y) & 0x00FFFFFF) == 0x0000FF00:
                    n += 1
                    if first is None:
                        first = (x, y)
        print("probe: %-38s green=%d first=%s"
              % ("FIRST FRAME (no tool, no camera)", n, first))
    firstFrame()

    registry.getCommandPlugin("usdGenPomadeTools.openWorkspace").run()
    session = container.session
    viewport = container.viewport
    if not session.hydrate(groomPath=GROOM, stage=stage):
        print("FAIL: hydrate")
        return 1
    if container.workspace is not None:
        container.workspace.hide()
    dll = session.dll
    viewport.setMode("hierarchy")
    state = container.pomadeState
    while state.activeLevel < FOCUS_LEVEL:
        viewport.enterLevel()
    dll.Pomade_RefillGuides(session.model, ctypes.c_float(1.0))
    dll.Pomade_Rasterise(session.model)
    session.publishAll()

    box = cap.unionBoxes((cap.stageBounds(stage), cap.modelBounds(session)))
    cap.frameBox(stage, view, box)
    view.setFocus()
    viewport.syncDisplayScale()
    session.publishAll()
    for _ in range(200):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            break
        cap.wait(25)

    def green(label):
        view.update()
        view.repaint()
        view.updateGL()
        img = view.grabFrameBuffer()
        n = 0
        ink = 0
        first = None
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                px = img.pixel(x, y) & 0x00FFFFFF
                if px == 0x0000FF00:
                    n += 1
                    if first is None:
                        first = (x, y)
                r = (px >> 16) & 255
                g = (px >> 8) & 255
                b = px & 255
                # Anything that is not the flat grey backdrop.
                if abs(r - g) > 6 or abs(g - b) > 6 or abs(r - b) > 6:
                    ink += 1
        print("probe: %-38s green=%d ink=%d first=%s"
              % (label, n, ink, first))
        return n

    green("baseline (hierarchy, L2 focus)")

    # One level at a time.
    for hide in (1, 2, 3):
        for lvl in (1, 2, 3):
            dll.Pomade_SetLevelDisplay(session.model, lvl,
                                      0 if lvl == hide else 1, 1)
        session.publishAll()
        green("level %d hidden" % hide)
    for lvl in (1, 2, 3):
        dll.Pomade_SetLevelDisplay(session.model, lvl, 0, 1)
    session.publishAll()
    green("every level hidden")
    for lvl in (1, 2, 3):
        dll.Pomade_SetLevelDisplay(session.model, lvl, 1, 1)
    session.publishAll()

    # Centers-only on one level at a time (mesh, rings and guides off).
    for keep in (1, 2, 3):
        for lvl in (1, 2, 3):
            dll.Pomade_SetLevelDrawMode(session.model, lvl,
                                       1 if lvl == keep else 0, 1, 1)
        session.publishAll()
        green("only level %d, centers-only" % keep)
    for lvl in (1, 2, 3):
        dll.Pomade_SetLevelDrawMode(session.model, lvl, 1, 1, 0)
    session.publishAll()

    # Graph overlay and the two manipulators.
    zero3 = (ctypes.c_float * 3)(0.0, 0.0, 0.0)
    zero9 = (ctypes.c_float * 9)(1, 0, 0, 0, 1, 0, 0, 0, 1)
    dll.Pomade_SetGizmo(session.model, 0, zero3, zero9, ctypes.c_float(1.0), -1)
    dll.Pomade_SetBrushRing(session.model, zero3, zero3, ctypes.c_float(0.0))
    session.publishAll()
    green("gizmo + brush ring cleared")

    # Rings on every tube, then off.
    dll.Pomade_SetRingDisplay(session.model, 2)
    session.publishAll()
    green("rings on every tube")
    dll.Pomade_SetRingDisplay(session.model, 0)
    session.publishAll()
    green("rings off")

    # Guides on.
    dll.Pomade_SetDisplayPolicy(session.model, b"fill", b"preview",
                               FOCUS_LEVEL)
    session.publishAll()
    green("fill policy (guides on)")

    # The last split: with the model gone the Pomade index publishes
    # nothing at all, so anything left in the frame belongs to the stage.
    viewport.uninstall()
    session.deactivate()
    cap.wait(50)
    green("model DEACTIVATED (stage only)")

    # Name the stage prim that draws it: hide one subtree at a time.
    from pxr import UsdGeom
    for path in ("/PomadeGroom/Guides", "/PomadeGroom/Tubes",
                 "/PomadeGroom/ScalpGraph", "/World/Scalp", "/Groom",
                 "/PomadeGroom"):
        prim = stage.GetPrimAtPath(path)
        if not prim:
            print("probe: %s absent" % path)
            continue
        im = UsdGeom.Imageable(prim)
        if im:
            im.MakeInvisible()
        else:
            prim.SetActive(False)
        cap.wait(20)
        green("stage only, %s hidden" % path)
    return 0


def testUsdviewInputFunction(appController):
    return run(appController)
