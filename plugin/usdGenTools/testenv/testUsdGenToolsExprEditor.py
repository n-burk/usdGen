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
SOURCE = "$value * (0.15 + 0.85 * (1 - $t) * (1 - $t))"
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
    print("PASS: usdGenTools expression editor")
