# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Exercise asynchronous playback/capture flow without pxr, Qt or a viewport."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

folder = Path(__file__).resolve().parents[1] / "python/usdGenTools"
package = types.ModuleType("playbackTestPackage")
package.__path__ = [str(folder)]
sys.modules[package.__name__] = package
qt = types.ModuleType("pxr.Usdviewq.qt")
qt.QtCore = types.SimpleNamespace(QObject=object, QTimer=types.SimpleNamespace(singleShot=Mock()),
    QProcess=types.SimpleNamespace(ProcessState=types.SimpleNamespace(NotRunning=0),
        ExitStatus=types.SimpleNamespace(NormalExit=0),
        ProcessError=types.SimpleNamespace(FailedToStart=0)))
qt.QtWidgets = types.SimpleNamespace()
sys.modules[qt.__name__] = qt
spec = importlib.util.spec_from_file_location("playbackTestPackage.playback", folder / "playback.py")
playback = importlib.util.module_from_spec(spec)
spec.loader.exec_module(playback)


class PlaybackFlowTests(unittest.TestCase):
    def owner(self):
        owner = object.__new__(playback.PlaybackController)
        owner.controller = Mock()
        owner.api = Mock()
        owner.controller._dataModel.playing = True
        owner.controller.framesPerSecond = 24
        owner.view = Mock()
        owner.view.GetCurrentRendererId.return_value = "HdStormRendererPlugin"
        owner.movie = None
        owner.closing = False
        owner.deadline = 0
        owner.waitStarted = None
        owner.waitFrame = None
        owner.lastError = ""
        owner.errorSerial = 0
        owner.renderFailure = None
        owner.frame = Mock(return_value=1)
        owner.ready = Mock(return_value=False)
        owner.stop = Mock()
        return owner

    def test_pending_play_pumps_view_without_advancing(self):
        owner = self.owner()
        with patch.object(playback.time, "monotonic", return_value=10):
            owner.advance()
        owner.view.update.assert_called_once()
        owner.controller._advanceFrame.assert_not_called()
        owner.stop.assert_not_called()

    def test_ready_play_advances_once_and_obeys_fps_deadline(self):
        owner = self.owner()
        owner.ready.return_value = True
        with patch.object(playback.time, "monotonic", return_value=10):
            owner.advance()
            owner.advance()
        owner.controller._advanceFrame.assert_called_once()
        self.assertAlmostEqual(owner.deadline, 10 + 1 / 24)

    def test_pause_and_movie_prevent_timer_advance(self):
        owner = self.owner()
        owner.controller._dataModel.playing = False
        owner.advance()
        owner.controller._dataModel.playing = True
        owner.movie = object()
        owner.advance()
        owner.ready.assert_not_called()
        owner.controller._advanceFrame.assert_not_called()

    def test_resume_same_pending_frame_restarts_and_enforces_timeout(self):
        owner = self.owner()
        del owner.stop  # Exercise the real stop/reset path.
        with patch.dict(playback.os.environ, {"USDGEN_USDVIEW_FRAME_TIMEOUT": "10"}):
            with patch.object(playback.time, "monotonic", return_value=10):
                owner.advance()
            self.assertEqual(owner.waitFrame, 1)
            owner.stop()
            self.assertIsNone(owner.waitStarted)
            owner.controller._dataModel.playing = False
            with patch.object(playback.time, "monotonic", return_value=99):
                owner.advance()
            self.assertIsNone(owner.waitStarted)
            owner.controller._dataModel.playing = True
            with patch.object(playback.time, "monotonic", return_value=100):
                owner.advance()
            self.assertEqual(owner.waitFrame, 1)
            self.assertEqual(owner.waitStarted, 100)
            with patch.object(playback.time, "monotonic", return_value=111):
                owner.advance()
        owner.controller.statusMessage.assert_called_once_with(
            "Timed out waiting for complete groom frame 1.")
        owner.controller._advanceFrame.assert_not_called()

    def test_native_failure_stops_play(self):
        owner = self.owner()
        owner.lastError = "Cook failed"
        owner.advance()
        owner.stop.assert_called_once_with("Cook failed")
        owner.controller._advanceFrame.assert_not_called()

    def test_non_storm_play_delegates_stock_callback(self):
        owner = self.owner()
        owner.view.GetCurrentRendererId.return_value = "HdMoonrayRendererPlugin"
        with patch.object(playback, "_originalAdvance", Mock()) as original:
            owner.advance()
            original.assert_called_once_with(owner.controller)
        owner.ready.assert_not_called()
        owner.controller._qtimer.setInterval.assert_called_once_with(0)

    def movie(self, directory):
        movie = object.__new__(playback.Playblast)
        owner = self.owner()
        movie.owner = owner
        movie.frames = (1, 2)
        movie.index = 0
        movie.directory = directory
        movie.finished = movie.cancelled = False
        movie.dimensions = None
        movie.waitStarted = 10
        movie.stage = owner.api.stage
        movie.progress = Mock()
        movie.finish = Mock()
        movie.setFrame = Mock()
        movie.encode = Mock()
        owner.ready.return_value = True
        owner.signature = Mock(return_value="published")
        owner.fence = Mock()
        owner.fence.ready.return_value = True
        owner.fence.drawn = (1, "published")
        image = Mock()
        image.isNull.return_value = False
        image.width.return_value = 640
        image.height.return_value = 480
        image.save.return_value = True
        owner.api.GrabViewportShot.return_value = image
        return movie, image

    def test_pending_movie_never_captures(self):
        with tempfile.TemporaryDirectory() as directory:
            movie, image = self.movie(directory)
            movie.owner.ready.return_value = False
            with patch.object(playback.time, "monotonic", return_value=11):
                movie.tick()
            movie.owner.api.GrabViewportShot.assert_not_called()
            image.save.assert_not_called()
            self.assertEqual(movie.index, 0)

    def test_capture_rechecks_publication_and_rejects_late_change(self):
        with tempfile.TemporaryDirectory() as directory:
            movie, image = self.movie(directory)
            movie.owner.signature.side_effect = ["published", "changed", "changed"]
            movie.tick()
            image.save.assert_not_called()
            self.assertEqual(movie.index, 0)

    def test_complete_frame_sequence_encodes_after_last_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            movie, image = self.movie(directory)
            movie.tick()
            movie.encode.assert_not_called()
            self.assertEqual(movie.index, 1)
            movie.owner.frame.return_value = 2
            movie.owner.fence.drawn = (2, "published")
            movie.tick()
            self.assertEqual(image.save.call_count, 2)
            self.assertEqual(movie.index, 2)
            movie.encode.assert_called_once()

    def test_capture_error_finishes_without_advance(self):
        with tempfile.TemporaryDirectory() as directory:
            movie, image = self.movie(directory)
            image.save.return_value = False
            movie.tick()
            self.assertEqual(movie.index, 0)
            movie.finish.assert_called_once()
            self.assertIn("Could not save", movie.finish.call_args.args[0])

    def restorableMovie(self, directory):
        movie, image = self.movie(str(Path(directory) / "frames"))
        Path(movie.directory).mkdir()
        movie.partial = str(Path(directory) / "partial.mp4")
        Path(movie.partial).write_bytes(b"unfinished")
        movie.destination = str(Path(directory) / "existing.mp4")
        Path(movie.destination).write_bytes(b"previous movie")
        movie.timer = Mock()
        movie.process = None
        movie.pendingError = ""
        movie.originalFrame = 17
        movie.wasPlaying = True
        widget = Mock()
        movie.controls = [(widget, True)]
        movie.owner.movie = movie
        movie.owner.fence = Mock()
        movie.deleteLater = Mock()
        del movie.finish
        return movie, widget

    def test_cancel_preserves_output_and_restores_frame_controls_and_play(self):
        with tempfile.TemporaryDirectory() as directory:
            movie, widget = self.restorableMovie(directory)
            movie.cancel()
            self.assertEqual(Path(movie.destination).read_bytes(), b"previous movie")
            self.assertFalse(Path(movie.partial).exists())
            self.assertFalse(Path(movie.directory).exists())
            widget.setEnabled.assert_called_once_with(True)
            movie.owner.controller.setFrame.assert_called_once_with(17)
            movie.owner.controller._playClicked.assert_called_once()
            self.assertIsNone(movie.owner.movie)

    def test_cancel_encoder_waits_for_exit_before_cleaning_files(self):
        with tempfile.TemporaryDirectory() as directory:
            movie, widget = self.restorableMovie(directory)
            movie.process = Mock()
            movie.process.state.return_value = 1
            movie.cancel()
            movie.process.terminate.assert_called_once()
            self.assertTrue(Path(movie.partial).exists())
            self.assertTrue(Path(movie.directory).exists())
            movie.process.state.return_value = 0
            movie.encoded(1, 1)
            self.assertFalse(Path(movie.partial).exists())
            self.assertEqual(Path(movie.destination).read_bytes(), b"previous movie")

    def test_bootstrap_retries_until_stageview_and_timer_exist(self):
        controller = types.SimpleNamespace(_stageView=None)
        api = types.SimpleNamespace(_UsdviewApi__appController=controller)
        qt.QtCore.QTimer.singleShot.reset_mock()
        playback._attach(api)
        qt.QtCore.QTimer.singleShot.assert_called_once()
        self.assertEqual(qt.QtCore.QTimer.singleShot.call_args.args[0], 100)
        controller._stageView = object()
        controller._qtimer = object()
        with patch.object(playback, "PlaybackController", return_value="attached") as factory:
            playback._attach(api)
            factory.assert_called_once_with(api, controller)
        self.assertEqual(controller._usdgenPlayback, "attached")

    def test_unattached_controller_uses_original_callback(self):
        original = Mock()
        app = types.ModuleType("pxr.Usdviewq.appController")
        app.AppController = type("AppController", (), {"_advanceFrameForPlayback": original})
        view = types.ModuleType("pxr.Usdviewq.stageView")
        view.StageView = type("StageView", (), {"_paintGLWithRenderer": Mock()})
        with patch.dict(sys.modules, {app.__name__: app, view.__name__: view}), \
                patch.object(playback, "_installed", False):
            playback.Install(None)
            controller = types.SimpleNamespace()
            app.AppController._advanceFrameForPlayback(controller)
            original.assert_called_once_with(controller)

    def test_caught_error_signal_invalidates_draw_and_next_clean_paint_recovers(self):
        owner = self.owner()
        owner.signature = Mock(return_value="published")
        owner.fence = playback.PresentationFence()
        app = types.ModuleType("pxr.Usdviewq.appController")
        app.AppController = type("AppController", (), {"_advanceFrameForPlayback": Mock()})
        view = types.ModuleType("pxr.Usdviewq.stageView")
        originalPaint = Mock(side_effect=lambda *_: owner._renderError("caught paint error"))
        view.StageView = type("StageView", (), {"_paintGLWithRenderer": originalPaint})
        renderer = Mock()
        renderer.IsConverged.return_value = True
        with patch.dict(sys.modules, {app.__name__: app, view.__name__: view}), \
                patch.object(playback, "_installed", False):
            playback.Install(None)
            viewport = view.StageView()
            viewport._usdgenPlayback = owner
            viewport._paintGLWithRenderer(renderer)
            self.assertEqual(owner.errorSerial, 1)
            self.assertIsNotNone(owner.renderFailure)
            owner.fence.swapped(1, "published")
            self.assertFalse(owner.fence.ready(1, "published"))
            originalPaint.side_effect = None
            viewport._paintGLWithRenderer(renderer)
            self.assertIsNone(owner.renderFailure)
            owner.fence.swapped(1, "published")
            self.assertTrue(owner.fence.ready(1, "published"))


if __name__ == "__main__":
    unittest.main()
