# SeExpr expression editor dock for usdview.
#
# Edits the usdGen:expr:source of a UsdGenExpression prim in place. The dock
# follows usdview's prim selection: a selected UsdGenExpression prim is
# edited directly, and a selected operator offers every expression its
# attributes are connected to (the outputs:* connections the graph builders
# read), so the editor works from either end of a connection.
#
# Writes go through the ordinary UsdAttribute.Set on the stage's current edit
# target, so they are undoable in the same way as any other usdview edit and
# the groom scene index recooks from the resulting stage notice.

from pxr import Sdf, Tf, Usd
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

EXPRESSION_TYPE = "UsdGenExpression"
SOURCE_ATTR = "usdGen:expr:source"
OUTPUT_PREFIX = "outputs:"

# libs/usdGen/usdGen/expressions/context.cpp kVars, as a reference pane.
VARIABLES = [
    ("$value", "the consumer's literal, in the consumer's own type"),
    ("$t", "root->tip parameter, 0 at the root and 1 at the tip"),
    ("$time", "stage time in seconds"),
    ("$frame", "stage time code"),
    ("$index / $count", "element index and count in the evaluation domain"),
    ("$primIndex / $primCount", "strand index and strand count"),
    ("$pointIndex / $pointCount", "CV index and CV count (point domain)"),
    ("$id", "stable strand id; $idLo / $idHi are its 32-bit halves"),
    ("$seed", "the operator's usdGen:seed"),
    ("$u / $v", "root surface parameters; $faceId / $patchId the root face"),
    ("$P / $Pref", "current and rest position (float3)"),
    ("$rootP / $rootPref", "current and rest root position"),
    ("$N / $Nref", "current and rest root normal"),
    ("$dPdu / $dPdv", "surface tangents at the root ($dPduref / $dPdvref at rest)"),
    ("$cLength / $cWidth", "strand length and width"),
    ("$Cs / $As", "surface colour and alpha at the root"),
    ("$descId", "description id"),
]

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
    order the connections are authored."""
    if not prim or not prim.IsValid():
        return []
    if prim.GetTypeName() == EXPRESSION_TYPE:
        return [prim]
    stage = prim.GetStage()
    found = []
    for attr in prim.GetAttributes():
        for target in attr.GetConnections():
            if not target.name.startswith(OUTPUT_PREFIX):
                continue
            source = stage.GetPrimAtPath(target.GetPrimPath())
            if (source and source.GetTypeName() == EXPRESSION_TYPE
                    and source not in found):
                found.append(source)
    return found


class ExpressionEditorDock(QtWidgets.QDockWidget):

    def __init__(self, usdviewApi):
        super(ExpressionEditorDock, self).__init__(
            "SeExpr Expression Editor", usdviewApi.qMainWindow)
        self.setObjectName("usdGenExpressionEditor")
        self._api = usdviewApi
        self._prim = None
        self._noticeKey = None
        self._loading = False
        self._buildUi()
        self._api.qMainWindow.addDockWidget(
            QtCore.Qt.RightDockWidgetArea, self)

        selection = self._api.dataModel.selection
        selection.signalPrimSelectionChanged.connect(self._onSelectionChanged)
        if hasattr(self._api.dataModel, "signalStageReplaced"):
            self._api.dataModel.signalStageReplaced.connect(self._onStageReplaced)
        self._listenToStage()
        self.refreshFromSelection()

    # ---- UI ------------------------------------------------------------

    def _buildUi(self):
        body = QtWidgets.QWidget(self)
        layout = QtWidgets.QVBoxLayout(body)

        row = QtWidgets.QHBoxLayout()
        row.addWidget(QtWidgets.QLabel("Expression:"))
        self.targetCombo = QtWidgets.QComboBox()
        self.targetCombo.setSizeAdjustPolicy(
            QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.targetCombo.setMinimumContentsLength(24)
        self.targetCombo.currentIndexChanged.connect(self._onTargetChanged)
        row.addWidget(self.targetCombo, 1)
        layout.addLayout(row)

        self.outputsLabel = QtWidgets.QLabel()
        self.outputsLabel.setWordWrap(True)
        layout.addWidget(self.outputsLabel)

        self.sourceEdit = QtWidgets.QPlainTextEdit()
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)
        self.sourceEdit.setFont(font)
        self.sourceEdit.setPlaceholderText(
            "Select a UsdGenExpression prim, or an operator whose "
            "attributes are connected to one.")
        self.sourceEdit.textChanged.connect(self._onTextChanged)
        layout.addWidget(self.sourceEdit, 1)

        buttons = QtWidgets.QHBoxLayout()
        self.applyButton = QtWidgets.QPushButton("Apply (Ctrl+Return)")
        self.applyButton.clicked.connect(self.apply)
        self.revertButton = QtWidgets.QPushButton("Revert")
        self.revertButton.clicked.connect(self.revert)
        buttons.addWidget(self.applyButton)
        buttons.addWidget(self.revertButton)
        buttons.addStretch(1)
        layout.addLayout(buttons)

        self.statusLabel = QtWidgets.QLabel()
        self.statusLabel.setWordWrap(True)
        layout.addWidget(self.statusLabel)

        reference = QtWidgets.QTreeWidget()
        reference.setHeaderLabels(["Variable", "Meaning"])
        reference.setRootIsDecorated(False)
        for name, meaning in VARIABLES:
            item = QtWidgets.QTreeWidgetItem([name, meaning])
            item.setFont(0, font)
            reference.addTopLevelItem(item)
        reference.resizeColumnToContents(0)
        reference.setMaximumHeight(170)
        layout.addWidget(QtWidgets.QLabel("Variables:"))
        layout.addWidget(reference)

        shortcut = QtGui.QShortcut(
            QtGui.QKeySequence("Ctrl+Return"), self.sourceEdit)
        shortcut.setContext(QtCore.Qt.WidgetWithChildrenShortcut)
        shortcut.activated.connect(self.apply)

        self.setWidget(body)
        self._setEditable(False)

    def showAndRaise(self):
        self.show()
        self.raise_()
        self.sourceEdit.setFocus()

    # ---- selection -----------------------------------------------------

    @property
    def prim(self):
        return self._prim

    def refreshFromSelection(self):
        # The selection model reads the focus prim off the stage, which is
        # None while usdview swaps stages.
        focus = None
        if self._api.stage:
            focus = self._api.dataModel.selection.getFocusPrim()
        candidates = ExpressionCandidates(focus)
        current = self._prim.GetPath() if self._prim else None
        self._loading = True
        try:
            self.targetCombo.clear()
            for prim in candidates:
                self.targetCombo.addItem(prim.GetPath().pathString, prim.GetPath())
        finally:
            self._loading = False
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
        self._load()

    # ---- content -------------------------------------------------------

    def _load(self):
        self._loading = True
        try:
            if not self._prim:
                self.sourceEdit.setPlainText("")
                self.outputsLabel.setText("")
                self._setEditable(False)
                self.statusLabel.setText("")
                return
            attr = self._prim.GetAttribute(SOURCE_ATTR)
            source = attr.Get() if attr else None
            self.sourceEdit.setPlainText(source or "")
            outputs = []
            for a in self._prim.GetAttributes():
                name = a.GetName()
                if name.startswith(OUTPUT_PREFIX):
                    outputs.append("%s (%s)" % (name[len(OUTPUT_PREFIX):],
                                                a.GetTypeName()))
            self.outputsLabel.setText(
                "Outputs: " + (", ".join(outputs) if outputs else "none declared"))
            self._setEditable(True)
            self.statusLabel.setText("")
        finally:
            self._loading = False
        self._markDirty(False)

    def _setEditable(self, editable):
        self.sourceEdit.setReadOnly(not editable)
        self.applyButton.setEnabled(False)
        self.revertButton.setEnabled(False)

    def _markDirty(self, dirty):
        enabled = bool(self._prim) and dirty
        self.applyButton.setEnabled(enabled)
        self.revertButton.setEnabled(enabled)

    def _onTextChanged(self):
        if self._loading or not self._prim:
            return
        self._markDirty(self.sourceEdit.toPlainText() != self._currentSource())

    def _currentSource(self):
        attr = self._prim.GetAttribute(SOURCE_ATTR) if self._prim else None
        value = attr.Get() if attr else None
        return value or ""

    def isDirty(self):
        return self.applyButton.isEnabled()

    def apply(self):
        if not self._prim:
            return False
        text = self.sourceEdit.toPlainText()
        attr = self._prim.GetAttribute(SOURCE_ATTR)
        if not attr:
            attr = self._prim.CreateAttribute(
                SOURCE_ATTR, Sdf.ValueTypeNames.String, custom=True)
        if not attr.Set(text):
            self.statusLabel.setText("Could not author %s on %s" % (
                SOURCE_ATTR, self._prim.GetPath()))
            return False
        layer = self._prim.GetStage().GetEditTarget().GetLayer()
        self.statusLabel.setText("Applied to %s" % layer.GetDisplayName())
        self._markDirty(False)
        return True

    def revert(self):
        if not self._prim:
            return
        self._load()

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
        self._setPrim(None)
        self.refreshFromSelection()

    def _onObjectsChanged(self, notice, sender):
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
                self.statusLabel.setText(
                    "%s changed on the stage; Revert to load it." % attrPath)
