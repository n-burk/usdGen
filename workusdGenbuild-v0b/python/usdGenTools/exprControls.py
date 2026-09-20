# The controls panel: one widget per `$name = <value>; # <annotation>`
# statement in the expression, the way SeExpr2's editor builds them.
#
# The parsing lives in exprApi (no Qt, so the tests share it); everything here
# is presentation. A control never touches the text: it reports a new value and
# the dock rewrites exactly that value's span, which is what keeps an edit to
# one control from disturbing anything else in the expression.
#
# Callbacks rather than signals, to match the rest of this package: the dock
# owns every decision about what an edit means.

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from . import exprApi

# The kinds the Add Widget dialog offers, in the order SeExpr's own dialog
# lists them, with the label shown and the initial value each starts from.
WIDGET_KINDS = (
    ("Integer", "int", 0),
    ("Float", "float", 0.5),
    ("Vector", "vector", (0.0, 0.0, 0.0)),
    ("Color", "color", (1.0, 1.0, 1.0)),
    ("Curve", "curve", [(0.0, 0.0, exprApi.INTERP_MONOTONE),
                        (1.0, 1.0, exprApi.INTERP_MONOTONE)]),
    ("Color Curve", "ccurve", [(0.0, (0.0, 0.0, 0.0), exprApi.INTERP_MONOTONE),
                               (1.0, (1.0, 1.0, 1.0), exprApi.INTERP_MONOTONE)]),
    ("String", "string", ""),
)

NAME_WIDTH = 92
# Wide enough for a four-decimal value AND the spin buttons beside it: at 96
# the arrows sat on top of the last digit.
VALUE_WIDTH = 116


def _fixedFont():
    return QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)


def _swatch(color):
    """A small pixmap of `color`, for a button that opens a colour picker."""
    pixmap = QtGui.QPixmap(28, 14)
    pixmap.fill(color)
    painter = QtGui.QPainter(pixmap)
    painter.setPen(QtGui.QColor(30, 30, 30))
    painter.drawRect(0, 0, 27, 13)
    painter.end()
    return pixmap


def _toColor(values):
    return QtGui.QColor.fromRgbF(*[max(0.0, min(1.0, float(v)))
                                   for v in values[:3]])


def _fromColor(color):
    return (color.redF(), color.greenF(), color.blueF())


class _Control(QtWidgets.QWidget):
    """Shared shell: the `$name` on a fixed column, then the body."""

    def __init__(self, index, control, onChanged, onDragChanged, parent=None):
        super(_Control, self).__init__(parent)
        self.index = index
        self.control = control
        self._onChanged = onChanged
        self._onDragChanged = onDragChanged
        self._updating = False

        self._outer = QtWidgets.QHBoxLayout(self)
        self._outer.setContentsMargins(0, 0, 0, 0)
        self._outer.setSpacing(4)
        self.nameLabel = QtWidgets.QLabel(control.name)
        self.nameLabel.setFixedWidth(NAME_WIDTH)
        self.nameLabel.setFont(_fixedFont())
        self.nameLabel.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop)
        self.nameLabel.setToolTip("%s  (%s)" % (control.name, control.kind))
        self._outer.addWidget(self.nameLabel)

    def report(self, value):
        """Hand a new value to the dock, unless we are only displaying one."""
        if not self._updating:
            self._onChanged(self.index, value)

    def setControl(self, control):
        """Take a re-scanned record for the same control; the widget state is
        already right, only the offsets moved."""
        self.control = control


class _Scale(object):
    """The mapping between a value and an integral slider position.

    Kept as a plain object so the number and vector controls share it without
    inheriting from each other."""

    STEPS = 1000

    def __init__(self, low, high):
        self.set(low, high)

    def set(self, low, high):
        if high <= low:
            high = low + 1.0
        self.low, self.high = float(low), float(high)

    def toSlider(self, value):
        span = self.high - self.low
        t = 0.0 if span <= 0 else (value - self.low) / span
        return int(round(max(0.0, min(1.0, t)) * self.STEPS))

    def toValue(self, position):
        return self.low + (self.high - self.low) * (float(position) / self.STEPS)


class NumberControl(_Control):
    """`$x = 0.5; # 0, 1` -- a slider and a spinbox over the annotated range.

    The range boxes rewrite the ANNOTATION rather than the value, because in
    this grammar the comment is where a range is stored; widening a slider is
    therefore an edit to the expression like any other."""

    def __init__(self, index, control, onChanged, onDragChanged,
                 onRangeChanged, parent=None):
        super(NumberControl, self).__init__(index, control, onChanged,
                                            onDragChanged, parent)
        self._onRangeChanged = onRangeChanged
        self.isInt = control.kind == "int"
        low = control.data["minimum"]
        high = control.data["maximum"]
        self._scale = _Scale(low, high)

        self.slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
        self.slider.setMinimumWidth(60)
        self._outer.addWidget(self.slider, 1)

        if self.isInt:
            self.valueBox = QtWidgets.QSpinBox()
            self.valueBox.setRange(-2 ** 31, 2 ** 31 - 1)
        else:
            self.valueBox = QtWidgets.QDoubleSpinBox()
            self.valueBox.setDecimals(4)
            self.valueBox.setRange(-1e9, 1e9)
        self.valueBox.setFixedWidth(VALUE_WIDTH)
        self.valueBox.setKeyboardTracking(False)
        self._outer.addWidget(self.valueBox)

        self.minimumBox = self._rangeBox(low)
        self.maximumBox = self._rangeBox(high)
        self._outer.addWidget(self.minimumBox)
        self._outer.addWidget(self.maximumBox)

        self._applyRange(low, high)
        self.setValue(control.data["value"])

        self.slider.valueChanged.connect(self._onSlider)
        self.slider.sliderPressed.connect(lambda: self._onDragChanged(True))
        self.slider.sliderReleased.connect(lambda: self._onDragChanged(False))
        self.valueBox.valueChanged.connect(self._onSpin)
        self.minimumBox.valueChanged.connect(self._onRangeEdited)
        self.maximumBox.valueChanged.connect(self._onRangeEdited)

    def _rangeBox(self, value):
        box = QtWidgets.QDoubleSpinBox()
        box.setDecimals(0 if self.isInt else 2)
        box.setRange(-1e9, 1e9)
        box.setFixedWidth(64)
        box.setKeyboardTracking(False)
        box.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
        box.setValue(value)
        box.setToolTip("Slider range. Editing it rewrites the `# min, max` "
                       "comment on this line.")
        return box

    def _applyRange(self, low, high):
        self._scale.set(low, high)
        if self.isInt:
            self.slider.setRange(int(round(self._scale.low)),
                                 int(round(self._scale.high)))
        else:
            self.slider.setRange(0, _Scale.STEPS)
            self.valueBox.setSingleStep(
                max(1e-4, (self._scale.high - self._scale.low) / 100.0))

    def value(self):
        return self.valueBox.value()

    def range(self):
        return self.minimumBox.value(), self.maximumBox.value()

    def setValue(self, value):
        self._updating = True
        try:
            self.valueBox.setValue(value)
            self.slider.setValue(int(round(value)) if self.isInt
                                 else self._scale.toSlider(value))
        finally:
            self._updating = False

    def _onSlider(self, position):
        if self._updating:
            return
        value = float(position) if self.isInt else self._scale.toValue(position)
        self._updating = True
        try:
            self.valueBox.setValue(value)
        finally:
            self._updating = False
        self.report(value)

    def _onSpin(self, value):
        if self._updating:
            return
        self._updating = True
        try:
            self.slider.setValue(int(round(value)) if self.isInt
                                 else self._scale.toSlider(value))
        finally:
            self._updating = False
        self.report(value)

    def _onRangeEdited(self, _value):
        if self._updating:
            return
        low, high = self.range()
        self._applyRange(low, high)
        self.setValue(self.valueBox.value())
        self._onRangeChanged(self.index, low, high)


class VectorControl(_Control):
    """`$c = [1, 0.5, 0.2]; # color` -- a swatch and three component sliders,
    or the same three sliders alone when the annotation does not say colour."""

    LABELS = ("x", "y", "z", "w")
    COLOR_LABELS = ("r", "g", "b", "a")

    def __init__(self, index, control, onChanged, onDragChanged, parent=None):
        super(VectorControl, self).__init__(index, control, onChanged,
                                            onDragChanged, parent)
        self.isColor = control.kind == "color"
        values = list(control.data["values"])
        self._scale = _Scale(control.data["minimum"], control.data["maximum"])

        body = QtWidgets.QVBoxLayout()
        body.setContentsMargins(0, 0, 0, 0)
        body.setSpacing(1)
        self._outer.addLayout(body, 1)

        self.swatch = None
        if self.isColor:
            self.swatch = QtWidgets.QPushButton()
            self.swatch.setFixedSize(34, 18)
            self.swatch.setToolTip("Pick a colour")
            self.swatch.clicked.connect(self._pickColor)
            self._outer.insertWidget(1, self.swatch)
            self._outer.setAlignment(self.swatch, QtCore.Qt.AlignTop)

        labels = self.COLOR_LABELS if self.isColor else self.LABELS
        self.sliders = []
        self.boxes = []
        for component in range(len(values)):
            row = QtWidgets.QHBoxLayout()
            row.setContentsMargins(0, 0, 0, 0)
            row.setSpacing(4)
            tag = QtWidgets.QLabel(labels[component] if component < len(labels)
                                   else str(component))
            tag.setFixedWidth(10)
            tag.setFont(_fixedFont())
            row.addWidget(tag)
            slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
            slider.setRange(0, _Scale.STEPS)
            slider.setMinimumWidth(50)
            row.addWidget(slider, 1)
            box = QtWidgets.QDoubleSpinBox()
            box.setDecimals(4)
            box.setRange(-1e9, 1e9)
            box.setFixedWidth(VALUE_WIDTH)
            box.setKeyboardTracking(False)
            box.setSingleStep(max(1e-4, (self._scale.high - self._scale.low) / 100.0))
            row.addWidget(box)
            body.addLayout(row)
            self.sliders.append(slider)
            self.boxes.append(box)
            slider.valueChanged.connect(
                lambda position, c=component: self._onSlider(c, position))
            slider.sliderPressed.connect(lambda: self._onDragChanged(True))
            slider.sliderReleased.connect(lambda: self._onDragChanged(False))
            box.valueChanged.connect(
                lambda value, c=component: self._onSpin(c, value))

        self.setValue(values)

    def value(self):
        return tuple(box.value() for box in self.boxes)

    def setValue(self, values):
        self._updating = True
        try:
            for box, slider, value in zip(self.boxes, self.sliders, values):
                box.setValue(value)
                slider.setValue(self._scale.toSlider(value))
            self._refreshSwatch()
        finally:
            self._updating = False

    def _refreshSwatch(self):
        if self.swatch is not None:
            self.swatch.setIcon(QtGui.QIcon(_swatch(_toColor(self.value()))))
            self.swatch.setIconSize(QtCore.QSize(28, 14))

    def _onSlider(self, component, position):
        if self._updating:
            return
        value = self._scale.toValue(position)
        self._updating = True
        try:
            self.boxes[component].setValue(value)
            self._refreshSwatch()
        finally:
            self._updating = False
        self.report(self.value())

    def _onSpin(self, component, value):
        if self._updating:
            return
        self._updating = True
        try:
            self.sliders[component].setValue(self._scale.toSlider(value))
            self._refreshSwatch()
        finally:
            self._updating = False
        self.report(self.value())

    def _pickColor(self):
        chosen = QtWidgets.QColorDialog.getColor(
            _toColor(self.value()), self, "Pick a colour")
        if not chosen.isValid():
            return
        self.setValue(_fromColor(chosen) + self.value()[3:])
        self.report(self.value())


class CurveWidget(QtWidgets.QWidget):
    """The editable knot list of a `curve(...)` or `ccurve(...)` value.

    Left-click a knot to select and drag it, left-click empty space to add
    one, right-click or Delete to remove one. A colour curve's knots carry no
    height, so they ride a rail along the bottom and only their position
    moves; double-clicking one opens the colour picker."""

    MINIMUM_HEIGHT = 108
    MARGIN = 7
    RADIUS = 4
    PICK_DISTANCE = 8

    def __init__(self, isColor, onChanged, onDragChanged, onSelected,
                 parent=None):
        super(CurveWidget, self).__init__(parent)
        self.isColor = isColor
        self._onChanged = onChanged
        self._onDragChanged = onDragChanged
        self._onSelected = onSelected
        self._knots = []
        self._selected = -1
        self._dragging = False
        self.setMinimumHeight(self.MINIMUM_HEIGHT)
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding,
                           QtWidgets.QSizePolicy.Fixed)
        self.setFocusPolicy(QtCore.Qt.StrongFocus)
        self.setCursor(QtCore.Qt.CrossCursor)

    # ---- state ---------------------------------------------------------

    def knots(self):
        return list(self._knots)

    def setKnots(self, knots, selected=None):
        self._knots = sorted(knots, key=lambda k: k[0])
        if selected is not None:
            self._selected = selected
        self._selected = max(-1, min(self._selected, len(self._knots) - 1))
        self.update()

    def selectedIndex(self):
        return self._selected

    def selectKnot(self, index):
        self._selected = max(-1, min(index, len(self._knots) - 1))
        self.update()
        self._onSelected(self._selected)

    def setSelectedKnot(self, position=None, value=None, interp=None):
        """Change the selected knot from the row of boxes below the canvas."""
        if self._selected < 0:
            return
        p, v, i = self._knots[self._selected]
        knot = (p if position is None else position,
                v if value is None else value,
                i if interp is None else interp)
        knots = list(self._knots)
        knots[self._selected] = knot
        order = sorted(range(len(knots)), key=lambda n: knots[n][0])
        self._selected = order.index(self._selected)
        self.setKnots(knots)
        self._onChanged(self.knots())

    def removeSelected(self):
        # One knot is the least a curve can be built from; refusing the last
        # deletion keeps the value a curve rather than an empty call.
        if self._selected < 0 or len(self._knots) <= 1:
            return False
        del self._knots[self._selected]
        self._selected = min(self._selected, len(self._knots) - 1)
        self.update()
        self._onSelected(self._selected)
        self._onChanged(self.knots())
        return True

    # ---- geometry ------------------------------------------------------

    def _valueRange(self):
        """The vertical extent, always containing [0, 1] so a curve that only
        moves inside the unit range does not jump scale as it is dragged."""
        if self.isColor or not self._knots:
            return 0.0, 1.0
        values = [k[1] for k in self._knots]
        return min(0.0, min(values)), max(1.0, max(values))

    def _plotRect(self):
        return self.rect().adjusted(self.MARGIN, self.MARGIN,
                                    -self.MARGIN, -self.MARGIN)

    def _toPixel(self, position, value):
        rect = self._plotRect()
        low, high = self._valueRange()
        span = high - low or 1.0
        x = rect.left() + rect.width() * max(0.0, min(1.0, position))
        if self.isColor:
            return QtCore.QPointF(x, rect.bottom() - self.RADIUS - 1)
        y = rect.bottom() - rect.height() * ((value - low) / span)
        return QtCore.QPointF(x, y)

    def _fromPixel(self, point):
        rect = self._plotRect()
        low, high = self._valueRange()
        position = 0.0 if rect.width() <= 0 else \
            (point.x() - rect.left()) / float(rect.width())
        value = 0.0 if rect.height() <= 0 else \
            (rect.bottom() - point.y()) / float(rect.height())
        return (max(0.0, min(1.0, position)), low + (high - low) * value)

    def _knotAt(self, point):
        for index, (position, value, _interp) in enumerate(self._knots):
            pixel = self._toPixel(position, value if not self.isColor else 0.0)
            if (pixel - QtCore.QPointF(point)).manhattanLength() \
                    <= self.PICK_DISTANCE * 1.5:
                return index
        return -1

    # ---- painting ------------------------------------------------------

    def paintEvent(self, _event):
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        rect = self._plotRect()
        painter.fillRect(self.rect(), self.palette().base())
        painter.setPen(QtGui.QPen(QtGui.QColor(90, 90, 90)))
        painter.drawRect(rect)

        if not self._knots:
            return
        if self.isColor:
            self._paintColorBand(painter, rect)
        else:
            self._paintCurve(painter, rect)

        for index, (position, value, _interp) in enumerate(self._knots):
            pixel = self._toPixel(position, value)
            selected = index == self._selected
            painter.setPen(QtGui.QPen(
                QtGui.QColor(255, 255, 255) if selected
                else QtGui.QColor(40, 40, 40), 1.5))
            if self.isColor:
                painter.setBrush(QtGui.QBrush(_toColor(value)))
                painter.drawRect(QtCore.QRectF(
                    pixel.x() - self.RADIUS, pixel.y() - self.RADIUS,
                    self.RADIUS * 2, self.RADIUS * 2))
            else:
                painter.setBrush(QtGui.QBrush(
                    QtGui.QColor(255, 196, 92) if selected
                    else QtGui.QColor(120, 170, 230)))
                painter.drawEllipse(pixel, self.RADIUS, self.RADIUS)

    def _paintCurve(self, painter, rect):
        low, high = self._valueRange()
        # The unit gridlines, so a value of 1 is readable without a ruler.
        painter.setPen(QtGui.QPen(QtGui.QColor(70, 70, 70), 1,
                                  QtCore.Qt.DotLine))
        for mark in (0.0, 1.0):
            if low <= mark <= high:
                y = self._toPixel(0.0, mark).y()
                painter.drawLine(QtCore.QPointF(rect.left(), y),
                                 QtCore.QPointF(rect.right(), y))
        path = QtGui.QPainterPath()
        steps = max(2, rect.width())
        for step in range(int(steps) + 1):
            x = step / float(steps)
            point = self._toPixel(x, exprApi.EvaluateCurve(self._knots, x))
            if step == 0:
                path.moveTo(point)
            else:
                path.lineTo(point)
        painter.setPen(QtGui.QPen(QtGui.QColor(150, 210, 150), 1.6))
        painter.setBrush(QtCore.Qt.NoBrush)
        painter.drawPath(path)

    def _paintColorBand(self, painter, rect):
        # A gradient rather than a column of one-pixel fills: those landed on
        # fractional x and banded the ramp with a moire stripe.
        band = QtCore.QRectF(rect.left() + 1, rect.top() + 1,
                             rect.width() - 2, rect.height() - 14)
        gradient = QtGui.QLinearGradient(band.topLeft(), band.topRight())
        steps = 48
        for step in range(steps + 1):
            x = step / float(steps)
            gradient.setColorAt(x, _toColor(exprApi.EvaluateCurve(self._knots, x)))
        painter.fillRect(band, QtGui.QBrush(gradient))
        painter.setPen(QtGui.QPen(QtGui.QColor(90, 90, 90)))
        painter.setBrush(QtCore.Qt.NoBrush)
        painter.drawRect(band)

    # ---- input ---------------------------------------------------------

    def mousePressEvent(self, event):
        index = self._knotAt(event.position() if hasattr(event, "position")
                             else event.pos())
        if event.button() == QtCore.Qt.RightButton:
            if index >= 0:
                self.selectKnot(index)
                self.removeSelected()
            return
        if event.button() != QtCore.Qt.LeftButton:
            return
        if index >= 0:
            self.selectKnot(index)
            self._dragging = True
            self._onDragChanged(True)
            return
        position, value = self._fromPixel(
            event.position() if hasattr(event, "position") else event.pos())
        interp = self._knots[0][2] if self._knots else exprApi.INTERP_MONOTONE
        knot = (position,
                exprApi.EvaluateCurve(self._knots, position) if self.isColor
                else value,
                interp)
        knots = self._knots + [knot]
        knots.sort(key=lambda k: k[0])
        self._knots = knots
        self.selectKnot(knots.index(knot))
        self._onChanged(self.knots())

    def mouseMoveEvent(self, event):
        if not self._dragging or self._selected < 0:
            return
        position, value = self._fromPixel(
            event.position() if hasattr(event, "position") else event.pos())
        self.setSelectedKnot(position=position,
                             value=None if self.isColor else value)
        self._onSelected(self._selected)

    def mouseReleaseEvent(self, _event):
        if self._dragging:
            self._dragging = False
            self._onDragChanged(False)

    def mouseDoubleClickEvent(self, event):
        if not self.isColor:
            return
        index = self._knotAt(event.position() if hasattr(event, "position")
                             else event.pos())
        if index < 0:
            return
        self.selectKnot(index)
        chosen = QtWidgets.QColorDialog.getColor(
            _toColor(self._knots[index][1]), self, "Pick a knot colour")
        if chosen.isValid():
            self.setSelectedKnot(value=_fromColor(chosen))

    def keyPressEvent(self, event):
        if event.key() in (QtCore.Qt.Key_Delete, QtCore.Qt.Key_Backspace):
            self.removeSelected()
            return
        super(CurveWidget, self).keyPressEvent(event)


class CurveControl(_Control):
    """A curve widget plus the selected knot's position, value and
    interpolation, so a knot can be typed as well as dragged."""

    def __init__(self, index, control, onChanged, onDragChanged, parent=None):
        super(CurveControl, self).__init__(index, control, onChanged,
                                           onDragChanged, parent)
        self.isColor = control.kind == "ccurve"

        body = QtWidgets.QVBoxLayout()
        body.setContentsMargins(0, 0, 0, 0)
        body.setSpacing(2)
        self._outer.addLayout(body, 1)

        self.curve = CurveWidget(self.isColor, self._onKnotsChanged,
                                 onDragChanged, self._onKnotSelected)
        body.addWidget(self.curve)

        row = QtWidgets.QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(4)
        row.addWidget(QtWidgets.QLabel("at"))
        self.positionBox = QtWidgets.QDoubleSpinBox()
        self.positionBox.setDecimals(3)
        self.positionBox.setRange(0.0, 1.0)
        self.positionBox.setSingleStep(0.05)
        self.positionBox.setFixedWidth(88)
        self.positionBox.setKeyboardTracking(False)
        self.positionBox.valueChanged.connect(self._onPositionTyped)
        row.addWidget(self.positionBox)

        if self.isColor:
            self.valueButton = QtWidgets.QPushButton()
            self.valueButton.setFixedSize(34, 18)
            self.valueButton.setToolTip("Colour of the selected knot")
            self.valueButton.clicked.connect(self._pickKnotColor)
            row.addWidget(self.valueButton)
            self.valueBox = None
        else:
            self.valueBox = QtWidgets.QDoubleSpinBox()
            self.valueBox.setDecimals(4)
            self.valueBox.setRange(-1e9, 1e9)
            self.valueBox.setSingleStep(0.05)
            self.valueBox.setFixedWidth(VALUE_WIDTH)
            self.valueBox.setKeyboardTracking(False)
            self.valueBox.valueChanged.connect(self._onValueTyped)
            row.addWidget(self.valueBox)
            self.valueButton = None

        self.interpCombo = QtWidgets.QComboBox()
        for code, name in enumerate(exprApi.INTERP_NAMES):
            self.interpCombo.addItem(name, code)
        self.interpCombo.setToolTip(
            "How the segment ENDING at this knot is interpolated, which is "
            "the sense SeExpr's curve() gives the code.")
        self.interpCombo.currentIndexChanged.connect(self._onInterpChanged)
        row.addWidget(self.interpCombo)

        self.deleteButton = QtWidgets.QPushButton("Delete knot")
        self.deleteButton.clicked.connect(self.curve.removeSelected)
        row.addWidget(self.deleteButton)
        row.addStretch(1)
        body.addLayout(row)

        self.curve.setKnots(control.data["knots"], selected=0)
        self._onKnotSelected(0)

    def knots(self):
        return self.curve.knots()

    def setValue(self, knots):
        self._updating = True
        try:
            self.curve.setKnots(knots)
            self._onKnotSelected(self.curve.selectedIndex())
        finally:
            self._updating = False

    def _onKnotsChanged(self, knots):
        self.report(knots)

    def _onKnotSelected(self, index):
        knots = self.curve.knots()
        enabled = 0 <= index < len(knots)
        for widget in (self.positionBox, self.interpCombo, self.deleteButton):
            widget.setEnabled(enabled)
        if self.valueBox is not None:
            self.valueBox.setEnabled(enabled)
        if self.valueButton is not None:
            self.valueButton.setEnabled(enabled)
        if not enabled:
            return
        position, value, interp = knots[index]
        wasUpdating = self._updating
        self._updating = True
        try:
            self.positionBox.setValue(position)
            if self.valueBox is not None:
                self.valueBox.setValue(value)
            else:
                self.valueButton.setIcon(QtGui.QIcon(_swatch(_toColor(value))))
                self.valueButton.setIconSize(QtCore.QSize(28, 14))
            self.interpCombo.setCurrentIndex(
                max(0, min(int(interp), len(exprApi.INTERP_NAMES) - 1)))
        finally:
            self._updating = wasUpdating

    def _onPositionTyped(self, value):
        if self._updating:
            return
        self.curve.setSelectedKnot(position=value)

    def _onValueTyped(self, value):
        if self._updating:
            return
        self.curve.setSelectedKnot(value=value)

    def _onInterpChanged(self, index):
        if self._updating or index < 0:
            return
        self.curve.setSelectedKnot(interp=self.interpCombo.itemData(index))

    def _pickKnotColor(self):
        index = self.curve.selectedIndex()
        knots = self.curve.knots()
        if not 0 <= index < len(knots):
            return
        chosen = QtWidgets.QColorDialog.getColor(
            _toColor(knots[index][1]), self, "Pick a knot colour")
        if chosen.isValid():
            self.curve.setSelectedKnot(value=_fromColor(chosen))


class StringControl(_Control):
    """`$s = "..."; # string`.

    The engine rejects string literals today, so this control exists to make
    the model complete and the rejection legible: validation flags the text,
    and the control still shows what the text says."""

    def __init__(self, index, control, onChanged, onDragChanged, parent=None):
        super(StringControl, self).__init__(index, control, onChanged,
                                            onDragChanged, parent)
        self.edit = QtWidgets.QLineEdit(control.data["value"])
        self.edit.setToolTip(
            "String literals are not part of the expression language the "
            "engine accepts; validation will say so.")
        self.edit.editingFinished.connect(
            lambda: self.report(self.edit.text()))
        self._outer.addWidget(self.edit, 1)

    def value(self):
        return self.edit.text()

    def setValue(self, value):
        self._updating = True
        try:
            self.edit.setText(value)
        finally:
            self._updating = False


def _signature(control):
    """What must change before a control's WIDGET has to be rebuilt.

    A value alone never does: rebuilding mid-drag would pull the knot or the
    slider out from under the pointer."""
    if control.kind in ("curve", "ccurve"):
        return (control.name, control.kind, len(control.data["knots"]),
                control.data["lookup"])
    if control.kind in ("vector", "color"):
        return (control.name, control.kind, len(control.data["values"]),
                control.data["minimum"], control.data["maximum"])
    if control.kind in ("int", "float"):
        return (control.name, control.kind, control.data["minimum"],
                control.data["maximum"])
    return (control.name, control.kind)


def BuildControl(index, control, onChanged, onDragChanged, onRangeChanged):
    if control.kind in ("int", "float"):
        return NumberControl(index, control, onChanged, onDragChanged,
                             onRangeChanged)
    if control.kind in ("vector", "color"):
        return VectorControl(index, control, onChanged, onDragChanged)
    if control.kind in ("curve", "ccurve"):
        return CurveControl(index, control, onChanged, onDragChanged)
    return StringControl(index, control, onChanged, onDragChanged)


class ControlPanel(QtWidgets.QWidget):
    """Every control the expression declares, over the plain-literal fallback.

    Rebuilt only when the set of controls actually changes shape, so an edit
    made through a control does not destroy the widget that made it."""

    def __init__(self, onChanged, onRangeChanged, onDragChanged, onAddWidget,
                 literalPanel, parent=None):
        super(ControlPanel, self).__init__(parent)
        self._onChanged = onChanged
        self._onRangeChanged = onRangeChanged
        self._onDragChanged = onDragChanged
        self.controls = []
        self.rows = []
        self._signatures = ()
        self.literalPanel = literalPanel

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(4)

        top = QtWidgets.QHBoxLayout()
        self.addButton = QtWidgets.QPushButton("Add Widget...")
        self.addButton.setToolTip(
            "Declare a new $variable at the top of the expression and give it "
            "a control.")
        self.addButton.clicked.connect(onAddWidget)
        top.addWidget(self.addButton)
        top.addStretch(1)
        outer.addLayout(top)

        self.empty = QtWidgets.QLabel(
            "No controls. A top-level `$name = 0.5; # 0, 1` gives this panel a "
            "slider; `# color`, curve(...) and ccurve(...) give the other "
            "control kinds. Use Add Widget to write one.")
        self.empty.setWordWrap(True)
        self.empty.setAlignment(QtCore.Qt.AlignTop)
        outer.addWidget(self.empty)

        self._area = QtWidgets.QScrollArea()
        self._area.setWidgetResizable(True)
        self._area.setFrameShape(QtWidgets.QFrame.NoFrame)
        self._body = QtWidgets.QWidget()
        self._layout = QtWidgets.QVBoxLayout(self._body)
        self._layout.setContentsMargins(2, 2, 2, 2)
        self._layout.setSpacing(4)
        self._layout.addStretch(1)
        self._area.setWidget(self._body)
        outer.addWidget(self._area, 1)
        self._area.hide()

        self.literalHeading = QtWidgets.QLabel("Loose numbers")
        self.literalHeading.setToolTip(
            "Numeric literals that no $variable holds. Assign one to a "
            "variable to give it a proper control.")
        outer.addWidget(self.literalHeading)
        outer.addWidget(literalPanel, 1)

    # ---- content -------------------------------------------------------

    def setSource(self, text):
        """Rescan `text`, rebuilding only what changed shape."""
        controls = exprApi.ScanControls(text)
        signatures = tuple(_signature(c) for c in controls)
        self.controls = controls
        if signatures != self._signatures:
            self._signatures = signatures
            self._rebuild()
        else:
            for row, control in zip(self.rows, controls):
                row.setControl(control)
        self._refreshLiterals(text)

    def refreshOffsets(self, text):
        """After a rewrite this panel made: take the new offsets and leave
        every widget exactly as the user left it."""
        controls = exprApi.ScanControls(text)
        if tuple(_signature(c) for c in controls) != self._signatures:
            self.setSource(text)
            return
        self.controls = controls
        for row, control in zip(self.rows, controls):
            row.setControl(control)
        self._refreshLiterals(text, offsetsOnly=True)

    def _refreshLiterals(self, text, offsetsOnly=False):
        # After an edit the panel itself made, only the offsets moved: a
        # rebuild here would take the slider away from the pointer dragging it.
        exclude = exprApi.ControlSpans(self.controls)
        if offsetsOnly:
            self.literalPanel.refreshOffsets(text, exclude=exclude)
        else:
            self.literalPanel.setSource(text, exclude=exclude)
        visible = bool(self.literalPanel.rows)
        self.literalHeading.setVisible(visible)
        self.literalPanel.setVisible(visible)

    def _rebuild(self):
        for row in self.rows:
            self._layout.removeWidget(row)
            row.setParent(None)
            row.deleteLater()
        self.rows = []
        for index, control in enumerate(self.controls):
            row = BuildControl(index, control, self._onChanged,
                               self._onDragChanged, self._onRangeChanged)
            self._layout.insertWidget(self._layout.count() - 1, row)
            self.rows.append(row)
        self.empty.setVisible(not self.rows)
        self._area.setVisible(bool(self.rows))


class AddWidgetDialog(QtWidgets.QDialog):
    """SeExpr's ExprAddDialog: a name, a type, and the initial value or range
    that type needs. Accepting it inserts one declaration line."""

    def __init__(self, parent, existingNames=()):
        super(AddWidgetDialog, self).__init__(parent)
        self.setWindowTitle("Add Widget")
        self._existing = set(existingNames)

        layout = QtWidgets.QFormLayout(self)
        self.nameEdit = QtWidgets.QLineEdit(self._suggestName())
        layout.addRow("Name:", self.nameEdit)

        self.kindCombo = QtWidgets.QComboBox()
        for label, kind, _initial in WIDGET_KINDS:
            self.kindCombo.addItem(label, kind)
        self.kindCombo.setCurrentIndex(1)  # Float, the common case
        self.kindCombo.currentIndexChanged.connect(self._onKindChanged)
        layout.addRow("Type:", self.kindCombo)

        self.valueBox = QtWidgets.QDoubleSpinBox()
        self.valueBox.setDecimals(4)
        self.valueBox.setRange(-1e9, 1e9)
        self.valueBox.setValue(0.5)
        layout.addRow("Value:", self.valueBox)
        self.valueLabel = layout.labelForField(self.valueBox)

        self.minimumBox = QtWidgets.QDoubleSpinBox()
        self.minimumBox.setDecimals(4)
        self.minimumBox.setRange(-1e9, 1e9)
        self.minimumBox.setValue(0.0)
        self.maximumBox = QtWidgets.QDoubleSpinBox()
        self.maximumBox.setDecimals(4)
        self.maximumBox.setRange(-1e9, 1e9)
        self.maximumBox.setValue(1.0)
        rangeRow = QtWidgets.QHBoxLayout()
        rangeRow.addWidget(self.minimumBox)
        rangeRow.addWidget(self.maximumBox)
        self.rangeWidget = QtWidgets.QWidget()
        self.rangeWidget.setLayout(rangeRow)
        layout.addRow("Range:", self.rangeWidget)
        self.rangeLabel = layout.labelForField(self.rangeWidget)

        self.colorButton = QtWidgets.QPushButton()
        self.colorButton.setFixedSize(40, 20)
        self._color = QtGui.QColor(255, 255, 255)
        self.colorButton.clicked.connect(self._pickColor)
        layout.addRow("Colour:", self.colorButton)
        self.colorLabel = layout.labelForField(self.colorButton)

        self.textEdit = QtWidgets.QLineEdit()
        layout.addRow("Text:", self.textEdit)
        self.textLabel = layout.labelForField(self.textEdit)

        self.lookupEdit = QtWidgets.QLineEdit("$t")
        self.lookupEdit.setToolTip(
            "The expression the curve is looked up by, curve()'s first "
            "argument.")
        layout.addRow("Lookup:", self.lookupEdit)
        self.lookupLabel = layout.labelForField(self.lookupEdit)

        self.note = QtWidgets.QLabel()
        self.note.setWordWrap(True)
        layout.addRow(self.note)

        self.buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
        self.buttons.accepted.connect(self._onAccept)
        self.buttons.rejected.connect(self.reject)
        layout.addRow(self.buttons)

        # Every input feeds the preview line, or it would show a declaration
        # the dialog is no longer describing.
        self.nameEdit.textChanged.connect(self._refreshNote)
        self.valueBox.valueChanged.connect(self._refreshNote)
        self.minimumBox.valueChanged.connect(self._refreshNote)
        self.maximumBox.valueChanged.connect(self._refreshNote)
        self.textEdit.textChanged.connect(self._refreshNote)
        self.lookupEdit.textChanged.connect(self._refreshNote)
        self._refreshSwatch()
        self._onKindChanged(self.kindCombo.currentIndex())

    def _suggestName(self):
        for n in range(1, 100):
            candidate = "$var%d" % n
            if candidate not in self._existing:
                return candidate
        return "$var"

    def kind(self):
        return self.kindCombo.currentData()

    def name(self):
        text = self.nameEdit.text().strip()
        if text and not text.startswith("$"):
            text = "$" + text
        return text

    def _onKindChanged(self, _index):
        kind = self.kind()
        isNumber = kind in ("int", "float")
        isVector = kind in ("vector", "color")
        isCurve = kind in ("curve", "ccurve")
        for widget, label, shown in (
                (self.valueBox, self.valueLabel, isNumber),
                (self.rangeWidget, self.rangeLabel, isNumber),
                (self.colorButton, self.colorLabel, isVector),
                (self.textEdit, self.textLabel, kind == "string"),
                (self.lookupEdit, self.lookupLabel, isCurve)):
            widget.setVisible(shown)
            if label is not None:
                label.setVisible(shown)
        self.valueBox.setDecimals(0 if kind == "int" else 4)
        self.minimumBox.setDecimals(0 if kind == "int" else 4)
        self.maximumBox.setDecimals(0 if kind == "int" else 4)
        if kind == "int":
            self.valueBox.setValue(round(self.valueBox.value()))
            self.maximumBox.setValue(max(1.0, round(self.maximumBox.value())))
        self._refreshNote()

    def _pickColor(self):
        chosen = QtWidgets.QColorDialog.getColor(self._color, self,
                                                 "Pick a colour")
        if chosen.isValid():
            self._color = chosen
            self._refreshSwatch()
            self._refreshNote()

    def _refreshSwatch(self):
        self.colorButton.setIcon(QtGui.QIcon(_swatch(self._color)))
        self.colorButton.setIconSize(QtCore.QSize(28, 14))

    def value(self):
        kind = self.kind()
        if kind in ("int", "float"):
            return self.valueBox.value()
        if kind in ("vector", "color"):
            return _fromColor(self._color)
        if kind == "string":
            return self.textEdit.text()
        for label, candidate, initial in WIDGET_KINDS:
            if candidate == kind:
                return initial
        return 0.0

    def declaration(self):
        """The `$name = ...; # ...` line this dialog describes."""
        return exprApi.DeclarationFor(
            self.name(), self.kind(), self.value(),
            minimum=self.minimumBox.value(), maximum=self.maximumBox.value(),
            lookup=self.lookupEdit.text().strip() or "$t")

    def _refreshNote(self):
        name = self.name()
        if not name:
            self.note.setText("A name is needed.")
        elif name in self._existing:
            self.note.setText("%s is already declared." % name)
        else:
            self.note.setText(self.declaration())
        ok = self.buttons.button(QtWidgets.QDialogButtonBox.Ok)
        if ok is not None:
            ok.setEnabled(bool(name) and name not in self._existing)

    def _onAccept(self):
        name = self.name()
        if not name or name in self._existing:
            return
        self.accept()
