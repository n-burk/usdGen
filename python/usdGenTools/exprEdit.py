# The text pane itself: a QPlainTextEdit with a line-number gutter and the
# completion popup SeExpr's editor pops while you type.
#
# The vocabulary is whatever the C ABI reported -- functions with their
# signatures and docs, variables with their types and docs -- so the popup can
# never offer a name the engine does not know.

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from . import exprApi

# How many characters must be typed before the popup appears. One is enough
# after a '$', because the sigil already says what is being named; a bare
# identifier needs two, or every '(' would drag a popup along with it.
MINIMUM_VARIABLE_PREFIX = 1
MINIMUM_NAME_PREFIX = 2


class CompletionModel(QtGui.QStandardItemModel):
    """SeExpr's ExprCompletionModel: the names in scope, each with the
    signature or type it completes to and a line of documentation.

    Three columns, because the popup is a tree view: the name the completer
    matches on, the signature, and the doc."""

    NAME, SIGNATURE, DOC = range(3)

    def __init__(self, parent=None):
        super(CompletionModel, self).__init__(0, 3, parent)
        self.functions = []
        self.variables = []

    def setVocabulary(self, functions, variables):
        self.functions = list(functions)
        self.variables = list(variables)
        self.clear()
        self.setColumnCount(3)
        for variable in self.variables:
            self._append(variable.name, variable.scalarType, variable.doc,
                         variable.name)
        for function in self.functions:
            self._append(function.name, function.signature, function.doc,
                         function.insert)

    def _append(self, name, signature, doc, insert):
        row = [QtGui.QStandardItem(name), QtGui.QStandardItem(signature),
               QtGui.QStandardItem(doc)]
        for item in row:
            item.setEditable(False)
            item.setToolTip("%s\n%s" % (signature, doc))
            item.setData(insert, QtCore.Qt.UserRole + 1)
        self.appendRow(row)

    def names(self):
        return [self.item(row, self.NAME).text()
                for row in range(self.rowCount())]

    def insertTextFor(self, name):
        """The text to put in the document for `name`; a '|' in it marks where
        the caret lands, exactly as the browsers' insert text does."""
        for row in range(self.rowCount()):
            item = self.item(row, self.NAME)
            if item.text() == name:
                return item.data(QtCore.Qt.UserRole + 1) or name
        return name

    def matches(self, prefix):
        """Every name starting with `prefix`, case-insensitively.

        A '$' prefix asks for variables and a bare prefix for functions: the
        two namespaces do not overlap in this language, and mixing them would
        offer `$t` where only a function can go."""
        if not prefix:
            return []
        wantVariables = prefix.startswith("$")
        lowered = prefix.lower()
        found = []
        for row in range(self.rowCount()):
            name = self.item(row, self.NAME).text()
            if name.startswith("$") != wantVariables:
                continue
            if name.lower().startswith(lowered):
                found.append(name)
        return found


class _LineNumberArea(QtWidgets.QWidget):
    """The gutter. It has no state: it asks the editor to paint it."""

    def __init__(self, editor):
        super(_LineNumberArea, self).__init__(editor)
        self._editor = editor

    def sizeHint(self):
        return QtCore.QSize(self._editor.lineNumberAreaWidth(), 0)

    def paintEvent(self, event):
        self._editor.paintLineNumbers(event)


class SourceEdit(QtWidgets.QPlainTextEdit):
    """The expression text, with line numbers and completion.

    Line numbers earn their width as soon as an expression is more than one
    statement, which is what the controls grammar makes normal; a diagnostic
    that says `line 3` is otherwise a number with nothing to point at."""

    def __init__(self, parent=None):
        super(SourceEdit, self).__init__(parent)
        self._lineNumbers = _LineNumberArea(self)
        self.blockCountChanged.connect(self._updateViewportMargins)
        self.updateRequest.connect(self._onUpdateRequest)
        self._updateViewportMargins()

        self.completionModel = CompletionModel(self)
        self.completer = QtWidgets.QCompleter(self.completionModel, self)
        self.completer.setWidget(self)
        self.completer.setCompletionColumn(CompletionModel.NAME)
        self.completer.setCompletionRole(QtCore.Qt.DisplayRole)
        self.completer.setCaseSensitivity(QtCore.Qt.CaseInsensitive)
        self.completer.setCompletionMode(QtWidgets.QCompleter.PopupCompletion)
        popup = QtWidgets.QTreeView()
        popup.setRootIsDecorated(False)
        popup.header().hide()
        popup.setAllColumnsShowFocus(True)
        popup.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectRows)
        popup.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.completer.setPopup(popup)
        self.completer.activated[str].connect(self.insertCompletion)
        self._completionEnabled = True

    # ---- vocabulary ----------------------------------------------------

    # Popup column widths. Fixed rather than content-derived: the completer
    # filters the model down to a row or two, so asking the view for a size
    # hint sizes the popup to whichever name happens to match.
    POPUP_COLUMNS = (120, 190, 300)

    def setVocabulary(self, functions, variables):
        self.completionModel.setVocabulary(functions, variables)
        popup = self.completer.popup()
        for column, width in enumerate(self.POPUP_COLUMNS):
            popup.setColumnWidth(column, width)
        popup.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)

    def setCompletionEnabled(self, enabled):
        self._completionEnabled = bool(enabled)
        if not enabled:
            self.completer.popup().hide()

    # ---- line numbers --------------------------------------------------

    def lineNumberAreaWidth(self):
        digits = len(str(max(1, self.blockCount())))
        return 8 + self.fontMetrics().horizontalAdvance("9") * max(2, digits)

    def _updateViewportMargins(self, _count=0):
        self.setViewportMargins(self.lineNumberAreaWidth(), 0, 0, 0)

    def _onUpdateRequest(self, rect, dy):
        if dy:
            self._lineNumbers.scroll(0, dy)
        else:
            self._lineNumbers.update(0, rect.y(),
                                     self._lineNumbers.width(), rect.height())
        if rect.contains(self.viewport().rect()):
            self._updateViewportMargins()

    def resizeEvent(self, event):
        super(SourceEdit, self).resizeEvent(event)
        contents = self.contentsRect()
        self._lineNumbers.setGeometry(QtCore.QRect(
            contents.left(), contents.top(), self.lineNumberAreaWidth(),
            contents.height()))

    def paintLineNumbers(self, event):
        painter = QtGui.QPainter(self._lineNumbers)
        painter.fillRect(event.rect(), self.palette().window())
        block = self.firstVisibleBlock()
        number = block.blockNumber()
        top = self.blockBoundingGeometry(block).translated(
            self.contentOffset()).top()
        bottom = top + self.blockBoundingRect(block).height()
        current = self.textCursor().blockNumber()
        while block.isValid() and top <= event.rect().bottom():
            if block.isVisible() and bottom >= event.rect().top():
                painter.setPen(QtGui.QColor(190, 190, 190) if number == current
                               else QtGui.QColor(120, 120, 120))
                painter.drawText(0, int(top), self._lineNumbers.width() - 4,
                                 self.fontMetrics().height(),
                                 QtCore.Qt.AlignRight, str(number + 1))
            block = block.next()
            top = bottom
            bottom = top + self.blockBoundingRect(block).height()
            number += 1

    # ---- completion ----------------------------------------------------

    def completionPrefix(self):
        """The `$variable` or identifier the caret is sitting at the end of."""
        text = self.toPlainText()
        end = self.textCursor().position()
        start = end
        while start > 0 and (text[start - 1].isalnum() or text[start - 1] == "_"):
            start -= 1
        if start > 0 and text[start - 1] == "$":
            start -= 1
        prefix = text[start:end]
        if prefix.startswith("$") or (prefix[:1].isalpha() or prefix[:1] == "_"):
            return prefix
        return ""

    def completionCandidates(self, prefix=None):
        """The names that would be offered for `prefix`, or for the caret."""
        if prefix is None:
            prefix = self.completionPrefix()
        return self.completionModel.matches(prefix)

    def insertCompletion(self, name):
        """Replace the prefix under the caret with `name`'s insert text."""
        prefix = self.completionPrefix()
        cursor = self.textCursor()
        cursor.setPosition(cursor.position() - len(prefix),
                           QtGui.QTextCursor.KeepAnchor)
        body = self.completionModel.insertTextFor(name)
        caret = body.find("|")
        body = body.replace("|", "")
        cursor.insertText(body)
        if caret >= 0:
            cursor.setPosition(cursor.position() - (len(body) - caret))
        self.setTextCursor(cursor)

    def _shouldComplete(self, prefix):
        if not prefix or not self._completionEnabled or self.isReadOnly():
            return False
        minimum = (MINIMUM_VARIABLE_PREFIX if prefix.startswith("$")
                   else MINIMUM_NAME_PREFIX)
        return len(prefix) >= minimum

    def maybeComplete(self):
        """Show, update or hide the popup for whatever the caret is on."""
        popup = self.completer.popup()
        prefix = self.completionPrefix()
        if not self._shouldComplete(prefix) or not self.completionCandidates(prefix):
            popup.hide()
            return False
        if prefix != self.completer.completionPrefix():
            self.completer.setCompletionPrefix(prefix)
            popup.setCurrentIndex(
                self.completer.completionModel().index(0, 0))
        rect = self.cursorRect()
        rect.setWidth(sum(self.POPUP_COLUMNS)
                      + popup.verticalScrollBar().sizeHint().width()
                      + 2 * popup.frameWidth() + 4)
        self.completer.complete(rect)
        return True

    def keyPressEvent(self, event):
        popup = self.completer.popup()
        if popup.isVisible():
            # The popup owns these keys while it is up; letting the editor see
            # them would insert a newline instead of accepting a completion.
            if event.key() in (QtCore.Qt.Key_Enter, QtCore.Qt.Key_Return,
                               QtCore.Qt.Key_Tab, QtCore.Qt.Key_Escape,
                               QtCore.Qt.Key_Up, QtCore.Qt.Key_Down):
                event.ignore()
                return
        if (event.key() == QtCore.Qt.Key_Space
                and event.modifiers() & QtCore.Qt.ControlModifier):
            self.maybeComplete()
            return
        super(SourceEdit, self).keyPressEvent(event)
        if event.text():
            self.maybeComplete()
        elif popup.isVisible():
            popup.hide()
