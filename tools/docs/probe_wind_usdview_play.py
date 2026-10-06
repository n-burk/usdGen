# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Probe real usdview Play and its Storm viewport on animated native scenes.

Run with testusdview and a coherent usdGen plugin build. Use --allow-async to
match bin/launch_usdview.ps1's ordinary interactive Storm mode. Images are
diagnostic captures under renders/docs, not published manual media.
"""

import hashlib
import os
import time


def testUsdviewInputFunction(controller):
    from pxr.Usdviewq.qt import QtWidgets

    stage = controller._dataModel.stage
    scene = os.path.splitext(os.path.basename(stage.GetRootLayer().realPath))[0]
    if scene not in ("wind", "wind-clumped", "wind-unclumped",
                     "collide", "collide-slide"):
        raise RuntimeError("Expected a native Wind or Collide practice scene")
    mode = os.environ.get("USDGEN_UI_PLAY_MODE", "sync")
    output = os.environ.get("USDGEN_UI_PLAY_OUTPUT", "renders/docs/wind-play-probe")
    os.makedirs(output, exist_ok=True)
    app = QtWidgets.QApplication.instance()
    ui = controller._ui
    view = controller._stageView
    camera = stage.GetPrimAtPath(
        "/World/CamMotion" if scene.startswith("collide") else "/World/Cam")
    if camera:
        controller._dataModel.viewSettings.cameraPrim = camera
    controller._dataModel.selection.clear()
    controller._dataModel.viewSettings.showBBoxes = False
    controller._mainWindow.resize(1250, 950)
    start_frame = int(os.environ.get("USDGEN_UI_PLAY_START_FRAME", "0"))
    controller.setFrame(start_frame)

    def sample(label):
        view.update()
        for _ in range(8):
            app.processEvents()
            time.sleep(0.02)
        view.repaint()
        app.processEvents()
        current = controller._dataModel.currentFrame.GetValue()
        image = view.grab()
        path = os.path.join(output, "%s-%s-%s.png" % (scene, mode, label))
        if image.isNull() or not image.save(path, "PNG"):
            raise RuntimeError("Could not save viewport " + path)
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        print("PLAY sample %s currentFrame=%s frameField=%s slider=%s viewportSha256=%s size=%dx%d" %
              (path, current, ui.frameField.text(), ui.frameSlider.value(), digest,
               image.width(), image.height()), flush=True)

    sample("start")
    if mode == "scrub":
        for frame in (21, 33, 57):
            controller.setFrame(frame)
            sample("scrub-%02d" % frame)
        print("SCRUB result scene=%s reached=%s" %
              (scene, controller._dataModel.currentFrame.GetValue()), flush=True)
        return 0
    ui.playButton.click()
    if not controller._dataModel.playing:
        raise RuntimeError("The live Play button did not start playback")
    marks = tuple(int(part) for part in
                  os.environ.get("USDGEN_UI_PLAY_MARKS", "12,24,48").split(","))
    captured = set()
    start = time.monotonic()
    deadline = float(os.environ.get("USDGEN_UI_PLAY_TIMEOUT_SECONDS", "18"))
    while time.monotonic() - start < deadline:
        app.processEvents()
        frame = int(controller._dataModel.currentFrame.GetValue())
        for mark in marks:
            if frame >= mark and mark not in captured:
                sample("play-%02d" % mark)
                captured.add(mark)
        if len(captured) == len(marks):
            break
        time.sleep(0.02)
    if controller._dataModel.playing:
        ui.playButton.click()
    print("PLAY result scene=%s mode=%s reached=%s captured=%s elapsed=%.2f" %
          (scene, mode, controller._dataModel.currentFrame.GetValue(),
           sorted(captured), time.monotonic() - start), flush=True)
    if not captured:
        raise RuntimeError("Live usdview Play did not advance the scene time")
    return 0
