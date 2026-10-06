# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Record one live Noodles Editor operator view through testusdview.

Supply USDGEN_UI_OPERATOR_ID=UsdGenScatter and an operator example scene to
bin/launch_usdview.ps1 -TestScript. This uses the same usdview Window >
Noodles Editor command and Add from Prim Tree selection behavior as an artist.
The screenshot is a Qt grab of the real localized Noodles Editor window.
"""

import os
import sys
import time


def testUsdviewInputFunction(controller):
    from pxr.Usdviewq.qt import QtWidgets
    from UsdNoodles import GetEditorManager

    op_type = os.environ.get("USDGEN_UI_OPERATOR_ID")
    if not op_type or not op_type.startswith("UsdGen"):
        raise ValueError("Set USDGEN_UI_OPERATOR_ID to a UsdGen schema type")
    stage = controller._dataModel.stage
    prims = [p for p in stage.Traverse() if p.GetTypeName() == op_type]
    if not prims:
        raise RuntimeError("No %s prim in %s" %
                           (op_type, stage.GetRootLayer().identifier))
    prim = prims[0]
    capture_camera = ("/World/CamMotion" if op_type == "UsdGenCollide" else
                      "/World/Cam" if op_type == "UsdGenWind" else None)
    if capture_camera and stage.GetPrimAtPath(capture_camera):
        camera_menu = controller._ui.menuCameraSelect
        motion_action = next((action for action in camera_menu.actions()
                              if str(action.data()) == capture_camera), None)
        if motion_action is None:
            raise RuntimeError("Camera > Select Camera has no " + capture_camera)
        motion_action.trigger()
        if str(controller._dataModel.viewSettings.cameraPrim.GetPath()) != \
                capture_camera:
            raise RuntimeError("Camera selection did not take effect: " + capture_camera)
        print("UI camera: %s > %s > %s (%s)" %
              (controller._ui.menuCamera.title(), camera_menu.title(),
               motion_action.text(), motion_action.data()), flush=True)
    controller._dataModel.selection.setPrimPath(str(prim.GetPath()))
    item = controller._getItemAtPath(prim.GetPath(), ensureExpanded=True)
    if item is not None:
        controller._ui.primView.scrollToItem(item)

    registry = controller._plugRegistry
    command = registry.getCommandPlugin(
        "NoodlesPluginContainer.ShowNoodlesEditor")
    if command is None:
        raise RuntimeError("Window > Noodles Editor is unavailable")
    command.run()
    manager = GetEditorManager()
    window = manager.getActiveWindow()
    if window is None:
        raise RuntimeError("Noodles Editor did not open")
    app = QtWidgets.QApplication.instance()
    start = time.monotonic()
    while not window.graphView.initialized or \
            window.graphView.nodeGraph.getStage() is None:
        app.processEvents()
        time.sleep(0.05)
        if time.monotonic() - start > 20:
            raise TimeoutError("Noodles Editor did not load the stage")
    graph = window.graphView
    graph.addNodesFromPrimTreeSelection()
    if str(prim.GetPath()) not in graph.nodes:
        raise RuntimeError("Add from Prim Tree did not show " +
                           str(prim.GetPath()))
    # Wind's native low/high billow controls need a taller real widget grab.
    window.resize(1040, 1200 if op_type == "UsdGenWind" else 820)
    app.processEvents()
    graph.frameScene()
    node = graph.nodes[str(prim.GetPath())]
    # Frame the actual value rows at a readable scale. frameScene reserves
    # three times the selected node's bounds for interactive navigation;
    # a reference screenshot can use the closer full-node fit.
    zoom = min(1.4,
               graph.width() / max(float(node.size[0]) * 1.3, 1.0),
               graph.height() / max(float(node.size[1]) * 1.15, 1.0))
    graph.zoom = zoom
    graph.panX = (float(node.position[0]) -
                  (graph.width() / zoom - float(node.size[0])) * 0.5)
    graph.panY = (float(node.position[1]) -
                  (graph.height() / zoom - float(node.size[1])) * 0.5)
    # Let the real Add-from-Prim-Tree notification fade before the grab.
    for _ in range(60):
        graph.update()
        app.processEvents()
        time.sleep(0.05)
    output = os.environ.get("USDGEN_UI_CAPTURE_DIR", "docs/site/media/ui")
    os.makedirs(output, exist_ok=True)
    name = os.environ.get("USDGEN_UI_CAPTURE_NAME") or \
        "operator-" + op_type[6:].lower() + "-noodles.png"
    if os.path.basename(name) != name or not name.endswith(".png"):
        raise ValueError("USDGEN_UI_CAPTURE_NAME must be a PNG filename")
    image = window.grab()
    path = os.path.join(output, name)
    if image.isNull() or not image.save(path, "PNG"):
        raise RuntimeError("Could not save Noodles Editor grab: " + path)
    print("UI capture %s (%dx%d) selectedPrim=%s" %
          (path, image.width(), image.height(), prim.GetPath()), flush=True)
    manager.closeAllWindows()
    app.processEvents()
    return 0


if __name__ == "__main__":
    print("SKIP: capture_operator_noodles_ui.py needs testusdview", file=sys.stderr)
