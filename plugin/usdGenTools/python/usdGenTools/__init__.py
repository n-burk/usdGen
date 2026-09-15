# usdGenTools -- usdview plugin container for the usdGen authoring tools.
#
# Discovered by usdview through plugin/usdGenTools/resources/plugInfo.json
# (Type "python"); this package must be importable, which the launchers
# arrange by putting build/python on PYTHONPATH. Everything Qt-dependent is
# imported lazily from the command callbacks so that importing the package
# in a non-GUI process (tests, headless tools) stays cheap and safe.

from pxr import Tf
from pxr.Usdviewq.plugin import PluginContainer

SHOW_EXPRESSION_EDITOR = "usdGenTools.showExpressionEditor"


class UsdGenToolsPluginContainer(PluginContainer):
    """Registers the usdGen menu and its tool commands with usdview."""

    def registerPlugins(self, plugRegistry, plugCtx):
        self._showExpressionEditor = plugRegistry.registerCommandPlugin(
            SHOW_EXPRESSION_EDITOR,
            "SeExpr Expression Editor",
            self._onShowExpressionEditor,
            "Edit the usdGen:expr:source of the selected UsdGenExpression "
            "prim, or of the expressions feeding the selected operator.")

    def configureView(self, plugRegistry, plugUIBuilder):
        menu = plugUIBuilder.findOrCreateMenu("usdGen")
        menu.addItem(self._showExpressionEditor, "Ctrl+Shift+E")

    def _onShowExpressionEditor(self, usdviewApi):
        from .exprEditor import GetExpressionEditor
        GetExpressionEditor(usdviewApi).showAndRaise()


Tf.Type.Define(UsdGenToolsPluginContainer)
