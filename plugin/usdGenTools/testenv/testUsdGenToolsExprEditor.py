# testusdview script for the usdGenTools SeExpr expression editor.
#
# Drives usdview headlessly against plan/examples/expression-width-plane.usda:
#   testusdview --testScript <this file> plan/examples/expression-width-plane.usda
# with PXR_PLUGINPATH_NAME naming the build-tree resources (schema, imaging,
# shaders, tools) and PYTHONPATH naming build/python and the OpenUSD python
# package. CMake registers it as testUsdGenToolsExprEditor (label T2) when
# testusdview is installed beside the OpenUSD prefix.
import sys

EXPR = "/World/Groom/Fur/Expressions/rootTipWidth"
WIDTH = "/World/Groom/Fur/Ops/width"
SOURCE = ("$rootWidth = 1.0;      # 0, 2\n"
          "$tipWidth = 0.15;      # 0, 1\n"
          "$profile = curve($t, 0, 1, 4, 0.5, 0.7, 4, 1, 0, 4);\n"
          "$value * mix($tipWidth, $rootWidth, $profile)")
NEW_SOURCE = "$value * (1 - 0.5 * $t)"


def check(cond, msg):
    if not cond:
        print("FAIL: " + msg)
        sys.exit(1)
    print("ok: " + msg)


def testUsdviewInputFunction(appController):
    from pxr.Usdviewq.qt import QtWidgets
    app = QtWidgets.QApplication.instance()
    registry = appController._plugRegistry
    check(registry is not None, "usdview loaded plugin containers")
    command = registry.getCommandPlugin("usdGenTools.showExpressionEditor")
    check(command is not None, "usdGenTools registered its editor command")
    command.run()
    app.processEvents()

    import usdGenTools.exprEditor as exprEditor
    dock = exprEditor._ACTIVE_EDITOR
    check(dock is not None and dock.isVisible(), "editor dock is open")
    check(dock.prim is None, "nothing selected -> no expression bound")

    stage = appController._dataModel.stage
    selection = appController._dataModel.selection

    # Selecting the expression prim itself.
    selection.setPrim(stage.GetPrimAtPath(EXPR))
    app.processEvents()
    check(dock.prim is not None and dock.prim.GetPath().pathString == EXPR,
          "expression prim selected -> editor follows it")
    check(dock.sourceEdit.toPlainText() == SOURCE, "source text loaded")
    check("result (float)" in dock.outputsLabel.text(), "outputs listed")
    check(not dock.isDirty(), "clean after load")

    # Selecting the operator that consumes it, from a cleared selection so a
    # stale combo cannot mask a failure in the connection walk.
    selection.clearPrims()
    app.processEvents()
    check(dock.prim is None and dock.targetCombo.count() == 0,
          "cleared selection empties the target list")
    selection.setPrim(stage.GetPrimAtPath(WIDTH))
    app.processEvents()
    check(dock.targetCombo.count() == 1 and
          dock.targetCombo.itemText(0) == EXPR,
          "operator selected -> its connected expression is offered")
    check(dock.prim.GetPath().pathString == EXPR,
          "operator selection edits the connected expression")

    # Editing and applying.
    dock.sourceEdit.setPlainText(NEW_SOURCE)
    app.processEvents()
    check(dock.isDirty(), "edit marks the editor dirty")
    check(dock.apply(), "apply succeeds")
    app.processEvents()
    attr = stage.GetPrimAtPath(EXPR).GetAttribute("usdGen:expr:source")
    check(attr.Get() == NEW_SOURCE, "usdGen:expr:source authored on the stage")
    check(not dock.isDirty(), "clean after apply")

    # An external edit is picked up when the editor is clean.
    attr.Set(SOURCE)
    app.processEvents()
    check(dock.sourceEdit.toPlainText() == SOURCE,
          "external stage edit reloads the text")

    # Deselecting clears the editor.
    selection.clearPrims()
    app.processEvents()
    check(dock.prim is None and dock.sourceEdit.toPlainText() == "",
          "clearing the selection empties the editor")

    testWidgetry(appController, dock, app)
    testControls(dock, app)
    testAddWidget(dock, app)
    testCompletion(dock, app)
    testLibraryBrowser(dock, app)
    testDiagnosticPositions(dock, app)
    testFunctionCategories(dock)
    testPreviewAndClear(appController, dock, app)
    print("PASS: usdGenTools expression editor")


def testWidgetry(appController, dock, app):
    """The editor widgetry around the text: highlighting, validation, the
    literal sliders, the browsers, the domain combo and connection editing."""
    from pxr.Usdviewq.qt import QtGui

    from usdGenTools import exprApi

    stage = appController._dataModel.stage
    selection = appController._dataModel.selection

    # --- syntax highlighting --------------------------------------------
    check(dock.highlighter is not None, "a highlighter exists")
    check(dock.highlighter.document() is dock.sourceEdit.document(),
          "the highlighter is attached to the editor's document")
    check(bool(dock.functionBrowser.functions),
          "the function browser is filled from the C ABI")
    check(bool(dock.variableBrowser.variables),
          "the variable browser is filled from the C ABI")

    selection.setPrim(stage.GetPrimAtPath(EXPR))
    app.processEvents()
    check(dock.prim is not None, "the expression prim is selected again")

    # --- live validation -------------------------------------------------
    dock.sourceEdit.setPlainText("$value * (1 - $t)")
    dock.validateNow()
    check(dock.diagnostics == [], "a good expression validates clean")
    check("OK" in dock.statusLabel.text(), "the status strip says OK")

    dock.sourceEdit.setPlainText("$value * (1 - ")
    dock.validateNow()
    check(bool(dock.diagnostics), "an unbalanced expression reports a diagnostic")
    first = dock.diagnostics[0]
    check(first.line >= 1 and first.column >= 1,
          "the diagnostic carries a line and a column (%d, %d)"
          % (first.line, first.column))
    check("line %d, col %d" % (first.line, first.column) in dock.statusLabel.text(),
          "the status strip shows the line and column")
    check(bool(dock.sourceEdit.extraSelections()),
          "the offending line is marked in the text")

    dock.sourceEdit.setPlainText("$value\n  * nosuchfunction($t)")
    dock.validateNow()
    check(bool(dock.diagnostics), "an unsupported function reports a diagnostic")
    check(dock.diagnostics[0].line == 2,
          "the diagnostic lands on the line the function is on")

    # --- the literal controls --------------------------------------------
    dock.sourceEdit.setPlainText("$value * (1.0 - 0.8 * $t)")
    app.processEvents()
    check([l.text for l in dock.literalPanel.literals] == ["1.0", "0.8"],
          "the literal panel found both literals")
    check(len(dock.literalPanel.rows) == 2, "a slider row per literal")
    check(dock.literalPanel.rows[1].value() == 0.8,
          "the row shows the literal's value")
    dock.setLiteralValue(1, 0.5)
    app.processEvents()
    check(dock.sourceEdit.toPlainText() == "$value * (1.0 - 0.5 * $t)",
          "dragging a literal rewrites it in place, leaving the rest alone")
    check(dock.literalPanel.literals[1].text == "0.5",
          "the panel tracks the rewritten literal")
    check(dock.isDirty(), "a literal edit marks the editor dirty")
    check(dock.windowTitle().endswith("*"),
          "the dock title carries the dirty indicator")
    dock.sourceEdit.undo()
    check(dock.sourceEdit.toPlainText() == "$value * (1.0 - 0.8 * $t)",
          "one undo step takes the literal edit back")

    # Live apply writes each rewrite straight to the stage.
    dock.liveApplyCheck.setChecked(True)
    dock.setLiteralValue(1, 0.25)
    app.processEvents()
    check(stage.GetPrimAtPath(EXPR).GetAttribute("usdGen:expr:source").Get()
          == "$value * (1.0 - 0.25 * $t)",
          "live apply writes the rewrite to the stage")
    check(not dock.isDirty(), "the editor is clean after a live apply")
    dock.liveApplyCheck.setChecked(False)

    # --- inserting from the browsers -------------------------------------
    dock.sourceEdit.setPlainText("$value * ")
    dock.sourceEdit.moveCursor(QtGui.QTextCursor.End)
    dock.insertAtCursor("abs(|)")
    check(dock.sourceEdit.toPlainText() == "$value * abs()",
          "a function inserts at the cursor")
    check(dock.sourceEdit.textCursor().position() == len("$value * abs("),
          "the caret lands inside the call")
    dock.insertAtCursor("$t")
    check(dock.sourceEdit.toPlainText() == "$value * abs($t)",
          "a variable inserts at the caret the function left")

    # --- connection editing ----------------------------------------------
    selection.setPrim(stage.GetPrimAtPath(WIDTH))
    app.processEvents()
    check(dock.connectionTree.topLevelItemCount() > 0,
          "the Connections tab lists the operator's attributes")
    check(dock.selectConnectionRow("usdGen:width"),
          "usdGen:width has a row in the Connections tab")

    width = stage.GetPrimAtPath(WIDTH).GetAttribute("usdGen:width")
    check(dock.disconnectSelectedAttribute(), "Disconnect reports success")
    app.processEvents()
    check(width.GetConnections() == [],
          "Disconnect removes the connection from the stage")
    check(dock.prim is None,
          "with nothing connected the editor has no expression to edit")

    check(dock.selectConnectionRow("usdGen:width"),
          "the row is still selectable after disconnecting")
    expression = dock.connectSelectedAttribute(name="taperWidth")
    app.processEvents()
    check(expression is not None and expression.IsValid(),
          "Connect creates an expression")
    check(expression.GetPath().pathString ==
          "/World/Groom/Fur/Expressions/taperWidth",
          "the expression lands under the description's Expressions scope")
    check(width.GetConnections() == [expression.GetPath()],
          "Connect authors the .connect by PRIM path")
    check(width.GetCustomDataByKey("usdGen:evaluation") == "point",
          "Connect authors the evaluation customData on the destination")
    check(expression.GetAttribute("outputs:result").GetTypeName() ==
          width.GetTypeName(),
          "outputs:result matches the attribute's type")
    check(dock.prim is not None and dock.prim.GetPath() == expression.GetPath(),
          "the editor follows the expression it just created")
    check(dock.sourceEdit.toPlainText() == "$value",
          "the new expression starts as a pass-through")

    # --- the domain combo -------------------------------------------------
    check(dock.binding is not None and dock.binding.GetName() == "usdGen:width",
          "the editor knows which attribute the expression drives")
    check(dock.domainCombo.currentData() == "point",
          "the domain combo shows the destination's evaluation domain")
    dock.domainCombo.setCurrentIndex(
        dock.domainCombo.findData("primitive"))
    app.processEvents()
    check(width.GetCustomDataByKey("usdGen:evaluation") == "primitive",
          "the domain combo writes usdGen:evaluation to the stage")
    check(dock.domainCode() == exprApi.DOMAIN_PRIMITIVE,
          "the combo reports the domain the compiler is given")

    byName = dict((v.name, v) for v in dock.variableBrowser.variables)
    check(byName["$t"].valid,
          "$t is valid in the primitive domain and shown as such")
    dock.domainCombo.setCurrentIndex(dock.domainCombo.findData("groom"))
    app.processEvents()
    byName = dict((v.name, v) for v in dock.variableBrowser.variables)
    check(not byName["$t"].valid,
          "$t is greyed out once the domain no longer defines it")
    check(byName["$seed"].valid, "$seed stays valid in the groom domain")

    # A groom-domain expression using $t must now fail to validate, which is
    # the whole point of tying the combo to the compiler.
    dock.sourceEdit.setPlainText("$value * (1 - $t)")
    dock.validateNow()
    check(bool(dock.diagnostics),
          "changing the domain revalidates and rejects $t")
    dock.domainCombo.setCurrentIndex(dock.domainCombo.findData("point"))
    dock.validateNow()
    check(dock.diagnostics == [],
          "the same text validates again in the point domain")


CONTROLS = ("$tip = 0.15; # 0, 1\n"
            "$tint = [1, 0.5, 0.2]; # color\n"
            "$profile = curve($t, 0, 1, 4, 0.5, 0.7, 4, 1, 0, 4); # curve\n"
            "$value * $tip * $profile")


def testControls(dock, app):
    """The Controls panel: one widget per top-level `$name = ...` statement,
    each rewriting exactly its own value in the text."""
    from usdGenTools import exprApi

    check(dock.tabs.tabText(0) == "Controls",
          "the Literals tab has become the Controls panel")

    dock.sourceEdit.setPlainText(CONTROLS)
    app.processEvents()
    panel = dock.controlPanel
    check([c.name for c in panel.controls] == ["$tip", "$tint", "$profile"],
          "a control per top-level assignment, in source order")
    check([c.kind for c in panel.controls] == ["float", "color", "curve"],
          "each control is typed by its value and its comment")
    check(len(panel.rows) == 3, "a widget per control")

    # --- the number control ------------------------------------------------
    number = panel.rows[0]
    check((number.minimumBox.value(), number.maximumBox.value()) == (0.0, 1.0),
          "the `# 0, 1` comment sets the slider range")
    check(number.value() == 0.15, "the number control shows the value")
    dock.setControlValue(0, 0.5)
    app.processEvents()
    check(dock.sourceEdit.toPlainText().startswith("$tip = 0.5; # 0, 1"),
          "the number control rewrites its own value and nothing else")
    check(dock.sourceEdit.toPlainText().endswith("$value * $tip * $profile"),
          "the body of the expression is untouched")
    check(dock.isDirty(), "a control edit marks the editor dirty")
    dock.sourceEdit.undo()
    check(dock.sourceEdit.toPlainText().startswith("$tip = 0.15;"),
          "one undo step takes a control edit back")

    # The range lives in the comment, so widening the slider edits the text.
    dock.sourceEdit.setPlainText(CONTROLS)
    app.processEvents()
    dock.controlPanel.rows[0].maximumBox.setValue(2.0)
    app.processEvents()
    check(dock.sourceEdit.toPlainText().splitlines()[0] == "$tip = 0.15; # 0.0, 2.0",
          "editing the range rewrites the `# min, max` comment")
    check(dock.controlPanel.controls[0].data["maximum"] == 2.0,
          "and the control takes the new range back from the text")

    # A statement with no comment grows one rather than losing the range.
    dock.sourceEdit.setPlainText("$gain = 0.25;\n$value * $gain")
    app.processEvents()
    check((dock.controlPanel.rows[0].minimumBox.value(),
           dock.controlPanel.rows[0].maximumBox.value()) == (0.0, 1.0),
          "a number with no annotation gets the default [0, 1] range")
    dock.controlPanel.rows[0].maximumBox.setValue(4.0)
    app.processEvents()
    check(dock.sourceEdit.toPlainText().splitlines()[0]
          == "$gain = 0.25; # 0.0, 4.0",
          "a statement with no annotation gets one at the end of its line")

    # --- the colour control ------------------------------------------------
    dock.sourceEdit.setPlainText(CONTROLS)
    app.processEvents()
    colour = dock.controlPanel.rows[1]
    check(colour.isColor and colour.swatch is not None,
          "`# color` gives the vector a swatch as well as sliders")
    check(len(colour.sliders) == 3, "a slider per component")
    check(colour.value() == (1.0, 0.5, 0.2), "the swatch shows the components")
    dock.setControlValue(1, (0.2, 0.3, 0.4))
    app.processEvents()
    check("$tint = [0.2, 0.3, 0.4]; # color" in dock.sourceEdit.toPlainText(),
          "the colour control rewrites the vector literal in place")
    check(dock.sourceEdit.toPlainText().startswith("$tip = 0.15; # 0, 1"),
          "the control above it is untouched")

    plain = "$offset = [0, 1, 0];\n$value"
    dock.sourceEdit.setPlainText(plain)
    app.processEvents()
    check(not dock.controlPanel.rows[0].isColor,
          "a vector without `# color` gets no swatch")

    # --- the curve control -------------------------------------------------
    dock.sourceEdit.setPlainText(CONTROLS)
    app.processEvents()
    curveRow = dock.controlPanel.rows[2]
    check(curveRow.curve.knots() == [(0.0, 1.0, 4), (0.5, 0.7, 4), (1.0, 0.0, 4)],
          "the curve widget is built from the knot list in the text")
    curveRow.curve.selectKnot(1)
    check(curveRow.curve.selectedIndex() == 1, "a knot can be selected")
    curveRow.curve.setSelectedKnot(value=0.25)
    app.processEvents()
    check("curve($t, 0.0, 1.0, 4, 0.5, 0.25, 4, 1.0, 0.0, 4)"
          in dock.sourceEdit.toPlainText(),
          "moving a knot rewrites the knot list")
    curveRow.interpCombo.setCurrentIndex(exprApi.INTERP_SMOOTH)
    app.processEvents()
    check("0.5, 0.25, 2," in dock.sourceEdit.toPlainText(),
          "the interpolation combo writes the knot's interpolation code")
    check(curveRow.curve.removeSelected(), "a knot can be deleted")
    app.processEvents()
    check("curve($t, 0.0, 1.0, 4, 1.0, 0.0, 4)" in dock.sourceEdit.toPlainText(),
          "deleting a knot takes its triple out of the call")

    ramp = '$ramp = ccurve($t, 0, [1, 0, 0], 4, 1, [0, 0, 1], 4); # ccurve\n$value'
    dock.sourceEdit.setPlainText(ramp)
    app.processEvents()
    colourCurve = dock.controlPanel.rows[0]
    check(colourCurve.isColor and colourCurve.valueButton is not None,
          "a colour curve offers a swatch per knot instead of a value box")
    colourCurve.curve.selectKnot(0)
    colourCurve.curve.setSelectedKnot(value=(0.0, 1.0, 0.0))
    app.processEvents()
    check("ccurve($t, 0.0, [0.0, 1.0, 0.0], 4," in dock.sourceEdit.toPlainText(),
          "a colour knot rewrites as a vector literal")

    # --- the loose-number fallback -----------------------------------------
    dock.sourceEdit.setPlainText("$value * 0.8")
    app.processEvents()
    check(dock.controlPanel.controls == [], "a bare expression declares nothing")
    check(len(dock.literalPanel.rows) == 1,
          "a number no variable holds still gets a slider")
    dock.setLiteralValue(0, 0.5)
    app.processEvents()
    check(dock.sourceEdit.toPlainText() == "$value * 0.5",
          "the loose-number slider still rewrites its literal")

    dock.sourceEdit.setPlainText("$tip = 0.15; # 0, 1\n$value * $tip * 0.8")
    app.processEvents()
    check([l.text for l in dock.literalPanel.literals] == ["0.8"],
          "a number a control already drives is not offered twice")


def testAddWidget(dock, app):
    """SeExpr's Add Widget dialog, and the line it inserts."""
    from pxr.Usdviewq.qt import QtWidgets

    from usdGenTools import exprControls

    dock.sourceEdit.setPlainText("$value * 2")
    app.processEvents()
    dialog = exprControls.AddWidgetDialog(dock, existingNames=["$tip"])
    dialog.nameEdit.setText("$blend")
    dialog.kindCombo.setCurrentIndex(dialog.kindCombo.findData("color"))
    app.processEvents()
    check(dialog.declaration() == "$blend = [1.0, 1.0, 1.0]; # color",
          "the dialog describes the declaration it would insert")
    check(not dialog.lookupEdit.isVisible(),
          "a colour widget does not ask for a curve lookup")
    dialog.kindCombo.setCurrentIndex(dialog.kindCombo.findData("int"))
    dialog.valueBox.setValue(3)
    dialog.minimumBox.setValue(1)
    dialog.maximumBox.setValue(10)
    app.processEvents()
    check(dialog.declaration() == "$blend = 3; # 1, 10",
          "an integer widget keeps its value and range integral")
    okButton = dialog.buttons.button(QtWidgets.QDialogButtonBox.Ok)
    dialog.nameEdit.setText("$tip")
    app.processEvents()
    check(not okButton.isEnabled(), "a name already in use is refused")
    dialog.nameEdit.setText("$blend")
    app.processEvents()
    check(okButton.isEnabled(), "a free name is accepted")
    line = dialog.declaration()
    dialog.deleteLater()

    check(dock.insertDeclaration(line) == line, "the declaration is inserted")
    app.processEvents()
    check(dock.sourceEdit.toPlainText() == line + "\n$value * 2",
          "Add Widget puts the declaration at the top of the expression")
    check([c.name for c in dock.controlPanel.controls] == ["$blend"],
          "and the panel grows the control it declared")
    check(dock.controlPanel.rows[0].isInt,
          "an integer declaration builds an integer control")


def testCompletion(dock, app):
    """The completion popup: the engine's own functions and variables."""
    from pxr.Usdviewq.qt import QtGui

    model = dock.sourceEdit.completionModel
    names = model.names()
    check("clamp" in names, "the completion model carries the functions")
    check("$t" in names, "the completion model carries the variables")
    check(model.rowCount() >= len(dock.functionBrowser.functions),
          "it carries at least everything the function browser lists")

    dock.sourceEdit.setPlainText("$value * cla")
    dock.sourceEdit.moveCursor(QtGui.QTextCursor.End)
    app.processEvents()
    check(dock.sourceEdit.completionPrefix() == "cla",
          "the prefix under the caret is the word being typed")
    check(dock.sourceEdit.completionCandidates() == ["clamp"],
          "a function prefix offers functions")
    check(dock.sourceEdit.maybeComplete(), "the popup comes up for it")
    check(dock.sourceEdit.completer.completionCount() >= 1,
          "the completer has something to offer")
    dock.sourceEdit.insertCompletion("clamp")
    app.processEvents()
    check(dock.sourceEdit.toPlainText() == "$value * clamp(, , )",
          "accepting a completion inserts the call, not just the name")
    check(dock.sourceEdit.textCursor().position() == len("$value * clamp("),
          "and leaves the caret inside the parentheses")

    dock.sourceEdit.setPlainText("$value * $se")
    dock.sourceEdit.moveCursor(QtGui.QTextCursor.End)
    app.processEvents()
    check(dock.sourceEdit.completionPrefix() == "$se",
          "a '$' is part of the prefix it starts")
    candidates = dock.sourceEdit.completionCandidates()
    check(candidates == ["$seed"], "a '$' prefix offers variables")
    check(all(n.startswith("$") for n in candidates),
          "and offers no functions, where only a variable can go")
    dock.sourceEdit.insertCompletion("$seed")
    check(dock.sourceEdit.toPlainText() == "$value * $seed",
          "a variable completes to its own name")

    dock.sourceEdit.setPlainText("$value * ")
    dock.sourceEdit.moveCursor(QtGui.QTextCursor.End)
    check(dock.sourceEdit.completionPrefix() == "",
          "there is no prefix where nothing is being typed")
    check(not dock.sourceEdit.maybeComplete(),
          "and no popup either")

    signature = model.item(0, model.SIGNATURE).text()
    check(bool(signature),
          "each completion carries the signature or type shown beside it")


def testLibraryBrowser(dock, app):
    """The saved-expression library: shipped presets, save, load, filter."""
    import os
    import shutil
    import tempfile

    from usdGenTools import exprLibrary

    directory = tempfile.mkdtemp(prefix="usdGenExprEditor")
    previous = os.environ.get(exprLibrary.PATH_ENVIRONMENT)
    try:
        os.environ[exprLibrary.PATH_ENVIRONMENT] = directory
        browser = dock.libraryBrowser
        browser.refresh()
        app.processEvents()
        names = [e.name for e in browser.entries]
        check("rootTipTaper" in names,
              "the shipped presets are in the library browser")
        check("widthProfile" in names, "including a curve preset")

        dock.sourceEdit.setPlainText("$tip = 0.2; # 0, 1\n$value * $tip")
        app.processEvents()
        path = dock.saveToLibrary("editor round trip")
        check(path is not None and os.path.exists(path),
              "Save writes the expression into the user library")
        check(os.path.dirname(path) == os.path.abspath(directory),
              "and puts it where USDGEN_EXPRESSION_PATH says")

        dock.sourceEdit.setPlainText("$value")
        app.processEvents()
        check(dock.loadFromLibrary("editor_round_trip"),
              "the saved expression is there to load")
        app.processEvents()
        check("$tip = 0.2" in dock.sourceEdit.toPlainText(),
              "loading brings the text back")
        check([c.name for c in dock.controlPanel.controls] == ["$tip"],
              "and rebuilds the controls it declares")
        check(dock.isDirty(),
              "a loaded expression is an unapplied edit until Apply")

        check(dock.loadFromLibrary("widthProfile"),
              "a shipped preset loads too")
        app.processEvents()
        check(dock.controlPanel.controls[0].kind == "curve",
              "and the curve preset builds a curve control")

        # The domain filter hides what cannot apply here, and keeps what says
        # nothing about where it belongs.
        saved = dict((e.name, e) for e in browser.entries)
        check(saved["rootTipTaper"].domain == "point",
              "a preset carries the domain it was written for")
        check(saved["editor_round_trip"].domain == dock.domainName(),
              "Save records the editor's own evaluation domain")
        index = browser.domainCombo.findData("groom")
        browser.domainCombo.setCurrentIndex(index)
        app.processEvents()
        check(not browser.selectEntry("rootTipTaper"),
              "a point-domain preset is filtered out of the groom domain")
        browser.domainCombo.setCurrentIndex(0)
        app.processEvents()
        check(browser.selectEntry("rootTipTaper"),
              "and comes back when the filter is cleared")

        browser.searchEdit.setText("clump")
        app.processEvents()
        check(not browser.selectEntry("rootTipTaper") and
              browser.selectEntry("clumpMask"),
              "the search box narrows the tree to matching names")
        browser.searchEdit.setText("")
        app.processEvents()
    finally:
        if previous is None:
            os.environ.pop(exprLibrary.PATH_ENVIRONMENT, None)
        else:
            os.environ[exprLibrary.PATH_ENVIRONMENT] = previous
        shutil.rmtree(directory, ignore_errors=True)
        dock.libraryBrowser.refresh()


def testDiagnosticPositions(dock, app):
    """Positions across a multi-statement expression, and the marker."""
    dock.sourceEdit.setPlainText("$value\n  * 2\n  * nosuchfunction($t)\n  * 3")
    dock.validateNow()
    app.processEvents()
    check(bool(dock.diagnostics), "an unsupported function is still rejected")
    first = dock.diagnostics[0]
    check(first.line == 3,
          "a diagnostic on the third line of four reports line 3 (got %d)"
          % first.line)
    check(first.column >= 5,
          "and the column the offending name starts at (got %d)" % first.column)
    selections = dock.sourceEdit.extraSelections()
    check(bool(selections), "the offending line is marked in the text")
    check(any(s.cursor.blockNumber() == 2 for s in selections),
          "the marker is on the block the diagnostic named")

    # A string literal is refused by the frontend today and will still be
    # refused once it grows multi-statement support, so this check does not
    # depend on which engine it runs against.
    dock.sourceEdit.setPlainText('$value * "text"')
    dock.validateNow()
    check(bool(dock.diagnostics), "a string literal is rejected")
    check(dock.diagnostics[0].line >= 1,
          "with a line to point at (got %d)" % dock.diagnostics[0].line)

    dock.sourceEdit.setPlainText("$value * (1 - $t)")
    dock.validateNow()
    check(dock.diagnostics == [], "and a good expression is clean again")
    check(not dock.sourceEdit.extraSelections() or
          all(s.cursor.blockNumber() == 0 for s in dock.sourceEdit.extraSelections()),
          "the error marker is gone")

    check(dock.sourceEdit.lineNumberAreaWidth() > 0,
          "the editor carries a line-number gutter")


def testFunctionCategories(dock):
    categories = dock.functionBrowser.categories()
    check("Trigonometry" in categories,
          "the function browser groups by the engine's categories")
    check("Operators" in categories,
          "the operators are a group of their own")
    check("Other" not in categories or categories[-1] == "Other",
          "the leftovers bin, if there is one, sorts last")
    total = sum(dock.functionBrowser.tree.topLevelItem(i).childCount()
                for i in range(dock.functionBrowser.tree.topLevelItemCount()))
    check(total == len(dock.functionBrowser.functions),
          "every function the ABI reported is under exactly one group")


def testPreviewAndClear(appController, dock, app):
    """Preview writes to the session layer; Apply commits; Revert drops it."""
    stage = appController._dataModel.stage
    prim = dock.prim
    check(prim is not None, "there is an expression to preview")
    attr = prim.GetAttribute("usdGen:expr:source")
    path = attr.GetPath()

    previous = stage.GetEditTarget()
    stage.SetEditTarget(stage.GetRootLayer())
    try:
        dock.sourceEdit.setPlainText("$value * 0.25")
        app.processEvents()
        check(dock.preview(), "Preview succeeds")
        app.processEvents()
        check(dock.isPreviewing(), "the editor says it is previewing")
        check(attr.Get() == "$value * 0.25",
              "the previewed text composes onto the stage")
        check(stage.GetSessionLayer().GetAttributeAtPath(path) is not None,
              "the override lives in the session layer")
        check(stage.GetRootLayer().GetAttributeAtPath(path) is None or
              stage.GetRootLayer().GetAttributeAtPath(path).default
              != "$value * 0.25",
              "and the layer being authored is untouched")
        check(dock.isDirty(), "a preview is still an unapplied edit")

        dock.revert()
        app.processEvents()
        check(not dock.isPreviewing(), "Revert ends the preview")
        check(stage.GetSessionLayer().GetAttributeAtPath(path) is None or
              stage.GetSessionLayer().GetAttributeAtPath(path).default is None,
              "and takes the session-layer override away")
        check(attr.Get() != "$value * 0.25",
              "so the stage is back to what was authored")

        # Apply after a preview must commit to the edit target AND clear the
        # session override, or the commit would stay invisible behind it.
        dock.sourceEdit.setPlainText("$value * 0.75")
        app.processEvents()
        dock.preview()
        check(dock.apply(), "Apply succeeds after a preview")
        app.processEvents()
        check(not dock.isPreviewing(), "Apply ends the preview")
        check(stage.GetRootLayer().GetAttributeAtPath(path).default
              == "$value * 0.75",
              "the text is committed to the edit target")
        check(not dock.isDirty(), "and the editor is clean")

        dock.clear()
        app.processEvents()
        check(dock.sourceEdit.toPlainText() == "", "Clear empties the text")
        check(dock.isDirty(), "Clear is an edit like any other")
        check(attr.Get() == "$value * 0.75",
              "and writes nothing until Apply")
        dock.revert()
        app.processEvents()
    finally:
        stage.SetEditTarget(previous)
