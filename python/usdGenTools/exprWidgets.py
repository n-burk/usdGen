# The panels around the text: literal sliders, the function and variable
# browsers, and the saved-expression library.
#
# None of them touch a stage or the engine. They are given values and lists and
# they call back; the dock owns every decision about what that means.

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from . import exprApi, exprLibrary


class LiteralRow(QtWidgets.QWidget):
    """One numeric literal from the source, as a slider plus a spinbox.

    The slider is integral, so the source of truth for the value is the
    spinbox; the slider is a position on the editable [min, max] range."""

    STEPS = 1000

    # Column widths shared with the panel's header, so the two line up.
    LABEL_WIDTH = 178
    VALUE_WIDTH = 112
    RANGE_WIDTH = 66

    def __init__(self, index, literal, onChanged, onDragChanged, parent=None):
        super(LiteralRow, self).__init__(parent)
        self._index = index
        self._onChanged = onChanged
        self._onDragChanged = onDragChanged
        self._updating = False

        low, high = exprApi.DefaultRange(literal.value)

        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)

        self.label = QtWidgets.QLabel()
        self.label.setFixedWidth(self.LABEL_WIDTH)
        self.label.setFont(
            QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont))
        layout.addWidget(self.label)

        self.slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
        self.slider.setRange(0, self.STEPS)
        self.slider.setMinimumWidth(60)
        layout.addWidget(self.slider, 1)

        self.valueBox = QtWidgets.QDoubleSpinBox()
        self.valueBox.setDecimals(4)
        self.valueBox.setRange(-1e9, 1e9)
        self.valueBox.setFixedWidth(self.VALUE_WIDTH)
        self.valueBox.setKeyboardTracking(False)
        layout.addWidget(self.valueBox)

        self.minimumBox = self._rangeBox(low)
        layout.addWidget(self.minimumBox)
        self.maximumBox = self._rangeBox(high)
        layout.addWidget(self.maximumBox)

        self.setLabel(literal.label)
        self._setRange(low, high)
        self.setValue(literal.value)

        self.slider.valueChanged.connect(self._onSlider)
        self.slider.sliderPressed.connect(lambda: self._onDragChanged(True))
        self.slider.sliderReleased.connect(lambda: self._onDragChanged(False))
        self.valueBox.valueChanged.connect(self._onSpin)
        self.minimumBox.valueChanged.connect(self._onRangeEdited)
        self.maximumBox.valueChanged.connect(self._onRangeEdited)

    def _rangeBox(self, value):
        box = QtWidgets.QDoubleSpinBox()
        box.setDecimals(2)
        box.setRange(-1e9, 1e9)
        box.setFixedWidth(self.RANGE_WIDTH)
        box.setKeyboardTracking(False)
        # No spin buttons: the range is typed occasionally, not nudged, and
        # the buttons cost more width here than the digits do.
        box.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
        box.setValue(value)
        box.setToolTip("Slider range; edit to widen or narrow it.")
        return box

    def _setRange(self, low, high):
        if high <= low:
            high = low + 1.0
        self._low, self._high = low, high
        self.valueBox.setSingleStep(max(1e-4, (high - low) / 100.0))

    def value(self):
        return self.valueBox.value()

    def setValue(self, value):
        """Show `value` without reporting it back as an edit."""
        self._updating = True
        try:
            self.valueBox.setValue(value)
            span = self._high - self._low
            position = 0 if span <= 0 else (value - self._low) / span
            self.slider.setValue(int(round(max(0.0, min(1.0, position)) * self.STEPS)))
        finally:
            self._updating = False

    def setLabel(self, label):
        metrics = QtGui.QFontMetrics(self.label.font())
        self.label.setText(metrics.elidedText(
            label, QtCore.Qt.ElideRight, self.LABEL_WIDTH - 2))
        self.label.setToolTip(label)

    def _onSlider(self, position):
        if self._updating:
            return
        value = self._low + (self._high - self._low) * (float(position) / self.STEPS)
        self._updating = True
        try:
            self.valueBox.setValue(value)
        finally:
            self._updating = False
        self._onChanged(self._index, value)

    def _onSpin(self, value):
        if self._updating:
            return
        self._updating = True
        try:
            span = self._high - self._low
            position = 0 if span <= 0 else (value - self._low) / span
            self.slider.setValue(int(round(max(0.0, min(1.0, position)) * self.STEPS)))
        finally:
            self._updating = False
        self._onChanged(self._index, value)

    def _onRangeEdited(self, _value):
        if self._updating:
            return
        self._setRange(self.minimumBox.value(), self.maximumBox.value())
        self.setValue(self.valueBox.value())


class LiteralPanel(QtWidgets.QWidget):
    """A row per numeric literal in the source.

    Rebuilt only when the set of literals actually changes, so a drag does not
    pull the slider out from under the pointer."""

    def __init__(self, onChanged, onDragChanged, parent=None):
        super(LiteralPanel, self).__init__(parent)
        self._onChanged = onChanged
        self._onDragChanged = onDragChanged
        self.literals = []
        self.rows = []

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        self.empty = QtWidgets.QLabel(
            "No numeric literals in this expression.")
        self.empty.setAlignment(QtCore.Qt.AlignCenter)
        self.empty.setWordWrap(True)
        outer.addWidget(self.empty)

        self._header = self._buildHeader()
        outer.addWidget(self._header)

        self._area = QtWidgets.QScrollArea()
        self._area.setWidgetResizable(True)
        self._area.setFrameShape(QtWidgets.QFrame.NoFrame)
        self._body = QtWidgets.QWidget()
        self._layout = QtWidgets.QVBoxLayout(self._body)
        self._layout.setContentsMargins(2, 2, 2, 2)
        self._layout.setSpacing(2)
        self._layout.addStretch(1)
        self._area.setWidget(self._body)
        outer.addWidget(self._area, 1)
        self._area.hide()
        self._header.hide()

    def _buildHeader(self):
        """Column titles, on the same fixed widths the rows use."""
        header = QtWidgets.QWidget()
        layout = QtWidgets.QHBoxLayout(header)
        layout.setContentsMargins(2, 0, 2, 0)
        layout.setSpacing(4)
        for text, width, stretch in (
                ("Literal", LiteralRow.LABEL_WIDTH, 0),
                ("", 0, 1),
                ("Value", LiteralRow.VALUE_WIDTH, 0),
                ("Min", LiteralRow.RANGE_WIDTH, 0),
                ("Max", LiteralRow.RANGE_WIDTH, 0)):
            label = QtWidgets.QLabel(text)
            if width:
                label.setFixedWidth(width)
            layout.addWidget(label, stretch)
        return header

    @staticmethod
    def _keep(literals, exclude):
        """Drop the literals a proper control already drives, so a number is
        never on two widgets at once."""
        if not exclude:
            return literals
        return [l for l in literals
                if not any(start <= l.start and l.end <= end
                           for start, end in exclude)]

    def setSource(self, text, exclude=()):
        """Rescan `text`. Rows are rebuilt only when the literals differ in
        count or in spelling; otherwise only their offsets and labels move."""
        literals = self._keep(exprApi.ScanLiterals(text), exclude)
        if len(literals) == len(self.literals) and all(
                a.text == b.text for a, b in zip(literals, self.literals)):
            self.literals = literals
            for row, literal in zip(self.rows, literals):
                row.setLabel(literal.label)
            return
        self.literals = literals
        self._rebuild()

    def refreshOffsets(self, text, exclude=()):
        """After a rewrite: take the new offsets without touching the widgets,
        as long as the literal count is unchanged."""
        literals = self._keep(exprApi.ScanLiterals(text), exclude)
        if len(literals) != len(self.literals):
            self.literals = literals
            self._rebuild()
            return
        self.literals = literals
        for row, literal in zip(self.rows, literals):
            row.setLabel(literal.label)

    def _rebuild(self):
        for row in self.rows:
            self._layout.removeWidget(row)
            row.setParent(None)
            row.deleteLater()
        self.rows = []
        for index, literal in enumerate(self.literals):
            row = LiteralRow(index, literal, self._onChanged, self._onDragChanged)
            self._layout.insertWidget(self._layout.count() - 1, row)
            self.rows.append(row)
        self.empty.setVisible(not self.rows)
        self._area.setVisible(bool(self.rows))
        self._header.setVisible(bool(self.rows))


class _Browser(QtWidgets.QWidget):
    """Shared shell: a tree, an Insert button, and double-click to insert."""

    def __init__(self, headers, onInsert, parent=None):
        super(_Browser, self).__init__(parent)
        self._onInsert = onInsert
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)

        self.tree = QtWidgets.QTreeWidget()
        self.tree.setHeaderLabels(headers)
        self.tree.setRootIsDecorated(False)
        self.tree.setAlternatingRowColors(True)
        self.tree.setUniformRowHeights(True)
        self.tree.itemDoubleClicked.connect(self._onDoubleClicked)
        layout.addWidget(self.tree, 1)

        self.insertButton = QtWidgets.QPushButton("Insert")
        self.insertButton.clicked.connect(self.insertSelected)
        layout.addWidget(self.insertButton)

    def _onDoubleClicked(self, item, _column):
        self._insert(item)

    def insertSelected(self):
        items = self.tree.selectedItems()
        if items:
            self._insert(items[0])

    def _insert(self, item):
        payload = item.data(0, QtCore.Qt.UserRole)
        if payload:
            self._onInsert(payload)


class FunctionBrowser(_Browser):
    """The functions and operators the engine accepts, grouped by category.

    The list and the categories are whatever the C ABI reports; nothing about
    the language is written down here."""

    def __init__(self, onInsert, parent=None):
        super(FunctionBrowser, self).__init__(
            ["Function", "Description"], onInsert, parent)
        self.tree.setRootIsDecorated(True)
        self.functions = []

    def categories(self):
        """The category names in the order the tree shows them."""
        return [self.tree.topLevelItem(i).text(0)
                for i in range(self.tree.topLevelItemCount())]

    def setFunctions(self, functions):
        self.functions = list(functions)
        self.tree.clear()
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)
        groups = {}
        order = []
        for function in self.functions:
            category = function.category or "Other"
            if category not in groups:
                groups[category] = []
                order.append(category)
            groups[category].append(function)
        # Alphabetical, except that "Other" is a leftovers bin and belongs last
        # wherever its name would otherwise put it.
        order.sort(key=lambda name: (name == "Other", name.lower()))
        for category in order:
            parent = QtWidgets.QTreeWidgetItem([category, ""])
            parent.setFirstColumnSpanned(True)
            parent.setFlags(QtCore.Qt.ItemIsEnabled)
            self.tree.addTopLevelItem(parent)
            for function in groups[category]:
                item = QtWidgets.QTreeWidgetItem(
                    [function.signature, function.doc])
                item.setFont(0, font)
                item.setToolTip(0, "%s\n%s" % (function.signature, function.doc))
                item.setToolTip(1, function.doc)
                item.setData(0, QtCore.Qt.UserRole, function.insert)
                parent.addChild(item)
            parent.setExpanded(True)
        self.tree.resizeColumnToContents(0)
        # Capped: the longest signature would otherwise take the whole pane
        # and leave the description with nothing at the width a dock gets.
        self.tree.setColumnWidth(0, min(self.tree.columnWidth(0), 190))
        if not self.functions:
            self.tree.setHeaderLabels(["Function", "unavailable"])


class VariableBrowser(_Browser):
    """The expression variables, greyed out where the current evaluation
    domain does not define them."""

    def __init__(self, onInsert, parent=None):
        super(VariableBrowser, self).__init__(
            ["Variable", "Type", "Domains", "Meaning"], onInsert, parent)
        self.variables = []

    def setVariables(self, variables):
        self.variables = list(variables)
        self.tree.clear()
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)
        disabled = QtGui.QBrush(QtGui.QColor(128, 128, 128))
        for variable in self.variables:
            if variable.components == 0:
                typeText = "consumer's"
            elif variable.components > 1:
                typeText = "%s[%d]" % (variable.scalarType, variable.components)
            else:
                typeText = variable.scalarType
            item = QtWidgets.QTreeWidgetItem(
                [variable.name, typeText, ", ".join(variable.domains),
                 variable.doc])
            item.setFont(0, font)
            tip = "%s  %s\nValid in: %s\n%s" % (
                variable.name, typeText, ", ".join(variable.domains) or "none",
                variable.doc)
            for column in range(4):
                item.setToolTip(column, tip)
                if not variable.valid:
                    item.setForeground(column, disabled)
            item.setData(0, QtCore.Qt.UserRole, variable.name)
            if not variable.valid:
                item.setToolTip(0, tip + "\n(not available in this domain)")
            self.tree.addTopLevelItem(item)
        self.tree.resizeColumnToContents(0)
        self.tree.resizeColumnToContents(1)


class LibraryBrowser(QtWidgets.QWidget):
    """SeExpr's ExprBrowser: the saved `.se` expressions, by library.

    Loading replaces the editor's text, so it goes through the dock, which is
    the only thing that knows whether there are unapplied edits to lose."""

    ANY_DOMAIN = "any domain"

    def __init__(self, onLoad, onSave, parent=None):
        super(LibraryBrowser, self).__init__(parent)
        self._onLoad = onLoad
        self._onSave = onSave
        self.entries = []

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)

        filters = QtWidgets.QHBoxLayout()
        self.searchEdit = QtWidgets.QLineEdit()
        self.searchEdit.setPlaceholderText("Search")
        self.searchEdit.setClearButtonEnabled(True)
        self.searchEdit.textChanged.connect(self._refreshTree)
        filters.addWidget(self.searchEdit, 1)
        self.domainCombo = QtWidgets.QComboBox()
        self.domainCombo.addItem(self.ANY_DOMAIN, "")
        for name in exprApi.DOMAIN_NAMES:
            self.domainCombo.addItem(name, name)
        self.domainCombo.setToolTip(
            "Show only expressions written for one evaluation domain. A file "
            "says which with a `# domain: point` line; files without one are "
            "always shown.")
        self.domainCombo.currentIndexChanged.connect(self._refreshTree)
        filters.addWidget(self.domainCombo)
        layout.addLayout(filters)

        self.tree = QtWidgets.QTreeWidget()
        self.tree.setHeaderLabels(["Expression", "Domain"])
        self.tree.setRootIsDecorated(True)
        self.tree.setUniformRowHeights(True)
        # Names are what the eye reads here; the domain is one short word, so
        # it takes only what it needs and the name column keeps the rest.
        self.tree.header().setSectionResizeMode(
            0, QtWidgets.QHeaderView.Stretch)
        self.tree.header().setSectionResizeMode(
            1, QtWidgets.QHeaderView.ResizeToContents)
        self.tree.itemDoubleClicked.connect(
            lambda item, column: self.loadSelected())
        self.tree.itemSelectionChanged.connect(self._onSelectionChanged)
        layout.addWidget(self.tree, 1)

        buttons = QtWidgets.QHBoxLayout()
        self.loadButton = QtWidgets.QPushButton("Load")
        self.loadButton.clicked.connect(self.loadSelected)
        self.saveButton = QtWidgets.QPushButton("Save")
        self.saveButton.setToolTip(
            "Overwrite the selected expression in the user library.")
        self.saveButton.clicked.connect(lambda: self._onSave(False))
        self.saveAsButton = QtWidgets.QPushButton("Save As...")
        self.saveAsButton.clicked.connect(lambda: self._onSave(True))
        self.refreshButton = QtWidgets.QPushButton("Refresh")
        self.refreshButton.clicked.connect(self.refresh)
        for button in (self.loadButton, self.saveButton, self.saveAsButton,
                       self.refreshButton):
            buttons.addWidget(button)
        buttons.addStretch(1)
        layout.addLayout(buttons)

        self.pathLabel = QtWidgets.QLabel()
        self.pathLabel.setWordWrap(True)
        self.pathLabel.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self.pathLabel)

        self.refresh()

    # ---- content -------------------------------------------------------

    def refresh(self):
        """Re-read the libraries from disk."""
        self.entries = exprLibrary.ListExpressions()
        self._refreshTree()

    def _refreshTree(self):
        wanted = self.selectedName()
        needle = self.searchEdit.text().strip().lower()
        domain = self.domainCombo.currentData() or ""
        self.tree.clear()
        groups = {}
        for entry in self.entries:
            if needle and needle not in entry.name.lower():
                continue
            # A file with no domain marker suits any domain, so filtering by
            # one must not hide it.
            if domain and entry.domain and entry.domain != domain:
                continue
            groups.setdefault(entry.library, []).append(entry)
        restore = None
        for library in sorted(groups):
            parent = QtWidgets.QTreeWidgetItem([library, ""])
            parent.setFlags(QtCore.Qt.ItemIsEnabled)
            self.tree.addTopLevelItem(parent)
            for entry in groups[library]:
                item = QtWidgets.QTreeWidgetItem([entry.name, entry.domain])
                item.setToolTip(0, entry.path)
                item.setData(0, QtCore.Qt.UserRole, entry)
                parent.addChild(item)
                if entry.name == wanted:
                    restore = item
            parent.setExpanded(True)
        if restore is not None:
            self.tree.setCurrentItem(restore)
        self._onSelectionChanged()

    def selectedEntry(self):
        items = self.tree.selectedItems()
        if not items:
            return None
        return items[0].data(0, QtCore.Qt.UserRole)

    def selectedName(self):
        entry = self.selectedEntry()
        return entry.name if entry else ""

    def selectEntry(self, name):
        """Select the entry called `name`. True when it was there."""
        for index in range(self.tree.topLevelItemCount()):
            parent = self.tree.topLevelItem(index)
            for child in range(parent.childCount()):
                item = parent.child(child)
                entry = item.data(0, QtCore.Qt.UserRole)
                if entry and entry.name == name:
                    self.tree.setCurrentItem(item)
                    return True
        return False

    def _onSelectionChanged(self):
        entry = self.selectedEntry()
        self.loadButton.setEnabled(entry is not None)
        self.saveButton.setEnabled(entry is not None)
        self.pathLabel.setText(entry.path if entry else
                               "Saves go to " + exprLibrary.UserDirectory())

    def loadSelected(self):
        entry = self.selectedEntry()
        if entry is None:
            return False
        self._onLoad(entry)
        return True
