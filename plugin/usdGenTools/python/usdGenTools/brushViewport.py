# Viewport stroke capture for the brush tool: the StageView event filters.
#
# Two filters, installed together (the tonicViewport.py arrangement):
#   * one on the StageView, turning left press / move / release into
#     BrushLoop.press / move / release at PHYSICAL pixels (Qt's logical
#     coordinates times devicePixelRatioF, the scaling StageView's own
#     handlers do), tracking the brush rings between gestures, and driving
#     a live drag-adjust (F / Shift+F / Ctrl+F);
#   * one on QApplication, installed after usdview's AppEventFilter so Qt
#     runs ours first. It claims Escape only while our own gesture or
#     adjust is live (plan/08 section 3.5: AppEventFilter swallows Escape,
#     so a view-local filter would never see it), and the brush hotkeys
#     only under the conditions below.
#
# Hotkeys (a DCC style; brushPanels.hotkeyFor is the table):
#   F / Shift+F / Ctrl+F  drag-adjust radius / strength / hardness: the
#                         press anchors at the cursor, horizontal motion
#                         changes the value with live rings and a readout,
#                         left click / Enter confirms, Esc / right click
#                         cancels;
#   [ / ]                 radius -10% / +10%;
#   Shift+[ / Shift+]     hardness -0.1 / +0.1.
# They are claimed ONLY while the pointer is over the StageView, a surface
# is bound, strokes are armed, no gesture or adjust is live, no popup or
# modal is up and no text widget OUTSIDE the brush palette has focus
# (brushPanels.hotkeyDecision). usdview's AppEventFilter keeps focus on any
# spinbox/slider/combo ("jealous focus"), so focus held by the palette is
# taken back instead: the pointer entering the view, or a claimed key,
# moves focus to the StageView and the palette editor commits its value.
# Over the view with nothing bound or strokes disarmed, a hotkey passes
# through with a status hint. usdview binds F to Frame
# Selected through a QAction shortcut, so the filter accepts the
# ShortcutOverride for a claimed key (which suppresses the shortcut) and
# handles the KeyPress that follows; with any condition false it touches
# neither and framing works as stock. Ctrl+Z / Ctrl+Y are never claimed.
#
# The filter is narrow by contract: Alt/Meta presses always pass through
# to the camera, misses pass through to usdview picking. Every captured
# step ends in UpdateViewport(), the mandatory repaint request after an
# edit (S43). No exception ever escapes an event filter: it is reported
# to the status sink and the event passes through.
#
# Live groom: loop.move writes the scratch primvar at most every
# LIVE_GROOM_MIN_INTERVAL; this module owns the single-shot flush timer
# (LIVE_GROOM_FLUSH_MS, restarted by every captured press/move) that lands
# the trailing move of a burst through loop.flushLiveGroom.
#
# The rings are a mouse-transparent child widget of the StageView: the
# outer ring (radius) white over a dark halo, the inner ring (hardness *
# radius) thinner and dimmer, and a centre dot, in LOGICAL pixels. They
# show while a surface is bound and strokes are armed, through hover, drag
# and adjust; they never capture.
#
# Qt lives ONLY in this module and brushPalette.py (plan/08 section 1.2).

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from . import brushCamera, brushLoop, brushPanels, brushPick


def stageViewOf(usdviewApi):
    """The StageView widget behind `usdviewApi`, or None.

    UsdviewApi exposes no stageView property in this USD build; the
    fallbacks are the app controller's private member and then the widget
    tree (the same lesson tonicViewport.stageViewOf records)."""
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
    try:
        from pxr.Usdviewq.stageView import StageView
    except ImportError:
        return None
    return window.findChild(StageView)


def _ratio(view):
    """devicePixelRatioF, 1.0 when the view cannot say (tests, teardown)."""
    try:
        return max(float(view.devicePixelRatioF()), 1.0)
    except (AttributeError, RuntimeError, TypeError, ValueError):
        return 1.0


def _logical(event):
    """Event position as logical (Qt) pixels."""
    pos = event.position() if hasattr(event, "position") else event.pos()
    return (float(pos.x()), float(pos.y()))


def _physical(event, view):
    """Event position as physical (device) pixels."""
    x, y = _logical(event)
    ratio = _ratio(view)
    return (x * ratio, y * ratio)


def _hasCameraModifier(event):
    mods = event.modifiers()
    return bool(mods & (QtCore.Qt.KeyboardModifier.AltModifier
                        | QtCore.Qt.KeyboardModifier.MetaModifier))


_KEY_NAMES = {
    QtCore.Qt.Key.Key_F: "F",
    QtCore.Qt.Key.Key_BracketLeft: "[",
    QtCore.Qt.Key.Key_BracketRight: "]",
    QtCore.Qt.Key.Key_BraceLeft: "{",
    QtCore.Qt.Key.Key_BraceRight: "}",
}


def _keyName(event):
    try:
        return _KEY_NAMES.get(event.key())
    except (TypeError, KeyError):
        return None


# Object names of the brush palette dock and its root (brushPalette.py).
PALETTE_OBJECT_NAMES = ("usdGenBrushPaletteDock", "usdGenBrushPalette")


def _inPalette(widget):
    """True when `widget` is the brush palette dock or inside it."""
    try:
        while widget is not None:
            if widget.objectName() in PALETTE_OBJECT_NAMES:
                return True
            widget = widget.parentWidget()
    except RuntimeError:
        return False
    return False


def _focusClass():
    """brushPanels.FOCUS_*: where keyboard focus sits right now."""
    widget = QtWidgets.QApplication.focusWidget()
    if widget is None:
        return brushPanels.FOCUS_NONE
    if _inPalette(widget):
        return brushPanels.FOCUS_PALETTE
    if _textFocus(widget):
        return brushPanels.FOCUS_FOREIGN
    return brushPanels.FOCUS_NONE


def _textFocus(widget=None):
    """True when a text-editing widget holds keyboard focus."""
    if widget is None:
        widget = QtWidgets.QApplication.focusWidget()
    if widget is None:
        return False
    textTypes = (QtWidgets.QLineEdit, QtWidgets.QTextEdit,
                 QtWidgets.QPlainTextEdit, QtWidgets.QAbstractSpinBox)
    if isinstance(widget, textTypes):
        return True
    if isinstance(widget, QtWidgets.QComboBox) and widget.isEditable():
        return True
    return False


class _StageViewFilter(QtCore.QObject):
    def __init__(self, controller):
        super(_StageViewFilter, self).__init__()
        self._controller = controller

    def eventFilter(self, watched, event):
        try:
            return self._controller.viewEvent(watched, event)
        except Exception as exc:  # never escape a Qt event filter
            self._controller.reportError("viewport", exc)
            return False


class _KeyFilter(QtCore.QObject):
    def __init__(self, controller):
        super(_KeyFilter, self).__init__()
        self._controller = controller

    def eventFilter(self, _watched, event):
        try:
            kind = event.type()
            if kind == QtCore.QEvent.Type.ShortcutOverride:
                return self._controller.keyEvent(event, True)
            if kind == QtCore.QEvent.Type.KeyPress:
                return self._controller.keyEvent(event, False)
        except Exception as exc:  # never escape a Qt event filter
            self._controller.reportError("hotkey", exc)
        return False


class _RingOverlay(QtWidgets.QWidget):
    """The brush rings, a transparent child of the StageView.

    Mouse-transparent so it never eats the events the stroke filter
    reads; hidden whenever there is nothing to show. Repaints only the
    rings' own rectangle, old plus new, so hover tracking never
    recomposites the whole view. Logical pixels throughout."""

    _PAD = 5.0  # halo width plus antialiasing spill, each side

    def __init__(self, view):
        super(_RingOverlay, self).__init__(view)
        self.setAttribute(
            QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(
            QtCore.Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(
            QtCore.Qt.WidgetAttribute.WA_NoSystemBackground)
        self.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
        self._ring = None   # (x, y, outer, inner), or None when hidden
        self._accent = False
        self.setGeometry(view.rect())
        self.hide()

    def ring(self):
        """(x, y, outer) in logical pixels, or None."""
        if self._ring is None:
            return None
        return self._ring[:3]

    def rings(self):
        """(x, y, outer, inner) in logical pixels, or None."""
        return self._ring

    def _rect(self, ring):
        x, y, radius = ring[0], ring[1], ring[2]
        pad = radius + self._PAD
        return QtCore.QRectF(x - pad, y - pad, 2.0 * pad, 2.0 * pad)

    def setRing(self, x, y, radius, inner=0.0, accent=False):
        ring = (float(x), float(y), float(radius), float(inner))
        if ring == self._ring and accent == self._accent \
                and self.isVisible():
            return
        dirty = self._rect(ring)
        if self._ring is not None:
            dirty = dirty.united(self._rect(self._ring))
        self._ring = ring
        self._accent = bool(accent)
        if self.isHidden():
            self.show()
            self.raise_()
        self.update(dirty.toAlignedRect())

    def clear(self):
        if self._ring is None and self.isHidden():
            return
        self._ring = None
        # hide() repaints the uncovered parent region on its own.
        self.hide()

    def paintEvent(self, _event):
        if self._ring is None:
            return
        x, y, radius, inner = self._ring
        if radius <= 0.0:
            return
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing)
        centre = QtCore.QPointF(x, y)
        painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
        halo = QtGui.QPen(QtGui.QColor(0, 0, 0, 200))
        halo.setWidthF(3.0)
        painter.setPen(halo)
        painter.drawEllipse(centre, radius + 1.0, radius + 1.0)
        ink = QtGui.QColor(120, 185, 255, 245) if self._accent \
            else QtGui.QColor(255, 255, 255, 240)
        pen = QtGui.QPen(ink)
        pen.setWidthF(1.5)
        painter.setPen(pen)
        painter.drawEllipse(centre, radius, radius)
        if 1.5 <= inner < radius - 0.5:
            innerHalo = QtGui.QPen(QtGui.QColor(0, 0, 0, 120))
            innerHalo.setWidthF(2.0)
            painter.setPen(innerHalo)
            painter.drawEllipse(centre, inner + 0.75, inner + 0.75)
            dim = QtGui.QColor(ink)
            dim.setAlpha(150)
            innerPen = QtGui.QPen(dim)
            innerPen.setWidthF(1.0)
            innerPen.setStyle(QtCore.Qt.PenStyle.DashLine)
            painter.setPen(innerPen)
            painter.drawEllipse(centre, inner, inner)
        painter.setPen(QtCore.Qt.PenStyle.NoPen)
        painter.setBrush(QtGui.QBrush(ink))
        painter.drawEllipse(centre, 2.0, 2.0)
        painter.end()


class _Readout(QtWidgets.QLabel):
    """The drag-adjust value readout, a small pill next to the anchor."""

    def __init__(self, view):
        super(_Readout, self).__init__(view)
        self.setAttribute(
            QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
        self.setStyleSheet(
            "QLabel { background: rgba(28, 28, 32, 225); color: #e8e8ec;"
            " border: 1px solid #4d9be6; border-radius: 6px;"
            " padding: 3px 8px; font-weight: 600; }")
        self.hide()

    def showText(self, text, x, y):
        self.setText(text)
        self.adjustSize()
        parent = self.parentWidget()
        px, py = int(x) + 18, int(y) - self.height() - 10
        if parent is not None:
            px = max(4, min(px, parent.width() - self.width() - 4))
            py = max(4, min(py, parent.height() - self.height() - 4))
        self.move(px, py)
        if self.isHidden():
            self.show()
        self.raise_()


class _Adjust(object):
    """One live drag-adjust: which value, where it started, from what."""

    def __init__(self, action, field, start, anchor):
        self.action = action
        self.field = field
        self.start = start
        self.anchor = anchor   # logical (x, y)
        self.value = start


class BrushViewportController(object):
    """Installs the stroke filters and routes events into a BrushLoop."""

    def __init__(self, usdviewApi, state, loop=None):
        self._api = usdviewApi
        self._state = state
        self._loop = loop if loop is not None else brushLoop.BrushLoop(state)
        self._view = None
        self._viewFilter = _StageViewFilter(self)
        self._keyFilter = _KeyFilter(self)
        self._installed = False
        self._statusSink = None
        self._overlay = None
        self._readout = None
        self._trackingWas = False
        # Hover-ring cache: the snapshot is mesh-sized, so it is read
        # once per bound surface (every press refreshes it for free)
        # while picks re-run past the move throttle.
        self._hoverSnapshot = None
        self._hoverKey = None
        self._hoverAt = None
        self._hoverRadiusWorld = None
        self._hoverCached = None
        self._pointer = None        # last logical pointer pos over the view
        self._adjust = None
        self._swallowRelease = set()
        self._groomTimer = None
        self.groomFlushes = 0       # timer flushes that wrote (T3 reads)
        self._stageSignal = None
        self._connectStageReplaced()

    @property
    def loop(self):
        return self._loop

    @property
    def installed(self):
        return self._installed

    @property
    def view(self):
        return self._view

    @property
    def adjusting(self):
        """The live drag-adjust action id, or None."""
        return self._adjust.action if self._adjust is not None else None

    def setStatusSink(self, sink):
        self._statusSink = sink

    def _status(self, text, kind="idle"):
        sink = self._statusSink
        if sink is None:
            return
        try:
            sink(text, kind)
        except TypeError:
            sink(text)

    def reportError(self, where, exc):
        """An exception inside a filter: report it, never raise it."""
        try:
            self._status("%s error: %s" % (where, exc), "error")
        except Exception:
            pass

    # -- install ----------------------------------------------------------

    def install(self):
        """Install both filters. False when there is no StageView."""
        if self._installed and self._viewAlive():
            return True
        if self._installed:
            self.uninstall()
        view = stageViewOf(self._api)
        if view is None:
            return False
        try:
            view.installEventFilter(self._viewFilter)
            self._overlay = _RingOverlay(view)
            self._readout = _Readout(view)
        except RuntimeError:
            return False
        self._view = view
        # Hover moves only exist with mouse tracking; without it the
        # ring would never update (and QTest hovers never arrive). A
        # tracking failure degrades the ring, never the strokes.
        try:
            self._trackingWas = bool(view.hasMouseTracking())
            view.setMouseTracking(True)
        except RuntimeError:
            pass
        app = QtWidgets.QApplication.instance()
        if app is not None:
            # Installed AFTER usdview's AppEventFilter, so Qt runs ours
            # first -- the only reason Escape is reachable at all.
            app.installEventFilter(self._keyFilter)
        self._installed = True
        return True

    def uninstall(self):
        self._endAdjust(commit=False, quiet=True)
        if self._view is not None:
            try:
                self._view.removeEventFilter(self._viewFilter)
            except RuntimeError:
                pass
            try:
                self._view.setMouseTracking(self._trackingWas)
            except RuntimeError:
                pass
            self._view = None
        for widget in (self._overlay, self._readout):
            if widget is not None:
                try:
                    widget.hide()
                    widget.deleteLater()
                except RuntimeError:
                    pass
        self._overlay = None
        self._readout = None
        self._resetHover()
        self._stopGroomTimer()
        self._groomTimer = None
        app = QtWidgets.QApplication.instance()
        if app is not None:
            try:
                app.removeEventFilter(self._keyFilter)
            except RuntimeError:
                pass
        self._installed = False

    def _resetHover(self):
        self._hoverSnapshot = None
        self._hoverKey = None
        self._hoverAt = None
        self._hoverRadiusWorld = None
        self._hoverCached = None
        self._pointer = None

    def _connectStageReplaced(self):
        dataModel = getattr(self._api, "dataModel", None)
        signal = getattr(dataModel, "signalStageReplaced", None)
        if signal is None:
            return
        try:
            signal.connect(self.onStageReplaced)
            self._stageSignal = signal
        except (AttributeError, RuntimeError, TypeError):
            self._stageSignal = None

    def onStageReplaced(self, *_args):
        """A new stage: drop timers and caches, reinstall the filters."""
        try:
            wasInstalled = self._installed
            self._stopGroomTimer()
            self.uninstall()
            if wasInstalled:
                self.install()
        except Exception as exc:
            self.reportError("stage replace", exc)

    def _viewAlive(self):
        """False once the StageView's C++ object is gone."""
        if self._view is None:
            return False
        try:
            self._view.isVisible()
            return True
        except RuntimeError:
            return False

    def _currentStage(self):
        dataModel = getattr(self._api, "dataModel", None)
        return getattr(dataModel, "stage", None)

    def _update(self):
        try:
            self._api.UpdateViewport()
        except Exception:
            pass

    # -- event routing --------------------------------------------------

    def viewEvent(self, watched, event):
        """The StageView filter body. True claims the event."""
        if watched is not self._view or not self._viewAlive():
            return False
        kind = event.type()
        Type = QtCore.QEvent.Type
        if kind == Type.MouseButtonPress:
            if self._adjust is not None:
                return self._adjustPress(event)
            if event.button() == QtCore.Qt.MouseButton.LeftButton:
                return self.pressEvent(watched, event)
        elif kind == Type.MouseMove:
            first = self._pointer is None
            self._pointer = _logical(event)
            if self._adjust is not None:
                self._adjustMove(event)
                return True
            if first:
                self.reclaimFocus()
            return self.moveEvent(watched, event)
        elif kind == Type.MouseButtonRelease:
            button = event.button()
            if button in self._swallowRelease:
                self._swallowRelease.discard(button)
                return True
            if button == QtCore.Qt.MouseButton.LeftButton:
                return self.releaseEvent(watched, event)
        elif kind == Type.Enter:
            self.reclaimFocus()
        elif kind == Type.Leave:
            self._pointer = None
            if self._adjust is None:
                self.hoverLeave()
        elif kind == Type.Resize:
            self.syncOverlay()
        return False

    def keyEvent(self, event, override):
        """The application key filter body. True claims the event.

        `override` is the ShortcutOverride pass: accepting it keeps
        usdview's shortcut (Frame Selected on F) from firing, and the
        KeyPress that follows is delivered here."""
        key = event.key()
        Key = QtCore.Qt.Key
        if self._adjust is not None:
            if key == Key.Key_Escape:
                return self._claimKey(event, override,
                                      lambda: self._endAdjust(False))
            if key in (Key.Key_Return, Key.Key_Enter):
                return self._claimKey(event, override,
                                      lambda: self._endAdjust(True))
            if _keyName(event) == "F":
                # Auto-repeat of the key that started the adjust.
                return self._claimKey(event, override, lambda: None)
            return False
        if key == Key.Key_Escape:
            if not self._loop.gestureActive():
                return False
            if override:
                event.accept()
                return True
            return self.escapeEvent()
        name = _keyName(event)
        if name is None:
            return False
        mods = event.modifiers()
        Mod = QtCore.Qt.KeyboardModifier
        action = brushPanels.hotkeyFor(
            name,
            shift=bool(mods & Mod.ShiftModifier),
            ctrl=bool(mods & Mod.ControlModifier),
            alt=bool(mods & (Mod.AltModifier | Mod.MetaModifier)))
        if action is None:
            return False
        claim, steal, hint = self.hotkeyDecision()
        if not claim:
            if hint and not override and not event.isAutoRepeat():
                self._status(hint, "idle")
            return False
        if steal:
            # The palette holds focus (usdview's jealous focus keeps it
            # on a spinbox forever): hand it to the view, committing the
            # editor's value, and take the key.
            self.takeFocus()
        if override:
            event.accept()
            return True
        if event.isAutoRepeat() and action in brushPanels.ADJUST_FIELDS:
            return True
        self.runHotkey(action)
        return True

    def _claimKey(self, event, override, run):
        if override:
            event.accept()
            return True
        run()
        return True

    def pointerOverView(self):
        """True when the pointer is over the StageView.

        The tracked hover position covers synthetic (QTest direct) moves;
        the cursor check covers a pointer that has not moved since the
        view appeared."""
        if not self._viewAlive():
            return False
        if self._pointer is not None:
            return True
        try:
            local = self._view.mapFromGlobal(QtGui.QCursor.pos())
            return self._view.rect().contains(local)
        except RuntimeError:
            return False

    def _busy(self):
        """A live gesture/adjust or an open popup/modal: hotkeys pass."""
        if self._loop.gestureActive() or self._adjust is not None:
            return True
        app = QtWidgets.QApplication.instance()
        return app is not None and (app.activePopupWidget() is not None
                                    or app.activeModalWidget() is not None)

    def hotkeyDecision(self):
        """(claim, stealFocus, hint) for a brush hotkey right now."""
        if not self._installed or not self._viewAlive():
            return (False, False, "")
        return brushPanels.hotkeyDecision(
            pointerOver=self.pointerOverView(),
            bound=getattr(self._state, "binding", None) is not None,
            armed=bool(getattr(self._state, "strokesArmed", True)),
            busy=self._busy(),
            focus=_focusClass())

    def hotkeysClaimable(self):
        """Every condition the brush hotkeys are claimed under."""
        return self.hotkeyDecision()[0]

    def takeFocus(self):
        """Move keyboard focus to the StageView (an editor commits)."""
        if not self._viewAlive():
            return False
        try:
            self._view.setFocus(QtCore.Qt.FocusReason.MouseFocusReason)
            # Belt and braces: if the view could not take focus (hidden,
            # disabled), at least drop it from the palette editor so it
            # commits and stops eating keys.
            held = QtWidgets.QApplication.focusWidget()
            if held is not None and _inPalette(held):
                held.clearFocus()
            return True
        except RuntimeError:
            return False

    def reclaimFocus(self):
        """The pointer is over the view: pull focus back from the palette.

        usdview's AppEventFilter never moves focus off a spinbox, slider
        or combo, so without this every key after a palette edit would go
        to the palette. Focus held anywhere else is left alone."""
        widget = QtWidgets.QApplication.focusWidget()
        if widget is None or not _inPalette(widget) or self._busy():
            return False
        return self.takeFocus()

    def runHotkey(self, action):
        """Apply one brush hotkey action (also callable from tests)."""
        state = self._state
        if action in brushPanels.ADJUST_FIELDS:
            self.startAdjust(action)
            return
        if action == "radiusDown":
            state.radiusWorld = brushPanels.stepRadius(state.radiusWorld, -1)
        elif action == "radiusUp":
            state.radiusWorld = brushPanels.stepRadius(state.radiusWorld, 1)
        elif action == "hardnessDown":
            state.hardness = brushPanels.stepHardness(state.hardness, -1)
        elif action == "hardnessUp":
            state.hardness = brushPanels.stepHardness(state.hardness, 1)
        else:
            return
        self._hoverAt = None  # re-pick the ring at the new size
        self._redrawHoverRing()
        self._status("radius %.3g  hardness %.2f"
                     % (state.radiusWorld, state.hardness), "idle")

    # -- drag-adjust ------------------------------------------------------

    def startAdjust(self, action):
        """Begin an F / Shift+F / Ctrl+F drag-adjust at the pointer."""
        field = brushPanels.ADJUST_FIELDS.get(action)
        if field is None or not self._viewAlive():
            return False
        anchor = self._pointer
        if anchor is None:
            try:
                local = self._view.mapFromGlobal(QtGui.QCursor.pos())
                anchor = (float(local.x()), float(local.y()))
            except RuntimeError:
                return False
        start = float(getattr(self._state, field, 0.0))
        self._adjust = _Adjust(action, field, start, anchor)
        self._adjustRings()
        self._status("adjust %s: move left/right, click or Enter to "
                     "confirm, Esc or right click to cancel"
                     % brushPanels.ADJUST_LABELS[action].lower(), "stroke")
        return True

    def adjustTo(self, logicalX):
        """Set the live adjust from a logical x (the move handler; tests)."""
        adjust = self._adjust
        if adjust is None:
            return None
        adjust.value = brushPanels.adjustValue(
            adjust.action, adjust.start, float(logicalX) - adjust.anchor[0])
        setattr(self._state, adjust.field, adjust.value)
        self._adjustRings()
        return adjust.value

    def _adjustMove(self, event):
        x, _y = _logical(event)
        self.adjustTo(x)

    def _adjustPress(self, event):
        button = event.button()
        if button == QtCore.Qt.MouseButton.LeftButton:
            self._endAdjust(True)
        else:
            self._endAdjust(False)
        # Swallow the matching release so usdview never sees half a click.
        self._swallowRelease.add(button)
        return True

    def _endAdjust(self, commit, quiet=False):
        adjust = self._adjust
        if adjust is None:
            return
        self._adjust = None
        if not commit:
            setattr(self._state, adjust.field, adjust.start)
        if self._readout is not None:
            try:
                self._readout.hide()
            except RuntimeError:
                pass
        self._hoverAt = None
        if not quiet:
            label = brushPanels.ADJUST_LABELS[adjust.action]
            if commit:
                self._status("%s %.3g" % (label.lower(),
                                          getattr(self._state, adjust.field)),
                             "idle")
            else:
                self._status("%s adjust cancelled" % label.lower(), "idle")
            self._redrawHoverRing()

    def _adjustRings(self):
        """Rings and readout at the adjust anchor, sized by the live value."""
        adjust = self._adjust
        if adjust is None or self._overlay is None:
            return
        lx, ly = adjust.anchor
        ratio = _ratio(self._view)
        outer = self._ringPixelsAt(lx * ratio, ly * ratio,
                                   self._state.radiusWorld)
        if outer is None:
            # Off the surface: a nominal ring scaled with the radius.
            base = 60.0
            start = self._adjustStartRadius()
            outer = base * (self._state.radiusWorld / start) if start > 0 \
                else base
        else:
            outer = outer / ratio
        outer, inner = brushPanels.ringRadii(outer, self._state.hardness)
        self._overlay.setRing(lx, ly, outer, inner, accent=True)
        label = brushPanels.ADJUST_LABELS[adjust.action]
        value = getattr(self._state, adjust.field)
        text = ("%s  %.3g" % (label, value) if adjust.field == "radiusWorld"
                else "%s  %.2f" % (label, value))
        if self._readout is not None:
            self._readout.showText(text, lx + outer, ly)

    def _adjustStartRadius(self):
        adjust = self._adjust
        if adjust is not None and adjust.field == "radiusWorld":
            return adjust.start
        return float(self._state.radiusWorld)

    def _ringPixelsAt(self, x, y, radiusWorld):
        """Physical outer-ring radius for a pick at physical (x, y)."""
        binding = getattr(self._state, "binding", None)
        stage = self._currentStage()
        if binding is None or stage is None:
            return None
        snapshot = self._hoverSnapshotFor(stage, binding)
        camera = brushCamera.resolve(self._view)
        if snapshot is None or camera is None:
            return None
        ring = brushPick.ringPixels(snapshot, camera, x, y, radiusWorld)
        return ring[2] if ring is not None else None

    # -- strokes ----------------------------------------------------------

    def pressEvent(self, watched, event):
        if watched is not self._view or _hasCameraModifier(event):
            return False
        stage = self._currentStage()
        if stage is None:
            return False
        x, y = _physical(event, self._view)
        camera = brushCamera.resolve(self._view)
        captured, info = self._loop.press(stage, camera, x, y)
        if captured:
            self._noteGestureSnapshot()
            self._armGroomFlush()
            # The drag's first ring: a click without a move still marks
            # the dab, and the hover ring may never have shown.
            self._dragRing(x, y)
            self._status("stroke: " + info, "stroke")
            self._update()
            return True
        if info and info not in ("miss", "no surface bound",
                                 "strokes are disarmed"):
            self._status("stroke: " + info, "error")
        return False

    def moveEvent(self, watched, event):
        if watched is not self._view:
            return False
        if not self._loop.gestureActive():
            self._hoverRing(event)
            return False
        if not (event.buttons() & QtCore.Qt.MouseButton.LeftButton):
            return False
        stage = self._currentStage()
        if stage is None:
            return False
        x, y = _physical(event, self._view)
        ok, info = self._loop.move(stage, x, y)
        if not ok:
            self._status("stroke: " + info, "error")
            self._stopGroomTimer()
            self._update()
        elif info not in ("throttled", "miss"):
            self._armGroomFlush()
            live = info.endswith("live groom")
            self._status("stroke: " + info, "cooking" if live else "stroke")
            self._update()
        self._dragRing(x, y)
        return True

    def releaseEvent(self, watched, event):
        if watched is not self._view:
            return False
        if not self._loop.gestureActive():
            return False
        stage = self._currentStage()
        if stage is None:
            return False
        self._stopGroomTimer()
        baked, info = self._loop.release(stage)
        self._status(("baked: " if baked else "stroke: ") + info,
                     "cooking" if baked else "error")
        self._update()
        return True

    def escapeEvent(self):
        if not self._loop.gestureActive():
            return False
        stage = self._currentStage()
        if stage is None:
            return False
        self._stopGroomTimer()
        self._loop.cancel(stage)
        self._status("stroke cancelled", "idle")
        self._update()
        return True

    # -- live groom flush -------------------------------------------------

    def _armGroomFlush(self):
        """(Re)start the single-shot trailing flush."""
        if not getattr(self._state, "liveGroom", True):
            return
        if self._groomTimer is None and self._viewAlive():
            try:
                timer = QtCore.QTimer(self._view)
                timer.setSingleShot(True)
                timer.setInterval(int(brushLoop.LIVE_GROOM_FLUSH_MS))
                timer.timeout.connect(self._groomFlush)
                self._groomTimer = timer
            except RuntimeError:
                return
        if self._groomTimer is not None:
            try:
                self._groomTimer.start()
            except RuntimeError:
                self._groomTimer = None

    def _stopGroomTimer(self):
        if self._groomTimer is not None:
            try:
                self._groomTimer.stop()
            except RuntimeError:
                self._groomTimer = None

    def _groomFlush(self):
        try:
            if not self._loop.gestureActive():
                return
            stage = self._currentStage()
            if stage is None:
                return
            written, info = self._loop.flushLiveGroom(stage)
            if written:
                self.groomFlushes += 1
                self._status("live groom: " + info, "cooking")
                self._update()
        except Exception as exc:  # a timer slot must not raise either
            self.reportError("live groom", exc)

    # -- rings ------------------------------------------------------------

    def hoverLeave(self):
        """The cursor left the view: drop the ring, keep the snapshot."""
        if self._overlay is not None:
            try:
                self._overlay.clear()
            except RuntimeError:
                self._overlay = None
        self._hoverAt = None

    def syncOverlay(self):
        """Re-cover the view after a resize."""
        if self._overlay is not None and self._viewAlive():
            try:
                self._overlay.setGeometry(self._view.rect())
            except RuntimeError:
                pass

    def overlayRing(self):
        """The logical-pixel (x, y, outer) ring, or None. Tests read this."""
        if self._overlay is None:
            return None
        return self._overlay.ring()

    def overlayRings(self):
        """The logical-pixel (x, y, outer, inner) rings, or None."""
        if self._overlay is None:
            return None
        return self._overlay.rings()

    def _showRing(self, lx, ly, ring):
        """Draw a physical-pixel ring centred on the cursor; None hides."""
        if ring is None:
            self._overlay.clear()
            return
        outer, inner = brushPanels.ringRadii(ring[2] / _ratio(self._view),
                                             self._state.hardness)
        self._overlay.setRing(lx, ly, outer, inner)

    def _redrawHoverRing(self):
        """Redraw the hover rings at the tracked pointer (value changed)."""
        if self._pointer is None or self._overlay is None:
            return
        if self._loop.gestureActive():
            return
        lx, ly = self._pointer
        ratio = _ratio(self._view)
        ring = None
        outer = self._ringPixelsAt(lx * ratio, ly * ratio,
                                   self._state.radiusWorld)
        if outer is not None:
            ring = (lx * ratio, ly * ratio, outer)
        self._hoverCached = ring
        self._hoverAt = (lx * ratio, ly * ratio)
        self._hoverRadiusWorld = self._state.radiusWorld
        self._showRing(lx, ly, ring)

    def _noteGestureSnapshot(self):
        """Refresh the hover snapshot from the live gesture (press read it)."""
        gesture = getattr(self._state, "gesture", None)
        snapshot = getattr(gesture, "snapshot", None)
        binding = getattr(self._state, "binding", None)
        if snapshot is None or binding is None:
            return
        self._hoverSnapshot = snapshot
        self._hoverKey = self._hoverKeyOf(binding)
        self._hoverAt = None

    @staticmethod
    def _hoverKeyOf(binding):
        # A subset binding's faces mask the hover pick (the ring hides off
        # the subset), so they key the snapshot too.
        return (str(binding.surfacePath),
                getattr(binding, "faces", None))

    def _hoverSnapshotFor(self, stage, binding):
        """The cached hover snapshot, re-read when the binding changes.

        Paint never moves points, so a snapshot outlives every stroke;
        only an outside sculpt or a rebind stales it -- the same
        staleness class as the press-frozen drag snapshot."""
        key = self._hoverKeyOf(binding)
        if self._hoverSnapshot is None or self._hoverKey != key:
            surface = stage.GetPrimAtPath(binding.surfacePath)
            snapshot, _error = brushPick.snapshotMesh(
                surface, getattr(binding, "faces", None))
            if snapshot is None:
                self._hoverSnapshot = None
                self._hoverKey = None
                return None
            self._hoverSnapshot = snapshot
            self._hoverKey = key
        return self._hoverSnapshot

    def _hoverRing(self, event):
        """Track the cursor rings while no gesture is live. Never captures."""
        if self._overlay is None or self._view is None:
            return
        binding = getattr(self._state, "binding", None)
        if binding is None or not getattr(self._state, "strokesArmed", True):
            self._overlay.clear()
            self._hoverAt = None
            return
        stage = self._currentStage()
        if stage is None:
            self._overlay.clear()
            return
        lx, ly = _logical(event)
        ratio = _ratio(self._view)
        x, y = lx * ratio, ly * ratio
        radiusWorld = self._state.radiusWorld
        if (self._hoverAt is not None
                and self._hoverRadiusWorld == radiusWorld
                and brushPick.movedPixels(self._hoverAt, (x, y))
                <= brushPick.PICK_MOVE_PIXELS):
            self._showRing(lx, ly, self._hoverCached)
            return
        snapshot = self._hoverSnapshotFor(stage, binding)
        camera = brushCamera.resolve(self._view)
        ring = None
        if snapshot is not None and camera is not None:
            ring = brushPick.ringPixels(snapshot, camera, x, y, radiusWorld)
        self._hoverAt = (x, y)
        self._hoverRadiusWorld = radiusWorld
        self._hoverCached = ring
        self._showRing(lx, ly, ring)

    def _dragRing(self, x, y):
        """Track the rings through a stroke, on the frozen gesture pick."""
        if self._overlay is None:
            return
        gesture = getattr(self._state, "gesture", None)
        snapshot = getattr(gesture, "snapshot", None)
        camera = getattr(gesture, "camera", None)
        if snapshot is None or camera is None:
            self._overlay.clear()
            return
        ratio = _ratio(self._view)
        ring = brushPick.ringPixels(snapshot, camera, x, y,
                                    self._state.radiusWorld)
        self._showRing(x / ratio, y / ratio, ring)
