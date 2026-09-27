# usdGenTonicTools.tonicWorkspace -- the Tonic dock (plan/18 section 3.5).
#
# One QDockWidget: a persistent file row (Undo/Redo, Save/Export/Import),
# a mode shelf, the active mode's sub-mode shelf, a generated parameter
# form (tonicPanels.descriptors), an actions block (tonicPanels.actions), a
# warnings list (tonicHud.warnings), a Display group and a status strip
# (message area, tonicHud.syncSummary pill, tool line, GPU/CPU chip).
# Nothing here computes a value or drives the ABI
# directly: the file buttons call the container's one dialog path
# (saveGroomInteractive & co, shared with the menu and Ctrl+Shift+S), and
# every parameter edit and one-shot action goes through the Qt-free
# descriptors in tonicPanels.py, so the dock itself has no logic to unit
# test.
#
# Follows the ExpressionEditorDock pattern (usdGenTools/exprEditor.py): a
# QDockWidget added to usdviewApi.qMainWindow's right dock area, Qt imported
# only from this module (pxr.Usdviewq.qt), never at import time from any
# sibling that other code loads headlessly.
from __future__ import annotations

import math
import re
import time

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from . import (tonicDockIds, tonicHierarchy, tonicHud, tonicLib, tonicModes,
               tonicPanels)
from . import tonicCamera
from . import tonicGizmoSettings

TITLE = "Tonic"
REFRESH_MS = 250
SHELF_BUTTON_HEIGHT = 28
STATUS_LINES = 3
# DK-05 status strip. A message stays up this long after its last line; the
# pill is as wide as its longest routine word, and anything longer (a
# fallback or commit-failure reason) elides into the tooltip.
MESSAGE_FADE_MS = 4000
PILL_WIDTH_TEXT = "Committing..."
NAVIGATION_STYLES = (("maya", "Maya"), ("blender", "Blender"))
# Parity G14: the three redo bindings (tonicModes.HotkeyAction), as the
# RigExec toolbar's redo tooltip lists them.
UNDO_KEYS = "(Ctrl+Z)"
REDO_KEYS = "(Ctrl+Y, Ctrl+Shift+Z, Shift+Z)"
# (background, foreground) per tonicHud.syncSummary tone.
_PILL_COLORS = {
    "ok": ("#2e6b47", "#e8f5ec"),
    "busy": ("#8a6a12", "#fff4d6"),
    "info": ("#35526b", "#e3eef7"),
    "error": ("#9c2f24", "#ffe9e6"),
    # No scalp bound: nothing to sync, so neither green nor a warning.
    "neutral": ("#4a4f55", "#d7dbe0"),
}
_MESSAGE_COLORS = {"error": "#ff6b5e", "warning": "#e6b450"}
# A status line reads as a refusal when it says so, whatever level its
# sender gave it: most senders still pass text alone (SS-02 adds levels).
_ERROR_TEXT = re.compile(r"cannot|failed|refus|nothing to|not found",
                         re.IGNORECASE)
# DK-04 icon sizes: the mode shelf's glyph sits over its label, the
# transform row is icon-only at the usdRig toolbar's 18 px, and every
# other shelf puts a 16 px glyph beside its text.
MODE_ICON_PX = 20
TOOL_ICON_PX = 18
SHELF_ICON_PX = 16
MODE_BUTTON_HEIGHT = 46

# The checked look of the usdRig viewport toolbar (gizmoUI.py 508-513), so
# the Tonic dock reads as the same family as the RigExec manipulators.
_DOCK_STYLE = (
    "QToolButton:hover { background-color: #364b5c; }"
    "QToolButton:checked { background-color: #4879b4; color: #ffffff;"
    " border: 1px solid #79a6dc; border-radius: 3px; }"
    "QToolButton:checked:hover { background-color: #5288c4; }")

# Descriptors whose control is not a form row: the transform tool is the
# Q/W/E/R icon row above the form. The descriptor stays in
# tonicPanels.descriptors for scripting and for parameterIds().
_OFF_FORM_IDS = frozenset(("transformTool",))


def _loadIcon(name):
    """The dock's QIcon for manifest `name`, or None (the caller shows text).

    tonicIcons' generated/copied PNGs first; the vendored usdRig
    QPainterPath glyphs only when that art is missing and a glyph exists
    for the name (the transform row and the header), so a broken install
    still draws those buttons instead of leaving them blank."""
    if not name:
        return None
    icon = None
    try:
        from . import tonicIcons
        icon = tonicIcons.loadIcon(name)
    except ImportError:
        icon = None
    if icon is not None:
        return icon
    try:
        from . import tonicGizmoIcons
    except ImportError:
        return None
    return tonicGizmoIcons.IconFor(name)

# Sub-mode shelf source per mode: the *_SUBMODES tuple, the setter that
# records a click on `state`, and whether the shelf shows at all (plan/18
# section 3.5 item 2: "hidden for Output").
_SUBMODES_BY_MODE = {
    "graph": (tonicModes.GRAPH_SUBMODES, tonicModes.SetActiveGraphSubMode),
    "tube": (tonicModes.TUBE_SUBMODES, tonicModes.SetActiveTubeSubMode),
    "fill": (tonicModes.FILL_SUBMODES, tonicModes.SetActiveFillSubMode),
    "hierarchy": (tonicModes.HIERARCHY_SUBMODES,
                 tonicModes.SetActiveHierarchySubMode),
    "sculpt": (tonicModes.SCULPT_SUBMODES, tonicModes.SetActiveSculptSubMode),
}

_SUBMODE_STATE_ATTR = {
    "graph": "graphSubMode",
    "tube": "tubeSubMode",
    "fill": "fillSubMode",
    "hierarchy": "hierarchySubMode",
    "sculpt": "sculptSubMode",
}

# DK-08 warnings: one glyph per severity (the generated status_* art, or
# Qt's message-box icon while that art is missing), and the tooltip suffix
# on a row whose click selects what it is about.
_SEVERITY_ICONS = {
    "error": ("status_error", "SP_MessageBoxCritical"),
    "warning": ("status_warning", "SP_MessageBoxWarning"),
    "info": ("status_info", "SP_MessageBoxInformation"),
}

# DK-08 breadcrumb: [up] Groom > Tube 0 > L2 (4 children).
GROOM_CRUMB_TEXT = "Groom"
GROOM_ICON = "mode_hierarchy"
EXIT_LEVEL_ICON = "act_exit_level"
EXIT_LEVEL_TIP = "Exit level (Ctrl+Up)"
_CRUMB_SEPARATOR = "›"
_CRUMB_ELLIPSIS = "…"

# DK-08: Re-subdivide's second press discards the children's sculpt, so
# while it is armed the button itself says so, in the busy pill's amber.
RESUBDIVIDE_CONFIRM_TEXT = "Confirm re-subdivide (discards child sculpt)"
_ARMED_STYLE = ("QPushButton { background-color: #8a6a12; color: #fff4d6;"
                " border: 1px solid #e6b450; border-radius: 3px; }")

_WORKSPACES = {}


def openWorkspace(container, usdviewApi):
    """Show the process-wide Tonic dock for `container`, creating it once.

    Idempotent: a second call re-shows, raises and refreshes the existing
    dock instead of building a duplicate (plan/18 section 2 module-map
    contract).
    """
    key = id(container)
    workspace = _WORKSPACES.get(key)
    if workspace is not None:
        try:
            workspace.show()
            workspace.raise_()
            workspace.refresh()
            return workspace
        except RuntimeError:
            # The underlying C++ QDockWidget was already destroyed (a
            # stage reload tore down the main window's docks).
            del _WORKSPACES[key]
    workspace = TonicWorkspace(container, usdviewApi)
    _WORKSPACES[key] = workspace
    return workspace


def _escape(text):
    """Plain text inside the status strip's rich text."""
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


def _shelfTip(mode):
    """'Label (Hotkey)' over the status line: one tooltip shape for every
    mode, sub-mode and component button, so the key is always discoverable
    from the button itself (DK-02)."""
    return "%s (%s)\n%s" % (mode.label, mode.hotkey, mode.status)


def _dress(btn, kind, itemId, px, style=None):
    """Name `btn` "<prefix>:<id>" and give it its icon; True if it got one.

    `style` is the QToolButton style to use when the icon exists; without
    an icon the button keeps its text (plain text is the documented
    fallback, never an empty button). QPushButtons ignore `style`."""
    btn.setObjectName(tonicDockIds.objectName(kind, itemId))
    icon = _loadIcon(tonicDockIds.iconName(kind, itemId,
                                           tonicPanels.ACTION_ICONS))
    if icon is None:
        return False
    btn.setIcon(icon)
    btn.setIconSize(QtCore.QSize(px, px))
    if style is not None and isinstance(btn, QtWidgets.QToolButton):
        btn.setToolButtonStyle(style)
    return True


def _hidePage(page):
    """Take a cached page out of its stack's size hint.

    QStackedLayout sizes itself to the largest page unless a page's size
    policy is Ignored, so every page that is not the shown one is marked
    Ignored. That is what used to take a setFixedHeight per mode switch,
    and a fixed height too small for its page is what let the group box
    titles below overlap the shelf."""
    page.setSizePolicy(QtWidgets.QSizePolicy.Ignored,
                       QtWidgets.QSizePolicy.Ignored)


def _showPage(stack, page):
    """Show `page` in `stack` and let the stack size to it alone."""
    for index in range(stack.count()):
        other = stack.widget(index)
        if other is not page:
            _hidePage(other)
    page.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
                       QtWidgets.QSizePolicy.Preferred)
    stack.setCurrentWidget(page)
    stack.updateGeometry()


def _ramp_to_text(pairs):
    pairs = list(pairs)
    return ", ".join(
        "%.4g:%.4g" % (pairs[i], pairs[i + 1])
        for i in range(0, len(pairs) - 1, 2))


def _paramSignature(descriptors):
    """What a generated parameter form is built from.

    A cached per-mode page is reusable exactly while this is unchanged.
    Sculpt's strength range follows the brush; the active level is not
    part of any row (DK-06). A row's tooltip and enabled flag follow state
    too (Output's rows grey until Build, DK-07), but _refreshParams applies
    those to the live widgets, so they are left out: a Build or a Whole
    strand tick must not rebuild the form under the artist's cursor.
    """
    return tuple(
        (d.id, d.label, d.kind, d.min, d.max, d.step,
         tuple(d.choices) if d.choices else (),
         getattr(d, "unit", ""), tuple(getattr(d, "choiceLabels", ())))
        for d in descriptors)


SLIDER_STEPS = 1000


def _sliderIsLog(descriptor):
    """A range spanning two decades or more (Density 0.1-1000, brush
    radius 2-512 px) slides logarithmically: linear, the whole useful low
    end of Density was the first few pixels of travel."""
    lo, hi = float(descriptor.min), float(descriptor.max)
    return lo > 0.0 and hi / lo >= 100.0


def _sliderToValue(descriptor, pos):
    lo, hi = float(descriptor.min), float(descriptor.max)
    f = min(max(float(pos) / SLIDER_STEPS, 0.0), 1.0)
    if _sliderIsLog(descriptor):
        return lo * (hi / lo) ** f
    return lo + (hi - lo) * f


def _valueToSlider(descriptor, value):
    lo, hi = float(descriptor.min), float(descriptor.max)
    value = min(max(float(value), lo), hi)
    if hi <= lo:
        return 0
    if _sliderIsLog(descriptor):
        f = math.log(value / lo) / math.log(hi / lo)
    else:
        f = (value - lo) / (hi - lo)
    return int(round(f * SLIDER_STEPS))


def _text_to_ramp(text):
    """'pos:val, pos:val' -> [pos, val, ...]; raises ValueError on a typo
    (the dock catches it and marks the field, DK-06)."""
    flat = []
    for token in text.split(","):
        token = token.strip()
        if not token:
            continue
        pos, _, val = token.partition(":")
        pair = (float(pos), float(val))
        if not all(math.isfinite(v) for v in pair):
            # float() takes 'nan'/'inf'; the model would refuse them
            # silently after the field looked accepted.
            raise ValueError("length profile values must be finite")
        flat.extend(pair)
    return flat


def _crumbParts(label):
    """("L2 Tube 5") -> (2, "Tube 5"); a label with no level gives 0."""
    head, _sep, rest = str(label).partition(" ")
    if head[:1] == "L" and head[1:].isdigit():
        return int(head[1:]), rest.strip()
    return 0, str(label)


class _BreadcrumbBar(QtWidgets.QWidget):
    """The level breadcrumb: [up] Groom > Tube 0 > L2 (4 children).

    One flat tool button per place a click can go (the level number is in
    its tooltip, the button reads the tube's name), then the frontier --
    where the artist is editing -- as plain text, because clicking it
    would go nowhere. When the row is too narrow the crumbs after Groom
    give way to an ellipsis oldest-first, so the end of the path always
    shows. The buttons are pooled: a level walk re-labels them instead of
    building widgets (see the "nothing here is retired" note below).
    """

    def __init__(self, onCrumb, onExit, parent=None):
        super(_BreadcrumbBar, self).__init__(parent)
        self.setObjectName("tonicBreadcrumb")
        self._onCrumb = onCrumb
        self._key = None
        self._targets = []
        self._crumbs = []   # [(separator QLabel, QToolButton)], pooled
        self.setSizePolicy(QtWidgets.QSizePolicy.Ignored,
                           QtWidgets.QSizePolicy.Fixed)
        row = QtWidgets.QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(2)
        self._row = row
        self.exitButton = QtWidgets.QToolButton(self)
        self.exitButton.setObjectName("tonicBreadcrumbExit")
        self.exitButton.setAutoRaise(True)
        self.exitButton.setToolTip(EXIT_LEVEL_TIP)
        icon = _loadIcon(EXIT_LEVEL_ICON)
        if icon is not None:
            self.exitButton.setIcon(icon)
            self.exitButton.setIconSize(QtCore.QSize(SHELF_ICON_PX,
                                                     SHELF_ICON_PX))
        else:
            self.exitButton.setArrowType(QtCore.Qt.UpArrow)
        self.exitButton.clicked.connect(lambda checked=False: onExit())
        row.addWidget(self.exitButton)
        self._ellipsis = QtWidgets.QLabel(_CRUMB_ELLIPSIS, self)
        self._ellipsis.setToolTip("Earlier levels are hidden: widen the dock "
                                  "to see the whole path.")
        self._ellipsis.hide()
        row.addWidget(self._ellipsis)
        self._frontierSeparator = QtWidgets.QLabel(_CRUMB_SEPARATOR, self)
        row.addWidget(self._frontierSeparator)
        self.frontierLabel = QtWidgets.QLabel(self)
        self.frontierLabel.setObjectName("tonicBreadcrumbFrontier")
        self.frontierLabel.setTextFormat(QtCore.Qt.PlainText)
        font = self.frontierLabel.font()
        font.setBold(True)
        self.frontierLabel.setFont(font)
        row.addWidget(self.frontierLabel)
        row.addStretch(1)
        self.setFixedHeight(max(self.exitButton.sizeHint().height(),
                                self.fontMetrics().height() + 6))

    def crumbButtons(self):
        """The clickable crumbs shown now, root (Groom) first."""
        return [button for _sep, button in self._crumbs[:len(self._targets)]]

    def setSegments(self, crumbs, frontier):
        """`crumbs` is [(target, text, tooltip, iconName)], `frontier` is
        (text, tooltip) or None. Nothing is touched when neither moved."""
        key = (tuple(crumbs), frontier)
        if key == self._key:
            return
        self._key = key
        while len(self._crumbs) < len(crumbs):
            index = len(self._crumbs)
            separator = QtWidgets.QLabel(_CRUMB_SEPARATOR, self)
            button = QtWidgets.QToolButton(self)
            button.setAutoRaise(True)
            button.setObjectName("tonicCrumb:%d" % index)
            button.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
            button.setIconSize(QtCore.QSize(SHELF_ICON_PX, SHELF_ICON_PX))
            button.clicked.connect(
                lambda checked=False, i=index: self._clicked(i))
            at = self._row.indexOf(self._frontierSeparator)
            self._row.insertWidget(at, separator)
            self._row.insertWidget(at + 1, button)
            if index == 0:
                # The ellipsis stands for the crumbs after Groom.
                self._row.removeWidget(self._ellipsis)
                self._row.insertWidget(self._row.indexOf(button) + 1,
                                       self._ellipsis)
            self._crumbs.append((separator, button))
        self._targets = [crumb[0] for crumb in crumbs]
        for i, (_separator, button) in enumerate(self._crumbs):
            if i >= len(crumbs):
                continue
            _target, text, tip, iconName = crumbs[i]
            button.setText(text)
            button.setToolTip(tip)
            icon = _loadIcon(iconName) if iconName else None
            button.setIcon(icon if icon is not None else QtGui.QIcon())
        text, tip = frontier if frontier is not None else ("", "")
        self.frontierLabel.setText(text)
        self.frontierLabel.setToolTip(tip)
        self._elide()

    def _clicked(self, index):
        if index < len(self._targets):
            self._onCrumb(self._targets[index])

    def resizeEvent(self, event):
        super(_BreadcrumbBar, self).resizeEvent(event)
        self._elide()

    def _elide(self):
        count = len(self._targets)
        for i, (separator, button) in enumerate(self._crumbs):
            separator.setVisible(0 < i < count)
            button.setVisible(i < count)
        hasFrontier = bool(self.frontierLabel.text())
        self._frontierSeparator.setVisible(hasFrontier and count > 0)
        self.frontierLabel.setVisible(hasFrontier)
        self._ellipsis.hide()
        available = self.width()
        if available <= 0:
            return
        # Oldest first, never Groom and never the crumb next to the
        # frontier: that pair is the whole path in two words.
        for i in range(1, count - 1):
            self._row.invalidate()
            if self._row.sizeHint().width() <= available:
                break
            separator, button = self._crumbs[i]
            separator.hide()
            button.hide()
            self._ellipsis.show()


class _FaceHighlightOverlay(QtWidgets.QWidget):
    """Scalp faces outlined over the stage view (the coverage row's click).

    Neither the Hydra publish nor usdview's selection can highlight a
    subset of the scalp's faces, so the dock paints them itself: a
    mouse-transparent child of the StageView that re-projects the face
    polygons through the live camera on every paint, which keeps it
    pinned to the scalp across a camera move.
    """

    FILL = (255, 70, 40, 80)
    EDGE = (255, 120, 60, 235)

    def __init__(self, view):
        super(_FaceHighlightOverlay, self).__init__(view)
        self._view = view
        self._polygons = []
        self.setObjectName("tonicFaceHighlight")
        self.setAttribute(
            QtCore.Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(QtCore.Qt.WidgetAttribute.WA_NoSystemBackground,
                          True)
        self.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
        self.setGeometry(view.rect())
        self.hide()
        frustum = getattr(view, "signalFrustumChanged", None)
        if frustum is not None:
            frustum.connect(self.update)

    def polygonCount(self):
        return len(self._polygons)

    def setPolygons(self, polygons):
        self._polygons = [list(polygon) for polygon in polygons]
        self.update()

    def paintEvent(self, _event):
        if not self._polygons:
            return
        camera = tonicCamera.resolve(self._view)
        if camera is None:
            return
        try:
            ratio = max(float(self._view.devicePixelRatioF()), 1.0)
        except AttributeError:
            ratio = 1.0
        painter = QtGui.QPainter(self)
        try:
            painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing,
                                  True)
            pen = QtGui.QPen(QtGui.QColor(*self.EDGE))
            pen.setWidthF(2.0)
            painter.setPen(pen)
            painter.setBrush(QtGui.QBrush(QtGui.QColor(*self.FILL)))
            for polygon in self._polygons:
                projected = [camera.worldToPixels(p) for p in polygon]
                if any(p is None for p in projected):
                    continue
                painter.drawPolygon(QtGui.QPolygonF(
                    [QtCore.QPointF(p[0] / ratio, p[1] / ratio)
                     for p in projected]))
        finally:
            painter.end()


class TonicWorkspace(QtWidgets.QDockWidget):

    def __init__(self, container, usdviewApi):
        super(TonicWorkspace, self).__init__(TITLE, usdviewApi.qMainWindow)
        self.setObjectName("usdGenTonicWorkspace")
        self._container = container
        self._api = usdviewApi
        self._activeMode = ""
        self._paramWidgets = []   # [(descriptor, widget)] of the live page
        # modeId -> {subMode, subButtons, subGroup, params, paramRows,
        # paramSignature, actions, hasSubModes}. Built on first use, kept
        # for the life of the dock, switched with setCurrentWidget.
        self._pages = {}
        self._pageLevel = 0
        # What the last refresh actually showed. refresh() is called on
        # every publish, on every idle pump and on a 250 ms timer -- about
        # four times per artist op in the TN-5 controller soak -- so each
        # part compares its own inputs first and does nothing when they
        # have not moved (plan/17 section 7, the V9 note).
        self._shelfKey = None
        self._warningsKey = None
        self._warningsTexts = None
        self._statusText = None
        self._crumbText = None
        self._statusAmber = None
        self._pillKey = None
        self._chipKey = None
        self._summaryText = None
        # The last line the session's status sink delivered (DK-05), and
        # its level; both clear when the message fades. The serial counts
        # lines, so an action can tell whether it said anything.
        self._messageText = ""
        self._messageLevel = None
        self._messageSerial = 0
        self._staleContent = False
        self._warningsAt = 0.0
        self._geometryKey = None
        self._boundShown = None
        self._resumeShown = None
        self._instructionText = None
        self._toolStatusText = None
        self._undoKey = None
        self._toolKey = None
        self._offFormIds = ()
        # (kind, id) -> button for every button built outside a mode page
        # (header, mode shelf, F8-F11 row, transform row). Sub-mode and
        # action buttons live on their mode's page; button() finds them.
        self._buttons = {}
        # DK-08: severity glyphs (built once), the coverage row's face
        # highlight and the Re-subdivide button's armed look.
        self._severityIcons = {}
        self._faceOverlay = None
        self._highlightFaces = []
        self._highlightText = None
        self._scalpMeshKey = None
        self._scalpMeshCache = None
        self._resubdivideShown = None
        self._buildUi()
        self._api.qMainWindow.addDockWidget(
            QtCore.Qt.RightDockWidgetArea, self)
        self._timer = QtCore.QTimer(self)
        self._timer.setInterval(REFRESH_MS)
        self._timer.timeout.connect(self._onTimerTick)
        self.visibilityChanged.connect(self._onVisibilityChanged)
        state = container.tonicState
        self._setActiveMode(state.activeMode or tonicModes.MODES[0].id)
        state.workspaceOpen = True
        self.show()

    # ---- what the dock is showing ----------------------------------------
    #
    # Three read-only answers, for the T3 script that has to tell "the
    # panel rebuilt itself" from "the state changed and the panel did
    # not". Qt offers no way to read a QFormLayout back as descriptors.

    @property
    def activeMode(self):
        """The mode whose shelf, rows and actions are currently built."""
        return self._activeMode

    def parameterIds(self):
        """The descriptor ids the active mode shows a control for: the form
        rows, plus the ones drawn elsewhere (transformTool is the Q/W/E/R
        row)."""
        return ([descriptor.id for descriptor, _w in self._paramWidgets] +
                list(self._offFormIds))

    def subModeIds(self):
        """The sub-mode ids on the shelf under the mode buttons."""
        return list(self._subModeButtons)

    def button(self, kind, itemId):
        """The dock button for (kind, id), or None (DK-04 test hook).

        `kind` is one of tonicDockIds.PREFIXES: "mode", "sub", "comp",
        "tool", "action" or "file". Sub-mode and action buttons live on
        their mode's cached page; the active mode's page answers first and
        a page not built yet is built here, so a script can reach
        Hierarchy's Subdivide before the dock has ever shown Hierarchy.
        The button's objectName is tonicDockIds.objectName(kind, id)."""
        itemId = str(itemId)
        found = self._buttons.get((kind, itemId))
        if found is not None or kind not in ("sub", "action"):
            return found
        state = self._container.tonicState
        order = [self._activeMode] + [m.id for m in tonicModes.MODES
                                      if m.id != self._activeMode]
        for modeId in order:
            if not modeId:
                continue
            if kind == "sub":
                subs = tonicDockIds.SUBMODES.get(modeId, ())
                if not any(sub.id == itemId for sub in subs):
                    continue
            elif not any(action.id == itemId
                         for action in tonicPanels.actions(modeId)):
                continue
            # Build a missing page, never rebuild a cached one: only
            # _adoptMode may swap the rows the dock is showing.
            page = self._pages.get(modeId) or self._ensurePage(modeId, state)
            pool = page["subButtons"] if kind == "sub" else \
                page["actionButtons"]
            if itemId in pool:
                return pool[itemId]
        return None

    # ---- why nothing here is retired any more -----------------------------
    #
    # This dock used to tear the sub-mode shelf, the parameter form and
    # the action buttons down on every mode switch and hold the old
    # widgets in a `_retired` list until the next rebuild freed them.
    # Two costs came with that, both measured on the TN-5 controller soak
    # (plan/17 section 7, the V9 note):
    #
    #  * the free never happened. `_dropRetired` called deleteLater(),
    #    and Qt will not deliver a DeferredDelete posted at event-loop
    #    level 0 from inside a nested processEvents() -- which is every
    #    turn a T3 script gives it. The event sat in the posted queue
    #    holding the receiver, so each retired QToolButton, its connected
    #    lambdas and PySide's per-connection weakrefs lived for the life
    #    of the process: +60 Python objects per artist op, 285 000 live
    #    objects and +380 MB of private bytes in three minutes, with the
    #    Qt object count under the main window flat the whole time
    #    because a retired widget has no parent to be counted under.
    #  * the rebuild itself was about sixteen widgets created and
    #    destroyed per mode switch, and the relayout cascade behind it
    #    was most of the dock's share of the event traffic.
    #
    # Each mode now has one page per stack, built on first use and kept.
    # A mode switch is setCurrentWidget: nothing is created, nothing is
    # destroyed, and there is nothing to retire.

    # ---- construction ----------------------------------------------------

    def _buildUi(self):
        body = QtWidgets.QWidget(self)
        body.setObjectName("tonicWorkspaceBody")
        # Keep the standard dark Qt palette, with just enough checked-state
        # contrast to show the active mode, sub-mode and tool at a glance.
        body.setStyleSheet(_DOCK_STYLE)
        layout = QtWidgets.QVBoxLayout(body)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(4)

        # The scalp stays at the top while the artist changes modes.  Keeping
        # the current mesh visible prevents a mode switch from looking like
        # a lost binding, and makes the replacement action deliberate.
        # "Scalp" is the one word for it everywhere (menu, dock, picker,
        # status): three nouns for one mesh read as three different things.
        geometryBox = QtWidgets.QGroupBox("Scalp", body)
        geometryBox.setObjectName("tonicGeometryBlock")
        geometryRow = QtWidgets.QHBoxLayout(geometryBox)
        geometryRow.setContentsMargins(6, 10, 6, 4)
        self._geometryPathLabel = QtWidgets.QLabel("No scalp bound")
        self._geometryPathLabel.setObjectName("tonicGeometryPath")
        self._geometryPathLabel.setWordWrap(False)
        self._geometryPathLabel.setSizePolicy(
            QtWidgets.QSizePolicy.Ignored, QtWidgets.QSizePolicy.Preferred)
        geometryRow.addWidget(self._geometryPathLabel, 1)
        self._bindGeometryButton = QtWidgets.QPushButton("Bind scalp mesh...")
        _dress(self._bindGeometryButton, "file", "bind", SHELF_ICON_PX)
        self._buttons[("file", "bind")] = self._bindGeometryButton
        # Until a scalp is bound this is the only thing the dock can do, so
        # it is the default button and wears the accent colour.
        self._bindGeometryButton.setDefault(True)
        self._bindGeometryButton.setStyleSheet(
            "QPushButton { background-color: #2f7fbf; color: #ffffff;"
            " font-weight: bold; padding: 3px 10px; }"
            "QPushButton:hover { background-color: #3a8fd2; }"
            "QPushButton:pressed { background-color: #266a9f; }")
        self._bindGeometryButton.setToolTip(
            "Choose the scalp Mesh (or a face GeomSubset of one) from the "
            "current stage. Rebinding an edited groom requires explicit "
            "confirmation.")
        self._bindGeometryButton.clicked.connect(self._onBindGeometry)
        geometryRow.addWidget(self._bindGeometryButton)
        # Resume is SS-03's: the container answers whether the stage holds
        # a saved groom for it. Hidden until it says so, so a container
        # without the hook never shows a button that cannot work.
        self._resumeGroomButton = QtWidgets.QPushButton("Resume groom")
        _dress(self._resumeGroomButton, "file", "resume", SHELF_ICON_PX)
        self._buttons[("file", "resume")] = self._resumeGroomButton
        self._resumeGroomButton.setToolTip(
            "Rebuild the Tonic model from the groom saved in this stage.")
        self._resumeGroomButton.clicked.connect(self._onResumeGroom)
        self._resumeGroomButton.setVisible(False)
        self._resumeGroomButton.setEnabled(False)
        geometryRow.addWidget(self._resumeGroomButton)
        layout.addWidget(geometryBox)
        self._firstRunHint = QtWidgets.QLabel(
            "Step 1 — Bind a scalp mesh to start grooming. Select the "
            "scalp in the viewport, then click Bind scalp mesh.")
        self._firstRunHint.setObjectName("tonicFirstRunHint")
        self._firstRunHint.setWordWrap(True)
        self._firstRunHint.setTextFormat(QtCore.Qt.PlainText)
        layout.addWidget(self._firstRunHint)

        # The file row: Undo/Redo and Save/Export/Import, in every mode.
        # Undo had no control at all (only Ctrl+Z, which the artist had to
        # know), and the file buttons lived on Output's action page only,
        # so saving meant leaving the mode you were working in.
        self._fileRow = QtWidgets.QWidget(body)
        self._fileRow.setObjectName("tonicFileRow")
        fileLayout = QtWidgets.QHBoxLayout(self._fileRow)
        fileLayout.setContentsMargins(0, 0, 0, 0)
        fileLayout.setSpacing(3)
        self._undoButton = self._makeFileButton(
            "undo", "Undo", "Undo (Ctrl+Z)",
            lambda: self._onUndoRedo(tonicModes.ACTION_UNDO), False)
        self._redoButton = self._makeFileButton(
            "redo", "Redo", "Redo %s" % REDO_KEYS,
            lambda: self._onUndoRedo(tonicModes.ACTION_REDO), False)
        fileLayout.addWidget(self._undoButton)
        fileLayout.addWidget(self._redoButton)
        fileLayout.addStretch(1)
        self._saveButton = self._makeFileButton(
            "save", "Save",
            "Save the groom to a .usdc and sublayer it (Ctrl+Shift+S)",
            self._onSaveGroom, True)
        self._exportButton = self._makeFileButton(
            "export", "Export",
            "Export the focused level's center curves to a USD file",
            self._onExportCenterCurves, True)
        self._importButton = self._makeFileButton(
            "import", "Import",
            "Import a USD file's curves as locked tubes under the first "
            "selected tube (the root tube when nothing is selected)",
            self._onImportCurves, True)
        # The usdRig toolbar ends on its Settings button; Tonic's tool
        # settings are the active mode's Parameters form, so the button
        # brings that form into view rather than opening a second window.
        self._settingsButton = self._makeFileButton(
            "settings", "Settings",
            "Tool settings: show the active tool's parameters",
            self._onSettings, False)
        for btn in (self._saveButton, self._exportButton,
                    self._importButton, self._settingsButton):
            fileLayout.addWidget(btn)
        layout.addWidget(self._fileRow)

        # 1. Mode shelf: six checkable buttons in a small grid so the dock
        # remains usable at the normal 350-420 px width. Glyph over the
        # label; the number key is in the tooltip (DK-04), which is where
        # every other shelf keeps its key too.
        modeRow = QtWidgets.QGridLayout()
        modeRow.setContentsMargins(0, 0, 0, 0)
        modeRow.setHorizontalSpacing(3)
        modeRow.setVerticalSpacing(3)
        self._modeGroup = QtWidgets.QButtonGroup(self)
        self._modeGroup.setExclusive(True)
        self._modeButtons = {}
        for index, mode in enumerate(tonicModes.MODES):
            btn = QtWidgets.QToolButton()
            btn.setText(mode.label)
            btn.setCheckable(True)
            iconic = _dress(btn, "mode", mode.id, MODE_ICON_PX,
                            QtCore.Qt.ToolButtonTextUnderIcon)
            btn.setFixedHeight(MODE_BUTTON_HEIGHT if iconic
                               else SHELF_BUTTON_HEIGHT)
            btn.setSizePolicy(QtWidgets.QSizePolicy.Expanding,
                              QtWidgets.QSizePolicy.Fixed)
            btn.setToolTip(_shelfTip(mode))
            btn.clicked.connect(
                lambda checked, m=mode.id: self._setActiveMode(m))
            self._modeGroup.addButton(btn)
            self._modeButtons[mode.id] = btn
            self._buttons[("mode", mode.id)] = btn
            modeRow.addWidget(btn, index // 3, index % 3)
        layout.addLayout(modeRow)

        # 2-4. Sub-mode shelf, generated parameter form and action
        # buttons. Each mode gets ONE page in each stack, built the first
        # time that mode is shown and kept; switching modes is
        # setCurrentWidget, which creates and destroys nothing.
        #
        # They used to be torn down and rebuilt on every mode switch. That
        # is about sixteen widgets created and destroyed each time, and in
        # the TN-5 controller soak -- where three ops in four press a mode
        # key -- it drove 13 Create and 13 Destroy events per artist op
        # and a relayout cascade over the whole dock: 140 Move, 92 Resize,
        # 40 Paint events per op, every one of them through two Python
        # application event filters.
        self._subModeStack = QtWidgets.QStackedWidget()
        self._subModeStack.setObjectName("tonicSubModeStack")
        self._subModeStack.setContentsMargins(0, 0, 0, 0)
        # Maximum, not a fixed height: a fixed height smaller than the page
        # let the group boxes below draw their titles over the shelf. The
        # stack's hint is the shown page's alone (_showPage marks the
        # others Ignored), so short pages leave no gap either.
        self._subModeStack.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
                                         QtWidgets.QSizePolicy.Maximum)
        self._subModeGroup = None
        self._subModeButtons = {}
        layout.addWidget(self._subModeStack)
        self._tubeSelectionRow = QtWidgets.QWidget(body)
        tubeSelectionLayout = QtWidgets.QGridLayout(self._tubeSelectionRow)
        tubeSelectionLayout.setContentsMargins(0, 0, 0, 0)
        tubeSelectionLayout.setHorizontalSpacing(3)
        tubeSelectionLayout.setVerticalSpacing(3)
        self._tubeSelectionGroup = QtWidgets.QButtonGroup(
            self._tubeSelectionRow)
        self._tubeSelectionGroup.setExclusive(True)
        self._tubeSelectionButtons = {}
        # Built from TUBE_SUBMODES so the row prints the keys the viewport
        # actually binds (DK-02): a hand-written copy here drifted once.
        # Two columns, glyph beside label, like every sub-mode shelf.
        for index, sub in enumerate(tonicModes.TUBE_SUBMODES):
            btn = QtWidgets.QToolButton(self._tubeSelectionRow)
            btn.setText(sub.label)
            btn.setCheckable(True)
            _dress(btn, "comp", sub.id, SHELF_ICON_PX,
                   QtCore.Qt.ToolButtonTextBesideIcon)
            btn.setFixedHeight(SHELF_BUTTON_HEIGHT)
            btn.setSizePolicy(QtWidgets.QSizePolicy.Expanding,
                              QtWidgets.QSizePolicy.Fixed)
            btn.setToolTip(_shelfTip(sub))
            btn.clicked.connect(
                lambda checked, k=sub.id: self._setTubeSelectionKind(k))
            self._tubeSelectionGroup.addButton(btn)
            self._tubeSelectionButtons[sub.id] = btn
            self._buttons[("comp", sub.id)] = btn
            tubeSelectionLayout.addWidget(btn, index // 2, index % 2)
        self._tubeSelectionRow.setVisible(False)
        layout.addWidget(self._tubeSelectionRow)
        # The transform row (DK-04, parity G21): Select/Move/Rotate/Scale
        # as one exclusive, icon-only row, the usdRig viewport toolbar's
        # shape. It replaces the Transform combo that used to sit in the
        # middle of Tube's parameter form, where nobody looked for it.
        # Shown in Tube and Hierarchy; Hierarchy's buttons make the same
        # jump to Tube that its W/E/R keys make, so they wait for a tube.
        self._transformRow = QtWidgets.QWidget(body)
        self._transformRow.setObjectName("tonicTransformRow")
        transformLayout = QtWidgets.QHBoxLayout(self._transformRow)
        transformLayout.setContentsMargins(0, 0, 0, 0)
        transformLayout.setSpacing(3)
        transformLabel = QtWidgets.QLabel("Transform")
        transformLabel.setStyleSheet("font-size: 11px;")
        transformLayout.addWidget(transformLabel)
        self._toolGroup = QtWidgets.QButtonGroup(self._transformRow)
        self._toolGroup.setExclusive(True)
        self._toolButtons = {}
        for tool in tonicDockIds.TRANSFORM_TOOLS:
            btn = QtWidgets.QToolButton(self._transformRow)
            # The text stays even though only the glyph shows: it is what
            # accessibility and a missing-art fallback read.
            btn.setText(tool.label)
            btn.setCheckable(True)
            _dress(btn, "tool", tool.id, TOOL_ICON_PX,
                   QtCore.Qt.ToolButtonIconOnly)
            btn.setFixedHeight(SHELF_BUTTON_HEIGHT)
            btn.setMinimumWidth(SHELF_BUTTON_HEIGHT)
            btn.setToolTip("%s (%s): %s" % (tool.label, tool.hotkey,
                                            tool.status))
            btn.clicked.connect(
                lambda checked, t=tool.id: self._onTransformTool(t))
            self._toolGroup.addButton(btn)
            self._toolButtons[tool.id] = btn
            self._buttons[("tool", tool.id)] = btn
            transformLayout.addWidget(btn)
        # Parity G21: the Global/Local toggle and the group-pivot cycle,
        # the two usdRig toolbar glyphs after the tools. Checkable so the
        # checked highlight can mean "not the default" (Local, Centre);
        # the click handler, not the check state, decides what comes next
        # and _syncGizmoToggles puts the widget back in step.
        transformLayout.addSpacing(8)
        self._gizmoToggles = {}
        self._gizmoToggleKey = None
        self._gizmoIconCache = {}
        handlers = {"orientation": self._onOrientationToggle,
                    "groupPivot": self._onGroupPivotCycle}
        for toggleId, _hotkey in tonicDockIds.GIZMO_TOGGLES:
            btn = QtWidgets.QToolButton(self._transformRow)
            btn.setText({"orientation": "Global",
                         "groupPivot": "Individual"}[toggleId])
            btn.setCheckable(True)
            _dress(btn, "gizmo", toggleId, TOOL_ICON_PX,
                   QtCore.Qt.ToolButtonIconOnly)
            btn.setFixedHeight(SHELF_BUTTON_HEIGHT)
            btn.setMinimumWidth(SHELF_BUTTON_HEIGHT)
            btn.clicked.connect(
                lambda checked=False, h=handlers[toggleId]: h())
            self._gizmoToggles[toggleId] = btn
            self._buttons[("gizmo", toggleId)] = btn
            transformLayout.addWidget(btn)
        transformLayout.addStretch(1)
        self._transformRow.setVisible(False)
        layout.addWidget(self._transformRow)
        self._instructionLabel = QtWidgets.QLabel()
        self._instructionLabel.setObjectName("tonicInstruction")
        self._instructionLabel.setWordWrap(True)
        self._instructionLabel.setTextFormat(QtCore.Qt.PlainText)
        self._instructionLabel.setStyleSheet("font-size: 11px;")
        layout.addWidget(self._instructionLabel)
        # The active tool's own one-liner (loop.statusLine(): counts,
        # brush radius, fill density...). It was computed per mode and
        # never shown anywhere the artist looks. Pinned like the status
        # strip -- Ignored width, fixed two-line height -- because it can
        # change on every publish and must not re-lay the dock out.
        self._toolStatusLabel = QtWidgets.QLabel()
        self._toolStatusLabel.setObjectName("tonicStatusLine")
        self._toolStatusLabel.setWordWrap(True)
        self._toolStatusLabel.setTextFormat(QtCore.Qt.PlainText)
        self._toolStatusLabel.setStyleSheet("font-size: 11px; color: #9fb4c4;")
        self._toolStatusLabel.setSizePolicy(QtWidgets.QSizePolicy.Ignored,
                                            QtWidgets.QSizePolicy.Fixed)
        self._toolStatusLabel.setFixedHeight(
            self._toolStatusLabel.fontMetrics().height() * 2 + 2)
        layout.addWidget(self._toolStatusLabel)

        paramsBox = QtWidgets.QGroupBox("Parameters")
        self._paramsBox = paramsBox
        paramsOuter = QtWidgets.QVBoxLayout(paramsBox)
        paramsOuter.setContentsMargins(6, 12, 6, 5)
        self._paramsStack = QtWidgets.QStackedWidget()
        self._paramsStack.setObjectName("tonicParametersStack")
        self._paramsStack.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
                                        QtWidgets.QSizePolicy.Maximum)
        paramsOuter.addWidget(self._paramsStack)
        layout.addWidget(paramsBox)

        # The mode's one-shot actions. Save/Export/Import used to sit on
        # Output's page too; they are on the file row now, in every mode.
        actionsBox = QtWidgets.QGroupBox("Actions")
        actionsOuter = QtWidgets.QVBoxLayout(actionsBox)
        actionsOuter.setContentsMargins(6, 12, 6, 5)
        self._actionsStack = QtWidgets.QStackedWidget()
        self._actionsStack.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
                                         QtWidgets.QSizePolicy.Maximum)
        actionsOuter.addWidget(self._actionsStack)
        layout.addWidget(actionsBox)

        # 5. Warnings: hidden while there is nothing to say (DK-08), one
        # severity glyph per row, and a pointing hand over a row whose
        # click selects (or shows) what it is about.
        warningsBox = QtWidgets.QGroupBox("Warnings")
        warningsBox.setObjectName("tonicWarnings")
        warningsLayout = QtWidgets.QVBoxLayout(warningsBox)
        self._warningsList = QtWidgets.QListWidget()
        self._warningsList.setMaximumHeight(96)
        self._warningsList.setIconSize(QtCore.QSize(SHELF_ICON_PX,
                                                    SHELF_ICON_PX))
        self._warningsList.setMouseTracking(True)
        self._warningsList.itemClicked.connect(self._onWarningClicked)
        self._warningsList.itemEntered.connect(self._onWarningHovered)
        self._warningsList.viewportEntered.connect(
            lambda: self._warningsList.viewport().unsetCursor())
        warningsLayout.addWidget(self._warningsList)
        layout.addWidget(warningsBox)
        warningsBox.setVisible(False)
        self._warningsBox = warningsBox

        # 6. Display: what the viewport shows and how it is driven. The two
        # visibility toggles used to sit loose between the breadcrumb and
        # the version dump, and the diagnostics, ladder and navigation
        # switches had no control at all (DK-05).
        displayBox = QtWidgets.QGroupBox("Display", body)
        displayBox.setObjectName("tonicDisplayGroup")
        displayLayout = QtWidgets.QGridLayout(displayBox)
        displayLayout.setContentsMargins(6, 12, 6, 5)
        displayLayout.setHorizontalSpacing(8)
        displayLayout.setVerticalSpacing(2)
        self._generatedCheck = QtWidgets.QCheckBox("Show generated curves")
        self._generatedCheck.setObjectName("tonicShowGeneratedCurves")
        self._generatedCheck.setToolTip(
            "Show the generated guide curves when the active display mode "
            "supports them.")
        self._generatedCheck.toggled.connect(self._onGeneratedToggled)
        displayLayout.addWidget(self._generatedCheck, 0, 0)
        self._amplifiedCheck = QtWidgets.QCheckBox("Show amplified hair")
        self._amplifiedCheck.setObjectName("tonicShowAmplifiedHair")
        self._amplifiedCheck.setToolTip(
            "Grow the full hair from the guides on every commit. Off keeps "
            "the viewport on the guides alone, which is faster.")
        self._amplifiedCheck.toggled.connect(self._onAmplifiedToggled)
        displayLayout.addWidget(self._amplifiedCheck, 0, 1)
        self._diagnosticsCheck = QtWidgets.QCheckBox("Show diagnostics")
        self._diagnosticsCheck.setObjectName("tonicShowDiagnostics")
        self._diagnosticsCheck.setToolTip(
            "Show the model, stage and map versions, the last swap time "
            "and the device under the status strip.")
        self._diagnosticsCheck.toggled.connect(self._onDiagnosticsToggled)
        displayLayout.addWidget(self._diagnosticsCheck, 1, 0)
        self._ladderCheck = QtWidgets.QCheckBox("Ladder enabled")
        self._ladderCheck.setObjectName("tonicLadderEnabled")
        self._ladderCheck.setToolTip(
            "Drop preview detail automatically while a drag runs over its "
            "time budget, and restore it on release.")
        self._ladderCheck.toggled.connect(self._onLadderToggled)
        displayLayout.addWidget(self._ladderCheck, 1, 1)
        navigationRow = QtWidgets.QHBoxLayout()
        navigationRow.setContentsMargins(0, 0, 0, 0)
        navigationRow.addWidget(QtWidgets.QLabel("Navigation"))
        self._navigationCombo = QtWidgets.QComboBox()
        self._navigationCombo.setObjectName("tonicNavigationStyle")
        for styleId, styleLabel in NAVIGATION_STYLES:
            self._navigationCombo.addItem(styleLabel, styleId)
        self._navigationCombo.setToolTip(
            "Maya: Alt+LMB orbits, Alt+MMB pans, Alt+RMB dollies.\n"
            "Blender: also MMB orbits, Shift+MMB pans, Ctrl+MMB dollies.")
        self._navigationCombo.currentIndexChanged.connect(
            self._onNavigationChanged)
        navigationRow.addWidget(self._navigationCombo)
        navigationRow.addStretch(1)
        displayLayout.addLayout(navigationRow, 2, 0, 1, 2)
        layout.addWidget(displayBox)

        # 7. Status strip (DK-05): the last message, then [sync pill]
        # [tool summary] [GPU/CPU chip], then the breadcrumb, then the
        # version dump -- only when Show diagnostics is on. Every label
        # here is pinned (Ignored or fixed width, fixed height): a label
        # whose size hint moves with its text re-runs the dock's nested
        # layouts, and unpinned the strip did that on almost every
        # publish -- 139 Move and 91 Resize events per artist op in the
        # TN-5 controller soak, each through two Python application event
        # filters.
        self._messageLabel = QtWidgets.QLabel()
        self._messageLabel.setObjectName("tonicMessage")
        self._messageLabel.setWordWrap(True)
        self._messageLabel.setTextFormat(QtCore.Qt.PlainText)
        self._messageLabel.setSizePolicy(QtWidgets.QSizePolicy.Ignored,
                                         QtWidgets.QSizePolicy.Fixed)
        self._messageLabel.setFixedHeight(
            self._messageLabel.fontMetrics().height() * 2 + 2)
        layout.addWidget(self._messageLabel)
        self._messageTimer = QtCore.QTimer(self)
        self._messageTimer.setSingleShot(True)
        self._messageTimer.setInterval(MESSAGE_FADE_MS)
        self._messageTimer.timeout.connect(self._clearMessage)

        stripRow = QtWidgets.QHBoxLayout()
        stripRow.setSpacing(4)
        self._syncPill = QtWidgets.QLabel()
        self._syncPill.setObjectName("tonicSyncPill")
        self._syncPill.setTextFormat(QtCore.Qt.PlainText)
        self._syncPill.setAlignment(QtCore.Qt.AlignCenter)
        pillMetrics = self._syncPill.fontMetrics()
        self._syncPill.setFixedSize(
            pillMetrics.horizontalAdvance(PILL_WIDTH_TEXT) + 16,
            pillMetrics.height() + 4)
        stripRow.addWidget(self._syncPill)
        self._statusSummary = QtWidgets.QLabel()
        self._statusSummary.setObjectName("tonicStatusSummary")
        self._statusSummary.setTextFormat(QtCore.Qt.PlainText)
        self._statusSummary.setStyleSheet("font-size: 11px;")
        self._statusSummary.setSizePolicy(QtWidgets.QSizePolicy.Ignored,
                                          QtWidgets.QSizePolicy.Fixed)
        self._statusSummary.setFixedHeight(pillMetrics.height() + 4)
        stripRow.addWidget(self._statusSummary, 1)
        self._gpuChip = QtWidgets.QLabel("GPU")
        self._gpuChip.setObjectName("tonicGpuChip")
        self._gpuChip.setTextFormat(QtCore.Qt.PlainText)
        self._gpuChip.setAlignment(QtCore.Qt.AlignCenter)
        self._gpuChip.setFixedSize(pillMetrics.horizontalAdvance("GPU") + 14,
                                   pillMetrics.height() + 4)
        stripRow.addWidget(self._gpuChip)
        layout.addLayout(stripRow)

        # The breadcrumb (plan/18 section 3.5 item 6: "breadcrumb
        # (clickable)"), DK-08: flat buttons Groom > Tube 0, the frontier
        # "L2 (4 children)" as text, and an Exit level arrow at the left.
        # A crumb click is the same edit the Enter/Exit keys make.
        self._breadcrumbBar = _BreadcrumbBar(self._onBreadcrumbClicked,
                                             self._onExitLevelClicked)
        layout.addWidget(self._breadcrumbBar)
        # The version dump: a full-width PLAIN-text label, because it
        # changes on almost every publish and rich text would re-parse the
        # HTML each time. It is developer telemetry, so it hides unless
        # Show diagnostics is on; the pill's tooltip carries the same text.
        statusDetailRow = QtWidgets.QHBoxLayout()
        self._statusDetail = QtWidgets.QLabel()
        self._statusDetail.setObjectName("tonicDiagnostics")
        self._statusDetail.setWordWrap(True)
        self._statusDetail.setTextFormat(QtCore.Qt.PlainText)
        self._statusDetail.setStyleSheet("font-size: 11px;")
        self._statusDetail.setSizePolicy(QtWidgets.QSizePolicy.Ignored,
                                         QtWidgets.QSizePolicy.Fixed)
        self._statusDetail.setFixedHeight(
            self._statusDetail.fontMetrics().height() * STATUS_LINES)
        self._statusDetail.setVisible(False)
        statusDetailRow.addWidget(self._statusDetail, 1)
        layout.addLayout(statusDetailRow)

        layout.addStretch(1)
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(body)
        self._scroll = scroll
        self.setWidget(scroll)
        self.setMinimumWidth(320)

    # ---- mode / sub-mode switching ----------------------------------------

    def _setActiveMode(self, modeId):
        """Select `modeId`: records it on state, drives the viewport and
        shows the mode's cached pages below the shelf. Each mode remembers
        its last sub-mode (the controller only falls back to the mode's
        default when none was ever chosen); with no scalp bound the switch
        is refused and the current mode stays."""
        state = self._container.tonicState
        viewport = getattr(self._container, "viewport", None)
        if state.activeMode and not self._scalpBound():
            # The controller refuses every switch until a scalp is bound
            # and says why; recording the mode here anyway (the shim
            # fallback below) would show a tool that never started.
            if (modeId != state.activeMode and viewport is not None and
                    hasattr(viewport, "setMode")):
                viewport.setMode(modeId)
            self._adoptMode(state.activeMode)
            self.refresh()
            return
        if viewport is not None and hasattr(viewport, "setMode"):
            # ViewportController owns the live-loop transition and default
            # sub-mode, including its display-policy publication.
            viewport.setMode(modeId)
            if state.activeMode != modeId:
                # Keep lightweight test/fallback viewport shims stateful too.
                tonicModes.SetActiveMode(state, modeId)
                submodes = _SUBMODES_BY_MODE.get(modeId)
                if submodes is not None:
                    submodes[1](state, "")
        else:
            tonicModes.SetActiveMode(state, modeId)
            submodes = _SUBMODES_BY_MODE.get(modeId)
            if submodes is not None:
                submodes[1](state, "")
        self._adoptMode(modeId)
        self.refresh()

    def _ensurePage(self, modeId, state):
        """The cached page set for `modeId`, built or rebuilt if needed."""
        page = self._pages.get(modeId)
        signature = _paramSignature(tonicPanels.descriptors(modeId, state))
        if page is not None and page["paramSignature"] == signature:
            return page
        if page is not None:
            # The rows themselves changed (Output after its build, Sculpt
            # on a Smooth switch). Replace that one page; nothing
            # else in the dock is touched.
            old = page["params"]
            self._paramsStack.removeWidget(old)
            old.setParent(None)
            page["params"] = None
        else:
            subPage, subButtons, subGroup = self._buildSubModePage(modeId)
            _hidePage(subPage)
            self._subModeStack.addWidget(subPage)
            actionPage, actionButtons = self._buildActionPage(modeId)
            _hidePage(actionPage)
            self._actionsStack.addWidget(actionPage)
            page = {"subMode": subPage, "subButtons": subButtons,
                    "subGroup": subGroup, "actions": actionPage,
                    "actionButtons": actionButtons,
                    "hasSubModes": modeId in _SUBMODES_BY_MODE}
            self._pages[modeId] = page
        paramPage, rows, offForm, signature = self._buildParamPage(modeId,
                                                                   state)
        # A page built for button() or a rebuilt inactive page must not
        # size the stack; _showPage turns the shown one back on.
        _hidePage(paramPage)
        self._paramsStack.addWidget(paramPage)
        page["params"] = paramPage
        page["paramRows"] = rows
        page["offForm"] = offForm
        page["paramSignature"] = signature
        return page

    def _adoptMode(self, modeId):
        """Show the panel for `modeId` without driving anything.

        The half of _setActiveMode that is about widgets. refresh() calls
        it when the mode changed somewhere else, which is the usual case:
        the number keys go to the viewport controller, not to this dock
        (plan/18 section 3.4).
        """
        self._activeMode = modeId
        # The shelf below is about to change, so whatever refresh() last
        # synced is gone: force the next sync.
        self._shelfKey = None
        self._syncModeButtons()
        state = self._container.tonicState
        page = self._ensurePage(modeId, state)
        self._pageLevel = int(getattr(state, "activeLevel", 0))
        # _showPage, not setCurrentWidget + setFixedHeight: cached pages
        # would otherwise size each stack to its tallest page (blank gaps
        # around short forms), and the fixed heights that fixed that were
        # short enough on some pages for the group titles to overlap.
        _showPage(self._subModeStack, page["subMode"])
        # Tube's F8--F11 row is the sole visible component shelf. Keep the
        # legacy sub-mode page cached for loop state, but do not duplicate it
        # beside the compact selection buttons.
        showSubModes = page["hasSubModes"] and modeId != "tube"
        self._subModeStack.setVisible(showSubModes)
        self._tubeSelectionRow.setVisible(modeId == "tube")
        self._transformRow.setVisible(
            modeId in tonicDockIds.TRANSFORM_MODES)
        self._toolKey = None
        self._syncTubeSelectionButtons()
        _showPage(self._paramsStack, page["params"])
        _showPage(self._actionsStack, page["actions"])
        self._subModeButtons = page["subButtons"]
        self._subModeGroup = page["subGroup"]
        self._paramWidgets = page["paramRows"]
        self._offFormIds = tuple(page["offForm"])
        self._syncInstruction()

    def _syncModeButtons(self):
        for modeId, btn in self._modeButtons.items():
            btn.blockSignals(True)
            btn.setChecked(modeId == self._activeMode)
            btn.blockSignals(False)

    def _setTubeSelectionKind(self, kind):
        if kind not in self._tubeSelectionButtons:
            return
        state = self._container.tonicState
        state.tubeSelectionKind = kind
        viewport = getattr(self._container, "viewport", None)
        setter = getattr(viewport, "setSelectionKind", None)
        if callable(setter):
            setter(kind)
        self._syncTubeSelectionButtons()
        self.refresh()

    def _syncTubeSelectionButtons(self):
        if not self._tubeSelectionButtons:
            return
        current = str(getattr(self._container.tonicState,
                              "tubeSelectionKind", "tube"))
        if current not in self._tubeSelectionButtons:
            current = "tube"
        for kind, btn in self._tubeSelectionButtons.items():
            btn.blockSignals(True)
            btn.setChecked(kind == current)
            btn.blockSignals(False)

    def _buildSubModePage(self, modeId):
        """The sub-mode shelf page for modeId, built once."""
        page = QtWidgets.QWidget()
        row = QtWidgets.QGridLayout(page)
        row.setContentsMargins(0, 0, 0, 0)
        row.setHorizontalSpacing(3)
        row.setVerticalSpacing(3)
        buttons = {}
        group = None
        entry = _SUBMODES_BY_MODE.get(modeId)
        if entry is not None:
            submodes, setter = entry
            group = QtWidgets.QButtonGroup(page)
            group.setExclusive(True)
            for index, sub in enumerate(submodes):
                btn = QtWidgets.QToolButton()
                # Label only, glyph beside it, key in the tooltip (DK-04);
                # two columns so a label and its glyph fit a 350 px dock.
                btn.setText(sub.label)
                btn.setCheckable(True)
                if modeId != "tube":
                    # Tube's page stays hidden behind the F8-F11 row (the
                    # "comp" buttons), so it gets no name or art of its
                    # own: one objectName, one visible button.
                    _dress(btn, "sub", sub.id, SHELF_ICON_PX,
                           QtCore.Qt.ToolButtonTextBesideIcon)
                btn.setFixedHeight(SHELF_BUTTON_HEIGHT)
                btn.setSizePolicy(QtWidgets.QSizePolicy.Expanding,
                                  QtWidgets.QSizePolicy.Fixed)
                btn.setToolTip(_shelfTip(sub))
                btn.clicked.connect(
                    lambda checked, s=sub.id, fn=setter:
                    self._onSubMode(fn, s))
                group.addButton(btn)
                buttons[sub.id] = btn
                row.addWidget(btn, index // 2, index % 2)
        return page, buttons, group

    def _onSubMode(self, setter, subId):
        state = self._container.tonicState
        viewport = getattr(self._container, "viewport", None)
        if viewport is not None and hasattr(viewport, "setSubMode"):
            # The loop must see the old state before clearing transient
            # drafts during a sub-mode transition.
            viewport.setSubMode(subId)
            attr = _SUBMODE_STATE_ATTR.get(state.activeMode)
            if attr and getattr(state, attr, "") != subId:
                # A minimal viewport shim may only record the call.
                setter(state, subId)
        else:
            setter(state, subId)
        self._syncSubModeButtons()
        self._syncInstruction()
        self.refresh()

    def _syncSubModeButtons(self):
        attr = _SUBMODE_STATE_ATTR.get(self._activeMode)
        current = getattr(self._container.tonicState, attr, "") if attr \
            else ""
        for subId, btn in self._subModeButtons.items():
            btn.blockSignals(True)
            btn.setChecked(subId == current)
            btn.blockSignals(False)

    def _syncInstruction(self):
        """Show the short interaction recipe for the active tool."""
        # Every tool has a line now (DK-02); it used to exist only for
        # Graph's Create region and for Tube/Fill, and the rest of the
        # shelf left the artist guessing which keys and clicks did what.
        state = self._container.tonicState
        attr = _SUBMODE_STATE_ATTR.get(self._activeMode)
        subMode = getattr(state, attr, "") if attr else ""
        if self._activeMode == "tube":
            # The F8--F11 row, not tubeSubMode's shelf, is what is shown.
            subMode = getattr(state, "tubeSelectionKind", "") or subMode
        text = tonicModes.hintFor(self._activeMode, subMode)
        if text and self._activeMode in tonicModes.SELECTION_MODES:
            text = "%s\n%s" % (text, tonicModes.SELECTION_HINT)
        if not self._scalpBound():
            # Every tool is greyed out until Bind (DK-01); the line says
            # what to do first, not how a tool that cannot act would.
            text = tonicModes.UNBOUND_HINT
        if text != self._instructionText:
            self._instructionText = text
            self._instructionLabel.setText(text)
            self._instructionLabel.setVisible(bool(text))

    # ---- parameters --------------------------------------------------

    def _buildParamPage(self, modeId, state):
        """The generated parameter form for `modeId`, built once.

        Returns the page, its [(descriptor, widget)] rows, the ids of the
        descriptors drawn somewhere other than the form (_OFF_FORM_IDS)
        and the signature the rows were built from. A few builders read
        `state` (Sculpt's strength range follows the brush, Output's rows
        follow the build), so the signature is what decides whether a
        cached page is stale. The active level is NOT part of it any more:
        the level rows name it in a refreshed tooltip (DK-06).
        """
        page = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(page)
        form.setContentsMargins(0, 0, 0, 0)
        rows = []
        offForm = []
        descriptors = tonicPanels.descriptors(modeId, state)
        for descriptor in descriptors:
            if descriptor.id in _OFF_FORM_IDS:
                offForm.append(descriptor.id)
                continue
            widget = self._makeParamWidget(descriptor)
            field = self._paramField(descriptor, widget)
            form.addRow(descriptor.label, field)
            if descriptor.id == "texelResolution":
                widget.setObjectName("tonicTexelResolution")
            elif descriptor.id == "uniformScale":
                widget.setObjectName("tonicUniformSectionScale")
            # The row's label shares the widget's tooltip and greying
            # (DK-07); a slider row's label belongs to its container.
            widget._tonicLabel = form.labelForField(field)
            self._applyRowChrome(descriptor, widget,
                                 getattr(descriptor, "tooltip", ""),
                                 getattr(descriptor, "enabled", True))
            rows.append((descriptor, widget))
        return page, rows, offForm, _paramSignature(descriptors)

    @staticmethod
    def _applyRowChrome(descriptor, widget, tooltip, enabled):
        """Tooltip and enabled state on a row's widget, label and slider.

        Only what changed is touched: this runs on every refresh, and a
        setToolTip/setEnabled per row per 250 ms tick would repaint the
        dock for nothing. A refused ramp keeps its red 'not a length
        profile' tooltip until the artist edits it."""
        tooltip = str(tooltip or "")
        enabled = bool(enabled)
        parts = [widget, getattr(widget, "_tonicLabel", None),
                 getattr(widget, "_tonicSlider", None)]
        keepTip = (descriptor.kind == "ramp" and
                   bool(getattr(widget, "_tonicInvalid", False)))
        widget._tonicTip = tooltip
        for part in parts:
            if part is None:
                continue
            if not keepTip and tooltip and part.toolTip() != tooltip:
                part.setToolTip(tooltip)
            if part.isEnabled() != enabled:
                part.setEnabled(enabled)

    def _makeParamWidget(self, descriptor):
        # Spin boxes commit on Enter, focus-out, a step arrow or the paired
        # slider -- never per keystroke (DK-06). With keyboard tracking on,
        # typing '12' into Density set 1 and then 12: two refills, two
        # publishes and a one-guide-per-unit flash in between.
        if descriptor.kind == "int":
            w = QtWidgets.QSpinBox()
            w.setKeyboardTracking(False)
            w.setRange(int(descriptor.min), int(descriptor.max))
            w.setSingleStep(max(1, int(descriptor.step or 1)))
            w.valueChanged.connect(
                lambda v, d=descriptor, spin=w: self._commitSpin(d, spin, v))
            w.editingFinished.connect(
                lambda d=descriptor, spin=w:
                self._onSpinEditingFinished(d, spin))
            return w
        if descriptor.kind == "float":
            w = QtWidgets.QDoubleSpinBox()
            w.setKeyboardTracking(False)
            w.setRange(float(descriptor.min), float(descriptor.max))
            w.setSingleStep(float(descriptor.step or 0.1))
            w.setDecimals(4)
            unit = getattr(descriptor, "unit", "")
            if unit:
                # '%' reads as part of the number; words and 'px' do not.
                w.setSuffix(unit if unit == "%" else " " + unit)
            w.valueChanged.connect(
                lambda v, d=descriptor, spin=w: self._commitSpin(d, spin, v))
            w.editingFinished.connect(
                lambda d=descriptor, spin=w:
                self._onSpinEditingFinished(d, spin))
            return w
        if descriptor.kind == "bool":
            w = QtWidgets.QCheckBox()
            # stateChanged, not toggled: a mixed row (tonicPanels.MIXED)
            # is shown part-checked, which Qt counts as checked, so the
            # click that takes it to fully checked never toggles.
            w.stateChanged.connect(
                lambda _s, d=descriptor, box=w: self._onCheckChanged(d, box))
            return w
        if descriptor.kind == "enum":
            w = QtWidgets.QComboBox()
            # The artist reads the choice label ('K-means', '128 x 128');
            # the stored choice rides along as item data, which is what
            # the descriptor gets and what refresh looks up.
            labels = tuple(getattr(descriptor, "choiceLabels", ()) or ())
            for i, choice in enumerate(descriptor.choices):
                display = labels[i] if i < len(labels) else str(choice)
                w.addItem(display, choice)
            w.currentIndexChanged.connect(
                lambda i, d=descriptor, box=w:
                self._onParamChanged(d, box.itemData(i)))
            return w
        if descriptor.kind == "ramp":
            w = QtWidgets.QLineEdit()
            w.setPlaceholderText(tonicPanels.RAMP_PLACEHOLDER)
            w.setToolTip(tonicPanels.RAMP_HINT)
            w.editingFinished.connect(
                lambda d=descriptor, edit=w: self._onRampEdited(d, edit))
            w.textEdited.connect(
                lambda _text, edit=w: self._markRampInvalid(edit, False))
            return w
        raise ValueError("tonicWorkspace: unknown descriptor kind %r"
                         % (descriptor.kind,))

    def _paramField(self, descriptor, widget):
        """What the form row holds: the widget, or it with a slider/readout.

        Bounded floats get a paired slider (tonicPanels.SLIDER_PARAM_IDS);
        Density also gets the guide count it produced, because '/unit²'
        alone does not tell the artist whether 12 is a lot. The spin box
        stays the row's widget in _paramWidgets; the extras hang off it.
        """
        slider = None
        if (descriptor.kind == "float" and
                descriptor.id in tonicPanels.SLIDER_PARAM_IDS and
                float(descriptor.max) > float(descriptor.min)):
            slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
            slider.setObjectName("tonicSlider_%s" % descriptor.id)
            slider.setRange(0, SLIDER_STEPS)
            slider.setToolTip(descriptor.label)
            # A drag must not take the keyboard from the viewport's
            # hotkeys; the spin box beside it is the keyboard route.
            slider.setFocusPolicy(QtCore.Qt.NoFocus)
            slider.valueChanged.connect(
                lambda pos, d=descriptor, spin=widget:
                self._onSliderMoved(d, spin, pos))
            slider.sliderPressed.connect(
                lambda d=descriptor, spin=widget:
                self._onSliderPressed(d, spin))
            slider.sliderReleased.connect(
                lambda d=descriptor, spin=widget:
                self._onSliderReleased(d, spin))
        readout = None
        if descriptor.id == "density":
            readout = QtWidgets.QLabel("")
            readout.setObjectName("tonicDensityReadout")
            readout.setStyleSheet("QLabel { color: #9aa4ad; }")
            readout.setToolTip(
                "Guides the current density grows over the filled tubes.")
        widget._tonicSlider = slider
        widget._tonicReadout = readout
        if slider is None and readout is None:
            return widget
        field = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(field)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(1)
        row = QtWidgets.QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(widget, 1)
        if slider is not None:
            row.addWidget(slider, 1)
        column.addLayout(row)
        if readout is not None:
            column.addWidget(readout)
        return field

    # ---- spin box / slider / ramp commits (DK-06) -----------------------

    def _commitSpin(self, descriptor, widget, value):
        """One committed spin value -> one descriptor set and publish.

        Enter emits valueChanged and then editingFinished, and the focus
        loss that follows emits editingFinished again; the last committed
        value is what keeps that to exactly one set."""
        if getattr(widget, "_tonicCommitted", None) == value:
            return
        widget._tonicCommitted = value
        self._syncSlider(descriptor, widget)
        self._onParamChanged(descriptor, value)

    def _onSpinEditingFinished(self, descriptor, widget):
        self._commitSpin(descriptor, widget, widget.value())
        # Hand the keyboard back: a focused spin box keeps eating the
        # number and letter keys the shelf and the viewport answer to.
        if widget.hasFocus():
            widget.clearFocus()

    def _syncSlider(self, descriptor, widget):
        slider = getattr(widget, "_tonicSlider", None)
        if slider is None or slider.isSliderDown():
            # Under the mouse the slider is the source; nudging it to the
            # spin box's rounded value would make the handle jitter.
            return
        pos = _valueToSlider(descriptor, widget.value())
        if pos != slider.value():
            slider.blockSignals(True)
            slider.setValue(pos)
            slider.blockSignals(False)

    def _onSliderMoved(self, descriptor, widget, pos):
        # The spin box is the one commit path: its valueChanged runs
        # _commitSpin, so a slider step and a typed value are the same edit.
        widget.setValue(_sliderToValue(descriptor, pos))

    def _onSliderPressed(self, descriptor, widget):
        """Open one gesture for the drag, so it is one undo step.

        Only rows that write undoable model state are bracketed (Fill's
        density and edge bias: their refill lands in the undo snapshot).
        The other slider rows are tool settings or model display values
        that undo does not restore; a bracket around them would leave an
        undo step that changes nothing."""
        widget._tonicPressValue = widget.value()
        widget._tonicGesture = False
        if descriptor.id not in tonicPanels.SLIDER_GESTURE_IDS:
            return
        session = getattr(self._container, "session", None)
        begin = getattr(session, "beginGesture", None)
        if callable(begin) and getattr(session, "model", None) is not None:
            widget._tonicGesture = bool(begin("Edit %s" % descriptor.label))

    def _onSliderReleased(self, descriptor, widget):
        session = getattr(self._container, "session", None)
        if getattr(widget, "_tonicGesture", False):
            widget._tonicGesture = False
            if widget.value() == getattr(widget, "_tonicPressValue", None):
                # A click that did not move the value: no undo step.
                dirty = session.cancelGesture()
                if dirty:
                    session.publish(dirty)
            else:
                session.endGesture()
        self._syncSlider(descriptor, widget)
        self.refresh()

    def _onRampEdited(self, descriptor, edit):
        """Commit a length profile, or refuse a typo without raising.

        A bad token used to raise out of the editingFinished slot (a
        traceback per Enter); now the field turns red, says what it takes
        and nothing is written."""
        text = edit.text()
        if text == getattr(edit, "_tonicCommitted", None) and \
                not getattr(edit, "_tonicInvalid", False):
            return
        try:
            pairs = _text_to_ramp(text)
            session = getattr(self._container, "session", None)
            # The descriptor validates the pairs before it writes anything.
            descriptor.set(self._container.tonicState, session, pairs)
        except ValueError:
            self._markRampInvalid(edit, True)
            return
        self._markRampInvalid(edit, False)
        edit._tonicCommitted = text
        self._publishUiEdit(getattr(self._container, "session", None))
        if edit.hasFocus():
            edit.clearFocus()
        self.refresh()

    def _markRampInvalid(self, edit, invalid):
        if bool(getattr(edit, "_tonicInvalid", False)) == bool(invalid):
            return
        edit._tonicInvalid = bool(invalid)
        if invalid:
            edit.setStyleSheet("QLineEdit { border: 1px solid %s; }"
                               % _MESSAGE_COLORS["error"])
            edit.setToolTip("Not a length profile. Use %s."
                            % tonicPanels.RAMP_HINT)
        else:
            edit.setStyleSheet("")
            # Back to the row's own tooltip (it quotes RAMP_HINT too).
            edit.setToolTip(getattr(edit, "_tonicTip", "") or
                            tonicPanels.RAMP_HINT)

    @staticmethod
    def _paramEditing(descriptor, widget):
        """Whether the artist is mid-edit in `widget` (refresh leaves it).

        Keyboard tracking is off, so a half-typed spin value is not the
        widget's value yet; the 250 ms refresh would otherwise overwrite
        the '1' of '12' before Enter. A refused ramp keeps its red text
        until the artist edits it."""
        if descriptor.kind == "ramp":
            return bool(getattr(widget, "_tonicInvalid", False)) or (
                widget.hasFocus() and widget.isModified())
        if descriptor.kind not in ("int", "float") or not widget.hasFocus():
            return False
        try:
            if descriptor.kind == "float":
                shown = widget.textFromValue(widget.value())
            else:
                shown = str(widget.value())
            return widget.cleanText().strip() != str(shown).strip()
        except (AttributeError, TypeError, RuntimeError):
            return True

    def _onCheckChanged(self, descriptor, box):
        """A bool row's click. Part-checked is only ever shown (the tubes
        the row speaks for disagree); a click on it checks the box, and
        that writes every one of them."""
        if box.checkState() == QtCore.Qt.CheckState.PartiallyChecked:
            return
        box.setTristate(False)
        self._onParamChanged(descriptor, box.isChecked())

    def _onParamChanged(self, descriptor, value):
        session = getattr(self._container, "session", None)
        descriptor.set(self._container.tonicState, session, value)
        if descriptor.id == "transformTool":
            # The descriptor owns the state value; the active TubeLoop owns
            # the gizmo placement and selection redraw.  Keep this bridge in
            # the dock because descriptors intentionally receive only the
            # Qt-free session, not the viewport container.
            viewport = getattr(self._container, "viewport", None)
            loop = (getattr(viewport, "loop", None)
                    if viewport is not None else None)
            applyTool = getattr(loop, "setTransformTool", None)
            if callable(applyTool):
                applyTool(value)
        self._publishUiEdit(session)
        self.refresh()

    # ---- the transform row (DK-04) ------------------------------------

    def _onTransformTool(self, tool):
        """A Q/W/E/R row click: the same edit the key makes.

        In Hierarchy it is also the key's jump (tonicViewport's
        _runTubeShortcut): Tube, whole-tube components, the hierarchy's
        selected tubes kept -- the Tube sub-mode is set before the mode
        switch so the new loop never clears that selection. The tool
        itself goes through the transformTool descriptor and the loop's
        setTransformTool bridge in _onParamChanged, as the combo did."""
        state = self._container.tonicState
        viewport = getattr(self._container, "viewport", None)
        fromHierarchy = self._activeMode == "hierarchy"
        if fromHierarchy:
            tonicModes.SetActiveTubeSubMode(state, "tube")
            self._setActiveMode("tube")
        if self._activeMode != "tube":
            self._toolKey = None
            self._syncToolButtons(getattr(self._container, "session", None))
            return
        descriptor = next(
            (d for d in tonicPanels.descriptors("tube", state)
             if d.id == "transformTool"), None)
        if descriptor is None:
            return
        self._onParamChanged(descriptor, tool)
        # The key path redraws the Qt gizmo overlay itself; a dock click
        # has to ask, or the old tool's handles stay up until the pointer
        # next moves over the view.
        syncOverlay = getattr(viewport, "_syncGizmoOverlay", None)
        if callable(syncOverlay):
            syncOverlay()
        self._container._status(self._api, "Tonic Tube: %s%s" % (
            str(tool).title(), " (from Hierarchy)" if fromHierarchy else ""))

    def _syncToolButtons(self, session):
        """Check the state's transform tool; gate Hierarchy on a tube."""
        mode = self._activeMode
        if mode not in tonicDockIds.TRANSFORM_MODES:
            return
        self._syncGizmoToggles(self._container.tonicState)
        tool = str(getattr(self._container.tonicState, "transformTool",
                           "move")).lower()
        enabled = True
        if mode == "hierarchy":
            # The jump carries the hierarchy's selected tubes into Tube;
            # with none selected it would land on an empty gizmo.
            count = getattr(session, "selectionCount", None)
            try:
                enabled = bool(callable(count) and
                               count(tonicLib.TONIC_PICK_TUBE_VERT) > 0)
            except (AttributeError, TypeError, ValueError, RuntimeError,
                    OSError):
                enabled = False
        key = (mode, tool, enabled)
        if key == self._toolKey:
            return
        self._toolKey = key
        for toolId, btn in self._toolButtons.items():
            btn.blockSignals(True)
            btn.setChecked(toolId == tool)
            btn.blockSignals(False)
            btn.setEnabled(enabled)

    # ---- the transform row's toggles (parity G16/G17/G21) --------------

    def _tubeLoop(self):
        """The viewport's live TubeLoop, or None outside Tube."""
        viewport = getattr(self._container, "viewport", None)
        loop = getattr(viewport, "loop", None) if viewport is not None \
            else None
        if loop is None or getattr(loop, "modeId", "") != "tube":
            return None
        return loop

    def _afterGizmoToggle(self):
        """Redraw the handles and the dock after a toggle click."""
        viewport = getattr(self._container, "viewport", None)
        syncOverlay = getattr(viewport, "_syncGizmoOverlay", None)
        if callable(syncOverlay):
            syncOverlay()
        self._publishUiEdit(getattr(self._container, "session", None))
        self.refresh()

    def _onOrientationToggle(self):
        """The Global/Local button: `L`'s twin for the live tool.

        With a Tube loop live it goes through the loop (it re-places the
        gizmo and refuses mid-drag); in Hierarchy, where the row also
        shows, the state is set directly and Tube reads it on entry."""
        state = self._container.tonicState
        loop = self._tubeLoop()
        toggle = getattr(loop, "toggleOrientation", None)
        if callable(toggle):
            toggle()
        else:
            new = tonicGizmoSettings.NextToggleOrientation(
                tonicGizmoSettings.NormalizeOrientation(
                    getattr(state, "transformOrientation", "world")))
            state.transformOrientation = new
            tonicGizmoSettings.settingsFor(state).Notify()
            self._container._status(self._api, "Tonic Tube: %s orientation "
                                    "(%s)" % (
                                        tonicGizmoSettings.ToggleLabel(new),
                                        tonicGizmoSettings.OrientationLabel(
                                            new)))
        self._afterGizmoToggle()

    def _onGroupPivotCycle(self):
        """The group-pivot button: `P`'s twin (Rotate/Scale only)."""
        state = self._container.tonicState
        loop = self._tubeLoop()
        cycle = getattr(loop, "cycleGroupPivot", None)
        if callable(cycle):
            cycle()
        else:
            tool = getattr(state, "transformTool", "move")
            settings = tonicGizmoSettings.settingsFor(state).For(tool)
            if tonicGizmoSettings.GroupPivotChoices(tool):
                settings.groupPivot = tonicGizmoSettings.NextGroupPivot(
                    settings.groupPivot, tool)
        self._afterGizmoToggle()

    def _gizmoIcon(self, name):
        """A cached QIcon for a toggle glyph (the icon swaps with state)."""
        if name not in self._gizmoIconCache:
            self._gizmoIconCache[name] = _loadIcon(name)
        return self._gizmoIconCache[name]

    def _syncGizmoToggles(self, state):
        """Put the Global/Local and group-pivot buttons on the state.

        The glyph and the text name the setting actually in force; checked
        means "not the default" (Local, Selection Centre), the usdRig
        toolbar's convention. Orientation greys out under Select (no
        handles), the pivot under Move/Select (GroupPivotChoices)."""
        tool = str(getattr(state, "transformTool", "move")).lower()
        orientation = tonicGizmoSettings.NormalizeOrientation(
            getattr(state, "transformOrientation", "world"))
        choices = tonicGizmoSettings.GroupPivotChoices(tool)
        pivot = tonicGizmoSettings.settingsFor(state).For(tool).groupPivot
        key = (tool, orientation, pivot, bool(choices))
        if key == self._gizmoToggleKey:
            return
        self._gizmoToggleKey = key
        orientButton = self._gizmoToggles["orientation"]
        local = orientation == tonicGizmoSettings.ORIENT_TUBE
        orientButton.blockSignals(True)
        orientButton.setChecked(local)
        orientButton.blockSignals(False)
        orientButton.setEnabled(tool != "select")
        orientButton.setText(tonicGizmoSettings.ToggleLabel(orientation))
        icon = self._gizmoIcon(tonicDockIds.ORIENT_ICONS.get(orientation))
        if icon is not None:
            orientButton.setIcon(icon)
        there = tonicGizmoSettings.NextToggleOrientation(orientation)
        if tool == "select":
            orientTip = ("Axis orientation (L): pick Move, Rotate or Scale "
                         "first.")
        else:
            orientTip = ("Axis orientation (L): %s -- %s. Click or press L "
                         "for %s. Each tool keeps its own (Rotate starts "
                         "Local)." % (
                             tonicGizmoSettings.OrientationLabel(orientation),
                             "the handles follow the tube's root direction"
                             if local else
                             "the handles are on the scene axes"
                             if orientation == tonicGizmoSettings.ORIENT_WORLD
                             else "the handles face the view",
                             tonicGizmoSettings.ToggleLabel(there)))
        orientButton.setToolTip(orientTip)
        pivotButton = self._gizmoToggles["groupPivot"]
        centre = bool(choices) and \
            pivot == tonicGizmoSettings.GROUP_PIVOT_CENTRE
        pivotButton.blockSignals(True)
        pivotButton.setChecked(centre)
        pivotButton.blockSignals(False)
        pivotButton.setEnabled(bool(choices))
        pivotButton.setText(tonicGizmoSettings.GroupPivotLabel(pivot))
        icon = self._gizmoIcon(tonicDockIds.PIVOT_ICONS.get(pivot))
        if icon is not None:
            pivotButton.setIcon(icon)
        if not choices:
            pivotTip = ("Group pivot (P): where Rotate and Scale turn "
                        "several selected items about. Not used by %s."
                        % tool.title())
        else:
            pivotTip = ("Group pivot (P): %s -- %s. Click or press P for "
                        "the next one." % (
                            tonicGizmoSettings.GroupPivotLabel(pivot),
                            "everything turns about ONE point, the middle "
                            "of the selection" if centre else
                            "each tube turns about its own root, each ring "
                            "about its own centre"))
        pivotButton.setToolTip(pivotTip)

    @staticmethod
    def _publishUiEdit(session):
        """Flush one dock edit and request an immediate usdview repaint."""
        if session is None:
            return
        publish = getattr(session, "publish", None)
        if callable(publish):
            publish()
            return
        refreshViewport = getattr(session, "refreshViewport", None)
        if callable(refreshViewport):
            refreshViewport()

    def _refreshParams(self, session):
        state = self._container.tonicState
        for descriptor, widget in self._paramWidgets:
            readout = getattr(widget, "_tonicReadout", None)
            if readout is not None:
                text = tonicPanels.guideCountText(session)
                if text != readout.text():
                    readout.setText(text)
            if self._paramEditing(descriptor, widget):
                continue
            value = descriptor.get(state, session)
            widget.blockSignals(True)
            if descriptor.kind == "int":
                widget.setValue(int(value))
                widget._tonicCommitted = widget.value()
            elif descriptor.kind == "float":
                widget.setValue(float(value))
                widget._tonicCommitted = widget.value()
                self._syncSlider(descriptor, widget)
            elif descriptor.kind == "bool":
                if isinstance(value, str) and value == tonicPanels.MIXED:
                    widget.setTristate(True)
                    widget.setCheckState(
                        QtCore.Qt.CheckState.PartiallyChecked)
                else:
                    widget.setTristate(False)
                    widget.setChecked(bool(value))
            elif descriptor.kind == "enum":
                idx = widget.findData(value)
                widget.setCurrentIndex(max(idx, 0))
            elif descriptor.kind == "ramp":
                text = _ramp_to_text(value)
                if text != widget.text():
                    widget.setText(text)
                widget._tonicCommitted = text
            widget.blockSignals(False)
        # Tooltips and greying follow state (DK-07): the level rows name
        # the level being edited, Output's rows wait for Build, Brush reach
        # waits for Whole strand to be unticked. Read after the gets above,
        # which are what bring e.g. state.outputEnabled up to date.
        current = {d.id: d for d in
                   tonicPanels.descriptors(self._activeMode, state)}
        for descriptor, widget in self._paramWidgets:
            fresh = current.get(descriptor.id, descriptor)
            tooltip = getattr(fresh, "tooltip", "")
            enabled = getattr(fresh, "enabled", True)
            if descriptor.id == "uniformScale":
                # Live only with section rings (or section CVs) selected;
                # descriptors() sees state, not the session's selection.
                enabled = bool(tonicPanels._selectedSectionRings(session))
                tooltip = (tonicPanels.UNIFORM_SCALE_TIP if enabled
                           else tonicPanels.UNIFORM_SCALE_EMPTY_TIP)
            self._applyRowChrome(descriptor, widget, tooltip, enabled)

    # ---- geometry --------------------------------------------------------

    def _scalpBound(self):
        """Whether the session has a live model on a bound scalp."""
        session = getattr(self._container, "session", None)
        return getattr(session, "model", None) is not None

    def _stageMeshPaths(self):
        """Every Mesh prim path in the current stage, for the scalp picker,
        plus every face GeomSubset directly under a Mesh (plan/02 2.20).

        Listed by schema type only (a subset also by its elementType token,
        one uniform read). The full topology check (__init__._validMeshPrim)
        reads every point and face index, which is far too slow to run over
        a production stage just to fill a combo box, so it runs once, on the
        path the artist accepts.
        """
        stage = getattr(self._api, "stage", None)
        if stage is None:
            return []
        try:
            from pxr import UsdGeom
            prims = stage.Traverse()
        except (ImportError, AttributeError):
            return []
        paths = []
        for prim in prims:
            try:
                if prim.IsA(UsdGeom.Mesh):
                    paths.append(str(prim.GetPath()))
                elif prim.IsA(UsdGeom.Subset) and \
                        prim.GetParent().IsA(UsdGeom.Mesh) and \
                        str(UsdGeom.Subset(prim).GetElementTypeAttr().Get()
                            or "") == "face":
                    paths.append(str(prim.GetPath()))
            except (AttributeError, TypeError, ValueError, RuntimeError):
                continue
        return sorted(set(paths))

    def _pickScalpPath(self, paths, session):
        """The modal scalp picker; the chosen path, or "" on cancel."""
        dialog = QtWidgets.QDialog(self)
        dialog.setObjectName("tonicGeometryPicker")
        dialog.setWindowTitle("Bind scalp mesh")
        dialog.setModal(True)
        column = QtWidgets.QVBoxLayout(dialog)
        column.addWidget(QtWidgets.QLabel("Choose the scalp Mesh:"))
        combo = QtWidgets.QComboBox(dialog)
        combo.setObjectName("tonicGeometryChoices")
        combo.addItems(paths)
        # The viewport selection first: the first-run hint tells the artist
        # to select the scalp, so the picker must honour that choice.
        selected = [str(p) for p in getattr(self._api, "selectedPaths", ())
                    if str(p) in paths]
        current = str(getattr(session, "scalpPath", "") or "")
        if selected:
            default = selected[0]
        else:
            default = current if current in paths else paths[0]
        if default:
            combo.setCurrentIndex(paths.index(default))
        column.addWidget(combo)
        buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel,
            parent=dialog)
        buttons.setObjectName("tonicGeometryPickerButtons")
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        column.addWidget(buttons)
        if dialog.exec_() != QtWidgets.QDialog.Accepted:
            return ""
        return str(combo.currentData() or combo.currentText())

    def _onBindGeometry(self):
        paths = self._stageMeshPaths()
        if not paths:
            message = "Tonic: the current stage has no Mesh to bind as the scalp."
            self._container._status(self._api, message)
            QtWidgets.QMessageBox.critical(self, "Bind scalp mesh", message)
            return
        session = getattr(self._container, "session", None)
        if len(paths) == 1:
            # A picker with one row is a click with no choice in it; the
            # button's tooltip names the mesh this binds.
            path = paths[0]
        else:
            path = self._pickScalpPath(paths, session)
        if not path:
            return
        # The container names the reason (a malformed Mesh, or a GeomSubset
        # that is not a face subset of one); a container that only answers
        # yes/no keeps the Mesh wording.
        explain = getattr(self._container, "geometryError", None)
        validator = getattr(self._container, "isValidGeometry", None)
        reason = explain(self._api, path) if callable(explain) else (
            "" if not callable(validator) or validator(self._api, path)
            else "%s is not a valid scalp mesh. It needs points, faces of "
                 "three or more vertices and face indices inside the point "
                 "list." % path)
        if reason:
            message = "Tonic: %s" % reason
            self._container._status(self._api, message)
            QtWidgets.QMessageBox.critical(self, "Bind scalp mesh", message)
            self.refresh()
            return
        replace = False
        if session is not None and getattr(session, "model", None) is not None:
            current = str(getattr(session, "scalpPath", "") or "")
            if current == path:
                self._container._status(
                    self._api, "Tonic: %s is already bound" % path)
                self.refresh()
                return
            answer = QtWidgets.QMessageBox.warning(
                self, "Replace bound scalp",
                "Binding %s will replace the edited groom currently bound to "
                "%s and clear its regions and maps. Continue?" %
                (path, current or "the current mesh"),
                QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.Cancel,
                QtWidgets.QMessageBox.Cancel)
            if answer != QtWidgets.QMessageBox.Yes:
                return
            replace = True
        # Activation builds the model and its first publish on this thread:
        # a visible second or more on a dense scalp, which without a busy
        # cursor reads as a click that did nothing.
        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        try:
            bound = self._container.bindGeometry(
                self._api, path, replace=replace)
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()
        if bound is None:
            # Cancel on the Resume / Start new question (SS-03): the
            # artist backed out, nothing failed, so no error box.
            self.refresh()
            return
        if not bound:
            session = getattr(self._container, "session", None)
            detail = getattr(session, "lastError", lambda: "")()
            message = ("Tonic: could not bind %s." % path)
            if detail:
                message += " " + str(detail)
            QtWidgets.QMessageBox.critical(self, "Bind scalp mesh", message)
        self.refresh()

    def _onResumeGroom(self):
        resume = getattr(self._container, "resumeGroom", None)
        if not callable(resume):
            return
        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        try:
            resume(self._api)
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()
        self.refresh()

    def _refreshGeometry(self):
        session = getattr(self._container, "session", None)
        bound = self._scalpBound()
        path = (str(getattr(session, "scalpPath", "") or "")
                if bound else "")
        # The stage is part of the key only for the unbound tooltip, which
        # names the one mesh a click would bind; a stage reload changes it.
        key = (path, bound, None if bound else id(getattr(self._api, "stage",
                                                           None)))
        if key == self._geometryKey:
            return
        self._geometryKey = key
        if path:
            self._geometryPathLabel.setText("Scalp: %s" % path)
            self._geometryPathLabel.setToolTip(path)
        else:
            self._geometryPathLabel.setText("No scalp bound")
            self._geometryPathLabel.setToolTip("")
        tip = ("Choose the scalp Mesh (or a face GeomSubset of one) from the "
               "current stage. Rebinding an edited groom requires explicit "
               "confirmation.")
        if not bound:
            paths = self._stageMeshPaths()
            if len(paths) == 1:
                tip = ("Bind %s as the scalp: it is the only Mesh in the "
                       "stage." % paths[0])
        self._bindGeometryButton.setToolTip(tip)
        self._syncBound(bound)

    def _syncBound(self, bound):
        """Grey out every tool until a scalp is bound.

        Unbound, the dock used to offer six live modes, their sub-modes,
        parameters and actions that all did nothing; now the only live
        control is the one that starts the groom, and the hint says so.
        """
        if bound == self._boundShown:
            return
        self._boundShown = bound
        for btn in self._modeButtons.values():
            btn.setEnabled(bound)
        for widget in (self._subModeStack, self._tubeSelectionRow,
                       self._transformRow,
                       self._paramsStack, self._actionsStack,
                       self._warningsList, self._saveButton,
                       self._exportButton, self._importButton):
            widget.setEnabled(bound)
        self._firstRunHint.setVisible(not bound)
        # The viewport HUD reads 'Bind a scalp mesh to start' while
        # unbound; re-sync it now rather than on the next mouse event.
        viewport = getattr(self._container, "viewport", None)
        refreshGizmo = getattr(viewport, "refreshGizmo", None)
        if callable(refreshGizmo):
            try:
                refreshGizmo()
            except RuntimeError:
                pass        # its StageView's C++ side is already gone

    def _refreshResume(self):
        """Show Resume groom only while the container says it can work."""
        canResume = getattr(self._container, "canResumeGroom",
                            lambda api: False)
        try:
            show = bool(canResume(self._api))
        except (AttributeError, TypeError, ValueError, RuntimeError):
            show = False
        if show == self._resumeShown:
            return
        self._resumeShown = show
        self._resumeGroomButton.setVisible(show)
        self._resumeGroomButton.setEnabled(show)

    # ---- actions ----------------------------------------------------------

    def _buildActionPage(self, modeId):
        """The one-shot action buttons for `modeId`, built once.

        Returns the page and {action id: QPushButton} for button()."""
        page = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(page)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(3)
        column.setAlignment(QtCore.Qt.AlignTop)
        buttons = {}
        for action in tonicPanels.actions(modeId):
            label = action.label
            if action.hotkeyLabel:
                label = "%s (%s)" % (label, action.hotkeyLabel)
            btn = QtWidgets.QPushButton(label)
            # Glyph and text both: an action is a verb, and a row of
            # verbs as bare glyphs is a guessing game (DK-04).
            _dress(btn, "action", action.id, SHELF_ICON_PX)
            tip = getattr(action, "tooltip", "")
            if tip:
                btn.setToolTip(tip)
            btn.setFixedHeight(SHELF_BUTTON_HEIGHT)
            btn.clicked.connect(
                lambda checked=False, a=action: self._onAction(a))
            column.addWidget(btn)
            buttons[action.id] = btn
        return page, buttons

    def _onAction(self, action):
        serial = self._messageSerial
        action.handler(self._container)
        # GZ-08: an action that moved or reselected the gizmo's target
        # re-places it now, not on the next mouse event.
        getattr(getattr(self._container, "viewport", None), "refreshGizmo",
                lambda: None)()
        # Panel actions publish their model edit themselves; successful
        # session.publish calls refresh the viewport centrally.
        self.refresh()
        if self._messageSerial == serial:
            # Most actions (Relax, Match surface, Snap root...) report
            # nothing, so a click that worked looked exactly like one that
            # did nothing. Say what ran, with the tool's line after it.
            summary = self._toolStatusText or ""
            self._onStatus("Tonic: %s -- %s" % (action.label, summary)
                           if summary else "Tonic: %s" % action.label)

    # ---- the file row -------------------------------------------------

    def _makeFileButton(self, fileId, text, tip, handler, withText):
        """One file-row QToolButton, objectName "tonicFile:<fileId>": the
        icon when the glyph ships (or its drawn fallback), else -- or also,
        for the file commands -- its text."""
        btn = QtWidgets.QToolButton(self._fileRow)
        btn.setText(text)
        btn.setToolTip(tip)
        btn.setFixedHeight(SHELF_BUTTON_HEIGHT)
        _dress(btn, "file", fileId, SHELF_ICON_PX,
               QtCore.Qt.ToolButtonTextBesideIcon if withText
               else QtCore.Qt.ToolButtonIconOnly)
        self._buttons[("file", fileId)] = btn
        btn.clicked.connect(lambda checked=False: handler())
        return btn

    def _onSettings(self):
        """Bring the active mode's Parameters form into view.

        Focus stays where it was: a focused spin box would take the
        viewport's hotkeys (tonicViewport._textFocus) until the artist
        clicked back into the view."""
        self._scroll.ensureWidgetVisible(self._paramsBox, 0, 0)

    def _onUndoRedo(self, action):
        # Through the controller, exactly as Ctrl+Z / Ctrl+Y: it refreshes
        # the overlays and wakes the idle commit after the model steps.
        viewport = getattr(self._container, "viewport", None)
        runAction = getattr(viewport, "runAction", None)
        if runAction is not None:
            runAction(action)
        else:
            session = getattr(self._container, "session", None)
            step = getattr(session, action, None)
            if step is not None:
                step()
        self.refresh()

    @staticmethod
    def _stackDepth(session, method, entry):
        """An undo/redo stack depth, or None when nothing can answer."""
        reader = getattr(session, method, None)
        try:
            if callable(reader):
                return int(reader())
            fn = getattr(getattr(session, "dll", None), entry, None)
            if fn is not None:
                return int(fn(session.model))
        except (AttributeError, TypeError, ValueError, RuntimeError,
                OSError):
            pass
        return None

    def _dragLive(self, session):
        """True while a viewport drag or a session gesture is open."""
        viewport = getattr(self._container, "viewport", None)
        return bool(getattr(session, "gestureActive", False) or
                    (viewport is not None and
                     getattr(viewport, "gestureActive", False)))

    def _refreshUndo(self, session, dragging=False):
        """Enable Undo/Redo from the model's stacks; name the undo step.

        Parity G14: both grey out while a drag is live, as the RigExec
        toolbar's do -- the controller refuses an undo mid-gesture anyway,
        and a button that looks live but does nothing reads as broken.
        The stacks are not read then: this runs on every drag sample."""
        if dragging:
            key = ("drag",)
            if key == self._undoKey:
                return
            self._undoKey = key
            self._undoButton.setEnabled(False)
            self._redoButton.setEnabled(False)
            self._undoButton.setToolTip("Undo %s: finish the drag first"
                                        % UNDO_KEYS)
            self._redoButton.setToolTip("Redo %s: finish the drag first"
                                        % REDO_KEYS)
            return
        label = ""
        canUndo = canRedo = False
        if session is not None and getattr(session, "model", None) is not None:
            try:
                label = str(session.undoLabel() or "")
            except (AttributeError, TypeError, RuntimeError, OSError):
                label = ""
            undoDepth = self._stackDepth(session, "undoDepth",
                                         "Tonic_GetUndoDepth")
            redoDepth = self._stackDepth(session, "redoDepth",
                                         "Tonic_GetRedoDepth")
            canUndo = undoDepth > 0 if undoDepth is not None else bool(label)
            # With no depth to read, keep Redo live: the session reports
            # "nothing to redo" itself, which beats a button that never
            # enables.
            canRedo = redoDepth > 0 if redoDepth is not None else True
        key = (canUndo, canRedo, label)
        if key == self._undoKey:
            return
        self._undoKey = key
        self._undoButton.setEnabled(canUndo)
        self._redoButton.setEnabled(canRedo)
        self._undoButton.setToolTip(
            "Undo %s %s" % (label, UNDO_KEYS) if label
            else "Undo %s" % UNDO_KEYS)
        self._redoButton.setToolTip("Redo %s" % REDO_KEYS)

    def _onSaveGroom(self):
        self._container.saveGroomInteractive(self._api, self)

    def _onExportCenterCurves(self):
        self._container.exportCenterCurvesInteractive(self._api, self)

    def _onImportCurves(self):
        self._container.importCurvesInteractive(self._api, self)

    def _onAmplifiedToggled(self, value):
        # plan/17 section 3.2: the model owns the swap, so the Tonic scene
        # index can hide the cook's amplified tiles and the guide preview
        # in the same publish. The Output panel row takes the same session
        # path (DK-06), so the two controls can no longer disagree.
        session = getattr(self._container, "session", None)
        setter = getattr(session, "setAmplifiedHair", None)
        if callable(setter):
            setter(bool(value))
        else:
            self._container.tonicState.showAmplifiedHair = bool(value)
        self.refresh()

    def _onGeneratedToggled(self, value):
        state = self._container.tonicState
        value = bool(value)
        previous = bool(getattr(state, "showGeneratedCurves", True))
        session = getattr(self._container, "session", None)
        setter = getattr(session, "setGeneratedCurvesVisible", None)
        if callable(setter):
            if not setter(value):
                value = previous
        else:
            state.showGeneratedCurves = value
        self.refresh()

    # ---- warnings / status --------------------------------------------

    def _refreshWarnings(self, session, status=None):
        state = self._container.tonicState
        key = tonicHud.warningsKey(state, session, status)
        if key is not None and key == self._warningsKey:
            return
        if self._warningsKey is not None:
            # The key moves on every model version, which is every
            # publish, so the key alone still means three bounded ABI
            # reads (4 096 smoothness scores among them) per artist op.
            # The list is advisory -- coarse centroid coverage, root
            # crossings,
            # kink spikes -- and nobody reads it inside a stroke, so it
            # is re-read at the dock's own 250 ms cadence at most. A
            # forced refresh (key None, mode change, end of gesture)
            # still goes straight through.
            now = time.monotonic()
            if (now - self._warningsAt) * 1000.0 < REFRESH_MS:
                return
            self._warningsAt = now
        else:
            self._warningsAt = time.monotonic()
        self._warningsKey = key
        rows = tonicHud.warnings(state, session, status)
        texts = tuple((row.severity, row.text) for row in rows)
        if texts == self._warningsTexts:
            # The model moved but said nothing new. Rebuilding the widget
            # here would drop and re-create a QListWidgetItem (and the
            # Python select-action closure it carries in UserRole) several
            # times per artist op for no visible change.
            return
        self._warningsTexts = texts
        self._warningsList.clear()
        for row in rows:
            item = QtWidgets.QListWidgetItem(self._severityIcon(row.severity),
                                             row.text)
            # The suffix names what the click does: select, Retry commit,
            # or outline the faces (tonicHud.clickHint).
            item.setToolTip(row.text + tonicHud.clickHint(row.selectAction))
            item.setData(QtCore.Qt.UserRole, row.selectAction)
            self._warningsList.addItem(item)
        self._warningsBox.setVisible(bool(rows))
        if self._highlightText is not None and \
                not any(row.text == self._highlightText for row in rows):
            # The faces the highlight outlines are no longer what the
            # coverage row reports (a region claimed them, or the row
            # went): an outline of stale faces would mislead.
            self.clearScalpHighlight()

    def _severityIcon(self, severity):
        """The warnings row glyph for `severity` (built once per dock)."""
        icon = self._severityIcons.get(severity)
        if icon is not None:
            return icon
        name, fallback = _SEVERITY_ICONS.get(severity,
                                             _SEVERITY_ICONS["info"])
        icon = _loadIcon(name)
        if icon is None:
            pixmaps = getattr(QtWidgets.QStyle, "StandardPixmap",
                              QtWidgets.QStyle)
            icon = self.style().standardIcon(getattr(pixmaps, fallback))
        self._severityIcons[severity] = icon
        return icon

    def _onWarningHovered(self, item):
        viewport = self._warningsList.viewport()
        if item is not None and item.data(QtCore.Qt.UserRole) is not None:
            viewport.setCursor(QtCore.Qt.PointingHandCursor)
        else:
            viewport.unsetCursor()

    def _onWarningClicked(self, item):
        action = item.data(QtCore.Qt.UserRole)
        if action is not None:
            # Whatever the click shows belongs to this row's text; when
            # the row changes, _refreshWarnings takes the highlight away.
            self._highlightText = item.text()
            action(self._container)
            # GZ-08: a warning row that selects its subject moves the gizmo.
            getattr(getattr(self._container, "viewport", None),
                    "refreshGizmo", lambda: None)()
            self.refresh()

    # ---- the coverage highlight (DK-08) ------------------------------

    def highlightedFaces(self):
        """The scalp face indices the viewport outline shows (test hook)."""
        overlay = self._faceOverlay
        if overlay is None or overlay.isHidden():
            return []
        return list(self._highlightFaces)

    def highlightScalpFaces(self, faces):
        """Outline scalp `faces` over the viewport; returns how many.

        The coverage row's click. Clicking the row again while it shows
        the same faces takes the outline away.
        """
        faces = sorted(set(int(face) for face in faces))
        if faces and faces == self.highlightedFaces():
            self.clearScalpHighlight()
            self._onStatus("Tonic: coverage highlight cleared")
            return 0
        text = self._highlightText
        self.clearScalpHighlight()
        self._highlightText = text
        if not faces:
            self._onStatus("Tonic: every scalp face has a region")
            return 0
        polygons = self._scalpFacePolygons(faces)
        view = self._stageView()
        if not polygons or view is None:
            self._onStatus("Tonic: cannot outline the faces without a "
                           "region: the scalp mesh or the viewport is not "
                           "readable", "warning")
            return 0
        overlay = self._faceOverlay
        if overlay is None or overlay.parent() is not view:
            overlay = _FaceHighlightOverlay(view)
            self._faceOverlay = overlay
        self._highlightFaces = faces
        overlay.setGeometry(view.rect())
        overlay.setPolygons(polygons)
        overlay.show()
        overlay.raise_()
        self._onStatus("Tonic: %d scalp face%s without a region outlined in "
                       "red; click the row again to clear."
                       % (len(faces), "" if len(faces) == 1 else "s"))
        return len(faces)

    def clearScalpHighlight(self):
        self._highlightFaces = []
        self._highlightText = None
        overlay = self._faceOverlay
        if overlay is not None:
            try:
                overlay.setPolygons([])
                overlay.hide()
            except RuntimeError:
                # The StageView (and the overlay with it) went with a
                # stage reload.
                self._faceOverlay = None

    def _stageView(self):
        viewport = getattr(self._container, "viewport", None)
        return getattr(viewport, "view", None) if viewport is not None \
            else None

    def _scalpMeshArrays(self):
        """(points, counts, indices) of the bound scalp, or None.

        The model holds its own copy but exposes no reader, so this reads
        the stage mesh the session bound (the model binds its raw points,
        which is also what the region tint draws). Cached per model and
        scalp path: the scalp does not change under a live model. A face
        GeomSubset scalp reads its parent Mesh (the session recorded it at
        bind): face ids are parent-mesh ids, so the parent's arrays are the
        ones they index (plan/02 section 2.20).
        """
        session = getattr(self._container, "session", None)
        path = str(getattr(session, "scalpMeshPath", "") or
                   getattr(session, "scalpPath", "") or "")
        stage = getattr(self._api, "stage", None)
        if not path or stage is None:
            return None
        key = (id(getattr(session, "model", None)), path)
        if key == self._scalpMeshKey:
            return self._scalpMeshCache
        from pxr import Sdf
        prim = stage.GetPrimAtPath(path)

        def read(name):
            if prim:
                attr = prim.GetAttribute(name)
                value = attr.Get() if attr else None
                if value is not None:
                    return value
            # An inactive prim has no properties, and the region-tint
            # probes (like an artist hiding the stage's own draw under
            # the tint) deactivate the scalp: read the authored default
            # from the layer stack instead.
            propPath = Sdf.Path(path).AppendProperty(name)
            for layer in stage.GetLayerStack(True):
                spec = layer.GetAttributeAtPath(propPath)
                if spec is not None and spec.HasDefaultValue():
                    return spec.default
            return None

        points = read("points")
        counts = read("faceVertexCounts")
        indices = read("faceVertexIndices")
        mesh = None
        if points and counts and indices:
            mesh = ([(float(p[0]), float(p[1]), float(p[2]))
                     for p in points],
                    [int(c) for c in counts], [int(i) for i in indices])
        self._scalpMeshKey = key
        self._scalpMeshCache = mesh
        return mesh

    def _scalpFacePolygons(self, faces):
        mesh = self._scalpMeshArrays()
        if mesh is None:
            return []
        points, counts, indices = mesh
        starts = []
        at = 0
        for count in counts:
            starts.append(at)
            at += count
        polygons = []
        for face in faces:
            if not 0 <= face < len(counts):
                continue
            corners = indices[starts[face]:starts[face] + counts[face]]
            if all(0 <= i < len(points) for i in corners):
                polygons.append([points[i] for i in corners])
        return polygons

    # ---- the breadcrumb (DK-08) ----------------------------------------

    def breadcrumbButtons(self):
        """The breadcrumb's clickable crumbs, Groom first (test hook)."""
        return self._breadcrumbBar.crumbButtons()

    def _onExitLevelClicked(self):
        """The breadcrumb's up arrow: exactly what Ctrl+Up does."""
        viewport = getattr(self._container, "viewport", None)
        loop = (getattr(viewport, "loop", None)
                if viewport is not None else None)
        handler = getattr(loop, "exitLevel", None)
        if handler is None:
            handler = getattr(viewport, "exitLevel", None)
        if handler is not None:
            handler()
        else:
            self._onStatus(tonicHierarchy.exitLevel(
                self._container.tonicState))
        self.refresh()

    def _onBreadcrumbClicked(self, href):
        """Follow a per-tube breadcrumb, or retain a legacy level link."""
        text = str(href)
        try:
            if text.startswith("tube:"):
                tubeId = int(text.split(":", 1)[1])
                viewport = getattr(self._container, "viewport", None)
                loop = (getattr(viewport, "loop", None)
                        if viewport is not None else None)
                focusTube = getattr(loop, "focusTube", None)
                if focusTube is not None:
                    focusTube(tubeId)
                    self.refresh()
                return
            level = int(text)
        except (TypeError, ValueError):
            return
        viewport = getattr(self._container, "viewport", None)
        loop = (getattr(viewport, "loop", None)
                if viewport is not None else None)
        focus = getattr(loop, "focusLevel", None)
        if focus is not None:
            focus(level)
        else:
            tonicHierarchy.focusLevel(self._container.tonicState, level)
        self.refresh()

    def _breadcrumbModel(self, state, session):
        """(crumbs, frontier, canExit) for _BreadcrumbBar.setSegments."""
        childCount = None
        parentId = int(getattr(state, "focusParentId", -1))
        if parentId >= 0 and session is not None and \
                getattr(session, "model", None) is not None:
            try:
                childCount = len(tonicHierarchy.tubeChildren(
                    session.dll, session.model, parentId))
            except (RuntimeError, NotImplementedError, AttributeError,
                    TypeError):
                childCount = None
        segments = tonicHierarchy.breadcrumbSegments(state, childCount) or \
            [(1, "L1")]
        # The last segment is where the artist is (the frontier, or the
        # focused level/tube): text, since a click there goes nowhere.
        links, last = segments[:-1], segments[-1]
        rootTarget = segments[0][0] if segments[0][0] is not None else 1
        crumbs = [(rootTarget, GROOM_CRUMB_TEXT,
                   "The whole groom (L1) - click to return to it",
                   GROOM_ICON)]
        for target, label in links:
            level, name = _crumbParts(label)
            crumbs.append((target, name or "Level %d" % level,
                           "Level %d - click to focus" % level, ""))
        level, _name = _crumbParts(last[1])
        frontier = (last[1], "Level %d: where you are editing" % level
                    if level else "Where you are editing")
        canExit = int(getattr(state, "activeLevel", 1)) > 1 or \
            len(segments) > 1
        return crumbs, frontier, canExit

    def _refreshStatusStrip(self, session, status=None):
        state = self._container.tonicState
        strip = tonicHud.statusStrip(state, session, status)
        crumbs = self._breadcrumbModel(state, session)
        text = tonicHud.diagnosticsText(strip, status)
        # setText re-lays the label out and schedules a repaint;
        # setStyleSheet re-polishes the widget. Neither is worth doing for
        # a string that has not moved, and at four refreshes per artist op
        # most have not.
        if crumbs != self._crumbText:
            self._crumbText = crumbs
            self._breadcrumbBar.setSegments(crumbs[0], crumbs[1])
            self._breadcrumbBar.exitButton.setEnabled(crumbs[2])
        if text != self._statusText:
            self._statusText = text
            self._statusDetail.setText(text)
        # `_statusAmber` stays the one boolean a script reads for "work in
        # flight"; the pill is what the artist reads.
        self._statusAmber = strip.amber
        pill = tonicHud.syncSummary(strip, status)
        if pill != self._pillKey:
            self._pillKey = pill
            pillText, tone = pill
            background, foreground = _PILL_COLORS.get(tone,
                                                      _PILL_COLORS["info"])
            metrics = self._syncPill.fontMetrics()
            # Sized here, not at construction: the label only has the
            # dock's font once it is parented, and the routine words must
            # never elide. Fixed once, so no later text moves the layout.
            width = max(metrics.horizontalAdvance(t) for t in
                        (PILL_WIDTH_TEXT, tonicHud.NO_MODEL_TEXT)) + 16
            if width != self._syncPill.width():
                self._syncPill.setFixedWidth(width)
            self._syncPill.setText(metrics.elidedText(
                pillText, QtCore.Qt.ElideRight,
                self._syncPill.width() - 10))
            self._syncPill.setStyleSheet(
                "QLabel { background-color: %s; color: %s;"
                " border-radius: 8px; padding: 0px 4px; }"
                % (background, foreground))
        # The full pill text (a failure reason may elide) over the dump.
        tip = "%s\n%s" % (pill[0], text)
        if tip != self._syncPill.toolTip():
            self._syncPill.setToolTip(tip)
        chip = (tonicHud.deviceChip(strip), strip.fallbackReason)
        if chip != self._chipKey:
            self._chipKey = chip
            self._gpuChip.setText(chip[0])
            self._gpuChip.setToolTip(
                "Editing on the GPU." if not chip[1] else
                "CPU fallback: %s" % chip[1])
            self._gpuChip.setStyleSheet(
                "QLabel { border: 1px solid %s; color: %s;"
                " border-radius: 3px; font-size: 10px; }"
                % (("#6b8a9e", "#b8cbd8") if not chip[1] else
                   ("#c79a2e", "#e6c26a")))
        self._syncDisplay(state, session)

    def _syncDisplay(self, state, session):
        """Show the Display group's controls as the state has them.

        Every one of these fields can move without the dock: a hotkey
        toggles the curves, the session turns amplified hair on for Output,
        a script sets the navigation style."""
        amplified = bool(state.showAmplifiedHair)
        generatedGetter = getattr(session, "generatedCurvesVisible", None)
        generated = (bool(generatedGetter()) if callable(generatedGetter)
                     else bool(getattr(state, "showGeneratedCurves", True)))
        diagnostics = bool(getattr(state, "showDiagnostics", False))
        ladder = bool(getattr(state, "ladderEnabled", True))
        navigation = str(getattr(state, "navigationStyle", "maya")).lower()
        for check, value in ((self._amplifiedCheck, amplified),
                             (self._generatedCheck, generated),
                             (self._diagnosticsCheck, diagnostics),
                             (self._ladderCheck, ladder)):
            if value != check.isChecked():
                check.blockSignals(True)
                check.setChecked(value)
                check.blockSignals(False)
        if diagnostics != self._statusDetail.isVisibleTo(self):
            self._statusDetail.setVisible(diagnostics)
        index = self._navigationCombo.findData(navigation)
        if index >= 0 and index != self._navigationCombo.currentIndex():
            self._navigationCombo.blockSignals(True)
            self._navigationCombo.setCurrentIndex(index)
            self._navigationCombo.blockSignals(False)

    def _onDiagnosticsToggled(self, value):
        self._container.tonicState.showDiagnostics = bool(value)
        self._statusDetail.setVisible(bool(value))

    def _onLadderToggled(self, value):
        # tonicLadder reads the field at the next press (FB-02). The dock
        # cannot be clicked mid-drag, so there is never a stepped ladder
        # to give back here: the ladder restores itself on every release.
        self._container.tonicState.ladderEnabled = bool(value)

    def _onNavigationChanged(self, index):
        styleId = self._navigationCombo.itemData(index)
        if styleId:
            self._container.tonicState.navigationStyle = str(styleId)

    # ---- messages -----------------------------------------------------

    def _onStatus(self, text, level="info"):
        """The session's status sink (DK-05): one line in the message area.

        Every refusal and result the session and the loops report used to
        reach only usdview's bottom status bar, which nobody watches while
        working in the dock. The line still goes there too; here it is
        coloured by level -- 'error', or text that reads as a refusal --
        and fades after MESSAGE_FADE_MS.
        """
        text = str(text or "")
        if not text:
            return
        try:
            self._api.PrintStatus(text)
        except AttributeError:
            pass
        level = str(level or "info")
        if level != "error" and _ERROR_TEXT.search(text):
            level = "error"
        self._messageText = text
        self._messageLevel = level
        self._messageSerial += 1
        color = _MESSAGE_COLORS.get(level)
        try:
            self._messageLabel.setText(text)
            self._messageLabel.setStyleSheet(
                "QLabel { color: %s; }" % color if color else "")
            self._messageTimer.start()
        except RuntimeError:
            # The C++ dock went with a stage reload while the session kept
            # this sink; the status bar above already has the line.
            pass

    def _clearMessage(self):
        self._messageText = ""
        self._messageLevel = None
        try:
            self._messageLabel.setText("")
            self._messageLabel.setStyleSheet("")
        except RuntimeError:
            pass

    # ---- refresh / timer --------------------------------------------------

    def _shelfSignature(self, state):
        attr = _SUBMODE_STATE_ATTR.get(self._activeMode)
        return (self._activeMode,
                getattr(state, attr, "") if attr else "")

    def refresh(self):
        """Re-read state/session and update every widget. Cheap enough to
        call after every publish (plan/18 section 3.5); also called on a
        250 ms timer while the dock is visible.

        "Cheap enough" is load-bearing and used not to be true: the TN-5
        controller soak measured four of these per artist op at 11 ms an
        op, which was most of the deferred work the V7 note could not
        name. Every part now re-reads only when its own inputs moved, and
        the one TonicSession.status() this takes is passed down instead of
        each reader building its own.
        """
        session = getattr(self._container, "session", None)
        state = self._container.tonicState
        # A number key switches the mode through the viewport controller,
        # so the dock has to notice a mode it did not set itself -- or it
        # goes on showing the previous mode's sub-mode shelf, parameter
        # rows and action buttons while the shelf button says otherwise.
        if state.activeMode and state.activeMode != self._activeMode:
            self._adoptMode(state.activeMode)
        # A level change no longer re-adopts the page: the hierarchy rows
        # say 'This level ...' and name the level in a tooltip that
        # _refreshParams re-reads, so Enter/Exit level keeps the same form
        # (and whatever widget has the focus) instead of rebuilding it
        # (DK-06).
        if self._activeMode:
            # Some parameter rows are conditional on committed model state.
            # Output's density and width controls appear after its first
            # commit, while the mode itself remains active; notice that
            # signature change without rebuilding on every timer tick.
            page = self._pages.get(self._activeMode)
            signature = _paramSignature(
                tonicPanels.descriptors(self._activeMode, state))
            if page is None or page["paramSignature"] != signature:
                self._adoptMode(self._activeMode)
        shelfKey = self._shelfSignature(state)
        if shelfKey != self._shelfKey:
            self._shelfKey = shelfKey
            self._syncModeButtons()
            self._syncSubModeButtons()
        self._syncTubeSelectionButtons()
        self._syncInstruction()
        self._refreshGeometry()
        # Parity G14: Undo/Redo grey out for the whole drag -- a session
        # gesture, or a controller-side one (a pending press, a marquee)
        # that has not opened a session bracket yet.
        dragging = self._dragLive(session)
        if dragging:
            self._refreshUndo(session, dragging=True)
        if getattr(session, "gestureActive", False):
            # Mid-stroke. The publish hook fires once per mouse sample, so
            # a six-sample drag would re-read the whole panel six times
            # while the artist is watching the viewport, not the dock.
            # Note it as stale instead: the release publishes again and
            # both the idle pump and the 250 ms timer refresh after it.
            self._staleContent = True
            return
        self._refreshResume()
        if not dragging:
            self._refreshUndo(session)
        self._syncToolButtons(session)
        self._syncResubdivideButton()
        overlay = self._faceOverlay
        view = self._stageView()
        if overlay is not None and view is not None and \
                self._highlightFaces:
            try:
                if overlay.geometry() != view.rect():
                    overlay.setGeometry(view.rect())
            except RuntimeError:
                self._faceOverlay = None
        if self._staleContent:
            # Whatever the gesture changed has to be re-read even if the
            # keys below happen to match what the gesture started from.
            self._staleContent = False
            self._warningsKey = None
            self._statusText = None
            self._crumbText = None
            self._pillKey = None
        status = None
        readStatus = getattr(session, "status", None)
        if callable(readStatus):
            status = dict(readStatus() or {})
        if getattr(session, "model", None) is None:
            # No session yet, or one with no model: the pill reads 'No
            # scalp bound' (tonicHud.syncSummary), never 'Synced'.
            status = dict(status or {})
            status["active"] = False
        self._refreshParams(session)
        self._refreshWarnings(session, status)
        self._refreshStatusStrip(session, status)
        self._refreshToolStatus(session)

    def _syncResubdivideButton(self):
        """Show Re-subdivide's armed confirm on the button itself (DK-08).

        The first press only arms it (the children's sculpt is about to
        go); the loop's resubdivideArmed lapses with Escape or a new
        selection, and the button reverts with it.
        """
        page = self._pages.get("hierarchy")
        button = page["actionButtons"].get("resubdivide") if page else None
        if button is None:
            return
        armed = False
        viewport = getattr(self._container, "viewport", None)
        loop = getattr(viewport, "loop", None) if viewport is not None \
            else None
        if loop is not None and getattr(loop, "modeId", "") == "hierarchy":
            armed = bool(getattr(loop, "resubdivideArmed", False))
        if armed == self._resubdivideShown:
            return
        self._resubdivideShown = armed
        if armed:
            button.setText(RESUBDIVIDE_CONFIRM_TEXT)
            button.setStyleSheet(_ARMED_STYLE)
            button.setToolTip("Click again to merge the children and split "
                              "the parent again; their sculpt is discarded. "
                              "Escape or a new selection cancels.")
            return
        for action in tonicPanels.actions("hierarchy"):
            if action.id == "resubdivide":
                label = action.label
                if action.hotkeyLabel:
                    label = "%s (%s)" % (label, action.hotkeyLabel)
                button.setText(label)
                button.setToolTip(getattr(action, "tooltip", "") or "")
                break
        button.setStyleSheet("")

    def _refreshToolStatus(self, session):
        """The active loop's statusLine() under the shelf (DK-02)."""
        viewport = getattr(self._container, "viewport", None)
        if viewport is not None and getattr(viewport, "gestureActive", False):
            # A controller-side gesture (a marquee, a pending drag) has no
            # session gesture yet; the line catches up on the release.
            return
        loop = getattr(viewport, "loop", None)
        text = ""
        bound = session is not None and getattr(session, "model", None) \
            is not None
        if (bound and loop is not None and
                getattr(loop, "modeId", "") == self._activeMode):
            try:
                text = str(loop.statusLine() or "")
            except Exception as exc:  # noqa: BLE001 - never break refresh
                # A loop's line reads the ABI; one failing read must not
                # take the 250 ms refresh (and so the whole dock) down.
                text = "%s: status unavailable (%s)" % (
                    self._activeMode.title(), exc)
        elif self._activeMode:
            # Output is a panel with no loop, and an unbound dock has no
            # model to count: show what the mode is for instead.
            mode = tonicModes.ModeById(self._activeMode)
            text = mode.status if mode is not None else ""
        if text != self._toolStatusText:
            self._toolStatusText = text
            self._toolStatusLabel.setText(text)
        if text != self._summaryText:
            # The strip's summary is the same line, one row high (DK-05):
            # the strip is where the eye lands after an action, the label
            # under the shelf is where it is while choosing the tool.
            self._summaryText = text
            self._statusSummary.setText(text)
            self._statusSummary.setToolTip(text)

    def _onVisibilityChanged(self, visible):
        # workspaceOpen gates the V2 application-level hotkey filter (plan/18
        # section 3.4): the shelf's number/letter keys only fire while the
        # dock is actually visible, not merely constructed.
        self._container.tonicState.workspaceOpen = bool(visible)
        viewport = getattr(self._container, "viewport", None)
        setActive = getattr(viewport, "setWorkspaceActive", None)
        if visible:
            # resume() retakes the StageView a hidden dock gave back, and
            # the keyboard focus with it (FB-01).
            resume = getattr(viewport, "resume", None)
            if resume is not None:
                resume()
            elif setActive is not None:
                setActive(True)
            self._timer.start()
        else:
            self._timer.stop()
            # The coverage outline is the dock's, over a view the dock no
            # longer drives.
            self.clearScalpHighlight()
            # A click-created region is intentionally only a viewport
            # draft.  Closing the dock must not leave that temporary
            # contour armed when the workspace is shown again.
            cancel = getattr(viewport, "cancelGesture", None)
            if cancel is not None:
                cancel()
            # A closed dock used to leave the view filter eating every
            # click; suspend() hands the view back to usdview's picking
            # and keeps the session for the next show.
            suspend = getattr(viewport, "suspend", None)
            if suspend is not None:
                suspend()
            elif setActive is not None:
                setActive(False)

    def _onTimerTick(self):
        if self.isVisible():
            self.refresh()
