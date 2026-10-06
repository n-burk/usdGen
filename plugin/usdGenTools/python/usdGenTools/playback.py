# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Wait for groom publication and viewport presentation without blocking Qt."""

import math
import os
import shutil
import tempfile
import time

from pxr.Usdviewq.qt import QtCore, QtWidgets
from .playbackState import PresentationFence, movieFrames

_installed = False
_originalAdvance = None


def _timeout():
    try:
        seconds = float(os.environ.get("USDGEN_USDVIEW_FRAME_TIMEOUT", "900"))
        return seconds if math.isfinite(seconds) and seconds >= 0 else 900.0
    except ValueError:
        return 900.0


def Install(api):
    global _installed, _originalAdvance
    from pxr.Usdviewq.appController import AppController
    from pxr.Usdviewq.stageView import StageView
    if not _installed:
        original = StageView._paintGLWithRenderer
        originalAdvance = AppController._advanceFrameForPlayback
        _originalAdvance = originalAdvance

        def paint(view, renderer):
            owner = getattr(view, "_usdgenPlayback", None)
            if owner is None:
                return original(view, renderer)
            owner.fence.invalidate()
            frame = owner.frame()
            errorSerial = owner.errorSerial
            before = owner.signature()
            try:
                original(view, renderer)
            except Exception as exc:
                owner.renderFailure = (frame, str(exc))
                raise
            if owner.errorSerial != errorSerial:
                # Some usdview paint helpers catch renderer exceptions and
                # emit signalErrorMessage instead of propagating them.
                # A converged flag cannot make that failed draw valid.
                return
            owner.renderFailure = None
            after = owner.signature()
            owner.fence.drawnFrame(frame, before, after,
                                   bool(renderer.IsConverged()))

        def advance(controller):
            owner = getattr(controller, "_usdgenPlayback", None)
            if owner is not None:
                owner.advance()
            else:
                originalAdvance(controller)

        StageView._paintGLWithRenderer = paint
        AppController._advanceFrameForPlayback = advance
        _installed = True
    # Plugin registration precedes StageView/timer construction.
    QtCore.QTimer.singleShot(0, lambda: _attach(api))


def _attach(api, attempts=0):
    controller = getattr(api, "_UsdviewApi__appController", None)
    if controller is None:
        return
    if getattr(controller, "_stageView", None) is None or not hasattr(controller, "_qtimer"):
        if attempts < 100:
            QtCore.QTimer.singleShot(100, lambda: _attach(api, attempts + 1))
        return
    if getattr(controller, "_usdgenPlayback", None) is None:
        controller._usdgenPlayback = PlaybackController(api, controller)


class PlaybackController(QtCore.QObject):
    def __init__(self, api, controller):
        super().__init__(controller._mainWindow)
        self.api = api
        self.controller = controller
        self.view = controller._stageView
        self.fence = PresentationFence()
        self.view._usdgenPlayback = self
        self.view.frameSwapped.connect(self._swapped)
        self.view.signalErrorMessage.connect(self._renderError)
        self.native = None
        self.lastError = ""
        self.renderFailure = None
        self.errorSerial = 0
        self.deadline = 0.0
        self.waitStarted = None
        self.waitFrame = None
        self.movie = None
        self.closing = False
        self.button = QtWidgets.QPushButton("Playblast", controller._ui.playButton.parent())
        self.button.setObjectName("usdGenPlayblastButton")
        self.button.setToolTip("Save fully populated viewport frames as an MP4 at timeline FPS")
        controller._ui.playButtonContainer.addWidget(self.button)
        self.button.clicked.connect(self.startMovie)
        controller._mainWindow.installEventFilter(self)
        # The stock timer's zero interval otherwise spins while waiting.
        controller._qtimer.setInterval(16)

    def eventFilter(self, watched, event):
        if event.type() == QtCore.QEvent.Type.Close and self.movie:
            self.closing = True
            self.movie.cancel()
        return False

    def frame(self):
        return float(self.controller._dataModel.currentFrame.GetValue())

    def signature(self):
        if self.renderFailure is not None:
            failedFrame, error = self.renderFailure
            if failedFrame == self.frame():
                self.lastError = "Viewport render failed: %s" % error
                return None
            self.renderFailure = None
        try:
            if self.native is None:
                from .frameStatusApi import FrameStatusApi
                self.native = FrameStatusApi()
            result = self.native.status(self.api.stage, self.view._renderer,
                                        self.frame())
            state = result.get("state", "unavailable")
            self.lastError = result.get("error", "") if state != "pending" else ""
            if state == "ready":
                return result["signature"]
            if state not in ("pending", "ready") and not self.lastError:
                self.lastError = "Native frame readiness is %s." % state
        except Exception as exc:
            self.lastError = "Cannot verify groom frame readiness: %s" % exc
        return None

    def _swapped(self):
        self.fence.swapped(self.frame(), self.signature())

    def _renderError(self, message):
        self.errorSerial += 1
        self.renderFailure = (self.frame(), str(message))
        self.lastError = "Viewport render failed: %s" % message
        self.fence.invalidate()
        if self.movie:
            self.movie.finish(self.lastError)
        elif self.controller._dataModel.playing:
            self.stop(self.lastError)

    def ready(self):
        return self.fence.ready(self.frame(), self.signature())

    def stop(self, message=""):
        if self.controller._dataModel.playing:
            self.controller._ui.playButton.setChecked(False)
            self.controller._playClicked()
        self.deadline = 0.0
        self.waitStarted = None
        if message:
            self.controller.statusMessage(message)

    def advance(self):
        if self.movie or not self.controller._dataModel.playing:
            return
        rendererId = str(self.view.GetCurrentRendererId()).lower()
        if rendererId not in ("gl", "storm", "hdstormrendererplugin"):
            # Complete-operation delegates retain usdview's playback path.
            self.controller._qtimer.setInterval(0)
            _originalAdvance(self.controller)
            return
        self.controller._qtimer.setInterval(16)
        now = time.monotonic()
        frame = self.frame()
        if frame != self.waitFrame or self.waitStarted is None:
            self.waitFrame, self.waitStarted = frame, now
        if not self.ready():
            if self.lastError:
                self.stop(self.lastError)
            elif (_timeout() and self.waitStarted is not None and
                  now - self.waitStarted > _timeout()):
                self.stop("Timed out waiting for complete groom frame %g." % frame)
            else:
                self.view.update()
            return
        if now < self.deadline:
            return
        fps = float(self.controller.framesPerSecond)
        if not math.isfinite(fps) or fps <= 0:
            self.stop("Timeline FPS must be positive and finite.")
            return
        self.deadline = now + 1.0 / fps
        self.controller._advanceFrame()

    def startMovie(self):
        if self.movie:
            return
        encoder = os.environ.get("USDGEN_FFMPEG") or shutil.which("ffmpeg")
        if not encoder or not os.path.isfile(encoder):
            QtWidgets.QMessageBox.warning(self.controller._mainWindow, "Playblast",
                "ffmpeg was not found. Install it on PATH or set USDGEN_FFMPEG.")
            return
        try:
            fps = float(self.controller.framesPerSecond)
            frames = movieFrames(self.controller._timeSamples, fps)
        except ValueError as exc:
            QtWidgets.QMessageBox.warning(self.controller._mainWindow, "Playblast", str(exc))
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self.controller._mainWindow, "Save Playblast", "playblast.mp4", "MP4 movie (*.mp4)")
        if not path:
            return
        if not path.lower().endswith(".mp4"):
            path += ".mp4"
        try:
            self.movie = Playblast(self, path, encoder, frames, fps)
            self.movie.start()
        except Exception as exc:
            if self.movie:
                self.movie.finish(str(exc))
            else:
                QtWidgets.QMessageBox.warning(self.controller._mainWindow, "Playblast", str(exc))


class Playblast(QtCore.QObject):
    def __init__(self, owner, destination, encoder, frames, fps):
        super().__init__(owner)
        self.owner = owner
        self.destination, self.encoder = destination, encoder
        self.frames, self.fps = frames, fps
        self.index = 0
        fd, self.partial = tempfile.mkstemp(prefix=".usdgen-playblast-", suffix=".mp4",
                                          dir=os.path.dirname(os.path.abspath(destination)))
        os.close(fd)
        self.directory = tempfile.mkdtemp(prefix="usdgen-playblast-")
        self.originalFrame = owner.frame()
        self.wasPlaying = bool(owner.controller._dataModel.playing)
        self.controls = []
        self.cancelled = False
        self.finished = False
        self.dimensions = None
        self.process = None
        self.pendingError = ""
        self.stage = owner.api.stage
        self.progress = QtWidgets.QProgressDialog("Preparing viewport frames…", "Cancel",
                                                  0, len(frames) + 1,
                                                  owner.controller._mainWindow)
        self.progress.setWindowTitle("Playblast")
        self.progress.setMinimumDuration(0)
        self.progress.setAutoClose(False)
        self.progress.setAutoReset(False)
        self.progress.canceled.connect(self.cancel)
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(16)
        self.timer.timeout.connect(self.tick)

    def start(self):
        self.owner.stop()
        ui = self.owner.controller._ui
        for name in ("frameSlider", "frameField", "rangeBegin", "rangeEnd", "stepSize", "playButton"):
            widget = getattr(ui, name, None)
            if widget is not None:
                self.controls.append((widget, widget.isEnabled()))
                widget.setEnabled(False)
        for widget in (self.owner.view, self.owner.button):
            self.controls.append((widget, widget.isEnabled()))
            widget.setEnabled(False)
        self.progress.show()
        self.setFrame()
        self.timer.start()

    def setFrame(self):
        self.owner.fence.invalidate()
        self.owner.controller.setFrame(self.frames[self.index])
        self.waitStarted = time.monotonic()
        self.owner.view.update()

    def tick(self):
        if self.finished or self.cancelled:
            return
        try:
            if self.owner.api.stage != self.stage:
                raise RuntimeError("The stage changed during playblast.")
            if self.owner.frame() != self.frames[self.index]:
                raise RuntimeError("The timeline changed during playblast.")
            if not self.owner.ready():
                if self.owner.lastError:
                    raise RuntimeError(self.owner.lastError)
                if _timeout() and time.monotonic() - self.waitStarted > _timeout():
                    raise RuntimeError("Timed out waiting for frame %g." % self.frames[self.index])
                self.owner.view.update()
                return
            signature = self.owner.signature()
            image = self.owner.api.GrabViewportShot()
            # QOpenGLWidget.grabFramebuffer may repaint without emitting a
            # frameSwapped signal. The previous frame was presented already;
            # any extra draw must also carry this exact complete generation.
            if self.owner.fence.drawn != (self.owner.frame(), self.owner.signature()):
                self.owner.view.update()
                return
            if signature != self.owner.signature():
                self.owner.view.update()
                return
            if image is None or image.isNull():
                raise RuntimeError("Viewport capture returned an empty image.")
            dimensions = (image.width(), image.height())
            if self.dimensions is None:
                self.dimensions = dimensions
            elif dimensions != self.dimensions:
                raise RuntimeError("The viewport size changed during playblast.")
            path = os.path.join(self.directory, "%08d.png" % self.index)
            if not image.save(path, "PNG"):
                raise RuntimeError("Could not save viewport frame %g." % self.frames[self.index])
            self.index += 1
            self.progress.setValue(self.index)
            if self.index == len(self.frames):
                self.encode()
            else:
                self.progress.setLabelText("Capturing frame %g (%d/%d)" %
                                           (self.frames[self.index], self.index + 1, len(self.frames)))
                self.setFrame()
        except Exception as exc:
            self.finish(str(exc))

    def encode(self):
        self.timer.stop()
        self.progress.setLabelText("Encoding movie…")
        self.process = QtCore.QProcess(self)
        self.process.finished.connect(self.encoded)
        self.process.errorOccurred.connect(self.encodingError)
        self.process.start(self.encoder, ["-hide_banner", "-loglevel", "error", "-y",
            "-framerate", "%.12g" % self.fps, "-start_number", "0", "-i",
            os.path.join(self.directory, "%08d.png"), "-frames:v", str(len(self.frames)),
            "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-c:v", "libx264",
            "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart", self.partial])

    def encodingError(self, error):
        if error == QtCore.QProcess.ProcessError.FailedToStart:
            self.finish("Could not start ffmpeg: %s" % self.process.errorString())

    def encoded(self, code, status):
        if self.cancelled:
            self.finish(self.pendingError)
        elif code != 0 or status != QtCore.QProcess.ExitStatus.NormalExit:
            detail = bytes(self.process.readAllStandardError()).decode("utf-8", "replace")[-4000:]
            self.finish("Movie encoding failed: %s" % detail)
        else:
            try:
                if os.path.getsize(self.partial) == 0:
                    raise RuntimeError("Encoder produced an empty movie.")
                os.replace(self.partial, self.destination)
                self.finish()
                self.owner.controller.statusMessage("Saved playblast: %s" % self.destination)
            except Exception as exc:
                self.finish(str(exc))

    def cancel(self):
        self.cancelled = True
        self.timer.stop()
        if self.process and self.process.state() != QtCore.QProcess.ProcessState.NotRunning:
            self.process.terminate()
            QtCore.QTimer.singleShot(1000, self._killEncoder)
        else:
            self.finish()

    def _killEncoder(self):
        if self.process and self.process.state() != QtCore.QProcess.ProcessState.NotRunning:
            self.process.kill()

    def finish(self, error=""):
        if self.finished:
            return
        if self.process and self.process.state() != QtCore.QProcess.ProcessState.NotRunning:
            self.pendingError = error
            self.cancelled = True
            self.timer.stop()
            self.process.terminate()
            QtCore.QTimer.singleShot(1000, self._killEncoder)
            return
        self.finished = True
        self.timer.stop()
        self.progress.close()
        self.owner.movie = None
        for widget, enabled in self.controls:
            widget.setEnabled(enabled)
        if self.owner.api.stage == self.stage:
            self.owner.controller.setFrame(self.originalFrame)
        self.owner.fence.invalidate()
        self.owner.deadline = 0.0
        if (self.wasPlaying and not error and not self.owner.closing and
                self.owner.api.stage == self.stage):
            self.owner.controller._ui.playButton.setChecked(True)
            self.owner.controller._playClicked()
        shutil.rmtree(self.directory, ignore_errors=True)
        if os.path.exists(self.partial):
            os.unlink(self.partial)
        if error and not self.owner.closing:
            QtWidgets.QMessageBox.warning(self.owner.controller._mainWindow, "Playblast", error)
        self.deleteLater()
