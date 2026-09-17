# SeExpr expression editor dock for usdview.
#
# Edits the usdGen:expr:source of a UsdGenExpression prim in place. The dock
# follows usdview's prim selection: a selected UsdGenExpression prim is
# edited directly, and a selected operator offers every expression its
# attributes are connected to (the outputs:* connections the graph builders
# read), so the editor works from either end of a connection.
#
# Around the text sit the things a SeExpr editor is expected to have:
# highlighting, live validation against the real compiler, browsable function
# and variable references, a slider per numeric literal, the evaluation domain
# of the binding, and connect/disconnect for the selected operator.
#
# Everything the editor knows about the language comes from the engine through
# the usdGenImaging C ABI (usdGenToolsApi.h, bound in exprApi.py). Nothing
# about SeExpr is written down in this package.
#
# Writes go through the stage's current edit target, so they are undoable in
# the same way as any other usdview edit and the groom scene index recooks from
# the resulting stage notice.
#
# "Colour hair by value" overrides the hair material to show a value on every
# strand: the edited expression, a Ptex map it reads, or an operator attribute
# (usdGen:preview:* on the description, always in the session layer; see
# exprPreview.py).

from pxr import Sdf, Tf, Usd
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from . import (exprApi, exprAuthor, exprControls, exprEdit, exprHighlight,
               exprLibrary, exprPreview, exprWidgets)

EXPRESSION_TYPE = exprAuthor.EXPRESSION_TYPE
SOURCE_ATTR = exprAuthor.SOURCE_ATTR
OUTPUT_PREFIX = exprAuthor.OUTPUT_PREFIX
RESULT_ATTR = exprAuthor.RESULT_ATTR

TITLE = "SeExpr Expression Editor"

# How long the text must sit still before it is compiled. Long enough that
# typing a word does not queue a compile per keystroke, short enough that the
# status strip feels attached to the text.
VALIDATE_DELAY_MS = 300

_ACTIVE_EDITOR = None


def GetExpressionEditor(usdviewApi):
    """Return the process-wide editor dock, creating it on first use."""
    global _ACTIVE_EDITOR
    if _ACTIVE_EDITOR is None:
        _ACTIVE_EDITOR = ExpressionEditorDock(usdviewApi)
    return _ACTIVE_EDITOR


def ExpressionCandidates(prim):
    """UsdGenExpression prims relevant to `prim`: itself when it is one,
    otherwise every expression its attributes are connected to, in the
    order the connections are authored.

    A connection may name an output (`<Expr>.outputs:result`) or the
    expression PRIM itself; both spellings are valid and both are followed
    here, exactly as the two graph-desc builders resolve them."""
    if not prim or not prim.IsValid():
        return []
    if prim.GetTypeName() == EXPRESSION_TYPE:
        return [prim]
    found = []
    for attr in prim.GetAttributes():
        source = exprAuthor.ExpressionTarget(attr)
        if source and source not in found:
            found.append(source)
    return found


class ExpressionEditorDock(QtWidgets.QDockWidget):

    def __init__(self, usdviewApi):
        super(ExpressionEditorDock, self).__init__(TITLE, usdviewApi.qMainWindow)
        self.setObjectName("usdGenExpressionEditor")
        self._api = usdviewApi
        self._prim = None            # the UsdGenExpression being edited
        self._operator = None        # the prim whose attributes we may connect
        self._binding = None         # the attribute this expression drives
        self._noticeKey = None
        self._loading = False
        self._rewriting = False
        self._dragging = False
        self._coalescing = False
        self._previewing = False
        # The session layer's own usdGen:expr:source opinion from before a
        # preview replaced it: usdview's edit target IS the session layer, so
        # that opinion is often the applied text itself.
        self._previewBackup = None
        self._authoring = False
        self._appliedSource = ""
        self.diagnostics = []
        # Hair colour preview state: what the source combo lists, an operator
        # attribute the Connections tab asked for, and the description this
        # editor has authored a preview on.
        self._hairSources = []
        self._pinnedHairSource = None
        self._previewDescription = None
        self._hairExpression = None
        self._loadingHair = False
        self._buildUi()
        self._api.qMainWindow.addDockWidget(
            QtCore.Qt.RightDockWidgetArea, self)

        selection = self._api.dataModel.selection
        selection.signalPrimSelectionChanged.connect(self._onSelectionChanged)
        if hasattr(self._api.dataModel, "signalStageReplaced"):
            self._api.dataModel.signalStageReplaced.connect(self._onStageReplaced)
        self._listenToStage()
        self._loadReference()
        self.refreshFromSelection()

    # ---- UI ------------------------------------------------------------

    def _buildUi(self):
        body = QtWidgets.QWidget(self)
        layout = QtWidgets.QVBoxLayout(body)
        layout.setSpacing(4)
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)

        row = QtWidgets.QHBoxLayout()
        row.addWidget(QtWidgets.QLabel("Expression:"))
        self.targetCombo = QtWidgets.QComboBox()
        self.targetCombo.setSizeAdjustPolicy(
            QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.targetCombo.setMinimumContentsLength(24)
        self.targetCombo.currentIndexChanged.connect(self._onTargetChanged)
        row.addWidget(self.targetCombo, 1)
        layout.addLayout(row)

        binding = QtWidgets.QHBoxLayout()
        binding.addWidget(QtWidgets.QLabel("Evaluate:"))
        self.domainCombo = QtWidgets.QComboBox()
        for name in exprApi.DOMAIN_NAMES:
            self.domainCombo.addItem(name, name)
        self.domainCombo.setToolTip(
            "Where the expression runs: once per groom, once per curve root, "
            "or once per CV. Stored as the destination attribute's "
            "customData usdGen:evaluation.")
        self.domainCombo.currentIndexChanged.connect(self._onDomainChanged)
        binding.addWidget(self.domainCombo)
        self.outputTypeLabel = QtWidgets.QLabel()
        self.outputTypeLabel.setToolTip(
            "The type of the expression's outputs:result, which must match "
            "the attribute it drives.")
        binding.addWidget(self.outputTypeLabel, 1)
        self.liveApplyCheck = QtWidgets.QCheckBox("Live apply")
        self.liveApplyCheck.setToolTip(
            "Write every slider change straight to the stage so the viewport "
            "follows the drag.")
        binding.addWidget(self.liveApplyCheck)
        layout.addLayout(binding)

        self.outputsLabel = QtWidgets.QLabel()
        self.outputsLabel.setWordWrap(True)
        self.outputsLabel.hide()
        layout.addWidget(self.outputsLabel)

        self.sourceEdit = exprEdit.SourceEdit()
        self.sourceEdit.setFont(font)
        self.sourceEdit.setTabStopDistance(
            4 * QtGui.QFontMetricsF(font).horizontalAdvance(" "))
        self.sourceEdit.setPlaceholderText(
            "Select a UsdGenExpression prim, or an operator whose "
            "attributes are connected to one.")
        self.sourceEdit.textChanged.connect(self._onTextChanged)
        self.sourceEdit.cursorPositionChanged.connect(self._refreshSelections)
        self.sourceEdit.setMinimumHeight(110)
        layout.addWidget(self.sourceEdit, 2)

        self.highlighter = exprHighlight.ExpressionHighlighter(
            self.sourceEdit.document())
        self._brackets = exprHighlight.BracketMatcher(self.sourceEdit)

        self.statusLabel = QtWidgets.QLabel()
        self.statusLabel.setWordWrap(True)
        self.statusLabel.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        self.statusLabel.setFrameShape(QtWidgets.QFrame.StyledPanel)
        self.statusLabel.setMinimumHeight(24)
        layout.addWidget(self.statusLabel)

        buttons = QtWidgets.QHBoxLayout()
        self.applyButton = QtWidgets.QPushButton("Apply (Ctrl+Return)")
        self.applyButton.clicked.connect(self.apply)
        self.previewButton = QtWidgets.QPushButton("Preview")
        self.previewButton.setToolTip(
            "Write the text to the stage's SESSION layer so the viewport "
            "shows it, without touching the layer being authored. Apply "
            "commits it to the edit target; Revert drops it.")
        self.previewButton.clicked.connect(self.preview)
        self.revertButton = QtWidgets.QPushButton("Revert")
        self.revertButton.clicked.connect(self.revert)
        self.clearButton = QtWidgets.QPushButton("Clear")
        self.clearButton.setToolTip(
            "Empty the text. Nothing is written until Apply.")
        self.clearButton.clicked.connect(self.clear)
        for button in (self.applyButton, self.previewButton, self.revertButton,
                       self.clearButton):
            buttons.addWidget(button)
        buttons.addStretch(1)
        layout.addLayout(buttons)

        layout.addWidget(self._buildHairPreviewGroup())

        self.tabs = QtWidgets.QTabWidget()
        self.literalPanel = exprWidgets.LiteralPanel(
            self._onLiteralChanged, self._onLiteralDrag)
        self.controlPanel = exprControls.ControlPanel(
            self._onControlChanged, self._onControlRangeChanged,
            self._onLiteralDrag, self.addWidget, self.literalPanel)
        self.tabs.addTab(self.controlPanel, "Controls")

        self.functionBrowser = exprWidgets.FunctionBrowser(self.insertAtCursor)
        self.variableBrowser = exprWidgets.VariableBrowser(self.insertAtCursor)
        reference = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        reference.addWidget(self.functionBrowser)
        reference.addWidget(self.variableBrowser)
        reference.setStretchFactor(0, 2)
        reference.setStretchFactor(1, 3)
        self.tabs.addTab(reference, "Reference")

        self.libraryBrowser = exprWidgets.LibraryBrowser(
            self._onLibraryLoad, self._onLibrarySave)
        self.tabs.addTab(self.libraryBrowser, "Library")

        self.tabs.addTab(self._buildConnectionsTab(), "Connections")
        self.tabs.setMinimumHeight(260)
        layout.addWidget(self.tabs, 3)

        shortcut = QtGui.QShortcut(
            QtGui.QKeySequence("Ctrl+Return"), self.sourceEdit)
        shortcut.setContext(QtCore.Qt.WidgetWithChildrenShortcut)
        shortcut.activated.connect(self.apply)

        self._validateTimer = QtCore.QTimer(self)
        self._validateTimer.setSingleShot(True)
        self._validateTimer.timeout.connect(self.validateNow)

        self.setWidget(body)
        self._setEditable(False)

    def _buildHairPreviewGroup(self):
        group = QtWidgets.QGroupBox("Colour hair by value")
        group.setCheckable(True)
        group.setChecked(False)
        group.setToolTip(
            "Override the hair material and colour every strand by the chosen "
            "expression, Ptex map or operator attribute. Written to the "
            "session layer only (usdGen:preview:* on the description); "
            "unchecking restores the look.")
        group.toggled.connect(self._onHairPreviewToggled)
        grid = QtWidgets.QGridLayout(group)
        grid.setContentsMargins(6, 4, 6, 4)
        grid.setHorizontalSpacing(6)
        grid.setVerticalSpacing(3)

        self.hairSourceCombo = QtWidgets.QComboBox()
        self.hairSourceCombo.setSizeAdjustPolicy(
            QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.hairSourceCombo.setMinimumContentsLength(24)
        self.hairSourceCombo.setToolTip(
            "Expression: evaluated over the published strands.\n"
            "Ptex map: read at every strand root, as ptex() reads it.\n"
            "Attribute: the values its operator was cooked with, or its "
            "authored value when nothing drives it.")
        grid.addWidget(QtWidgets.QLabel("Source:"), 0, 0)
        grid.addWidget(self.hairSourceCombo, 0, 1, 1, 4)

        self.hairMapCombo = QtWidgets.QComboBox()
        for label, token in exprPreview.COLOR_MAPS:
            self.hairMapCombo.addItem(label, token)
        self.hairMapCombo.setToolTip(
            "Distinct ids gives every distinct value its own colour (clump "
            "and region maps) and ignores the range. RGB shows the first "
            "three components as a colour.")
        grid.addWidget(QtWidgets.QLabel("Colours:"), 1, 0)
        grid.addWidget(self.hairMapCombo, 1, 1)

        self.hairMinSpin = QtWidgets.QDoubleSpinBox()
        self.hairMaxSpin = QtWidgets.QDoubleSpinBox()
        for spin, value in ((self.hairMinSpin, exprPreview.DEFAULT_RANGE[0]),
                            (self.hairMaxSpin, exprPreview.DEFAULT_RANGE[1])):
            spin.setRange(-1e6, 1e6)
            spin.setDecimals(4)
            spin.setSingleStep(0.05)
            spin.setValue(value)
            spin.setKeyboardTracking(False)
            spin.setToolTip("The values mapped to the two ends of the colours.")
        grid.addWidget(QtWidgets.QLabel("Range:"), 1, 2)
        grid.addWidget(self.hairMinSpin, 1, 3)
        grid.addWidget(self.hairMaxSpin, 1, 4)

        self.hairEvaluationCombo = QtWidgets.QComboBox()
        for label, token in exprPreview.EVALUATIONS:
            self.hairEvaluationCombo.addItem(label, token)
        self.hairEvaluationCombo.setToolTip(
            "Where a previewed expression is evaluated. An attribute keeps "
            "its binding's domain and a map is read per strand.")
        grid.addWidget(QtWidgets.QLabel("Evaluate:"), 2, 0)
        grid.addWidget(self.hairEvaluationCombo, 2, 1)

        self.hairFlatCheck = QtWidgets.QCheckBox("Flat")
        self.hairFlatCheck.setToolTip(
            "Show the mapped colour exactly, without view shading, so a value "
            "can be read off the screen.")
        grid.addWidget(self.hairFlatCheck, 2, 2, 1, 2)
        self.hairFollowCheck = QtWidgets.QCheckBox("Follow edits")
        self.hairFollowCheck.setChecked(True)
        self.hairFollowCheck.setToolTip(
            "While colouring by this expression, preview every edit that "
            "compiles in the session layer, so the colours follow the text.")
        grid.addWidget(self.hairFollowCheck, 2, 4)

        self.hairStatusLabel = QtWidgets.QLabel()
        self.hairStatusLabel.setWordWrap(True)
        grid.addWidget(self.hairStatusLabel, 3, 0, 1, 5)
        grid.setColumnStretch(1, 1)

        self.hairSourceCombo.currentIndexChanged.connect(self._onHairSourceChanged)
        self.hairMapCombo.currentIndexChanged.connect(self._onHairPreviewChanged)
        self.hairEvaluationCombo.currentIndexChanged.connect(
            self._onHairPreviewChanged)
        self.hairMinSpin.valueChanged.connect(self._onHairPreviewChanged)
        self.hairMaxSpin.valueChanged.connect(self._onHairPreviewChanged)
        self.hairFlatCheck.toggled.connect(self._onHairPreviewChanged)
        self.hairPreviewGroup = group
        return group

    def _buildConnectionsTab(self):
        page = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)

        self.connectionLabel = QtWidgets.QLabel()
        self.connectionLabel.setWordWrap(True)
        layout.addWidget(self.connectionLabel)

        self.connectionTree = QtWidgets.QTreeWidget()
        self.connectionTree.setHeaderLabels(["Attribute", "Type", "Connected to"])
        self.connectionTree.setRootIsDecorated(False)
        self.connectionTree.setAlternatingRowColors(True)
        self.connectionTree.itemSelectionChanged.connect(
            self._onConnectionSelectionChanged)
        self.connectionTree.itemDoubleClicked.connect(
            lambda item, column: self._onConnectClicked())
        layout.addWidget(self.connectionTree, 1)

        row = QtWidgets.QHBoxLayout()
        self.connectButton = QtWidgets.QPushButton("Connect to expression...")
        self.connectButton.clicked.connect(self._onConnectClicked)
        self.disconnectButton = QtWidgets.QPushButton("Disconnect")
        self.disconnectButton.clicked.connect(self.disconnectSelectedAttribute)
        self.showOnHairButton = QtWidgets.QPushButton("Show on hair")
        self.showOnHairButton.setToolTip(
            "Colour the hair by the selected attribute: the values its "
            "operator was cooked with, or its authored value.")
        self.showOnHairButton.clicked.connect(
            lambda: self.previewAttributeOnHair())
        row.addWidget(self.connectButton)
        row.addWidget(self.disconnectButton)
        row.addWidget(self.showOnHairButton)
        row.addStretch(1)
        layout.addLayout(row)
        return page

    def showAndRaise(self):
        self.show()
        self.raise_()
        self.sourceEdit.setFocus()

    # ---- the engine's vocabulary ---------------------------------------

    def _loadReference(self):
        """Fill the browsers and the highlighter from the C ABI, once."""
        api = exprApi.GetApi()
        self._functions = api.functions()
        self._allVariables = api.variables(0)
        self.functionBrowser.setFunctions(self._functions)
        self.variableBrowser.setVariables(self._allVariables)
        self.highlighter.setVocabulary(
            [v.name for v in self._allVariables],
            [f.name for f in self._functions])
        # The completion popup offers exactly what the browsers list: one
        # vocabulary, from the engine, however the user reaches for it.
        self.sourceEdit.setVocabulary(self._functions, self._allVariables)
        if not api.available:
            self._setStatus(
                "Validation and the reference lists are unavailable: %s"
                % api.unavailableReason, "warning")

    def _refreshVariablesForDomain(self):
        api = exprApi.GetApi()
        if not api.available:
            return
        self.variableBrowser.setVariables(api.variables(self.domainCode()))

    def domainName(self):
        return self.domainCombo.currentData() or exprAuthor.DEFAULT_EVALUATION

    def domainCode(self):
        return exprApi.DOMAIN_CODES.get(self.domainName(), exprApi.DOMAIN_GROOM)

    def outputComponents(self):
        """How many components the expression must produce, from the type of
        the expression's own outputs:result."""
        if not self._prim:
            return 1
        result = self._prim.GetAttribute(RESULT_ATTR)
        if result and result.IsValid():
            return exprAuthor.OutputComponents(result.GetTypeName())
        for attr in self._prim.GetAttributes():
            if attr.GetName().startswith(OUTPUT_PREFIX):
                return exprAuthor.OutputComponents(attr.GetTypeName())
        return 1

    # ---- selection -----------------------------------------------------

    @property
    def prim(self):
        return self._prim

    @property
    def binding(self):
        """The attribute the edited expression drives, or None."""
        return self._binding

    def refreshFromSelection(self):
        # The selection model reads the focus prim off the stage, which is
        # None while usdview swaps stages.
        focus = None
        if self._api.stage:
            focus = self._api.dataModel.selection.getFocusPrim()
        operator = (focus if focus and focus.IsValid()
                    and focus.GetTypeName() != EXPRESSION_TYPE else None)
        if (self._pinnedHairSource is not None and
                (operator is None or self._pinnedHairSource.path.GetPrimPath()
                 != operator.GetPath())):
            self._pinnedHairSource = None
        self._operator = operator
        candidates = ExpressionCandidates(focus)
        current = self._prim.GetPath() if self._prim else None
        self._loading = True
        try:
            self.targetCombo.clear()
            for prim in candidates:
                self.targetCombo.addItem(prim.GetPath().pathString, prim.GetPath())
        finally:
            self._loading = False
        self._refreshConnections()
        if not candidates:
            self._setPrim(None)
            return
        index = 0
        for i, prim in enumerate(candidates):
            if prim.GetPath() == current:
                index = i
        self.targetCombo.setCurrentIndex(index)
        self._setPrim(candidates[index])

    def _onSelectionChanged(self, added, removed):
        self.refreshFromSelection()

    def _onTargetChanged(self, index):
        if self._loading or index < 0:
            return
        path = self.targetCombo.itemData(index)
        stage = self._api.stage
        self._setPrim(stage.GetPrimAtPath(path) if stage and path else None)

    def _setPrim(self, prim):
        self._prim = prim if prim and prim.IsValid() else None
        self._binding = self._findBinding()
        self._load()
        self._refreshHairSources()

    def _findBinding(self):
        """The destination attribute whose usdGen:evaluation customData picks
        the domain. The selected operator's own attribute when there is one,
        else the first attribute anywhere in the description bound to this
        expression."""
        if not self._prim:
            return None
        if self._operator:
            for attr in exprAuthor.ConnectableAttributes(self._operator):
                if exprAuthor.ExpressionTarget(attr) == self._prim:
                    return attr
        bindings = exprAuthor.BindingsOf(self._prim)
        return bindings[0] if bindings else None

    # ---- content -------------------------------------------------------

    def _load(self):
        self._loading = True
        try:
            if not self._prim:
                self._appliedSource = ""
                self.sourceEdit.setPlainText("")
                self.outputsLabel.setText("")
                self.outputTypeLabel.setText("")
                self._setEditable(False)
                self._setStatus("", "neutral")
                self.diagnostics = []
                self.controlPanel.setSource("")
                self._refreshSelections()
                self._markDirty(False)
                return
            attr = self._prim.GetAttribute(SOURCE_ATTR)
            source = attr.Get() if attr else None
            self._appliedSource = source or ""
            self.sourceEdit.setPlainText(self._appliedSource)
            outputs = []
            resultType = ""
            for a in self._prim.GetAttributes():
                name = a.GetName()
                if name.startswith(OUTPUT_PREFIX):
                    outputs.append("%s (%s)" % (name[len(OUTPUT_PREFIX):],
                                                a.GetTypeName()))
                    if name == RESULT_ATTR:
                        resultType = str(a.GetTypeName())
            self.outputsLabel.setText(
                "Outputs: " + (", ".join(outputs) if outputs else "none declared"))
            self.outputTypeLabel.setText(
                "result: %s" % (resultType or "undeclared"))
            # The binding row already shows outputs:result, so this line is
            # only worth its height when there is something else to say.
            self.outputsLabel.setVisible(
                len(outputs) != 1 or not resultType)
            self._setEditable(True)
            self.controlPanel.setSource(self.sourceEdit.toPlainText())
            self._loadDomainFromBinding()
        finally:
            self._loading = False
        self._markDirty(False)
        self._refreshSelections()
        self.validateNow()

    def _loadDomainFromBinding(self):
        name = exprAuthor.EvaluationOf(self._binding) if self._binding \
            else exprAuthor.DEFAULT_EVALUATION
        index = self.domainCombo.findData(name)
        if index >= 0:
            self.domainCombo.setCurrentIndex(index)
        self.domainCombo.setEnabled(self._binding is not None)
        self.domainCombo.setToolTip(
            "Evaluation domain of %s" % self._binding.GetPath()
            if self._binding else
            "No attribute is connected to this expression, so there is no "
            "destination whose usdGen:evaluation customData to write.")
        self._refreshVariablesForDomain()

    def _setEditable(self, editable):
        self.sourceEdit.setReadOnly(not editable)
        self.sourceEdit.setCompletionEnabled(editable)
        self.applyButton.setEnabled(False)
        self.revertButton.setEnabled(False)
        self.previewButton.setEnabled(editable)
        self.clearButton.setEnabled(editable)
        self.controlPanel.addButton.setEnabled(editable)

    def _markDirty(self, dirty):
        enabled = bool(self._prim) and dirty
        self.applyButton.setEnabled(enabled)
        # Revert also drops a preview, so it stays live while one is showing.
        self.revertButton.setEnabled(enabled or self._previewing)
        self.setWindowTitle(TITLE + (" *" if enabled else ""))

    def _onTextChanged(self):
        if self._loading or not self._prim:
            return
        self._markDirty(self.sourceEdit.toPlainText() != self._currentSource())
        if not self._rewriting:
            self.controlPanel.setSource(self.sourceEdit.toPlainText())
        self._validateTimer.start(VALIDATE_DELAY_MS)

    def _currentSource(self):
        """The text as last loaded from or committed to the stage.

        Remembered rather than re-read, because a Preview puts the editor's
        own text in the session layer: composing the attribute back would then
        report every unapplied edit as already applied."""
        return self._appliedSource

    def isDirty(self):
        return self.applyButton.isEnabled()

    def _writeSource(self, text):
        """Author `text` as usdGen:expr:source through the current edit
        target. False, with the reason in the status strip, when it will not
        take."""
        attr = self._prim.GetAttribute(SOURCE_ATTR)
        if not attr:
            attr = self._prim.CreateAttribute(
                SOURCE_ATTR, Sdf.ValueTypeNames.String, custom=True)
        if not attr.Set(text):
            self._setStatus("Could not author %s on %s" % (
                SOURCE_ATTR, self._prim.GetPath()), "error")
            return False
        # geoSampler("name")/ptex("name") read input:name; give the user the
        # relationship to target.
        exprAuthor.EnsureInputRelationships(self._prim, text)
        return True

    def apply(self):
        if not self._prim:
            return False
        text = self.sourceEdit.toPlainText()
        self._authoring = True
        try:
            if not self._writeSource(text):
                return False
        finally:
            self._authoring = False
        self._appliedSource = text
        # A preview lives in the session layer and would otherwise go on
        # shadowing what was just committed.
        self._dropPreview(committed=True)
        layer = self._prim.GetStage().GetEditTarget().GetLayer()
        self._markDirty(False)
        if not self.diagnostics:
            self._setStatus("Applied to %s" % layer.GetDisplayName(), "ok")
        return True

    def preview(self):
        """Show the text in the viewport without committing it.

        The override goes in the stage's session layer, which is the USD way
        to say "for this session only": the layer being authored is untouched,
        the scene index recooks from the composed result all the same, and
        Apply or Revert decides what happens to it."""
        if not self._prim:
            return False
        stage = self._prim.GetStage()
        session = stage.GetSessionLayer()
        if not session:
            self._setStatus("This stage has no session layer to preview in.",
                            "warning")
            return False
        if not self._previewing:
            spec = session.GetAttributeAtPath(
                self._prim.GetPath().AppendProperty(SOURCE_ATTR))
            self._previewBackup = (spec.default if spec is not None
                                   and spec.HasDefaultValue() else None)
        self._authoring = True
        try:
            with Usd.EditContext(stage, session):
                if not self._writeSource(self.sourceEdit.toPlainText()):
                    return False
        finally:
            self._authoring = False
        self._previewing = True
        self._markDirty(self.sourceEdit.toPlainText() != self._currentSource())
        self._setStatus(
            "Previewing in the session layer. Apply commits it to %s; Revert "
            "drops it." % stage.GetEditTarget().GetLayer().GetDisplayName(),
            "warning")
        return True

    def _dropPreview(self, committed=False):
        """Remove the session-layer override a preview left, if any.

        `committed` is Apply's call: the text was just written to the edit
        target, and when that target is the session layer itself the preview
        opinion has already been replaced by the committed one. Otherwise the
        session layer gets back what it held before the preview, or loses the
        opinion when it held none; after an Apply to another layer the session
        opinion always goes, or it would shadow the commit."""
        if not self._previewing:
            return
        self._previewing = False
        backup, self._previewBackup = self._previewBackup, None
        if not self._prim or not self._prim.IsValid():
            return
        stage = self._prim.GetStage()
        session = stage.GetSessionLayer()
        attr = self._prim.GetAttribute(SOURCE_ATTR)
        if not session or not attr:
            return
        if committed and stage.GetEditTarget().GetLayer() == session:
            return
        self._authoring = True
        try:
            with Usd.EditContext(stage, session):
                if backup is not None and not committed:
                    attr.Set(backup)
                else:
                    # Clear() drops what THIS edit target authored and nothing
                    # else, so the opinion in the layer being authored
                    # survives untouched.
                    attr.Clear()
        finally:
            self._authoring = False

    def isPreviewing(self):
        return self._previewing

    def revert(self):
        if not self._prim:
            return
        self._dropPreview()
        self._load()

    def clear(self):
        """Empty the text. Nothing reaches the stage until Apply."""
        if self.sourceEdit.isReadOnly():
            return
        self.sourceEdit.selectAll()
        self.sourceEdit.textCursor().removeSelectedText()
        self.sourceEdit.setPlainText("")

    def insertAtCursor(self, text):
        """Insert `text` at the cursor. A '|' in it marks where the cursor
        should end up, which is how the browsers put the caret inside a call's
        parentheses."""
        if self.sourceEdit.isReadOnly():
            return
        caret = text.find("|")
        body = text.replace("|", "")
        cursor = self.sourceEdit.textCursor()
        cursor.insertText(body)
        if caret >= 0:
            cursor.setPosition(cursor.position() - (len(body) - caret))
            self.sourceEdit.setTextCursor(cursor)
        self.sourceEdit.setFocus()

    # ---- validation ----------------------------------------------------

    def validateNow(self):
        """Compile the current text through the engine frontend and report."""
        self.diagnostics = []
        api = exprApi.GetApi()
        if not self._prim:
            self._setStatus("", "neutral")
            self._refreshSelections()
            return
        if not api.available:
            self._setStatus("No validation: %s" % api.unavailableReason,
                            "warning")
            self._refreshSelections()
            return
        text = self.sourceEdit.toPlainText()
        if not text.strip():
            self._setStatus("Empty expression.", "warning")
            self._refreshSelections()
            return
        self.diagnostics = api.compile(text, self.domainCode(),
                                       self.outputComponents())
        if not self.diagnostics:
            self._setStatus("OK - compiles for the %s domain, %d component(s)."
                            % (self.domainName(), self.outputComponents()), "ok")
            self._followEditOnHair()
        else:
            first = self.diagnostics[0]
            where = ("line %d, col %d: " % (first.line, first.column)
                     if first.line > 0 else "")
            extra = ("  (+%d more)" % (len(self.diagnostics) - 1)
                     if len(self.diagnostics) > 1 else "")
            self._setStatus(where + first.message + extra, "error")
        self._refreshSelections()

    def _setStatus(self, text, kind):
        colors = {"ok": "#5fbf60", "error": "#ff6b6b", "warning": "#e0b050",
                  "neutral": ""}
        self.statusLabel.setText(text)
        color = colors.get(kind, "")
        self.statusLabel.setStyleSheet(
            "color: %s;" % color if color else "")

    def _refreshSelections(self):
        """One extra-selection list, shared by bracket matching and the
        marker on the line an error is on."""
        selections = list(self._brackets.selections())
        if self.diagnostics:
            first = self.diagnostics[0]
            selections = exprHighlight.ErrorSelection(
                self.sourceEdit, first.line, first.column) + selections
        self.sourceEdit.setExtraSelections(selections)

    def _onDomainChanged(self, _index):
        if self._loading:
            return
        self._refreshVariablesForDomain()
        if self._binding:
            if not exprAuthor.SetEvaluation(self._binding, self.domainName()):
                self._setStatus(
                    "Could not author usdGen:evaluation on %s"
                    % self._binding.GetPath(), "error")
                return
        self.validateNow()

    # ---- literal controls ----------------------------------------------

    def _onLiteralDrag(self, dragging):
        self._dragging = dragging
        if not dragging:
            self._coalescing = False

    def setLiteralValue(self, index, value):
        """Rewrite literal `index` in the text to `value`. The slider calls
        this; a test can call it directly."""
        self._onLiteralChanged(index, value)

    def _rewriteSpan(self, start, end, replacement):
        """Replace exactly text[start:end] with `replacement`.

        Through a QTextCursor rather than by rebuilding the text: every other
        character is left alone, the caret is carried across the edit, and a
        drag coalesces into ONE undo step."""
        if self.sourceEdit.isReadOnly():
            return False
        visible = self.sourceEdit.textCursor()
        caret = visible.position()
        delta = len(replacement) - (end - start)

        self._rewriting = True
        try:
            cursor = QtGui.QTextCursor(self.sourceEdit.document())
            if self._coalescing:
                cursor.joinPreviousEditBlock()
            else:
                cursor.beginEditBlock()
                self._coalescing = self._dragging
            cursor.setPosition(start)
            cursor.setPosition(end, QtGui.QTextCursor.KeepAnchor)
            cursor.insertText(replacement)
            cursor.endEditBlock()
        finally:
            self._rewriting = False

        if caret >= end:
            caret += delta
        elif caret > start:
            caret = start + len(replacement)
        visible.setPosition(max(0, min(caret, len(self.sourceEdit.toPlainText()))))
        self.sourceEdit.setTextCursor(visible)
        return True

    def _afterRewrite(self):
        """Common tail of every edit a panel makes to the text."""
        text = self.sourceEdit.toPlainText()
        self.controlPanel.refreshOffsets(text)
        self._markDirty(text != self._currentSource())
        self._validateTimer.start(VALIDATE_DELAY_MS)
        if self.liveApplyCheck.isChecked():
            self.apply()

    def _onLiteralChanged(self, index, value):
        literals = self.literalPanel.literals
        if index >= len(literals):
            return
        literal = literals[index]
        replacement = exprApi.FormatLiteral(value, literal.text)
        if replacement == literal.text:
            return
        if self._rewriteSpan(literal.start, literal.end, replacement):
            self._afterRewrite()

    # ---- the controls panel ---------------------------------------------

    def setControlValue(self, index, value):
        """Drive control `index` from outside the widget, as a test does."""
        self._onControlChanged(index, value)

    def _onControlChanged(self, index, value):
        controls = self.controlPanel.controls
        if index >= len(controls):
            return
        control = controls[index]
        replacement = exprApi.FormatControlValue(control, value)
        if replacement == control.valueText:
            return
        if self._rewriteSpan(control.valueStart, control.valueEnd, replacement):
            self._afterRewrite()

    def _onControlRangeChanged(self, index, low, high):
        """A number control's range lives in its `# min, max` comment, so
        widening the slider is an edit to the expression text like any other.

        When the statement carries no comment yet, one is appended at the end
        of its line rather than beside the value: a comment runs to the end of
        the line, so anywhere else would swallow the rest of the statement."""
        controls = self.controlPanel.controls
        if index >= len(controls):
            return
        control = controls[index]
        isInt = control.kind == "int"
        annotation = "# %s, %s" % (exprApi.FormatNumber(low, isInt),
                                   exprApi.FormatNumber(high, isInt))
        if control.commentStart >= 0:
            start, end = control.commentStart, control.commentEnd
        else:
            text = self.sourceEdit.toPlainText()
            stop = text.find("\n", control.valueEnd)
            start = end = len(text) if stop < 0 else stop
            annotation = " " + annotation
        if self._rewriteSpan(start, end, annotation):
            self._afterRewrite()

    def addWidget(self):
        """SeExpr's Add Widget: declare a new $variable and give it a control.

        Returns the inserted declaration, or None when the dialog was
        cancelled."""
        if self.sourceEdit.isReadOnly():
            return None
        taken = [c.name for c in self.controlPanel.controls]
        taken += [v.name for v in getattr(self, "_allVariables", [])]
        dialog = exprControls.AddWidgetDialog(self, existingNames=taken)
        if dialog.exec() != QtWidgets.QDialog.Accepted:
            return None
        return self.insertDeclaration(dialog.declaration())

    def insertDeclaration(self, line):
        """Put `line` at the very top of the expression, on its own line.

        The top, because a SeExpr variable has to be assigned before it is
        read and the body that reads it is what is already there."""
        if self.sourceEdit.isReadOnly():
            return None
        if not self._rewriteSpan(0, 0, line + "\n"):
            return None
        self.controlPanel.setSource(self.sourceEdit.toPlainText())
        self._markDirty(self.sourceEdit.toPlainText() != self._currentSource())
        self._validateTimer.start(VALIDATE_DELAY_MS)
        return line

    # ---- the expression library -----------------------------------------

    def _onLibraryLoad(self, entry):
        text = exprLibrary.Load(entry.path)
        if not text:
            self._setStatus("Could not read %s" % entry.path, "error")
            return
        if self.sourceEdit.isReadOnly():
            self._setStatus(
                "Select an expression prim before loading %s." % entry.name,
                "warning")
            return
        self._rewriteSpan(0, len(self.sourceEdit.toPlainText()), text.rstrip("\n"))
        self.controlPanel.setSource(self.sourceEdit.toPlainText())
        domain = exprLibrary.DomainOf(text)
        note = ""
        if domain and domain != self.domainName():
            note = ("  It was written for the %s domain; this binding "
                    "evaluates per %s." % (domain, self.domainName()))
        self._markDirty(self.sourceEdit.toPlainText() != self._currentSource())
        self._setStatus("Loaded %s. Apply to write it to the stage.%s"
                        % (entry.name, note), "warning")
        self.validateNow()

    def loadFromLibrary(self, name):
        """Load the library entry called `name`. True when there was one."""
        if not self.libraryBrowser.selectEntry(name):
            return False
        return self.libraryBrowser.loadSelected()

    def _onLibrarySave(self, askForName):
        name = self.libraryBrowser.selectedName()
        if askForName or not name:
            name, accepted = QtWidgets.QInputDialog.getText(
                self, "Save expression", "Name:",
                QtWidgets.QLineEdit.Normal,
                name or (self._prim.GetName() if self._prim else "expression"))
            if not accepted or not name.strip():
                return None
        return self.saveToLibrary(name)

    def saveToLibrary(self, name):
        """Write the current text to the user library as `name`.se.

        The evaluation domain goes in with it, so the browser can filter on it
        the next time round. Returns the path, or None when the write failed."""
        try:
            path = exprLibrary.Save(name, self.sourceEdit.toPlainText(),
                                    domain=self.domainName())
        except (IOError, OSError) as exc:
            self._setStatus("Could not save %s: %s" % (name, exc), "error")
            return None
        self.libraryBrowser.refresh()
        self.libraryBrowser.selectEntry(exprLibrary.SanitiseName(name))
        self._setStatus("Saved %s" % path, "ok")
        return path

    # ---- connections ---------------------------------------------------

    def _refreshConnections(self):
        self.connectionTree.clear()
        prim = self._operator
        if not prim:
            self.connectionLabel.setText(
                "Select an operator prim to connect its attributes to "
                "expressions.")
            self.connectButton.setEnabled(False)
            self.disconnectButton.setEnabled(False)
            self.showOnHairButton.setEnabled(False)
            return
        self.connectionLabel.setText(prim.GetPath().pathString)
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)
        for attr in exprAuthor.ConnectableAttributes(prim):
            target = exprAuthor.ExpressionTarget(attr)
            item = QtWidgets.QTreeWidgetItem(
                [attr.GetName(), str(attr.GetTypeName()),
                 target.GetPath().pathString if target else ""])
            item.setFont(0, font)
            item.setFont(2, font)
            item.setToolTip(2, "Evaluated per %s" % exprAuthor.EvaluationOf(attr))
            item.setData(0, QtCore.Qt.UserRole, attr.GetName())
            self.connectionTree.addTopLevelItem(item)
        self.connectionTree.resizeColumnToContents(0)
        self.connectionTree.resizeColumnToContents(1)
        if self.connectionTree.topLevelItemCount():
            self.connectionTree.setCurrentItem(self.connectionTree.topLevelItem(0))
        self._onConnectionSelectionChanged()

    def selectedAttribute(self):
        """The operator attribute the Connections tab has selected."""
        items = self.connectionTree.selectedItems()
        if not items or not self._operator:
            return None
        return self._operator.GetAttribute(items[0].data(0, QtCore.Qt.UserRole))

    def selectConnectionRow(self, attributeName):
        """Select the row for `attributeName`. True when it was there."""
        for index in range(self.connectionTree.topLevelItemCount()):
            item = self.connectionTree.topLevelItem(index)
            if item.data(0, QtCore.Qt.UserRole) == attributeName:
                self.connectionTree.setCurrentItem(item)
                return True
        return False

    def _onConnectionSelectionChanged(self):
        attr = self.selectedAttribute()
        self.connectButton.setEnabled(attr is not None)
        self.disconnectButton.setEnabled(
            attr is not None and exprAuthor.ExpressionTarget(attr) is not None)
        self.showOnHairButton.setEnabled(
            attr is not None and exprAuthor.DescriptionOf(attr.GetPrim()) is not None)

    def _onConnectClicked(self):
        attr = self.selectedAttribute()
        if attr is None:
            return
        description = exprAuthor.DescriptionOf(attr.GetPrim())
        existing = exprAuthor.ExistingExpressions(description, attr.GetTypeName())
        dialog = _ConnectDialog(self, attr, existing)
        if dialog.exec() != QtWidgets.QDialog.Accepted:
            return
        self.connectSelectedAttribute(name=dialog.name(),
                                      expressionPath=dialog.expressionPath())

    def connectSelectedAttribute(self, name=None, expressionPath=None,
                                 source=exprAuthor.DEFAULT_SOURCE):
        """Connect the selected attribute to an expression and edit it.

        Returns the expression prim, or None when nothing was selected or the
        attribute is not under a UsdGenDescription."""
        attr = self.selectedAttribute()
        if attr is None:
            return None
        try:
            expression = exprAuthor.ConnectToExpression(
                attr, name=name, expressionPath=expressionPath, source=source)
        except ValueError as exc:
            self._setStatus(str(exc), "error")
            return None
        self.refreshFromSelection()
        self.selectConnectionRow(attr.GetName())
        index = self.targetCombo.findData(expression.GetPath())
        if index >= 0:
            self.targetCombo.setCurrentIndex(index)
        self._setStatus("%s now drives %s (per %s)."
                        % (expression.GetPath(), attr.GetName(),
                           exprAuthor.EvaluationOf(attr)), "ok")
        return expression

    def disconnectSelectedAttribute(self):
        """Remove the selected attribute's expression connection."""
        attr = self.selectedAttribute()
        if attr is None:
            return False
        removed = exprAuthor.DisconnectExpression(attr)
        self.refreshFromSelection()
        self.selectConnectionRow(attr.GetName())
        self._setStatus(
            "%s is no longer connected." % attr.GetName() if removed else
            "Could not disconnect %s." % attr.GetName(),
            "ok" if removed else "error")
        return removed

    # ---- hair colour preview -------------------------------------------

    def hairPreviewEnabled(self):
        return self.hairPreviewGroup.isChecked()

    def setHairPreview(self, enabled):
        """Turn the hair colour preview on or off, as the group's checkbox
        does. True when the stage agrees afterwards."""
        self.hairPreviewGroup.setChecked(bool(enabled))
        description = self._hairDescription()
        previewing = bool(description) and exprPreview.IsPreviewing(description)
        return previewing == bool(enabled)

    def hairSource(self):
        """The PreviewSource the combo has selected, or None."""
        index = self.hairSourceCombo.currentIndex()
        if 0 <= index < len(self._hairSources):
            return self._hairSources[index]
        return None

    def selectHairSource(self, kind, path=None):
        """Select the first listed source of `kind` (and `path`, when given).
        True when there was one."""
        for index, source in enumerate(self._hairSources):
            if source.kind == kind and (path is None or
                                        source.path == Sdf.Path(str(path))):
                self.hairSourceCombo.setCurrentIndex(index)
                return True
        return False

    def setHairColors(self, colorMap=None, valueRange=None, evaluation=None,
                      flat=None):
        """Set the preview controls; the preview follows when it is on."""
        self._loadingHair = True
        try:
            if colorMap is not None:
                self.hairMapCombo.setCurrentIndex(
                    max(0, self.hairMapCombo.findData(colorMap)))
            if valueRange is not None:
                self.hairMinSpin.setValue(valueRange[0])
                self.hairMaxSpin.setValue(valueRange[1])
            if evaluation is not None:
                self.hairEvaluationCombo.setCurrentIndex(
                    max(0, self.hairEvaluationCombo.findData(evaluation)))
            if flat is not None:
                self.hairFlatCheck.setChecked(bool(flat))
        finally:
            self._loadingHair = False
        self._onHairPreviewChanged()

    def previewAttributeOnHair(self, attr=None):
        """Colour the hair by `attr`, by default the Connections tab's
        selection. True when the preview took."""
        attr = attr if attr is not None else self.selectedAttribute()
        source = exprPreview.AttributeSource(attr) if attr is not None else None
        if source is None:
            return False
        self._pinnedHairSource = source
        self._refreshHairSources(prefer=source)
        self._setHairChecked(True)
        return self._applyHairPreview()

    def _setHairChecked(self, checked):
        """Check or uncheck the group without authoring anything."""
        self._loadingHair = True
        try:
            self.hairPreviewGroup.setChecked(bool(checked))
        finally:
            self._loadingHair = False

    def _hairDescription(self, source=None):
        """The description prim a preview of `source` is authored on."""
        source = source if source is not None else self.hairSource()
        stage = self._api.stage
        if source is None or source.description is None or not stage:
            return None
        prim = stage.GetPrimAtPath(source.description)
        return prim if prim and prim.IsValid() else None

    def _refreshHairSources(self, prefer=None):
        """List what the hair can be coloured by for the current expression
        and operator.

        While this editor previews, the preview follows the editor: the same
        kind of source is kept for the new target and re-authored, and a
        selection with nothing to offer leaves the colouring as it is.
        Otherwise the controls show whatever the stage already previews, so
        reopening the editor finds the colouring it left."""
        previous = self.hairSource()
        owning = self.hairPreviewEnabled()
        sources = exprPreview.SourcesFor(self._prim, self._binding)
        if (self._pinnedHairSource is not None and
                self._pinnedHairSource not in sources):
            sources.append(self._pinnedHairSource)
        keep = owning and not sources and previous is not None
        if keep:
            sources = [previous]

        # A newly edited expression is previewed where its binding
        # evaluates it.
        expressionPath = self._prim.GetPath() if self._prim else None
        if expressionPath != self._hairExpression:
            self._hairExpression = expressionPath
            if self._binding is not None:
                self._setHairControl(
                    self.hairEvaluationCombo,
                    exprPreview.DefaultEvaluation(self._binding))

        state = None
        if not owning and sources:
            description = self._hairDescription(sources[0])
            state = exprPreview.PreviewState(description)
            if state["source"] is None:
                state = None
            else:
                state["description"] = description
                authored = next((s for s in sources
                                 if s.path == state["source"]), None)
                if authored is None:
                    authored = exprPreview.SourceForPath(
                        description.GetStage(), state["source"],
                        description.GetPath())
                    sources.append(authored)
                if prefer is None:
                    prefer = authored

        if prefer is None and previous is not None:
            if previous in sources:
                prefer = previous
            else:
                prefer = next((s for s in sources if s.kind == previous.kind),
                              None)

        self._loadingHair = True
        try:
            self._hairSources = sources
            self.hairSourceCombo.clear()
            for source in sources:
                self.hairSourceCombo.addItem(source.label, str(source.path))
            if sources:
                index = sources.index(prefer) if prefer in sources else 0
                self.hairSourceCombo.setCurrentIndex(index)
        finally:
            self._loadingHair = False
        self._syncHairControls()

        if state is not None:
            self._previewDescription = state["description"].GetPath()
            self._setHairControl(self.hairMapCombo, state["colorMap"])
            self._setHairControl(self.hairEvaluationCombo, state["evaluation"])
            self._loadingHair = True
            try:
                self.hairMinSpin.setValue(state["range"][0])
                self.hairMaxSpin.setValue(state["range"][1])
                self.hairFlatCheck.setChecked(state["shading"] == "flat")
            finally:
                self._loadingHair = False
            self._setHairChecked(True)
            self._setHairStatus("%s is coloured by %s." % (
                self._previewDescription, state["source"]), "ok")
        elif owning and not keep:
            self._applyHairPreview()

    def _setHairControl(self, combo, token):
        """Select `token` in `combo` without authoring anything."""
        index = combo.findData(token)
        if index < 0:
            return
        self._loadingHair = True
        try:
            combo.setCurrentIndex(index)
        finally:
            self._loadingHair = False

    def _syncHairControls(self):
        source = self.hairSource()
        self.hairEvaluationCombo.setEnabled(
            source is not None and source.kind == exprPreview.EXPRESSION)
        self.hairFollowCheck.setEnabled(
            source is not None and source.kind == exprPreview.EXPRESSION)

    def _onHairPreviewToggled(self, enabled):
        if self._loadingHair:
            return
        if enabled:
            self._applyHairPreview()
        else:
            self._clearHairPreview()

    def _onHairSourceChanged(self, _index):
        if self._loadingHair:
            return
        self._syncHairControls()
        if self.hairPreviewEnabled():
            self._applyHairPreview()

    def _onHairPreviewChanged(self, *_args):
        if self._loadingHair:
            return
        if self.hairPreviewEnabled():
            self._applyHairPreview()

    def _setHairStatus(self, text, kind):
        colors = {"ok": "", "error": "#ff6b6b", "warning": "#e0b050",
                  "neutral": ""}
        self.hairStatusLabel.setText(text)
        color = colors.get(kind, "")
        self.hairStatusLabel.setStyleSheet("color: %s;" % color if color else "")

    def _applyHairPreview(self):
        """Author the controls' preview. True when the stage took it."""
        source = self.hairSource()
        description = self._hairDescription(source)
        if source is None or description is None:
            self._clearHairPreview()
            self._setHairStatus(
                "Nothing to colour by: select an expression, or an operator "
                "under a UsdGenDescription.", "warning")
            return False
        if (self._previewDescription is not None and
                self._previewDescription != description.GetPath()):
            self._clearHairPreview()
        shading = "flat" if self.hairFlatCheck.isChecked() else "lit"
        self._authoring = True
        try:
            ok = exprPreview.SetPreview(
                description, source.path,
                colorMap=self.hairMapCombo.currentData(),
                valueRange=(self.hairMinSpin.value(), self.hairMaxSpin.value()),
                evaluation=self.hairEvaluationCombo.currentData(),
                shading=shading)
        finally:
            self._authoring = False
        if not ok:
            self._setHairStatus("Could not author usdGen:preview on %s."
                                % description.GetPath(), "error")
            return False
        self._previewDescription = description.GetPath()
        self._setHairStatus("%s is coloured by %s (session layer)."
                            % (description.GetPath(), source.label), "ok")
        return True

    def _clearHairPreview(self):
        path = self._previewDescription
        self._previewDescription = None
        stage = self._api.stage
        if path is None or not stage:
            self._setHairStatus("", "neutral")
            return True
        self._authoring = True
        try:
            ok = exprPreview.ClearPreview(stage.GetPrimAtPath(path))
        finally:
            self._authoring = False
        self._setHairStatus(
            "" if ok else "Could not clear usdGen:preview on %s." % path,
            "neutral" if ok else "error")
        return ok

    def _followEditOnHair(self):
        """Preview a compiling edit when the hair shows this expression."""
        source = self.hairSource()
        if (self._loading or not self._prim or not self.hairPreviewEnabled()
                or not self.hairFollowCheck.isChecked() or source is None
                or source.kind != exprPreview.EXPRESSION
                or source.path != self._prim.GetPath() or not self.isDirty()):
            return
        self.preview()

    def _syncHairFromStage(self, notice):
        """Someone else cleared the preview this editor authored: show it."""
        path = self._previewDescription
        stage = self._api.stage
        if path is None or not stage or not self.hairPreviewEnabled():
            return
        description = stage.GetPrimAtPath(path)
        if description and exprPreview.IsPreviewing(description):
            return
        self._previewDescription = None
        self._setHairChecked(False)
        self._setHairStatus("The preview was cleared on the stage.", "neutral")

    # ---- stage notices -------------------------------------------------

    def _listenToStage(self):
        if self._noticeKey is not None:
            self._noticeKey.Revoke()
            self._noticeKey = None
        stage = self._api.stage
        if stage:
            self._noticeKey = Tf.Notice.Register(
                Usd.Notice.ObjectsChanged, self._onObjectsChanged, stage)

    def _onStageReplaced(self):
        self._listenToStage()
        # The old stage's session layer went with it.
        self._previewDescription = None
        self._pinnedHairSource = None
        self._setHairChecked(False)
        self._setPrim(None)
        self.refreshFromSelection()

    def _onObjectsChanged(self, notice, sender):
        # Our own Apply and Preview come back through here; treating them as
        # someone else's edit would reload the text out from under the user.
        if self._authoring:
            return
        self._syncHairFromStage(notice)
        if not self._prim or not self._prim.IsValid():
            if self._prim is not None:
                self.refreshFromSelection()
            return
        attrPath = self._prim.GetPath().AppendProperty(SOURCE_ATTR)
        if (notice.AffectedObject(self._prim.GetAttribute(SOURCE_ATTR))
                if self._prim.GetAttribute(SOURCE_ATTR) else False):
            # An external edit (another tool, undo, a layer reload): show it
            # unless the user has unapplied changes of their own.
            if not self.isDirty():
                self._load()
            else:
                self._setStatus(
                    "%s changed on the stage; Revert to load it." % attrPath,
                    "warning")


class _ConnectDialog(QtWidgets.QDialog):
    """Ask whether to make a new expression or reuse one already under the
    description's Expressions scope."""

    NEW = "<new expression>"

    def __init__(self, parent, attr, existing):
        super(_ConnectDialog, self).__init__(parent)
        self.setWindowTitle("Connect %s" % attr.GetName())
        self._existing = list(existing)

        layout = QtWidgets.QFormLayout(self)
        self.sourceCombo = QtWidgets.QComboBox()
        self.sourceCombo.addItem(self.NEW, None)
        for prim in self._existing:
            self.sourceCombo.addItem(prim.GetName(), prim.GetPath())
        self.sourceCombo.currentIndexChanged.connect(self._onSourceChanged)
        layout.addRow("Expression:", self.sourceCombo)

        self.nameEdit = QtWidgets.QLineEdit(
            exprAuthor.SuggestExpressionName(attr))
        layout.addRow("New name:", self.nameEdit)

        note = QtWidgets.QLabel(
            "A new expression starts as %s, which passes the attribute's own "
            "value through unchanged, and is evaluated per %s."
            % (exprAuthor.DEFAULT_SOURCE, exprAuthor.EvaluationOf(attr)))
        note.setWordWrap(True)
        layout.addRow(note)

        buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addRow(buttons)

    def _onSourceChanged(self, index):
        self.nameEdit.setEnabled(index == 0)

    def expressionPath(self):
        return self.sourceCombo.currentData()

    def name(self):
        return self.nameEdit.text().strip() or None
