# testUsdviewTonicOutput -- real Output description and amplified-hair
# acceptance through usdview's installed Tonic workspace.
#
# The test deliberately uses the same artist route as the other T3 fixtures:
# bind a scalp, draw a closed region, subdivide its root from the dock, and
# sculpt a visible child with a real StageView drag.  Output is then built by
# the public session action, not by authoring USD attributes in the test.
import ctypes
import os
import sys

failures = 0


def frameSurface(stage, view, center):
    """Frame either shipped scalp fixture through the same top camera."""
    from pxr import Gf, Sdf, UsdGeom
    cx, cz = center
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicOutputCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.05, 500.0))
    zAxis = Gf.Vec3d(0.0, 1.0, 0.0)
    xAxis = Gf.Vec3d(1.0, 0.0, 0.0)
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(cx, 6.0, cz, 1.0))
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = next((candidate for candidate in xf.GetOrderedXformOps()
               if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform),
              None)
    (op if op is not None else xf.AddTransformOp()).Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/TonicOutputCamera")
    return view.getActiveSceneCamera() is not None


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
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    argv = list(sys.argv)
    for index, arg in enumerate(argv):
        if arg == "--testScript" and index + 1 < len(argv):
            return os.path.dirname(os.path.abspath(argv[index + 1]))
        if arg.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(arg.split("=", 1)[1]))
    return ""


def _qtTest():
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    return importlib.import_module("%s.QtTest" % PySideModule)


def wait(milliseconds=30):
    _qtTest().QTest.qWait(int(milliseconds))


def typeKey(view, name):
    from pxr.Usdviewq.qt import QtCore
    keys = {"d": QtCore.Qt.Key.Key_D, "5": QtCore.Qt.Key.Key_5}
    _qtTest().QTest.keyClick(view, keys[name],
                             QtCore.Qt.KeyboardModifier.NoModifier)


def pump(viewport, session, tries=160):
    for _ in range(tries):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            return True
        wait(25)
    return False


def idleTimerStopped(viewport):
    timer = getattr(viewport, "_timer", None)
    return timer is None or not bool(timer.isActive())


def waitForAutomaticIdle(viewport, session, predicate=lambda: True,
                         tries=160, requireCommitted=True):
    """Let Qt's real idle timer commit a panel action without test pumping."""
    for _ in range(tries):
        committed = session.committedVersion == session.modelVersion
        if ((committed or not requireCommitted) and
                not session.hasPendingWork() and
                idleTimerStopped(viewport) and predicate()):
            return True
        # Deliberately only process the Qt event loop: invoking pumpOnce,
        # session.pump or scheduleIdle here would hide a dormant timer bug.
        wait(25)
    return False


def tubeIds(session):
    count = ctypes.c_int(0)
    if session.dll.Tonic_ReadTubeIds(session.model, None, 0,
                                    ctypes.byref(count)) != 0:
        return []
    values = (ctypes.c_int * max(int(count.value), 1))()
    if session.dll.Tonic_ReadTubeIds(session.model, values, len(values),
                                    ctypes.byref(count)) != 0:
        return []
    return [int(values[index]) for index in range(int(count.value))]


def centers(session, tubeId):
    count = int(session.dll.Tonic_GetTubeCenterCount(session.model,
                                                     int(tubeId)))
    result = []
    for cv in range(max(count, 0)):
        point = (ctypes.c_float * 3)()
        if session.dll.Tonic_GetTubeCenterCV(session.model, int(tubeId), cv,
                                             point) != 0:
            return []
        result.append(tuple(float(point[axis]) for axis in range(3)))
    return result


def changed(before, after, epsilon=1e-5):
    return len(before) == len(after) and any(
        abs(before[index][axis] - after[index][axis]) > epsilon
        for index in range(len(before)) for axis in range(3))


def curvePayload(prim):
    if not prim:
        return [], []
    points = prim.GetAttribute("points").Get() or []
    counts = prim.GetAttribute("curveVertexCounts").Get() or []
    return list(points), list(counts)


def cagePayload(prim):
    """The complete authored sparse cage, excluding transient cooked hair."""
    if not prim:
        return None
    names = (
        "usdGen:surfaceCage:ownerIds",
        "usdGen:surfaceCage:ownerDensities",
        "usdGen:surfaceCage:ownerSeeds",
        "usdGen:surfaceCage:ownerCvCounts",
        "usdGen:surfaceCage:ownerEdgeBias",
        "usdGen:surfaceCage:ownerChartCentroids",
        "usdGen:surfaceCage:ownerChartMeanRadii",
        "usdGen:surfaceCage:ownerLengthProfileOffsets",
        "usdGen:surfaceCage:ownerLengthProfile",
        "usdGen:surfaceCage:triangles",
        "usdGen:surfaceCage:triangleOwnerIndices",
        "usdGen:surfaceCage:triangleRootCharts",
        "usdGen:surfaceCage:normalizedT")
    points, counts = curvePayload(prim)
    values = []
    for name in names:
        value = prim.GetAttribute(name).Get()
        if value is None:
            return None
        values.append(tuple(value))
    return tuple(points), tuple(counts), tuple(values)


def outputMapPayload(prim):
    if not prim:
        return None
    asset = prim.GetAttribute("usdGen:map:file").Get()
    generation = prim.GetAttribute("usdGen:map:textureGeneration").Get()
    owned = prim.GetAttribute("usdGen:tonic:outputOwned").Get()
    if asset is None or generation is None or not bool(owned):
        return None
    resolved = asset.resolvedPath or asset.path
    if not resolved or not os.path.isfile(resolved):
        return None
    return asset.path, int(generation)


def sourceCageContract(sourceOp, mapPrim):
    if not sourceOp or not mapPrim:
        return False
    mapPayload = outputMapPayload(mapPrim)
    return bool(mapPayload and
                str(sourceOp.GetAttribute("usdGen:interpolationMode").Get()) ==
                "surfaceCage" and
                int(sourceOp.GetAttribute("usdGen:regionMapChannel").Get()) == 0 and
                int(sourceOp.GetAttribute("usdGen:expectMapGeneration").Get()) ==
                mapPayload[1] and
                sourceOp.GetRelationship("usdGen:regionMap").GetTargets() ==
                [mapPrim.GetPath()])


def imageSignature(view):
    """Small deterministic framebuffer signature for scene-index output."""
    view.update()
    view.repaint()
    view.updateGL()
    image = view.grabFrameBuffer()
    samples = []
    for y in range(0, image.height(), max(image.height() // 96, 1)):
        for x in range(0, image.width(), max(image.width() // 96, 1)):
            samples.append(int(image.pixel(x, y)) & 0x00ffffff)
    return tuple(samples)


def waitForImage(view, predicate, tries=120):
    """Wait for the independent usdGen scene cook to reach the framebuffer.

    Tonic's commit pump only swaps its authoring layer.  The description
    cooker publishes its render tiles asynchronously through the usdGen
    scene index, so accepting the commit alone is not evidence of hair.
    """
    last = imageSignature(view)
    if predicate(last):
        return last
    for _ in range(tries):
        wait(25)
        last = imageSignature(view)
        if predicate(last):
            return last
    return None


def stableImage(view, tries=20):
    """Return a framebuffer sample after two consecutive rendered frames agree."""
    previous = imageSignature(view)
    for _ in range(tries):
        wait(25)
        current = imageSignature(view)
        if current == previous:
            return current
        previous = current
    return previous


def imageDifferenceCount(before, after):
    if len(before) != len(after):
        return max(len(before), len(after))
    return sum(left != right for left, right in zip(before, after))


def imageDifferenceStats(before, after):
    """Return changed sample count plus RGB delta totals for failure output."""
    count = imageDifferenceCount(before, after)
    if len(before) != len(after):
        return count, -1, -1
    deltas = []
    for left, right in zip(before, after):
        deltas.append(abs((left & 0xff) - (right & 0xff)) +
                      abs(((left >> 8) & 0xff) - ((right >> 8) & 0xff)) +
                      abs(((left >> 16) & 0xff) - ((right >> 16) & 0xff)))
    return count, sum(deltas), max(deltas, default=0)


def saveFrame(view, name):
    """Keep failure artifacts in CTest's temporary directory for inspection."""
    directory = os.path.join(os.getcwd(), "Testing", "Temporary")
    try:
        os.makedirs(directory, exist_ok=True)
        path = os.path.join(directory, name)
        return bool(view.grabFrameBuffer().save(path))
    except Exception as error:
        info("could not save %s: %s" % (name, error))
        return False


def action(workspace, text):
    from pxr.Usdviewq.qt import QtWidgets
    for button in workspace.findChildren(QtWidgets.QAbstractButton):
        if button.text().split(" (", 1)[0] == text:
            button.click()
            wait(20)
            return True
    return False


def setParameter(workspace, identifier, value):
    """Change one real Output panel widget by its public descriptor id."""
    for descriptor, widget in getattr(workspace, "_paramWidgets", ()):
        if descriptor.id != identifier:
            continue
        setter = getattr(widget, "setValue", None)
        if setter is None:
            return False
        setter(float(value))
        wait(20)
        return True
    return False


def run(appController):
    global failures
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import tonicBridge, tonicCamera
        from testUsdviewTonicGraph import Mouse
        from testUsdviewTonicSculpt import aimCamera
    except ImportError as exc:
        print("FAIL: cannot import Tonic test dependencies: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    container = usdGenTonicTools.container()
    check(view is not None and registry is not None and container is not None,
          "usdview exposes the Tonic workspace and StageView")
    if view is None or registry is None or container is None:
        return 1
    if stage.GetPrimAtPath("/Scalp"):
        scalpPath, surfaceCenter = "/Scalp", (2.0, 2.0)
    elif stage.GetPrimAtPath("/Plane"):
        scalpPath, surfaceCenter = "/Plane", (0.0, 0.0)
    else:
        check(False, "the fixture provides /Scalp or the user test /Plane")
        return 1
    cx, cz = surfaceCenter
    rect = ((cx - 1.0, cz - 1.0), (cx + 1.0, cz - 1.0),
            (cx + 1.0, cz + 1.0), (cx - 1.0, cz + 1.0))
    # Render/FPS HUD text changes every paint and must not satisfy the Output
    # imaging assertion.  Keep it off for this visual acceptance fixture.
    priorHud = bool(dataModel.viewSettings.showHUD)
    dataModel.viewSettings.showHUD = False
    check(frameSurface(stage, view, surfaceCenter),
          "the output fixture frames its scalp")
    view.setFocus()
    wait(40)

    dataModel.selection.setPrimPath(scalpPath)
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    workspace = container.workspace
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    check(workspace is not None and session is not None and
          session.model is not None and viewport is not None and
          viewport.installed,
          "Bind scalp opens the real live Tonic session")
    if workspace is None or session is None or session.model is None or \
            viewport is None:
        return 1

    def shutdown():
        try:
            viewport.uninstall()
        finally:
            session.deactivate()
            dataModel.viewSettings.showHUD = priorHud

    # Keep the authored support prim alive: the Output CurveSource keeps a
    # relationship to it while the usdGen cook resolves rest bindings.  A
    # visibility opinion removes it from the framebuffer without invalidating
    # that relationship (SetActive(False) would reject a later cook).
    from pxr import UsdGeom
    UsdGeom.Imageable(stage.GetPrimAtPath(scalpPath)).CreateVisibilityAttr(
        UsdGeom.Tokens.invisible)
    wait(40)
    camera = tonicCamera.resolve(view)
    check(camera is not None, "the live StageView camera resolves")
    if camera is None:
        shutdown()
        return 1
    mouse = Mouse(view)
    mouse.direct = True
    viewport.setPointerInside(True)
    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((cx, 0.0, cz)), 1e-9), 2.0)

    # A real closed Graph drag creates the artist region and its root stub.
    typeKey(view, "d")
    path = []
    for index, (x0, z0) in enumerate(rect):
        x1, z1 = rect[(index + 1) % len(rect)]
        for step in range(5):
            t = float(step) / 5.0
            projected = camera.worldToPixels((x0 + (x1 - x0) * t, 0.0,
                                               z0 + (z1 - z0) * t))
            if projected is not None:
                path.append((projected[0], projected[1]))
    first = camera.worldToPixels((rect[0][0], 0.0, rect[0][1]))
    if first is not None:
        path.append((first[0], first[1]))
    mouse.drag(path)
    check(session.graphCounts() == (4, 4, 1) and tubeIds(session) == [0],
          "a real Graph region creates its root tube")
    if tubeIds(session) != [0]:
        shutdown()
        return 1

    # Use the dock action for subdivision so Output consumes a sculpted L2
    # child rather than an artificial descriptor injected by the test.
    # The top view looks straight into the tube's open end.  Turn side-on
    # before the real hierarchy body click so its ray crosses a broad wall.
    check(aimCamera(stage, view, (cx + 11.0, 1.5, cz),
                    (cx, 1.5, cz)),
          "the hierarchy camera exposes the root tube wall")
    wait(40)
    camera = tonicCamera.resolve(view)
    workspace._modeButtons["hierarchy"].click()
    wait(20)
    rootHandle = tonicBridge.tubeCenterHandle(session.dll, session.model, 0, 1)
    rootPixel = camera.worldToPixels(rootHandle) if camera is not None else None
    if rootPixel is not None:
        mouse.click(rootPixel)
    check(action(workspace, "Subdivide"),
          "the hierarchy dock exposes Subdivide for the selected root")
    children = [tube for tube in tubeIds(session) if tube != 0]
    check(len(children) >= 2,
          "subdivision creates visible child tubes for sculpt output (%r)"
          % children)
    if len(children) < 2:
        shutdown()
        return 1
    child = int(children[0])

    # Sculpt a child through a real brush event.  Resolve its displayed
    # center handle after the hierarchy action, then use its live screen
    # coordinate for the press and drag.
    workspace._modeButtons["sculpt"].click()
    wait(20)
    camera = tonicCamera.resolve(view)
    beforeChild = centers(session, child)
    handle = tonicBridge.tubeCenterHandle(session.dll, session.model, child,
                                          max(len(beforeChild) - 2, 1))
    start = camera.worldToPixels(handle) if camera is not None else None
    state.brushRadiusPx = 60.0
    if start is not None:
        mouse.press(start)
        mouse.move((start[0] + 30.0, start[1]))
        mouse.release((start[0] + 30.0, start[1]))
    afterChild = centers(session, child)
    check(start is not None and changed(beforeChild, afterChild),
          "a real Sculpt child stroke changes the source tube")

    # Build through Luna's public session action.  The native settings are
    # then the sole source for the committed description and worker cook.
    workspace._modeButtons["output"].click()
    wait(20)
    check(waitForAutomaticIdle(viewport, session, requireCommitted=False),
          "the Output action starts from an idle stopped viewport timer")
    check(session.outputSettingsAvailable(),
          "the active DLL exposes atomic Output settings")
    check(session.setOutputSettings(enabled=False, densityMultiplier=1.0,
                                    strandWidth=0.02, publish=True,
                                    enqueue=False),
          "Output settings accept the full-Fill baseline")
    check(action(workspace, "Build/update description"),
          "the public Build/update description action queues Output")
    check(waitForAutomaticIdle(
        viewport, session,
        lambda: bool(stage.GetPrimAtPath("/TonicGroom/Output")) and
        bool(stage.GetPrimAtPath("/TonicGroom/OutputCurves"))),
          "the real idle timer commits the Output description")

    output = stage.GetPrimAtPath("/TonicGroom/Output")
    source = stage.GetPrimAtPath("/TonicGroom/OutputCurves")
    outputMap = stage.GetPrimAtPath("/TonicGroom/OutputRegionMap")
    sparseSource = cagePayload(source)
    check(bool(output) and bool(source) and bool(outputMap) and sparseSource,
          "the stage has the owned sparse OutputCurves, OutputRegionMap and Output triple")
    check(sum(sparseSource[1]) == len(sparseSource[0]) and len(sparseSource[1]) > 0,
          "the output source contains complete sparse cage rails")
    sourceOp = stage.GetPrimAtPath("/TonicGroom/Output/Ops/source")
    widthOp = stage.GetPrimAtPath("/TonicGroom/Output/Ops/width")
    sourceTargets = (sourceOp.GetRelationship("usdGen:curves").GetTargets()
                     if sourceOp else [])
    committedWidth = (widthOp.GetAttribute("usdGen:width").Get()
                      if widthOp else None)
    check(sourceTargets == [source.GetPath()] and
          sourceCageContract(sourceOp, outputMap) and
          abs(float(committedWidth) - 0.02) < 1e-6,
          "Output owns a surface-cage CurveSource-to-Width graph over its map")
    enabled, multiplier, width = session.outputSettings()
    check(enabled and abs(multiplier - 1.0) < 1e-5 and
          abs(width - 0.02) < 1e-5,
          "the committed Output settings round-trip through the session")

    # Output sampling is independent of the preview cache.  Clear guides
    # after the description exists, commit again, and retain source data.
    beforeClear = sparseSource
    check(session.dll.Tonic_ClearGeneratedCurves(session.model) == 0 and
          session.enqueueCommit() and pump(viewport, session),
          "guides clear and the Output description recommits")
    sparseSource = cagePayload(stage.GetPrimAtPath("/TonicGroom/OutputCurves"))
    check(sparseSource == beforeClear,
          "clearing preview guides never erases authored sparse Output rails")

    # Density is a runtime CurveSource value.  Edit the committed USD source
    # directly: the engine must recook denser transient hair without sending
    # anything through Tonic or mutating the authored cage/map payload.
    from pxr import Sdf, Usd
    mapBeforeDensity = outputMapPayload(outputMap)
    # Output visibility is controlled by the real workspace display mode;
    # merely toggling its checkbox while Sculpt owns the viewport does not
    # request its scene-index population.
    workspace._modeButtons["output"].click()
    wait(20)
    workspace._amplifiedCheck.setChecked(False)
    hiddenForDensity = stableImage(view)
    workspace._amplifiedCheck.setChecked(True)
    imageBeforeDensity = waitForImage(
        view, lambda image: imageDifferenceCount(hiddenForDensity, image) >=
        max(24, len(image) // 500))
    # The live Tonic commit layer is stronger than the stage's root layer.
    # Author the artist-side runtime value into a temporary *stronger* session
    # sublayer, then remove that opinion before the panel commits again.
    originalTarget = stage.GetEditTarget()
    densityLayer = Sdf.Layer.CreateAnonymous("tonic-output-runtime-density")
    sessionLayer = stage.GetSessionLayer()
    priorSubLayers = list(sessionLayer.subLayerPaths)
    sessionLayer.subLayerPaths = [densityLayer.identifier] + priorSubLayers
    stage.SetEditTarget(Usd.EditTarget(densityLayer))
    directDensity = bool(sourceOp and
                         sourceOp.GetAttribute("usdGen:densityMultiplier").Set(4.0))
    composedDensity = (sourceOp.GetAttribute("usdGen:densityMultiplier").Get()
                       if sourceOp else None)
    denserImage = (waitForImage(
        view, lambda image: imageBeforeDensity is not None and
        imageDifferenceCount(imageBeforeDensity, image) >=
        max(12, len(image) // 2000)) if directDensity else None)
    sparseAfterDensity = cagePayload(source)
    directDensityOk = (imageBeforeDensity is not None and directDensity and
                       composedDensity is not None and
                       abs(float(composedDensity) - 4.0) < 1e-6 and
                       denserImage is not None and
          sparseAfterDensity == beforeClear and
          outputMapPayload(outputMap) == mapBeforeDensity and
          sourceCageContract(sourceOp, outputMap))
    if not directDensityOk:
        # Keep the visual proof diagnostic separate from the authored-state
        # checks: a composed USD value without a changed framebuffer points to
        # an imaging invalidation defect, while a missing baseline points to
        # a fixture visibility problem.
        baselineStats = (imageDifferenceStats(hiddenForDensity, imageBeforeDensity)
                         if imageBeforeDensity is not None else None)
        finalDensityImage = imageSignature(view)
        recookStats = (imageDifferenceStats(imageBeforeDensity, finalDensityImage)
                       if imageBeforeDensity is not None else None)
        info("direct density source=%r composed=%r baseline=%r recook=%r "
             "image=%r cageSame=%r mapSame=%r" %
             (directDensity, composedDensity, baselineStats, recookStats,
              denserImage is not None, sparseAfterDensity == beforeClear,
              outputMapPayload(outputMap) == mapBeforeDensity))
        saveFrame(view, "tonic-output-density-direct.png")
    check(directDensityOk,
          "direct source density recooks amplified output without a Tonic cage or map rebake")
    stage.SetEditTarget(originalTarget)
    sessionLayer.subLayerPaths = priorSubLayers

    # Width uses the same panel-to-session wake path as density.  The stage
    # graph is the committed evidence, rather than a direct model query.
    # Amplified-display state is presentation-only and advances the native
    # revision without requiring a description commit.  Width still must
    # start from a quiescent real idle timer, but need not reconcile that
    # display-only revision first.
    check(waitForAutomaticIdle(viewport, session, requireCommitted=False),
          "the width action starts from a stopped idle timer")
    widthWidget = setParameter(workspace, "outputStrandWidth", 0.035)
    widthCommitted = (waitForAutomaticIdle(
        viewport, session,
        lambda: abs(float(widthOp.GetAttribute("usdGen:width").Get()) -
                    0.035) < 1e-6)
        if widthWidget else False)
    check(widthWidget and widthCommitted,
          "the Output width widget commits through the real idle timer")

    # A later child sculpt must propagate into the description on commit.
    workspace._modeButtons["sculpt"].click()
    wait(20)
    camera = tonicCamera.resolve(view)
    beforeOutput = cagePayload(source)
    beforeChild = centers(session, child)
    handle = tonicBridge.tubeCenterHandle(session.dll, session.model, child,
                                          max(len(beforeChild) - 2, 1))
    start = camera.worldToPixels(handle) if camera is not None else None
    if start is not None:
        mouse.press(start)
        mouse.move((start[0] - 26.0, start[1] + 14.0))
        mouse.release((start[0] - 26.0, start[1] + 14.0))
    check(start is not None and changed(beforeChild, centers(session, child)),
          "a later real child sculpt changes its descriptor source")
    check(session.enqueueCommit() and pump(viewport, session),
          "the sculpted child refreshes the committed Output description")
    afterOutput = cagePayload(stage.GetPrimAtPath("/TonicGroom/OutputCurves"))
    check(afterOutput != beforeOutput,
          "the committed Output curve samples follow the child sculpt")

    # Show/hide is read through the real workspace control.  The framebuffer
    # delta proves the generated usdGen imaging scene changes, while the
    # authored helper source remains hidden and therefore is never mistaken
    # for the rendered amplified result.
    workspace._modeButtons["output"].click()
    wait(20)
    workspace._amplifiedCheck.setChecked(False)
    # Establish a stable no-tiles image before requesting an output cook.
    # A meaningful sample delta proves rendered amplified strands rather than
    # merely a repaint or a completed authoring transaction.
    hiddenImage = stableImage(view)
    saveFrame(view, "tonic-output-hidden.png")
    minimumHairPixels = max(24, len(hiddenImage) // 500)
    workspace._amplifiedCheck.setChecked(True)
    shownImage = waitForImage(
        view, lambda image: imageDifferenceCount(hiddenImage, image) >=
        minimumHairPixels)
    check(bool(session.dll.Tonic_GetAmplifiedHair(session.model)) and
          shownImage is not None,
          "Show amplified hair changes the real usdGen imaging output")
    saveFrame(view, "tonic-output-shown.png")
    workspace._amplifiedCheck.setChecked(False)
    restoredImage = waitForImage(view, lambda image: image == hiddenImage)
    if restoredImage is None:
        finalImage = imageSignature(view)
        changedSamples, totalDelta, maximumDelta = imageDifferenceStats(
            hiddenImage, finalImage)
        info("amplified hide framebuffer did not restore baseline: changed "
             "samples=%d/%d totalRgbDelta=%d maxRgbDelta=%d; artifacts "
             "Testing/Temporary/tonic-output-{hidden,shown,restored}.png" %
             (changedSamples, len(hiddenImage), totalDelta, maximumDelta))
        saveFrame(view, "tonic-output-restored.png")
    check(not bool(session.dll.Tonic_GetAmplifiedHair(session.model)) and
          restoredImage is not None,
          "hiding amplified hair restores the scene without exposing source")
    visibility = source.GetAttribute("visibility").Get() if source else None
    check(str(visibility) == "invisible",
          "the /TonicGroom/OutputCurves helper source stays hidden from imaging")

    shutdown()
    print("testUsdviewTonicOutput: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no StageView for Output acceptance")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicOutput needs testusdview")
    sys.exit(0)
