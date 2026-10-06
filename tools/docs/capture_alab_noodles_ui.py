# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Capture a real Noodles edit of the prepared ALab stoat's Noise operator.

Run with bin/launch_usdview.ps1 -TestScript and
examples/alab/stoat-groom.usda. The edit lands only in an ignored persistent
layer under renders/docs/; the checked-in example and downloaded ALab asset
are not modified. Window > Noodles Editor, Prim Tree Add, Layer Editor edit
target, value click/type/Enter and Ctrl+S are exercised in the live Qt app.
"""

import os
import sys
import time


NOISE = "/World/Groom/BrownBody/Ops/noise"
FIELD = "usdGen:noise:magnitude"
VALUE = 0.031


def testUsdviewInputFunction(controller):
    from pxr import Sdf
    from pxr.Usdviewq.qt import QtCore, QtWidgets, PySideModule
    from UsdNoodles import GetEditorManager, GetLayerEditorManager
    import importlib

    QtTest = importlib.import_module("%s.QtTest" % PySideModule).QTest
    output = os.environ.get("USDGEN_UI_CAPTURE_DIR", "docs/site/media/ui")
    os.makedirs(output, exist_ok=True)
    app = QtWidgets.QApplication.instance()
    stage = controller._dataModel.stage
    prim = stage.GetPrimAtPath(NOISE)
    if not prim or prim.GetTypeName() != "UsdGenNoise":
        raise RuntimeError("Prepared ALab BrownBody Noise operator is missing")
    old = float(prim.GetAttribute(FIELD).Get())

    # The temporary layer remains under ignored renders/ for inspection.
    edit_path = os.path.abspath("renders/docs/stoat-ui-edit.usda")
    os.makedirs(os.path.dirname(edit_path), exist_ok=True)
    if os.path.exists(edit_path):
        os.remove(edit_path)
    layer = Sdf.Layer.CreateNew(edit_path)
    layer.Save()
    session = stage.GetSessionLayer()
    session.subLayerPaths.insert(0, layer.identifier)

    controller._dataModel.selection.setPrimPath(NOISE)
    item = controller._getItemAtPath(NOISE, ensureExpanded=True)
    if item is not None:
        controller._ui.primView.scrollToItem(item)
    command = controller._plugRegistry.getCommandPlugin(
        "NoodlesPluginContainer.ShowNoodlesEditor")
    if command is None:
        raise RuntimeError("Window > Noodles Editor is unavailable")
    command.run()
    manager = GetEditorManager()
    window = manager.getActiveWindow()
    if window is None:
        raise RuntimeError("Noodles Editor did not open")
    start = time.monotonic()
    while not window.graphView.initialized or \
            window.graphView.nodeGraph.getStage() is None:
        app.processEvents()
        time.sleep(0.05)
        if time.monotonic() - start > 20:
            raise TimeoutError("Noodles Editor did not initialize")
    graph = window.graphView
    graph.addNodesFromPrimTreeSelection()
    if NOISE not in graph.nodes:
        raise RuntimeError("A did not add selected Noise operator")
    window.resize(1040, 820)
    app.processEvents()
    node = graph.nodes[NOISE]
    zoom = min(1.4, graph.width() / (float(node.size[0]) * 1.3),
               graph.height() / (float(node.size[1]) * 1.15))
    graph.zoom = zoom
    graph.panX = (float(node.position[0]) -
                  (graph.width() / zoom - float(node.size[0])) * 0.5)
    graph.panY = (float(node.position[1]) -
                  (graph.height() / zoom - float(node.size[1])) * 0.5)

    def settle(ms=250):
        end = time.monotonic() + ms / 1000.0
        while time.monotonic() < end:
            graph.update()
            app.processEvents()
            time.sleep(0.02)

    def shot(widget, name):
        settle()
        image = widget.grab()
        path = os.path.join(output, name + ".png")
        if image.isNull() or not image.save(path, "PNG"):
            raise RuntimeError("Could not save real Noodles UI: " + path)
        print("UI capture %s (%dx%d)" %
              (path, image.width(), image.height()), flush=True)

    shot(window, "alab-05-select-noise-in-noodles")

    layer_window = GetLayerEditorManager().getWindow()
    if layer_window is None:
        raise RuntimeError("Layer Editor did not accompany Noodles Editor")
    layer_widget = layer_window.editorWidget
    layer_widget.refresh()
    layer_list = layer_widget._listWidget
    target_item = next((layer_list.item(i) for i in range(layer_list.count())
                        if layer_list.item(i).data(QtCore.Qt.UserRole) == layer),
                       None)
    if target_item is None:
        raise RuntimeError("Temporary edit layer is not listed")
    QtTest.mouseClick(layer_list.viewport(), QtCore.Qt.LeftButton,
                      pos=layer_list.visualItemRect(target_item).center())
    if stage.GetEditTarget().GetLayer() != layer:
        raise RuntimeError("Layer Editor click did not set edit target")
    shot(layer_window, "alab-08-layer-editor")

    rect = graph._valueCellRectFor(node, FIELD, 0)
    if rect is None:
        raise RuntimeError("Noise magnitude value cell is not visible")
    world_x = (rect.left + rect.right) * 0.5
    world_y = (rect.top + rect.bottom) * 0.5
    screen_x = int(round((world_x - graph.panX) * graph.zoom))
    screen_y = int(round((world_y - graph.panY) * graph.zoom))
    graph.setFocus()
    QtTest.mouseClick(graph, QtCore.Qt.LeftButton,
                      pos=QtCore.QPoint(screen_x, screen_y))
    if graph._valueEditTarget is None:
        raise RuntimeError("Click did not open Noise magnitude value editor")
    QtTest.keyClicks(graph, str(VALUE))
    QtTest.keyClick(graph, QtCore.Qt.Key_Return)
    settle()
    actual = float(prim.GetAttribute(FIELD).Get())
    if abs(actual - VALUE) > 1e-5 or abs(old - VALUE) < 1e-5:
        raise RuntimeError("Noodles value edit failed: %r -> %r" %
                           (old, actual))
    if not layer.dirty:
        raise RuntimeError("Noise edit did not author the selected layer")
    shot(window, "alab-06-edit-noise-magnitude")

    graph.setFocus()
    QtTest.keyClick(graph, QtCore.Qt.Key_S,
                    QtCore.Qt.ControlModifier)
    settle(1800)
    if layer.dirty or not os.path.isfile(edit_path):
        raise RuntimeError("Ctrl+S did not save the selected edit layer")
    shot(window, "alab-07-save-noise-layer")
    print("Noodles edit verified: %s %.6f -> %.6f, saved layer basename %s" %
          (FIELD, old, actual, os.path.basename(edit_path)), flush=True)
    manager.closeAllWindows()
    layer_window.close()
    app.processEvents()
    return 0


if __name__ == "__main__":
    print("SKIP: capture_alab_noodles_ui.py needs testusdview", file=sys.stderr)
