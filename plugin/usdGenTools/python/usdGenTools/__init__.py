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
SHOW_BRUSH_PALETTE = "usdGenTools.showBrushPalette"
SET_SUPERSAMPLE = "usdGenTools.setSupersample%dx"

_CONTAINER = None


def container():
    """The live plugin container, or None outside usdview.

    The palette, the viewport controller and the T3 scripts all need the
    one brush state and loop; usdview hands the container to nobody, so it
    publishes itself here (the usdGenPomadeTools.container() arrangement)."""
    return _CONTAINER

# The factors the menu offers. 2x is the useful one for hair; 4x is a
# look-dev-only setting (fill and AOV memory go as N^2).
SUPERSAMPLE_FACTORS = (1, 2, 4)


def _ActionGroup():
    """QActionGroup, which moved from QtWidgets (Qt5) to QtGui (Qt6)."""
    from pxr.Usdviewq.qt import QtGui, QtWidgets
    for module in (QtGui, QtWidgets):
        factory = getattr(module, "QActionGroup", None)
        if factory is not None:
            return factory(None)
    return None


class UsdGenToolsPluginContainer(PluginContainer):
    """Registers the usdGen menu and its tool commands with usdview."""

    def __init__(self):
        super(UsdGenToolsPluginContainer, self).__init__()
        # The whole of the brush tool's mutable state, built here and never
        # assigned during registerPlugins (plan/08 section 1.3). Qt-free.
        from . import brushLoop, brushState
        self._brushState = brushState.BrushToolState()
        self._brushLoop = brushLoop.BrushLoop(self._brushState)
        self._brushViewport = None
        self._brushPalette = None
        global _CONTAINER
        _CONTAINER = self

    @property
    def brushState(self):
        return self._brushState

    @property
    def brushLoop(self):
        return self._brushLoop

    @property
    def brushViewport(self):
        return self._brushViewport

    @property
    def brushPalette(self):
        return self._brushPalette

    def ensureBrushViewport(self, usdviewApi, statusSink=None):
        """The one brush viewport controller, installed on the StageView.

        The status sink is (re)wired on every call: viewport messages
        (strokes, live groom, hotkeys, filter errors) reach the palette's
        status strip. Without an explicit sink the open palette's is used."""
        from .brushViewport import BrushViewportController
        if self._brushViewport is None:
            self._brushViewport = BrushViewportController(
                usdviewApi, self._brushState, self._brushLoop)
        if statusSink is None and self._brushPalette is not None:
            statusSink = getattr(self._brushPalette, "onLoopStatus", None)
        if statusSink is not None:
            self._brushViewport.setStatusSink(statusSink)
        if not self._brushViewport.installed:
            self._brushViewport.install()
        return self._brushViewport

    def registerPlugins(self, plugRegistry, plugCtx):
        self._showExpressionEditor = plugRegistry.registerCommandPlugin(
            SHOW_EXPRESSION_EDITOR,
            "SeExpr Expression Editor",
            self._onShowExpressionEditor,
            "Edit the usdGen:expr:source of the selected UsdGenExpression "
            "prim, or of the expressions feeding the selected operator.")
        self._showBrushPalette = plugRegistry.registerCommandPlugin(
            SHOW_BRUSH_PALETTE,
            "Brush Palette",
            self._onShowBrushPalette,
            "Paint a UsdGenPaintMap on the selected mesh: bind, stroke, "
            "preview and bake.")

        # Supersampling patches StageView, so it is installed once here rather
        # than on first use: a command that has never been run still has to
        # honour USDGEN_USDVIEW_SUPERSAMPLE from the launcher.
        from .supersample import Install, AddFactorObserver
        Install()
        from .playback import Install as InstallPlayback
        InstallPlayback(plugCtx)
        self._supersampleActions = {}
        AddFactorObserver(self._onSupersampleFactorChanged)

        self._supersampleCommands = [
            plugRegistry.registerCommandPlugin(
                SET_SUPERSAMPLE % factor,
                "Off" if factor == 1 else "%dx" % factor,
                self._makeSupersampleCallback(factor),
                "Render the viewport at %dx the window size and box-downsample "
                "it in linear light on present." % factor
                if factor > 1 else
                "Present the viewport at window resolution (the stock path).")
            for factor in SUPERSAMPLE_FACTORS]

    def configureView(self, plugRegistry, plugUIBuilder):
        from .supersample import GetFactor

        menu = plugUIBuilder.findOrCreateMenu("usdGen")
        menu.addItem(self._showExpressionEditor, "Ctrl+Shift+E")
        menu.addItem(self._showBrushPalette, "Ctrl+Shift+B")

        # Checkable and mutually exclusive, with the factor in force checked.
        # PluginMenu.addItem hands back the QAction it made, which is the only
        # way to get at one -- the plugin API has no checkable item of its own.
        supersampleMenu = menu.findOrCreateSubmenu("Viewport Supersampling")
        group = _ActionGroup()
        if group is not None:
            group.setExclusive(True)
        self._supersampleGroup = group
        for factor, command in zip(SUPERSAMPLE_FACTORS,
                                   self._supersampleCommands):
            action = supersampleMenu.addItem(command)
            action.setCheckable(True)
            action.setChecked(factor == GetFactor())
            if group is not None:
                group.addAction(action)
            self._supersampleActions[factor] = action

    def _onShowExpressionEditor(self, usdviewApi):
        from .exprEditor import GetExpressionEditor
        GetExpressionEditor(usdviewApi).showAndRaise()

    def _onShowBrushPalette(self, usdviewApi):
        from .brushPalette import GetBrushPalette
        # The palette borrows this container's state through container().
        self._brushPalette = GetBrushPalette(usdviewApi)
        self._brushPalette.showAndRaise()

    def _makeSupersampleCallback(self, factor):
        def callback(usdviewApi):
            from .supersample import SetFactor
            # Nothing here touches GL: a menu callback runs outside the paint,
            # where usdview's GL context is not current. Set the factor and ask
            # for a repaint; the paint path does every GL call, including
            # allocating the N x target for the new factor.
            SetFactor(factor)
            self._onSupersampleFactorChanged(factor)
            usdviewApi.UpdateViewport()
        return callback

    def _onSupersampleFactorChanged(self, factor):
        """Keep the check marks on whatever factor is actually in force."""
        for itemFactor, action in self._supersampleActions.items():
            action.setChecked(itemFactor == factor)


Tf.Type.Define(UsdGenToolsPluginContainer)
