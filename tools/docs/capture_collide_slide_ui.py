# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Capture real usdview Current Frame and translate keys for the Collide slide.

Run through testusdview with a coherent plugin build and the accepted full-keyed
``examples/docs/operators/collide.usda``. The script checks authored keys and
both collider targets, selects the sphere and its translate property, and
grabs the live Qt usdview window at frames 0, 27 and 99. It requires
synchronous testusdview scene processing and rejects ``--allow-async``. It does
not edit the practice scene.
"""

import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import sys
import time


def testUsdviewInputFunction(controller):
    from pxr import Usd, UsdGeom
    from pxr.Usdviewq.qt import QtCore, QtWidgets, PySideModule
    from UsdNoodles import GetEditorManager, GetLayerEditorManager

    QtTest = importlib.import_module("%s.QtTest" % PySideModule).QTest

    if "--allow-async" in sys.argv:
        raise RuntimeError(
            "Collide UI capture requires synchronous testusdview; remove --allow-async")

    stage = controller._dataModel.stage
    root = stage.GetRootLayer().realPath
    expected_source = os.environ.get("USDGEN_UI_EXPECT_SCENE")
    if expected_source:
        if os.path.normcase(os.path.realpath(root)) != os.path.normcase(
                os.path.realpath(expected_source)):
            raise RuntimeError("Loaded scene does not match USDGEN_UI_EXPECT_SCENE")
    elif os.path.basename(root) not in ("collide.usda", "collide-slide.usda"):
        raise RuntimeError("Expected the canonical full-keyed Collide practice scene")
    expected_hash = os.environ.get("USDGEN_UI_EXPECT_SCENE_SHA256")
    actual_hash = hashlib.sha256(Path(root).read_bytes()).hexdigest()
    if expected_hash and actual_hash.lower() != expected_hash.lower():
        raise RuntimeError("Collide slide scene changed before UI capture")

    def assert_immutable_source():
        if stage.GetRootLayer().dirty or hashlib.sha256(
                Path(root).read_bytes()).hexdigest() != actual_hash:
            raise RuntimeError("UI capture changed the immutable source scene")

    sphere = stage.GetPrimAtPath("/World/Shield")
    floor = stage.GetPrimAtPath("/World/ScalpVolume")
    collide = stage.GetPrimAtPath("/World/Groom/Fur/Ops/collide")
    camera = stage.GetPrimAtPath("/World/CamMotion")
    if not sphere or not floor or not collide or not camera:
        raise RuntimeError("Collide sphere, floor, operator or motion camera is missing")
    targets = [str(path) for path in
               collide.GetRelationship("usdGen:colliders").GetTargets()]
    if targets != ["/World/Shield", "/World/ScalpVolume"]:
        raise RuntimeError("Collide targets must name Shield and ScalpVolume")
    if floor.GetAttribute("visibility").Get() != "invisible":
        raise RuntimeError("The closed under-patch collision floor must be invisible")
    subdivision = sphere.GetAttribute("subdivisionScheme")
    if not subdivision or subdivision.Get() != "catmullClark":
        raise RuntimeError("Shield must carry Catmull-Clark subdivision")
    if (collide.GetAttribute("usdGen:iterations").Get() != 128 or
            collide.GetAttribute("usdGen:pushAmount").Get() != 1 or
            collide.GetAttribute("usdGen:offset").Get() != 0 or
            collide.GetAttribute("usdGen:resolveType").Get() != "flexible"):
        raise RuntimeError("Unexpected native flexible Collide preset")
    cut_mode = collide.GetAttribute("usdGen:deepPenetrationMode")
    cut_threshold = collide.GetAttribute("usdGen:cutDepthThreshold")
    cut_blend = collide.GetAttribute("usdGen:cutBlendDepth")
    if (
            not cut_mode or cut_mode.Get() != "cutThenCollide" or
            not cut_threshold or abs(float(cut_threshold.Get()) - 0.15) > 1e-7 or
            not cut_blend or abs(float(cut_blend.Get()) - 0.02) > 1e-7):
        raise RuntimeError("Collide must author cutThenCollide at threshold 0.15 and blend 0.02")
    translate = sphere.GetAttribute("xformOp:translate")
    expected = {0: (0.5, 1.45, 0.5), 27: (0.5, 0.525, 0.5),
                99: (0.5, 0.525, 0.85)}
    if not translate or [int(t) for t in translate.GetTimeSamples()] != [0, 27, 99]:
        raise RuntimeError("Collide slide translate must have exact 0/27/99 keys")
    for frame, value in expected.items():
        actual = translate.Get(Usd.TimeCode(frame))
        if actual is None or any(abs(float(actual[i]) - value[i]) > 1e-6
                                 for i in range(3)):
            raise RuntimeError("Unexpected translate key at frame %d" % frame)

    ui = controller._ui
    ui.actionVery_High.trigger()
    if not ui.actionVery_High.isChecked():
        raise RuntimeError("Display > Complexity > Very High did not take effect")
    app = QtWidgets.QApplication.instance()
    view = controller._stageView
    main = controller._mainWindow
    camera_action = next((action for action in ui.menuCameraSelect.actions()
                          if str(action.data()) == "/World/CamMotion"), None)
    if camera_action is None:
        raise RuntimeError("Camera > Select Camera lacks CamMotion")
    camera_action.trigger()
    if controller._dataModel.viewSettings.cameraPrim != camera:
        raise RuntimeError("CamMotion camera selection did not take effect")
    controller._dataModel.viewSettings.showBBoxes = False
    # Per-render timings and GPU counters change even for a held frame.
    # Keep the stable Complexity/Camera and renderer labels as capture evidence.
    controller._dataModel.viewSettings.showHUD = True
    controller._dataModel.viewSettings.showHUD_Performance = False
    controller._dataModel.viewSettings.showHUD_GPUstats = False
    controller._dataModel.viewSettings.showHUD_Complexity = True
    ui.primStageSplitter.setSizes([350, 580])
    main.resize(930, 840)

    def viewport_digest():
        image = view.grab()
        payload = QtCore.QByteArray()
        stream = QtCore.QBuffer(payload)
        stream.open(QtCore.QIODevice.WriteOnly)
        if image.isNull() or not image.save(stream, "PNG"):
            raise RuntimeError("Could not sample the live Storm viewport")
        stream.close()
        return hashlib.sha256(bytes(payload)).hexdigest()

    def wait_for_synchronous_frame(frame):
        """Set a frame synchronously, then wait only for Storm presentation."""
        start = time.monotonic()
        controller.setFrame(frame)
        first_visible_converged = None
        stable_digest = None
        stable_samples = 0
        max_stable_samples = 0
        converged_samples = 0
        frame_visible = False
        converged = False
        # Qt event processing can block on the synchronous native cook.
        # These deadlines are checked between calls; they cannot preempt Qt.
        while True:
            now = time.monotonic()
            if first_visible_converged is None:
                if now - start > 600:
                    break
            elif now - first_visible_converged > 90 or now - start > 690:
                break
            view.update()
            app.processEvents()
            renderer = getattr(view, "_renderer", None)
            frame_visible = int(float(ui.frameField.text())) == frame
            converged = renderer is not None and renderer.IsConverged()
            if converged:
                converged_samples += 1
            now = time.monotonic()
            if frame_visible and converged and first_visible_converged is None:
                if now - start > 600:
                    break
                first_visible_converged = now
            if now - start > 690 or (first_visible_converged is not None and
                    now - first_visible_converged > 90):
                break
            digest = viewport_digest() if frame_visible and converged else None
            if digest is not None and digest == stable_digest:
                stable_samples += 1
            elif digest is not None:
                stable_digest = digest
                stable_samples = 1
            else:
                stable_digest = None
                stable_samples = 0
            # Synchronous testusdview finishes scene processing during
            # setFrame/event processing before converged presentation.
            # Stable pixels are not publication evidence in an async session.
            max_stable_samples = max(max_stable_samples, stable_samples)
            if stable_samples >= 4:
                return stable_digest
            time.sleep(0.25)
        output = os.environ.get("USDGEN_UI_CAPTURE_DIR", "renders/docs/collide-ui-review")
        os.makedirs(output, exist_ok=True)
        failure_path = os.path.join(output, "collide-wait-failed-%03d.png" % frame)
        failure_image = main.grab()
        failure_saved = not failure_image.isNull() and failure_image.save(failure_path, "PNG")
        raise TimeoutError(
            "Held frame %d did not reach stable Storm presentation: "
            "frameVisible=%s converged=%s convergedSamples=%d "
            "stableSamples=%d maxStableSamples=%d lastDigest=%s "
            "elapsedSeconds=%.2f firstVisibleConvergedSeconds=%s "
            "initialDeadlineSeconds=600 presentationDeadlineSeconds=90 "
            "outerDeadlineSeconds=690 failureImage=%s sceneSha256=%s" %
            (frame, frame_visible, converged, converged_samples, stable_samples,
             max_stable_samples, stable_digest, time.monotonic() - start,
             ("%.2f" % (first_visible_converged - start)
              if first_visible_converged is not None else "none"),
             failure_path if failure_saved else "save failed", actual_hash))

    if os.environ.get("USDGEN_UI_CUT_CONTROLS_ONLY") == "1":
        wait_for_synchronous_frame(0)
        controller._dataModel.selection.setPrimPath(str(collide.GetPath()))
        command = controller._plugRegistry.getCommandPlugin(
            "NoodlesPluginContainer.ShowNoodlesEditor")
        if command is None:
            raise RuntimeError("Noodles Editor is unavailable")
        command.run()
        window = GetEditorManager().getActiveWindow()
        start = time.monotonic()
        while window is None or not window.graphView.initialized or \
                window.graphView.nodeGraph.getStage() is None:
            app.processEvents()
            time.sleep(0.05)
            window = GetEditorManager().getActiveWindow()
            if time.monotonic() - start > 20:
                raise TimeoutError("Noodles Editor did not load")
        graph = window.graphView
        graph.addNodesFromPrimTreeSelection()
        node = graph.nodes.get(str(collide.GetPath()))
        if node is None:
            raise RuntimeError("Noodles did not add the Collide node")
        mode_row = node.value_row_for_property(
            "usdGen:deepPenetrationMode", Usd.TimeCode.Default())
        threshold_row = node.value_row_for_property(
            "usdGen:cutDepthThreshold", Usd.TimeCode.Default())
        blend_row = node.value_row_for_property(
            "usdGen:cutBlendDepth", Usd.TimeCode.Default())
        if mode_row is None or tuple(mode_row.texts) != ("cutThenCollide",):
            raise RuntimeError("Noodles lacks the authored cutThenCollide control")
        if threshold_row is None or not threshold_row.texts or \
                abs(float(threshold_row.texts[0]) - 0.15) > 1e-7:
            raise RuntimeError("Noodles lacks the authored 0.15 cut threshold")
        if blend_row is None or not blend_row.texts or \
                abs(float(blend_row.texts[0]) - 0.02) > 1e-7:
            raise RuntimeError("Noodles lacks the authored 0.02 cut blend depth")
        window.resize(1040, 900)
        app.processEvents()
        zoom = min(1.2, graph.width() / (float(node.size[0]) * 1.25),
                   graph.height() / (float(node.size[1]) * 1.15))
        graph.zoom = zoom
        graph.panX = float(node.position[0]) - \
            (graph.width() / zoom - float(node.size[0])) * 0.5
        graph.panY = float(node.position[1]) - \
            (graph.height() / zoom - float(node.size[1])) * 0.5
        graph.update()
        for _ in range(12):
            app.processEvents()
            time.sleep(0.03)
        output = os.environ.get("USDGEN_UI_CAPTURE_DIR", "docs/site/media/ui")
        os.makedirs(output, exist_ok=True)
        # Clear the transient node-added toast before recording controls.
        QtTest.qWait(2200)
        app.processEvents()
        image = window.grab()
        path = os.path.join(output, "collide-cut-controls-noodles.png")
        if image.isNull() or not image.save(path, "PNG"):
            raise RuntimeError("Could not save Collide cut-controls capture " + path)
        print("UI capture %s (%dx%d) deepPenetrationMode=cutThenCollide "
              "cutDepthThreshold=0.15 cutBlendDepth=0.02 resolveType=flexible "
              "captureMode=synchronous Current Frame=0 sceneSha256=%s" %
              (path, image.width(), image.height(), actual_hash), flush=True)
        assert_immutable_source()
        return
    controller._dataModel.selection.setPrimPath("/World/Shield")
    item = controller._getItemAtPath(sphere.GetPath(), ensureExpanded=True)
    if item is not None:
        ui.primView.scrollToItem(item)
    controller._dataModel.selection.setProp(translate)

    def show_property_row(property_name):
        tree = ui.propertyView
        for index in range(tree.topLevelItemCount()):
            row = tree.topLevelItem(index)
            if str(row.text(1)) == property_name:
                tree.scrollToItem(row, QtWidgets.QAbstractItemView.ScrollHint.PositionAtCenter)
                return
        raise RuntimeError("usdview property tree does not show " + property_name)

    output = os.environ.get("USDGEN_UI_CAPTURE_DIR", "docs/site/media/ui")
    os.makedirs(output, exist_ok=True)
    for frame in (0, 27, 99):
        cooked_digest = wait_for_synchronous_frame(frame)
        controller._dataModel.selection.setProp(translate)
        app.processEvents()
        show_property_row("xformOp:translate")
        if int(float(ui.frameField.text())) != frame:
            raise RuntimeError("Current Frame field did not show %d" % frame)
        view.repaint()
        app.processEvents()
        image = main.grab()
        path = os.path.join(output, "collide-key-%03d.png" % frame)
        if image.isNull() or not image.save(path, "PNG"):
            raise RuntimeError("Could not save live usdview capture " + path)
        print("UI capture %s (%dx%d) Current Frame=%d translate=%s "
              "captureMode=synchronous settledViewportSha256=%s sceneSha256=%s" %
              (path, image.width(), image.height(), frame,
              tuple(float(v) for v in translate.Get(Usd.TimeCode(frame))),
              cooked_digest, actual_hash),
              flush=True)

    wait_for_synchronous_frame(27)
    controller._dataModel.selection.setPrimPath("/World/Shield")
    controller._dataModel.selection.setProp(subdivision)
    app.processEvents()
    tree = ui.propertyView
    row = next((tree.topLevelItem(i) for i in range(tree.topLevelItemCount())
                if str(tree.topLevelItem(i).text(1)) == "subdivisionScheme"), None)
    if row is None:
        raise RuntimeError("usdview property tree omits subdivisionScheme")
    tree.scrollToItem(row, QtWidgets.QAbstractItemView.ScrollHint.PositionAtCenter)
    view.update()
    app.processEvents()
    image = main.grab()
    path = os.path.join(output, "collide-shield-subdivision.png")
    if image.isNull() or not image.save(path, "PNG"):
        raise RuntimeError("Could not save Shield subdivision capture " + path)
    print("UI capture %s (%dx%d) subdivisionScheme=catmullClark "
          "Complexity=Very High sceneSha256=%s" %
          (path, image.width(), image.height(), actual_hash), flush=True)

    # Show the two authored physical targets in the same real usdview session.
    wait_for_synchronous_frame(27)
    controller._dataModel.selection.setPrimPath(str(collide.GetPath()))
    item = controller._getItemAtPath(collide.GetPath(), ensureExpanded=True)
    if item is not None:
        ui.primView.scrollToItem(item)
    relation = collide.GetRelationship("usdGen:colliders")
    controller._dataModel.selection.setProp(relation)
    app.processEvents()
    tree = ui.propertyView
    row = next((tree.topLevelItem(i) for i in range(tree.topLevelItemCount())
                if str(tree.topLevelItem(i).text(1)) == "usdGen:colliders"), None)
    if row is None:
        raise RuntimeError("usdview property tree omits usdGen:colliders")
    tree.scrollToItem(row, QtWidgets.QAbstractItemView.ScrollHint.PositionAtCenter)
    view.update()
    for _ in range(12):
        app.processEvents()
        time.sleep(0.03)
    image = main.grab()
    path = os.path.join(output, "collide-targets.png")
    if image.isNull() or not image.save(path, "PNG"):
        raise RuntimeError("Could not save Collide target view " + path)
    print("UI capture %s (%dx%d) colliders=%s sceneSha256=%s" %
          (path, image.width(), image.height(), targets, actual_hash), flush=True)

    # Show the real editable Noodles cell, with its exact key value visible.
    # Escape cancels the editor; the checked-in practice layer stays untouched.
    wait_for_synchronous_frame(27)
    controller._dataModel.selection.setPrimPath("/World/Shield")
    command = controller._plugRegistry.getCommandPlugin(
        "NoodlesPluginContainer.ShowNoodlesEditor")
    if command is None:
        raise RuntimeError("Noodles Editor is unavailable")
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
            raise TimeoutError("Noodles Editor did not load")
    graph = window.graphView
    graph.addNodesFromPrimTreeSelection()
    node = graph.nodes.get("/World/Shield")
    if node is None:
        raise RuntimeError("Noodles did not add /World/Shield")
    scheme_row = node.value_row_for_property("subdivisionScheme", Usd.TimeCode.Default())
    if scheme_row is None or not scheme_row.editable or \
            tuple(scheme_row.texts) != ("catmullClark",):
        raise RuntimeError("Noodles lacks the editable Mesh subdivisionScheme token")
    window.resize(1040, 900)
    app.processEvents()
    zoom = min(1.4, graph.width() / (float(node.size[0]) * 1.3),
               graph.height() / (float(node.size[1]) * 1.15))
    graph.zoom = zoom
    graph.panX = float(node.position[0]) - \
        (graph.width() / zoom - float(node.size[0])) * 0.5
    graph.panY = float(node.position[1]) - \
        (graph.height() / zoom - float(node.size[1])) * 0.5
    graph._setValueWriteMode("animation")
    row = node.value_row_for_property("xformOp:translate", Usd.TimeCode(27))
    if row is None or not row.editable or row.count != 3:
        raise RuntimeError("Noodles lacks the editable double3 translate row")
    layer_window = GetLayerEditorManager().getWindow()
    if layer_window is None:
        raise RuntimeError("Layer Editor is unavailable")
    layer_window.editorWidget.refresh()
    layer_list = layer_window.editorWidget._listWidget
    root_layer = stage.GetRootLayer()
    item = next((layer_list.item(i) for i in range(layer_list.count())
                 if layer_list.item(i).data(QtCore.Qt.UserRole) == root_layer), None)
    if item is None:
        raise RuntimeError("Root layer is absent from Layer Editor")
    QtTest.mouseClick(layer_list.viewport(), QtCore.Qt.LeftButton,
                      pos=layer_list.visualItemRect(item).center())
    if stage.GetEditTarget().GetLayer() != root_layer:
        raise RuntimeError("Root layer did not become the edit target")
    node.invalidate_value_row("xformOp:translate")
    graph.update()
    app.processEvents()
    rect = graph._valueCellRectFor(node, "xformOp:translate", 1)
    if rect is None:
        raise RuntimeError("Noodles translate Y cell is not visible")
    point = QtCore.QPoint(int(round(((rect.left + rect.right) * 0.5 - graph.panX) * graph.zoom)),
                          int(round(((rect.top + rect.bottom) * 0.5 - graph.panY) * graph.zoom)))
    graph.setFocus()
    QtTest.mouseClick(graph, QtCore.Qt.LeftButton, pos=point)
    if graph._valueEditTarget is None:
        raise RuntimeError("Noodles did not open the translate Y editor")
    QtTest.keyClicks(graph, "0.525")
    app.processEvents()
    full = window.grab()
    # The focused capture is a single unmodified crop of the live Qt window.
    crop = QtCore.QRect(round(full.width() * 375 / 1300),
                        round(full.height() * 775 / 1125),
                        round(full.width() * 555 / 1300),
                        round(full.height() * 325 / 1125))
    image = full.copy(crop)
    path = os.path.join(output, "collide-key-authoring-noodles.png")
    if image.isNull() or not image.save(path, "PNG"):
        raise RuntimeError("Could not save Noodles key editor capture " + path)
    QtTest.keyClick(graph, QtCore.Qt.Key_Escape)
    if root_layer.dirty or \
            abs(float(translate.Get(Usd.TimeCode(27))[1]) - 0.525) > 1e-6:
        raise RuntimeError("UI capture changed the practice scene")
    print("UI capture %s (%dx%d) Noodles Animation frame=27 y=0.525 "
          "sceneSha256=%s" % (path, image.width(), image.height(), actual_hash),
          flush=True)

    # Capture the real Collide node controls from the accepted scene.
    if cut_mode.Get() == "cutThenCollide":
        wait_for_synchronous_frame(0)
        controller._dataModel.selection.setPrimPath(str(collide.GetPath()))
        graph.addNodesFromPrimTreeSelection()
        cut_node = graph.nodes.get(str(collide.GetPath()))
        if cut_node is None:
            raise RuntimeError("Noodles did not add the Collide node")
        mode_row = cut_node.value_row_for_property(
            "usdGen:deepPenetrationMode", Usd.TimeCode.Default())
        threshold_row = cut_node.value_row_for_property(
            "usdGen:cutDepthThreshold", Usd.TimeCode.Default())
        blend_row = cut_node.value_row_for_property(
            "usdGen:cutBlendDepth", Usd.TimeCode.Default())
        if mode_row is None or tuple(mode_row.texts) != ("cutThenCollide",):
            raise RuntimeError("Noodles lacks the authored cutThenCollide control")
        if threshold_row is None or not threshold_row.texts or \
                abs(float(threshold_row.texts[0]) - 0.15) > 1e-7:
            raise RuntimeError("Noodles lacks the authored 0.15 cut threshold")
        if blend_row is None or not blend_row.texts or \
                abs(float(blend_row.texts[0]) - 0.02) > 1e-7:
            raise RuntimeError("Noodles lacks the authored 0.02 cut blend depth")
        window.resize(1040, 900)
        app.processEvents()
        zoom = min(1.2, graph.width() / (float(cut_node.size[0]) * 1.25),
                   graph.height() / (float(cut_node.size[1]) * 1.15))
        graph.zoom = zoom
        graph.panX = float(cut_node.position[0]) - \
            (graph.width() / zoom - float(cut_node.size[0])) * 0.5
        graph.panY = float(cut_node.position[1]) - \
            (graph.height() / zoom - float(cut_node.size[1])) * 0.5
        graph.update()
        for _ in range(12):
            app.processEvents()
            time.sleep(0.03)
        # Clear the transient node-added toast before recording controls.
        QtTest.qWait(2200)
        app.processEvents()
        image = window.grab()
        path = os.path.join(output, "collide-cut-controls-noodles.png")
        if image.isNull() or not image.save(path, "PNG"):
            raise RuntimeError("Could not save Collide cut-controls capture " + path)
        print("UI capture %s (%dx%d) deepPenetrationMode=cutThenCollide "
              "cutDepthThreshold=0.15 cutBlendDepth=0.02 resolveType=flexible "
              "captureMode=synchronous Current Frame=0 sceneSha256=%s" %
              (path, image.width(), image.height(), actual_hash), flush=True)
    assert_immutable_source()
    manager.closeAllWindows()
    layer_window.close()

    # Eighth capture: exercise the actual usdview session visibility action.
    # Counts/mean lengths are a bounded UI check, not full curve-array parity.
    from usdGenTools.brushApi import groomCurveStats

    def native_stats():
        result = groomCurveStats("/World/Groom/Fur")
        if result is None or result[0] != 8000 or not math.isfinite(result[1]):
            raise RuntimeError("Companion visibility capture requires 8000 native curves")
        count, total_length = result
        return {"curveCount": count, "totalLength": total_length,
                "meanLength": total_length / count}

    def assert_collision_inputs():
        current_targets = [str(path) for path in
                           collide.GetRelationship("usdGen:colliders").GetTargets()]
        if current_targets != targets or subdivision.Get() != "catmullClark":
            raise RuntimeError("Visibility action changed collider targets or subdivision")
        return {"colliders": current_targets, "shieldSubdivisionScheme": str(subdivision.Get())}

    selection = controller._dataModel.selection
    previous_prims = tuple(selection.getPrimPaths())
    previous_props = tuple(selection.getPropPaths())
    previous_target = stage.GetEditTarget()
    session = stage.GetSessionLayer()
    imageable = UsdGeom.Imageable(sphere)
    visibility_attribute = imageable.GetVisibilityAttr()
    before_digest = wait_for_synchronous_frame(99)
    before_stats = native_stats()
    before_inputs = assert_collision_inputs()
    before_visibility = str(imageable.ComputeVisibility(Usd.TimeCode(99)))
    if before_visibility != str(UsdGeom.Tokens.inherited):
        raise RuntimeError("Shield must be visible before the companion visibility action")
    session_visibility = session.GetAttributeAtPath(visibility_attribute.GetPath())
    if session_visibility is not None and session_visibility.HasInfo("default"):
        raise RuntimeError("Companion action would overwrite an existing session visibility opinion")
    proof = {"frame": 99, "sourceSceneSha256": actual_hash,
             "editTarget": "session", "editTargetSetBy": "stage.SetEditTarget(session)",
             "hideAction": "actionMake_Invisible", "hideUi": "Edit > Make Invisible (Ctrl+H)",
             "restoreAction": "actionRemove_Session_Visibility",
             "restoreUi": "Edit > Remove Session Visibility (Ctrl+U)",
             "before": {"stats": before_stats, "inputs": before_inputs,
                        "resolvedVisibility": before_visibility,
                        "viewportSha256": before_digest},
             "statsTolerance": 1e-10, "statsOnlyComparison": True,
             "fullBasisCurvesParityProven": False}
    hidden_attempted = False
    try:
        stage.SetEditTarget(session)
        if stage.GetEditTarget().GetLayer() != session:
            raise RuntimeError("Session layer did not become the visibility edit target")
        selection.setPrimPath("/World/Shield")
        selection.clearProps()
        controller._updateEditMenu()
        hidden_attempted = True
        ui.actionMake_Invisible.trigger()
        after_digest = wait_for_synchronous_frame(99)
        after_visibility = str(imageable.ComputeVisibility(Usd.TimeCode(99)))
        if (str(visibility_attribute.Get()) != str(UsdGeom.Tokens.invisible) or
                after_visibility != str(UsdGeom.Tokens.invisible)):
            raise RuntimeError("Edit > Make Invisible did not author invisible Shield visibility")
        after_stats = native_stats()
        after_inputs = assert_collision_inputs()
        mean_delta = abs(after_stats["meanLength"] - before_stats["meanLength"])
        if after_stats["curveCount"] != before_stats["curveCount"] or mean_delta > 1e-10:
            raise RuntimeError("Shield visibility changed native curve count or mean length")
        assert_immutable_source()
        selection.setProp(visibility_attribute)
        app.processEvents()
        show_property_row("visibility")
        view.repaint()
        app.processEvents()
        image = main.grab()
        path = Path(output) / "collide-fur-only-ui.png"
        if image.isNull() or not image.save(str(path), "PNG"):
            raise RuntimeError("Could not save the companion usdview visibility capture")
        proof["after"] = {"stats": after_stats, "inputs": after_inputs,
                          "resolvedVisibility": after_visibility,
                          "viewportSha256": after_digest}
        proof["meanLengthDelta"] = mean_delta
        proof["image"] = path.name
        proof["imageSha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        print("UI capture %s (%dx%d) frame=99 sessionVisibility=invisible "
              "nativeCurves=8000 meanLengthDelta=%.12g sceneSha256=%s" %
              (path, image.width(), image.height(), mean_delta, actual_hash), flush=True)
    finally:
        try:
            if hidden_attempted:
                stage.SetEditTarget(session)
                selection.setPrimPath("/World/Shield")
                controller._updateEditMenu()
                ui.actionRemove_Session_Visibility.trigger()
                restored_digest = wait_for_synchronous_frame(99)
                restored_visibility = str(imageable.ComputeVisibility(Usd.TimeCode(99)))
                restored_stats = native_stats()
                if restored_visibility != before_visibility:
                    raise RuntimeError("Remove Session Visibility did not restore visible Shield")
                if (restored_stats["curveCount"] != before_stats["curveCount"] or
                        abs(restored_stats["meanLength"] - before_stats["meanLength"]) > 1e-10):
                    raise RuntimeError("Restored Shield visibility changed native groom statistics")
                assert_collision_inputs()
                proof["restored"] = {"stats": restored_stats,
                                     "resolvedVisibility": restored_visibility,
                                     "viewportSha256": restored_digest}
        finally:
            stage.SetEditTarget(previous_target)
            # Batch clear/add: unbatched clearPrims inserts the root selection.
            with selection.batchPrimChanges:
                selection.clearPrims()
                for prim_path in previous_prims:
                    selection.addPrimPath(prim_path)
            with selection.batchPropChanges:
                selection.clearProps()
                for prop_path in previous_props:
                    selection.addPropPath(prop_path)
    assert_immutable_source()
    proof["sourceUnchanged"] = True
    proof["rootLayerDirty"] = stage.GetRootLayer().dirty
    restored_target = stage.GetEditTarget()
    proof["editTargetBefore"] = {"layer": previous_target.GetLayer().identifier,
                                 "mapFunction": str(previous_target.GetMapFunction())}
    proof["editTargetAfter"] = {"layer": restored_target.GetLayer().identifier,
                                "mapFunction": str(restored_target.GetMapFunction())}
    proof["previousEditTargetRestored"] = (
        restored_target.GetLayer() == previous_target.GetLayer() and
        restored_target.GetMapFunction() == previous_target.GetMapFunction())
    proof["selectionBefore"] = {"prims": [str(p) for p in previous_prims],
                                "properties": [str(p) for p in previous_props]}
    proof["selectionAfter"] = {"prims": [str(p) for p in selection.getPrimPaths()],
                               "properties": [str(p) for p in selection.getPropPaths()]}
    proof["previousSelectionRestored"] = (tuple(selection.getPrimPaths()) == previous_prims and
                                          tuple(selection.getPropPaths()) == previous_props)
    proof["restorationPassed"] = (proof["previousEditTargetRestored"] and
                                  proof["previousSelectionRestored"])
    (Path(output) / "visibility-ui-proof.json").write_text(
        json.dumps(proof, indent=2) + "\n", encoding="utf-8")
    if not proof["restorationPassed"]:
        raise RuntimeError("Companion capture failed to restore edit target or selection; "
                           "see visibility-ui-proof.json")
    return 0
