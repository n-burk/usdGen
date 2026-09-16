# A testusdview script that photographs the expression editor.
#
# Not a ctest: it exists so the dock can be looked at without an interactive
# session, which `bin/launch_usdview.ps1` needs and CI cannot give. It opens
# the dock against plan/examples/expression-width-plane.usda, connects the
# Width operator's usdGen:width to a new expression, and writes a PNG of each
# panel.
#
#   testusdview --testScript plugin/usdGenTools/testenv/shotUsdGenToolsExprEditor.py \
#               plan/examples/expression-width-plane.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the T2 test uses. The output
# directory comes from USDGEN_SHOT_DIR, defaulting to the working directory.

import os
import sys

WIDTH = "/World/Groom/Fur/Ops/width"
SOURCE = "$value * (1.0 - 0.8 * $t)"

# An expression exercising every control kind at once, which is what the
# controls panel has to lay out well.
CONTROLS = ("$tip = 0.15; # 0, 1\n"
            "$segments = 4; # 1, 10\n"
            "$tint = [0.85, 0.55, 0.25]; # color\n"
            "$profile = curve($t, 0, 0.6, 4, 0.25, 1.0, 4, 1, 0.05, 4); # curve\n"
            "$ramp = ccurve($t, 0, [0.2, 0.1, 0.05], 4, 1, [0.9, 0.8, 0.6], 4); "
            "# ccurve\n"
            "$value * $tip * $profile")

DOCK_SIZE = (680, 980)


def _write(widget, outputDir, name, written):
    path = os.path.join(outputDir, name + ".png")
    if not widget.grab().save(path):
        print("FAIL: could not write " + path)
        sys.exit(1)
    written.append(path)
    return path


def _writeWithPopup(dock, popup, outputDir, name, written):
    """The dock with a top-level popup composed onto it where it sits.

    A popup is its own window, so grabbing the dock alone would show the
    editor without the thing being photographed."""
    from pxr.Usdviewq.qt import QtCore, QtGui

    base = dock.grab()
    overlay = popup.grab()
    offset = popup.mapToGlobal(QtCore.QPoint(0, 0)) - \
        dock.mapToGlobal(QtCore.QPoint(0, 0))
    painter = QtGui.QPainter(base)
    painter.drawPixmap(offset, overlay)
    painter.end()
    path = os.path.join(outputDir, name + ".png")
    if not base.save(path):
        print("FAIL: could not write " + path)
        sys.exit(1)
    written.append(path)
    return path


def testUsdviewInputFunction(appController):
    from pxr.Usdviewq.qt import QtGui, QtWidgets

    app = QtWidgets.QApplication.instance()
    outputDir = os.environ.get("USDGEN_SHOT_DIR") or os.getcwd()
    if not os.path.isdir(outputDir):
        os.makedirs(outputDir)

    appController._plugRegistry.getCommandPlugin(
        "usdGenTools.showExpressionEditor").run()
    app.processEvents()

    import usdGenTools.exprEditor as exprEditor
    from usdGenTools import exprControls
    dock = exprEditor._ACTIVE_EDITOR

    stage = appController._dataModel.stage
    appController._dataModel.selection.setPrim(stage.GetPrimAtPath(WIDTH))
    app.processEvents()

    # The example already connects usdGen:width, so drop that and rebuild the
    # connection through the editor -- which is the path being photographed.
    dock.selectConnectionRow("usdGen:width")
    dock.disconnectSelectedAttribute()
    app.processEvents()
    dock.selectConnectionRow("usdGen:width")
    expression = dock.connectSelectedAttribute(name="taperWidth")
    app.processEvents()
    if expression is None:
        print("FAIL: could not connect usdGen:width")
        sys.exit(1)

    dock.sourceEdit.setPlainText(SOURCE)
    dock.apply()
    dock.validateNow()
    app.processEvents()

    dock.setFloating(True)
    dock.resize(*DOCK_SIZE)
    dock.show()
    app.processEvents()

    written = []

    # The controls panel, with one of every kind showing at once.
    dock.tabs.setCurrentIndex(0)
    dock.sourceEdit.setPlainText(CONTROLS)
    dock.validateNow()
    app.processEvents()
    _write(dock, outputDir, "controls", written)

    # The loose-number fallback, and the plain editor with a clean status.
    dock.sourceEdit.setPlainText(SOURCE)
    dock.validateNow()
    app.processEvents()
    _write(dock, outputDir, "editor", written)

    for index, name in ((1, "reference"), (2, "library"), (3, "connections")):
        dock.tabs.setCurrentIndex(index)
        app.processEvents()
        _write(dock, outputDir, name, written)

    # The Add Widget dialog, as its own window.
    dock.tabs.setCurrentIndex(0)
    app.processEvents()
    dialog = exprControls.AddWidgetDialog(dock, existingNames=["$tip"])
    dialog.nameEdit.setText("$clumpWeight")
    dialog.kindCombo.setCurrentIndex(dialog.kindCombo.findData("float"))
    dialog.minimumBox.setValue(0.0)
    dialog.maximumBox.setValue(2.0)
    dialog.show()
    app.processEvents()
    _write(dialog, outputDir, "addwidget", written)
    dialog.close()
    dialog.deleteLater()
    app.processEvents()

    # The completion popup, composed onto the dock where it actually appears.
    dock.sourceEdit.setPlainText("$value * cl")
    dock.sourceEdit.moveCursor(QtGui.QTextCursor.End)
    dock.sourceEdit.setFocus()
    app.processEvents()
    if dock.sourceEdit.maybeComplete():
        app.processEvents()
        _writeWithPopup(dock, dock.sourceEdit.completer.popup(), outputDir,
                        "completion", written)
        dock.sourceEdit.completer.popup().hide()
    else:
        print("FAIL: the completion popup did not come up")
        sys.exit(1)

    # And one with a diagnostic showing, so the error strip and the marked
    # line are visible too.
    dock.sourceEdit.setPlainText("$value\n  * 2\n  * nosuch($t)\n  * 3")
    dock.validateNow()
    app.processEvents()
    _write(dock, outputDir, "error", written)

    for path in written:
        print("wrote " + path)
    print("PASS: usdGenTools expression editor screenshots")
