# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Real asynchronous Storm playback/movie acceptance through testusdview.

Set USDGEN_PLAYBLAST_TEST_OUTPUT to an ignored diagnostic MP4 destination.
USDGEN_PLAYBLAST_START_FRAME optionally selects the first of six consecutive
timeline samples (default 0; use 25 for the Collide deep-contact range).
Run with --allow-async --renderer GL and an animated groom fixture. Native
readiness is never replaced. The file picker and one capture-error injection
are controlled so the callback can complete unattended.
"""
import hashlib
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from unittest.mock import patch


def _wait(predicate, view, timeout=900):
    from pxr.Usdviewq.qt import QtCore
    loop = QtCore.QEventLoop()
    timer = QtCore.QTimer()
    errors = []
    start = time.monotonic()
    pulses = [0]
    def tick():
        pulses[0] += 1
        try:
            if predicate():
                loop.quit()
            elif time.monotonic() - start > timeout:
                raise RuntimeError("Timed out waiting for the real viewport.")
            else:
                view.update()
        except Exception as exc:
            errors.append(exc)
            loop.quit()
    timer.setInterval(25)
    timer.timeout.connect(tick)
    timer.start()
    tick()
    if not errors and timer.isActive():
        # A successful first tick needs no nested event loop.
        try:
            complete = predicate()
        except Exception as exc:
            errors.append(exc)
            complete = True
        if not complete:
            loop.exec()
    timer.stop()
    if errors:
        raise errors[0]
    return pulses[0]


def testUsdviewInputFunction(controller):
    from pxr.Usdviewq.qt import QtWidgets
    from pxr.Usdviewq.common import SelectionHighlightModes
    output = os.environ.get("USDGEN_PLAYBLAST_TEST_OUTPUT")
    if not output:
        raise RuntimeError("Set USDGEN_PLAYBLAST_TEST_OUTPUT to an ignored diagnostic path.")
    output = str(Path(output).resolve())
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    view = controller._stageView
    _wait(lambda: getattr(controller, "_usdgenPlayback", None) is not None, view, 30)
    owner = controller._usdgenPlayback
    assert view._allowAsync, "Acceptance requires actual asynchronous Storm."
    ui = controller._ui
    report = {"playAdvances": [], "captureFrames": [], "warnings": []}
    startFrame = int(os.environ.get("USDGEN_PLAYBLAST_START_FRAME", "0"))
    frameSequence = tuple(range(startFrame, startFrame + 6))
    report["timelineRange"] = [frameSequence[0], frameSequence[-1]]
    originalRange = (controller.realStartTimeCode, controller.realEndTimeCode, controller.step)
    originalFrame = owner.frame()
    originalAdvance = controller._advanceFrame
    originalCapture = owner.api.GrabViewportShot
    settings = controller._dataModel.viewSettings
    originalVisuals = (settings.showHUD, settings.selHighlightMode,
                       settings.ambientLightOnly, settings.cameraPrim)
    originalSelection = tuple(controller._dataModel.selection.getPrimPaths())
    fixture = Path(owner.api.stage.GetRootLayer().identifier).name.lower()
    defaultCount = 6500 if "wind" in fixture else 8000 if "collide" in fixture else 0
    expectedCount = int(os.environ.get("USDGEN_PLAYBLAST_EXPECTED_CURVES", str(defaultCount)))
    groomPath = os.environ.get("USDGEN_PLAYBLAST_GROOM_PATH", "/World/Groom/Fur")
    report["expectedCurveCount"] = expectedCount
    report["groomPath"] = groomPath
    started = time.monotonic()

    def nativeReady():
        if owner.ready():
            return True
        if owner.lastError:
            raise RuntimeError(owner.lastError)
        return False

    def recordAdvance():
        assert owner.ready(), "Play advanced before actual native/render/presentation readiness."
        status = owner.native.status(owner.api.stage, view._renderer, owner.frame())
        assert status["state"] == "ready", status
        count = curveCount()
        report["playAdvances"].append({"frame": owner.frame(), "signature": status["signature"],
                                       "curveCount": count})
        originalAdvance()

    def recordCapture():
        assert owner.ready(), "Capture started before a real presented complete frame."
        status = owner.native.status(owner.api.stage, view._renderer, owner.frame())
        assert status["state"] == "ready", status
        count = curveCount()
        report["captureFrames"].append({"frame": owner.frame(), "signature": status["signature"],
                                        "curveCount": count})
        return originalCapture()

    def curveCount():
        stats = owner.native.lib.UsdGenBrush_GroomCurveStats
        stats.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_longlong),
                          ctypes.POINTER(ctypes.c_double)]
        stats.restype = ctypes.c_int
        curves, length = ctypes.c_longlong(), ctypes.c_double()
        found = stats(groomPath.encode("utf-8"), ctypes.byref(curves), ctypes.byref(length))
        assert found == 1, "No native published curve statistics for %s" % groomPath
        assert curves.value > 0, "Published groom is empty."
        if expectedCount:
            assert curves.value == expectedCount, (owner.frame(), curves.value, expectedCount)
        return int(curves.value)

    try:
        settings.showHUD = False
        settings.selHighlightMode = SelectionHighlightModes.NEVER
        controller._dataModel.selection.clearPrims()
        controller.realStartTimeCode, controller.realEndTimeCode, controller.step = startFrame, startFrame + 5, 1
        controller._UpdateTimeSamples(False)
        assert tuple(controller._timeSamples) == frameSequence
        controller.setFrame(startFrame)
        _wait(nativeReady, view)
        # Let asynchronous population and initial free-camera framing settle
        # before choosing an authored camera through the actual camera menu.
        # A CLI camera at engine startup can precede its Hydra population.
        cameraPath = os.environ.get("USDGEN_PLAYBLAST_CAMERA",
                                    "/World/CamMotion" if "collide" in fixture else "/World/Cam")
        actions = [action for action in ui.menuCameraSelect.actions()
                   if str(action.data()) == cameraPath]
        assert len(actions) == 1, "No unique camera menu action for %s" % cameraPath
        actions[0].trigger()
        if ui.actionAmbient_Only.isChecked():
            ui.actionAmbient_Only.trigger()
        assert not settings.ambientLightOnly, "Camera light remains enabled."
        owner.fence.invalidate()
        view.update()
        _wait(nativeReady, view)
        report["camera"] = cameraPath
        report["cameraLightEnabled"] = settings.ambientLightOnly
        controller._advanceFrame = recordAdvance
        ui.playButton.click()
        report["playEventPulses"] = _wait(lambda: len(report["playAdvances"]) >= 2, view)
        ui.playButton.click()
        assert not controller._dataModel.playing
        report["scrubDurations"] = []
        report["scrubStates"] = []
        for frame in (startFrame + 5, startFrame + 1, startFrame + 4, startFrame):
            begin = time.monotonic()
            controller.setFrame(frame)
            report["scrubDurations"].append(time.monotonic() - begin)
            assert owner.frame() == frame
            report["scrubStates"].append(owner.native.status(
                owner.api.stage, view._renderer, frame)["state"])
        report["scrubEventPulses"] = _wait(nativeReady, view)
        assert view._allowAsync and not controller._dataModel.playing
        assert max(report["scrubDurations"]) < 1.0, "Scrubbing blocked the GUI for a second."
        screenshot = str(Path(output).with_suffix(".viewer.png"))
        assert owner.button.isVisible() and owner.button.text() == "Playblast"
        assert controller.GrabWindowShot().save(screenshot, "PNG")
        savedFrame = owner.frame()
        savedControls = {name: getattr(ui, name).isEnabled() for name in
                         ("frameSlider", "frameField", "playButton")}
        with patch.object(QtWidgets.QFileDialog, "getSaveFileName", return_value=(output, "MP4")), \
                patch.object(QtWidgets.QMessageBox, "warning", side_effect=lambda *args:
                             report["warnings"].append(str(args[-1]))), \
                patch.object(owner.api, "GrabViewportShot", side_effect=recordCapture):
            owner.button.click()
            assert owner.movie is not None, report["warnings"]
            report["movieEventPulses"] = _wait(lambda: owner.movie is None, view)
        assert not report["warnings"], report["warnings"]
        assert owner.frame() == savedFrame and not controller._dataModel.playing
        assert all(getattr(ui, name).isEnabled() == enabled for name, enabled in savedControls.items())
        assert [entry["frame"] for entry in report["captureFrames"]] == list(frameSequence)
        probe = shutil.which("ffprobe")
        assert probe, "ffprobe is required to verify the movie."
        result = subprocess.run([probe, "-v", "error", "-count_frames", "-select_streams", "v:0",
            "-show_entries", "stream=nb_read_frames,avg_frame_rate,duration,width,height",
            "-of", "json", output], check=True, capture_output=True, text=True)
        stream = json.loads(result.stdout)["streams"][0]
        assert int(stream["nb_read_frames"]) == 6, stream
        numerator, denominator = map(float, stream["avg_frame_rate"].split("/"))
        assert abs(numerator / denominator - controller.framesPerSecond) < 1e-6, stream
        assert abs(float(stream["duration"]) - 6 / controller.framesPerSecond) < 0.002, stream
        report["ffprobe"] = stream
        digest = hashlib.sha256(Path(output).read_bytes()).hexdigest()
        with patch.object(QtWidgets.QFileDialog, "getSaveFileName", return_value=(output, "MP4")):
            owner.button.click()
            assert owner.movie is not None
            owner.movie.cancel()
            _wait(lambda: owner.movie is None, view, 10)
        assert hashlib.sha256(Path(output).read_bytes()).hexdigest() == digest
        assert owner.frame() == savedFrame and not controller._dataModel.playing
        _wait(nativeReady, view)
        def captureError():
            raise RuntimeError("Acceptance capture error injection")
        with patch.object(QtWidgets.QFileDialog, "getSaveFileName", return_value=(output, "MP4")), \
                patch.object(owner.api, "GrabViewportShot", side_effect=captureError), \
                patch.object(QtWidgets.QMessageBox, "warning", side_effect=lambda *args:
                             report["warnings"].append(str(args[-1]))):
            owner.button.click()
            assert owner.movie is not None
            _wait(lambda: owner.movie is None, view)
        assert any("capture error injection" in warning for warning in report["warnings"])
        assert hashlib.sha256(Path(output).read_bytes()).hexdigest() == digest
        assert owner.frame() == savedFrame and not controller._dataModel.playing
        report["cancelRestored"] = report["captureErrorRestored"] = True
        report["elapsedSeconds"] = time.monotonic() - started
        Path(output).with_suffix(".acceptance.json").write_text(json.dumps(report, indent=2))
        print("PASS: real native-gated Play, progressive scrub, six-frame MP4, Cancel/error restore")
        return 0
    finally:
        owner.stop()
        controller._advanceFrame = originalAdvance
        controller.realStartTimeCode, controller.realEndTimeCode, controller.step = originalRange
        controller._UpdateTimeSamples(False)
        controller.setFrame(originalFrame)
        settings.showHUD, settings.selHighlightMode = originalVisuals[:2]
        if ui.actionAmbient_Only.isChecked() != originalVisuals[2]:
            ui.actionAmbient_Only.trigger()
        settings.cameraPrim = originalVisuals[3]
        controller._dataModel.selection.clearPrims()
        for path in originalSelection:
            controller._dataModel.selection.addPrimPath(path)
