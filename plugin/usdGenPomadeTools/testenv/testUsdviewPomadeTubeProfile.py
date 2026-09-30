# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""T3: draw a region on a sphere and inspect a four-ring tube profile.

Run through bin/launch_usdview.ps1 -TestScript with
examples/pomade-sphere-scalp.usda. Captures and the resumable USD scene go
under USDGEN_TUBE_PROFILE_OUT (default build/tube-profile-validation).
"""
import ctypes
import math
import os
import sys


def testUsdviewInputFunction(appController):
    # testusdview execs this file without a dependable __file__.
    script = next((sys.argv[i + 1] for i, arg in enumerate(sys.argv[:-1])
                   if arg == "--testScript"), "")
    if script:
        sys.path.insert(0, os.path.dirname(os.path.abspath(script)))
    from pomadeT3 import Mouse, typeKey, wait, frameScalp, sideCamera
    from testUsdviewPomadeCvRegions import _bindGeometryFromDock, stagedTubePoints
    import usdGenPomadeTools
    from usdGenPomadeTools import pomadeBridge, pomadeCamera, pomadeTube
    from pxr import UsdGeom
    from pxr.Usdviewq.qt import QtCore, QtGui

    stage = appController._dataModel.stage
    view = appController._stageView
    window = appController._mainWindow
    out = os.path.abspath(os.environ.get(
        "USDGEN_TUBE_PROFILE_OUT", "build/tube-profile-validation"))
    os.makedirs(out, exist_ok=True)
    appController._ui.primStageSplitter.setSizes([0, 1])
    appController._ui.topBottomSplitter.setSizes([1, 0])
    pixelRatio = float(view.devicePixelRatioF())
    view.SetPhysicalWindowSize(int(1000 * pixelRatio), int(900 * pixelRatio))
    window.resize(1510, 1110)
    wait(50)
    assert frameScalp(stage, view, eye=(0.0, 5.5, 0.0))
    appController._plugRegistry.getCommandPlugin(
        "usdGenPomadeTools.openWorkspace").run()
    container = usdGenPomadeTools.container()
    workspace = container.workspace
    workspace.setMinimumWidth(450)
    assert _bindGeometryFromDock(workspace)
    session = container.session
    viewport = container.viewport
    state = container.pomadeState
    view.setFocus()
    wait(80)
    camera = pomadeCamera.resolve(view)
    assert camera is not None
    mouse = Mouse(view)
    mouse.direct = True
    # Eight actual viewport clicks around the sphere's upper cap, followed
    # by Enter, exercise graph closure and automatic tube creation.
    radius = 0.85
    height = math.sqrt(4.0 - radius * radius)
    for slot in range(8):
        angle = 2.0 * math.pi * slot / 8.0
        pixel = camera.worldToPixels((radius * math.cos(angle), height,
                                       radius * math.sin(angle)))
        assert pixel is not None
        mouse.click(pixel[:2])
    typeKey(view, "return")
    wait(80)
    ids = pomadeBridge.readTubeIds(session.dll, session.model)
    assert len(ids) == 1, "viewport region closure must create one tube"
    tube = ids[0]
    region = session.dll.Pomade_RegionForTube(session.model, tube)
    assert region >= 0
    # Use the same native region constructor as the UI, with four sparse
    # sections, then edit their scales through the production model API.
    assert session.dll.Pomade_BuildTubeFromRegion(
        session.model, region, 4, 0, ctypes.c_float(3.4)) == 0
    for ring, scale in enumerate((1.0, 0.30, 1.0, 0.30)):
        assert session.dll.Pomade_ScaleSectionRing(
            session.model, ring, ctypes.c_float(scale)) == 0
    assert session.dll.Pomade_GetDisplaySegments(session.model) == 8
    assert pomadeBridge.tubeSectionCount(session.dll, session.model, tube) == 4
    points = stagedTubePoints(session)
    assert points is not None and len(points) == 25 * 8
    for point in points[:8]:
        assert 1.99 < math.sqrt(sum(v * v for v in point)) < 2.001, \
            "the root boundary must sit on the sphere, not its tangent plane"

    def ringRadius(row):
        ring = points[row * 8:(row + 1) * 8]
        center = tuple(sum(p[a] for p in ring) / 8.0 for a in range(3))
        return sum(math.dist(p, center) for p in ring) / 8.0

    radii = [ringRadius(row) for row in range(25)]
    for row, ratio in ((0, 1.0), (8, 0.30), (16, 1.0), (24, 0.30)):
        assert abs(radii[row] / radii[0] - ratio) < 0.015
    assert radii[9] - radii[8] < radii[12] - radii[11]
    assert radii[16] - radii[15] < radii[12] - radii[11]
    print("ok: sphere tube has four exact wide/thin/wide/thin controls and eased spans")
    state.showGeneratedCurves = False
    session.select(pomadeTube.PICK_TUBE_VERT, [tube])
    viewport.setMode("tube")
    viewport.setSubMode("section")
    state.transformTool = "select"
    appController._dataModel.selection.clear()
    appController._dataModel.viewSettings.showBBoxes = False
    assert sideCamera(stage, view, eye=(5.0, 2.8, 9.0),
                      target=(0.0, 1.8, 0.0), up=(0.0, 1.0, 0.0))
    # An orthographic inspection view makes the width ratios directly
    # comparable and keeps all four controls plus the sphere in frame.
    profileCamera = UsdGeom.Camera(stage.GetPrimAtPath("/PomadeT3SideCamera"))
    profileCamera.CreateProjectionAttr(UsdGeom.Tokens.orthographic)
    profileCamera.CreateHorizontalApertureAttr(100.0)
    profileCamera.CreateVerticalApertureAttr(90.0)
    session.publishAll()
    workspace.refresh()
    for _ in range(10):
        viewport.pumpOnce()
        wait(30)
    resolved = pomadeCamera.resolve(view)
    print("profile radius/control points:", radii[0],
          pomadeBridge.tubeCenters(session.dll, session.model, tube))
    print("profile projected endpoints:", resolved.worldToPixels(points[0]),
          resolved.worldToPixels(points[-1]))
    for point in points:
        pixel = resolved.worldToPixels(point)
        assert pixel is not None and 0 < pixel[0] < 1000 * pixelRatio
        assert 0 < pixel[1] < 900 * pixelRatio, "the entire profile must be visible"

    def capture(name):
        view.update()
        view.repaint()
        wait(60)
        frame = view.grabFrameBuffer()
        assert frame.save(os.path.join(out, name + "-viewport.png"), "PNG")
        shot = window.grab()
        painter = QtGui.QPainter(shot)
        try:
            origin = view.mapTo(window, QtCore.QPoint(0, 0))
            painter.drawImage(QtCore.QRect(origin, view.size()), frame)
        finally:
            painter.end()
        assert shot.save(os.path.join(out, name + ".png"), "PNG")

    capture("profile-controls")
    # Opaque display makes the normal discontinuities and profile readable.
    viewport.setSubMode("tube")
    session.clearSelection()
    session.publishAll()
    workspace.refresh()
    wait(100)
    capture("profile-shaded")
    session.enqueueCommit()
    assert session.flushLatest(timeout=20.0, tick=lambda: wait(20))
    assert stage.Flatten().Export(os.path.join(out, "sphere-profile.usdc"))
    print("ok: captured live Storm controls and shaded profile; saved sphere-profile.usdc")
    viewport.uninstall()
    session.deactivate()
    return 0
