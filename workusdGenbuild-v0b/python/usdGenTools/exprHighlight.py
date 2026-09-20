# Syntax highlighting and bracket matching for the expression editor.
#
# The highlighter is told which names the engine actually knows (from the C
# ABI's variable and function lists, not from a copy kept here), so an unknown
# `$variable` or an unsupported function reads as wrong while it is being
# typed, before the debounced compile confirms it.

from pxr.Usdviewq.qt import QtGui, QtWidgets

from . import exprApi

# Chosen against usdview's dark palette, and kept legible on a light one: all
# six hues sit in the mid-luminance band rather than at either extreme.
COLOR_VARIABLE = "#63b8ff"
COLOR_UNKNOWN = "#ff6b6b"
COLOR_FUNCTION = "#c792ea"
COLOR_NUMBER = "#f2955c"
COLOR_OPERATOR = "#7fdbe8"
COLOR_COMMENT = "#7a9e6e"
COLOR_STRING = "#d7a06a"

_OPERATORS = set("+-*/<>=!?:,()[]%^&|~")


def _format(color, bold=False, italic=False, underline=None):
    fmt = QtGui.QTextCharFormat()
    fmt.setForeground(QtGui.QColor(color))
    if bold:
        fmt.setFontWeight(QtGui.QFont.Bold)
    if italic:
        fmt.setFontItalic(True)
    if underline is not None:
        fmt.setUnderlineStyle(underline)
        fmt.setUnderlineColor(QtGui.QColor(COLOR_UNKNOWN))
    return fmt


class ExpressionHighlighter(QtGui.QSyntaxHighlighter):
    """SeExpr highlighting: $variables, functions, numbers, operators,
    # comments and string literals.

    String literals are legal only as the names and element expression of
    geoSampler()/ptex(); anywhere else the compile diagnostic says so."""

    def __init__(self, document):
        super(ExpressionHighlighter, self).__init__(document)
        self._variables = set()
        self._functions = set()
        wavy = QtGui.QTextCharFormat.WaveUnderline
        self._formats = {
            "variable": _format(COLOR_VARIABLE),
            "badVariable": _format(COLOR_UNKNOWN, underline=wavy),
            "function": _format(COLOR_FUNCTION, bold=True),
            "badFunction": _format(COLOR_UNKNOWN, underline=wavy),
            "number": _format(COLOR_NUMBER),
            "operator": _format(COLOR_OPERATOR),
            "comment": _format(COLOR_COMMENT, italic=True),
            "string": _format(COLOR_STRING, italic=True),
        }

    def setVocabulary(self, variableNames, functionNames):
        """The names the engine knows. Empty sets mean the C ABI could not be
        loaded, in which case nothing is marked unknown -- guessing would be
        worse than staying quiet."""
        self._variables = set(variableNames)
        self._functions = set(functionNames)
        self.rehighlight()

    def highlightBlock(self, text):
        consumed = [False] * len(text)
        for match in exprApi.TOKEN_RE.finditer(text):
            start, end = match.span()
            kind = match.lastgroup
            if kind == "comment":
                style = "comment"
            elif kind == "string":
                style = "string"
            elif kind == "number":
                style = "number"
            elif kind == "variable":
                style = ("variable" if not self._variables
                         or match.group() in self._variables else "badVariable")
            else:  # a bare identifier: a function only when a '(' follows
                tail = text[end:].lstrip()
                if not tail.startswith("("):
                    continue
                style = ("function" if not self._functions
                         or match.group() in self._functions else "badFunction")
            self.setFormat(start, end - start, self._formats[style])
            for i in range(start, end):
                consumed[i] = True

        for index, char in enumerate(text):
            if not consumed[index] and char in _OPERATORS:
                self.setFormat(index, 1, self._formats["operator"])


class BracketMatcher(object):
    """Extra selections marking the bracket pair around the cursor.

    Returned rather than applied, because the editor also marks the line an
    error is on and a QPlainTextEdit has exactly one extra-selection list."""

    OPENING = "([{"
    CLOSING = ")]}"

    MATCH_COLOR = "#3c6e41"
    MISMATCH_COLOR = "#8c3030"

    def __init__(self, edit):
        self._edit = edit

    def selections(self):
        cursor = self._edit.textCursor()
        text = self._edit.toPlainText()
        position = cursor.position()
        for candidate in (position, position - 1):
            if candidate < 0 or candidate >= len(text):
                continue
            other = self._match(text, candidate)
            if other is None:
                continue
            ok = other >= 0
            return [self._selection(p, ok)
                    for p in ((candidate, other) if ok else (candidate,))]
        return []

    def _match(self, text, position):
        """The index of the partner bracket, -1 when unmatched, None when the
        character is not a bracket at all."""
        char = text[position]
        if char in self.OPENING:
            partner = self.CLOSING[self.OPENING.index(char)]
            step, stop = 1, len(text)
        elif char in self.CLOSING:
            partner = self.OPENING[self.CLOSING.index(char)]
            step, stop = -1, -1
        else:
            return None
        depth = 0
        index = position
        while index != stop:
            here = text[index]
            if here == char:
                depth += 1
            elif here == partner:
                depth -= 1
                if depth == 0:
                    return index
            index += step
        return -1

    def _selection(self, position, ok):
        selection = QtWidgets.QTextEdit.ExtraSelection()
        selection.format.setBackground(
            QtGui.QColor(self.MATCH_COLOR if ok else self.MISMATCH_COLOR))
        cursor = QtGui.QTextCursor(self._edit.document())
        cursor.setPosition(position)
        cursor.movePosition(QtGui.QTextCursor.NextCharacter,
                            QtGui.QTextCursor.KeepAnchor)
        selection.cursor = cursor
        return selection


def ErrorSelection(edit, line, column):
    """An extra selection marking the diagnostic's line, and its column when
    the frontend gave one. Nothing when the position is unknown."""
    if line < 1:
        return []
    document = edit.document()
    block = document.findBlockByNumber(min(line, document.blockCount()) - 1)
    if not block.isValid():
        return []
    selection = QtWidgets.QTextEdit.ExtraSelection()
    selection.format.setUnderlineStyle(QtGui.QTextCharFormat.WaveUnderline)
    selection.format.setUnderlineColor(QtGui.QColor(COLOR_UNKNOWN))
    selection.format.setProperty(QtGui.QTextFormat.FullWidthSelection, True)
    selection.format.setBackground(QtGui.QColor(90, 30, 30, 90))
    cursor = QtGui.QTextCursor(block)
    if column > 1:
        cursor.setPosition(block.position() + min(column - 1, block.length() - 1))
    selection.cursor = cursor
    selection.cursor.clearSelection()
    return [selection]
