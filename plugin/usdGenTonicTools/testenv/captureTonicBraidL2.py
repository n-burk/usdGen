# captureTonicBraidL2 -- writes renders/tonic-braid-hierarchy-l2.png
# (plan/18 section 2.4a, "The T3 image check for V0").
#
#   testusdview --testScript \
#       plugin/usdGenTonicTools/testenv/captureTonicBraidL2.py \
#       examples/tonic-braid-hierarchy.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the T3 tonic tests use;
# bin/launch_usdview.ps1 -TestScript builds that environment.
#
# NOT a test -- nothing here asserts a budget or a pixel, and it writes
# into the source tree, which is why its name does not start with "test"
# and ctest does not run it. It is captureTonicWorkspace's sibling: that
# one frames the whole workspace on the 4x4 graph fixture, this one
# frames the VIEWPORT alone on the committed braid hierarchy, which is
# the scene plan/18 section 2.4a uses for the viewport look.
#
# What the shot has to show: the L1 parent x-rayed, its L2 children solid
# and focused (thick center curves, 8 px CV dots), the guide preview in
# the clump colour, and the scalp painted in the colour of the tube
# rooted in it -- everything at the pixel sizes the display scale
# converts, so the dots stay dots however far the camera sits.
import ctypes
import os
import sys

# The viewport's logical size, pinned before anything renders:
# testusdview fixes the StageView with SetPhysicalWindowSize so its
# scripts render a known frame.
VIEW = (1100, 900)
# Where the shot camera stands, as a direction and a distance in units of
# the groom's own extent, so the framing follows the geometry rather than
# a hand-tuned position. Same three-quarter direction bin/record_tonic.py
# frames its goldens from, which is what makes the two comparable.
DIRECTION = (0.55, 0.42, 0.72)
DISTANCE = 2.6
# A square aperture on the shot camera, so the distance above frames the
# same way whatever the window's aspect: usdview's default 20.955 x 15.29
# would crop the groom vertically at any distance that fits it across.
APERTURE = 24.0
GROOM = "/TonicGroom"
FOCUS_LEVEL = 2


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


def wait(ms):
    import importlib
    from pxr.Usdviewq.qt import PySideModule
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    QtTest.QTest.qWait(int(ms))


def modelBounds(session):
    """World (min, max) of every tube the live model holds, or None.

    Read through the selection, the way bin/record_tonic.py reads it, and
    put the selection back: the tubes are Hydra-only prims, so no
    UsdGeomBBoxCache can see them and the camera has to ask the model.
    """
    from usdGenTonicTools import tonicBridge, tonicLib
    dll = session.dll
    ids = tonicBridge.readTubeIds(dll, session.model)
    if not ids:
        return None
    array = (ctypes.c_int * len(ids))(*[int(i) for i in ids])
    if dll.Tonic_SelectSet(session.model, tonicLib.TONIC_PICK_TUBE_VERT,
                           array, None, None, len(ids)) != 0:
        return None
    lo = (ctypes.c_float * 3)()
    hi = (ctypes.c_float * 3)()
    status = dll.Tonic_GetSelectionBounds(session.model, lo, hi)
    dll.Tonic_SelectClear(session.model, 0)
    if status != 0:
        return None
    return ((float(lo[0]), float(lo[1]), float(lo[2])),
            (float(hi[0]), float(hi[1]), float(hi[2])))


def stageBounds(stage):
    """The stage's own world box, or None.

    Unioned with the model's below. Neither is enough alone: the tubes
    are Hydra-only prims no BBoxCache can see, and the model's own box
    (Tonic_GetSelectionBounds over the tube vertices) covers the CV hull
    rather than the tessellated surface, so it under-reports the groom's
    height by more than a camera can absorb.
    """
    from pxr import Usd, UsdGeom
    cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(),
                              [UsdGeom.Tokens.default_,
                               UsdGeom.Tokens.render,
                               UsdGeom.Tokens.proxy])
    aligned = cache.ComputeWorldBound(
        stage.GetPseudoRoot()).ComputeAlignedRange()
    if aligned.IsEmpty():
        return None
    lo, hi = aligned.GetMin(), aligned.GetMax()
    return ((lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2]))


def unionBoxes(boxes):
    live = [box for box in boxes if box is not None]
    if not live:
        return None
    return (tuple(min(box[0][i] for box in live) for i in range(3)),
            tuple(max(box[1][i] for box in live) for i in range(3)))


def frameBox(stage, view, box):
    """Author a three-quarter camera that frames `box`, and activate it."""
    from pxr import Gf, Sdf, UsdGeom
    lo, hi = box
    centre = Gf.Vec3d(*[0.5 * (lo[i] + hi[i]) for i in range(3)])
    extent = max(max(hi[i] - lo[i] for i in range(3)), 1e-3)
    direction = Gf.Vec3d(*DIRECTION).GetNormalized()
    eye = centre + direction * (DISTANCE * extent)
    zAxis = direction
    xAxis = Gf.Cross(Gf.Vec3d(0, 1, 0), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    matrix = Gf.Matrix4d(1.0)
    matrix.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    matrix.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    matrix.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    matrix.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicBraidShotCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateHorizontalApertureAttr(APERTURE)
    cam.CreateVerticalApertureAttr(APERTURE)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.05, float(400.0 * extent)))
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = None
    for candidate in xf.GetOrderedXformOps():
        if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform:
            op = candidate
            break
    if op is None:
        op = xf.AddTransformOp()
    op.Set(matrix)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/TonicBraidShotCamera")
    return view.getActiveSceneCamera() is not None


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import usdGenTonicTools

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = appController._plugRegistry
    container = usdGenTonicTools.container()
    if view is None or registry is None or container is None:
        print("FAIL: no stage view / plugin registry / container")
        return 1

    ui = appController._ui
    ui.primStageSplitter.setSizes([0, 1])
    ui.topBottomSplitter.setSizes([1, 0])
    # usdview's own chrome is not part of the look being reviewed: the
    # selection highlight would tint the scalp yellow over the region
    # colours, and the bounding box would draw a white wireframe through
    # the groom.
    dataModel.selection.clear()
    dataModel.viewSettings.showBBoxes = False
    ratio = float(view.devicePixelRatioF())
    view.SetPhysicalWindowSize(int(VIEW[0] * ratio), int(VIEW[1] * ratio))
    wait(50)

    # The tool, live on the committed braid: hydrate rebuilds L1/L2/L3
    # from the stage (plan/18 section 7 G3). No workspace dock here --
    # this shot is about the viewport.
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    session = container.session
    viewport = container.viewport
    if session is None or not session.hydrate(groomPath=GROOM, stage=stage):
        print("FAIL: could not hydrate %s" % GROOM)
        return 1
    workspace = container.workspace
    if workspace is not None:
        workspace.hide()

    # The look comes from THE display policy (plan/18 section 2.4a) and
    # from nothing else. It is pushed the way the tool pushes it -- mode,
    # then focus level, through the controller -- so the shot shows what
    # an artist sees and not a hand-built state: setting levels directly
    # here is exactly how a screenshot drifts away from the product, and
    # it also leaves the outgoing mode's gizmo standing in the frame.
    dll = session.dll
    mode = os.environ.get("TONIC_SHOT_MODE", "hierarchy")
    subMode = os.environ.get("TONIC_SHOT_SUBMODE", "")
    if viewport is None:
        print("FAIL: no viewport controller")
        return 1
    viewport.setMode(mode)
    if subMode:
        viewport.setSubMode(subMode)
    state = container.tonicState
    while state.activeLevel < FOCUS_LEVEL:
        viewport.enterLevel()
    info("display policy: mode %r sub %r focus %d"
         % (mode, subMode, state.activeLevel))
    if os.environ.get("TONIC_SHOT_SELECT", "") == "1":
        # One L2 child selected, so the shot shows the white control
        # curve and CV dots the section 2.4a selection row asks for.
        ids = (ctypes.c_int * 1)(1)
        subIds = (ctypes.c_int * 1)(-1)
        if dll.Tonic_SelectSet(session.model, 1, ids, subIds, None, 1) != 0:
            info("select: %s" % session.lastError())
        else:
            info("selected tube 1: %d item(s)" % session.selectionCount(1))
    # Hydrate rebuilds the tube hierarchy but not the K9/K10 guide
    # PREVIEW -- that is Fill mode's output and lives only in the live
    # model. The shot is of the tool in use, and section 2.4a's guide row
    # is about the preview, so it is filled here at full density.
    if dll.Tonic_RefillGuides(session.model, ctypes.c_float(1.0)) != 0:
        info("guide refill: %s" % session.lastError())
    # Hydrate restores the scalp graph but not the rasterised region map
    # (K3 output, live state). Without it every face reads "uncovered" and
    # the scalp paints dark red instead of the clump colours section 2.4a
    # is about.
    if dll.Tonic_Rasterise(session.model) != 0:
        info("rasterise: %s" % session.lastError())
    session.publishAll()

    box = unionBoxes((stageBounds(stage), modelBounds(session)))
    if box is None:
        print("FAIL: nothing to frame")
        return 1
    info("framing %r" % (box,))
    if not frameBox(stage, view, box):
        print("FAIL: the shot camera did not activate")
        return 1
    view.setFocus()
    # The overlay dots are sized from this camera (plan/18 section 2.4a),
    # so the scale is measured after the camera is up and before the grab.
    if viewport is not None:
        info("display scale set: %s" % viewport.syncDisplayScale())
    session.publishAll()
    for _ in range(200):
        if viewport is not None:
            viewport.pumpOnce()
        if not session.hasPendingWork():
            break
        wait(25)
    guides = ctypes.c_int(0)
    dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides), None)
    info("tubes %d, guides %d, display scale %.6f"
         % (int(dll.Tonic_GetTubeCount(session.model)), guides.value,
            session.displayScale()))

    # No manipulator in this shot: it is about the level look, and a
    # gizmo left standing by the mode the workspace opened in is not part
    # of it.
    zero3 = (ctypes.c_float * 3)(0.0, 0.0, 0.0)
    zero9 = (ctypes.c_float * 9)(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    dll.Tonic_SetGizmo(session.model, 0, zero3, zero9,
                       ctypes.c_float(1.0), -1)
    dll.Tonic_SetBrushRing(session.model, zero3, zero3, ctypes.c_float(0.0))
    session.publishAll()

    # usdview draws unconditional RGB origin axes after every frame
    # (stageView.DrawAxis: pure-green +Y, scaled by camera distance, with
    # no toggle). Under opaque tubes they are occluded; through x-rayed
    # tubes the Y axis reads as a bright green line through the groom —
    # the V9 "green sliver", which bisected to this and nothing else
    # (probeGreen.py). It is viewer chrome, not tool content, so the shot
    # turns it off. T3 pixel tests keep it: they are green with it on and
    # their measurements were taken that way.
    if hasattr(view, "DrawAxis"):
        view.DrawAxis = lambda *args, **kwargs: None

    view.update()
    view.repaint()
    view.updateGL()
    image = view.grabFrameBuffer()
    root = os.path.normpath(os.path.join(here, "..", "..", ".."))
    out = os.path.join(root, "renders",
                       os.environ.get("TONIC_SHOT_OUT",
                                      "tonic-braid-hierarchy-l2.png"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if not image.save(out, "PNG"):
        print("FAIL: could not write %s" % out)
        return 1
    print("wrote %s (%dx%d)" % (out, image.width(), image.height()))

    if viewport is not None:
        viewport.uninstall()
    session.deactivate()
    return 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: captureTonicBraidL2 needs testusdview")
    sys.exit(0)
