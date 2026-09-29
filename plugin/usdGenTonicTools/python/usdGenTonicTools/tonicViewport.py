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

import ctypes
import time

from . import tonicBridge
from . import tonicCamera
from . import tonicGizmo
from . import tonicGizmoSettings
from . import tonicHierarchy
from . import tonicLadder
from . import tonicLib
from . import tonicLoops
from . import tonicModes

# plan/18 section 3.2: a move under this many LOGICAL pixels is not a move
# (scaled by the display ratio where it is compared, parity G24).
MOVE_THRESHOLD_PX = tonicGizmo.CLICK_SLOP_PX
# ... and a hover under this many is not a hover.
HOVER_THRESHOLD_PX = 3.0
# plan/18 section 3.3: the idle pump's period while anything is pending.
PUMP_INTERVAL_MS = 50
# FB-03 framing: StageView's own frame fit (about a 10 % margin), and the
# on-screen size under which a selection is centred rather than zoomed onto.
FRAME_FIT = 1.1
FRAME_MIN_PIXELS = 8.0
# Parity G11: the keys held during a gizmo drag -> the Sample modifier name
# TubeLoop reads.  J = relative Step Snap, X = the pivot on the world grid.
# Both are shared with usdview (J toggles its framed view), so they are
# claimed only while a gizmo drag is live.
HOLD_KEYS = {"j": "stepSnap", "x": "grid"}
# Parity G13: Tube-mode gizmo keys.  '+' is Shift+'=' on most layouts, so
# Shift is not read on these.
SIZE_KEYS = ("+", "=", "-")


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


def keyCode(event):
    """A QKeyEvent's raw key code (the latch identity), or its keyName."""
    try:
        return int(event.key())
    except (AttributeError, TypeError, ValueError):
        return keyName(event)


class KeyPressLatch(object):
    """Run the key handler exactly ONCE per physical key press.

    usdRig gizmoUI `_Act`/`_claimedKey`: one press reaches the
    application-level filter many times.  Qt sends a ShortcutOverride and
    then a KeyPress, and delivers each unaccepted one to the focus widget
    and then to every ancestor up to the window -- seven hops from
    usdview's StageView.  A handled key stops the delivery at once, but a
    DECLINED key (runAction returned False: a two-CV region's Enter, Ctrl+Z
    mid-drag) keeps propagating, and acting on every delivery printed
    "needs at least 3 CVs" seven times for one Enter.

    The rule: act on the first KeyPress delivery, remember the key with
    its result, and answer every later delivery of the same press with
    that result -- True keeps swallowing, False keeps passing the event on
    to usdview untouched -- without acting again.  A ShortcutOverride
    (which Qt sends before every physical press, auto-repeats included)
    or the key's real KeyRelease starts the next press; so does a
    different key or modifier set, for events sent straight at the view
    with no override in front.  Qt-free: identities are whatever the
    caller passes (the controller uses (keyCode, modifierSet)).
    """

    def __init__(self):
        self._press = None          # (identity, result) of the live press

    @property
    def latched(self):
        """The identity of the press already acted on, or None."""
        return None if self._press is None else self._press[0]

    def newPress(self):
        """A ShortcutOverride: whatever KeyPress follows is a new press."""
        self._press = None

    def press(self, identity, handler):
        """One KeyPress delivery; runs `handler()` on the first only."""
        live = self._press
        if live is not None and live[0] == identity:
            return live[1]
        # Latched before the call: a handler that raises still counts as
        # the press's one run (declined), so the propagating copies do not
        # raise -- and report -- once per widget.
        marker = (identity, False)
        self._press = marker
        result = bool(handler())
        if self._press is marker:
            self._press = (identity, result)
        return result

    def release(self, key, autoRepeat=False):
        """A KeyRelease: the real one of the latched key ends its press."""
        live = self._press
        if not autoRepeat and live is not None and live[0][0] == key:
            self._press = None

    def clear(self):
        self._press = None


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


# -- cursors, HUD and band tint (FB-02) --------------------------------------
#
# Qt-free on purpose: the tables below are what a T0 test reads, and the
# controller maps the names onto Qt.CursorShape / painted colours.

CURSOR_ARROW = "arrow"
CURSOR_CROSS = "cross"
CURSOR_BLANK = "blank"
CURSOR_SIZE_ALL = "sizeAll"
CURSOR_CLOSED_HAND = "closedHand"

# Graph tools whose click lands a point on the scalp: a cross says "this
# pixel", which an arrow's hot spot at its tip does not.
GRAPH_CROSS_SUBMODES = frozenset(("draw", "place", "region", "reposition"))


def cursorFor(mode, subMode="", transformTool="", hoverHandle=-1,
              dragging=False, band=False, ring=False, edgeStroke=False):
    """The viewport cursor name for one (mode, sub-mode, tool, handle).

    `dragging` is a live drag that moves something (a gizmo handle, a
    Reposition CV/edge, a Place node), `band` a live box or lasso, `ring`
    Sculpt's brush ring on screen (the ring IS the cursor there, so the
    arrow is hidden under it), `edgeStroke` Hierarchy's edge-split tool.
    `hoverHandle` is the gizmo handle under an idle pointer (-1 = none);
    the Select tool has no gizmo, so it never has one.
    """
    mode = str(mode or "")
    subMode = str(subMode or "")
    if band:
        return CURSOR_CROSS
    if dragging:
        return CURSOR_CLOSED_HAND
    if mode == "graph" and subMode in GRAPH_CROSS_SUBMODES:
        return CURSOR_CROSS
    if mode == "hierarchy" and edgeStroke:
        return CURSOR_CROSS
    if mode == "sculpt":
        return CURSOR_BLANK if ring else CURSOR_CROSS
    if (mode == "tube" and str(transformTool or "") != "select" and
            int(hoverHandle) >= 0):
        return CURSOR_SIZE_ALL
    return CURSOR_ARROW


# The band's colour record, keyed by the TONIC_SELECT_* the modifier table
# gives a box/lasso (tonicLoops.selectModeFor(..., band=True)).
BAND_REPLACE = "replace"
BAND_ADD = "add"
BAND_REMOVE = "remove"
BAND_RGB = {BAND_REPLACE: (90, 190, 255),
            BAND_ADD: (90, 220, 120),
            BAND_REMOVE: (255, 96, 96)}
BAND_GLYPH = {BAND_REPLACE: "", BAND_ADD: "+", BAND_REMOVE: "−"}
BAND_FILL_ALPHA = 0.12


def bandRecordFor(modifiers):
    """'replace' / 'add' / 'remove' for a band dragged with `modifiers`."""
    mode = tonicLoops.selectModeFor(modifiers or (), band=True)
    if int(mode) == tonicLib.TONIC_SELECT_REMOVE:
        return BAND_REMOVE
    if int(mode) == tonicLib.TONIC_SELECT_SET:
        return BAND_REPLACE
    return BAND_ADD


HUD_SEPARATOR = " › "


def hudTitle(state, bound=True):
    """'Mode › Sub-mode › Tool' for the viewport HUD; '' with no mode.

    Unbound (no scalp, so every tool is gated) it names the first step
    instead of a tool that cannot act.
    """
    modeId = str(getattr(state, "activeMode", "") or "")
    mode = tonicModes.ModeById(modeId)
    if mode is None:
        return ""
    if not bound:
        return tonicModes.UNBOUND_TITLE
    parts = [mode.label]
    subId = str(getattr(state, "%sSubMode" % modeId, "") or "")
    for sub in tonicLoops.subModesFor(modeId):
        if sub.id == subId:
            parts.append(sub.label)
            break
    if modeId == "tube":
        tool = str(getattr(state, "transformTool", "") or "")
        if tool:
            parts.append(tool.title())
    return HUD_SEPARATOR.join(parts)


def hudHint(state, bound=True):
    """The HUD's bottom line: the dock's instruction line for the tool."""
    if not bound:
        return tonicModes.UNBOUND_HINT
    modeId = str(getattr(state, "activeMode", "") or "")
    subId = str(getattr(state, "%sSubMode" % modeId, "") or "")
    return tonicModes.hintFor(modeId, subId)


# How long the ladder chip stays up after a release restored fidelity.
LADDER_CHIP_LINGER_MS = 1000


def _makeHudOverlay(controller, view):
    """The in-viewport HUD: tool breadcrumb, hint line and ladder chip.

    Mouse-transparent and keyboard-free like every other overlay here; it
    paints only text the controller already resolved (`title`, `hint`,
    `chip`), so a paint never touches the model or the camera.
    """
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

    class HudOverlay(QtWidgets.QWidget):
        def __init__(self, parent):
            super(HudOverlay, self).__init__(parent)
            self.setAttribute(
                QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            self.setAttribute(QtCore.Qt.WidgetAttribute.WA_NoSystemBackground,
                              True)
            self.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
            self.setGeometry(parent.rect())
            self.title = ""
            self.hint = ""
            self.chip = ""
            self.hide()

        def text(self):
            """Everything the HUD shows, one line each (for tests)."""
            return "\n".join(line for line in (self.title, self.chip,
                                               self.hint) if line)

        def _box(self, painter, x, y, text, colour, bold=False,
                 bottom=False):
            font = QtGui.QFont(painter.font())
            font.setBold(bold)
            painter.setFont(font)
            metrics = QtGui.QFontMetrics(font)
            # A long hint on a narrow viewport elides rather than running
            # off the right edge.
            room = max(int(self.width() - x - 20), 40)
            text = metrics.elidedText(text, QtCore.Qt.TextElideMode.ElideRight,
                                      room)
            width = metrics.horizontalAdvance(text) \
                if hasattr(metrics, "horizontalAdvance") \
                else metrics.width(text)
            height = metrics.height()
            top = y - height - 6 if bottom else y
            rect = QtCore.QRectF(x, top, width + 12, height + 6)
            painter.setPen(QtCore.Qt.PenStyle.NoPen)
            painter.setBrush(QtGui.QColor(20, 20, 20, 150))
            painter.drawRoundedRect(rect, 4.0, 4.0)
            painter.setPen(colour)
            painter.drawText(rect.adjusted(6, 3, -6, -3),
                             int(QtCore.Qt.AlignmentFlag.AlignLeft |
                                 QtCore.Qt.AlignmentFlag.AlignVCenter), text)
            return rect

        def paintEvent(self, _event):
            if not (self.title or self.hint or self.chip):
                return
            painter = QtGui.QPainter(self)
            try:
                painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing,
                                      True)
                y = 8.0
                if self.title:
                    rect = self._box(painter, 8.0, y, self.title,
                                     QtGui.QColor(235, 235, 235), bold=True)
                    y = rect.bottom() + 4.0
                if self.chip:
                    # Amber: the picture is knowingly degraded right now.
                    self._box(painter, 8.0, y, self.chip,
                              QtGui.QColor(255, 190, 80))
                if self.hint:
                    self._box(painter, 8.0, self.height() - 8.0, self.hint,
                              QtGui.QColor(210, 210, 210), bottom=True)
            finally:
                painter.end()

    return HudOverlay(view)


def _makeRegionDraftOverlay(controller, view):
    """A transparent paint layer for Graph's uncommitted region contour.

    Draft points intentionally never reach the Tonic model: a close is the
    first model edit, and therefore the one undo step.  Hydra cannot draw
    state it does not own, so this small Qt child paints only the temporary
    orange CVs, their connected contour and the last-CV rubber band.  It
    projects the stored world points on each paint, which keeps the draft
    pinned to the scalp across a camera move or viewport resize.  The same
    layer draws Hierarchy's edge-split stroke, which is equally pre-model
    until Shift+D consumes it.
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

        def _paintEdges(self, edges):
            """Hierarchy's split edge: dashed, with endpoint dots.

            The recorded edge (what Shift+D will cut along) is cyan; the
            stroke still being drawn is orange like every other live draft,
            so a re-stroke reads as "replacing" the cyan one.
            """
            pairs = [(edges.get("recorded"), QtGui.QColor(0, 200, 255, 235)),
                     (edges.get("live"), QtGui.QColor(255, 145, 0, 235))]
            pairs = [(pair, colour) for pair, colour in pairs
                     if pair is not None and len(pair) == 2]
            if not pairs:
                return
            camera = tonicCamera.resolve(view)
            if camera is None:
                return
            try:
                ratio = max(float(view.devicePixelRatioF()), 1.0)
            except AttributeError:
                ratio = 1.0
            painter = QtGui.QPainter(self)
            try:
                painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing,
                                      True)
                for pair, colour in pairs:
                    a = self._logicalPoint(camera, pair[0], ratio)
                    b = self._logicalPoint(camera, pair[1], ratio)
                    if a is None or b is None:
                        continue
                    pen = QtGui.QPen(colour)
                    pen.setWidthF(2.0)
                    pen.setStyle(QtCore.Qt.PenStyle.DashLine)
                    painter.setPen(pen)
                    painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
                    painter.drawLine(a, b)
                    painter.setPen(QtGui.QPen(colour))
                    painter.setBrush(QtGui.QBrush(colour))
                    painter.drawEllipse(a, 4.0, 4.0)
                    painter.drawEllipse(b, 4.0, 4.0)
            finally:
                painter.end()

        def paintEvent(self, _event):
            self._paintEdges(controller.hierarchyEdgePreview())
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
                closeArmed = bool(preview.get("closeArmed"))
                if closeArmed:
                    # The hover sits on the first CV: draw the segment a
                    # click would author as a solid edge of the contour, so
                    # the closed shape reads before it exists.
                    painter.drawLine(projected[-1], projected[0])
                elif hoverPoint is not None:
                    rubber = QtGui.QPen(orange)
                    rubber.setWidthF(1.5)
                    rubber.setStyle(QtCore.Qt.PenStyle.DashLine)
                    painter.setPen(rubber)
                    painter.drawLine(projected[-1], hoverPoint)
                # The first CV has a ring so its role as the close target is
                # legible before the contour gains its third point; it grows
                # and thickens while a click there would close.
                firstPen = QtGui.QPen(QtGui.QColor(255, 215, 120, 250))
                firstPen.setWidthF(3.0 if closeArmed else 2.0)
                painter.setPen(firstPen)
                painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
                ring = 10.0 if closeArmed else 6.5
                painter.drawEllipse(projected[0], ring, ring)
                painter.setPen(QtGui.QPen(orange))
                painter.setBrush(QtGui.QBrush(orange))
                for point in projected:
                    painter.drawEllipse(point, 3.5, 3.5)
            finally:
                painter.end()

    return RegionDraftOverlay(view)


def _makeMarqueeOverlay(view):
    """A painted, mouse-transparent selection rectangle over StageView.

    A QRubberBand drew the same platform rectangle for replace, add and
    remove; this one is a 1 px border over a 12 % fill in the colour of
    the modifier the band will apply (`colourRecord`: replace/add/remove,
    BAND_RGB) with a '+'/'−' glyph by the cursor corner (FB-02).  It covers
    the view and paints `band` (a logical QRect) so the glyph may sit
    outside the rectangle itself.
    """
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

    class MarqueeOverlay(QtWidgets.QWidget):
        def __init__(self, parent):
            super(MarqueeOverlay, self).__init__(parent)
            self.setAttribute(
                QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            self.setAttribute(QtCore.Qt.WidgetAttribute.WA_NoSystemBackground,
                              True)
            self.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
            self.setGeometry(parent.rect())
            self.band = None             # logical QRect, normalised
            self.cursorPoint = None      # logical QPoint of the drag end
            self.colourRecord = BAND_REPLACE
            self.hide()

        def paintEvent(self, _event):
            rect = self.band
            if rect is None:
                return
            rgb = BAND_RGB.get(self.colourRecord, BAND_RGB[BAND_REPLACE])
            painter = QtGui.QPainter(self)
            try:
                pen = QtGui.QPen(QtGui.QColor(rgb[0], rgb[1], rgb[2], 235))
                pen.setWidthF(1.0)
                painter.setPen(pen)
                painter.setBrush(QtGui.QColor(
                    rgb[0], rgb[1], rgb[2], int(round(255 * BAND_FILL_ALPHA))))
                painter.drawRect(QtCore.QRectF(rect).adjusted(0.5, 0.5,
                                                              -0.5, -0.5))
                glyph = BAND_GLYPH.get(self.colourRecord, "")
                corner = self.cursorPoint
                if glyph and corner is not None:
                    font = QtGui.QFont(painter.font())
                    font.setBold(True)
                    painter.setFont(font)
                    painter.setPen(QtGui.QColor(rgb[0], rgb[1], rgb[2], 255))
                    painter.drawText(QtCore.QPointF(corner.x() + 10.0,
                                                    corner.y() + 18.0),
                                     glyph)
            finally:
                painter.end()

    return MarqueeOverlay(view)


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
                # The same add/remove tint as the box (FB-02).
                rgb = BAND_RGB.get(controller.bandRecord(),
                                   BAND_RGB[BAND_REPLACE])
                pen = QtGui.QPen(QtGui.QColor(rgb[0], rgb[1], rgb[2], 235))
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

        # The drawing is RigExec's GizmoOverlay (gizmoUI.py _Draw*), port
        # for port: same shapes, opacities and draw order.  Records arrive
        # in PHYSICAL pixels; every size below is divided by the ratio once,
        # so LINE_WIDTH and the tip sizes stay LOGICAL on a HiDPI display.

        @staticmethod
        def _color(rgb, opacity=1.0):
            return QtGui.QColor(
                int(round(rgb[0] * 255)), int(round(rgb[1] * 255)),
                int(round(rgb[2] * 255)),
                int(round(max(0.0, min(1.0, opacity)) * 255)))

        @staticmethod
        def _point(point, ratio):
            return QtCore.QPointF(point[0] / ratio, point[1] / ratio)

        def _polygon(self, points, ratio):
            return QtGui.QPolygonF([self._point(p, ratio) for p in points])

        def _recordColor(self, record, opacity=1.0):
            # GizmoState already resolved the state colour; an ungrabbable
            # handle arrives with LOCKED_OPACITY and is dimmed here.
            return self._color(record["color"],
                               record.get("opacity", 1.0) * opacity)

        def _pen(self, record):
            pen = QtGui.QPen(self._recordColor(record))
            pen.setWidthF(tonicGizmo.LINE_WIDTH)
            pen.setCapStyle(QtCore.Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(QtCore.Qt.PenJoinStyle.RoundJoin)
            return pen

        def paintEvent(self, _event):
            records, pie = controller.gizmoPaint()
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
                # Draw order is hit order (RigExec paintEvent): the free
                # ball under the rings, the rings, the rotation wedge, then
                # axes, planes and the centre on top -- what looks on top
                # is what a press picks.
                for record in records:
                    if record["kind"] == "free":
                        self._drawSphere(painter, record, ratio)
                for record in records:
                    if record["kind"] in ("ring", "view"):
                        self._drawRing(painter, record, ratio)
                self._drawPie(painter, pie, ratio)
                for record in records:
                    if record["kind"] == "axis":
                        tip = record.get("tip")
                        if tip == "cube":
                            self._drawScaleAxis(painter, record, ratio)
                        elif tip == "cone":
                            self._drawArrow(painter, record, ratio)
                        else:
                            self._drawLine(painter, record, ratio)
                for record in records:
                    if record["kind"] == "plane":
                        self._drawPlane(painter, record, ratio)
                for record in records:
                    if record["kind"] == "center":
                        self._drawCenter(painter, record, ratio)
                self._drawReadout(painter, ratio)
            finally:
                painter.end()

        def _drawReadout(self, painter, ratio):
            """GZ-07: the live drag value in a dark box beside the centre.

            `readoutText` keeps what was last painted ("" when nothing),
            so a T3 can see the label without reading pixels.
            """
            readout = controller.gizmoReadout()
            self.readoutText = readout[0] if readout else ""
            if not readout:
                return
            text, anchor = readout
            font = QtGui.QFont(painter.font())
            font.setBold(True)
            painter.setFont(font)
            metrics = QtGui.QFontMetricsF(font)
            pad = 4.0
            width = metrics.horizontalAdvance(text) \
                if hasattr(metrics, "horizontalAdvance") \
                else metrics.width(text)
            # Up and to the right of the pivot, clear of the centre square
            # and the arrows' first stretch, kept inside the viewport.
            x = anchor[0] / ratio + 16.0
            y = anchor[1] / ratio - 16.0 - metrics.height()
            box = QtCore.QRectF(x, y, width + 2.0 * pad,
                                metrics.height() + 2.0 * pad)
            if box.right() > self.width():
                box.moveRight(float(self.width()) - 2.0)
            if box.top() < 0.0:
                box.moveTop(2.0)
            painter.setPen(QtCore.Qt.PenStyle.NoPen)
            painter.setBrush(QtGui.QBrush(QtGui.QColor(20, 20, 24, 200)))
            painter.drawRoundedRect(box, 3.0, 3.0)
            painter.setPen(QtGui.QColor(235, 235, 235))
            painter.drawText(box, QtCore.Qt.AlignmentFlag.AlignCenter, text)

        def _drawLine(self, painter, record, ratio):
            painter.setPen(self._pen(record))
            painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
            painter.drawLine(self._point(record["points"][0], ratio),
                             self._point(record["points"][-1], ratio))

        def _drawArrow(self, painter, record, ratio):
            """A move axis: a line stopping short of a filled cone tip."""
            import math
            start = self._point(record["points"][0], ratio)
            end = self._point(record["points"][-1], ratio)
            dx, dy = end.x() - start.x(), end.y() - start.y()
            length = math.hypot(dx, dy)
            radius = tonicGizmo.CONE_RADIUS * record["sizePx"] / ratio
            coneLength = min(radius * tonicGizmo.CONE_LENGTH_RATIO,
                             length * 0.9)
            painter.setPen(self._pen(record))
            painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
            if length < 1e-6:
                return
            ux, uy = dx / length, dy / length
            base = QtCore.QPointF(end.x() - ux * coneLength,
                                  end.y() - uy * coneLength)
            painter.drawLine(start, base)
            painter.setBrush(QtGui.QBrush(self._recordColor(record)))
            painter.setPen(QtCore.Qt.PenStyle.NoPen)
            painter.drawPolygon(QtGui.QPolygonF([
                end,
                QtCore.QPointF(base.x() - uy * radius, base.y() + ux * radius),
                QtCore.QPointF(base.x() + uy * radius,
                               base.y() - ux * radius)]))

        def _drawScaleAxis(self, painter, record, ratio):
            """A scale axis: a line ending in a filled cube (a square)."""
            start = self._point(record["points"][0], ratio)
            end = self._point(record["points"][-1], ratio)
            side = tonicGizmo.CUBE_SIDE * record["sizePx"] / ratio
            painter.setPen(self._pen(record))
            painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
            painter.drawLine(start, end)
            painter.setBrush(QtGui.QBrush(self._recordColor(record)))
            painter.setPen(QtCore.Qt.PenStyle.NoPen)
            painter.drawRect(QtCore.QRectF(end.x() - side * 0.5,
                                           end.y() - side * 0.5, side, side))

        def _drawPlane(self, painter, record, ratio):
            painter.setBrush(QtGui.QBrush(self._recordColor(
                record, record.get("fillAlpha",
                                   tonicGizmo.PLANE_FILL_OPACITY))))
            painter.setPen(self._pen(record))
            painter.drawPolygon(self._polygon(record["points"], ratio))

        def _drawCenter(self, painter, record, ratio):
            point = self._point(record["points"][0], ratio)
            side = tonicGizmo.CENTER_SIDE * record["sizePx"] / ratio
            painter.setBrush(QtGui.QBrush(self._recordColor(record)))
            painter.setPen(QtCore.Qt.PenStyle.NoPen)
            painter.drawRect(QtCore.QRectF(point.x() - side * 0.5,
                                           point.y() - side * 0.5,
                                           side, side))

        def _drawRing(self, painter, record, ratio):
            # Only the camera-side runs, which are also all HitTest picks;
            # a fully visible run repeats its first point, so the seam is
            # closed without a chord across the manipulator.
            painter.setBrush(QtCore.Qt.BrushStyle.NoBrush)
            painter.setPen(self._pen(record))
            for run in record.get("frontPoints") or [record["points"]]:
                if len(run) > 1:
                    painter.drawPolyline(self._polygon(run, ratio))

        def _drawSphere(self, painter, record, ratio):
            # The ball keeps its own grey whatever the state (RigExec): it
            # is a wash behind the rings, not a handle that lights up.
            point = self._point(record["points"][0], ratio)
            radius = float(record.get("radiusPx", 0.0)) / ratio
            painter.setBrush(QtGui.QBrush(self._color(
                tonicGizmo.SPHERE_COLOR,
                record.get("fillAlpha", tonicGizmo.SPHERE_FILL_OPACITY))))
            pen = QtGui.QPen(self._color(tonicGizmo.SPHERE_COLOR,
                                         tonicGizmo.SPHERE_OPACITY))
            pen.setWidthF(tonicGizmo.LINE_WIDTH)
            painter.setPen(pen)
            painter.drawEllipse(point, radius, radius)

        def _drawPie(self, painter, pie, ratio):
            """The rotation-amount wedge from the press to the sweep."""
            if not pie:
                return
            polygon, color = pie
            if len(polygon) < 3:
                return
            painter.setBrush(QtGui.QBrush(self._color(
                color, tonicGizmo.PIE_OPACITY)))
            painter.setPen(QtCore.Qt.PenStyle.NoPen)
            painter.drawPolygon(self._polygon(polygon, ratio))

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
        self._loopModel = None        # the model self._loop was built on
        self._camera = None
        self._gesture = False
        self._brushResizeArmed = False
        self._brushResizeActive = False
        # Whether the current F hold started a width drag: releasing F
        # without one is a tap, which frames instead (FB-03).
        self._brushResizeUsed = False
        # A host-application MMB camera drag Tonic drives itself:
        # [mode, lastX, lastY] in physical pixels, or None (FB-03).
        self._navDrag = None
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
        # FB-02: the in-viewport HUD, the cursor Tonic last set (a
        # tonicViewport.CURSOR_* name, None = usdview's own) and the cursor
        # the view had before install, which uninstall/suspend restore.
        self._hudOverlay = None
        self._cursorName = None
        self._cursorWas = None
        # The modifiers of the live band's last event (its colour record)
        # and the ladder chip lingering after a release: (text, until s).
        self._bandModifiers = frozenset()
        self._ladderChipLinger = None
        # (loop, gizmo paint signature, records, pie): what the gizmo
        # overlay paints from, rebuilt on camera/gesture/selection events
        # rather than on every paint (parity G22).
        self._gizmoPaintCache = None
        self._rolloverWas = None
        self._focusPolicyWas = None
        # Who received the last press: "tonic" (claimed, even as a miss) or
        # "stageview" (the camera's, or usdview's own pick).  An idle move
        # with a button held is claimed or passed on by this owner, and its
        # release re-runs one hover (FB-01).
        self._pressOwner = None
        # The last press-less pointer sample, for the hover that re-runs
        # after a camera move: (x, y, modifiers) in physical pixels.
        self._lastPointer = None
        self._rehoverPending = False
        # Dock hidden: the view filter is off and usdview owns the view, but
        # the session and its model stay live (FB-01 suspend/resume).
        self._suspended = False
        # The fallback ladder (plan/18 section 3.7): armed at press, fed
        # every move's measured time, restored at release.
        self._ladder = tonicLadder.FallbackLadder(session, state)
        # Parity G11: the snap holds (J = step snap, X = world grid) held
        # during a gizmo drag, as Sample modifier names.  Claimed only while
        # a drag is live, cleared when it ends or the window deactivates.
        self._holds = set()
        # One run of onKey per physical press, however many widgets the
        # declined KeyPress propagates through (usdRig _claimedKey).
        self._keyLatch = KeyPressLatch()
        # Whether the live gesture has had a forwarded move: a hold is
        # re-applied at once only to a drag that has actually moved, so a
        # tweak press held still stays a click.
        self._gestureMoved = False
        # The GizmoSettings listener while installed (dock rows, +/-, L).
        self._settingsListening = None

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
        # Through the session, like every loop's line, so the dock's
        # message area (the session's status sink, DK-05) sees the
        # controller's refusals too; the session falls back to usdview's
        # status bar when no sink is installed.
        report = getattr(self._session, "report", None)
        if report is not None:
            report(text)
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
        from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
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
        # usdview's StageView takes no keyboard focus, so once a dock field
        # had it, clicking the viewport left it there and every hotkey read
        # as typing.  ClickFocus lets a click hand the keys back to the view.
        try:
            self._focusPolicyWas = view.focusPolicy()
            view.setFocusPolicy(QtCore.Qt.FocusPolicy.ClickFocus)
        except (AttributeError, RuntimeError):
            self._focusPolicyWas = None
        self._suspended = False
        self._viewFilter = _ViewFilter(self, view)
        view.installEventFilter(self._viewFilter)
        self._regionOverlay = _makeRegionDraftOverlay(self, view)
        self._marqueeOverlay = _makeMarqueeOverlay(view)
        self._lassoOverlay = _makeLassoOverlay(self, view)
        self._gizmoOverlay = _makeGizmoOverlay(self, view)
        self._hudOverlay = _makeHudOverlay(self, view)
        # The view's own cursor (usually none: StageView inherits the
        # arrow), so uninstall hands back exactly what it found.
        try:
            self._cursorWas = (QtGui.QCursor(view.cursor())
                               if view.testAttribute(
                                   QtCore.Qt.WidgetAttribute.WA_SetCursor)
                               else None)
        except (AttributeError, RuntimeError):
            self._cursorWas = None
        self._cursorName = None
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
        # A gizmo setting (dock row, +/-, L, Reset) re-places the gizmo at
        # once through the live camera (parity G20).
        settings = tonicGizmoSettings.settingsFor(self._state)
        settings.AddListener(self._onGizmoSettings)
        self._settingsListening = settings
        # workspaceOpen belongs to the dock, which sets it from its own
        # visibility (plan/18 section 3.4: the hotkeys live while the
        # workspace is open, not merely while the filter is installed).
        self._connectStageSignals()
        self._connectFrustumSignal()
        self.setWorkspaceActive(bool(self._state.workspaceOpen))
        self.syncDisplayScale()
        if not self._state.activeMode:
            self.setMode(tonicModes.MODES[0].id)
        self._syncHud()
        self._syncCursor()
        if getattr(self._session, "hasPendingWork", lambda: False)():
            self.scheduleIdle()
        # Installed with no workspace on screen (usdGen > Tonic > Bind
        # selected as scalp before Open workspace) is the same as a hidden
        # dock: the filter would otherwise claim every left click as a
        # Graph region press and forced tracking would make StageView run
        # a Hydra pick per hover.  The dock's first show resumes.
        if not self._state.workspaceOpen:
            self.suspend()
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
        if self._view is not None and self._focusPolicyWas is not None:
            try:
                self._view.setFocusPolicy(self._focusPolicyWas)
            except (AttributeError, RuntimeError):
                pass
        self._focusPolicyWas = None
        self._disconnectFrustumSignal()
        self._disconnectStageSignals()
        if self._settingsListening is not None:
            self._settingsListening.RemoveListener(self._onGizmoSettings)
            self._settingsListening = None
        self._holds.clear()
        self._keyLatch.clear()
        self._pressOwner = None
        self._lastPointer = None
        self._suspended = False
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
        if self._hudOverlay is not None:
            self._hudOverlay.hide()
            self._hudOverlay.deleteLater()
            self._hudOverlay = None
        self._restoreCursor()
        self._cursorWas = None
        self._ladderChipLinger = None
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
        # The HUD and the tool cursor exist only while the tool is on
        # screen; a closed dock gives usdview its own arrow back (FB-02).
        if active:
            self._syncHud()
            self._syncCursor()
        else:
            if self._hudOverlay is not None:
                self._hudOverlay.hide()
            self._restoreCursor()
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
        if getattr(self, "_stageSignalConnected", False):
            return
        try:
            api.dataModel.signalStageReplaced.connect(self._onStageReplaced)
            self._stageSignalConnected = True
        except (AttributeError, RuntimeError):
            pass

    def _disconnectStageSignals(self):
        # Qt does not de-duplicate connections: without this every
        # uninstall/install cycle (a replaced StageView, a re-bind after
        # shutdown) ran _onStageReplaced once more per File > Reopen.
        api = self._api
        if api is None or not getattr(self, "_stageSignalConnected", False):
            return
        self._stageSignalConnected = False
        try:
            api.dataModel.signalStageReplaced.disconnect(
                self._onStageReplaced)
        except (AttributeError, RuntimeError, TypeError):
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
            view.signalFrustumChanged.connect(self._onFrustumChanged)
        except (AttributeError, RuntimeError):
            pass

    def _disconnectFrustumSignal(self):
        view = self._view
        if view is None:
            return
        try:
            view.signalFrustumChanged.disconnect(self._onFrustumChanged)
        except (AttributeError, RuntimeError, TypeError):
            pass

    def _onFrustumChanged(self):
        self.syncDisplayScale()
        # What sits under a still cursor changed with the camera.  The hover
        # waits for the event loop: this signal fires inside StageView's
        # paintGL, which must not be asked to render again from within.
        self._scheduleRehover()

    def _scheduleRehover(self):
        """Re-run one hover at the last pointer sample, once, deferred."""
        if self._rehoverPending or not self._installed or self._suspended:
            return
        try:
            from pxr.Usdviewq.qt import QtCore
            self._rehoverPending = True
            QtCore.QTimer.singleShot(0, self._rehover)
        except (AttributeError, ImportError, RuntimeError):
            self._rehoverPending = False

    def _rehover(self):
        self._rehoverPending = False
        # A camera drag re-renders every move by itself; its release asks
        # again once the button is up.
        if (not self._installed or self._suspended or self._gesture or
                self._pressOwner is not None or not self._pointerInside or
                not self._state.workspaceOpen or not self._ready() or
                self._lastPointer is None or self._view is None):
            return
        x, y, modifiers = self._lastPointer
        if "alt" in modifiers or "meta" in modifiers:
            return
        self._lastHoverXY = None        # same pixel, new camera: evaluate
        try:
            self._hoverAt(self._view, x, y, modifiers)
        except Exception as exc:        # noqa: BLE001 - timer boundary
            self._status("Tonic viewport hover: %s" % exc)

    def suspend(self):
        """The dock was hidden: give the StageView back to usdview.

        The view filter comes off (a click picks prims again, a hover rolls
        over), tracking and rollover return to usdview's settings and the
        idle pump stops, but the session, its model and the key filter
        stay, so showing the dock resumes exactly where the artist was.
        """
        if not self._installed or self._suspended:
            return
        self._recoverGesture("workspace hidden")
        self._clearHover()
        self._suspended = True
        self._pressOwner = None
        view = self._view
        if view is not None and self._viewFilter is not None:
            view.removeEventFilter(self._viewFilter)
        self.setWorkspaceActive(False)
        if view is not None and self._trackingWas is not None:
            try:
                view.setMouseTracking(self._trackingWas)
            except (AttributeError, RuntimeError):
                pass
        # usdview's StageView is NoFocus: with the dock hidden a click on it
        # must not pull the keys off usdview's own search field.
        if view is not None and self._focusPolicyWas is not None:
            try:
                view.setFocusPolicy(self._focusPolicyWas)
            except (AttributeError, RuntimeError):
                pass
        self.stopIdle()
        for overlay in (self._regionOverlay, self._marqueeOverlay,
                        self._lassoOverlay, self._gizmoOverlay,
                        self._hudOverlay):
            if overlay is not None:
                overlay.hide()
        self._pointerInside = False
        self._refresh()

    def resume(self):
        """The dock is visible again: retake the view, focus included."""
        if not self._installed:
            return
        view = self._view
        if self._suspended:
            self._suspended = False
            try:
                self._trackingWas = bool(view.hasMouseTracking())
            except (AttributeError, RuntimeError):
                self._trackingWas = None
            try:
                from pxr.Usdviewq.qt import QtCore
                self._focusPolicyWas = view.focusPolicy()
                view.setFocusPolicy(QtCore.Qt.FocusPolicy.ClickFocus)
            except (AttributeError, ImportError, RuntimeError):
                self._focusPolicyWas = None
            if view is not None and self._viewFilter is not None:
                view.installEventFilter(self._viewFilter)
            self.syncDisplayScale()
            self._syncRegionOverlay()
            self._syncMarqueeOverlay()
            self._syncGizmoOverlay()
            if getattr(self._session, "hasPendingWork", lambda: False)():
                self.scheduleIdle()
        self.setWorkspaceActive(True)
        # Opening or re-showing the workspace must never leave the keys on
        # a dock widget; the viewport is where the artist works.
        self._takeViewFocus(view)
        self._refresh()

    def _takeViewFocus(self, view=None):
        view = view if view is not None else self._view
        if view is None:
            return
        try:
            view.setFocus()
        except (AttributeError, RuntimeError):
            pass

    def viewHasKeys(self):
        """Viewport keys are live: the pointer is over it or it has focus."""
        return self._pointerInside or self.viewHasFocus()

    @property
    def suspended(self):
        return self._suspended

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
        if (self._state.activeMode and
                (self._session is None or self._session.model is None)):
            # Nothing is bound, so every mode but the initial one would be a
            # loop over no model: pressing 2 used to enter Tube silently and
            # leave the artist in a tool that cannot do anything. install()
            # still runs its first setMode (activeMode is empty then).
            self._status("Tonic: bind a scalp mesh first")
            return ""
        if mode.id == self._state.activeMode and self._loop is not None and \
                getattr(self, "_loopModel", None) is getattr(
                    self._session, "model", None):
            # GZ-08: picking the mode already current (its hotkey, its shelf
            # button) is a no-op.  Rebuilding the loop threw away its gizmo,
            # Graph's region draft and the brush ring for nothing.  A bind
            # or resume (a different model) still rebuilds: the old loop's
            # drafts belong to the old model.
            status = tonicModes.SetActiveMode(self._state, mode.id)
            self._status(status)
            return status
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
        self._loopModel = getattr(self._session, "model", None)
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
        # GZ-08: a Tube loop entered with a selection shows its gizmo now,
        # sized for the live camera (activate() had none to size it with).
        self.refreshGizmo()
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
        # Every return False below hands the press, and so its drag, to
        # StageView; the claimed paths retake it.
        self._pressOwner = "stageview"
        if self._installed and self._state.workspaceOpen:
            self._takeViewFocus(view)
        navigation = self._navigationPress(view, event)
        if navigation is not None:
            return navigation
        if not self._ready():
            return False
        # A plain middle press repeats the last-dragged gizmo handle from
        # anywhere (RigExec/a DCC, parity G04) in a loop that offers it;
        # otherwise only the left button is Tonic's.
        middleButton = getattr(QtCore.Qt.MouseButton, "MiddleButton", None)
        middle = (middleButton is not None and
                  event.button() == middleButton and
                  callable(getattr(self._loop, "middlePress", None)))
        if event.button() != QtCore.Qt.MouseButton.LeftButton and \
                not middle:
            return False
        if middle and self._gesture:
            self._pressOwner = "tonic"
            return True                  # a live left drag keeps going
        # A missing MouseButtonRelease (focus change, native modal dialog,
        # or a view replacement) must never consume the next real click.
        if self._gesture:
            self._recoverGesture("new press after lost release")
        modifiers = modifierSet(event)
        if "alt" in modifiers or "meta" in modifiers:
            return False                 # the camera's, always
        # A band this press may start is tinted by these (FB-02).
        self._bandModifiers = modifiers
        if middle:
            modifiers = modifiers | frozenset(("middle",))
        x, y = eventPixels(view, event)
        self._camera = tonicCamera.resolve(view)
        if self._camera is None:
            return False
        self.syncDisplayScale(self._camera)
        sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                   modifiers)
        if not middle and self._brushResizeArmed and self._canBrushResize():
            begin = getattr(self._loop, "beginRadiusResize", None)
            if begin is not None and begin(sample):
                self._brushResizeActive = True
                self._brushResizeUsed = True
                self._gesture = True
                self._lastXY = (x, y)
                self._syncRegionOverlay()
                self._syncMarqueeOverlay()
                self._syncGizmoOverlay()
                self._refresh()
                self._pressOwner = "tonic"
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
            if not self._state.workspaceOpen:
                return False
            # A Tonic miss (Sculpt off every tube, an empty Fill click, a
            # middle press with no handle to repeat) is still a Tonic
            # click: handing it on made StageView pick and replace the
            # usdview prim selection under the artist.
            self._pressOwner = "tonic"
            self._syncGizmoOverlay()
            self._refresh()
            return True
        self._syncActiveCutBreadcrumb()
        self._gesture = True
        self._gestureMoved = False
        # The button that owns the capture: a middle repeat drag is held
        # by the middle button, and its moves/release must read that one.
        self._gestureButton = event.button()
        self._pressOwner = "tonic"
        self._lastXY = (x, y)
        self._ladder.arm(self._state.activeLevel)
        self._session.publish()
        self._syncRegionOverlay()
        self._syncMarqueeOverlay()
        self._syncGizmoOverlay()
        self._refresh()
        return True

    # -- navigation (FB-03) ------------------------------------------------

    def _navigationPress(self, view, event):
        """A middle/right press over the open workspace; None passes on.

        StageView turns every non-Alt press into `pickObject`: a middle
        press re-picks under the groom and a right press opens usdview's
        prim context menu, neither of which an artist working on hair
        meant.  Alt/Meta presses stay usdview's camera in both styles.
        """
        from pxr.Usdviewq.qt import QtCore
        buttons = QtCore.Qt.MouseButton
        button = event.button()
        middle = button == getattr(buttons, "MiddleButton", None)
        right = button == buttons.RightButton
        if not (middle or right):
            return None
        if not (self._installed and self._state.workspaceOpen):
            return None
        modifiers = modifierSet(event)
        if "alt" in modifiers or "meta" in modifiers:
            return None
        if self._gesture:
            # A second button under a live left drag must not move the
            # camera its press-time projection was measured with.
            self._pressOwner = "tonic"
            return True
        style = str(getattr(self._state, "navigationStyle", "maya")).lower()
        if middle and style == "blender":
            mode = ("truck" if "shift" in modifiers else
                    "zoom" if "ctrl" in modifiers else "tumble")
            camera = self._freeCamera(view, switch=True)
            if camera is not None:
                self._seatPivot(camera, view)
                x, y = eventPixels(view, event)
                self._navDrag = [mode, x, y]
        elif middle and self._ready() and self._middleRepeats(view):
            return None                  # Maya: the G04 handle repeat
        self._pressOwner = "tonic"
        return True

    def _middleRepeats(self, view=None):
        """Whether a plain host-application middle press repeats a gizmo handle.

        host-application MMB is the camera, always: an orbit that turned into
        a handle drag whenever a tube was selected would be unusable.  The
        loop is asked at this press's camera, so a remembered handle the
        overlay now dims as ungrabbable leaves the press to usdview.
        """
        middlePress = getattr(self._loop, "middlePress", None)
        if not callable(middlePress):
            return False
        available = getattr(self._loop, "middleRepeatAvailable", None)
        if not callable(available):
            return True                  # the loop's press decides
        try:
            camera = tonicCamera.resolve(view) if view is not None else None
            return bool(available(camera))
        except Exception:               # noqa: BLE001 - GUI boundary
            return False

    @staticmethod
    def _viewSettings(view, api=None):
        dataModel = getattr(view, "_dataModel", None)
        if dataModel is None and api is not None:
            dataModel = getattr(api, "dataModel", None)
        return getattr(dataModel, "viewSettings", None)

    def _freeCamera(self, view, switch=False):
        """usdview's free camera, taken over from a scene camera first."""
        view = view if view is not None else self._view
        if view is None:
            return None
        if switch:
            # StageView does the same before its own Alt drags and framing:
            # a scene camera prim is never edited by navigation.
            toFree = getattr(view, "switchToFreeCamera", None)
            if callable(toFree):
                try:
                    toFree()
                except (AttributeError, RuntimeError):
                    return None
        settings = self._viewSettings(view, self._api)
        return getattr(settings, "freeCamera", None)

    def _navigationMove(self, view, event):
        """One host-application MMB drag step on usdview's free camera."""
        mode, lastX, lastY = self._navDrag
        x, y = eventPixels(view, event)
        dx, dy = x - lastX, y - lastY
        if dx == 0 and dy == 0:
            return True
        camera = self._freeCamera(view)
        if camera is None:
            return True
        # StageView.mouseMoveEvent's own factors, so a DCC drag moves
        # exactly as far as usdview's Alt drag over the same pixels.
        if mode == "tumble":
            camera.Tumble(0.25 * dx, 0.25 * dy)
        elif mode == "zoom":
            zoomDelta = -0.002 * (dx + dy)
            if camera.orthographic:
                camera.fov *= (1 + zoomDelta)
            else:
                camera.AdjustDistance(1 + zoomDelta)
        else:
            height = float(view.GetPhysicalWindowSize()[1])
            perPixel = camera.ComputePixelsToWorldFactor(height)
            camera.Truck(-dx * perPixel, dy * perPixel)
        self._navDrag = [mode, x, y]
        view.updateGL()
        return True

    def frameBounds(self):
        """World (min, max) that F frames, or None when Tonic has nothing.

        The model's selection bounds cover every selected kind (graph nodes
        and edges, CVs, rings, section CVs), but a whole selected tube
        counts only as its centroid there, so its displayed centre line is
        added to frame the tube itself.  Sculpt has no selection of its
        own, so with nothing selected it frames the whole groom.
        """
        session = self._session
        if session is None or session.model is None:
            return None
        points = []
        dll, model = session.dll, session.model
        lo = (ctypes.c_float * 3)()
        hi = (ctypes.c_float * 3)()
        try:
            if dll.Tonic_GetSelectionBounds(model, lo, hi) == tonicLib.TONIC_OK:
                points.append((lo[0], lo[1], lo[2]))
                points.append((hi[0], hi[1], hi[2]))
            tubes = [item[0] for item in
                     session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)]
            if (not points and self._state.activeMode == "sculpt" and
                    self._loop is not None):
                tubes = tonicBridge.readTubeIds(dll, model)
            for tubeId in tubes:
                points.extend(tonicHierarchy.tubeCenterHandles(dll, model,
                                                               tubeId))
        except (AttributeError, RuntimeError, NotImplementedError) as exc:
            self._status("Tonic frame: %s" % exc)
        if not points:
            return None
        return (tuple(min(p[a] for p in points) for a in range(3)),
                tuple(max(p[a] for p in points) for a in range(3)))

    def frameSelection(self):
        """Frame the Tonic selection in usdview's free camera; True if so.

        A selection too small to fill any of the view (one CV, a ring seen
        edge on) is centred at the current distance rather than zoomed onto,
        so repeated F never dives through the groom.
        """
        from pxr import Gf
        view = self._view
        if view is None:
            return False
        bounds = self.frameBounds()
        if bounds is None:
            return False
        lo, hi = bounds
        centre = tuple(0.5 * (lo[a] + hi[a]) for a in range(3))
        size = max(hi[a] - lo[a] for a in range(3))
        pixelSize = 0.0
        current = tonicCamera.resolve(view)
        if current is not None:
            try:
                pixelSize = size / max(current.worldPerPixel(centre), 1e-12)
            except (AttributeError, RuntimeError, ValueError):
                pixelSize = 0.0
        camera = self._freeCamera(view, switch=True)
        if camera is None:
            return False
        if pixelSize < FRAME_MIN_PIXELS:
            self._recentre(camera, view, centre)
        else:
            box = Gf.BBox3d(Gf.Range3d(Gf.Vec3d(*lo), Gf.Vec3d(*hi)))
            camera.frameSelection(box, FRAME_FIT)
        settings = self._viewSettings(view, self._api)
        if getattr(settings, "autoComputeClippingPlanes", False):
            closest = getattr(view, "computeAndSetClosestDistance", None)
            if callable(closest):
                closest()
        view.updateGL()
        self._status("Tonic: framed the %s" % (
            "selection" if self._session.selectionCount(0) else "groom"))
        self._scheduleRehover()
        return True

    @staticmethod
    def _viewAxis(view):
        """The unit world view direction through the viewport centre."""
        camera = tonicCamera.resolve(view)
        if camera is None:
            return None
        ray = camera.rayThrough(0.5 * camera.width, 0.5 * camera.height)
        return None if ray is None else ray[1]

    def _recentre(self, camera, view, point):
        """Slide the free camera sideways until `point` is on its axis.

        The depth to the point is kept, so it keeps its on-screen size; a
        bare `center` assignment would instead keep the old orbit distance,
        which is 0 for a free camera taken over from a plain camera prim --
        the eye would land on the point itself.
        """
        from pxr import Gf
        axis = self._viewAxis(view)
        target = Gf.Vec3d(*point)
        if axis is None:
            camera.center = target
            return
        axis = Gf.Vec3d(*axis)
        eye = camera.center - camera.dist * axis
        depth = Gf.Dot(target - eye, axis)
        if depth <= 1e-6:
            depth = (target - eye).GetLength()
        camera.center = target
        if depth > 1e-6:
            camera.dist = depth

    def _seatPivot(self, camera, view):
        """Give a pivot-less free camera a pivot at the groom's depth.

        FreeCamera.FromGfCamera takes its orbit distance from the scene
        camera's focusDistance, which a plain camera prim leaves at 0: the
        pivot is then the eye, so an orbit only looks around and a pan
        (world units per pixel scale with that distance) never moves.  The
        eye stays exactly where it is; only the pivot moves out to the
        scalp's depth along the view axis.
        """
        from pxr import Gf
        try:
            if float(camera.dist) > 1e-6:
                return
        except (AttributeError, TypeError, ValueError):
            return
        centre = getattr(self._session, "scalpCenter", None) \
            if self._session is not None else None
        axis = self._viewAxis(view)
        if centre is None or axis is None:
            return
        axis = Gf.Vec3d(*axis)
        eye = Gf.Vec3d(camera.center)    # dist 0: the centre is the eye
        depth = Gf.Dot(Gf.Vec3d(*centre) - eye, axis)
        if depth <= 1e-6:
            return
        camera.center = eye + depth * axis
        camera.dist = depth

    def hasFrameTarget(self):
        """Whether F is Tonic's (it frames usdview's prims otherwise)."""
        if not (self._installed and self._state.workspaceOpen and
                self._ready()):
            return False
        return self.frameBounds() is not None

    def onMove(self, view, event):
        x, y = eventPixels(view, event)
        self._pointerInside = True
        if self._navDrag is not None:
            if self._buttonsHeld(event):
                return self._navigationMove(view, event)
            self._navDrag = None         # its release was lost
            self._pressOwner = None
        if not self._ready():
            # Nothing is bound, so there is no Tonic hover, but the forced
            # mouse tracking would still turn every press-less move into a
            # StageView rollover pick and prim tooltip.  A press Tonic
            # swallowed (a middle/right click) keeps its drag too.
            if self._pressOwner == "tonic" and self._buttonsHeld(event):
                return True
            return (bool(self._state.workspaceOpen) and
                    not self._buttonsHeld(event))
        if not self._gesture:
            return self._onHover(view, event, x, y)
        if not self._leftButtonHeld(event):
            # Qt keeps sending hover moves after a lost capture.  Treat the
            # first one with no left button as the missing release, then let
            # it reacquire an idle hover target instead of wedging the tool.
            self._recoverGesture("move without left button")
            return self._onHover(view, event, x, y)
        if self._lastXY is not None:
            # x, y are physical pixels (eventPixels); the slop is logical.
            if (abs(x - self._lastXY[0]) + abs(y - self._lastXY[1]) <
                    tonicGizmo.clickSlopPixels(self._camera)):
                return True
        self._lastXY = (x, y)
        self._gestureMoved = True
        started = time.perf_counter()
        # Pressing or letting go of Shift/Ctrl mid-band re-tints it (FB-02).
        self._bandModifiers = modifierSet(event)
        sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                   self._bandModifiers | self._holdModifiers())
        if self._brushResizeActive:
            resize = getattr(self._loop, "resizeRadius", None)
            if resize is not None:
                resize(sample)
        else:
            self._loop.move(sample)
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
            self._syncHud()             # the chip names the new rung
        # Whatever the loop made of it, a move inside a Tonic gesture is
        # never StageView's: it would pick or re-render a second time.
        return True

    def _onHover(self, view, event, x, y):
        """An idle move.  True claims it from StageView.

        With no button held StageView only rollover-picks (usdview's prim
        tooltip) because the tool forces mouse tracking on, so an open
        workspace always claims such a move, hovering or not.  A move with a
        button held and no Tonic gesture is a camera drag: StageView renders
        it once, and a Tonic hover here would render it a second time
        against a camera that is moving -- it re-runs on release instead.
        """
        workspaceOpen = bool(self._state.workspaceOpen)
        if not self._buttonsHeld(event):
            # No button is down, so no drag is either, whatever release was
            # lost.
            self._pressOwner = None
        elif self._pressOwner == "stageview":
            return False             # the camera's drag
        elif self._pressOwner == "tonic":
            # A press Tonic swallowed (a miss) keeps its drag off usdview's
            # picker, without hovering under a held button.
            return True
        # A held button whose press this view never saw (a drag that began
        # elsewhere, or a synthetic move) is a hover like any other.
        modifiers = modifierSet(event)
        self._lastPointer = (x, y, modifiers)
        if not workspaceOpen:
            return False
        if "alt" in modifiers or "meta" in modifiers:
            return True              # the camera's key; hover resumes after
        self._hoverAt(view, x, y, modifiers)
        return True

    @staticmethod
    def _buttonsHeld(event):
        """Any mouse button down; False for a lightweight/headless event."""
        try:
            buttons = event.buttons()
        except (AttributeError, RuntimeError):
            return False
        if buttons is None:
            return False
        try:
            from pxr.Usdviewq.qt import QtCore
            return buttons != QtCore.Qt.MouseButton.NoButton
        except (AttributeError, ImportError):
            return bool(buttons)

    def _hoverAt(self, view, x, y, modifiers):
        """Tonic's hover at one physical pixel; True when it evaluated."""
        if self._ladder.hoverSuppressed:
            return False             # the ladder's last rung
        if self._ladder.hoverCoolingDown(time.perf_counter() * 1000.0):
            # ... and its post-release cool-down (FB-02): the full-detail
            # republish lands first; _resetGestureState re-hovers after.
            return False
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
        sample = tonicLoops.Sample(self._session, camera, x, y, modifiers)
        self._loop.hover(sample)
        self._syncRegionOverlay()
        self._syncGizmoOverlay()
        self._refresh()
        return True

    def onRelease(self, view, event):
        from pxr.Usdviewq.qt import QtCore
        if (self._navDrag is not None and
                event.button() == getattr(QtCore.Qt.MouseButton,
                                          "MiddleButton", None)):
            # The host-application camera drag ends; what is under the still
            # cursor changed with the camera.
            self._navDrag = None
            self._pressOwner = None
            self._scheduleRehover()
            return True
        if not self._gesture or not self._ready():
            owner, self._pressOwner = self._pressOwner, None
            if owner == "stageview":
                # A camera drag may have moved the scene under a cursor
                # that stayed still: one hover now that the button is up.
                self._scheduleRehover()
            return owner == "tonic"
        # Only the button that holds the capture ends it (left, or middle
        # for a repeat drag).
        held = getattr(self, "_gestureButton", None)
        if held is None:
            held = QtCore.Qt.MouseButton.LeftButton
        if event.button() not in (held, QtCore.Qt.MouseButton.NoButton):
            return False
        self._pressOwner = None
        x, y = eventPixels(view, event)
        released = False
        resizing = self._brushResizeActive
        try:
            sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                       modifierSet(event) |
                                       self._holdModifiers())
            if self._brushResizeActive:
                end = getattr(self._loop, "endRadiusResize", None)
                if end is not None:
                    end(sample)
            else:
                self._loop.release(sample)
            released = True
            # The release closes a Tonic gesture whatever the loop made of
            # it; StageView never saw the press.
            return True
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
        self._pressOwner = "stageview"   # as onPress: until claimed
        if event.button() != QtCore.Qt.MouseButton.LeftButton:
            # StageView's default double click is a second press, so a
            # middle/right one would pick or open the prim menu (FB-03).
            return self.onPress(view, event)
        if not self._ready():
            return False
        if self._gesture:
            self._recoverGesture("double click after lost release")
        modifiers = modifierSet(event)
        if "alt" in modifiers or "meta" in modifiers:
            return False
        # Hierarchy alone owns double-click navigation, and only when its
        # target resolver accepted a visible tube (plan/18 section 3.6).
        # Tube's double-click grows the selection over a component (GZ-08)
        # and never changes the level.
        handler = getattr(self._loop, "doubleClick", None)
        if handler is not None:
            x, y = eventPixels(view, event)
            self._camera = tonicCamera.resolve(view)
            sample = tonicLoops.Sample(self._session, self._camera, x, y,
                                       modifiers)
            if handler(sample):
                self._pressOwner = "tonic"
                # Tube's double-click (GZ-08) re-placed its gizmo on the
                # grown selection; the Qt overlay follows at once.
                self._syncGizmoOverlay()
                self._refresh()
                return True
        # Qt delivers MouseButtonDblClick in place of the second press.
        # It is hierarchy navigation only when that loop accepted a visible
        # tube target.  Graph, Sculpt and a Tube double-click over no
        # component must see the ordinary second press/release instead;
        # blindly entering a level here quickly puts
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
        # Leaving drops the hover (and Sculpt's ring) the cursor followed.
        self._syncCursor()

    def cancelGesture(self):
        """Escape: drop the live gesture, restoring the press-time base."""
        # Graph can retain an idle transient region draft after the click
        # release. Escape must still reach that loop even though the
        # controller no longer has a captured mouse gesture.
        # Escape only cancels (SL-03): with nothing live it leaves the
        # selection alone -- a stray Escape used to throw away a careful
        # multi-CV pick. Ctrl+Shift+A is the deselect key.
        return self._recoverGesture("cancelled", force=True,
                                    clearSelection=False)

    def _leftButtonHeld(self, event):
        """False only when a real Qt move reports that capture was lost."""
        try:
            from pxr.Usdviewq.qt import QtCore
            buttons = event.buttons()
        except (AttributeError, RuntimeError):
            return True                # lightweight/headless event
        if buttons is None:
            return True
        # The capture's own button: middle for a repeat drag (G04).
        held = getattr(self, "_gestureButton", None)
        if held is None:
            held = QtCore.Qt.MouseButton.LeftButton
        return bool(buttons & held)

    def _resetGestureState(self):
        """Clear controller-only capture state; safe after every exit path."""
        self._gesture = False
        self._gestureButton = None
        self._brushResizeActive = False
        self._lastXY = None
        # The snap holds belong to one drag (RigExec _ClearHolds).
        self._holds.clear()
        self._gestureMoved = False
        chip = self._ladder.chipLabel() if self._ladder.armed else ""
        nowMs = time.perf_counter() * 1000.0
        if self._ladder.restore(nowMs):
            self._noteLadderRestored(chip)
            coolMs = self._ladder.hoverCooldownRemainingMs(nowMs)
            if coolMs > 0.0:
                try:
                    from pxr.Usdviewq.qt import QtCore
                    QtCore.QTimer.singleShot(int(coolMs) + 20,
                                             self._scheduleRehover)
                except (AttributeError, ImportError, RuntimeError):
                    pass

    def _recoverGesture(self, reason="", force=False, clearSelection=False):
        """Cancel an interrupted gesture and leave the controller reusable.

        `force` covers a loop exception during press: its own bracket may be
        open before the controller has set `_gesture` true.  Normal mouse
        leave intentionally does not call this helper because Qt commonly
        keeps a valid left-button capture outside the widget.
        """
        # A lost capture ends a Tonic-driven camera drag too; its release
        # will never come.
        self._navDrag = None
        if self._loop is None:
            self._resetGestureState()
            return False
        if not self._gesture and not force:
            if self._brushResizeArmed:
                self._brushResizeArmed = False
                return True
            return False
        cancelled = False
        # Whose bracket may still be open afterwards: a live viewport
        # capture (the controller owns the mouse, so any open bracket is
        # the interrupted gesture's), or a loop cancel that raised before
        # it could close its own. An idle Escape with neither leaves a
        # bracket some other owner (a held dock slider) opened alone.
        closeLeaked = bool(self._gesture)
        try:
            if self._brushResizeActive:
                cancelResize = getattr(self._loop, "cancelRadiusResize", None)
                cancelled = bool(cancelResize()) if cancelResize is not None \
                    else False
            else:
                cancelled = bool(self._loop.cancel())
        except Exception as exc:        # noqa: BLE001 - recovery boundary
            closeLeaked = True
            self._status("Tonic viewport recovery: %s" % exc)
        finally:
            self._resetGestureState()
            self._brushResizeArmed = False
            if closeLeaked and self._closeLeakedBracket(reason):
                cancelled = True
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

    def _closeLeakedBracket(self, reason=""):
        """Roll back a session bracket the loop's cancel left open.

        The loop's cancel is the only thing that calls cancelGesture for
        its own bracket; if it raised (or forgot) after the press opened
        one, the model would keep a gesture open for good -- every later
        Begin refused, Tube drags dead, the dock's Undo greyed -- until a
        rebind. _recoverGesture calls this only when the controller held
        the capture or the loop's cancel raised, so whatever is still open
        is the interrupted gesture's and is cancelled to its press-time
        base. True when a bracket was closed.
        """
        session = self._session
        if session is None or not getattr(session, "gestureActive", False):
            return False
        closed = False
        try:
            # Bounded: a session with no model keeps its depth on cancel.
            for _ in range(8):
                if not session.gestureActive:
                    break
                dirty = session.cancelGesture()
                session.publish(int(dirty or 0))
                closed = True
        except Exception as exc:        # noqa: BLE001 - recovery boundary
            self._status("Tonic viewport recovery: %s" % exc)
            return closed
        if closed:
            self._status("Tonic: %s left an edit open -- rolled it back"
                         % (reason or "an interrupted gesture"))
        return closed

    def _clearHover(self):
        """Drop an idle prehighlight when its viewport context changes."""
        self._lastHoverXY = None
        if self._gesture or self._session is None:
            return False
        changed = False
        if getattr(self._loop, "clearHover", lambda: False)():
            changed = True               # e.g. Sculpt's idle brush ring
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
            return {"points": (), "hover": None, "closeArmed": False}
        return getter()

    def hierarchyEdgePreview(self):
        """HierarchyLoop's split edge: the stroke in flight and the recorded
        one, as world endpoint pairs (None when absent or off-tool)."""
        getter = getattr(self._loop, "edgePreview", None)
        if getter is None:
            return {"live": None, "recorded": None}
        try:
            return getter()
        except (AttributeError, RuntimeError):
            return {"live": None, "recorded": None}

    def _syncRegionOverlay(self):
        """Repaint and resize the mouse-transparent draft overlay."""
        overlay = self._regionOverlay
        view = self._view
        if overlay is None or view is None:
            return
        overlay.setGeometry(view.rect())
        edges = self.hierarchyEdgePreview()
        overlay.setVisible(bool(self.regionDraftPreview()["points"]) or
                           edges["live"] is not None or
                           edges["recorded"] is not None)
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
        # The painted band covers the view and draws the rect itself, so
        # its add/remove glyph can sit past the cursor corner (FB-02).
        if overlay.geometry() != view.rect():
            overlay.setGeometry(view.rect())
        overlay.band = rect
        overlay.cursorPoint = finish
        overlay.colourRecord = self.bandRecord()
        overlay.show()
        overlay.raise_()
        overlay.update()

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

    def _activeGizmo(self):
        loop = self._loop
        return getattr(loop, "_gizmo", None) if loop is not None else None

    def gizmoScreenHandles(self):
        """Physical-pixel handles from the active Tube loop, if any.

        Always built from the live camera and gizmo, and it refreshes the
        cache the overlay paints from.
        """
        return self._rebuildGizmoPaint()[0]

    def _rebuildGizmoPaint(self):
        """Resolve the camera once and rebuild the overlay's records + pie."""
        gizmo = self._activeGizmo()
        view = self._view
        camera = (tonicCamera.resolve(view)
                  if gizmo is not None and view is not None else None)
        if camera is None:
            self._gizmoPaintCache = None
            return (), None
        records = tuple(gizmo.screenHandles(camera))
        pie = gizmo.pieSlice() if records else None
        self._gizmoPaintCache = (self._loop, gizmo.paintSignature(),
                                 records, pie)
        return records, pie

    def gizmoPaint(self):
        """(records, pie) for the overlay's paintEvent.

        RigExec rebuilds its handles on frustum/resize/selection/gesture
        events, never per paint (parity G22).  Those events all reach
        `_syncGizmoOverlay`, which rebuilds; a paint only rebuilds when the
        gizmo itself changed since (its cheap paint signature), so a
        repaint Storm asks for costs no camera resolve at all.
        """
        gizmo = self._activeGizmo()
        cache = self._gizmoPaintCache
        if gizmo is not None and cache is not None and \
                cache[0] is self._loop and \
                cache[1] == gizmo.paintSignature():
            return cache[2], cache[3]
        return self._rebuildGizmoPaint()

    def gizmoReadout(self):
        """(text, (x, y) physical pixels of the pivot) while a drag runs.

        GZ-07: TubeLoop.dragReadout() drawn beside the gizmo centre.  None
        when no loop drag is live, so an idle repaint resolves no camera.
        """
        readout = getattr(self._loop, "dragReadout", None)
        text = readout() if callable(readout) else ""
        gizmo = self._activeGizmo()
        if not text or gizmo is None or self._view is None:
            return None
        camera = tonicCamera.resolve(self._view)
        point = (camera.worldToPixels(gizmo.origin)
                 if camera is not None else None)
        if point is None:
            return None
        return text, (point[0], point[1])

    def refreshGizmo(self):
        """GZ-08: re-place the loop's gizmo on the live selection and view.

        For changes that arrive without a mouse event -- undo/redo, a dock
        action, a warning row's click -- so the handles never trail the
        model.  A live drag keeps its press-time gizmo (the loop refuses).
        """
        refresh = getattr(self._loop, "refreshGizmo", None)
        if refresh is not None:
            # No view (headless) or no camera yet: the loop re-places with
            # the camera it last saw, keeping the gizmo's size.
            refresh(tonicCamera.resolve(self._view))
        self._syncGizmoOverlay()

    def _syncGizmoOverlay(self):
        """Keep the unoccluded Qt gizmo in lockstep with the live picker."""
        # Every mode/sub-mode/tool/hover/gesture change already lands here
        # (the dock's tool buttons included), so the HUD and the cursor
        # follow the same beat; both are no-ops when nothing changed.
        self._syncFeedback()
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

    # -- HUD, cursor and band tint (FB-02) -----------------------------------

    def _feedbackLive(self):
        return (self._installed and not self._suspended and
                self._view is not None and
                bool(self._state.workspaceOpen))

    def _syncFeedback(self):
        self._syncHud()
        self._syncCursor()

    def ladderChipText(self):
        """The HUD's ladder chip now: the live rung, or '' at full detail.

        A release that restored fidelity keeps the rung it had on screen
        for LADDER_CHIP_LINGER_MS, marked as restored, so a short heavy
        drag still says why it looked coarse.
        """
        live = self._ladder.chipLabel() if self._ladder.armed else ""
        if live:
            return live
        linger = self._ladderChipLinger
        if linger is None:
            return ""
        text, until = linger
        if time.monotonic() >= until:
            self._ladderChipLinger = None
            return ""
        return text

    def hudText(self):
        """(title, hint, chip): what the viewport HUD shows."""
        bound = getattr(self._session, "model", None) is not None
        return (hudTitle(self._state, bound), hudHint(self._state, bound),
                self.ladderChipText())

    def _syncHud(self):
        overlay = self._hudOverlay
        if overlay is None:
            return
        if not self._feedbackLive() or not self._state.activeMode:
            overlay.hide()
            return
        title, hint, chip = self.hudText()
        view = self._view
        if overlay.geometry() != view.rect():
            overlay.setGeometry(view.rect())
        changed = (title, hint, chip) != (overlay.title, overlay.hint,
                                          overlay.chip)
        overlay.title, overlay.hint, overlay.chip = title, hint, chip
        if not overlay.isVisible():
            overlay.show()
            overlay.raise_()
            changed = True
        if changed:
            overlay.update()

    def _noteLadderRestored(self, label):
        """A release restored a stepped ladder: linger its chip ~1 s."""
        if not label:
            return
        self._ladderChipLinger = (
            "%s → full" % label,
            time.monotonic() + LADDER_CHIP_LINGER_MS / 1000.0)
        try:
            from pxr.Usdviewq.qt import QtCore
            # A little after the deadline, so the check sees it expired.
            QtCore.QTimer.singleShot(LADDER_CHIP_LINGER_MS + 50,
                                     self._syncHud)
        except (AttributeError, ImportError, RuntimeError):
            pass

    def cursorName(self):
        """The CURSOR_* name the live mode, tool and pointer call for."""
        state = self._state
        loop = self._loop
        mode = str(state.activeMode or "")
        subMode = self._subModeOf(mode) if mode else ""
        gizmo = self._activeGizmo()
        hoverHandle = int(getattr(gizmo, "hoverHandle", -1)) \
            if gizmo is not None and getattr(gizmo, "visible", True) else -1
        band = self._gesture and loop is not None and (
            loop.marqueeRect() is not None or
            bool(self.selectionLassoPoints()))
        dragging = False
        if self._gesture and loop is not None and not band:
            active = getattr(loop, "gizmoDragActive", None)
            dragging = bool(callable(active) and active())
            if not dragging and mode == "graph":
                # Reposition moves a CV/edge and Place drags its new node;
                # Draw and Region lay points, which the cross already says.
                dragging = bool(
                    (subMode == "reposition" and
                     getattr(loop, "_repositionIds", ())) or
                    (subMode == "place" and
                     int(getattr(loop, "_dragNode", -1)) >= 0))
        edge = getattr(loop, "_wantsEdgeStroke", None)
        return cursorFor(
            mode, subMode, getattr(state, "transformTool", ""), hoverHandle,
            dragging=dragging, band=band,
            ring=bool(getattr(loop, "_ringShown", False)) or
            (self._brushResizeActive and mode == "sculpt"),
            edgeStroke=bool(callable(edge) and edge()))

    def _syncCursor(self):
        view = self._view
        if view is None:
            return
        if not self._feedbackLive():
            self._restoreCursor()
            return
        name = self.cursorName()
        if name == self._cursorName:
            return
        from pxr.Usdviewq.qt import QtCore, QtGui
        shapes = QtCore.Qt.CursorShape
        shape = {CURSOR_ARROW: shapes.ArrowCursor,
                 CURSOR_CROSS: shapes.CrossCursor,
                 CURSOR_BLANK: shapes.BlankCursor,
                 CURSOR_SIZE_ALL: shapes.SizeAllCursor,
                 CURSOR_CLOSED_HAND: shapes.ClosedHandCursor}[name]
        try:
            view.setCursor(QtGui.QCursor(shape))
        except (AttributeError, RuntimeError):
            return
        self._cursorName = name

    def _restoreCursor(self):
        """Hand the view back the cursor it had before install."""
        view = self._view
        if self._cursorName is None or view is None:
            self._cursorName = None
            return
        try:
            if self._cursorWas is not None:
                view.setCursor(self._cursorWas)
            else:
                view.unsetCursor()
        except (AttributeError, RuntimeError):
            pass
        self._cursorName = None

    def bandRecord(self):
        """The live band's colour record: 'replace', 'add' or 'remove'."""
        return bandRecordFor(self._bandModifiers)

    # -- the idle pump (plan/18 section 3.3) -------------------------------

    def scheduleIdle(self):
        if self._suspended:
            return                      # resume() restarts pending work
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
        workspace = getattr(container, "workspace", None)
        refresh = getattr(workspace, "refresh", None)
        if refresh is None:
            return
        viewFocused = self.viewHasFocus()
        refresh()
        if viewFocused and not self.viewHasFocus():
            # A rebuilt page can hand focus down the dock's tab chain when
            # its focused row is replaced; the keys stay with the viewport.
            self._takeViewFocus()

    def viewHasFocus(self):
        view = self._view
        try:
            return bool(view is not None and view.hasFocus())
        except (AttributeError, RuntimeError):
            return False

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

    def keyOverride(self, _event=None):
        """A ShortcutOverride delivery: the next KeyPress is a new press."""
        self._keyLatch.newPress()

    def deliverKeyPress(self, event):
        """One KeyPress delivery from the application filter.

        onKey runs once per physical press (KeyPressLatch); the later
        deliveries of the same press get its answer back without acting.
        """
        identity = (keyCode(event), modifierSet(event))
        return self._keyLatch.press(identity, lambda: self.onKey(event))

    def deliverKeyRelease(self, event):
        """One KeyRelease delivery: ends the latched press, then F/J/X."""
        repeating = getattr(event, "isAutoRepeat", None)
        self._keyLatch.release(keyCode(event),
                               bool(callable(repeating) and repeating()))
        return self.onKeyRelease(event)

    def onKey(self, event):
        """The plan/18 section 3.4 table, dispatched. True consumes."""
        if not self._installed or self._session is None:
            return False
        if not self._state.workspaceOpen:
            return False
        key = keyName(event)
        modifiers = modifierSet(event)
        if self._pressHold(key, modifiers):
            return True
        if key == "f" and not modifiers:
            if self._brushResizeArmed or self._brushResizeActive:
                return True             # OS key-repeat while F is held
            if self._canBrushResize():
                self._brushResizeArmed = True
                self._brushResizeUsed = False
                return True
        if self._runTubeShortcut(key, modifiers):
            return True
        action = tonicModes.HotkeyAction(key, modifiers,
                                         self.viewHasKeys(),
                                         self._textFocus())
        if action is None:
            return False
        return self.runAction(action[0], action[1])

    # -- snap holds (parity G11) -------------------------------------------

    def _holdModifiers(self):
        """The live snap holds as Sample modifier names."""
        return frozenset(self._holds)

    def holdActive(self, name):
        """True while the "stepSnap" / "grid" hold is claimed."""
        return name in self._holds

    def _holdCandidate(self, key, modifiers):
        """J / X belong to Tonic only while a gizmo drag is live.

        Anywhere else they stay usdview's (J toggles its framed view), and
        typing in a field always wins.
        """
        if key not in HOLD_KEYS or not self._installed or \
                not self._state.workspaceOpen or not self._gesture:
            return False
        if "alt" in modifiers or "meta" in modifiers or self._textFocus():
            return False
        dragging = getattr(self._loop, "gizmoDragActive", None)
        return bool(callable(dragging) and dragging())

    def _pressHold(self, key, modifiers):
        """A J / X press during a gizmo drag: claim it and snap at once."""
        if not self._holdCandidate(key, modifiers):
            return False
        name = HOLD_KEYS[key]
        if name not in self._holds:
            # OS key repeat re-sends the press; only the first one acts.
            self._holds.add(name)
            self._reapplyDrag(modifiers)
        return True

    def _releaseHold(self, key, modifiers, autoRepeat=False):
        """Letting go of J / X un-snaps the live drag at once."""
        name = HOLD_KEYS.get(key)
        if name is None or name not in self._holds:
            return False
        if autoRepeat:
            return True                  # still held
        self._holds.discard(name)
        self._reapplyDrag(modifiers)
        return True

    def _reapplyDrag(self, modifiers):
        """Re-run the live drag at the last cursor with the holds changed.

        RigExec _ReapplyDrag: a hold changes the result under a still
        cursor, so the artist sees the snap (or its release) without
        having to nudge the mouse.  A drag that has not moved yet is left
        alone, so a press held still stays a click.
        """
        if not self._gesture or not self._gestureMoved or \
                self._lastXY is None or self._camera is None or \
                self._loop is None:
            return False
        sample = tonicLoops.Sample(
            self._session, self._camera, self._lastXY[0], self._lastXY[1],
            frozenset(modifiers) | self._holdModifiers())
        self._loop.move(sample)
        self._session.publish()
        self._syncGizmoOverlay()
        self._refresh()
        return True

    # -- gizmo settings (parity G20) ----------------------------------------

    def _onGizmoSettings(self):
        """A gizmo setting changed: re-place the gizmo with the live camera.

        Reached from the dock's rows (through the GizmoSettings listeners),
        the `+`/`-`/`L` keys and Reset transform tool; a live drag keeps
        its press-time gizmo (TubeLoop.refreshGizmo refuses).
        """
        if not self._installed or self._loop is None or self._gesture:
            return
        refresh = getattr(self._loop, "refreshGizmo", None)
        if refresh is None or self._view is None:
            return
        camera = tonicCamera.resolve(self._view)
        if camera is None:
            return
        refresh(camera)
        self._session.publish(tonicLib.TONIC_DIRTY_GIZMO)
        self._syncGizmoOverlay()
        self._refresh()

    def onKeyRelease(self, event):
        """F is a held sculpt modifier, never a persistent usdview hotkey."""
        if self._holds:
            repeating = getattr(event, "isAutoRepeat", None)
            if self._releaseHold(keyName(event), modifierSet(event),
                                 bool(callable(repeating) and repeating())):
                return True
        if keyName(event) != "f":
            return False
        consumed = bool(self._brushResizeArmed or self._brushResizeActive)
        repeating = getattr(event, "isAutoRepeat", None)
        if callable(repeating) and repeating():
            return consumed
        # A tap -- F down and up with no width drag in between -- frames,
        # as F does in every other mode; a hold that dragged stays a resize.
        tap = (self._brushResizeArmed and not self._brushResizeActive and
               not self._brushResizeUsed)
        self._brushResizeArmed = False
        self._brushResizeUsed = False
        if tap:
            self.frameSelection()
        return consumed

    def _tubeShortcutCandidate(self, key, modifiers):
        """Whether a plain viewport key is reserved by the Tube workflow."""
        if key in SIZE_KEYS:
            # '+' is Shift+'=' on most layouts; Shift means nothing here.
            modifiers = frozenset(modifiers) - frozenset(("shift",))
        if (not self.viewHasKeys() or self._textFocus() or modifiers or
                self._state.activeMode not in ("tube", "hierarchy")):
            return False
        if key in ("f8", "f9", "f10", "f11"):
            return self._state.activeMode == "tube"
        if self._state.activeMode == "tube":
            # Parity G13: +/=/- resize the manipulator and L flips World <->
            # Tube orientation.  Tube mode only: L is Link in Graph,
            # Levels in Hierarchy and Lengthen in Sculpt.  Parity G17: P
            # cycles Rotate/Scale's group pivot (P is Place in Graph and
            # Params in Fill, never a Tube key).
            return key in ("q", "w", "e", "r", "l", "p") + SIZE_KEYS
        # Hierarchy Q/W/E/R enters Tube object editing while retaining the
        # hierarchy's already selected whole-tube owners -- the same jump
        # the dock's transform row makes, so its "Select (Q)" tooltip is
        # true here too (Q is no Hierarchy sub-mode letter).
        return key in ("q", "w", "e", "r")

    def _runTubeShortcut(self, key, modifiers):
        if not self._tubeShortcutCandidate(key, modifiers):
            return False
        component = {"f8": "tube", "f9": "center", "f10": "ring",
                     "f11": "section"}
        if key in component:
            self.setSelectionKind(component[key])
            return True
        if key in ("l", "p") or key in SIZE_KEYS:
            return self._runGizmoKey(key)
        fromHierarchy = self._state.activeMode == "hierarchy"
        if fromHierarchy:
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
        # The jump changes mode as well as tool; say so, or W in Hierarchy
        # reads as the dock switching to Tube on its own (DK-02).
        self._status("Tonic Tube: %s%s" % (
            tool.title(), " (from Hierarchy)" if fromHierarchy else ""))
        self._syncGizmoOverlay()
        self._refresh()
        return True

    def _runGizmoKey(self, key):
        """`L` orientation toggle, `+`/`=`/`-` manipulator size (G13), `P`
        group pivot cycle (G17).

        Declined (False) mid-drag, where the press-time gizmo must hold.
        """
        loop = self._loop
        camera = (tonicCamera.resolve(self._view)
                  if self._view is not None else None)
        if key == "l":
            toggle = getattr(loop, "toggleOrientation", None)
            if toggle is None or toggle(camera) is None:
                return False
        elif key == "p":
            cycle = getattr(loop, "cycleGroupPivot", None)
            if cycle is None:
                return False
            dragging = getattr(loop, "gizmoDragActive", None)
            if callable(dragging) and dragging():
                return False
            # Under Move/Select the loop refuses and says why in the
            # status line; the key is still Tube's, so it is consumed.
            cycle(camera)
        else:
            resize = getattr(loop, "scaleManipulator", None)
            step = tonicGizmoSettings.MANIPULATOR_SIZE_STEP
            factor = step if key in ("+", "=") else 1.0 / step
            if resize is None or resize(factor, camera) is None:
                return False
        self._syncGizmoOverlay()
        self.refreshWorkspace()
        self._refresh()
        return True

    @staticmethod
    def _textFocus():
        from pxr.Usdviewq.qt import QtWidgets
        widget = QtWidgets.QApplication.focusWidget()
        if isinstance(widget, QtWidgets.QComboBox):
            # A plain choice list takes arrow keys, not typing; only an
            # editable one would eat the hotkeys' letters and digits.
            return bool(widget.isEditable())
        return isinstance(widget, (QtWidgets.QLineEdit, QtWidgets.QTextEdit,
                                   QtWidgets.QPlainTextEdit,
                                   QtWidgets.QAbstractSpinBox))

    def runAction(self, action, argument=None):
        """Execute one hotkey action; True when it was ours."""
        modes = tonicModes
        if action == modes.ACTION_MODE:
            self.setMode(argument)
            return True
        if action == modes.ACTION_SUBMODE:
            subId = self._subModeForLetter(argument)
            if not subId:
                if str(argument).upper() == "F":
                    # Frame the Tonic selection; with none, usdview's F
                    # frames its prims (ShortcutOverride left it there).
                    return self.frameSelection()
                return False
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
            # has a retained Region CV to remove. It is Ctrl+Up's twin, so
            # it goes through the same loop-first route: HierarchyLoop's
            # exitLevel collapses the active cut (parent back on screen,
            # children hidden), which the plain level step never did.
            return self._loopOr("exitLevel", self.exitLevel)
        if action in (modes.ACTION_UNDO, modes.ACTION_REDO) and \
                self._gesture:
            # Parity G14: undo/redo refuse while a drag is live (RigExec
            # 2333-2348).  The drag's native bracket is still open, so an
            # undo now would pop the wrong step out from under it.
            self._status("Tonic: finish the drag before %s" % (
                "undo" if action == modes.ACTION_UNDO else "redo"))
            return False
        if action == modes.ACTION_UNDO:
            self._session.undo()
            # GZ-08: the step moved what the gizmo sits on; follow it now,
            # not on the next mouse event.
            self.refreshGizmo()
            self._refresh()
            self.scheduleIdle()
            return True
        if action == modes.ACTION_REDO:
            self._session.redo()
            self.refreshGizmo()
            self._refresh()
            self.scheduleIdle()
            return True
        if action == modes.ACTION_DELETE:
            if self._gesture:
                # Deleting what a live drag holds would leave its open
                # bracket editing tubes that no longer exist.
                self._status("Tonic: finish the drag before deleting")
                return False
            if self._loop is None or not self._loop.deleteSelection():
                return False
            self._session.publish()
            # A whole-tube or CV delete takes the gizmo's target with it.
            self._syncGizmoOverlay()
            self._refresh()
            self.scheduleIdle()
            return True
        if action in (modes.ACTION_SELECT_ALL, modes.ACTION_DESELECT_ALL,
                      modes.ACTION_INVERT):
            return self.selectionCommand(action)
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
        # Shift+D consumes Hierarchy's drawn split edge; the overlay that
        # shows it must go in the same key press, not on the next hover.
        self._syncRegionOverlay()
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

    # -- select all / none / invert (SL-03) --------------------------------

    # The kinds Ctrl+A and Ctrl+I range over in a mode whose loop has no
    # command of its own: exactly the kinds that mode's box select takes.
    # Sculpt selects nothing, so the keys are not its.
    SELECTION_KINDS = {
        "graph": tonicLib.TONIC_PICK_GRAPH_NODE,
        "hierarchy": tonicLib.TONIC_PICK_TUBE_VERT,
        "fill": tonicLib.TONIC_PICK_TUBE_VERT,
    }

    def selectionCommand(self, action):
        """Ctrl+A / Ctrl+Shift+A / Ctrl+I; True when it was ours.

        `action` is tonicModes.ACTION_SELECT_ALL / _DESELECT_ALL / _INVERT,
        which is also the name of the loop method that may own it (Tube
        does: its kinds follow the component sub-mode) and is called with
        the camera. Otherwise the controller runs it over SELECTION_KINDS.
        "All" is a box over the whole view, so it takes exactly what the
        focused level shows and nothing a hidden level or a collapsed
        branch holds.
        """
        session = self._session
        loop = self._loop
        if (self._gesture or loop is None or session is None or
                session.model is None):
            return False
        # A Graph two-click action draws its first pick as a selection;
        # replacing the selection must not leave that pick armed under it.
        disarm = getattr(loop, "_disarm", None)
        if callable(disarm):
            disarm(clearSelection=False)
        camera = (tonicCamera.resolve(self._view)
                  if self._view is not None else None)
        handler = getattr(loop, action, None)
        if handler is not None:
            done = bool(handler(camera))
        else:
            done = self._selectionFallback(action, camera, loop)
        if not done:
            return False
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        self._syncGizmoOverlay()
        self._refresh()
        self.scheduleIdle()
        return True

    def _selectionFallback(self, action, camera, loop):
        from . import tonicLoopsTube
        session = self._session
        label = "Tonic %s" % (getattr(loop, "label", "") or "")
        if action == tonicModes.ACTION_DESELECT_ALL:
            session.clearSelection(0)
            self._status("%s: nothing selected" % label.rstrip())
            return True
        kinds = self.SELECTION_KINDS.get(getattr(loop, "modeId", ""), 0)
        if not kinds or camera is None:
            return False
        band = tonicLoopsTube.viewBand(session, camera, kinds)
        if action == tonicModes.ACTION_SELECT_ALL:
            done = band(tonicLib.TONIC_SELECT_SET)
        else:
            done = tonicLoopsTube.invertBand(session, kinds, band)
        if done:
            self._status("%s: %d selected"
                         % (label.rstrip(), session.selectionCount(kinds)))
        return bool(done)

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
        if not self._beginHierarchyAction(session, "Subdivide"):
            return False
        made = 0
        done = 0
        failures = []
        for tubeId in tubes:
            try:
                made += len(tonicHierarchy.subdivide(
                    session.dll, session.model, tubeId, count,
                    self._state.splitMode, 0))
                done += 1
            except (RuntimeError, NotImplementedError) as exc:
                failures.append("T%d: %s" % (int(tubeId), exc))
        if not self._endHierarchyAction(session, done):
            self._status("Tonic Hierarchy: Subdivide: nothing split -- %s"
                         % (failures[0] if failures else "no tube split"))
            return False
        if failures:
            self._status("Tonic Hierarchy: Subdivide: %d done, %d failed: %s"
                         % (done, len(failures), failures[0]))
            return False
        self._status("Tonic Hierarchy: %d child tube(s)" % made)
        return True

    def _beginHierarchyAction(self, session, label):
        """Open the fallback hierarchy key's bracket; False when refused.

        HierarchyLoop._beginAction's twin for Shift+D / Shift+M from the
        other modes: a refused Begin means someone else's bracket is open
        (a live drag, a dock slider), so running the edit would land in
        that bracket and the closing endGesture would seal it.
        """
        if session.beginGesture(label):
            return True
        self._status("Tonic Hierarchy: %s could not start an undo step -- "
                     "nothing changed" % label)
        return False

    def _endHierarchyAction(self, session, done):
        """Seal the bracket when something changed, else roll it back.

        Mirrors HierarchyLoop._endAction: zero successes cancels, so the
        undo stack never gains a step that undoes to where it was.
        """
        if done:
            session.endGesture()
            session.publish()
            session.enqueueCommit()
        else:
            session.publish(int(session.cancelGesture() or 0))
        self._refresh()
        self.scheduleIdle()
        return bool(done)

    def mergeChildrenOfSelection(self):
        """Shift+M over the selected tubes."""
        session = self._session
        tubes = self.selectedTubes()
        if session is None or session.model is None or not tubes:
            self._status("Tonic: select a parent tube to merge")
            return False
        if not self._beginHierarchyAction(session, "Merge children"):
            return False
        done = 0
        failures = []
        for tubeId in tubes:
            # Tonic_MergeChildren on a leaf succeeds doing nothing, so a
            # success only counts when the tube had children to merge.
            try:
                hadChildren = bool(tonicHierarchy.tubeChildren(
                    session.dll, session.model, tubeId))
            except (RuntimeError, NotImplementedError):
                hadChildren = True      # cannot tell: let the merge decide
            if not hadChildren:
                continue
            try:
                tonicHierarchy.mergeChildren(session.dll, session.model,
                                             tubeId)
                done += 1
            except (RuntimeError, NotImplementedError) as exc:
                failures.append("T%d: %s" % (int(tubeId), exc))
        if not self._endHierarchyAction(session, done):
            self._status("Tonic Hierarchy: Merge children: nothing merged "
                         "-- %s" % (failures[0] if failures else
                                    "those tubes have no children"))
            return False
        if failures:
            self._status("Tonic Hierarchy: Merge children: %d done, %d "
                         "failed: %s" % (done, len(failures), failures[0]))
            return False
        self._status(tonicHierarchy.mergeChildrenStatus(
            "%d tube(s)" % done))
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
        # Ctrl+Shift+S takes the same dialog, suffix and failure warning
        # as the menu and the dock: the container owns the one path (DK-03).
        session = self._session
        if session is None or session.model is None:
            return False
        save = getattr(self._container, "saveGroomInteractive", None)
        if save is None:
            return False
        save(self._api)
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
                if kind == QtCore.QEvent.Type.Wheel:
                    # A dolly under a live drag would move the camera the
                    # gesture's press-time projection was measured with.
                    return bool(controller.gestureActive)
                if kind == QtCore.QEvent.Type.ContextMenu:
                    # The right press is Tonic's while the workspace is
                    # open (FB-03); the platform's follow-up context-menu
                    # event must not reach a parent's popup either.
                    return bool(controller._installed and
                                controller._state.workspaceOpen)
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
                    controller._pressOwner = None
                    controller.setPointerInside(False)
                    return False
                if kind == QtCore.QEvent.Type.Resize:
                    controller._syncRegionOverlay()
                    controller._syncMarqueeOverlay()
                    # The gizmo layer must cover the new rect and its
                    # cached handles are projected for the old one (G22).
                    controller._syncGizmoOverlay()
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
                # A hold whose release went to another window must not
                # snap the next drag (RigExec _ClearHolds on deactivate).
                controller._holds.clear()
                controller._keyLatch.clear()
                controller.setPointerInside(False)
                return False
            if kind == QtCore.QEvent.Type.ShortcutOverride:
                # Every physical press (auto-repeat included) starts with
                # an override: the KeyPress that follows acts once.
                controller.keyOverride(event)
                # usdview binds L itself.  Claim a Tonic key at Qt's
                # shortcut-arbitration stage so the following KeyPress still
                # reaches onKey instead of silently retaining the previous
                # brush.
                key = keyName(event)
                modifiers = modifierSet(event)
                if controller._holdCandidate(key, modifiers) or \
                        HOLD_KEYS.get(key) in controller._holds:
                    # J / X during a gizmo drag: claimed here so usdview's
                    # own J (Toggle Framed View) never fires (parity G11).
                    event.accept()
                    return True
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
                    controller.viewHasKeys(), controller._textFocus())
                if (action is not None and
                        action[0] == tonicModes.ACTION_SUBMODE and
                        not controller._subModeForLetter(action[1]) and
                        not (str(action[1]).upper() == "F" and
                             controller.hasFrameTarget())):
                    # F frames Tonic's selection when there is one and
                    # usdview's prim selection otherwise.
                    action = None
                if (controller._installed and
                        controller._state.workspaceOpen and
                        action is not None):
                    event.accept()
                    return True
                return False
            if kind == QtCore.QEvent.Type.KeyRelease:
                return controller.deliverKeyRelease(event)
            if kind != QtCore.QEvent.Type.KeyPress:
                return False
            try:
                # Once per physical press: a declined key propagates up the
                # StageView's parents and comes back here for each one.
                return controller.deliverKeyPress(event)
            except Exception as exc:        # noqa: BLE001 - never wedge Qt
                controller._status("Tonic hotkey: %s" % exc)
                return False

    return Filter()
