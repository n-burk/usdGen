# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Optional real usdview/Pomade screenshots for the artist walkthrough.

Set USDGEN_UI_CAPTURE_DIR to an output directory and run the existing
testUsdviewPomadeArtistWalkthrough.py through bin/launch_usdview.ps1. The
walkthrough drives Qt widgets and the live StageView; this module only records
its settled states. It is deliberately inert during ordinary test runs.
"""

import os


_layout_ready = False


def capture(controller, name):
    global _layout_ready
    output = os.environ.get("USDGEN_UI_CAPTURE_DIR")
    if not output:
        return
    from pxr.Usdviewq.qt import QtWidgets
    import usdGenPomadeTools

    os.makedirs(output, exist_ok=True)
    app = QtWidgets.QApplication.instance()
    container = usdGenPomadeTools.container()
    view = controller._stageView
    if not _layout_ready:
        # Let the actual Pomade dock and viewport fill the manual frame.
        # The ordinary test layout leaves the property browser open, which
        # clips the dock at narrower window sizes.
        ui = controller._ui
        ui.primStageSplitter.setSizes([0, 1])
        ui.topBottomSplitter.setSizes([1, 0])
        container.workspace.setMinimumWidth(490)
        ratio = float(view.devicePixelRatioF())
        view.SetPhysicalWindowSize(int(970 * ratio), int(830 * ratio))
        controller._mainWindow.resize(1560, 1060)
        _layout_ready = True
    session = getattr(container, "session", None)
    viewport = getattr(container, "viewport", None)
    if session is not None and viewport is not None:
        for _ in range(120):
            viewport.pumpOnce()
            if not session.hasPendingWork():
                break
            app.processEvents()
        container.workspace.refresh()
    previous_camera = view._dataModel.viewSettings.cameraPrim
    if name.startswith("pomade-") and 5 <= int(name.split("-")[1]) <= 11:
        from pxr import Usd
        from pomadeT3 import obliqueCamera
        stage = controller._dataModel.stage
        with Usd.EditContext(stage, stage.GetSessionLayer()):
            if not obliqueCamera(stage, view, eye=(6.0, 8.0, 12.0),
                                 target=(0.0, 1.8, 0.0),
                                 up=(0.0, 1.0, 0.0),
                                 primPath="/PomadeManualShotCamera"):
                raise RuntimeError("Could not activate Pomade screenshot camera")
    app.processEvents()
    view.update()
    view.repaint()
    app.processEvents()
    image = controller._mainWindow.grab()
    path = os.path.join(output, name + ".png")
    if image.isNull() or not image.save(path, "PNG"):
        raise RuntimeError("Could not capture real usdview window: " + path)
    view._dataModel.viewSettings.cameraPrim = previous_camera
    app.processEvents()
    print("UI capture %s (%dx%d)" % (path, image.width(), image.height()),
          flush=True)
