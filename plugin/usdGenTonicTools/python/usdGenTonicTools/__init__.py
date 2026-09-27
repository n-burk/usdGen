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

import atexit
import math
import os

from pxr import Tf
from pxr.Usdviewq.plugin import PluginContainer

from .tonicToolState import TonicToolState

_CONTAINER = None


def _validMeshPrim(prim):
    """Validate the complete topology contract before model activation."""
    try:
        if not prim or str(prim.GetTypeName()) != "Mesh":
            return False
        points = list(prim.GetAttribute("points").Get() or [])
        rawCounts = list(prim.GetAttribute("faceVertexCounts").Get() or [])
        rawIndices = list(
            prim.GetAttribute("faceVertexIndices").Get() or [])
        counts = []
        indices = []
        for value in rawCounts:
            converted = int(value)
            if float(value) != converted:
                return False
            counts.append(converted)
        for value in rawIndices:
            converted = int(value)
            if float(value) != converted:
                return False
            indices.append(converted)
    except (AttributeError, TypeError, ValueError, RuntimeError, OverflowError):
        return False
    if not points or not counts or not indices:
        return False
    if any(count < 3 for count in counts):
        return False
    if sum(counts) != len(indices):
        return False
    pointCount = len(points)
    for point in points:
        try:
            if len(point) < 3 or not all(
                    math.isfinite(float(point[axis])) for axis in range(3)):
                return False
        except (TypeError, ValueError, IndexError, OverflowError):
            return False
    return all(0 <= index < pointCount for index in indices)


def _scalpTargetError(stage, path):
    """"" when `path` can bind as the scalp, else why not.

    A Mesh must pass the full topology contract; a face GeomSubset must
    resolve (plan/02 section 2.20: elementType "face", directly under a
    Mesh, in-range parent face indices) and its parent must pass it. The
    malformed-Mesh wording is the one the dock has always shown.
    """
    from .tonicSession import resolveScalpTarget
    meshPrim, _faces, error = resolveScalpTarget(stage, path)
    if meshPrim is None:
        return error
    if not _validMeshPrim(meshPrim):
        return ("%s is not a valid scalp mesh. It needs points, faces of "
                "three or more vertices and face indices inside the point "
                "list." % meshPrim.GetPath())
    return ""


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
        # The last whole-stage scan for saved grooms (SS-03), the stage it
        # belongs to, and the notice listener that drops it on a change.
        self._groomScan = None
        self._groomScanKey = None
        self._groomListener = None
        # Whether shutdown() is wired to the application's quit (SS-06).
        self._quitHooked = False
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
        ("bindScalp", "Bind selected as scalp",
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

    def _status(self, usdviewApi, text):
        """One status line: the dock's message area when the dock is open
        (it mirrors to usdview's status bar itself), else the status bar.

        Bind refusals and file-command results used to reach only the
        status bar, which nobody watches while working in the dock
        (DK-05)."""
        if not text:
            return
        sink = getattr(self._workspace, "_onStatus", None)
        if sink is not None:
            try:
                sink(text)
                return
            except RuntimeError:
                pass                # the C++ dock went with a stage reload
        if usdviewApi is None:
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
        self._hookQuit()
        return self._viewport

    def _hookQuit(self):
        """Run shutdown() before the process exits, once per container.

        aboutToQuit is usdview's own quit (the event loop ending, with Qt
        still alive). atexit covers hosts that never run the loop to its
        end -- testusdview closes the windows and returns -- and a quit
        that raised before the signal went out; shutdown() is idempotent,
        so the second call is a no-op.
        """
        if self._quitHooked:
            return
        self._quitHooked = True
        try:
            from pxr.Usdviewq.qt import QtWidgets
            application = QtWidgets.QApplication.instance()
            if application is not None:
                application.aboutToQuit.connect(self.shutdown)
        except (ImportError, AttributeError, RuntimeError):
            pass
        atexit.register(self._shutdownAtExit)

    def shutdown(self):
        """Quit-time teardown: viewport, session, then the library (SS-06).

        The committer and bake workers are C++ threads. Left for the
        DLL's static destructors, they are joined under the Windows loader
        lock at process exit, which hangs usdview on quit; and the
        per-process bake directory (versioned .ptx maps) stays in the temp
        folder because deactivate() never ran. Each step runs even when the
        one before it fails: at quit there is nobody to report to, and a
        half-done teardown is the hang this exists to prevent.
        """
        viewport = self._viewport
        session = self._session
        try:
            if viewport is not None and viewport.installed:
                viewport.uninstall()
        except RuntimeError:
            pass            # its StageView's C++ side is already deleted
        finally:
            try:
                if session is not None:
                    session.deactivate()
            finally:
                if self._groomListener is not None:
                    self._groomListener.Revoke()
                    self._groomListener = None
                self._groomScan = None
                # The ctypes handle pins nothing the OS would not free,
                # but a later ensureSession() must load a fresh one rather
                # than call into workers this teardown destroyed.
                self._state.lib = None

    def _shutdownAtExit(self):
        # atexit prints any exception as a traceback, which every T3's
        # FAIL_REGULAR_EXPRESSION reads as a failure; by now Qt may be
        # half gone, and nothing is left to act on an error anyway.
        try:
            self.shutdown()
        except Exception:
            pass

    # -- the five commands -------------------------------------------------

    def _cmd_openWorkspace(self, usdviewApi):
        viewport = self.ensureViewport(usdviewApi)
        if viewport is None:
            return
        try:
            from . import tonicWorkspace
        except ImportError:
            # No dock to own workspaceOpen: the tool is on screen as soon
            # as it is asked for (install() suspends a closed workspace).
            self._state.workspaceOpen = True
            viewport.resume()
            self._status(usdviewApi,
                         "Tonic: viewport tool active (the workspace dock "
                         "lands with V3); modes on 1-6, Escape cancels")
            return
        self._workspace = tonicWorkspace.openWorkspace(self, usdviewApi)
        # install() leaves the controller suspended until a workspace is on
        # screen; the dock's visibilityChanged resumes it, and this covers a
        # show that emitted nothing (a dock already marked open).
        if self._state.workspaceOpen and viewport.suspended:
            viewport.resume()
        # Everything on the strip is derived from the model, so it goes
        # stale exactly when the viewport does: one hook, every publish.
        self._session.setPublishHook(self.refreshWorkspace)
        # Every session and loop status line lands in the dock's message
        # area (DK-05); the dock mirrors each one to usdview's status bar.
        self._session.setStatusSink(self._workspace._onStatus)
        self.refreshWorkspace()
        self._status(usdviewApi, "Tonic: workspace open")

    def _cmd_bindScalp(self, usdviewApi):
        # UsdviewApi.selectedPaths, not the selection model's own getter:
        # the P2 command asked for getSelectedPrimPaths, which no usdview
        # has (it is getPrimPaths), inside a try/except AttributeError, so
        # "Bind scalp" silently did nothing in a real session.
        paths = list(usdviewApi.selectedPaths)
        if not paths:
            self._status(usdviewApi, "Tonic: select the scalp mesh in the viewport first")
            return
        self.bindScalp(usdviewApi, str(paths[0]))

    def bindScalp(self, usdviewApi, scalpPath, replace=False, resume=False):
        """Compatibility spelling for callers of the original menu action.

        The menu binds without the Resume / Start new question: "Bind
        selected as scalp" says what it does, scripts drive it with no one
        to answer a modal box, and the dock's Resume groom button is the
        artist's way back into a saved groom.
        """
        return self.bindGeometry(usdviewApi, scalpPath, replace=replace,
                                 resume=resume)

    def isValidGeometry(self, usdviewApi, scalpPath):
        """Return whether a stage path satisfies the binding mesh contract."""
        return not self.geometryError(usdviewApi, scalpPath)

    def geometryError(self, usdviewApi, scalpPath):
        """"" when a stage path (Mesh or face GeomSubset) can bind, else why.
        """
        try:
            stage = usdviewApi.stage
        except AttributeError:
            stage = None
        if stage is None:
            return "no stage to bind %s from" % scalpPath
        return _scalpTargetError(stage, str(scalpPath))

    def bindGeometry(self, usdviewApi, scalpPath, replace=False,
                     resume=None):
        """Validate and bind one stage mesh for the Tonic model.

        This is the single binding entry point used by both the legacy
        "Bind selected as scalp" command and the workspace's scalp picker. The
        validation is deliberately before TonicSession.activate: that
        method tears down an existing model before it creates the next one,
        so a bad path must never discard an edited groom. Replacement is
        opt-in because binding a different mesh clears the graph and maps.

        When no model is live and the stage already holds a saved groom
        for this mesh, `resume` decides: None asks the artist (Resume /
        Start new / Cancel), True hydrates the saved groom, False starts a
        new one over it. A cancelled question returns None (SS-03).
        """
        session = self.ensureSession(usdviewApi)
        if session is None:
            return False
        try:
            stage = usdviewApi.stage
            path = str(scalpPath)
        except (AttributeError, TypeError, ValueError, RuntimeError):
            self._status(usdviewApi, "Tonic: no stage mesh at %s" % scalpPath)
            return False
        # This complete topology check must happen before the old model can
        # be deactivated: activate() tears down an existing model first. A
        # face GeomSubset binds its parent Mesh's faces (plan/02 2.20); a
        # bad subset is refused here, by name, never bound as the mesh.
        error = _scalpTargetError(stage, path)
        if error:
            self._status(usdviewApi, "Tonic: %s" % error)
            return False
        current = str(getattr(session, "scalpPath", "") or "")
        if getattr(session, "model", None) is not None:
            if current == path:
                self._status(usdviewApi, "Tonic: %s is already bound" % path)
                return True
            if not replace:
                self._status(
                    usdviewApi,
                    "Tonic: %s is already editing %s; confirm replacement "
                    "in the Tonic workspace" % (current or "a groom", path))
                return False
        # A saved groom for this mesh is the artist's earlier work: binding
        # would silently start over on top of it, so ask (SS-03). Only with
        # no live model -- a live model's own commit is the groom the stage
        # shows, and replacing it has its own confirmation above.
        note = ""
        if getattr(session, "model", None) is None and resume is not False:
            groomPath = self.resumableGroomPath(usdviewApi, path)
            if groomPath:
                if resume is None:
                    resume = self._askResume(usdviewApi, path, groomPath)
                    if resume is None:
                        self._status(usdviewApi, "Tonic: bind cancelled")
                        return None
                if resume:
                    return self.resumeGroom(usdviewApi, path)
        elif getattr(session, "model", None) is None:
            groomPath = self.resumableGroomPath(usdviewApi, path)
            if groomPath:
                note = ("; the saved groom at %s is covered until you save "
                        "over it (Resume groom edits it instead)" % groomPath)
        if not session.activate(path, stage=stage):
            return False
        self._finishBind(usdviewApi, path,
                         "Tonic: bound %s as the scalp%s" % (path, note))
        return True

    def _finishBind(self, usdviewApi, path, text):
        """What a bind and a resume both do once the model is live."""
        viewport = self.ensureViewport(usdviewApi)
        if viewport is not None:
            viewport.setMode("graph")
            # The overlay dots are sized in world units from a pixel
            # target, and the scale that converts them is measured at the
            # scalp: until a scalp is bound there is nothing to measure at,
            # so the first real reading is here (plan/18 section 2.4a).
            viewport.syncDisplayScale()
        if path:
            self._removeBoundMeshFromSelection(usdviewApi, path)
        if viewport is not None and not self._state.workspaceOpen:
            # A menu bind with no dock: the controller stays suspended (the
            # viewport is still usdview's) until the workspace opens.
            text += "; usdGen > Tonic > Open workspace to edit it"
        self._status(usdviewApi, text)
        self.refreshWorkspace()

    # -- Resume a saved groom (SS-03) ----------------------------------------

    GROOM_TYPE = "UsdGenTonicGroom"

    @staticmethod
    def _groomScalp(prim):
        """The scalp a groom prim names, "" when it names none."""
        from .tonicSession import TonicSession
        return TonicSession._scalpPathFromGroom(None, prim)

    def resumableGroomPath(self, usdviewApi, scalpPath=None):
        """The path of a saved groom Resume could hydrate, or "".

        A UsdGenTonicGroom at the default path, else any groom whose
        usdGen:tonic:scalp names `scalpPath` (any groom at all when no
        mesh is given). With a mesh given, a default-path groom that names
        a DIFFERENT scalp does not count: resuming it would bind that mesh
        instead of the one the artist chose.
        """
        from .tonicSession import DEFAULT_GROOM_PATH
        try:
            stage = usdviewApi.stage
        except AttributeError:
            return ""
        if stage is None:
            return ""
        want = str(scalpPath) if scalpPath else ""

        def matches(prim):
            if str(prim.GetTypeName()) != self.GROOM_TYPE:
                return False
            if not want:
                return True
            named = self._groomScalp(prim)
            return not named or named == want

        prim = stage.GetPrimAtPath(DEFAULT_GROOM_PATH)
        if prim and matches(prim):
            return DEFAULT_GROOM_PATH
        for groomPath, named in self._scanGrooms(stage):
            if not want or named == want:
                return groomPath
        return ""

    def _scanGrooms(self, stage):
        """[(groomPath, scalpPath)] for every groom prim on the stage.

        The dock asks canResumeGroom on its 250 ms refresh timer while no
        scalp is bound, and a whole-stage traversal that often would stall
        a big scene, so the scan is kept until the stage changes (any
        ObjectsChanged notice) or a different stage arrives.
        """
        try:
            key = (stage.GetRootLayer().identifier,
                   stage.GetSessionLayer().identifier)
        except (AttributeError, RuntimeError):
            return []
        if self._groomScan is not None and self._groomScanKey == key:
            return self._groomScan
        found = []
        iterator = iter(stage.Traverse())
        for prim in iterator:
            if str(prim.GetTypeName()) != self.GROOM_TYPE:
                continue
            # A groom's children (ScalpGraph, Guides, tubes) are never
            # grooms themselves; skipping them keeps a big groom cheap.
            iterator.PruneChildren()
            found.append((str(prim.GetPath()), self._groomScalp(prim)))
        self._groomScan = found
        self._groomScanKey = key
        self._watchGrooms(stage)
        return found

    def _watchGrooms(self, stage):
        from pxr import Usd
        if self._groomListener is not None:
            self._groomListener.Revoke()
            self._groomListener = None

        # A bound method, never a nested function: Tf keeps only a weak
        # reference to a plain callable, so a local closure died when this
        # returned and every later ObjectsChanged printed "Tried to call an
        # expired python callback" and never dropped the scan. A bound
        # method is held as (function, weak self): it lives exactly as long
        # as this container, and shutdown()/the next watch Revoke it.
        try:
            self._groomListener = Tf.Notice.Register(
                Usd.Notice.ObjectsChanged, self._onGroomStageChanged, stage)
        except (TypeError, RuntimeError):
            self._groomListener = None
            self._groomScanKey = None     # no notices: never trust it

    def _onGroomStageChanged(self, _notice, _sender):
        # Deliberately no inspection: while a model is live the committer
        # swaps a layer per commit, and this runs per swap.
        self._groomScan = None

    def canResumeGroom(self, usdviewApi, scalpPath=None):
        """Whether the dock's Resume groom button has something to do.

        Never while a model is live: the groom the stage shows then is
        that model's own commit.
        """
        session = self._session
        if session is not None and getattr(session, "model", None) is not None:
            return False
        return bool(self.resumableGroomPath(usdviewApi, scalpPath))

    def resumeGroom(self, usdviewApi, scalpPath=None):
        """Hydrate the stage's saved groom into a live model and edit it."""
        session = self.ensureSession(usdviewApi)
        if session is None:
            return False
        if getattr(session, "model", None) is not None:
            self._status(usdviewApi, "Tonic: a groom is already live; save "
                         "it before resuming another")
            return False
        groomPath = self.resumableGroomPath(usdviewApi, scalpPath)
        if not groomPath:
            self._status(usdviewApi, "Tonic: the stage holds no saved groom "
                         "to resume")
            return False
        if not session.hydrate(groomPath=groomPath, stage=usdviewApi.stage):
            return False
        counts = getattr(session, "hydratedCounts", None) or (0, 0, 0)
        self._finishBind(
            usdviewApi, str(getattr(session, "scalpPath", "") or ""),
            "Tonic: resumed %s on %s (%d tube(s), %d guide(s))"
            % (groomPath, session.scalpPath or "its scalp", counts[0],
               counts[1]))
        return True

    def _askResume(self, usdviewApi, path, groomPath):
        """Resume / Start new / Cancel: True, False or None."""
        from pxr.Usdviewq.qt import QtWidgets
        parent = self._workspace
        if parent is None:
            parent = getattr(usdviewApi, "qMainWindow", None)
        box = QtWidgets.QMessageBox(parent)
        box.setObjectName("tonicResumePrompt")
        box.setIcon(QtWidgets.QMessageBox.Question)
        box.setWindowTitle("Saved groom found")
        box.setText("The scene already holds a Tonic groom for %s (%s)."
                    % (path, groomPath))
        box.setInformativeText(
            "Resume edits that groom. Start new begins an empty groom "
            "that covers it in the viewport; the saved file is untouched "
            "until you save over it.")
        resumeButton = box.addButton("Resume",
                                     QtWidgets.QMessageBox.AcceptRole)
        resumeButton.setObjectName("tonicResumeButton")
        newButton = box.addButton("Start new",
                                  QtWidgets.QMessageBox.DestructiveRole)
        newButton.setObjectName("tonicStartNewButton")
        cancelButton = box.addButton(QtWidgets.QMessageBox.Cancel)
        box.setDefaultButton(resumeButton)
        box.setEscapeButton(cancelButton)
        box.exec_()
        clicked = box.clickedButton()
        if clicked is resumeButton:
            return True
        if clicked is newButton:
            return False
        return None

    @staticmethod
    def _removeBoundMeshFromSelection(usdviewApi, path):
        """Remove only the bound mesh from usdview's selection, if exposed."""
        dataModel = getattr(usdviewApi, "dataModel", None)
        if dataModel is None:
            dataModel = getattr(usdviewApi, "_dataModel", None)
        selection = getattr(dataModel, "selection", None)
        remove = getattr(selection, "removePrimPath", None)
        if not callable(remove):
            return
        try:
            remove(str(path))
        except (TypeError, ValueError, RuntimeError):
            # Older usdview builds may require an SdfPath object.
            try:
                from pxr import Sdf
                remove(Sdf.Path(str(path)))
            except (ImportError, TypeError, ValueError, RuntimeError):
                pass

    def _cmd_saveGroom(self, usdviewApi):
        self.saveGroomInteractive(usdviewApi)

    def _cmd_exportCenterCurves(self, usdviewApi):
        self.exportCenterCurvesInteractive(usdviewApi)

    def _cmd_importCurves(self, usdviewApi):
        self.importCurvesInteractive(usdviewApi)

    # -- the one save / export / import path (DK-03) -------------------------
    #
    # Save existed three times (menu, dock, Ctrl+Shift+S) with drifting
    # filters: the dock offered *.usda, which saveGroom refuses with a
    # status-bar line the artist never sees behind a closed dialog, and the
    # dock imported under parent -1 while the menu passed 0. Every route now
    # calls these helpers, which are the only QFileDialog call sites for the
    # three commands. Each returns True/False for the command's result and
    # None when there was nothing to do (no scalp, or the dialog cancelled).

    SETTINGS_ORG = "usdGen"
    SETTINGS_APP = "usdGenTonicTools"
    # An .ini path that replaces the artist's settings store (the registry
    # on Windows). The T3s point it into their temp folder, so a test run,
    # crashed or not, never leaves its dialog folder as the artist's.
    SETTINGS_FILE = None
    _DIALOG_DIR_KEY = "fileDialogDir"

    def _settings(self):
        """The QSettings every remembered Tonic preference goes through."""
        from pxr.Usdviewq.qt import QtCore
        if self.SETTINGS_FILE:
            fmt = getattr(QtCore.QSettings, "IniFormat", None)
            if fmt is None:
                fmt = QtCore.QSettings.Format.IniFormat
            return QtCore.QSettings(str(self.SETTINGS_FILE), fmt)
        return QtCore.QSettings(self.SETTINGS_ORG, self.SETTINGS_APP)

    def _boundSession(self, usdviewApi):
        session = self._session
        if session is None or getattr(session, "model", None) is None:
            self._status(usdviewApi, "Tonic: bind a scalp mesh first")
            return None
        return session

    @staticmethod
    def _dialogParent(usdviewApi, parent):
        if parent is not None:
            return parent
        return getattr(usdviewApi, "qMainWindow", None)

    @staticmethod
    def _rootLayerPath(usdviewApi):
        """The scene's root layer file, or "" for an anonymous stage."""
        try:
            identifier = str(usdviewApi.stage.GetRootLayer().identifier)
        except (AttributeError, RuntimeError):
            return ""
        if not identifier or identifier.startswith("anon:"):
            return ""
        return identifier

    def _sceneStem(self, usdviewApi):
        path = self._rootLayerPath(usdviewApi)
        stem = os.path.splitext(os.path.basename(path))[0] if path else ""
        return stem or "tonic"

    def _dialogDir(self, usdviewApi):
        """The last directory a Tonic file dialog used, else the scene's."""
        try:
            value = self._settings().value(self._DIALOG_DIR_KEY, "")
        except (TypeError, RuntimeError):
            value = ""
        # QSettings hands back None (not the default) for an unset key on
        # some PySide builds, and str(None) is a directory named "None".
        value = str(value) if value else ""
        if value and os.path.isdir(value):
            return value
        scene = self._rootLayerPath(usdviewApi)
        return os.path.dirname(os.path.abspath(scene)) if scene else ""

    def _rememberDir(self, path):
        try:
            self._settings().setValue(
                self._DIALOG_DIR_KEY,
                os.path.dirname(os.path.abspath(path)))
        except (TypeError, RuntimeError):
            pass

    def _askPath(self, usdviewApi, parent, caption, nameFilter, save,
                 suffix="", initialName=""):
        """One modal file dialog; "" when the artist cancels.

        The static getSaveFileName cannot take setDefaultSuffix, so the
        suffix is applied here instead: a bare name typed into the dialog
        ("x") becomes x.<suffix>, exactly what the default suffix does,
        while a name with its own extension is left for the command to
        accept or refuse out loud.
        """
        from pxr.Usdviewq.qt import QtWidgets
        directory = self._dialogDir(usdviewApi)
        if save:
            initial = (os.path.join(directory, initialName) if directory
                       else initialName)
            path, _chosen = QtWidgets.QFileDialog.getSaveFileName(
                parent, caption, initial, nameFilter)
        else:
            path, _chosen = QtWidgets.QFileDialog.getOpenFileName(
                parent, caption, directory, nameFilter)
        path = str(path) if path else ""
        if not path:
            return ""
        if save and suffix and not os.path.splitext(path)[1]:
            path = "%s.%s" % (path, suffix)
        self._rememberDir(path)
        return path

    def _runReported(self, usdviewApi, session, call, suffix=""):
        """Run one session file command; (ok, its last status line).

        The session reports why a save/export/import failed only as a
        status line. The helpers need that line for the warning dialog, so
        the command runs with a recording sink and the lines are forwarded
        to the real one afterwards (the test's recorder or usdview's
        status bar), `suffix` appended to the last one on success.
        """
        if not hasattr(session, "_statusFn"):
            # A session without the sink slot: run it plainly rather than
            # clobber whatever sink it does have.
            return bool(call()), ""
        previous = session._statusFn
        lines = []
        session.setStatusSink(lambda *args: lines.append(args))
        try:
            try:
                ok = bool(call())
            except (RuntimeError, OSError, ValueError) as exc:
                # Sdf/Usd writers raise Tf errors (RuntimeError) for an
                # unwritable path or an unknown file format.
                ok = False
                lines.append(("Tonic: %s" % exc,))
        finally:
            session.setStatusSink(previous)
        if ok and suffix and lines:
            last = lines[-1]
            lines[-1] = (str(last[0]) + suffix,) + tuple(last[1:])
        for args in lines:
            if previous is not None:
                previous(*args)
            else:
                self._status(usdviewApi, str(args[0]))
        return ok, (str(lines[-1][0]) if lines else "")

    def _finishFileCommand(self, parent, title, ok, line):
        if not ok:
            from pxr.Usdviewq.qt import QtWidgets
            QtWidgets.QMessageBox.warning(
                parent, title,
                line or "%s failed; see the usdview console." % title)
        self.refreshWorkspace()
        return ok

    _SAVE_TO_SCENE_KEY = "saveGroomToScene"

    def saveGroomToScene(self):
        """Whether Save also adds the groom to the scene's root layer.

        Off by default: adding it writes the scene file itself. On, a
        saved groom survives File > Reopen and a new usdview session, and
        the dock's Resume groom picks it up (SS-03). Remembered in the
        artist's QSettings like the dialog directory.
        """
        try:
            value = self._settings().value(self._SAVE_TO_SCENE_KEY, False)
        except (TypeError, RuntimeError):
            return False
        # The INI/registry backends hand booleans back as "true"/"false".
        if isinstance(value, str):
            return value.strip().lower() in ("1", "true", "yes")
        return bool(value)

    def setSaveGroomToScene(self, enabled):
        try:
            self._settings().setValue(self._SAVE_TO_SCENE_KEY, bool(enabled))
        except (TypeError, RuntimeError):
            pass

    @staticmethod
    def _saveTick():
        """Keep usdview painting while a save waits for the committer.

        User input is held back: a click landing mid-save could start a
        gesture on the model the save is copying.
        """
        from pxr.Usdviewq.qt import QtCore, QtWidgets
        QtWidgets.QApplication.processEvents(
            QtCore.QEventLoop.ExcludeUserInputEvents, 20)

    def saveGroomInteractive(self, usdviewApi, parent=None,
                             addToRootLayer=None):
        """Ask for a .usdc and save the live groom into it.

        `addToRootLayer` None follows saveGroomToScene(); True/False
        override it for this one save.
        """
        from pxr.Usdviewq.qt import QtCore, QtWidgets
        session = self._boundSession(usdviewApi)
        if session is None:
            return None
        if addToRootLayer is None:
            addToRootLayer = self.saveGroomToScene()
        title = "Save Tonic groom"
        parent = self._dialogParent(usdviewApi, parent)
        path = self._askPath(usdviewApi, parent, title, "USD crate (*.usdc)",
                             True, "usdc",
                             "%s-groom.usdc" % self._sceneStem(usdviewApi))
        if not path:
            return None

        def save():
            return session.saveGroom(path, addToRootLayer=addToRootLayer,
                                     tick=self._saveTick)

        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        try:
            ok, line = self._runReported(usdviewApi, session, save)
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()
        return self._finishFileCommand(parent, title, ok, line)

    def exportCenterCurvesInteractive(self, usdviewApi, parent=None):
        """Ask for a file and export the focused level's center curves."""
        session = self._boundSession(usdviewApi)
        if session is None:
            return None
        level = int(getattr(self._state, "activeLevel", 0) or 0)
        # Level 0 is "every tube" to exportCenterCurves, not a level.
        title = ("Export level %d center curves" % level if level > 0
                 else "Export all center curves")
        parent = self._dialogParent(usdviewApi, parent)
        path = self._askPath(usdviewApi, parent, title,
                             "USD (*.usda *.usdc)", True, "usda",
                             "%s-centers.usda" % self._sceneStem(usdviewApi))
        if not path:
            return None
        ok, line = self._runReported(
            usdviewApi, session,
            lambda: session.exportCenterCurves(path, level))
        return self._finishFileCommand(parent, title, ok, line)

    def importParentTubeId(self):
        """The tube imports land under: the first selected tube, else 0.

        Never -1: the dock used to pass -1 for "nothing selected", which
        the model rejects as an unknown parent, while the menu passed 0
        (the root tube). 0 is the root tube, so both now agree.
        """
        from . import tonicPanels
        session = self._session
        if session is None or getattr(session, "model", None) is None:
            return 0
        try:
            ids = tonicPanels.selectedTubeIds(session)
        except (AttributeError, TypeError, ValueError, RuntimeError):
            ids = []
        ids = [int(tube) for tube in ids or () if int(tube) >= 0]
        return ids[0] if ids else 0

    def importCurvesInteractive(self, usdviewApi, parent=None):
        """Ask for a USD file and import its curves under the selection."""
        session = self._boundSession(usdviewApi)
        if session is None:
            return None
        parentTubeId = self.importParentTubeId()
        title = ("Import curves under tube %d" % parentTubeId
                 if parentTubeId > 0
                 else "Import curves under the root tube (tube 0)")
        parent = self._dialogParent(usdviewApi, parent)
        path = self._askPath(usdviewApi, parent, title,
                             "USD (*.usda *.usdc *.usd)", False)
        if not path:
            return None
        # No suffix: the session's own line already names the parent
        # ("Imported N curves under tube M", SS-04).
        ok, line = self._runReported(
            usdviewApi, session,
            lambda: session.importCurves(path, parentTubeId))
        return self._finishFileCommand(parent, title, ok, line)


Tf.Type.Define(UsdGenTonicToolsPluginContainer)
