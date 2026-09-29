# Brush palette dock for usdview: the shelf, the generated sections, actions.
#
# The map-paint brush from plan/08-tools.md section 3.1 row 8: bind a surface
# mesh to a UsdGenPaintMap, stroke it in the viewport (face, u, v per sample
# from a CPU raycast, no Hydra pick per move), watch the map overlay repaint
# per move, and bake once into the edit target on release.
#
# Layout (a compact dark tool panel, a DCC/Substance side-panel density):
#
#   shelf       Paint | Add | Smooth | Erase     (exclusive icon buttons)
#   TARGET      mask presets (exclusive icons)   Setup | Paint to | Bind
#   BRUSH       Radius / Strength / Hardness     slider + spinbox rows
#               Falloff, Value (+ Flood), Channel
#   PREVIEW     eye toggle, ramp, live groom, viewport strokes
#   RESOLUTION  Auto + power-of-two texels + bound/suggested readout
#   history     Undo | Redo                     Keys (hotkey help)
#   status      state dot + message, detail line
#
# The rows are generated from brushPanels.descriptors(), never hand-written
# (plan/08 section 5.2); _rows[id] is (descriptor, editing widget) and every
# widget re-reads the state in refresh(). The dock follows no selection on its
# own: Bind reads usdview's prim selection on demand. The status detail line
# always ends with the edit target, because usdview points it at the session
# layer on open and an artist who does not know that loses work (plan/08
# section 4.6).

from pxr import Sdf
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from . import brushAuthor, brushPanels, brushState

TITLE = "Brush Palette"
OBJECT_NAME = "usdGenBrushPalette"

_ACTIVE_PALETTE = None

# -- look -------------------------------------------------------------------

COLORS = {
    "bg": "#232326",
    "panel": "#2a2a2e",
    "raised": "#34343a",
    "hover": "#3e3e45",
    "field": "#1b1b1e",
    "border": "#3b3b42",
    "text": "#d8d8dc",
    "dim": "#8b8b93",
    "accent": "#4d9be6",
    "accentSoft": "rgba(77, 155, 230, 56)",
    "icon": "#c9c9ce",
    "iconOn": "#9fcbff",
    "iconOff": "#56565c",
}

STYLE = """
QWidget#{root} {{ background: {bg}; color: {text}; }}
#{root} QWidget {{ font-size: 9pt; }}
#{root} QLabel {{ color: {text}; background: transparent; }}
#{root} QLabel[role="section"] {{ color: {dim}; font-size: 8pt;
    font-weight: 600; padding: 4px 0px 0px 2px; }}
#{root} QLabel[role="dim"] {{ color: {dim}; font-size: 8pt; }}
#{root} QLabel[role="row"] {{ color: {dim}; }}
#{root} QFrame[role="card"] {{ background: {panel};
    border: 1px solid {border}; border-radius: 6px; }}
#{root} QScrollArea, #{root} QScrollArea > QWidget > QWidget {{
    background: transparent; border: none; }}
#{root} QToolButton {{ background: {raised}; color: {text};
    border: 1px solid transparent; border-radius: 6px; padding: 3px 5px; }}
#{root} QToolButton:hover {{ background: {hover}; border-color: {border}; }}
#{root} QToolButton:pressed {{ background: {field}; }}
#{root} QToolButton:checked {{ background: {accentSoft};
    border: 1px solid {accent}; color: #ffffff; }}
#{root} QToolButton:focus {{ border: 1px solid {accent}; }}
#{root} QToolButton:disabled {{ color: {iconOff}; background: {panel}; }}
#{root} QToolButton[role="shelf"] {{ font-size: 8pt; padding: 4px 2px; }}
#{root} QAbstractSpinBox, #{root} QComboBox {{ background: {field};
    color: {text}; border: 1px solid {border}; border-radius: 6px;
    padding: 1px 6px; min-height: 20px;
    selection-background-color: {accent}; }}
#{root} QAbstractSpinBox:focus, #{root} QComboBox:focus {{
    border: 1px solid {accent}; }}
#{root} QAbstractSpinBox:disabled, #{root} QComboBox:disabled {{
    color: {iconOff}; }}
#{root} QComboBox::drop-down {{ border: none; width: 16px; }}
#{root} QComboBox QAbstractItemView {{ background: {panel}; color: {text};
    border: 1px solid {border}; selection-background-color: {accent}; }}
#{root} QSlider {{ min-height: 18px; background: transparent; }}
#{root} QSlider::groove:horizontal {{ height: 4px; background: {field};
    border-radius: 2px; }}
#{root} QSlider::sub-page:horizontal {{ background: {accent};
    border-radius: 2px; }}
#{root} QSlider::handle:horizontal {{ background: #e4e4e8; width: 12px;
    height: 12px; margin: -4px 0px; border-radius: 6px; }}
#{root} QSlider::handle:horizontal:hover {{ background: #ffffff; }}
#{root} QSlider:focus {{ border: none; }}
#{root} QCheckBox {{ spacing: 7px; padding: 1px 2px 1px 1px;
    background: transparent; }}
#{root} QCheckBox::indicator {{ width: 14px; height: 14px;
    margin: 0px 1px 0px 0px; border-radius: 4px;
    border: 1px solid {border}; background: {field}; }}
#{root} QCheckBox::indicator:checked {{ background: {accent};
    border-color: {accent}; }}
#{root} QCheckBox:focus {{ color: #ffffff; }}
#{root} QFrame#statusStrip {{ background: {field};
    border-top: 1px solid {border}; }}
""".format(root=OBJECT_NAME, **COLORS)

SHELF_ICON = 24
ROW_ICON = 18


def _tinted(pixmap, color):
    out = QtGui.QPixmap(pixmap.size())
    out.fill(QtCore.Qt.GlobalColor.transparent)
    painter = QtGui.QPainter(out)
    painter.drawPixmap(0, 0, pixmap)
    painter.setCompositionMode(
        QtGui.QPainter.CompositionMode.CompositionMode_SourceIn)
    painter.fillRect(out.rect(), QtGui.QColor(color))
    painter.end()
    return out


_PIXMAPS = {}


def _pixmap(name):
    """The 64 px source pixmap for icon `name`, or None when missing."""
    if name in _PIXMAPS:
        return _PIXMAPS[name]
    pix = None
    path = brushPanels.iconPath(name)
    if path is not None:
        loaded = QtGui.QPixmap(path)
        if not loaded.isNull():
            pix = loaded.scaled(
                64, 64, QtCore.Qt.AspectRatioMode.KeepAspectRatio,
                QtCore.Qt.TransformationMode.SmoothTransformation)
    _PIXMAPS[name] = pix
    return pix


def loadIcon(name, onName=None):
    """A tinted QIcon (normal / hover / checked / disabled), or None.

    The PNGs are white glyphs: Off tints neutral, On (checked) tints
    accent, Disabled dims. `onName` gives the checked state its own
    glyph (the eye toggle). None when the PNG is missing: the caller
    falls back to text."""
    off = _pixmap(name)
    if off is None:
        return None
    on = _pixmap(onName) if onName else off
    if on is None:
        on = off
    Mode, State = QtGui.QIcon.Mode, QtGui.QIcon.State
    icon = QtGui.QIcon()
    icon.addPixmap(_tinted(off, COLORS["icon"]), Mode.Normal, State.Off)
    icon.addPixmap(_tinted(off, "#ffffff"), Mode.Active, State.Off)
    icon.addPixmap(_tinted(on, COLORS["iconOn"]), Mode.Normal, State.On)
    icon.addPixmap(_tinted(on, COLORS["iconOn"]), Mode.Active, State.On)
    icon.addPixmap(_tinted(off, COLORS["iconOff"]), Mode.Disabled,
                   State.Off)
    icon.addPixmap(_tinted(on, COLORS["iconOff"]), Mode.Disabled, State.On)
    return icon


def _toolButton(parent, text, iconName=None, tooltip="", checkable=False,
                style="beside", iconOn=None, iconSize=ROW_ICON):
    """A QToolButton with a tinted icon, or text when the icon is missing."""
    button = QtWidgets.QToolButton(parent)
    button.setText(text)
    button.setToolTip(tooltip or text)
    button.setCheckable(checkable)
    button.setAutoRaise(False)
    button.setFocusPolicy(QtCore.Qt.FocusPolicy.TabFocus)
    icon = loadIcon(iconName, iconOn) if iconName else None
    Style = QtCore.Qt.ToolButtonStyle
    if icon is not None:
        button.setIcon(icon)
        button.setIconSize(QtCore.QSize(iconSize, iconSize))
        button.setToolButtonStyle({
            "under": Style.ToolButtonTextUnderIcon,
            "beside": Style.ToolButtonTextBesideIcon,
            "icon": Style.ToolButtonIconOnly,
        }[style])
    else:
        button.setToolButtonStyle(Style.ToolButtonTextOnly)
    return button


class _ChoiceButtons(QtWidgets.QWidget):
    """An exclusive row of tool buttons with a QComboBox-shaped API.

    count / itemData / itemText / currentIndex / currentData /
    setCurrentIndex / findData and currentIndexChanged, so generated
    rows and the T3 scripts drive it exactly like the combo it replaces."""

    currentIndexChanged = QtCore.Signal(int)

    def __init__(self, choices, icons, tooltip, parent=None):
        super(_ChoiceButtons, self).__init__(parent)
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(3)
        self.setToolTip(tooltip)
        self._items = list(choices)
        self._buttons = []
        self._current = -1
        for index, (label, _value) in enumerate(self._items):
            iconName = icons[index] if index < len(icons) else None
            button = _toolButton(
                self, label, iconName, "%s: %s" % (label, tooltip),
                checkable=True, style="icon", iconSize=ROW_ICON + 2)
            button.setSizePolicy(QtWidgets.QSizePolicy.Policy.Expanding,
                                 QtWidgets.QSizePolicy.Policy.Fixed)
            button.setMinimumHeight(28)
            button.clicked.connect(self._makeClick(index))
            layout.addWidget(button)
            self._buttons.append(button)

    def _makeClick(self, index):
        def click(_checked=False):
            self.setCurrentIndex(index)
            # A click on the current button keeps it checked.
            self._syncChecks()
        return click

    def _syncChecks(self):
        for i, button in enumerate(self._buttons):
            button.setChecked(i == self._current)

    def count(self):
        return len(self._items)

    def itemData(self, index):
        return self._items[index][1] if 0 <= index < len(self._items) \
            else None

    def itemText(self, index):
        return self._items[index][0] if 0 <= index < len(self._items) \
            else ""

    def findData(self, value):
        for i, (_label, data) in enumerate(self._items):
            if data == value:
                return i
        return -1

    def currentIndex(self):
        return self._current

    def currentData(self):
        return self.itemData(self._current)

    def buttons(self):
        return list(self._buttons)

    def setCurrentIndex(self, index):
        index = int(index)
        if not -1 <= index < len(self._items):
            return
        changed = index != self._current
        self._current = index
        self._syncChecks()
        if changed:
            self.currentIndexChanged.emit(index)


class _WrapLabel(QtWidgets.QLabel):
    """A word-wrapping label that always reserves its wrapped height.

    Layouts under a QDockWidget do not honour QLabel's height-for-width,
    so a wrapped line gets clipped; this keeps minimumHeight equal to the
    height the text needs at the label's current width."""

    def __init__(self, parent=None):
        super(_WrapLabel, self).__init__(parent)
        self.setWordWrap(True)
        self.setSizePolicy(QtWidgets.QSizePolicy.Policy.Preferred,
                           QtWidgets.QSizePolicy.Policy.Minimum)

    def _fit(self):
        margins = self.contentsMargins()
        width = self.width() - margins.left() - margins.right()
        if width <= 0:
            return
        # Measure the wrapped text itself: QLabel.heightForWidth reserves
        # an extra line against what it actually paints.
        rect = self.fontMetrics().boundingRect(
            QtCore.QRect(0, 0, width, 100000),
            int(QtCore.Qt.TextFlag.TextWordWrap), self.text())
        need = rect.height() + margins.top() + margins.bottom()
        if need > 0 and need != self.minimumHeight():
            self.setMinimumHeight(need)

    def setText(self, text):
        super(_WrapLabel, self).setText(text)
        self._fit()

    def resizeEvent(self, event):
        super(_WrapLabel, self).resizeEvent(event)
        self._fit()

    def sizeHint(self):
        # QLabel's wrapped hint guesses a tall narrow box; ask for exactly
        # the fitted height instead so the strip packs tight.
        hint = super(_WrapLabel, self).sizeHint()
        height = self.minimumHeight() or self.fontMetrics().lineSpacing()
        return QtCore.QSize(hint.width(), height)


class _Pow2SpinBox(QtWidgets.QSpinBox):
    """A spinbox over powers of two: steps double and halve."""

    def stepBy(self, steps):
        value = self.value()
        for _ in range(abs(int(steps))):
            value = value * 2 if steps > 0 else value // 2
        self.setValue(brushPanels.snapResolution(value))

    def stepEnabled(self):
        Step = QtWidgets.QAbstractSpinBox.StepEnabledFlag
        flags = Step.StepNone
        if self.value() < self.maximum():
            flags |= Step.StepUpEnabled
        if self.value() > self.minimum():
            flags |= Step.StepDownEnabled
        return flags


def GetBrushPalette(usdviewApi):
    """Return the process-wide palette dock, creating it on first use."""
    global _ACTIVE_PALETTE
    if _ACTIVE_PALETTE is not None:
        try:
            _ACTIVE_PALETTE.isVisible()
        except RuntimeError:  # the dock was deleted with its window
            _ACTIVE_PALETTE = None
    if _ACTIVE_PALETTE is None:
        _ACTIVE_PALETTE = BrushPaletteDock(usdviewApi)
    return _ACTIVE_PALETTE


class BrushPaletteDock(QtWidgets.QDockWidget):
    def __init__(self, usdviewApi):
        super(BrushPaletteDock, self).__init__(TITLE,
                                               usdviewApi.qMainWindow)
        self.setObjectName("usdGenBrushPaletteDock")
        self._api = usdviewApi
        self._viewport = None
        self._notice = ""
        self._noticeKind = "idle"
        self._noticeToken = 0
        self._suggestKey = None
        self._suggestion = None
        # State and loop live on the container (plan/08 section 1.3); the
        # dock only borrows them, so strokes survive closing the palette.
        from . import brushLoop
        container = getattr(usdviewApi, "_usdGenToolsContainer", None)
        if container is None:
            try:
                from . import container as getContainer
                container = getContainer()
            except ImportError:
                container = None
        if container is not None:
            self._state = container.brushState
            self._loop = container.brushLoop
            self._container = container
        else:
            self._state = brushState.BrushToolState()
            self._loop = brushLoop.BrushLoop(self._state)
            self._container = None

        self._rows = {}
        self._rowLabels = {}
        self._sliders = {}
        self._actionButtons = {}
        self._shelfButtons = {}
        self._buildUi()

        self._api.qMainWindow.addDockWidget(
            QtCore.Qt.DockWidgetArea.RightDockWidgetArea, self)

        dataModel = getattr(self._api, "dataModel", None)
        if dataModel is not None and hasattr(
                dataModel, "signalStageReplaced"):
            try:
                dataModel.signalStageReplaced.connect(
                    self._onStageReplaced)
            except (AttributeError, RuntimeError, TypeError):
                pass
        self.refresh()

    # -- construction -----------------------------------------------------

    def _buildUi(self):
        root = QtWidgets.QWidget(self)
        root.setObjectName(OBJECT_NAME)
        root.setAttribute(QtCore.Qt.WidgetAttribute.WA_StyledBackground)
        root.setStyleSheet(STYLE)
        outer = QtWidgets.QVBoxLayout(root)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        scroll = QtWidgets.QScrollArea(root)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(
            QtCore.Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        content = QtWidgets.QWidget(scroll)
        content.setMinimumWidth(240)
        body = QtWidgets.QVBoxLayout(content)
        body.setContentsMargins(8, 8, 8, 8)
        body.setSpacing(6)
        scroll.setWidget(content)
        outer.addWidget(scroll, 1)
        self._content = content

        descs = dict((d.id, d) for d in brushPanels.descriptors(self._state))
        acts = dict((a.id, a) for a in brushPanels.actions())

        # -- shelf
        shelf = self._card(content)
        shelfLayout = QtWidgets.QHBoxLayout(shelf)
        shelfLayout.setContentsMargins(4, 4, 4, 4)
        shelfLayout.setSpacing(4)
        self._shelfGroup = QtWidgets.QButtonGroup(self)
        self._shelfGroup.setExclusive(True)
        shelfTips = {
            "paint": "Paint: blend toward Value.",
            "add": "Add: offset by Value.",
            "smooth": "Smooth: average neighbouring texels.",
            "erase": "Erase: blend back to the map's default.",
        }
        for brushId, label in brushState.BRUSHES:
            button = _toolButton(
                shelf, label, "brush_" + brushId, shelfTips.get(brushId, label),
                checkable=True, style="under", iconSize=SHELF_ICON)
            button.setProperty("role", "shelf")
            button.setSizePolicy(QtWidgets.QSizePolicy.Policy.Expanding,
                                 QtWidgets.QSizePolicy.Policy.Fixed)
            button.setMinimumHeight(44)
            button.clicked.connect(self._makeShelfCallback(brushId))
            self._shelfGroup.addButton(button)
            shelfLayout.addWidget(button)
            self._shelfButtons[brushId] = button
        body.addWidget(shelf)

        # -- target
        body.addWidget(self._sectionLabel(content, "TARGET"))
        target = self._card(content)
        targetLayout = QtWidgets.QVBoxLayout(target)
        targetLayout.setContentsMargins(6, 6, 6, 6)
        targetLayout.setSpacing(6)
        preset = descs["maskPreset"]
        presets = _ChoiceButtons(preset.choices, preset.icons,
                                 preset.tooltip, target)
        presets.currentIndexChanged.connect(
            self._makeComboCallback(preset, presets))
        self._rows["maskPreset"] = (preset, presets)
        targetLayout.addWidget(presets)
        actionRow = QtWidgets.QHBoxLayout()
        actionRow.setSpacing(4)
        for actionId in ("setupDescription", "paintToDescription", "bind"):
            actionRow.addWidget(self._actionButton(target, acts[actionId]))
        targetLayout.addLayout(actionRow)
        body.addWidget(target)

        # -- brush
        body.addWidget(self._sectionLabel(content, "BRUSH"))
        brush = self._card(content)
        grid = QtWidgets.QGridLayout(brush)
        grid.setContentsMargins(6, 6, 6, 6)
        grid.setHorizontalSpacing(6)
        grid.setVerticalSpacing(5)
        grid.setColumnStretch(1, 1)
        row = 0
        for rowId in ("radiusWorld", "strength", "hardness"):
            desc = descs[rowId]
            grid.addWidget(self._rowLabel(brush, desc), row, 0)
            slider = self._makeSlider(brush, desc)
            grid.addWidget(slider, row, 1)
            spin = self._makeSpin(brush, desc)
            grid.addWidget(spin, row, 2)
            row += 1
        for rowId in ("falloff",):
            desc = descs[rowId]
            grid.addWidget(self._rowLabel(brush, desc), row, 0)
            grid.addWidget(self._makeCombo(brush, desc), row, 1, 1, 2)
            row += 1
        desc = descs["value"]
        grid.addWidget(self._rowLabel(brush, desc), row, 0)
        valueRow = QtWidgets.QHBoxLayout()
        valueRow.setSpacing(4)
        valueSpin = self._makeSpin(brush, desc)
        valueSpin.setMaximumWidth(16777215)
        valueRow.addWidget(valueSpin, 1)
        valueRow.addWidget(self._actionButton(brush, acts["flood"]))
        grid.addLayout(valueRow, row, 1, 1, 2)
        row += 1
        desc = descs["channel"]
        grid.addWidget(self._rowLabel(brush, desc), row, 0)
        grid.addWidget(self._makeCombo(brush, desc), row, 1, 1, 2)
        body.addWidget(brush)

        # -- preview
        body.addWidget(self._sectionLabel(content, "PREVIEW"))
        preview = self._card(content)
        previewRow = QtWidgets.QHBoxLayout(preview)
        previewRow.setContentsMargins(6, 6, 6, 6)
        previewRow.setSpacing(4)
        previewRow.addWidget(self._makeToggle(preview, descs["previewMap"]))
        ramp = self._makeCombo(preview, descs["colorMap"])
        previewRow.addWidget(ramp, 1)
        previewRow.addWidget(self._makeToggle(preview, descs["liveGroom"]))
        previewRow.addWidget(self._makeToggle(preview, descs["strokesArmed"]))
        body.addWidget(preview)

        # -- resolution
        body.addWidget(self._sectionLabel(content, "RESOLUTION"))
        resolution = self._card(content)
        resLayout = QtWidgets.QVBoxLayout(resolution)
        resLayout.setContentsMargins(6, 6, 6, 6)
        resLayout.setSpacing(4)
        resRow = QtWidgets.QHBoxLayout()
        resRow.setSpacing(6)
        resRow.addWidget(self._makeCheck(resolution, descs["resolutionAuto"]))
        resRow.addSpacing(10)
        resRow.addWidget(self._rowLabel(resolution, descs["resolution"]))
        resRow.addWidget(self._makePow2(resolution, descs["resolution"]), 1)
        resLayout.addLayout(resRow)
        self._resolutionReadout = _WrapLabel(resolution)
        self._resolutionReadout.setProperty("role", "dim")
        self._resolutionReadout.setWordWrap(True)
        self._resolutionReadout.setToolTip(
            "The bound map's resolution, and the texel density the mesh "
            "suggests for an auto bind.")
        resLayout.addWidget(self._resolutionReadout)
        body.addWidget(resolution)

        # -- history + keys
        history = QtWidgets.QHBoxLayout()
        history.setSpacing(4)
        history.addWidget(self._actionButton(content, acts["undo"]))
        history.addWidget(self._actionButton(content, acts["redo"]))
        history.addStretch(1)
        keys = _toolButton(content, "Keys", None, self._hotkeyTooltip())
        keys.clicked.connect(self._showHotkeys)
        self._keysButton = keys
        history.addWidget(keys)
        body.addLayout(history)
        body.addStretch(1)

        # -- status strip
        strip = QtWidgets.QFrame(root)
        strip.setObjectName("statusStrip")
        stripLayout = QtWidgets.QVBoxLayout(strip)
        stripLayout.setContentsMargins(8, 5, 8, 6)
        stripLayout.setSpacing(2)
        top = QtWidgets.QHBoxLayout()
        top.setSpacing(6)
        self._statusDot = QtWidgets.QLabel(strip)
        self._statusDot.setFixedSize(10, 10)
        top.addWidget(self._statusDot, 0, QtCore.Qt.AlignmentFlag.AlignVCenter)
        self._message = _WrapLabel(strip)
        self._message.setWordWrap(True)
        self._message.setTextInteractionFlags(
            QtCore.Qt.TextInteractionFlag.TextSelectableByMouse)
        top.addWidget(self._message, 1)
        stripLayout.addLayout(top)
        self._status = _WrapLabel(strip)
        self._status.setProperty("role", "dim")
        self._status.setWordWrap(True)
        self._status.setTextInteractionFlags(
            QtCore.Qt.TextInteractionFlag.TextSelectableByMouse)
        stripLayout.addWidget(self._status)
        self._target = _WrapLabel(strip)
        self._target.setProperty("role", "dim")
        self._target.setWordWrap(True)
        self._target.setTextInteractionFlags(
            QtCore.Qt.TextInteractionFlag.TextSelectableByMouse)
        self._target.setToolTip(
            "Where bakes are authored. usdview points the edit target at "
            "the session layer on open (plan/08 section 4.6).")
        stripLayout.addWidget(self._target)
        outer.addWidget(strip)

        self.setWidget(root)
        self._root = root

    def _card(self, parent):
        frame = QtWidgets.QFrame(parent)
        frame.setProperty("role", "card")
        return frame

    def _sectionLabel(self, parent, text):
        label = QtWidgets.QLabel(text, parent)
        label.setProperty("role", "section")
        return label

    def _rowLabel(self, parent, desc):
        label = QtWidgets.QLabel(desc.label, parent)
        label.setProperty("role", "row")
        label.setToolTip(desc.tooltip)
        self._rowLabels[desc.id] = label
        return label

    def _makeSlider(self, parent, desc):
        slider = QtWidgets.QSlider(QtCore.Qt.Orientation.Horizontal, parent)
        slider.setRange(0, 1000)
        slider.setSingleStep(10)
        slider.setPageStep(100)
        slider.setToolTip(desc.tooltip)
        slider.setMinimumWidth(60)
        slider.valueChanged.connect(self._makeSliderCallback(desc))
        self._sliders[desc.id] = slider
        return slider

    def _makeSpin(self, parent, desc):
        spin = QtWidgets.QDoubleSpinBox(parent)
        # A None max is unbounded (radius): the spinbox needs some
        # finite ceiling, far past any sane brush.
        spin.setRange(desc.min, desc.max if desc.max is not None else 1e9)
        spin.setSingleStep(desc.step or 0.01)
        spin.setDecimals(3)
        spin.setKeyboardTracking(False)
        spin.setButtonSymbols(
            QtWidgets.QAbstractSpinBox.ButtonSymbols.NoButtons)
        spin.setAlignment(QtCore.Qt.AlignmentFlag.AlignRight)
        spin.setFixedWidth(64)
        spin.setToolTip(desc.tooltip)
        spin.valueChanged.connect(
            self._makeSetCallback(desc, lambda v: float(v)))
        self._rows[desc.id] = (desc, spin)
        return spin

    def _makeCombo(self, parent, desc):
        combo = QtWidgets.QComboBox(parent)
        combo.setToolTip(desc.tooltip)
        combo.setSizeAdjustPolicy(
            QtWidgets.QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        combo.setMinimumContentsLength(5)
        self._fillCombo(combo, desc)
        combo.currentIndexChanged.connect(self._makeComboCallback(desc, combo))
        self._rows[desc.id] = (desc, combo)
        return combo

    def _fillCombo(self, combo, desc):
        combo.blockSignals(True)
        try:
            combo.clear()
            for label, value in desc.choices:
                combo.addItem(label, value)
        finally:
            combo.blockSignals(False)

    def _makeToggle(self, parent, desc):
        icons = list(desc.icons)
        iconOff = icons[-1] if len(icons) > 1 else (icons[0] if icons
                                                     else None)
        iconOn = icons[0] if icons else None
        # Short text for the no-icon fallback: the row is four buttons wide.
        short = {"previewMap": "Map", "liveGroom": "Groom",
                 "strokesArmed": "Strokes"}.get(desc.id, desc.label)
        button = _toolButton(parent, short, iconOff,
                             "%s: %s" % (desc.label, desc.tooltip),
                             checkable=True, style="icon", iconOn=iconOn,
                             iconSize=ROW_ICON + 2)
        button.setMinimumSize(30, 28)
        button.toggled.connect(self._makeSetCallback(desc, bool))
        self._rows[desc.id] = (desc, button)
        return button

    def _makeCheck(self, parent, desc):
        check = QtWidgets.QCheckBox(desc.label, parent)
        check.setToolTip(desc.tooltip)
        check.toggled.connect(self._makeSetCallback(desc, bool))
        self._rows[desc.id] = (desc, check)
        return check

    def _makePow2(self, parent, desc):
        spin = _Pow2SpinBox(parent)
        spin.setRange(int(desc.min), int(desc.max))
        spin.setKeyboardTracking(False)
        spin.setAlignment(QtCore.Qt.AlignmentFlag.AlignRight)
        spin.setSuffix(" px")
        spin.setButtonSymbols(
            QtWidgets.QAbstractSpinBox.ButtonSymbols.NoButtons)
        spin.setToolTip(desc.tooltip)
        spin.valueChanged.connect(self._makeSetCallback(desc, int))
        self._rows[desc.id] = (desc, spin)
        return spin

    def _actionButton(self, parent, action):
        button = _toolButton(parent, action.label, action.icon,
                             action.tooltip, style="beside")
        button.setSizePolicy(QtWidgets.QSizePolicy.Policy.Expanding,
                             QtWidgets.QSizePolicy.Policy.Fixed)
        button.setMinimumHeight(26)
        button.clicked.connect(self._makeActionCallback(action.handler))
        self._actionButtons[action.id] = button
        return button

    def _hotkeyTooltip(self):
        lines = ["%-18s %s" % (keys, what)
                 for keys, what in brushPanels.HOTKEYS]
        return ("<b>Viewport hotkeys</b><pre>%s</pre>%s"
                % ("\n".join(lines), brushPanels.HOTKEY_NOTE))

    def _showHotkeys(self, _checked=False):
        self._setNotice("; ".join("%s %s" % (k, w)
                                  for k, w in brushPanels.HOTKEYS), "idle")

    # -- callbacks ----------------------------------------------------------

    def _makeSetCallback(self, desc, convert):
        def callback(value):
            try:
                desc.set(self._state, convert(value))
            except (TypeError, ValueError):
                pass
            if desc.id == "previewMap":
                self._onPreviewMapChanged()
            self.refresh()
        return callback

    def _makeSliderCallback(self, desc):
        def callback(position):
            if desc.id == "radiusWorld":
                value = brushPanels.sliderToRadius(position)
            else:
                lo, hi = float(desc.min), float(desc.max)
                value = lo + (hi - lo) * position / 1000.0
            try:
                desc.set(self._state, value)
            except (TypeError, ValueError):
                pass
            self.refresh()
        return callback

    def _makeComboCallback(self, desc, combo):
        def callback(_index):
            before = desc.get(self._state)
            try:
                desc.set(self._state, combo.currentData())
            except (TypeError, ValueError):
                pass
            if desc.id == "maskPreset":
                self._onPresetChanged(before, combo)
            elif desc.id == "colorMap":
                self._onColorMapChanged()
            self.refresh()
        return callback

    def _makeShelfCallback(self, brushId):
        def callback(_checked=False):
            self._state.activeBrush = brushId
            self.refresh()
        return callback

    def _makeActionCallback(self, handler):
        def callback(_checked=False):
            try:
                handler(self)
            except Exception as exc:  # a slot must never raise into Qt
                self._setNotice("internal error: %s" % exc, "error")
            self.refresh()
        return callback

    def _stageNow(self):
        dataModel = getattr(self._api, "dataModel", None)
        return getattr(dataModel, "stage", None)

    def _update(self):
        try:
            self._api.UpdateViewport()
        except Exception:
            pass

    def _onPresetChanged(self, before, combo):
        """The mask-preset row: rebind, redraw, or revert on refusal."""
        ok, info = self._loop.rebindPreset(
            self._stageNow(), self._state.maskPreset)
        if not ok:
            # A live stroke owns the binding: put the state AND the row
            # back, so the palette never names a map it is not painting.
            self._state.maskPreset = before
            try:
                combo.blockSignals(True)
                combo.setCurrentIndex(combo.findData(before))
            finally:
                combo.blockSignals(False)
            self._setNotice(info, "error")
            return
        self._setNotice(info, "ok")
        self._update()

    def _onColorMapChanged(self):
        if self._state.binding is not None:
            self._loop.showBoundMap(self._stageNow())
            self._update()

    def _onPreviewMapChanged(self):
        ok, info = self._loop.setPreviewMap(self._stageNow(),
                                            self._state.previewMap)
        if not ok:
            self._setNotice("preview: %s" % info, "error")
        self._update()

    # -- actions ----------------------------------------------------------

    def _selectionPaths(self):
        dataModel = getattr(self._api, "dataModel", None)
        selection = getattr(dataModel, "selection", None)
        if selection is None:
            return []
        paths = []
        focus = None
        try:
            focus = selection.getFocusPrim()
        except (AttributeError, RuntimeError):
            focus = None
        if focus is not None:
            paths.append(focus)
        try:
            for prim in selection.getPrimPaths() or []:
                paths.append(prim)
        except (AttributeError, RuntimeError, TypeError):
            pass
        out = []
        for candidate in paths:
            try:
                if hasattr(candidate, "GetPath"):
                    out.append(candidate.GetPath())
                else:
                    out.append(Sdf.Path(str(candidate)))
            except (AttributeError, RuntimeError):
                continue
        return out

    def bindFromSelection(self):
        stage = self._stageNow()
        preset = brushAuthor.MaskPresetFor(
            getattr(self._state, "maskPreset", "density"))
        if preset is None:
            self._setNotice("unknown mask preset %r" % (
                getattr(self._state, "maskPreset", None),), "error")
            self.refresh()
            return False
        ok, info = self._loop.bindFromSelection(
            stage, self._selectionPaths(), mapName=preset.mapName,
            primvar=preset.primvar, channels=preset.channels,
            defaultValue=preset.defaultValue)
        if ok:
            self._ensureViewport()
            self._update()
        self._setNotice(info, "ok" if ok else "error")
        self.refresh()
        return ok

    def setupDescription(self):
        ok, info = self._loop.setupDescriptionFromSelection(
            self._stageNow(), self._selectionPaths())
        if ok:
            self._ensureViewport()
            self._update()
        self._setNotice(info, "ok" if ok else "error")
        self.refresh()
        return ok

    def paintToDescription(self):
        stage = self._stageNow()
        descs = brushAuthor.ListDescriptions(stage)
        if not descs:
            self._setNotice("no UsdGenDescription in the stage: "
                            "Setup description first", "error")
            self.refresh()
            return False
        if len(descs) == 1:
            choice, proceed = str(descs[0]), True
        else:
            items = [str(p) for p in descs]
            current = 0
            active = getattr(self._state, "activeDescription", "")
            if active in items:
                current = items.index(active)
            choice, proceed = QtWidgets.QInputDialog.getItem(
                self, "Paint to description", "Description:",
                items, current, False)
        if not proceed:
            return False
        ok, info = self._loop.paintToDescription(stage, choice)
        if ok:
            self._ensureViewport()
            self._update()
        self._setNotice(info, "ok" if ok else "error")
        self.refresh()
        return ok

    def _ensureViewport(self):
        """Install the viewport controller with its status wired here."""
        if self._container is not None:
            try:
                self._viewport = self._container.ensureBrushViewport(
                    self._api, statusSink=self.onLoopStatus)
                return
            except (AttributeError, RuntimeError, TypeError):
                pass
        if self._viewport is None:
            from . import brushViewport
            viewport = brushViewport.BrushViewportController(
                self._api, self._state, self._loop)
            if viewport.install():
                self._viewport = viewport
        if self._viewport is not None:
            self._viewport.setStatusSink(self.onLoopStatus)

    def flood(self):
        ok, info = self._loop.flood(self._stageNow())
        self._setNotice(info, "cooking" if ok else "error")
        self._update()
        self.refresh()
        return ok

    def undo(self):
        ok, info = self._loop.undo(self._stageNow())
        self._setNotice("undo: %s" % (self._state.undoStack.redoLabel()
                                      if ok else info),
                        "cooking" if ok else "error")
        self._update()
        self.refresh()
        return ok

    def redo(self):
        ok, info = self._loop.redo(self._stageNow())
        self._setNotice("redo: %s" % (self._state.undoStack.undoLabel()
                                      if ok else info),
                        "cooking" if ok else "error")
        self._update()
        self.refresh()
        return ok

    # -- status -----------------------------------------------------------

    def onLoopStatus(self, text, kind=None):
        """The viewport's status sink: message plus state-dot kind."""
        self._setNotice(text, kind or "idle")
        self.refresh()

    _onLoopStatus = onLoopStatus

    def _setNotice(self, text, kind="idle"):
        self._notice = text or ""
        self._noticeKind = kind
        self._noticeToken += 1
        if kind in ("cooking", "ok"):
            # No cook-finished signal reaches the tool: fall back to idle.
            token = self._noticeToken
            QtCore.QTimer.singleShot(900, lambda: self._settle(token))

    def _settle(self, token):
        try:
            if token == self._noticeToken and self._noticeKind in (
                    "cooking", "ok"):
                self._noticeKind = "idle"
                self._paintStatus()
        except RuntimeError:  # the dock is gone
            pass

    def _onStageReplaced(self, _sender=None, _stage=None):
        self._loop.setStage(self._stageNow())
        self._state.binding = None
        self._state.activeDescription = ""
        self._suggestKey = None
        self._suggestion = None
        self._setNotice("stage replaced: brush unbound", "idle")
        self.refresh()

    def _refreshSuggestion(self):
        """The auto-resolution suggestion for the bound surface (cached)."""
        binding = getattr(self._state, "binding", None)
        key = str(binding.surfacePath) if binding is not None else None
        if key == self._suggestKey:
            return self._suggestion
        self._suggestKey = key
        self._suggestion = None
        if key is None:
            return None
        stage = self._stageNow()
        try:
            from . import brushPick
            suggest = getattr(brushPick, "suggestResolution", None)
            if stage is None or suggest is None:
                return None
            snapshot, _error = brushPick.snapshotMesh(
                stage.GetPrimAtPath(binding.surfacePath))
            if snapshot is not None:
                resolution, info = suggest(snapshot)
                self._suggestion = info or ("%d px/face" % resolution)
        except Exception:
            self._suggestion = None
        return self._suggestion

    def refresh(self):
        """Re-read every widget from the state (signals blocked)."""
        state = self._state
        descs = dict((d.id, d) for d in brushPanels.descriptors(state))
        for rowId, (_old, widget) in list(self._rows.items()):
            desc = descs.get(rowId)
            if desc is None:
                continue
            self._rows[rowId] = (desc, widget)
            self._syncWidget(desc, widget)
        for rowId, slider in self._sliders.items():
            desc = descs.get(rowId)
            value = desc.get(state) if desc is not None else 0.0
            if rowId == "radiusWorld":
                position = brushPanels.radiusToSlider(value)
            else:
                lo, hi = float(desc.min), float(desc.max)
                span = (hi - lo) or 1.0
                position = int(round((float(value) - lo) / span * 1000.0))
            slider.blockSignals(True)
            slider.setValue(max(0, min(1000, position)))
            slider.blockSignals(False)
        for brushId, button in self._shelfButtons.items():
            button.setChecked(brushId == state.activeBrush)
        resolutionRow = self._rows.get("resolution")
        if resolutionRow is not None:
            resolutionRow[1].setEnabled(not state.resolutionAuto)
        stack = state.undoStack
        for actionId, can, label in (
                ("undo", getattr(stack, "canUndo", None),
                 getattr(stack, "undoLabel", None)),
                ("redo", getattr(stack, "canRedo", None),
                 getattr(stack, "redoLabel", None))):
            button = self._actionButtons.get(actionId)
            if button is None or can is None:
                continue
            enabled = bool(can())
            button.setEnabled(enabled)
            text = label() if (enabled and label is not None) else ""
            button.setToolTip("%s %s" % (
                actionId.capitalize(), text or "(nothing to %s)" % actionId))
        self._resolutionReadout.setText(brushPanels.resolutionText(
            state, self._refreshSuggestion()))
        self._paintStatus()

    def _syncWidget(self, desc, widget):
        value = desc.get(self._state)
        widget.blockSignals(True)
        try:
            if isinstance(widget, _ChoiceButtons):
                widget.setCurrentIndex(widget.findData(value))
            elif isinstance(widget, QtWidgets.QComboBox):
                items = [(widget.itemText(i), widget.itemData(i))
                         for i in range(widget.count())]
                if items != list(desc.choices):
                    self._fillCombo(widget, desc)
                    widget.blockSignals(True)
                index = widget.findData(value)
                widget.setCurrentIndex(index if index >= 0 else 0)
            elif isinstance(widget, (QtWidgets.QToolButton,
                                     QtWidgets.QCheckBox)):
                widget.setChecked(bool(value))
            elif isinstance(widget, QtWidgets.QSpinBox):
                widget.setValue(int(value))
            elif isinstance(widget, QtWidgets.QDoubleSpinBox):
                widget.setValue(float(value))
        except (TypeError, ValueError):
            pass
        finally:
            widget.blockSignals(False)

    def _paintStatus(self):
        target = "edit target: (no stage)"
        stage = self._stageNow()
        if stage is not None:
            try:
                target = "edit target: %s" % (
                    stage.GetEditTarget().GetLayer().GetDisplayName())
            except (AttributeError, RuntimeError):
                pass
        self._status.setText(brushPanels.statusText(self._state))
        self._target.setText(target)
        kind = brushPanels.statusKind(self._state, self._noticeKind)
        self._message.setText(self._notice or (
            "ready" if self._state.binding is not None
            else "select a mesh and Bind, or Setup a description"))
        self._statusDot.setStyleSheet(
            "background: %s; border-radius: 5px;"
            % brushPanels.STATUS_COLORS[kind])
        self._statusDot.setToolTip(kind)

    def statusKind(self):
        """The state dot's current kind (tests read this)."""
        return brushPanels.statusKind(self._state, self._noticeKind)

    def showAndRaise(self):
        self.show()
        self.raise_()
        if self._state.binding is not None:
            # Reopened over a live binding: reinstall and rewire status,
            # and redraw the map overlay the close dropped.
            self._ensureViewport()
            try:
                self._loop.showBoundMap(self._stageNow())
            except Exception:
                pass
        self.refresh()

    def closeEvent(self, event):
        """Closing the palette drops every map overlay (and a live stroke)."""
        try:
            stage = self._stageNow()
            if self._loop.gestureActive():
                self._loop.cancel(stage)
            from . import brushPreview
            brushPreview.ClearAllPreviews(stage)
            self._update()
        except Exception:
            pass
        super(BrushPaletteDock, self).closeEvent(event)

    # Compatibility: the old form rebuilt its rows; widgets now refresh.
    rebuildForm = refresh
