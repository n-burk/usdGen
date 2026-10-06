# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Capture the prepared ALab stoat in real usdview widgets.

Run from the repository root with the OpenUSD launcher after preparing the
local ALab asset. No downloaded character data is copied into the manual.

  $env:USDGEN_UI_CAPTURE_DIR = 'docs/site/media/ui'
  .\bin\launch_usdview.ps1 -TestScript tools/docs/capture_alab_usdview_ui.py examples/alab/stoat-groom.usda
  .\bin\launch_usdview.ps1 -TestScript tools/docs/capture_alab_usdview_ui.py examples/alab/stoat-guides.usda

The scene supplied to testusdview determines the captures. All images are
window grabs from the running Qt usdview and its live Storm StageView.
"""

import os
import sys
import time


def testUsdviewInputFunction(controller):
    from pxr.Usdviewq.qt import QtWidgets

    output = os.environ.get("USDGEN_UI_CAPTURE_DIR", "docs/site/media/ui")
    os.makedirs(output, exist_ok=True)
    stage = controller._dataModel.stage
    view = controller._stageView
    main = controller._mainWindow
    ui = controller._ui
    app = QtWidgets.QApplication.instance()
    scene = os.path.basename(stage.GetRootLayer().realPath)
    is_guides = scene == "stoat-guides.usda"
    if scene not in ("stoat-guides.usda", "stoat-groom.usda"):
        raise ValueError("Expected prepared stoat-groom.usda or stoat-guides.usda")
    camera = stage.GetPrimAtPath("/World/HeroCam")
    if not camera:
        raise RuntimeError("ALab example is missing /World/HeroCam")

    ui.primStageSplitter.setSizes([450, 1100])
    ui.topBottomSplitter.setSizes([1, 0])
    ratio = float(view.devicePixelRatioF())
    view.SetPhysicalWindowSize(int(1040 * ratio), int(850 * ratio))
    main.resize(1650, 1080)
    if hasattr(view, "DrawAxis"):
        view.DrawAxis = lambda *args, **kwargs: None
    controller._dataModel.viewSettings.cameraPrim = camera
    controller._dataModel.viewSettings.showBBoxes = False

    def settle():
        start = time.monotonic()
        for _ in range(400):
            view.update()
            app.processEvents()
            renderer = getattr(view, "_renderer", None)
            converged = renderer is not None and renderer.IsConverged()
            if converged and time.monotonic() - start >= 1.0:
                break
            if time.monotonic() - start > 45:
                break
            time.sleep(0.05)
        view.repaint()
        app.processEvents()

    def shot(name):
        settle()
        image = main.grab()
        path = os.path.join(output, name + ".png")
        if image.isNull() or not image.save(path, "PNG"):
            raise RuntimeError("Could not save usdview capture " + path)
        print("UI capture %s (%dx%d)" % (path, image.width(), image.height()),
              flush=True)

    if is_guides:
        if not stage.GetPrimAtPath("/World/Guides"):
            raise RuntimeError("ALab guide prims are missing")
        controller._dataModel.selection.setPrimPath("/World/Guides")
        shot("alab-03-sparse-guides")
    else:
        controller._dataModel.selection.clear()
        shot("alab-01-open-stoat-hero")
        body = "/World/Character/body_M_hrc/GEO/body_M_geo"
        if not stage.GetPrimAtPath(body):
            raise RuntimeError("ALab body mesh is missing: " + body)
        controller._dataModel.selection.setPrimPath(body)
        item = controller._getItemAtPath(body, ensureExpanded=True)
        if item is not None:
            ui.primView.scrollToItem(item)
        shot("alab-02-select-body-mesh")
        controller._dataModel.selection.clear()
        detail = stage.GetPrimAtPath("/World/DetailCam")
        if not detail:
            raise RuntimeError("ALab example is missing /World/DetailCam")
        controller._dataModel.viewSettings.cameraPrim = detail
        shot("alab-04-final-detail")
    return 0


if __name__ == "__main__":
    print("SKIP: capture_alab_usdview_ui.py needs testusdview", file=sys.stderr)
