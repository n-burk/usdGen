# usdGenTonicTools.tonicViewport -- the one viewport controller
# (plan/18 section 3.1, 3.2, 3.3, 3.4).
#
# Qt lives here and in the workspace dock, nowhere else (plan/08 section
# 1.2). Two filters:
#
#   * one on the StageView, which turns mouse presses, moves, releases and
#     hovers into ToolLoop calls over a camera resolved once per gesture;
#   * one on QApplication, installed only while the tool is active, which
#     owns the key table. It has to be application-level because usdview's
#     AppEventFilter swallows Escape and refocuses the main window on every
#     mouse move (plan/08 section 3.5); a filter installed later than
#     usdview's runs before it, which is what lets Escape reach us.
#
# The gesture contract is plan/18 section 3.2 exactly: no stage traffic
# inside a drag, one Tonic_Publish per event, and an idle QTimer that swaps
# committed layers and drains finished bakes when nothing is being dragged.
from __future__ import annotations

import time

from . import tonicCamera
from . import tonicGizmo
from . import tonicHierarchy
from . import tonicLadder
from . import tonicLib
from . import tonicLoops
from . import tonicModes

# plan/18 section 3.2: a move under this many pixels is not a move.
MOVE_THRESHOLD_PX = 2.0
# ... and a hover under this many is not a hover.
HOVER_THRESHOLD_PX = 3.0
# plan/18 section 3.3: the idle pump's period while anything is pending.
PUMP_INTERVAL_MS = 50


def stageViewOf(usdviewApi):
    """The StageView widget behind `usdviewApi`, or None.

    UsdviewApi exposes no `stageView` property in this USD build, which is
    why the P2 controller's `usdviewApi.stageView` silently produced None
    and its event filter was never installed (plan/18 finding F3). The
    public route is the widget tree.
    """
    if usdviewApi is None:
        return None
    view = getattr(usdviewApi, "stageView", None)
    if view is not None:
        return view
    for name in ("_UsdviewApi__appController", "_appController"):
        controller = getattr(usdviewApi, name, None)
        view = getattr(controller, "_stageView", None)
        if view is not None:
            return view
    window = getattr(usdviewApi, "qMainWindow", None)
    if window is None:
        return None
    from pxr.Usdviewq.stageView import StageView
    return window.findChild(StageView)


def modifierSet(event):
    """A QKeyEvent/QMouseEvent's modifiers as the Qt-free string set."""
    from pxr.Usdviewq.qt import QtCore
    mods = event.modifiers()
    names = set()
    table = (("shift", QtCore.Qt.KeyboardModifier.ShiftModifier),
             ("ctrl", QtCore.Qt.KeyboardModifier.ControlModifier),
             ("alt", QtCore.Qt.KeyboardModifier.AltModifier),
             ("meta", QtCore.Qt.KeyboardModifier.MetaModifier))
    for name, flag in table:
        if mods & flag:
            names.add(name)
    return frozenset(names)


def keyName(event):
    """A QKeyEvent's key as the Qt-free string HotkeyAction takes."""
    from pxr.Usdviewq.qt import QtCore
    key = event.key()
    named = {QtCore.Qt.Key.Key_Escape: "escape",
             QtCore.Qt.Key.Key_Delete: "delete",
             QtCore.Qt.Key.Key_Backspace: "backspace",
             QtCore.Qt.Key.Key_Return: "enter",
             QtCore.Qt.Key.Key_Enter: "enter",
             QtCore.Qt.Key.Key_Up: "up",
             QtCore.Qt.Key.Key_Down: "down",
             QtCore.Qt.Key.Key_Left: "left",
             QtCore.Qt.Key.Key_Right: "right",
             QtCore.Qt.Key.Key_F8: "f8",
             QtCore.Qt.Key.Key_F9: "f9",
             QtCore.Qt.Key.Key_F10: "f10",
             QtCore.Qt.Key.Key_F11: "f11",
             QtCore.Qt.Key.Key_BracketLeft: "[",
             QtCore.Qt.Key.Key_BracketRight: "]"}
    if key in named:
        return named[key]
    # QKeyEvent.text() is empty for some accelerators after usdview has
    # accepted the event (notably L on the stock StageView).  Tool hotkeys
    # are physical letter keys, so normalize the Qt keycode first instead
    # of silently leaving the prior brush active.
    if QtCore.Qt.Key.Key_A <= key <= QtCore.Qt.Key.Key_Z:
        return chr(int(key)).lower()
    text = event.text()
    if text and text.strip():
        return text.strip().lower()[:1]
    return ""


def eventPixels(view, event):
    """An event position as PHYSICAL, top-left-origin viewport pixels.

    The same scaling StageView's own handlers do, and the space
    computeWindowViewport reports, so a pixel here is the pixel K11 picks.
    """
    ratio = 1.0
    try:
        ratio = float(view.devicePixelRatioF())
    except AttributeError:
        pass
    position = getattr(event, "position", None)
    if position is not None:
        point = position()
        return (point.x() * ratio, point.y() * ratio)
    return (event.x() * ratio, event.y() * ratio)


def askSaveFile(parent, caption, nameFilter, initial=""):
    """A modal save dialog; "" when the artist cancels."""
    from pxr.Usdviewq.qt import QtWidgets
    path, _chosen = QtWidgets.QFileDialog.getSaveFileName(
        parent, caption, initial, nameFilter)
    return str(path) if path else ""


def askOpenFile(parent, caption, nameFilter, initial=""):
    """A modal open dialog; "" when the artist cancels."""
    from pxr.Usdviewq.qt import QtWidgets
    path, _chosen = QtWidgets.QFileDialog.getOpenFileName(
        parent, caption, initial, nameFilter)
    return str(path) if path else ""


def _makeRegionDraftOverlay(controller, view):
    """A transparent paint layer for Graph's uncommitted region contour.

    Draft points intentionally never reach the Tonic model: a close is the
    first model edit, and therefore the one undo step.  Hydra cannot draw
    state it does not own, so this small Qt child paints only the temporary
    orange CVs, their connected contour and the last-CV rubber band.  It
    projects the stored world points on each paint, which keeps the draft
    pinned to the scalp across a camera move or viewport resize.
    """
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

    class RegionDraftOverlay(QtWidgets.QWidget):
        def __init__(self, parent):
            super(RegionDraftOverlay, self).__init__(parent)
            self.setAttribute(
                QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            self.setAttribute(QtCore.Qt.WidgetAttribute.WA_NoSystemBackground,
                              True)
            self.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
            self.setGeometry(parent.rect())
            self.hide()

        @staticmethod
        def _logicalPoint(camera, point, ratio):
            projected = camera.worldToPixels(point)
            if projected is None:
                return None
            return QtCore.QPointF(projected[0] / ratio, projected[1] / ratio)

        def paintEvent(self, _event):
            preview = controller.regionDraftPreview()
            points = preview["points"]
            if not points:
                return
            camera = tonicCamera.resolve(view)
            if camera is None:
                return
            try:
                ratio = max(float(view.devicePixelRatioF()), 1.0)
            except AttributeError:
                ratio = 1.0
            projected = [self._logicalPoint(camera, point, ratio)
                         for point in points]
            projected = [point for point in projected if point is not None]
            if not projected:
                return
            hover = preview["hover"]
            hoverPoint = (self._logicalPoint(camera, hover, ratio)
                          if hover is not None else None)
            painter = QtGui.QPainter(self)
            try:
                painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing,
                                      True)
                orange = QtGui.QColor(255, 145, 0, 230)
                line = QtGui.QPen(orange)
                line.setWidthF(2.0)
                painter.setPen(line)
                for a, b in zip(projected, projected[1:]):
                    painter.drawLine(a, b)
                if hoverPoint is not None:
                    rubber = QtGui.QPen(orange)
                    rubber.setWidthF(1.5)
                    rubber.setStyle(QtCore.Qt.PenStyle.DashLine)
                    painter.setPen(rubber)
                    painter.drawLine(projected[-1], hoverPoint)
                # The first CV has a ring so its role as the close target is
                # legible before the contour gains its third point.
                firstPen = QtGui.QPen(QtGui.QColor(255, 215, 120, 250))
                firstPen.setWidthF(2.0)
                painter.setPen(firstPen)
                painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
                painter.drawEllipse(projected[0], 6.5, 6.5)
                painter.setPen(QtGui.QPen(orange))
                painter.setBrush(QtGui.QBrush(orange))
                for point in projected:
                    painter.drawEllipse(point, 3.5, 3.5)
            finally:
                painter.end()

    return RegionDraftOverlay(view)


def _makeMarqueeOverlay(view):
    """A visible, mouse-transparent selection rectangle over StageView."""
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    shape = getattr(QtWidgets.QRubberBand, "Rectangle", None)
    if shape is None:  # PySide6 scopes the enum; PySide2 did not.
        shape = QtWidgets.QRubberBand.Shape.Rectangle
    band = QtWidgets.QRubberBand(shape, view)
    band.setAttribute(QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents,
                      True)
    band.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
    band.hide()
    return band


def _makeLassoOverlay(controller, view):
    """A transparent painted lasso for Tube component selection."""
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

    class LassoOverlay(QtWidgets.QWidget):
        def __init__(self, parent):
            super(LassoOverlay, self).__init__(parent)
            self.setAttribute(QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents,
                              True)
            self.setAttribute(QtCore.Qt.WidgetAttribute.WA_NoSystemBackground,
                              True)
            self.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
            self.setGeometry(parent.rect())
            self.hide()

        def paintEvent(self, _event):
            points = controller.selectionLassoPoints()
            if len(points) < 2:
                return
            try:
                ratio = max(float(view.devicePixelRatioF()), 1.0)
            except AttributeError:
                ratio = 1.0
            polygon = QtGui.QPolygonF([
                QtCore.QPointF(point[0] / ratio, point[1] / ratio)
                for point in points])
            painter = QtGui.QPainter(self)
            try:
                painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing,
                                      True)
                pen = QtGui.QPen(QtGui.QColor(90, 190, 255, 235))
                pen.setWidthF(1.5)
                pen.setStyle(QtCore.Qt.PenStyle.DashLine)
                painter.setPen(pen)
                painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
                painter.drawPolyline(polygon)
            finally:
                painter.end()

    return LassoOverlay(view)


def _makeGizmoOverlay(controller, view):
    """Draw Tube's screen-space gizmo above Storm without owning input.

    Adapted from usdRig's gizmo UI pattern.  StageView continues to receive
    every mouse event; TubeLoop alone resolves a handle and performs a drag.
    GizmoState supplies physical-pixel geometry shared with its picker, and
    this child converts it to Qt logical pixels exactly once at paint time.
    """
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

    class GizmoOverlay(QtWidgets.QWidget):
        def __init__(self, parent):
            super(GizmoOverlay, self).__init__(parent)
            self.setAttribute(
                QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            self.setAttribute(QtCore.Qt.WidgetAttribute.WA_NoSystemBackground,
                              True)
            self.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
            self.setGeometry(parent.rect())
            self.hide()

        def paintEvent(self, _event):
            records = controller.gizmoScreenHandles()
            if not records:
                return
            try:
                ratio = max(float(view.devicePixelRatioF()), 1.0)
            except AttributeError:
                ratio = 1.0
            painter = QtGui.QPainter(self)
            try:
                painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing,
                                      True)
                for record in records:
                    color = record["color"]
                    alpha = 255 if record.get("grabbable", False) else 95
                    pen = QtGui.QPen(QtGui.QColor(
                        round(255.0 * color[0]), round(255.0 * color[1]),
                        round(255.0 * color[2]), alpha))
                    pen.setWidthF(2.0)
                    painter.setPen(pen)
                    painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
                    points = [QtCore.QPointF(point[0] / ratio,
                                             point[1] / ratio)
                              for point in record["points"]]
                    kind = record["kind"]
                    if kind == "center":
                        side = (tonicGizmo.CENTER_SIDE * 90.0) / ratio
                        point = points[0]
                        painter.setBrush(QtGui.QBrush(pen.color()))
                        painter.drawRect(QtCore.QRectF(point.x() - side * 0.5,
                                                       point.y() - side * 0.5,
                                                       side, side))
                    elif kind == "free":
                        point = points[0]
                        radius = float(record.get("radiusPx", 0.0)) / ratio
                        painter.drawEllipse(point, radius, radius)
                    elif kind == "plane":
                        painter.drawPolygon(QtGui.QPolygonF(points))
                    else:
                        painter.drawPolyline(QtGui.QPolygonF(points))
            finally:
                painter.end()

    return GizmoOverlay(view)


class ViewportController:
    """One event filter on the view, one on the app, one idle timer."""

    def __init__(self, state, session, usdviewApi, container=None):
        self._state = state
        self._session = session
        self._api = usdviewApi
        self._container = container
        self._view = None
        self._viewFilter = None
        self._keyFilter = None
        self._trackingWas = None
        self._timer = None
        self._loop = None
        self._camera = None
        self._gesture = False
        self._brushResizeArmed = False
        self._brushResizeActive = False
        self._lastXY = None
        self._lastHoverXY = None
        self._pointerInside = False
        self._installed = False
        # A region contour has no model representation until it closes, so
        # it needs a viewport-only painting layer.  It is mouse-transparent:
        # the StageView still receives every press/move/release.
        self._regionOverlay = None
        self._marqueeOverlay = None
        self._lassoOverlay = None
        self._gizmoOverlay = None
        self._rolloverWas = None
        # The fallback ladder (plan/18 section 3.7): armed at press, fed
        # every move's measured time, restored at release.
        self._ladder = tonicLadder.FallbackLadder(session, state)

    # -- accessors ---------------------------------------------------------

    @property
    def loop(self):
        return self._loop

    @property
    def view(self):
        return self._view

    @property
    def camera(self):
        return self._camera

    @property
    def installed(self):
        return self._installed

    @property
    def gestureActive(self):
        return self._gesture

    @property
    def ladder(self):
        return self._ladder

    @property
    def pointerInside(self):
        return self._pointerInside

    def _status(self, text):
        if not text:
            return
        api = self._api
        if api is None:
            return
        try:
            api.PrintStatus(text)
        except AttributeError:
            pass

    def _refresh(self):
        api = self._api
        if api is None:
            return
        try:
            api.UpdateViewport()
        except AttributeError:
            pass

    # -- install / uninstall -----------------------------------------------

    def install(self, view=None):
        """Attach both filters. True once the view is really there."""
        from pxr.Usdviewq.qt import QtCore, QtWidgets
        view = view if view is not None else stageViewOf(self._api)
        if view is None:
            self._state.viewportFailed = True
            return False
        if self._installed and self._view is view:
            return True
        self.uninstall()
        self._view = view
        # Hover highlight needs press-less moves, which Qt only delivers
        # to a tracking widget; usdview leaves it off, so the tool takes
        # it while installed and gives it back on uninstall.
        try:
            self._trackingWas = bool(view.hasMouseTracking())
            view.setMouseTracking(True)
        except AttributeError:
            self._trackingWas = None
        self._viewFilter = _ViewFilter(self, view)
        view.installEventFilter(self._viewFilter)
        self._regionOverlay = _makeRegionDraftOverlay(self, view)
        self._marqueeOverlay = _makeMarqueeOverlay(view)
        self._lassoOverlay = _makeLassoOverlay(self, view)
        self._gizmoOverlay = _makeGizmoOverlay(self, view)
        # GizmoState.push clears the Hydra record while this child exists.
        # The dynamic session flag deliberately keeps headless/fallback
        # routes free of any Qt dependency.
        self._session.qtGizmoOverlay = True
        gizmo = getattr(self._loop, "_gizmo", None)
        if gizmo is not None:
            gizmo.push(self._session)  # clear a pre-existing Hydra gizmo
        self._keyFilter = _KeyFilter(self)
        application = QtWidgets.QApplication.instance()
        if application is not None:
            # Installed AFTER usdview's AppEventFilter, so Qt runs ours
            # first and Escape is ours to claim.
            application.installEventFilter(self._keyFilter)
        self._timer = QtCore.QTimer(view)
        self._timer.setInterval(PUMP_INTERVAL_MS)
        self._timer.timeout.connect(self._onPump)
        # Register only after the timer exists: dock actions can enqueue a
        # commit without a gesture, and the session must be able to wake this
        # idle pump.  The callback itself remains Qt-free in TonicSession.
        setIdleHook = getattr(self._session, "setIdleHook", None)
        if callable(setIdleHook):
            setIdleHook(self.scheduleIdle)
        self._installed = True
        self._state.viewportFailed = False
        # workspaceOpen belongs to the dock, which sets it from its own
        # visibility (plan/18 section 3.4: the hotkeys live while the
        # workspace is open, not merely while the filter is installed).
        self._connectStageSignals()
        self._connectFrustumSignal()
        self.setWorkspaceActive(bool(self._state.workspaceOpen))
        self.syncDisplayScale()
        if not self._state.activeMode:
            self.setMode(tonicModes.MODES[0].id)
        if getattr(self._session, "hasPendingWork", lambda: False)():
            self.scheduleIdle()
        return True

    def uninstall(self):
        from pxr.Usdviewq.qt import QtWidgets
        if self._session is not None:
            setIdleHook = getattr(self._session, "setIdleHook", None)
            if callable(setIdleHook):
                setIdleHook(None)
        # A hidden/replaced StageView may never deliver its matching release.
        # Restore the loop before dropping the filters that would otherwise
        # be the only route back into it.
        self._recoverGesture("viewport uninstall")
        if self._timer is not None:
            self._timer.stop()
            self._timer = None
        if self._view is not None and self._viewFilter is not None:
            self._view.removeEventFilter(self._viewFilter)
        self.setWorkspaceActive(False)
        if self._view is not None and self._trackingWas is not None:
            try:
                self._view.setMouseTracking(self._trackingWas)
            except AttributeError:
                pass
        self._trackingWas = None
        if self._keyFilter is not None:
            application = QtWidgets.QApplication.instance()
            if application is not None:
                application.removeEventFilter(self._keyFilter)
        self._viewFilter = None
        self._keyFilter = None
        if self._regionOverlay is not None:
            self._regionOverlay.hide()
            self._regionOverlay.deleteLater()
            self._regionOverlay = None
        if self._marqueeOverlay is not None:
            self._marqueeOverlay.hide()
            self._marqueeOverlay.deleteLater()
            self._marqueeOverlay = None
        if self._lassoOverlay is not None:
            self._lassoOverlay.hide()
            self._lassoOverlay.deleteLater()
            self._lassoOverlay = None
        if self._gizmoOverlay is not None:
            self._gizmoOverlay.hide()
            self._gizmoOverlay.deleteLater()
            self._gizmoOverlay = None
        if self._session is not None:
            self._session.qtGizmoOverlay = False
            gizmo = getattr(self._loop, "_gizmo", None)
            if gizmo is not None:
                gizmo.push(self._session)  # restore the non-Qt fallback
        self._view = None
        self._installed = False

    def setWorkspaceActive(self, active):
        """Suppress usdview's rollover popup only while Tonic is visible."""
        if not active:
            self._brushResizeArmed = False
            if self._brushResizeActive:
                self._recoverGesture("workspace closed", force=True)
        view = self._view
        if view is None:
            return
        if active:
            if self._rolloverWas is None:
                try:
                    self._rolloverWas = bool(view.rolloverPicking)
                except (AttributeError, RuntimeError):
                    return
            try:
                # StageView's setter also turns mouse tracking off. Restore
                # it for Tonic hover and the selection rubber band.
                view.rolloverPicking = False
                view.setMouseTracking(True)
            except (AttributeError, RuntimeError):
                return
            try:
                from pxr.Usdviewq.qt import QtWidgets
                QtWidgets.QToolTip.hideText()
            except (AttributeError, RuntimeError):
                pass
            return
        if self._rolloverWas is not None:
            try:
                view.rolloverPicking = self._rolloverWas
            except (AttributeError, RuntimeError):
                pass
        self._rolloverWas = None

    def _connectStageSignals(self):
        api = self._api
        if api is None:
            return
        try:
            api.dataModel.signalStageReplaced.connect(self._onStageReplaced)
        except (AttributeError, RuntimeError):
            pass

    def _connectFrustumSignal(self):
        """Re-measure the display scale whenever the view moves.

        The overlay dots and curves are a fixed pixel size, which Storm can
        only deliver as `pixels * world-units-per-pixel` in world widths
        (plan/18 section 2.4a). Every orbit, dolly and window resize
        changes that number, and StageView announces all three with one
        signal.
        """
        view = self._view
        if view is None:
            return
        try:
            view.signalFrustumChanged.connect(self.syncDisplayScale)
        except (AttributeError, RuntimeError):
            pass

    def syncDisplayScale(self, camera=None):
        """Push world-units-per-pixel at the groom into the model.

        True when the model took a new value. Cheap enough to call on
        every publish: it is one 4x4 inverse and two unprojections, and
        the model ignores a value it already has, so an unchanged camera
        dirties nothing.
        """
        session = self._session
        if session is None or session.model is None:
            return False
        centre = session.scalpCenter
        if centre is None:
            return False
        if camera is None:
            camera = tonicCamera.resolve(self._view)
        if camera is None:
            return False
        perPixel = camera.worldPerPixel(centre)
        if not perPixel > 0.0:
            return False
        changed = session.setDisplayScale(perPixel)
        refresh = getattr(self._loop, "refreshGizmo", None)
        if refresh is not None:
            refresh(camera)
        self._syncRegionOverlay()
        self._syncGizmoOverlay()
        return changed

    def _onStageReplaced(self):
        """Plan/17 section 3.4: the model survives, the layers do not."""
        session = self._session
        if session is None:
            return
        session.detach()
        session.reattach()
        self._refresh()

    # -- modes -------------------------------------------------------------

    def setMode(self, modeId):
        """Switch modes: cancel any gesture, build the mode's loop."""
        mode = tonicModes.ModeById(str(modeId))
        if mode is None:
            return ""
        if mode.id != "sculpt":
            self._brushResizeArmed = False
        # A first hover at the same physical point in the new mode is still
        # meaningful (notably Reposition's CV/edge affordance), so do not
        # inherit the prior loop's motion throttle.
        if self._gesture:
            self._recoverGesture("mode switch")
        self._clearHover()
        if self._loop is not None and mode.id != self._state.activeMode:
            # The outgoing loop takes its overlays with it: a gizmo or a
            # brush ring is model state and would otherwise still be drawn
            # in a mode that cannot drag it (plan/18 section 2.4).
            self._loop.deactivate()
        status = tonicModes.SetActiveMode(self._state, mode.id)
        # Active-cut visibility is an interactive-workspace policy, not a
        # Hierarchy-only command.  Entering through Graph/Tube/Sculpt must
        # start on the same root frontier; headless/legacy callers that
        # never create this controller keep the native default (disabled).
        if (mode.id not in tonicLoops.PANEL_ONLY_MODES and
                self._session is not None and self._session.model is not None and
                tonicHierarchy.supportsActiveCut(self._session.dll)):
            self._enableActiveCutForWorkspace()
        # A mode with no loop is a panel mode (tonicLoops.PANEL_ONLY_MODES):
        # the shelf switches, the dock shows that mode's parameters and the
        # viewport keeps the camera, so the mode's own status line is the
        # whole truth about it.
        self._loop = tonicLoops.makeLoop(mode.id, self._session, self._state)
        if self._loop is not None and not self._subModeOf(mode.id):
            self.setSubMode(self._loop.defaultSubMode)
        if self._loop is not None:
            # Some modes need to publish an opt-in display/navigation policy
            # as soon as they become active.  It is intentionally a loop
            # hook, rather than a controller-wide mode special case.
            self._loop.activate()
        self._applyDisplayPolicy()
        self._syncRegionOverlay()
        self._syncMarqueeOverlay()
        self._syncGizmoOverlay()
        self._refresh()
        self._status(status)
        return status

    def _enableActiveCutForWorkspace(self):
        """Enable the active cut once and retire only hidden selections."""
        session = self._session
        try:
            wasEnabled = tonicHierarchy.getActiveCutEnabled(session.dll,
                                                             session.model)
            if wasEnabled:
                self._state.activeCutEnabled = True
                return False
            tonicHierarchy.setActiveCutEnabled(session.dll, session.model,
                                                True)
        except RuntimeError as exc:
            self._status("Tonic active cut: %s" % exc)
            return False
        self._state.activeCutEnabled = True
        # The transition can hide an old child/component immediately.  Keep
        # valid visible owners and all guide selection records; graph picks
        # are unrelated to the tube frontier and are left alone.
        kinds = (tonicLib.TONIC_PICK_TUBE_VERT,
                 tonicLib.TONIC_PICK_CENTER_CV,
                 tonicLib.TONIC_PICK_SECTION_RING,
                 tonicLib.TONIC_PICK_SECTION_CV,
                 tonicLib.TONIC_PICK_GUIDE)
        kept = {}
        for kind in kinds:
            records = session.readSelection(kind)
            if kind == tonicLib.TONIC_PICK_GUIDE:
                kept[kind] = records
                continue
            visible = []
            for record in records:
                try:
                    if tonicHierarchy.isTubeVisible(session.dll,
                                                     session.model,
                                                     int(record[0])):
                        visible.append(record)
                except RuntimeError:
                    pass
            kept[kind] = visible
        session.clearSelection(sum(kinds))
        for kind, records in kept.items():
            if records:
                session.select(kind, [record[0] for record in records],
                               [record[1] for record in records],
                               [record[2] for record in records])
        session.setHover(0, -1, -1, -1)
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    def _syncActiveCutBreadcrumb(self):
        """Let any visible tube/component selection update branch context."""
        session = self._session
        if (session is None or session.model is None or
                not bool(getattr(self._state, "activeCutEnabled", False)) or
                not tonicHierarchy.supportsActiveCut(session.dll)):
            return False
        owner = None
        # The active Tube component mode says which selection is current;
        # otherwise a whole-tube click must beat a stale component record
        # retained by a preceding edit mode.
        componentKind = {
            "center": tonicLib.TONIC_PICK_CENTER_CV,
            "ring": tonicLib.TONIC_PICK_SECTION_RING,
            "section": tonicLib.TONIC_PICK_SECTION_CV,
        }.get(getattr(self._state, "tubeSelectionKind", "")) \
            if self._state.activeMode == "tube" else None
        kinds = ((componentKind, tonicLib.TONIC_PICK_TUBE_VERT)
                 if componentKind is not None else
                 (tonicLib.TONIC_PICK_TUBE_VERT,
                  tonicLib.TONIC_PICK_CENTER_CV,
                  tonicLib.TONIC_PICK_SECTION_RING,
                  tonicLib.TONIC_PICK_SECTION_CV))
        for kind in kinds:
            records = session.readSelection(kind)
            if records:
                owner = int(records[0][0])
                break
        if owner is None:
            return False
        try:
            previousLevel = int(self._state.activeLevel)
            level = tonicHierarchy.tubeLevel(session.dll, session.model,
                                              owner)
            parent = tonicHierarchy.tubeParent(session.dll, session.model,
                                                owner)
            if (parent is not None and tonicHierarchy.getTubeExpanded(
                    session.dll, session.model, parent)):
                focusParent = int(parent)
                path = tonicHierarchy.tubeAncestorPath(
                    session.dll, session.model, focusParent)
            else:
                focusParent = -1
                path = tonicHierarchy.tubeAncestorPath(
                    session.dll, session.model, owner)
            tonicHierarchy.setActiveCutFocus(
                self._state, focusParent, path,
                names=tuple("T%d" % tubeId for tubeId in path),
                level=level)
            if int(self._state.activeLevel) != previousLevel:
                self._applyDisplayPolicy()
            return True
        except (RuntimeError, NotImplementedError):
            return False

    def _applyDisplayPolicy(self):
        """Push the plan/18 section 2.4a display policy for the live mode.

        THE table is in C++ (usdGenTonic::TonicPolicyLevelDisplay) and this
        is its only caller: one call sets every level's visibility, x-ray
        strength and centers flag, the ring display and the focus level.
        Python decides nothing about the look — it says which mode,
        sub-mode and level the artist is in, which is all it knows.

        Called on every mode change, sub-mode change and level change,
        because those are exactly the three inputs the table takes.
        """
        session = self._session
        if session is None or session.model is None:
            return
        entry = getattr(session.dll, "Tonic_SetDisplayPolicy", None)
        if entry is None:
            return
        mode = self._state.activeMode or ""
        if not mode:
            return
        sub = self._subModeOf(mode)
        entry(session.model, mode.encode("utf-8"),
              str(sub or "").encode("utf-8"),
              int(self._state.activeLevel))
        session.publish(tonicLib.TONIC_DIRTY_DISPLAY)

    def _subModeOf(self, modeId):
        return getattr(self._state, "%sSubMode" % modeId, "")

    def setSubMode(self, subId):
        """Switch the active mode's sub-mode; "" when there is no such one."""
        modeId = self._state.activeMode
        if not modeId:
            return ""
        # Re-evaluate immediately even when the pointer did not move: the
        # target classes and their accessibility radii can change by shelf.
        if self._gesture:
            self._recoverGesture("sub-mode switch")
        self._clearHover()
        # The live loop owns transient state (notably Graph's uncommitted
        # region contour), so it must see a sub-mode transition before the
        # state changes.  The old direct setter left that draft visible and
        # finishable after switching tools.
        if (self._loop is not None and
                getattr(self._loop, "modeId", "") == modeId):
            status = self._loop.setSubMode(subId)
        else:
            status = tonicLoops.setSubModeOn(self._state, modeId, subId)
        self._applyDisplayPolicy()
        self._syncRegionOverlay()
        self._syncMarqueeOverlay()
        self._syncGizmoOverlay()
        self._refresh()
        self._status(status)
        return status

    def setSelectionKind(self, kind):
        """Choose Whole Tube/Center/Ring/Section from the F8--F11 row."""
        kind = str(kind).lower()
        if kind not in ("tube", "center", "ring", "section"):
            return ""
        self._state.tubeSelectionKind = kind
        if self._state.activeMode != "tube":
            return ""
        return self.setSubMode(kind)

    def _subModeForLetter(self, letter):
        for mode in tonicLoops.subModesFor(self._state.activeMode):
            if mode.hotkey.upper() == str(letter).upper():
                return mode.id
        return ""

    # -- the gesture contract ----------------------------------------------

    def _ready(self):
        return (self._session is not None and
                self._session.model is not None and
                self._loop is not None)

    def onPress(self, view, event):
        from pxr.Usdviewq.qt import QtCore
        if not self._ready():
            return False
        if event.button() != QtCore.Qt.MouseButton.LeftButton:
            return False
        # A missing MouseButtonRelease (focus change, native modal dialog,
        # or a view replacement) must never consume the next real click.
        if self._gesture:
            self._recoverGesture("new press after lost release")
        modifiers = modifierSet(event)
        if "alt" in modifiers or "meta" in modifiers:
            return False                 # the camera's, always
        x, y = eventPixels(view, event)
        self._camera = tonicCamera.resolve(view)
        if self._camera is None:
            return False
        self.syncDisplayScale(self._camera)
        sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                   modifiers)
        if self._brushResizeArmed and self._canBrushResize():
            begin = getattr(self._loop, "beginRadiusResize", None)
            if begin is not None and begin(sample):
                self._brushResizeActive = True
                self._gesture = True
                self._lastXY = (x, y)
                self._syncRegionOverlay()
                self._syncMarqueeOverlay()
                self._syncGizmoOverlay()
                self._refresh()
                return True
        try:
            claimed = bool(self._loop.press(sample))
        except Exception as exc:          # noqa: BLE001 - GUI boundary
            # A loop can open its native bracket immediately before a
            # validation error.  It is not yet marked as a controller
            # gesture, so force its cancel path here.
            self._status("Tonic viewport press: %s" % exc)
            self._recoverGesture("press exception", force=True)
            return False
        if not claimed:
            return False
        self._syncActiveCutBreadcrumb()
        self._gesture = True
        self._lastXY = (x, y)
        self._ladder.arm(self._state.activeLevel)
        self._session.publish()
        self._syncRegionOverlay()
        self._syncMarqueeOverlay()
        self._syncGizmoOverlay()
        self._refresh()
        return True

    def onMove(self, view, event):
        x, y = eventPixels(view, event)
        self._pointerInside = True
        if not self._ready():
            return False
        if not self._gesture:
            return self._onHover(view, event, x, y)
        if not self._leftButtonHeld(event):
            # Qt keeps sending hover moves after a lost capture.  Treat the
            # first one with no left button as the missing release, then let
            # it reacquire an idle hover target instead of wedging the tool.
            self._recoverGesture("move without left button")
            return self._onHover(view, event, x, y)
        if self._lastXY is not None:
            if (abs(x - self._lastXY[0]) + abs(y - self._lastXY[1]) <
                    MOVE_THRESHOLD_PX):
                return True
        self._lastXY = (x, y)
        started = time.perf_counter()
        sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                   modifierSet(event))
        if self._brushResizeActive:
            resize = getattr(self._loop, "resizeRadius", None)
            claimed = bool(resize(sample)) if resize is not None else False
        else:
            claimed = bool(self._loop.move(sample))
        self._session.publish()
        self._syncRegionOverlay()
        self._syncMarqueeOverlay()
        self._syncGizmoOverlay()
        self._refresh()
        # The move is timed end to end -- the loop's work, the publish and
        # the viewport refresh request -- because that is the number TN-1
        # budgets and the number the ladder steps on (plan/18 section 3.7).
        elapsed = (time.perf_counter() - started) * 1000.0
        self._state.lastMoveMs = elapsed
        if not self._brushResizeActive and self._ladder.noteMove(elapsed):
            self._status("Tonic: %s" % self._ladder.describe())
        return claimed

    def _onHover(self, view, event, x, y):
        if self._ladder.hoverSuppressed:
            return False             # the ladder's last rung
        # Component dots in Tube and Hierarchy are as precise as Graph's
        # CV/edge affordances.  Do not make a one- or two-pixel crossing
        # wait for the old generic rollover threshold.
        immediateHover = getattr(self._loop, "modeId", "") in (
            "graph", "tube", "hierarchy")
        if not immediateHover and self._lastHoverXY is not None:
            if (abs(x - self._lastHoverXY[0]) +
                    abs(y - self._lastHoverXY[1]) < HOVER_THRESHOLD_PX):
                return False
        self._lastHoverXY = (x, y)
        camera = tonicCamera.resolve(view)
        if camera is None:
            return False
        sample = tonicLoops.Sample(self._session, camera, x, y,
                                   modifierSet(event))
        # Hover never claims the event: usdview's own rollover and the
        # camera modes keep working over the same pixels.
        self._loop.hover(sample)
        self._syncRegionOverlay()
        self._syncGizmoOverlay()
        self._refresh()
        return False

    def onRelease(self, view, event):
        from pxr.Usdviewq.qt import QtCore
        if not self._gesture or not self._ready():
            return False
        if event.button() not in (QtCore.Qt.MouseButton.LeftButton,
                                  QtCore.Qt.MouseButton.NoButton):
            return False
        x, y = eventPixels(view, event)
        claimed = False
        released = False
        resizing = self._brushResizeActive
        try:
            sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                       modifierSet(event))
            if self._brushResizeActive:
                end = getattr(self._loop, "endRadiusResize", None)
                claimed = bool(end(sample)) if end is not None else False
            else:
                claimed = bool(self._loop.release(sample))
            released = True
            return claimed
        except Exception as exc:          # noqa: BLE001 - GUI boundary
            self._status("Tonic viewport release: %s" % exc)
            # A failed release must restore the loop's press-time snapshot,
            # not leave an open native bracket behind.
            self._recoverGesture("release exception", force=True)
            return False
        finally:
            if released:
                # Controller state is reset even if an overlay/publish hook
                # subsequently fails.  The next click can always recover.
                self._resetGestureState()
                self._syncActiveCutBreadcrumb()
                self._session.publish()
                self._syncRegionOverlay()
                self._syncMarqueeOverlay()
                self._syncGizmoOverlay()
                self._refresh()
                if resizing:
                    self.refreshWorkspace()
                self.scheduleIdle()

    def onDoubleClick(self, view, event):
        """Let Hierarchy navigate a valid target; otherwise preserve clicks."""
        from pxr.Usdviewq.qt import QtCore
        if not self._ready():
            return False
        if event.button() != QtCore.Qt.MouseButton.LeftButton:
            return False
        if self._gesture:
            self._recoverGesture("double click after lost release")
        modifiers = modifierSet(event)
        if "alt" in modifiers or "meta" in modifiers:
            return False
        # Hierarchy alone owns double-click navigation, and only when its
        # target resolver accepted a visible tube (plan/18 section 3.6).
        handler = getattr(self._loop, "doubleClick", None)
        if handler is not None:
            x, y = eventPixels(view, event)
            self._camera = tonicCamera.resolve(view)
            sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                       modifiers)
            if handler(sample):
                self._refresh()
                return True
        # Qt delivers MouseButtonDblClick in place of the second press.
        # It is hierarchy navigation only when that loop accepted a visible
        # tube target.  Tube, Graph and Sculpt must see the ordinary second
        # press/release instead; blindly entering a level here quickly puts
        # them on an empty level where nothing can be picked.
        return self.onPress(view, event)

    def setPointerInside(self, inside):
        inside = bool(inside)
        if self._pointerInside and not inside:
            # Region CV and Reposition CV/edge prehighlights are only
            # meaningful over the viewport.  Keep a live drag's press target
            # intact, but clear an idle target and make re-entry evaluate
            # even at the same pixel.
            self._lastHoverXY = None
            cleared = False
            if not self._gesture:
                cleared = self._clearHover()
            if (not self._gesture and
                    getattr(self._loop, "modeId", "") == "graph" and
                    self._loop.subMode() in ("region", "reposition")):
                if self._loop.subMode() == "region":
                    # Retain the authored draft anchors, but remove its
                    # cursor-driven endpoint while the pointer is outside.
                    self._loop._regionHover = None
                    self._syncRegionOverlay()
                self._loop._setGraphHover(None)
                self._refresh()
            elif cleared:
                self._syncRegionOverlay()
                self._syncGizmoOverlay()
                self._refresh()
        self._pointerInside = inside

    def cancelGesture(self):
        """Escape: drop the live gesture, restoring the press-time base."""
        # Graph can retain an idle transient region draft after the click
        # release. Escape must still reach that loop even though the
        # controller no longer has a captured mouse gesture.
        return self._recoverGesture("cancelled", force=True,
                                    clearSelection=True)

    def _leftButtonHeld(self, event):
        """False only when a real Qt move reports that capture was lost."""
        try:
            from pxr.Usdviewq.qt import QtCore
            buttons = event.buttons()
        except (AttributeError, RuntimeError):
            return True                # lightweight/headless event
        if buttons is None:
            return True
        return bool(buttons & QtCore.Qt.MouseButton.LeftButton)

    def _resetGestureState(self):
        """Clear controller-only capture state; safe after every exit path."""
        self._gesture = False
        self._brushResizeActive = False
        self._lastXY = None
        self._ladder.restore()

    def _recoverGesture(self, reason="", force=False, clearSelection=False):
        """Cancel an interrupted gesture and leave the controller reusable.

        `force` covers a loop exception during press: its own bracket may be
        open before the controller has set `_gesture` true.  Normal mouse
        leave intentionally does not call this helper because Qt commonly
        keeps a valid left-button capture outside the widget.
        """
        if self._loop is None:
            self._resetGestureState()
            return False
        if not self._gesture and not force:
            if self._brushResizeArmed:
                self._brushResizeArmed = False
                return True
            return False
        cancelled = False
        try:
            if self._brushResizeActive:
                cancelResize = getattr(self._loop, "cancelRadiusResize", None)
                cancelled = bool(cancelResize()) if cancelResize is not None \
                    else False
            else:
                cancelled = bool(self._loop.cancel())
        except Exception as exc:        # noqa: BLE001 - recovery boundary
            self._status("Tonic viewport recovery: %s" % exc)
        finally:
            self._resetGestureState()
            self._brushResizeArmed = False
        if not cancelled and clearSelection and self._session is not None:
            if self._session.selectionCount(0) > 0:
                self._session.clearSelection(0)
                self._session.publish(tonicLib.TONIC_DIRTY_SELECTION)
                cancelled = True
        # The loop's cancel normally publishes its model dirty mask, but the
        # controller owns the overlays and must repaint them after every
        # capture loss, including a no-op loop cancel.
        if self._session is not None:
            self._session.publish()
        self._syncRegionOverlay()
        self._syncMarqueeOverlay()
        self._syncGizmoOverlay()
        self._refresh()
        self.scheduleIdle()
        return cancelled

    def _clearHover(self):
        """Drop an idle prehighlight when its viewport context changes."""
        self._lastHoverXY = None
        if self._gesture or self._session is None:
            return False
        changed = False
        graphClear = getattr(self._loop, "_setGraphHover", None)
        try:
            if graphClear is not None:
                changed = bool(graphClear(None))
            else:
                changed = bool(self._session.setHover(0, -1, -1, -1))
        except (AttributeError, RuntimeError):
            return False
        if changed:
            self._session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return changed

    def regionDraftPreview(self):
        """The active GraphLoop's uncommitted contour, if it has one."""
        loop = self._loop
        getter = getattr(loop, "draftRegionPreview", None)
        if getter is None:
            return {"points": (), "hover": None}
        return getter()

    def _syncRegionOverlay(self):
        """Repaint and resize the mouse-transparent draft overlay."""
        overlay = self._regionOverlay
        view = self._view
        if overlay is None or view is None:
            return
        overlay.setGeometry(view.rect())
        overlay.setVisible(bool(self.regionDraftPreview()["points"]))
        overlay.raise_()
        overlay.update()

    def _syncMarqueeOverlay(self):
        """Mirror the active loop's physical-pixel selection band in Qt."""
        overlay = self._marqueeOverlay
        view = self._view
        loop = self._loop
        if overlay is None:
            return
        self._syncLassoOverlay()
        if self.selectionLassoPoints():
            overlay.hide()
            return
        if view is None or loop is None:
            overlay.hide()
            return
        origin = loop.marqueeRect()
        end = self._lastXY
        if origin is None or end is None:
            overlay.hide()
            return
        try:
            ratio = max(float(view.devicePixelRatioF()), 1.0)
        except AttributeError:
            ratio = 1.0
        from pxr.Usdviewq.qt import QtCore
        start = QtCore.QPoint(round(origin[0] / ratio),
                              round(origin[1] / ratio))
        finish = QtCore.QPoint(round(end[0] / ratio), round(end[1] / ratio))
        rect = QtCore.QRect(start, finish).normalized()
        if rect.width() < 2 and rect.height() < 2:
            overlay.hide()
            return
        overlay.setGeometry(rect)
        overlay.show()
        overlay.raise_()

    def selectionLassoPoints(self):
        loop = self._loop
        entry = getattr(loop, "lassoPoints", None) if loop is not None else None
        return tuple(entry()) if callable(entry) else ()

    def _syncLassoOverlay(self):
        overlay = self._lassoOverlay
        view = self._view
        if overlay is None:
            return
        points = self.selectionLassoPoints()
        if view is None or len(points) < 2:
            overlay.hide()
            return
        overlay.setGeometry(view.rect())
        overlay.show()
        overlay.raise_()
        overlay.update()

    def gizmoScreenHandles(self):
        """Physical-pixel handles from the active Tube loop, if any."""
        loop = self._loop
        gizmo = getattr(loop, "_gizmo", None) if loop is not None else None
        view = self._view
        if gizmo is None or view is None:
            return ()
        camera = tonicCamera.resolve(view)
        return tuple(gizmo.screenHandles(camera)) if camera is not None else ()

    def _syncGizmoOverlay(self):
        """Keep the unoccluded Qt gizmo in lockstep with the live picker."""
        overlay = self._gizmoOverlay
        view = self._view
        if overlay is None:
            return
        records = self.gizmoScreenHandles()
        if view is None or not records:
            overlay.hide()
            return
        overlay.setGeometry(view.rect())
        overlay.show()
        overlay.raise_()
        overlay.update()

    # -- the idle pump (plan/18 section 3.3) -------------------------------

    def scheduleIdle(self):
        if self._timer is not None and not self._timer.isActive():
            self._timer.start()

    def stopIdle(self):
        if self._timer is not None and self._timer.isActive():
            self._timer.stop()

    def _onPump(self):
        if self._gesture or self._session is None:
            return
        self.syncDisplayScale()
        report = self._session.pump()
        if report["swapped"] or report["baked"]:
            self._refresh()
        self.refreshWorkspace()
        if not report["pending"]:
            self.stopIdle()

    def pumpOnce(self):
        """One pump tick, for a test or a dock button."""
        if self._gesture or self._session is None:
            return None
        self.syncDisplayScale()
        report = self._session.pump()
        self.refreshWorkspace()
        return report

    def refreshWorkspace(self):
        """Re-read the dock, when there is one and it is on screen."""
        container = self._container
        if container is None:
            return
        refresh = getattr(getattr(container, "workspace", None), "refresh",
                          None)
        if refresh is None:
            return
        refresh()

    # -- keys --------------------------------------------------------------

    def _canBrushResize(self):
        return (not self._gesture and self._installed and
                self._state.workspaceOpen and
                self._pointerInside and not self._textFocus() and
                self._state.activeMode == "sculpt" and
                self._loop is not None and
                getattr(self._loop, "modeId", "") == "sculpt")

    def _brushResizeShortcutCandidate(self):
        """F stays ours throughout a held Sculpt resize, including repeat."""
        return (self._installed and self._state.workspaceOpen and
                not self._textFocus() and self._state.activeMode == "sculpt" and
                self._loop is not None and
                getattr(self._loop, "modeId", "") == "sculpt" and
                (self._pointerInside or self._brushResizeArmed or
                 self._brushResizeActive))

    def onKey(self, event):
        """The plan/18 section 3.4 table, dispatched. True consumes."""
        if not self._installed or self._session is None:
            return False
        if not self._state.workspaceOpen:
            return False
        key = keyName(event)
        modifiers = modifierSet(event)
        if key == "f" and not modifiers:
            if self._brushResizeArmed or self._brushResizeActive:
                return True             # OS key-repeat while F is held
            if self._canBrushResize():
                self._brushResizeArmed = True
                return True
        if self._runTubeShortcut(key, modifiers):
            return True
        action = tonicModes.HotkeyAction(key, modifiers,
                                         self._pointerInside,
                                         self._textFocus())
        if action is None:
            return False
        return self.runAction(action[0], action[1])

    def onKeyRelease(self, event):
        """F is a held sculpt modifier, never a persistent usdview hotkey."""
        if keyName(event) != "f":
            return False
        consumed = bool(self._brushResizeArmed or self._brushResizeActive)
        repeating = getattr(event, "isAutoRepeat", None)
        if callable(repeating) and repeating():
            return consumed
        self._brushResizeArmed = False
        return consumed

    def _tubeShortcutCandidate(self, key, modifiers):
        """Whether a plain viewport key is reserved by the Tube workflow."""
        if (not self._pointerInside or self._textFocus() or modifiers or
                self._state.activeMode not in ("tube", "hierarchy")):
            return False
        if key in ("f8", "f9", "f10", "f11"):
            return self._state.activeMode == "tube"
        if self._state.activeMode == "tube":
            return key in ("q", "w", "e", "r")
        # Hierarchy W/E/R enters Tube object editing while retaining the
        # hierarchy's already selected whole-tube owners.
        return key in ("w", "e", "r")

    def _runTubeShortcut(self, key, modifiers):
        if not self._tubeShortcutCandidate(key, modifiers):
            return False
        component = {"f8": "tube", "f9": "center", "f10": "ring",
                     "f11": "section"}
        if key in component:
            self.setSelectionKind(component[key])
            return True
        if self._state.activeMode == "hierarchy":
            # Do not call TubeLoop.setSubMode here: it deliberately clears
            # incompatible component selections, while this route promises
            # to retain the hierarchy's whole-tube owners.
            tonicModes.SetActiveTubeSubMode(self._state, "tube")
            self.setMode("tube")
        loop = self._loop
        setTool = getattr(loop, "setTransformTool", None)
        tool = {"q": "select", "w": "move", "e": "rotate",
                "r": "scale"}.get(key)
        if setTool is None or tool is None or not setTool(tool):
            return False
        self._syncGizmoOverlay()
        self._refresh()
        return True

    @staticmethod
    def _textFocus():
        from pxr.Usdviewq.qt import QtWidgets
        widget = QtWidgets.QApplication.focusWidget()
        return isinstance(widget, (QtWidgets.QLineEdit, QtWidgets.QTextEdit,
                                   QtWidgets.QPlainTextEdit,
                                   QtWidgets.QAbstractSpinBox,
                                   QtWidgets.QComboBox))

    def runAction(self, action, argument=None):
        """Execute one hotkey action; True when it was ours."""
        modes = tonicModes
        if action == modes.ACTION_MODE:
            self.setMode(argument)
            return True
        if action == modes.ACTION_SUBMODE:
            subId = self._subModeForLetter(argument)
            if not subId:
                return False             # e.g. F, which usdview frames with
            self.setSubMode(subId)
            return True
        if action == modes.ACTION_CANCEL:
            return self.cancelGesture()
        if action == modes.ACTION_COMPLETE:
            complete = getattr(self._loop, "completeRegionDraft", None)
            if complete is None or not complete():
                return False
            self._syncRegionOverlay()
            self._refresh()
            self.scheduleIdle()
            return True
        if action == modes.ACTION_BACKSPACE:
            discard = getattr(self._loop, "discardRegionCV", None)
            if discard is not None and discard():
                self._syncRegionOverlay()
                self._refresh()
                return True
            # Backspace remains the hierarchy convenience key unless Graph
            # has a retained Region CV to remove.
            return self.exitLevel()
        if action == modes.ACTION_UNDO:
            self._session.undo()
            self._refresh()
            self.scheduleIdle()
            return True
        if action == modes.ACTION_REDO:
            self._session.redo()
            self._refresh()
            self.scheduleIdle()
            return True
        if action == modes.ACTION_DELETE:
            if self._loop is None or not self._loop.deleteSelection():
                return False
            self._session.publish()
            self._refresh()
            self.scheduleIdle()
            return True
        if action == modes.ACTION_RADIUS:
            return self._adjustRadius(float(argument))
        # The hierarchy keys belong to HierarchyLoop when it is the
        # active mode -- it knows the drawn split edge, the parent of a
        # selected child and the status line -- and to the controller's own
        # plain forms from any other mode (the keys are global, plan/18
        # section 3.4).
        if action == modes.ACTION_SUBDIVIDE:
            return self._loopOr("subdivideSelection", self.subdivideSelection)
        if action == modes.ACTION_MERGE:
            return self._loopOr("mergeChildrenOfSelection",
                                self.mergeChildrenOfSelection)
        if action == modes.ACTION_ENTER_LEVEL:
            return self._loopOr("enterLevel", self.enterLevel)
        if action == modes.ACTION_EXIT_LEVEL:
            return self._loopOr("exitLevel", self.exitLevel)
        if action == modes.ACTION_WELD:
            return self._graphAction("weldSelected")
        if action == modes.ACTION_UNWELD:
            return self._graphAction("unweldSelected")
        if action == modes.ACTION_SAVE:
            return self._saveGroom()
        return False

    def _loopOr(self, name, fallback):
        """The active loop's action, or the controller's plain one."""
        handler = (getattr(self._loop, name, None)
                   if self._loop is not None else None)
        if handler is None:
            return bool(fallback())
        done = bool(handler())
        self._refresh()
        self.scheduleIdle()
        return done

    def _adjustRadius(self, delta):
        if self._loop is None:
            return False
        return bool(self._loop.adjustRadius(delta))

    def _graphAction(self, name):
        loop = self._loop
        entry = getattr(loop, name, None) if loop is not None else None
        if entry is None:
            return False
        entry()
        self._session.publish()
        self._refresh()
        self.scheduleIdle()
        return True

    # -- hierarchy keys ----------------------------------------------------

    def selectedTubes(self):
        if self._session is None:
            return []
        return [i for i, _s, _ss in self._session.readSelection(
            tonicLib.TONIC_PICK_TUBE_VERT)]

    def subdivideSelection(self):
        """Shift+D over the selected tubes (plan/18 section 3.4)."""
        session = self._session
        tubes = self.selectedTubes()
        if session is None or session.model is None or not tubes:
            self._status("Tonic: select a tube to subdivide")
            return False
        count = tonicHierarchy.clampSubdivideCount(self._state.subdivideCount)
        session.beginGesture("Subdivide")
        made = 0
        for tubeId in tubes:
            try:
                made += len(tonicHierarchy.subdivide(
                    session.dll, session.model, tubeId, count,
                    self._state.splitMode, 0))
            except RuntimeError as exc:
                self._status("Tonic Hierarchy: %s" % exc)
        session.endGesture()
        session.publish()
        session.enqueueCommit()
        self._refresh()
        self.scheduleIdle()
        self._status("Tonic Hierarchy: %d child tube(s)" % made)
        return True

    def mergeChildrenOfSelection(self):
        """Shift+M over the selected tubes."""
        session = self._session
        tubes = self.selectedTubes()
        if session is None or session.model is None or not tubes:
            self._status("Tonic: select a parent tube to merge")
            return False
        session.beginGesture("Merge children")
        for tubeId in tubes:
            try:
                tonicHierarchy.mergeChildren(session.dll, session.model,
                                             tubeId)
            except RuntimeError as exc:
                self._status("Tonic Hierarchy: %s" % exc)
        session.endGesture()
        session.publish()
        session.enqueueCommit()
        self._refresh()
        self.scheduleIdle()
        self._status(tonicHierarchy.mergeChildrenStatus(
            "%d tube(s)" % len(tubes)))
        return True

    def enterLevel(self):
        status = tonicHierarchy.enterLevel(self._state)
        self._pushFocusLevel()
        self._status(status)
        return True

    def exitLevel(self):
        status = tonicHierarchy.exitLevel(self._state)
        self._pushFocusLevel()
        self._status(status)
        return True

    def _pushFocusLevel(self):
        session = self._session
        if session is None or session.model is None:
            return
        session.dll.Tonic_SetFocusLevel(session.model,
                                        int(self._state.activeLevel))
        session.publish(tonicLib.TONIC_DIRTY_DISPLAY)
        # The focus level is one of the display policy's three inputs, so
        # entering or leaving a level re-resolves the whole table: the new
        # focused level comes forward at 25 % and the rest fall back to
        # 10 % (plan/18 section 2.4a). With no mode active there is no
        # table to resolve and the focus level above is the whole change.
        self._applyDisplayPolicy()
        self._refresh()

    # -- file commands -----------------------------------------------------

    def _saveGroom(self):
        session = self._session
        if session is None or session.model is None:
            return False
        parent = getattr(self._api, "qMainWindow", None)
        path = askSaveFile(parent, "Save Tonic groom",
                           "USD crate (*.usdc)", "groom.usdc")
        if not path:
            return True
        session.saveGroom(path)
        return True


def _ViewFilter(controller, view):
    """The StageView filter: mouse in, ToolLoop calls out."""
    from pxr.Usdviewq.qt import QtCore

    class Filter(QtCore.QObject):
        def eventFilter(self, _obj, event):
            try:
                kind = event.type()
            except (AttributeError, RuntimeError):
                return False
            try:
                if kind == QtCore.QEvent.Type.MouseButtonPress:
                    return controller.onPress(view, event)
                if kind == QtCore.QEvent.Type.MouseMove:
                    return controller.onMove(view, event)
                if kind == QtCore.QEvent.Type.MouseButtonRelease:
                    return controller.onRelease(view, event)
                if kind == QtCore.QEvent.Type.MouseButtonDblClick:
                    return controller.onDoubleClick(view, event)
                hardLossTypes = tuple(value for value in (
                    getattr(QtCore.QEvent.Type, "Hide", None),
                    getattr(QtCore.QEvent.Type, "UngrabMouse", None),
                    getattr(QtCore.QEvent.Type, "WindowDeactivate", None))
                                  if value is not None)
                if kind in hardLossTypes:
                    # Leave is deliberately excluded: a captured left drag
                    # remains valid outside StageView. FocusOut is keyboard
                    # focus only, whereas capture/window loss has no matching
                    # release, so restore only for the latter.
                    controller._recoverGesture(
                        "viewport focus/capture loss type=%s" % int(kind))
                    controller.setPointerInside(False)
                    return False
                if kind == QtCore.QEvent.Type.Resize:
                    controller._syncRegionOverlay()
                    controller._syncMarqueeOverlay()
                if kind == QtCore.QEvent.Type.Enter:
                    controller.setPointerInside(True)
                elif kind == QtCore.QEvent.Type.Leave:
                    controller.setPointerInside(False)
            except Exception as exc:        # noqa: BLE001 - never wedge Qt
                controller._status("Tonic viewport: %s" % exc)
                # A press exception can have opened the loop's bracket before
                # the controller marks `_gesture`; force its cancel route.
                controller._recoverGesture("viewport event exception",
                                           force=True)
                return False
            return False

    return Filter(view)


def _KeyFilter(controller):
    """The application-level key filter, alive only while installed."""
    from pxr.Usdviewq.qt import QtCore

    class Filter(QtCore.QObject):
        def eventFilter(self, _obj, event):
            try:
                kind = event.type()
            except (AttributeError, RuntimeError):
                return False
            deactivated = tuple(value for value in (
                getattr(QtCore.QEvent.Type, "ApplicationDeactivate", None),
                getattr(QtCore.QEvent.Type, "WindowDeactivate", None))
                                if value is not None)
            if kind in deactivated:
                controller._recoverGesture("application deactivated")
                controller.setPointerInside(False)
                return False
            if kind == QtCore.QEvent.Type.ShortcutOverride:
                # usdview binds L itself.  Claim a Tonic key at Qt's
                # shortcut-arbitration stage so the following KeyPress still
                # reaches onKey instead of silently retaining the previous
                # brush.
                key = keyName(event)
                modifiers = modifierSet(event)
                if (controller._brushResizeShortcutCandidate() and key == "f" and
                        not modifiers):
                    event.accept()
                    return True
                if (controller._installed and
                        controller._state.workspaceOpen and
                        controller._tubeShortcutCandidate(key, modifiers)):
                    event.accept()
                    return True
                action = tonicModes.HotkeyAction(
                    key, modifiers,
                    controller._pointerInside, controller._textFocus())
                if (action is not None and
                        action[0] == tonicModes.ACTION_SUBMODE and
                        not controller._subModeForLetter(action[1])):
                    action = None       # e.g. F still frames in usdview
                if (controller._installed and
                        controller._state.workspaceOpen and
                        action is not None):
                    event.accept()
                    return True
                return False
            if kind == QtCore.QEvent.Type.KeyRelease:
                return controller.onKeyRelease(event)
            if kind != QtCore.QEvent.Type.KeyPress:
                return False
            try:
                return controller.onKey(event)
            except Exception as exc:        # noqa: BLE001 - never wedge Qt
                controller._status("Tonic hotkey: %s" % exc)
                return False

    return Filter()
