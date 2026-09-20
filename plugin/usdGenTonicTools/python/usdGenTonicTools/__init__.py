# usdGenTonicTools -- usdview plugin container for the Tonic authoring tool.
#
# Discovered by usdview through plugin/usdGenTonicTools/resources/plugInfo.json
# (Type "python"); this package must be importable, which the launchers
# arrange by putting build/python on PYTHONPATH. Everything Qt-dependent is
# imported lazily from the command callbacks so that importing the package
# in a non-GUI process (tests, headless tools) stays cheap and safe.
#
# Tonic is ONE viewport tool, not a menu (plan/18 section 1). The menu
# carries exactly five commands -- open the workspace, bind a scalp, and the
# three file commands that need a dialog -- and everything else the artist
# does is a mode, a sub-mode, a hotkey or a panel inside the workspace.
# Adding a sixth item is a plan/18 section 6 violation, not a convenience.
#
# The container owns the one TonicToolState (plan/08 section 1.3), the one
# TonicSession (model, committer, bake) and the one ViewportController; the
# workspace dock reads all three off it.

from pxr import Tf
from pxr.Usdviewq.plugin import PluginContainer

from .tonicToolState import TonicToolState

_CONTAINER = None


def container():
    """The live plugin container, or None outside usdview.

    The workspace, the hotkeys and the T3 scripts all need the one session;
    usdview hands the container to nobody, so it publishes itself here.
    """
    return _CONTAINER


class UsdGenTonicToolsPluginContainer(PluginContainer):
    """Registers usdGen -> Tonic and owns the tool's state and session."""

    def __init__(self):
        super(UsdGenTonicToolsPluginContainer, self).__init__()
        # The whole of the plugin's mutable state, built here and never
        # assigned during registerPlugins (the 08-tools.md section 1.3 rule).
        self._state = TonicToolState()
        self._session = None
        self._viewport = None
        self._workspace = None
        self._commands = {}
        global _CONTAINER
        _CONTAINER = self

    # -- what the workspace dock and the tests read ------------------------

    @property
    def tonicState(self):
        return self._state

    @property
    def session(self):
        return self._session

    @property
    def viewport(self):
        return self._viewport

    @property
    def workspace(self):
        return self._workspace

    # -- registration ------------------------------------------------------

    COMMANDS = (
        ("openWorkspace", "Open workspace",
         "Open the Tonic workspace and take the viewport."),
        ("bindScalp", "Bind scalp (selection)",
         "Bind the selected mesh as the Tonic scalp and start a session."),
        ("saveGroom", "Save groom…",
         "Write the live groom to a .usdc and sublayer it."),
        ("exportCenterCurves", "Export center curves…",
         "Write the center curves of the focused level to a USD file."),
        ("importCurves", "Import curves…",
         "Import a USD file's BasisCurves as locked child tubes."),
    )

    def registerPlugins(self, plugRegistry, plugCtx):
        for commandId, label, description in self.COMMANDS:
            self._commands[commandId] = plugRegistry.registerCommandPlugin(
                "usdGenTonicTools.%s" % commandId, label,
                self._makeCallback(commandId), description)

    def configureView(self, plugRegistry, plugUIBuilder):
        menu = plugUIBuilder.findOrCreateMenu("usdGen")
        tonicMenu = menu.findOrCreateSubmenu("Tonic")
        for commandId, _label, _description in self.COMMANDS:
            tonicMenu.addItem(self._commands[commandId])

    def _makeCallback(self, commandId):
        def callback(usdviewApi):
            handler = getattr(self, "_cmd_%s" % commandId)
            handler(usdviewApi)
        return callback

    # -- helpers -----------------------------------------------------------

    def refreshWorkspace(self):
        """Refresh the workspace dock, when one is open."""
        refresh = getattr(self._workspace, "refresh", None)
        if refresh is not None:
            refresh()

    @staticmethod
    def _status(usdviewApi, text):
        if not text or usdviewApi is None:
            return
        try:
            usdviewApi.PrintStatus(text)
        except AttributeError:
            pass

    def ensureSession(self, usdviewApi):
        """The one TonicSession, created on first use."""
        from . import tonicLib
        from .tonicSession import TonicSession
        if self._state.lib is None:
            try:
                self._state.lib = tonicLib.Library()
            except OSError as exc:
                self._status(usdviewApi, "Tonic: %s" % exc)
                return None
        if self._session is None:
            self._session = TonicSession(self._state, usdviewApi)
        return self._session

    def ensureViewport(self, usdviewApi):
        """The one ViewportController, installed on the StageView."""
        from .tonicViewport import ViewportController
        session = self.ensureSession(usdviewApi)
        if session is None:
            return None
        if self._viewport is None:
            self._viewport = ViewportController(self._state, session,
                                                usdviewApi, self)
        if not self._viewport.installed and not self._viewport.install():
            self._status(usdviewApi,
                         "Tonic: no StageView yet -- open a stage first")
            return None
        return self._viewport

    # -- the five commands -------------------------------------------------

    def _cmd_openWorkspace(self, usdviewApi):
        viewport = self.ensureViewport(usdviewApi)
        if viewport is None:
            return
        try:
            from . import tonicWorkspace
        except ImportError:
            self._status(usdviewApi,
                         "Tonic: viewport tool active (the workspace dock "
                         "lands with V3); modes on 1-6, Escape cancels")
            return
        self._workspace = tonicWorkspace.openWorkspace(self, usdviewApi)
        # Everything on the strip is derived from the model, so it goes
        # stale exactly when the viewport does: one hook, every publish.
        self._session.setPublishHook(self.refreshWorkspace)
        self.refreshWorkspace()
        self._status(usdviewApi, "Tonic: workspace open")

    def _cmd_bindScalp(self, usdviewApi):
        session = self.ensureSession(usdviewApi)
        if session is None:
            return
        # UsdviewApi.selectedPaths, not the selection model's own getter:
        # the P2 command asked for getSelectedPrimPaths, which no usdview
        # has (it is getPrimPaths), inside a try/except AttributeError, so
        # "Bind scalp" silently did nothing in a real session.
        paths = list(usdviewApi.selectedPaths)
        if not paths:
            self._status(usdviewApi, "Tonic: select the scalp mesh first")
            return
        if not session.activate(str(paths[0])):
            return
        viewport = self.ensureViewport(usdviewApi)
        if viewport is not None:
            viewport.setMode("graph")
            # The overlay dots are sized in world units from a pixel
            # target, and the scale that converts them is measured at the
            # scalp: until a scalp is bound there is nothing to measure at,
            # so the first real reading is here (plan/18 section 2.4a).
            viewport.syncDisplayScale()
        self._status(usdviewApi, "Tonic: bound %s as the scalp" % paths[0])

    def _cmd_saveGroom(self, usdviewApi):
        from . import tonicViewport
        session = self._session
        if session is None or session.model is None:
            self._status(usdviewApi, "Tonic: bind a scalp first")
            return
        path = tonicViewport.askSaveFile(
            getattr(usdviewApi, "qMainWindow", None), "Save Tonic groom",
            "USD crate (*.usdc)", "groom.usdc")
        if path:
            session.saveGroom(path)

    def _cmd_exportCenterCurves(self, usdviewApi):
        from . import tonicViewport
        session = self._session
        if session is None or session.model is None:
            self._status(usdviewApi, "Tonic: bind a scalp first")
            return
        path = tonicViewport.askSaveFile(
            getattr(usdviewApi, "qMainWindow", None),
            "Export Tonic center curves", "USD (*.usda *.usdc)",
            "centers.usda")
        if path:
            session.exportCenterCurves(path, self._state.activeLevel)

    def _cmd_importCurves(self, usdviewApi):
        from . import tonicViewport
        session = self._session
        if session is None or session.model is None:
            self._status(usdviewApi, "Tonic: bind a scalp first")
            return
        path = tonicViewport.askOpenFile(
            getattr(usdviewApi, "qMainWindow", None),
            "Import curves as Tonic tubes", "USD (*.usda *.usdc *.usd)")
        if path:
            session.importCurves(path, 0)


Tf.Type.Define(UsdGenTonicToolsPluginContainer)
