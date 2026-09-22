# testUsdviewTonicWorkflow -- T3: the full interactive workflow on the
# plane fixture, ending in 4 tubes welded together at the base.
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicWorkflow.py \
#               examples/tonic-graph-scalp.usda
#
# Every step below is a REAL event through the installed V2 viewport
# controller -- menu commands, mouse press/drag/release, hotkeys -- on the
# simple 4x4 plane scalp. The per-mode mechanics have their own T3s; what
# this one proves is that the whole artist pass hangs together, following
# the canonical Tonic process (Eurographics 2014: a 2D graph on the scalp,
# one closed region per clump, one clump volume per region):
#
#   * Bind scalp activates a live model on the plane;
#   * a real Graph stroke closes the outer region, Place clicks split its
#     edges and drop the centre node, and four spoke strokes weld end to
#     end through the shared nodes: 9 nodes, 12 edges, 4 closed regions,
#     each the interior of one middle face;
#   * G14 grows one L1 tube stub per region on the graph release -- 4
#     sibling tubes rooted side by side, sharing their base boundaries,
#     published at L1 with no L2 anywhere;
#   * the region map regions the tube interiors: each quadrant tints in
#     its own clump colour and each tube's region owns its quadrant face;
#   * Tube Center clicks a sibling's tip CV, raises the translate gizmo,
#     drags it (model moves, white dot follows in the published
#     framebuffer), and one Ctrl+Z puts it back;
#   * a Sculpt Grab stroke moves a second sibling's centers with the root
#     pinned and the arc length preserved;
#   * the Fill panel's density reaches a third sibling and the guides
#     follow;
#   * after the idle pump the commit lands flat on the stage -- four L1
#     roots under Tubes/, each carrying its regionId, plus Guides whose
#     per-curve tube and region ids agree with the tubes -- and the
#     committer reports a version.
import ctypes
import math
import os
import sys

failures = 0

# The scalp is the 4x4 quad grid in XZ at y = 0. The outer stroke is the
# x in [1, 3], z in [1, 3] square, covering the four middle faces; the
# cross splits it into four quadrant regions of one face each.
RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
MIDS = ((2.0, 1.0), (3.0, 2.0), (2.0, 3.0), (1.0, 2.0))
CENTER = (2.0, 2.0)
SPOKES = (((2.0, 1.0), (2.0, 2.0)), ((2.0, 2.0), (2.0, 3.0)),
          ((1.0, 2.0), (2.0, 2.0)), ((2.0, 2.0), (3.0, 2.0)))
QUADS = ((1.5, 1.5), (2.5, 1.5), (1.5, 2.5), (2.5, 2.5))


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
    for i, arg in enumerate(argv):
        if arg == "--testScript" and i + 1 < len(argv):
            return os.path.dirname(os.path.abspath(argv[i + 1]))
        if arg.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(arg.split("=", 1)[1]))
    return ""


def typeKey(view, name, modifiers=()):
    """One key press through QtTest, seen by the app-level filter."""
    from pxr.Usdviewq.qt import QtCore
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    keys = {"escape": QtCore.Qt.Key.Key_Escape,
            "z": QtCore.Qt.Key.Key_Z,
            "y": QtCore.Qt.Key.Key_Y,
            "c": QtCore.Qt.Key.Key_C,
            "d": QtCore.Qt.Key.Key_D,
            "p": QtCore.Qt.Key.Key_P,
            "w": QtCore.Qt.Key.Key_W,
            "g": QtCore.Qt.Key.Key_G,
            "1": QtCore.Qt.Key.Key_1,
            "2": QtCore.Qt.Key.Key_2,
            "3": QtCore.Qt.Key.Key_3,
            "4": QtCore.Qt.Key.Key_4,
            "5": QtCore.Qt.Key.Key_5,
            "down": QtCore.Qt.Key.Key_Down,
            "bracketleft": QtCore.Qt.Key.Key_BracketLeft,
            "bracketright": QtCore.Qt.Key.Key_BracketRight}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    QtTest.QTest.keyClick(view, keys[name], mods)


def wait(ms=30):
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    QtTest.QTest.qWait(int(ms))


# ---------------------------------------------------------------------------
# The model, read back
# ---------------------------------------------------------------------------

def levelInfo(session, level):
    """(faces, points, tubes) the scene index published for `level`."""
    faces = ctypes.c_int(0)
    points = ctypes.c_int(0)
    tubes = ctypes.c_int(0)
    rc = session.dll.Tonic_GetPublishedLevelInfo(
        session.model, int(level), ctypes.byref(faces), ctypes.byref(points),
        ctypes.byref(tubes))
    if rc != 0:
        return None
    return (faces.value, points.value, tubes.value)


def tubeCenters(session, tubeId, limit=64):
    """Every center CV of one tube, stopping at the first miss."""
    points = []
    for cv in range(limit):
        out = (ctypes.c_float * 3)()
        if session.dll.Tonic_GetTubeCenterCV(session.model, int(tubeId),
                                             int(cv), out) != 0:
            break
        points.append((float(out[0]), float(out[1]), float(out[2])))
    return points


def guideCount(session):
    guides = ctypes.c_int(0)
    session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides),
                                     None)
    return int(guides.value)


def l1TubeIds(session):
    """The L1 root ids, ascending (two-call probe)."""
    got = ctypes.c_int(0)
    session.dll.Tonic_ReadL1TubeIds(session.model, None, 0,
                                    ctypes.byref(got))
    out = (ctypes.c_int * max(int(got.value), 1))()
    if session.dll.Tonic_ReadL1TubeIds(session.model, out, len(out),
                                       ctypes.byref(got)) != 0:
        return []
    return [int(out[i]) for i in range(int(got.value))]


def tubeForRegion(session, regionId):
    """The tube rooted in `regionId`, or -1 when it has no stub yet."""
    return int(session.dll.Tonic_TubeForRegion(session.model,
                                               int(regionId)))


def tubeLevel(session, tubeId):
    return int(session.dll.Tonic_GetTubeLevel(session.model, int(tubeId)))


def quadTint(view, camera, x, z, halfPx=8):
    """Mean RGB of a small patch over a scalp quadrant centre."""
    view.update()
    view.repaint()
    view.updateGL()
    img = view.grabFrameBuffer()
    projected = camera.worldToPixels((x, 0.0, z))
    cx, cy = int(round(projected[0])), int(round(projected[1]))
    rs, gs, bs, total = 0, 0, 0, 0
    for y in range(max(cy - halfPx, 0), min(cy + halfPx + 1, img.height())):
        for xx in range(max(cx - halfPx, 0),
                        min(cx + halfPx + 1, img.width())):
            total += 1
            px = img.pixel(xx, y) & 0x00FFFFFF
            rs += (px >> 16) & 255
            gs += (px >> 8) & 255
            bs += px & 255
    if not total:
        return (0.0, 0.0, 0.0)
    return (float(rs) / total, float(gs) / total, float(bs) / total)


def gizmoKind(session, viewport=None):
    """Read the Qt controller gizmo before its intentionally blank native ABI."""
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


def arcLength(points):
    total = 0.0
    for i in range(1, len(points)):
        a, b = points[i - 1], points[i]
        total += math.sqrt(sum((b[k] - a[k]) ** 2 for k in range(3)))
    return total


def whiteFraction(view, camera, point, halfPx=6):
    """Fraction of near-white pixels in a small window around `point`.

    A selected CV dot publishes as white; everything else here is
    coloured or dark, so white is the unambiguous "published" reading.
    """
    view.update()
    view.repaint()
    view.updateGL()
    img = view.grabFrameBuffer()
    projected = camera.worldToPixels(point)
    cx, cy = int(round(projected[0])), int(round(projected[1]))
    white, total = 0, 0
    for y in range(max(cy - halfPx, 0), min(cy + halfPx + 1, img.height())):
        for x in range(max(cx - halfPx, 0),
                       min(cx + halfPx + 1, img.width())):
            total += 1
            px = img.pixel(x, y) & 0x00FFFFFF
            r, g, b = (px >> 16) & 255, (px >> 8) & 255, px & 255
            if r > 235 and g > 235 and b > 235:
                white += 1
    return float(white) / float(total) if total else 0.0


def pumpUntilCommitted(viewport, session, tries=120):
    for _ in range(tries):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            return True
        wait(25)
    return False


def frameTopCamera(stage, view):
    """A camera looking straight down at the 4x4 scalp, and activate it."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicWorkflowCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(2.0, 14.0, 2.0)
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
        "/TonicWorkflowCamera")
    return view.getActiveSceneCamera() is not None


def aimSideCamera(stage, view):
    """Round to the side, so the sibling tubes span the frame apart."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicWorkflowSide"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(15.0, 2.1, 2.0)
    target = Gf.Vec3d(2.0, 2.1, 2.0)
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
        "/TonicWorkflowSide")
    return view.getActiveSceneCamera() is not None


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

def run(appController):
    global failures
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import (tonicBridge, tonicCamera, tonicLib,
                                      tonicPanels)
        from testUsdviewTonicGraph import Mouse
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    if view is None or registry is None:
        print("FAIL: no stage view / plugin registry")
        return 1
    container = usdGenTonicTools.container()
    if container is None:
        print("FAIL: the Tonic plugin container did not register")
        return 1

    check(frameTopCamera(stage, view),
          "the scene camera looks down at the scalp")
    view.setFocus()
    wait(50)

    # -- open the tool the way the artist does -----------------------------
    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    messages = []
    if container.session is not None:
        container.session.setStatusSink(messages.append)
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    check(session is not None and session.model is not None,
          "Bind scalp created a live model (status %r)" % (messages[-2:],))
    check(viewport is not None and viewport.installed,
          "the viewport controller installed itself")
    if session is None or viewport is None or session.model is None:
        return 1
    if container.session is not None:
        container.session.setStatusSink(messages.append)
    # The model copied the scalp at bind time; the stage's own draw of
    # the same surface would only z-fight the pixel probes below.
    stage.GetPrimAtPath("/Scalp").SetActive(False)

    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the controller's camera did not resolve")
        return 1

    def pixel(x, z, y=0.0):
        projected = camera.worldToPixels((x, y, z))
        return (projected[0], projected[1])

    perPixel = camera.worldPerPixel((2.0, 0.0, 2.0))
    state.snapRadiusPx = max(0.1 / max(perPixel, 1e-9), 2.0)

    # -- Graph: the outer loop, the cross, four welded regions -----------
    mouse = Mouse(view)
    # The workflow below is deliberately a freehand regression, so choose
    # Draw rather than inheriting Graph's click-created Region default.
    viewport.setPointerInside(True)
    typeKey(view, "d")
    check(state.graphSubMode == "draw",
          "D explicitly selects Draw for the legacy graph stroke")

    def stroke(points, closed=False):
        """A press-drag-release through pixel `points` (real events)."""
        mouse.press(points[0])
        if not viewport.gestureActive:
            info("QtTest press did not land; using direct delivery")
            mouse.direct = True
            mouse.press(points[0])
        for point in points[1:]:
            mouse.move(point)
        mouse.release(points[-1])

    def rectPath(corners, steps=5):
        path = []
        for k in range(len(corners)):
            x0, z0 = corners[k]
            x1, z1 = corners[(k + 1) % len(corners)]
            for i in range(steps):
                t = float(i) / float(steps)
                path.append(pixel(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t))
        path.append(pixel(*corners[0]))
        return path

    stroke(rectPath(RECT))
    check(session.graphCounts() == (4, 4, 1),
          "the drag stroked the outer closed region (%r)"
          % (session.graphCounts(),))
    cvCount = int(session.dll.Tonic_GetCenterCVCount(session.model))
    check(cvCount == 5,
          "G14: the closed region got a tube stub on the graph release "
          "(%d center CVs)" % cvCount)
    check(int(session.dll.Tonic_GetTubeRegionId(session.model)) == 0,
          "and the stub is rooted in that region")

    # Place the four edge midpoints (each click splits its edge and welds
    # the node on) plus the free centre node. Splits keep the cycle, so
    # the region count holds at one while the corners stay shared.
    typeKey(view, "p")
    check(state.graphSubMode == "place",
          "the P hotkey selects the Place sub-mode (%r)"
          % state.graphSubMode)
    session.dll.Tonic_SetLevelDisplay(session.model, 1, 0, 0)
    session.publishAll()
    for mid in list(MIDS) + [CENTER]:
        mouse.click(pixel(*mid))
        wait(10)
    check(session.graphCounts() == (9, 8, 1),
          "five Place clicks split four edges and dropped the centre "
          "node (%r)" % (session.graphCounts(),))

    # Four spoke strokes, each end landing exactly on a placed node so it
    # welds: the first spoke dangles, the second closes the vertical bar
    # and splits the region in two, and the horizontal pair splits each
    # half again. No stroke adds a node -- every end welded.
    typeKey(view, "d")
    check(state.graphSubMode == "draw",
          "the D hotkey selects the Draw sub-mode (%r)"
          % state.graphSubMode)
    for (ax, az), (bx, bz) in SPOKES:
        lane = [pixel(ax + (bx - ax) * t, az + (bz - az) * t)
                for t in (0.0, 0.25, 0.5, 0.75, 1.0)]
        stroke(lane)
        wait(10)
    check(session.graphCounts() == (9, 12, 4),
          "four spokes weld into 9 nodes, 12 edges, 4 closed regions "
          "(%r)" % (session.graphCounts(),))

    # Belt and braces through the real controller: Weld all merges any
    # coincident stragglers the stroke ends missed.
    check(viewport.loop is not None and viewport.loop.modeId == "graph",
          "Graph's own loop is driving (%r)"
          % (viewport.loop.modeId if viewport.loop is not None else None,))
    check(bool(viewport.loop.weldAll()),
          "Weld all runs through the controller")
    check(session.graphCounts() == (9, 12, 4),
          "and the welded cross is stable (%r)"
          % (session.graphCounts(),))

    # -- Four siblings, welded at the base ---------------------------------
    sibs = l1TubeIds(session)
    check(len(sibs) == 4,
          "G14 grew one L1 stub per region (%r)" % (sibs,))
    check(all(tubeLevel(session, t) == 1 for t in sibs),
          "every sibling sits at level 1")
    rooted = [tubeForRegion(session, r) for r in range(4)]
    check(sorted(rooted) == sorted(sibs),
          "each region roots exactly one sibling (%r)" % (rooted,))
    infoL1 = levelInfo(session, 1)
    check(infoL1 is not None and infoL1[2] == 4,
          "L1 publishes the four siblings (%r)" % (infoL1,))
    check(levelInfo(session, 2) is None,
          "and there is no L2 anywhere")
    for t in sibs:
        check(len(tubeCenters(session, t)) == 5,
              "sibling %d is a 5-CV stub" % t)

    # The region map regions the tube interiors: with the tubes hidden,
    # each quadrant tints in its own clump colour, all four distinct.
    session.dll.Tonic_SetLevelDisplay(session.model, 1, 0, 0)
    session.publishAll()
    tints = [quadTint(view, camera, x, z) for (x, z) in QUADS]
    session.dll.Tonic_SetLevelDisplay(session.model, 1, 1, 0)
    session.publishAll()
    chroma = [max(t) - min(t) for t in tints]
    check(all(c > 40.0 for c in chroma),
          "every quadrant carries region tint (%r)"
          % ([round(c, 1) for c in chroma],))
    spread = min(math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))
                 for i, a in enumerate(tints)
                 for b in tints[i + 1:])
    check(spread > 60.0,
          "and the four quadrant tints differ (min spread %.1f)" % spread)

    tubeA, tubeB, tubeC = sibs[1], sibs[2], sibs[3]
    state.snapRadiusPx = 10.0

    # -- Tube: a sibling tip CV takes the gizmo for a drag -------------------
    # Top-down, like the Tube T3: the tip dot is seen through the tube's
    # open top, unblended, so the white checks below read the staged
    # selection colour and not the x-ray wall in front of it.
    typeKey(view, "2")
    check(state.activeMode == "tube"
          and viewport.loop is not None
          and viewport.loop.modeId == "tube",
          "the 2 hotkey selects Tube mode and its loop (%r)"
          % state.activeMode)
    check(state.tubeSubMode == "center",
          "which opens in the Center sub-mode (%r)" % state.tubeSubMode)
    cvsA = tubeCenters(session, tubeA)
    handlesA = tonicBridge.tubeCenterHandles(session.dll, session.model, tubeA)
    check(len(cvsA) > 1, "sibling %d has center CVs (%d)"
          % (tubeA, len(cvsA)))
    check(len(handlesA) == len(cvsA),
          "the sibling has one displayed core handle per authored center CV")
    tipA = handlesA[-1] if len(handlesA) == len(cvsA) else None
    if tipA is None:
        check(False, "the sibling tip core handle is available")
        return 1
    tipPixel = pixel(tipA[0], tipA[2], tipA[1])
    check(0 <= tipPixel[0] < camera.width
          and 0 <= tipPixel[1] < camera.height,
          "the sibling tip projects inside the frame (%r in %dx%d)"
          % ((int(tipPixel[0]), int(tipPixel[1])), camera.width,
             camera.height))
    check(whiteFraction(view, camera, tipA) < 0.05,
          "the sibling tip CV draws in the clump colour, not white")
    mouse.click(tipPixel)
    picked = session.readSelection(2)            # TonicPick_CenterCV
    check(any(s[0] == tubeA and s[1] == len(cvsA) - 1 for s in picked),
          "one click selected sibling %d's tip CV (%r)" % (tubeA, picked))
    kind, origin = gizmoKind(session, viewport)
    nativeKind, _ = gizmoKind(session)
    overlay = getattr(viewport, "_gizmoOverlay", None)
    check(kind == 1 and nativeKind == 0 and overlay is not None and
          overlay.isVisible(),
          "the Qt translate overlay is live while the native fallback is suppressed")
    check(whiteFraction(view, camera, tipA) > 0.05,
          "and the published CV dot turned white")
    travelPx = 60.0
    perTip = camera.worldPerPixel(tipA)
    # Moves go direct: QtTest's global-coordinate round trip can deliver
    # them to whatever sits under the REAL cursor rather than to the
    # view (the Sculpt T3 forces this for the same reason).
    mouse.direct = True
    mouse.press(tipPixel)
    check(viewport.gestureActive, "the press took the gizmo's free handle")
    for step in range(1, 7):
        mouse.move((tipPixel[0] + travelPx * step / 6.0, tipPixel[1]))
    mouse.release((tipPixel[0] + travelPx, tipPixel[1]))
    movedA = tubeCenters(session, tubeA)
    movedHandlesA = tonicBridge.tubeCenterHandles(session.dll, session.model,
                                                  tubeA)
    check(bool(movedA) and len(movedA) == len(cvsA),
          "the sibling survived the drag (%d CVs)" % len(movedA))
    # Top-down, a horizontal drag reads as +X (the Tube T3's own math).
    moved = (movedA[-1][0] - cvsA[-1][0]) if movedA else float("nan")
    want = travelPx * perTip
    check(bool(movedA) and abs(moved - want) < 0.10 * want,
          "the drag moved the sibling tip CV %.3f (want %.3f)" % (moved, want))
    check(bool(movedA) and len(movedHandlesA) == len(movedA) and
          whiteFraction(view, camera, movedHandlesA[-1]) > 0.05,
          "the white dot is where the cursor left it")
    check(whiteFraction(view, camera, tipA) < 0.05,
          "and gone from where it was")
    typeKey(view, "z", ("ctrl",))
    wait(20)
    restored = tubeCenters(session, tubeA)
    check(bool(restored) and
          abs(restored[-1][0] - cvsA[-1][0]) < 1e-4,
          "one Ctrl+Z put the sibling tip CV back")
    typeKey(view, "y", ("ctrl",))
    wait(20)
    redone = tubeCenters(session, tubeA)
    check(bool(redone) and bool(movedA) and
          abs(redone[-1][0] - movedA[-1][0]) < 1e-4,
          "and Ctrl+Y re-applied the drag")

    # -- side on: the sculpt stroke reads against a spanning tube ---------
    check(aimSideCamera(stage, view),
          "the camera moves round to the side of the tubes")
    wait(50)
    camera = tonicCamera.resolve(view)
    if camera is None:
        print("FAIL: the side camera did not resolve")
        return 1

    def pixelOf(point):
        projected = camera.worldToPixels(point)
        return (projected[0], projected[1])

    # -- Sculpt: a Grab stroke on the second sibling -------------------------
    typeKey(view, "5")
    loop = viewport.loop
    check(state.activeMode == "sculpt" and loop is not None
          and loop.modeId == "sculpt",
          "the 5 hotkey selects Sculpt mode and its loop (%r)"
          % state.activeMode)
    state.brushRadiusPx = 60.0
    state.sculptPreserveLength = True
    state.sculptMirrorX = False
    state.brushTRadius = 0.0
    baseB = tubeCenters(session, tubeB)
    handlesB = tonicBridge.tubeCenterHandles(session.dll, session.model, tubeB)
    baseLength = arcLength(baseB)
    start = pixelOf(handlesB[-2]) if len(handlesB) == len(baseB) else None
    if start is None:
        check(False, "the sibling sculpt core handle is available")
        return 1
    mouse.direct = True
    mouse.press(start)
    check(viewport.gestureActive, "the press opened a sculpt gesture")
    for step in range(1, 5):
        mouse.move((start[0] + 12.0 * step, start[1]))
    mouse.release((start[0] + 48.0, start[1]))
    check(not viewport.gestureActive, "the release closed it")
    movedB = tubeCenters(session, tubeB)
    check(bool(movedB) and len(movedB) == len(baseB),
          "the second sibling survived the stroke (%d CVs)" % len(movedB))
    same = bool(movedB) and len(movedB) == len(baseB)
    delta = max(abs(movedB[i][k] - baseB[i][k])
                for i in range(len(movedB)) for k in range(3)) if same else 0.0
    check(same and delta > 1e-4,
          "the stroke moved the sibling's centers (max %.6f)" % delta)
    check(same and all(abs(movedB[0][k] - baseB[0][k]) < 1e-4
                       for k in range(3)),
          "with the root pinned")
    relative = (abs(arcLength(movedB) - baseLength) / max(baseLength, 1e-9)
                if same else float("inf"))
    check(same and relative <= 1e-4,
          "and the arc length preserved to 1e-4 relative (%.2e)" % relative)

    # -- Fill: the panel's density reaches the selection -------------------
    typeKey(view, "3")
    check(state.activeMode == "fill",
          "the 3 hotkey selects Fill mode (%r)" % state.activeMode)
    session.select(1, [tubeC], None, None, 0)  # the third sibling, untouched
    descriptors = {d.id: d for d in tonicPanels.descriptors("fill", state)}
    check("density" in descriptors, "the Fill panel generates a density row")
    descriptors["density"].set(state, session, 8.0)
    sparse = guideCount(session)
    descriptors["density"].set(state, session, 32.0)
    dense = guideCount(session)
    check(dense > sparse > 0,
          "the panel's density reached the tube and the guides followed "
          "(%d -> %d)" % (sparse, dense))

    # -- the commit lands flat on the stage --------------------------------
    check(session.enqueueCommit() and pumpUntilCommitted(viewport, session),
          "the idle pump drains the committer")
    tubes = stage.GetPrimAtPath("/TonicGroom/Tubes")
    roots = [c for c in tubes.GetChildren()
             if c.GetTypeName() == "UsdGenTube"] if tubes else []
    check(len(roots) == 4,
          "four sibling roots committed under Tubes/ (%d)" % len(roots))
    byTube = {}
    for prim in roots:
        try:
            byTube[int(prim.GetName()[4:])] = prim
        except ValueError:
            pass
    check(sorted(byTube) == sorted(sibs),
          "the committed roots are the four live siblings (%r)"
          % (sorted(byTube),))
    check(all(p.GetAttribute("usdGen:tonic:level").Get() == 1
              for p in byTube.values()),
          "every committed root is level 1")
    regions = [p.GetAttribute("usdGen:tonic:regionId").Get()
               for p in byTube.values()]
    check(sorted(regions) == [0, 1, 2, 3],
          "each root carries its own region (%r)" % (regions,))
    nested = [c for p in byTube.values() for c in p.GetChildren()
              if c.GetTypeName() == "UsdGenTube"]
    check(len(nested) == 0, "and no tube nests under another")
    guides = stage.GetPrimAtPath("/TonicGroom/Guides")
    points = guides.GetAttribute("points").Get() if guides else None
    check(points is not None and len(points) > 0,
          "and the committed guides carry %d points"
          % (0 if points is None else len(points),))
    tubeIds = list(guides.GetAttribute("primvars:tubeId").Get())
    regionIds = list(guides.GetAttribute("primvars:regionId").Get())
    guideLevels = list(
        guides.GetAttribute("primvars:hierarchyLevel").Get())
    check(sorted(set(tubeIds)) == sorted(sibs),
          "the guides span all four siblings (%r)"
          % (sorted(set(tubeIds)),))
    tubeRegion = {t: byTube[t].GetAttribute("usdGen:tonic:regionId").Get()
                  for t in byTube}
    check(all(regionIds[i] == tubeRegion[t]
              for i, t in enumerate(tubeIds)),
          "every guide's region is its own tube's region")
    check(all(lv == 1 for lv in guideLevels),
          "every guide sits at level 1")
    # Each tube's guides root inside its own quadrant: the root CV of
    # every curve lies within a face-diagonal of its tube's live base.
    bases = dict((t, tubeCenters(session, t)[0]) for t in sibs)
    counts = list(guides.GetAttribute("curveVertexCounts").Get())
    ok, cursor = True, 0
    for i, t in enumerate(tubeIds):
        root = points[cursor]
        base = bases[t]
        if math.sqrt(sum((root[k] - base[k]) ** 2
                         for k in range(3))) > 1.5:
            ok = False
            break
        cursor += counts[i]
    check(ok, "every guide roots inside its own tube's quadrant")
    # The live region map agrees: exactly four faces -- the middle
    # square -- carry the four region ids, everything else one value.
    scalp = stage.GetPrimAtPath("/Scalp")
    faceRegions = list(scalp.GetAttribute(
        "primvars:usdGen:tonicRegion").Get())
    histogram = {}
    for r in faceRegions:
        histogram[r] = histogram.get(r, 0) + 1
    singles = sorted(r for r, n in histogram.items() if n == 1)
    rest = [(r, n) for r, n in histogram.items() if n != 1]
    check(len(faceRegions) == 16 and len(singles) == 4 and
          len(rest) == 1 and rest[0][1] == 12,
          "the region map covers the four middle faces, each its own "
          "region (%r)" % (histogram,))
    check(singles == [0, 1, 2, 3],
          "numbered 0 to 3 like the tubes' regions (%r)" % (singles,))
    status = session.status()
    check(status["committedVersion"] > 0,
          "the committer reports a committed version (v%d)"
          % status["committedVersion"])

    # -- tear down cleanly -------------------------------------------------
    viewport.uninstall()
    session.deactivate()
    print("testUsdviewTonicWorkflow: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicWorkflow needs testusdview (no live view)")
    sys.exit(0)
