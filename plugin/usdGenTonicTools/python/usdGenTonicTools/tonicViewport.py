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
             QtCore.Qt.Key.Key_Up: "up",
             QtCore.Qt.Key.Key_Down: "down",
             QtCore.Qt.Key.Key_Left: "left",
             QtCore.Qt.Key.Key_Right: "right",
             QtCore.Qt.Key.Key_BracketLeft: "[",
             QtCore.Qt.Key.Key_BracketRight: "]"}
    if key in named:
        return named[key]
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
        self._timer = None
        self._loop = None
        self._camera = None
        self._gesture = False
        self._lastXY = None
        self._lastHoverXY = None
        self._pointerInside = False
        self._installed = False
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
        self._viewFilter = _ViewFilter(self, view)
        view.installEventFilter(self._viewFilter)
        self._keyFilter = _KeyFilter(self)
        application = QtWidgets.QApplication.instance()
        if application is not None:
            # Installed AFTER usdview's AppEventFilter, so Qt runs ours
            # first and Escape is ours to claim.
            application.installEventFilter(self._keyFilter)
        self._timer = QtCore.QTimer(view)
        self._timer.setInterval(PUMP_INTERVAL_MS)
        self._timer.timeout.connect(self._onPump)
        self._installed = True
        self._state.viewportFailed = False
        # workspaceOpen belongs to the dock, which sets it from its own
        # visibility (plan/18 section 3.4: the hotkeys live while the
        # workspace is open, not merely while the filter is installed).
        self._connectStageSignals()
        self._connectFrustumSignal()
        self.syncDisplayScale()
        if not self._state.activeMode:
            self.setMode(tonicModes.MODES[0].id)
        return True

    def uninstall(self):
        from pxr.Usdviewq.qt import QtWidgets
        if self._timer is not None:
            self._timer.stop()
            self._timer = None
        if self._view is not None and self._viewFilter is not None:
            self._view.removeEventFilter(self._viewFilter)
        if self._keyFilter is not None:
            application = QtWidgets.QApplication.instance()
            if application is not None:
                application.removeEventFilter(self._keyFilter)
        self._viewFilter = None
        self._keyFilter = None
        self._view = None
        self._installed = False

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
        return session.setDisplayScale(perPixel)

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
        if self._gesture:
            self.cancelGesture()
        if self._loop is not None and mode.id != self._state.activeMode:
            # The outgoing loop takes its overlays with it: a gizmo or a
            # brush ring is model state and would otherwise still be drawn
            # in a mode that cannot drag it (plan/18 section 2.4).
            self._loop.deactivate()
        status = tonicModes.SetActiveMode(self._state, mode.id)
        # A mode with no loop is a panel mode (tonicLoops.PANEL_ONLY_MODES):
        # the shelf switches, the dock shows that mode's parameters and the
        # viewport keeps the camera, so the mode's own status line is the
        # whole truth about it.
        self._loop = tonicLoops.makeLoop(mode.id, self._session, self._state)
        if self._loop is not None and not self._subModeOf(mode.id):
            self.setSubMode(self._loop.defaultSubMode)
        self._applyDisplayPolicy()
        self._status(status)
        return status

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
        status = tonicLoops.setSubModeOn(self._state, modeId, subId)
        self._applyDisplayPolicy()
        self._status(status)
        return status

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
        modifiers = modifierSet(event)
        if "alt" in modifiers or "meta" in modifiers:
            return False                 # the camera's, always
        if event.button() != QtCore.Qt.MouseButton.LeftButton:
            return False
        x, y = eventPixels(view, event)
        self._camera = tonicCamera.resolve(view)
        if self._camera is None:
            return False
        self.syncDisplayScale(self._camera)
        sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                   modifiers)
        claimed = bool(self._loop.press(sample))
        if not claimed:
            return False
        self._gesture = True
        self._lastXY = (x, y)
        self._ladder.arm(self._state.activeLevel)
        self._session.publish()
        self._refresh()
        return True

    def onMove(self, view, event):
        x, y = eventPixels(view, event)
        self._pointerInside = True
        if not self._ready():
            return False
        if not self._gesture:
            return self._onHover(view, event, x, y)
        if self._lastXY is not None:
            if (abs(x - self._lastXY[0]) + abs(y - self._lastXY[1]) <
                    MOVE_THRESHOLD_PX):
                return True
        self._lastXY = (x, y)
        started = time.perf_counter()
        sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                   modifierSet(event))
        claimed = bool(self._loop.move(sample))
        self._session.publish()
        self._refresh()
        # The move is timed end to end -- the loop's work, the publish and
        # the viewport refresh request -- because that is the number TN-1
        # budgets and the number the ladder steps on (plan/18 section 3.7).
        elapsed = (time.perf_counter() - started) * 1000.0
        self._state.lastMoveMs = elapsed
        if self._ladder.noteMove(elapsed):
            self._status("Tonic: %s" % self._ladder.describe())
        return claimed

    def _onHover(self, view, event, x, y):
        if self._ladder.hoverSuppressed:
            return False             # the ladder's last rung
        if self._lastHoverXY is not None:
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
        return False

    def onRelease(self, view, event):
        from pxr.Usdviewq.qt import QtCore
        if not self._gesture or not self._ready():
            return False
        if event.button() not in (QtCore.Qt.MouseButton.LeftButton,
                                  QtCore.Qt.MouseButton.NoButton):
            return False
        x, y = eventPixels(view, event)
        sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                   modifierSet(event))
        claimed = bool(self._loop.release(sample))
        self._gesture = False
        self._lastXY = None
        self._ladder.restore()
        self._session.publish()
        self._refresh()
        self.scheduleIdle()
        return claimed

    def onDoubleClick(self, view, event):
        """Double-click enters the level under the cursor (section 3.4)."""
        from pxr.Usdviewq.qt import QtCore
        if not self._ready():
            return False
        if event.button() != QtCore.Qt.MouseButton.LeftButton:
            return False
        modifiers = modifierSet(event)
        if "alt" in modifiers or "meta" in modifiers:
            return False
        # A loop that knows what is under the cursor enters THAT tube's
        # level (plan/18 section 3.6); anything else just goes one deeper.
        handler = getattr(self._loop, "doubleClick", None)
        if handler is not None:
            x, y = eventPixels(view, event)
            self._camera = tonicCamera.resolve(view)
            sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                       modifiers)
            if handler(sample):
                self._refresh()
                return True
        self.enterLevel()
        return True

    def setPointerInside(self, inside):
        self._pointerInside = bool(inside)

    def cancelGesture(self):
        """Escape: drop the live gesture, restoring the press-time base."""
        if self._loop is None:
            return False
        cancelled = bool(self._loop.cancel())
        self._gesture = False
        self._lastXY = None
        self._ladder.restore()
        if not cancelled and self._session is not None:
            if self._session.selectionCount(0) > 0:
                self._session.clearSelection(0)
                self._session.publish(tonicLib.TONIC_DIRTY_SELECTION)
                cancelled = True
        if cancelled:
            self._refresh()
        return cancelled

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

    def onKey(self, event):
        """The plan/18 section 3.4 table, dispatched. True consumes."""
        if not self._installed or self._session is None:
            return False
        if not self._state.workspaceOpen:
            return False
        action = tonicModes.HotkeyAction(keyName(event), modifierSet(event),
                                         self._pointerInside,
                                         self._textFocus())
        if action is None:
            return False
        return self.runAction(action[0], action[1])

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
                if kind == QtCore.QEvent.Type.Enter:
                    controller.setPointerInside(True)
                elif kind == QtCore.QEvent.Type.Leave:
                    controller.setPointerInside(False)
            except Exception as exc:        # noqa: BLE001 - never wedge Qt
                controller._status("Tonic viewport: %s" % exc)
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
            if kind != QtCore.QEvent.Type.KeyPress:
                return False
            try:
                return controller.onKey(event)
            except Exception as exc:        # noqa: BLE001 - never wedge Qt
                controller._status("Tonic hotkey: %s" % exc)
                return False

    return Filter()
