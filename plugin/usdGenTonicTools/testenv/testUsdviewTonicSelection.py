# testUsdviewTonicSelection -- T3 selection acceptance through the real Qt
# StageView filter.  This deliberately never calls session.select(): every
# selection below is produced by the mouse path an artist uses.
import ctypes
import math
import os
import sys

failures = 0

# A clockwise square in the middle of the 4x4 XZ scalp fixture.
RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
STUB_CVS = 5


def frameComponentTargets(stage, view):
    """Frame an open tube almost end-on, with its center CVs separable.

    ``frameScalp`` intentionally looks exactly along the stub's +Y center
    line.  That remains useful for the later framebuffer assertion, but it
    overlays the center dots in screen space.  This slight oblique view still
    sees through the open tip while giving each displayed CV its own target.
    """
    from pxr import Gf, Sdf, UsdGeom

    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicSelectionCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(5.5, 12.0, 2.0)
    target = Gf.Vec3d(2.0, 2.0, 2.0)
    zAxis = (eye - target).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0.0, 0.0, 1.0), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = next((candidate for candidate in xf.GetOrderedXformOps()
               if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform),
              None)
    if op is None:
        op = xf.AddTransformOp()
    op.Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/TonicSelectionCamera")
    return view.getActiveSceneCamera() is not None


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def wait(ms=30):
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    importlib.import_module("%s.QtTest" % PySideModule).QTest.qWait(int(ms))


def testenvDir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for index, argument in enumerate(sys.argv):
        if argument == "--testScript" and index + 1 < len(sys.argv):
            return os.path.dirname(os.path.abspath(sys.argv[index + 1]))
        if argument.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(argument.split("=", 1)[1]))
    return ""


def hover(session):
    """The model's current highlight, set only by the real hover event."""
    kind = ctypes.c_uint(0)
    ident = ctypes.c_int(-1)
    sub = ctypes.c_int(-1)
    subsub = ctypes.c_int(-1)
    rc = session.dll.Tonic_GetHover(session.model, ctypes.byref(kind),
                                    ctypes.byref(ident), ctypes.byref(sub),
                                    ctypes.byref(subsub))
    if rc != 0:
        return None
    return (int(kind.value), int(ident.value), int(sub.value),
            int(subsub.value))


def sectionCV(session, tubeId, ring, slot):
    """World position of one live section CV through the public C ABI."""
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

    def placed(pair):
        u, v = pair[0] * scale.value, pair[1] * scale.value
        return (u * ct - v * st, u * st + v * ct)

    placedRing = [placed(pair) for pair in section[1]]
    # Tonic_GetTubeSectionFrame returns the ring centroid as origin.  Center
    # the transformed authored UVs before applying that frame.
    meanU = sum(pair[0] for pair in placedRing) / len(placedRing)
    meanV = sum(pair[1] for pair in placedRing) / len(placedRing)
    u, v = placedRing[slot]
    return tuple(origin[axis] + frame[axis] * (u - meanU) +
                 frame[axis + 3] * (v - meanV) for axis in range(3))


def sectionRingCenter(session, tubeId, ring):
    """The public frame API's origin is the pickable ring centroid."""
    entry = session.dll.Tonic_GetTubeSectionFrame
    cfloat3 = ctypes.POINTER(ctypes.c_float)
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, cfloat3,
                      cfloat3, cfloat3, cfloat3]
    entry.restype = ctypes.c_int
    origin = (ctypes.c_float * 3)()
    frame = (ctypes.c_float * 9)()
    if entry(session.model, int(tubeId), int(ring), origin, frame, None,
             None) != 0:
        return None
    return tuple(float(value) for value in origin)


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import (tonicBridge, tonicCamera,
                                      tonicHierarchy, tonicLib)
        # Reuse only the Qt event/camera/framebuffer mechanics.  This test
        # owns the selection assertions and does not execute testTube's run.
        from testUsdviewTonicTube import (Mouse, frameScalp,
                                          typeKey, whiteFraction)
    except ImportError as exc:
        print("FAIL: cannot import Tonic selection helpers: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    container = usdGenTonicTools.container()
    check(view is not None and registry is not None and container is not None,
          "usdview supplies a StageView and Tonic command registry")
    if view is None or registry is None or container is None:
        return 1

    def shutdown():
        viewport = getattr(container, "viewport", None)
        session = getattr(container, "session", None)
        try:
            if viewport is not None:
                viewport.uninstall()
        finally:
            if session is not None:
                session.deactivate()

    check(frameScalp(stage, view), "a stable top camera frames the scalp")
    view.setFocus()
    wait(30)
    try:
        rolloverBefore = bool(view.rolloverPicking)
    except AttributeError:
        rolloverBefore = None

    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    workspace = container.workspace
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    check(workspace is not None and workspace.isVisible() and
          session is not None and session.model is not None and
          viewport is not None and viewport.installed,
          "opening the dock binds a live model and viewport controller")
    if session is None or session.model is None or viewport is None:
        shutdown()
        return 1

    # The controller owns usdview rollover while its workspace is visible:
    # Tonic's own hover/highlight must be the only mouse-over presentation.
    if rolloverBefore is not None:
        check(not bool(view.rolloverPicking),
              "the visible workspace suppresses StageView rollover picking")
    check(viewport._marqueeOverlay is not None and
          viewport._marqueeOverlay.testAttribute(
              viewport._marqueeOverlay.Qt.WA_TransparentForMouseEvents)
          if hasattr(viewport._marqueeOverlay, "Qt") else
          viewport._marqueeOverlay is not None,
          "the selection-band overlay is installed as a mouse-transparent child")

    # The scalp is copied at bind time. Hiding its USD prim avoids z fighting
    # without changing the model's K1 scalp used by the Graph stroke.
    stage.GetPrimAtPath("/Scalp").SetActive(False)
    camera = tonicCamera.resolve(view)
    check(camera is not None, "the live controller resolves a camera")
    if camera is None:
        shutdown()
        return 1

    def pixel(point):
        projected = camera.worldToPixels(point)
        return (projected[0], projected[1])

    def centerHandle(tubeId, cv):
        """The centered core point artists see and can click."""
        return tonicBridge.tubeCenterHandle(session.dll, session.model,
                                            tubeId, cv)

    def clickControl(text):
        """Invoke a visible dock action through its Qt button."""
        from pxr.Usdviewq.qt import QtWidgets
        for button in workspace.findChildren(QtWidgets.QAbstractButton):
            if button.text().split(" (", 1)[0] == text:
                button.click()
                wait(15)
                return True
        return False

    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)
    mouse = Mouse(view)
    # This is still a real QMouseEvent delivered through the installed Qt
    # filter; it avoids the hidden test window's global-coordinate routing.
    mouse.direct = True
    viewport.setPointerInside(True)

    # Create one visible tube using the ordinary Graph freehand workflow.
    typeKey(view, "d")
    path = []
    for index, (x0, z0) in enumerate(RECT):
        x1, z1 = RECT[(index + 1) % len(RECT)]
        for step in range(5):
            t = float(step) / 5.0
            path.append(pixel((x0 + (x1 - x0) * t, 0.0,
                               z0 + (z1 - z0) * t)))
    path.append(pixel((RECT[0][0], 0.0, RECT[0][1])))
    mouse.drag(path)
    check(session.graphCounts() == (4, 4, 1) and
          int(session.dll.Tonic_GetCenterCVCount(session.model)) == STUB_CVS,
          "a real Graph stroke creates the visible five-CV root tube")

    typeKey(view, "2")
    check(state.activeMode == "tube" and state.tubeSubMode == "center",
          "Tube Center owns the ensuing click, hover and marquee input")
    tip = centerHandle(0, STUB_CVS - 1)
    check(tip is not None, "the visible root-tip center CV is readable")
    if tip is None:
        shutdown()
        return 1

    # Graph Snap can be deliberately tiny without shrinking the direct
    # manipulation target of an already displayed CV.  Six physical pixels
    # is inside the fixed 8px component target and intentionally outside the
    # former 2px generic pick floor.  The normal scalp camera is deliberately
    # end-on, so use an almost-end-on view which separates the dots before
    # checking an exact CV identity.
    check(frameComponentTargets(stage, view),
          "an oblique open-tube camera makes center CV targets distinct")
    wait(30)
    camera = tonicCamera.resolve(view)
    check(camera is not None, "the component-target camera resolves")
    if camera is None:
        shutdown()
        return 1
    tipPixel = pixel(tip)
    priorPixel = pixel(centerHandle(0, STUB_CVS - 2))
    tipGap = ((tipPixel[0] - priorPixel[0]) ** 2 +
              (tipPixel[1] - priorPixel[1]) ** 2) ** 0.5
    check(tipGap > 16.0,
          "the visible tip has a separate screen target (%.2fpx)" % tipGap)
    outward = ((tipPixel[0] - priorPixel[0]) / max(tipGap, 1e-9),
               (tipPixel[1] - priorPixel[1]) / max(tipGap, 1e-9))
    state.snapRadiusPx = 0.25
    tipNearPixel = (tipPixel[0] + 6.0 * outward[0],
                    tipPixel[1] + 6.0 * outward[1])

    # A press-less move travels through the real hover route.  It must
    # identify the same center CV a click would take and clear off-target.
    mouse.move(tipNearPixel)
    wait(15)
    check(hover(session) == (tonicLib.TONIC_PICK_CENTER_CV, 0,
                             STUB_CVS - 1, -1),
          "an 8px CV target prehighlights the exact per-tube tip under tiny Snap")
    mouse.move((10.0, 10.0))
    wait(15)
    check(hover(session) == (0, -1, -1, -1),
          "moving off the tube clears the Tonic highlight")

    mouse.click(tipNearPixel)
    nearSelection = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
    check(nearSelection == [(0, STUB_CVS - 1, -1)] and
          not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT),
          "the offset CV click takes the CV rather than its tube body %r" %
          (nearSelection,))
    typeKey(view, "escape")

    # Restore the end-on view for the framebuffer colour assertion below.
    check(frameScalp(stage, view),
          "the end-on camera is restored for the visual selection check")
    wait(30)
    camera = tonicCamera.resolve(view)
    check(camera is not None, "the restored selection camera resolves")
    if camera is None:
        shutdown()
        return 1
    tipPixel = pixel(tip)

    # Actual click, not a test-side selection call: it must select the tip
    # and publish the expected white visual selection highlight.
    whiteBefore = whiteFraction(view, camera, tip)
    mouse.click(tipPixel)
    selected = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
    whiteAfter = whiteFraction(view, camera, tip)
    check(selected == [(0, STUB_CVS - 1, -1)],
          "a visible center-CV click selects exactly that tube and CV %r"
          % (selected,))
    check(not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) and
          not session.readSelection(tonicLib.TONIC_PICK_SECTION_RING),
          "the visible CV click does not retain a whole tube or ring")
    check(whiteAfter > whiteBefore + 0.05,
          "the click's selected CV is visibly highlighted %.2f -> %.2f"
          % (whiteBefore, whiteAfter))

    # Escape is the real UI path for clearing this ordinary selection.
    typeKey(view, "escape")
    check(not session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),
          "Escape clears the clicked selection before the marquee")

    # Shift always reserves a drag for the selection band, including when
    # the pointer crosses a tube.  Observe the actual QRubberBand mid-drag,
    # then require release to select the center candidates inside it.
    start = (tipPixel[0] - 90.0, tipPixel[1] - 90.0)
    finish = (tipPixel[0] + 90.0, tipPixel[1] + 90.0)
    mouse.press(start, ("shift",))
    mouse.move(finish, ("shift",))
    wait(10)
    check(viewport._marqueeOverlay.isVisible(),
          "a real Shift drag shows the mouse-transparent selection band")
    mouse.release(finish, ("shift",))
    marqueeSelection = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
    expected = [(0, index, -1) for index in range(STUB_CVS)]
    check(marqueeSelection == expected,
          "the real marquee selects every visible root center candidate %r"
          % (marqueeSelection,))
    check(not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) and
          not session.readSelection(tonicLib.TONIC_PICK_SECTION_RING),
          "the component marquee drops stale whole-tube and ring selection")
    check(not viewport._marqueeOverlay.isVisible(),
          "releasing the marquee removes its transient Qt overlay")

    # The explicit Lasso shape is a plain drag as well as Shift-add.  The
    # physical-pixel polygon below encloses the same displayed CVs as the
    # box, while the translucent child remains mouse-transparent to the
    # StageView event filter.
    typeKey(view, "escape")
    state.selectionShape = "lasso"
    lasso = [(tipPixel[0] - 90.0, tipPixel[1] - 90.0),
             (tipPixel[0] + 90.0, tipPixel[1] - 90.0),
             (tipPixel[0] + 90.0, tipPixel[1] + 90.0),
             (tipPixel[0] - 90.0, tipPixel[1] + 90.0)]
    mouse.press(lasso[0])
    for point in lasso[1:]:
        mouse.move(point)
    wait(10)
    check(viewport._lassoOverlay.isVisible(),
          "a real Lasso drag paints its mouse-transparent polygon")
    mouse.release(lasso[0])
    lassoSelection = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
    check(lassoSelection == expected,
          "the real Lasso selects visible center CVs through SelectPolygon %r"
          % (lassoSelection,))
    check(not session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) and
          not session.readSelection(tonicLib.TONIC_PICK_SECTION_RING),
          "the component Lasso never silently adds a whole tube or ring")
    check(not viewport._lassoOverlay.isVisible(),
          "releasing the Lasso removes its transient Qt overlay")
    state.selectionShape = "box"

    # Now exercise the exact same Qt routes against a child tube's ring and
    # section CV.  This is deliberately after the root proof: the flattened
    # K11 candidate stream has to preserve the child owner, rather than
    # quietly spelling every section item as tube 0.
    # Select and subdivide the visible root through the Hierarchy dock.  The
    # UI action expands the active-cut branch; native child creation followed
    # by a global level change leaves this branch hidden from Tube picking.
    workspace._modeButtons["hierarchy"].click()
    wait(15)
    rootPoint = sectionCV(session, 0, 1, 0)
    rootPixel = pixel(rootPoint) if rootPoint is not None else None
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
    check(bool(children), "the root subdivides before child section picking")
    if children:
        entered = clickControl("Enter level")
        check(entered and int(state.activeLevel) >= 2,
              "the Hierarchy dock enters the created child level")
        child = int(children[0])
        ring = 1
        try:
            ringPoint = sectionRingCenter(session, child, ring)
            cvPoint = sectionCV(session, child, ring, 0)
        except (AttributeError, RuntimeError, TypeError, ValueError) as exc:
            ringPoint = cvPoint = None
            check(False, "the child section geometry reads through the ABI: %s"
                  % exc)
        workspace._modeButtons["tube"].click()
        wait(15)
        if "ring" in getattr(workspace, "_tubeSelectionButtons", {}):
            workspace._tubeSelectionButtons["ring"].click()
            wait(10)
        else:
            viewport.setMode("tube")
            viewport.setSubMode("ring")
        session.clearSelection()
        if ringPoint is not None and cvPoint is not None:
            # Ring mode displays section vertices, while the native generic
            # ring candidate lives at the centroid.  The Tube resolver maps
            # this real displayed vertex back to its ring for hover/click.
            ringVertexPixel = pixel(cvPoint)
            mouse.move(ringVertexPixel)
            wait(15)
            check(hover(session) == (tonicLib.TONIC_PICK_SECTION_RING,
                                     child, ring, -1),
                  "a displayed child section vertex prehighlights its owning ring")
            mouse.click(ringVertexPixel)
            ringSelection = session.readSelection(tonicLib.TONIC_PICK_SECTION_RING)
            check(ringSelection == [(child, ring, -1)],
                  "a real child-ring vertex click preserves its tube and ring %r"
                  % (ringSelection,))
        else:
            check(False, "a child ring has projectable center and vertex controls")

        if "section" in getattr(workspace, "_tubeSelectionButtons", {}):
            workspace._tubeSelectionButtons["section"].click()
            wait(10)
        else:
            viewport.setSubMode("section")
        session.clearSelection()
        if cvPoint is not None:
            cvPixel = pixel(cvPoint)
            mouse.click(cvPixel)
            cvSelection = session.readSelection(tonicLib.TONIC_PICK_SECTION_CV)
            check(cvSelection == [(child, ring, 0)],
                  "a real child section-CV click preserves tube/ring/slot %r"
                  % (cvSelection,))
            session.clearSelection()
            start = (cvPixel[0] - 3.0, cvPixel[1] - 3.0)
            finish = (cvPixel[0] + 3.0, cvPixel[1] + 3.0)
            mouse.drag([start, finish], ("shift",))
            marquee = session.readSelection(tonicLib.TONIC_PICK_SECTION_CV)
            check((child, ring, 0) in marquee,
                  "a real child-CV marquee retains its tube/ring/slot %r"
                  % (marquee,))
        else:
            check(False, "a child section CV has a projectable pick center")

    capturePath = os.environ.get("USDGEN_TONIC_SELECTION_CAPTURE")
    if capturePath:
        window = getattr(appController, "_mainWindow", None)
        shot = window.grab() if window is not None else view.grab()
        check(shot.save(capturePath, "PNG"),
              "the optional selection acceptance screenshot was written")

    if rolloverBefore is not None:
        workspace.hide()
        wait(10)
        check(bool(view.rolloverPicking) == rolloverBefore,
              "hiding the workspace restores StageView rollover picking")
        workspace.show()

    shutdown()
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicSelection needs testusdview")
    sys.exit(0)
