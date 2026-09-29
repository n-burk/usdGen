# usdGenPomadeTools.pomadeSession -- model, committer and bake lifetime
# (plan/18 section 3.1).
#
# Qt-free: pure ctypes over pomadeLib/pomadeLibStage plus pxr for the live
# sublayer and the file commands. Everything the viewport controller and the
# workspace dock do to the model goes through here, so there is one place
# that knows a model exists, one place that publishes it, and one place that
# swaps a committed layer in.
#
# Three rules this module exists to keep:
#   * Pomade_Publish is the ONLY way the viewport learns about a change
#     (plan/18 section 2.1), so every mutating helper here publishes.
#   * the stage is never touched from inside a gesture (plan/17 section 1.3):
#     release enqueues, the idle pump swaps.
#   * the gesture bracket is one undo step (plan/18 section 3.2), so a
#     200-sample drag pushes one snapshot instead of 200.
from __future__ import annotations

import ctypes
import inspect
import os
import shutil
import tempfile
import time

from . import pomadeLib

# What Pomade_Publish is asked for outside a gesture. Inside one the session
# passes POMADE_DIRTY_PENDING so only the leaf locators the model marked are
# republished -- that precision is the whole point of the V0 dirty bitmask,
# and asking for everything on every move would spend the move budget on
# levels nobody touched.
PUBLISH_PENDING = pomadeLib.POMADE_DIRTY_PENDING
PUBLISH_ALL = pomadeLib.POMADE_DIRTY_ALL

DEFAULT_GROOM_PATH = "/PomadeGroom"


def bakeDirectory():
    """Where this process's bake worker writes its versioned maps.

    Per PROCESS, not per box. The worker sweeps stale `<base>.v<n>.ptx`
    files out of its directory (plan/18 section 7 G5) and map versions
    restart at 1 in every model, so two Pomade processes that shared one
    directory would delete each other's live map: two usdviews on one
    workstation, or two T3 tests whose bake threads overlap while the
    first process is still tearing down. USDGENPOMADE_BAKE_DIR overrides
    it outright, which is how a test pins the directory it will inspect.
    """
    override = os.environ.get("USDGENPOMADE_BAKE_DIR")
    if override:
        return override
    base = os.path.join(tempfile.gettempdir(), "usdGenPomadeBake")
    _sweepStaleBakeDirectories(base)
    return os.path.join(base, "p%d" % os.getpid())


def _pidAlive(pid):
    """Whether process `pid` still runs; True when the answer is unknown.

    Unknown counts as alive: sweeping a live process's directory would
    delete the map its stage is reading, and a leftover directory costs
    only disk.
    """
    if pid <= 0:
        return False
    if pid == os.getpid():
        return True
    if os.name == "nt":
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL,
                                       wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        queryLimited = 0x1000        # PROCESS_QUERY_LIMITED_INFORMATION
        handle = kernel.OpenProcess(queryLimited, False, int(pid))
        if not handle:
            # ERROR_ACCESS_DENIED means the process exists and belongs to
            # someone else; ERROR_INVALID_PARAMETER means no such pid.
            return ctypes.get_last_error() == 5
        try:
            code = wintypes.DWORD(0)
            if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
                return True
            return code.value == 259  # STILL_ACTIVE
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(int(pid), 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True                   # EPERM: alive, not ours
    return True


def _sweepStaleBakeDirectories(base):
    """Remove `p<pid>` bake directories whose process is gone (SS-06).

    A crashed or killed usdview never runs deactivate(), so its per-process
    directory -- versioned .ptx maps, easily hundreds of MB on a dense
    scalp -- would stay in %TEMP% forever. Each new session sweeps the
    ones whose owner is dead; live owners are left alone.
    """
    try:
        names = os.listdir(base)
    except OSError:
        return
    for name in names:
        if not (name.startswith("p") and name[1:].isdigit()):
            continue
        if _pidAlive(int(name[1:])):
            continue
        shutil.rmtree(os.path.join(base, name), ignore_errors=True)


def _meshArrays(points, counts, indices):
    """numpy (flat float32 points, int32 counts, int32 indices, centre).

    None when numpy is missing or the arrays are not the shape a mesh's
    are (a Vt array without the buffer protocol converts to an object
    array); the caller then takes the list path.
    """
    try:
        import numpy
    except ImportError:
        return None
    try:
        xyz = numpy.asarray(points, dtype=numpy.float32)
        cnt = numpy.ascontiguousarray(counts, dtype=numpy.int32)
        idx = numpy.ascontiguousarray(indices, dtype=numpy.int32)
    except (TypeError, ValueError):
        return None
    if xyz.ndim != 2 or xyz.shape[1] < 3 or cnt.ndim != 1 or idx.ndim != 1:
        return None
    xyz = numpy.ascontiguousarray(xyz[:, :3])
    lo = xyz.min(axis=0)
    hi = xyz.max(axis=0)
    centre = tuple(float(0.5 * (lo[a] + hi[a])) for a in range(3))
    return xyz.reshape(-1), cnt, idx, centre


def resolveScalpTarget(stage, path):
    """(meshPrim, faces, error) for the scalp prim at `path` (plan/02 2.20).

    A Mesh binds whole: `faces` is None. A GeomSubset whose elementType is
    "face" and whose parent is a Mesh binds that parent's geometry
    restricted to the subset's `indices` -- PARENT-mesh face ids, returned
    sorted and unique. A subset never renumbers faces: geometry, primvars
    and the rest binding all stay on the parent. Anything else returns
    (None, None, reason) -- a non-face subset, a subset outside a Mesh, an
    empty or out-of-range index list is an error, never a silent bind of
    the whole mesh. The one resolver every bind and bind validation uses;
    PomadeResolveScalpTarget (pomadeCommit.h) is its hydrate-side twin.
    """
    from pxr import UsdGeom
    path = str(path)
    try:
        prim = stage.GetPrimAtPath(path)
    except (AttributeError, TypeError, ValueError, RuntimeError):
        prim = None
    if not prim:
        return None, None, "no prim at %s" % path
    if prim.IsA(UsdGeom.Mesh):
        return prim, None, ""
    if not prim.IsA(UsdGeom.Subset):
        return None, None, ("%s is a %s, not a Mesh or a face GeomSubset"
                            % (path, prim.GetTypeName() or "typeless prim"))
    subset = UsdGeom.Subset(prim)
    elementType = str(subset.GetElementTypeAttr().Get() or "")
    if elementType != "face":
        return None, None, ('GeomSubset %s has elementType "%s"; a scalp '
                            'subset must be "face"' % (path, elementType))
    parent = prim.GetParent()
    if not parent or not parent.IsA(UsdGeom.Mesh):
        return None, None, ("GeomSubset %s is not a child of a Mesh"
                            % path)
    try:
        faces = sorted(set(
            int(i) for i in (subset.GetIndicesAttr().Get() or [])))
    except (TypeError, ValueError, OverflowError):
        return None, None, "GeomSubset %s has unreadable indices" % path
    if not faces:
        return None, None, "GeomSubset %s names no faces" % path
    faceCount = len(parent.GetAttribute("faceVertexCounts").Get() or [])
    for face in (faces[0], faces[-1]):
        if face < 0 or face >= faceCount:
            return None, None, ("GeomSubset %s names face %d, but %s has "
                                "%d faces" % (path, face, parent.GetPath(),
                                              faceCount))
    return parent, faces, ""


def _facesCentre(flat, counts, indices, faces):
    """Bounding-box centre of the vertices `faces` use, or None.

    A face subset scalp frames on its own faces, not on the whole parent.
    """
    try:
        import numpy
    except ImportError:
        numpy = None
    try:
        if numpy is not None and hasattr(flat, "reshape"):
            cnt = numpy.asarray(counts, dtype=numpy.int64)
            keep = numpy.zeros(len(cnt), dtype=bool)
            keep[numpy.asarray(faces, dtype=numpy.int64)] = True
            corners = keep[numpy.repeat(numpy.arange(len(cnt)), cnt)]
            verts = numpy.asarray(indices)[corners]
            if not len(verts):
                return None
            xyz = numpy.asarray(flat).reshape(-1, 3)[verts]
            lo = xyz.min(axis=0)
            hi = xyz.max(axis=0)
            return tuple(float(0.5 * (lo[a] + hi[a])) for a in range(3))
        offsets = [0]
        for count in counts:
            offsets.append(offsets[-1] + int(count))
        verts = set()
        for face in faces:
            verts.update(int(i) for i in
                         indices[offsets[face]:offsets[face + 1]])
        if not verts:
            return None
        return tuple(0.5 * (min(flat[v * 3 + a] for v in verts) +
                            max(flat[v * 3 + a] for v in verts))
                     for a in range(3))
    except (IndexError, TypeError, ValueError):
        return None


def _cArray(values, ctype):
    """A ctypes argument over `values` (a numpy array or a list).

    numpy memory is borrowed, not copied: the caller keeps `values`
    alive for the duration of the call.
    """
    if hasattr(values, "ctypes"):
        return values.ctypes.data_as(ctypes.POINTER(ctype))
    return (ctype * len(values))(*values)


def _takesLevel(sink):
    """Whether a status sink accepts a second (level) argument.

    Older sinks -- `list.append`, usdview's PrintStatus, most T3
    recorders -- take the text alone and would raise on a second
    argument; the dock's _onStatus colours by level. A builtin with no
    introspectable signature is treated as text-only.
    """
    try:
        params = inspect.signature(sink).parameters.values()
    except (TypeError, ValueError):
        return False
    positional = 0
    for param in params:
        if param.kind == param.VAR_POSITIONAL:
            return True
        if param.kind in (param.POSITIONAL_ONLY,
                          param.POSITIONAL_OR_KEYWORD):
            positional += 1
    return positional >= 2


class _StatusSink:
    """A registered sink, called with `(text, level)` or `(text)` alone.

    Stored as the session's _statusFn so callers that borrow and forward
    it (the dock's file-command recorder calls `previous(*args)` with
    whatever the session sent) can always pass the level along, whatever
    the sink underneath accepts. Compares equal to the sink it wraps.
    """

    def __init__(self, sink):
        self.sink = sink
        self._withLevel = _takesLevel(sink)

    def __call__(self, text, level="info"):
        if self._withLevel:
            self.sink(text, level)
        else:
            self.sink(text)

    def __eq__(self, other):
        if isinstance(other, _StatusSink):
            other = other.sink
        return self.sink == other

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash(self.sink)


def _wrapSink(sink):
    if sink is None or isinstance(sink, _StatusSink):
        return sink
    return _StatusSink(sink)


def _errorText(exc, lastError=""):
    """One readable line for a failed file command's status.

    A Tf.ErrorException reads "\\n\\tError in 'Fn' at line N in file F :
    'what went wrong'"; the artist wants only the quoted part. The
    bridge's ctypes wrappers raise "Pomade_X failed with code N", which
    says nothing without the library's lastError(), so that is appended
    -- to that kind of error only: the library never clears its last
    error, so on a pxr or validation error it would describe some older
    failure.
    """
    text = str(exc).strip()
    marker = text.rfind(" : '")
    if marker >= 0:
        text = text[marker + 4:].rstrip().rstrip("'")
    text = " ".join(text.split())
    # The bridge prefixes its own module name; the artist needs the reason.
    if text.startswith("pomadeBridge: "):
        text = text[len("pomadeBridge: "):]
    if lastError and "failed with code" in text and lastError not in text:
        text = "%s (%s)" % (text, lastError) if text else lastError
    return text or exc.__class__.__name__


class PomadeSession:
    """The live Pomade model plus its commit and bake workers.

    `state` is the shared PomadeToolState; `usdviewApi` may be None, which is
    how the headless T0 test drives a session with a fake library.
    """

    def __init__(self, state, usdviewApi=None, statusFn=None):
        self._state = state
        self._api = usdviewApi
        self._statusFn = _wrapSink(statusFn)
        self._model = None
        self._committer = None
        self._bake = None
        self._stage = None
        # The anonymous live layer must stay referenced here: the session
        # layer holds it only by identifier, and SdfLayer::Find (which
        # Pomade_CommitterSwap uses) can only find a layer that is alive.
        self._liveLayer = None
        self._liveId = ""
        self._groomPath = DEFAULT_GROOM_PATH
        self._descPath = ""
        self._scalpPath = ""
        # The Mesh behind _scalpPath: the same path, or a face GeomSubset's
        # parent (plan/02 section 2.20). Recorded whenever the scalp is
        # read (bind, hydrate, reattach).
        self._scalpMeshPath = ""
        self._mapPath = DEFAULT_GROOM_PATH + "/RegionMap"
        self._mapFile = ""
        self._detached = False
        # The scalp path a reattach could not find in the new stage, or "".
        # Set only by reattach(); the session then stays detached until the
        # artist binds a scalp again (SS-01).
        self._scalpMissing = ""
        # (tubes, guides, imported) the last hydrate() restored; the file
        # commands' tests compare a reopened save against it.
        self._hydratedCounts = (0, 0, 0)
        self._gestureDepth = 0
        self._lastSwapCode = pomadeLib.POMADE_COMMITTER_NOTHING_PENDING
        # Why the latest commit did not reach the stage, or "" (SS-05): a
        # worker build that failed, an enqueue the artist-owned-output
        # guard refused, or a swap into a live layer that is gone. The
        # sync pill and the warnings list read it from status().
        self._commitError = ""
        self._bakeInFlight = False
        self._bakeDir = ""
        # The bake options this session has asked the worker for. `texels`
        # is the Output panel's per-face resolution override in texels per
        # side (0 = the bake's own area-driven plan); `levels` is the
        # channel count, and one level is the only count anything asks for
        # today.
        self._bakeTexels = 0
        self._bakeLevels = 1
        self._pendingOutputReveal = False
        # Called after every publish that reached the indices, in the order
        # they were added (SS-06). A list, not one slot: the dock's refresh
        # and a test's counter (or a second view) used to overwrite each
        # other. _publishHook is the one entry setPublishHook() owns.
        self._publishListeners = []
        self._publishHook = None
        # The viewport owns the Qt timer; these callbacks are the Qt-free
        # wake seam used whenever a worker item is queued from a dock
        # action. _idleHook is the entry setIdleHook() owns.
        self._idleListeners = []
        self._idleHook = None
        self._stageLib = None
        # Centre of the bound scalp's bounding box, in world units. The
        # viewport controller measures the display scale (world units per
        # screen pixel) there, so the overlay dots are the pixel size
        # plan/18 section 2.4a asks for. One point for the whole groom, not
        # one per dot: a per-dot depth would cost a projection per CV and
        # would make neighbouring dots different sizes.
        self._scalpCenter = None

    # -- accessors ---------------------------------------------------------

    @property
    def model(self):
        return self._model

    # The C ABI calls the model handle a context, and the panels/HUD read it
    # under that name; one object, two spellings, no second source of truth.
    @property
    def ctx(self):
        return self._model

    @property
    def committer(self):
        return self._committer

    @property
    def bake(self):
        return self._bake

    @property
    def state(self):
        return self._state

    @property
    def groomPath(self):
        return self._groomPath

    @property
    def scalpPath(self):
        return self._scalpPath

    @property
    def scalpMeshPath(self):
        """The Mesh carrying the scalp: scalpPath, or a face subset's parent."""
        return self._scalpMeshPath or self._scalpPath

    @property
    def scalpCenter(self):
        """Centre of the bound scalp, or None before a scalp is bound."""
        return self._scalpCenter

    def setDisplayScale(self, worldPerPixel):
        """World units per screen pixel at the groom (plan/18 §2.4a).

        Storm sizes points and curves in world units, so this is what
        turns the publisher's pixel targets into widths. Returns True when
        the model took it.
        """
        if self._model is None:
            return False
        try:
            value = float(worldPerPixel)
        except (TypeError, ValueError):
            return False
        if not (value >= 0.0) or value > 1e30:
            return False
        return self.dll.Pomade_SetDisplayScale(
            self._model, ctypes.c_float(value)) == pomadeLib.POMADE_OK

    def displayScale(self):
        """What the model currently scales its overlays by; 0.0 if unset."""
        if self._model is None:
            return 0.0
        out = ctypes.c_float(0.0)
        if self.dll.Pomade_GetDisplayScale(
                self._model, ctypes.byref(out)) != pomadeLib.POMADE_OK:
            return 0.0
        return float(out.value)

    @property
    def liveLayerId(self):
        return self._liveId

    @property
    def committerDetached(self):
        """True while the committer is idled because the stage went away.

        `detached` is the same fact under the name the status dict has
        always used; the workspace reads one, this module reads the other,
        and both come from the single _detached flag.
        """
        return self._detached

    @property
    def detached(self):
        return self._detached

    @property
    def scalpMissing(self):
        """The scalp path the last reattach could not find, or ""."""
        return self._scalpMissing

    @property
    def hydratedCounts(self):
        """(tubes, guides, imported) the last hydrate() brought back."""
        return self._hydratedCounts

    @property
    def gestureActive(self):
        return self._gestureDepth > 0

    @property
    def dll(self):
        lib = self._state.lib
        return lib.dll if lib is not None else None

    @property
    def stageLib(self):
        """The pomadeLibStage binding over the same DLL, bound on demand.

        The per-tube half of the ABI (plan/18 section 7 G2) lives in
        pomadeApiStage.h, so the Tube and Fill loops and their panels need
        it; binding it here keeps one StageLibrary per session instead of
        one per caller. None before the library loads.
        """
        if self._stageLib is None and self._state.lib is not None:
            from . import pomadeLibStage
            self._stageLib = pomadeLibStage.StageLibrary(self._state.lib)
        return self._stageLib

    def report(self, text, level="info"):
        """One status line to usdview (or to the test's recorder).

        `level` is 'info', 'warning' or 'error'; the dock colours by it.
        """
        self._status(text, level)

    def setStatusSink(self, sink):
        """Send status lines to `sink` instead of usdview's status bar.

        usdview's PrintStatus goes to a widget; a test needs the text, and
        so will the workspace's message area. A sink that takes two
        arguments is called with `(text, level)`, any other with the text
        alone (SS-02), so every existing recorder keeps working.
        """
        self._statusFn = _wrapSink(sink)

    def addPublishListener(self, listener):
        """Call `listener()` after every publish that reached the indices.

        The workspace dock wires its refresh here (plan/18 section 3.5):
        the status strip and the warnings are derived from the model, so
        they go stale exactly when the viewport would. Adding the same
        callable twice registers it once.
        """
        if listener is not None and listener not in self._publishListeners:
            self._publishListeners.append(listener)

    def removePublishListener(self, listener):
        """Stop calling `listener`; a listener never added is ignored."""
        if listener in self._publishListeners:
            self._publishListeners.remove(listener)

    def setPublishHook(self, hook):
        """Replace the listener a previous setPublishHook() installed.

        The one-slot spelling the dock and older tests use; listeners
        added with addPublishListener() are left alone. None removes it.
        """
        self._publishHook = self._swapListener(self._publishListeners,
                                               self._publishHook, hook)

    def addIdleListener(self, listener):
        """Call `listener()` whenever deferred work is queued.

        The session stays Qt-free: the viewport supplies ``scheduleIdle``
        once its timer is ready and removes it before teardown.
        """
        if listener is not None and listener not in self._idleListeners:
            self._idleListeners.append(listener)

    def removeIdleListener(self, listener):
        if listener in self._idleListeners:
            self._idleListeners.remove(listener)

    def setIdleHook(self, hook):
        """Replace the idle listener a previous setIdleHook() installed."""
        self._idleHook = self._swapListener(self._idleListeners,
                                            self._idleHook, hook)

    @staticmethod
    def _swapListener(listeners, previous, hook):
        if previous is not None and previous in listeners:
            listeners.remove(previous)
        if hook is not None and hook not in listeners:
            listeners.append(hook)
        return hook

    @staticmethod
    def _notify(listeners):
        # A snapshot: a listener may remove itself (or add another) while
        # it runs, and every listener registered at the call still fires.
        for listener in list(listeners):
            listener()

    def _wakeIdle(self):
        self._notify(self._idleListeners)

    def _status(self, text, level="info"):
        if self._statusFn is not None:
            self._statusFn(text, level)
            return
        api = self._api
        if api is None:
            return
        try:
            api.PrintStatus(text)
        except AttributeError:
            pass

    def lastError(self):
        lib = self._state.lib
        return lib.lastError() if lib is not None else ""

    # -- lifetime ----------------------------------------------------------

    def _ensureLibrary(self):
        if self._state.lib is None:
            self._state.lib = pomadeLib.Library()
        return self._state.lib

    def _createModel(self):
        dll = self.dll
        model = ctypes.c_void_p(None)
        if dll.Pomade_Create(ctypes.byref(model)) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return None
        return model

    def _startWorkers(self, stage):
        """Committer + live sublayer + bake worker. True on success."""
        from pxr import Sdf
        dll = self.dll
        committer = ctypes.c_void_p(None)
        desc = self._descPath.encode("utf-8") if self._descPath else None
        if dll.Pomade_CommitterCreate(self._model,
                                     self._groomPath.encode("utf-8"),
                                     desc,
                                     ctypes.byref(committer)) \
                != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return False
        self._committer = committer
        if self._scalpPath:
            # A face GeomSubset scalp links the subset but puts its rest
            # binding and live primvar on the parent Mesh (plan/02 2.20).
            meshPath = self._scalpMeshPath \
                if self._scalpMeshPath != self._scalpPath else ""
            if dll.Pomade_CommitterSetScalpTarget(
                    committer, self._scalpPath.encode("utf-8"),
                    meshPath.encode("utf-8") if meshPath else None) \
                    != pomadeLib.POMADE_OK:
                self._status("Pomade: " + self.lastError(), "error")
        # The model needs the groom path too, for one reason that has
        # nothing to do with committing: the scene index hides the
        # committed <groom>/Guides prim while this model is live
        # (plan/18 section 2.4a). That prim is the PREVIOUS commit's
        # curves -- plain white BasisCurves with no displayColor -- and
        # Storm draws them straight over the tubes the tool is editing.
        # It is a Hydra visibility opinion only, so the amplifier still
        # reads them off the stage and no cook is starved.
        setGroom = getattr(dll, "Pomade_SetGroomPath", None)
        if setGroom is not None:
            setGroom(self._model, self._groomPath.encode("utf-8"))
        self._liveLayer = Sdf.Layer.CreateAnonymous("usdGenPomade-live")
        self._liveId = self._liveLayer.identifier
        self._stage = stage
        if stage is not None:
            # Strongest sublayer of the session layer: the tool edits here
            # and a saved groom slots in underneath (plan/17 section 3.2).
            # Sdf.Layer has no InsertSubLayerPath in Python -- subLayerPaths
            # is a list proxy -- which is why the P2 controller's install
            # threw the moment it was reached.
            stage.GetSessionLayer().subLayerPaths.insert(0, self._liveId)
        bake = ctypes.c_void_p(None)
        outDir = bakeDirectory()
        if dll.Pomade_BakeCreate(self._model, outDir.encode("utf-8"), None,
                                ctypes.byref(bake)) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError(), "error")
            # Undo this call's half-start (SS-06): a committer thread with
            # no bake worker, and a live sublayer the stage would keep
            # composing after the tool gave up, are both worse than no
            # session at all.
            dll.Pomade_CommitterDestroy(committer)
            self._committer = None
            self._removeLiveSublayer()
            self._liveLayer = None
            self._liveId = ""
            return False
        self._bake = bake
        self._bakeDir = outDir
        # A resolution the artist chose before binding a scalp belongs to
        # the next bake context too.
        self._applyBakeOptions()
        return True

    def activate(self, scalpPath, groomPath=DEFAULT_GROOM_PATH, descPath="",
                 stage=None):
        """Create the model, bind the stage scalp, start commit + bake."""
        self._ensureLibrary()
        if self._model is not None:
            self.deactivate()
        stage = stage if stage is not None else self._apiStage()
        model = self._createModel()
        if model is None:
            return False
        self._model = model
        self._groomPath = groomPath or DEFAULT_GROOM_PATH
        self._descPath = descPath or ""
        self._scalpPath = str(scalpPath)
        self._scalpMeshPath = ""
        self._mapPath = self._groomPath + "/RegionMap"
        # Every failure from here on tears the half-built session down
        # (SS-06): a model with no committer, or a committer and live
        # sublayer with no bake worker, would otherwise stay behind with
        # `model` set, and the dock would offer tools over it.
        if not self.bindScalpFromStage(self._scalpPath, stage):
            self.deactivate()
            return False
        dll = self.dll
        dll.Pomade_SetSnapRadius(model, ctypes.c_float(0.05))
        dll.Pomade_SetMirrorX(model, 1 if self._state.mirrorX else 0)
        if not self._startWorkers(stage):
            self.deactivate()
            return False
        if dll.Pomade_Activate(model) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            self.deactivate()
            return False
        self._state.activated = True
        self._state.groomRoot = self._groomPath
        self.outputSettings()
        self.publish(PUBLISH_ALL)
        return True

    def hydrate(self, groomPath=DEFAULT_GROOM_PATH, layerOrStage="",
                stage=None):
        """Open a groom the stage already holds into a fresh live model.

        This is the path for an artist who opens a saved `.usdc` and wants
        to keep editing it: the whole hierarchy comes back through
        Pomade_Hydrate (plan/18 section 7 G3), the scalp binding is re-made
        from the stage, and the committer starts from the hydrated version.
        """
        from . import pomadeLibStage
        self._ensureLibrary()
        stage = stage if stage is not None else self._apiStage()
        if stage is None:
            self._status("Pomade: no stage to hydrate from")
            return False
        prim = stage.GetPrimAtPath(str(groomPath))
        if not prim:
            self._status("Pomade: no groom at %s" % groomPath, "error")
            return False
        model = self._createModel()
        if model is None:
            return False
        stageLib = pomadeLibStage.StageLibrary(self._state.lib)
        # Hydrate into the new model BEFORE tearing the old one down: the
        # old model's live layer may be all the stage holds of this groom,
        # and deactivate() takes that layer out of the session layer.
        try:
            if layerOrStage:
                tubes, guides, imported = stageLib.hydrate(
                    model, str(layerOrStage), str(groomPath))
            elif stageLib.canHydrateFromLayers():
                # What usdview shows is the root layer composed under the
                # session layer. A groom saved beside the live overlay is
                # only in the session layer's stack, which a stage opened
                # on the root layer alone never sees (SS-03).
                tubes, guides, imported = stageLib.hydrateFromLayers(
                    model, [stage.GetRootLayer().identifier,
                            stage.GetSessionLayer().identifier],
                    str(groomPath))
            else:
                tubes, guides, imported = stageLib.hydrate(
                    model, stage.GetRootLayer().identifier, str(groomPath))
        except RuntimeError as exc:
            self._status("Pomade: %s" % exc, "error")
            self.dll.Pomade_Destroy(model)
            return False
        # Read while the prim is still composed (see the ordering note).
        scalpPath = self._scalpPathFromGroom(stage, prim)
        if self._model is not None:
            self.deactivate()
        self._model = model
        self._groomPath = str(groomPath)
        self._mapPath = self._groomPath + "/RegionMap"
        self._hydratedCounts = (int(tubes), int(guides), int(imported))
        # Pomade_Hydrate has ALREADY bound the scalp and restored the scalp
        # graph, its region loops and the per-face region ids from the
        # committed ScalpGraph prim. Re-binding here would throw all of
        # that away -- PomadeModel::BindScalp clears _graph, _maps and
        # _loops on every call -- and it is why a hydrated groom used to
        # come up with a uniformly dark-red scalp: every face rasterised
        # as uncovered because there were no region loops left to cover
        # it. The path is recorded for the committer and nothing else.
        if scalpPath:
            self._scalpPath = scalpPath
            self._scalpMeshPath = ""
            self.recordScalpCenter(scalpPath, stage)
        # From here the new model is the session's: a failure tears it
        # down whole, as activate() does (SS-06).
        if not self._startWorkers(stage):
            self.deactivate()
            return False
        if self.dll.Pomade_Activate(model) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            self.deactivate()
            return False
        self._state.activated = True
        self._state.groomRoot = self._groomPath
        # Hydrate restores native Output settings from the committed groom;
        # mirror them before the Output panel is rebuilt.
        self.outputSettings()
        self.publish(PUBLISH_ALL)
        self._status("Pomade: hydrated %d tube(s), %d guide(s), %d imported"
                     % (tubes, guides, imported))
        return True

    @staticmethod
    def _scalpPathFromGroom(stage, groomPrim):
        """The scalp the committed groom points at, or ""."""
        for name in ("usdGen:pomade:scalp", "usdGen:scalp"):
            rel = groomPrim.GetRelationship(name)
            if rel:
                targets = rel.GetTargets()
                if targets:
                    return str(targets[0])
        return ""

    def _apiStage(self):
        api = self._api
        if api is None:
            return None
        try:
            return api.stage
        except AttributeError:
            return None

    def _scalpMesh(self, scalpPath, stage):
        """(flatPoints, counts, indices, centre, faces, meshPath) or None.

        The three arrays are numpy float32/int32 when numpy imports, else
        plain lists; _cArray() turns either into what Pomade_BindScalp
        takes. Per-element Python loops over the Vt arrays made a 100k-face
        scalp take seconds to bind (SS-06); numpy reads the Vt buffers in
        one copy each.

        `scalpPath` may name a face GeomSubset (resolveScalpTarget): the
        arrays are then its parent Mesh's, `faces` its parent-mesh face ids
        and `centre` the centre of those faces; `faces` is None for a Mesh.
        `meshPath` is the Mesh the arrays came from.
        """
        prim, faces, error = resolveScalpTarget(stage, scalpPath)
        if prim is None:
            self._status("Pomade: %s" % error, "error")
            return None
        meshPath = str(prim.GetPath())
        points = prim.GetAttribute("points").Get() or []
        counts = prim.GetAttribute("faceVertexCounts").Get() or []
        indices = prim.GetAttribute("faceVertexIndices").Get() or []
        if not len(points) or not len(counts):
            self._status("Pomade: %s is not a mesh with points" % meshPath)
            return None
        arrays = _meshArrays(points, counts, indices)
        if arrays is not None:
            flat, counts, indices, centre = arrays
        else:
            flat = [float(c) for p in points for c in (p[0], p[1], p[2])]
            centre = tuple(
                0.5 * (min(flat[a::3]) + max(flat[a::3])) for a in range(3))
            counts = [int(c) for c in counts]
            indices = [int(i) for i in indices]
        if faces is not None:
            centre = _facesCentre(flat, counts, indices, faces) or centre
        return (flat, counts, indices, centre, faces, meshPath)

    def recordScalpCenter(self, scalpPath, stage=None):
        """Note the scalp's bounding-box centre without touching the model.

        The centre is what the camera frames on; binding is what clears the
        scalp graph. Hydrate needs the first and must not do the second.
        The Mesh behind a face subset scalp is recorded with it, for the
        committer's rest binding.
        """
        stage = stage if stage is not None else self._apiStage()
        if stage is None:
            return False
        mesh = self._scalpMesh(scalpPath, stage)
        if mesh is None:
            return False
        self._scalpCenter = mesh[3]
        self._scalpMeshPath = mesh[5]
        return True

    def bindScalpFromStage(self, scalpPath, stage=None):
        """Bind the mesh (or face GeomSubset) at `scalpPath` as the scalp.

        NOTE: PomadeModel::BindScalp clears the scalp graph, its region
        loops and the rasterised face ids. Call it to attach a scalp, never
        to re-attach one the model already holds.
        """
        stage = stage if stage is not None else self._apiStage()
        if stage is None:
            self._status("Pomade: no stage")
            return False
        mesh = self._scalpMesh(scalpPath, stage)
        if mesh is None:
            return False
        # `flat`, `counts` and `indices` stay referenced until the call
        # returns: for numpy input the ctypes pointers borrow their memory.
        flat, counts, indices, centre, faces, meshPath = mesh
        pts = _cArray(flat, ctypes.c_float)
        cnt = _cArray(counts, ctypes.c_int)
        idx = _cArray(indices, ctypes.c_int)
        if faces is None:
            rc = self.dll.Pomade_BindScalp(self._model, pts, len(flat), cnt,
                                          len(counts), idx, len(indices))
        else:
            active = _cArray(faces, ctypes.c_int)
            rc = self.dll.Pomade_BindScalpSubset(
                self._model, pts, len(flat), cnt, len(counts), idx,
                len(indices), active, len(faces))
        if rc != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return False
        self._scalpCenter = centre
        self._scalpMeshPath = meshPath
        return True

    def deactivate(self):
        """Tear the model down; the stage keeps whatever was committed."""
        dll = self.dll
        if dll is not None:
            if self._bake is not None:
                dll.Pomade_BakeDestroy(self._bake)
            if self._committer is not None:
                dll.Pomade_CommitterDestroy(self._committer)
            if self._model is not None:
                dll.Pomade_Deactivate(self._model)
                dll.Pomade_Destroy(self._model)
        self._removeLiveSublayer()
        self._removeBakeDirectory()
        self._model = None
        self._committer = None
        self._bake = None
        self._liveLayer = None
        self._liveId = ""
        self._gestureDepth = 0
        self._scalpCenter = None
        # Everything below describes the torn-down workers and the stage
        # they wrote to (SS-06). Left set, the next activate() inherited
        # them: a bake "in flight" kept hasPendingWork() True forever (the
        # new worker never completes the old version), the map file named
        # the previous groom's .ptx in a directory that is now gone, an
        # Output reveal fired on the new groom's first swap, and the
        # stage handle kept a closed stage alive.
        self._bakeInFlight = False
        self._mapFile = ""
        self._pendingOutputReveal = False
        self._stage = None
        # Rebound on demand over whatever library is loaded next; shutdown
        # drops the library itself, and this binding would pin it.
        self._stageLib = None
        # A reattach that found no scalp leaves the session detached until
        # the artist binds again, and binding starts with this teardown.
        self._scalpMissing = ""
        self._detached = False
        self._commitError = ""
        self._lastSwapCode = pomadeLib.POMADE_COMMITTER_NOTHING_PENDING
        self._state.activated = False

    def _removeBakeDirectory(self):
        """Take this process's per-run bake directory away with it.

        Only the directory this session made, and only once the worker
        is destroyed (deactivate does that first), so nothing is racing
        for the files. An explicit USDGENPOMADE_BAKE_DIR belongs to
        whoever set it and is left alone.
        """
        if not self._bakeDir or os.environ.get("USDGENPOMADE_BAKE_DIR"):
            self._bakeDir = ""
            return
        shutil.rmtree(self._bakeDir, ignore_errors=True)
        self._bakeDir = ""

    def _removeLiveSublayer(self):
        stage = self._stage
        if stage is None or not self._liveId:
            return
        try:
            session = stage.GetSessionLayer()
            paths = [p for p in session.subLayerPaths if p != self._liveId]
            if len(paths) != len(session.subLayerPaths):
                session.subLayerPaths = paths
        except Exception:
            pass

    def detach(self):
        """The stage went away under the tool (plan/17 section 3.4)."""
        if self._committer is None:
            return False
        self.dll.Pomade_CommitterDetach(self._committer)
        self._removeLiveSublayer()
        self._stage = None
        self._detached = True
        return True

    def reattach(self, stage=None):
        """A new stage arrived: re-host the live sublayer and re-commit.

        The model is the artist's work and survives the stage (plan/17
        section 3.4), so this never re-binds the scalp: Pomade_BindScalp
        clears the scalp graph, its region loops and the undo stack, and a
        File > Reopen used to commit an empty groom that way. Only the
        scalp's centre is re-measured, as hydrate does.

        A stage without the bound scalp keeps the session detached and
        the live layer out of it: the old groom would float over a scene
        that has nothing to grow it from.
        """
        if self._committer is None:
            return False
        stage = stage if stage is not None else self._apiStage()
        if stage is None:
            return False
        if self._scalpPath and not stage.GetPrimAtPath(self._scalpPath):
            self._scalpMissing = self._scalpPath
            self._status("Scalp %s not found in the new stage: bind a scalp "
                         "mesh to continue" % self._scalpPath)
            return False
        self._scalpMissing = ""
        self._stage = stage
        if self._liveLayer is not None:
            paths = list(stage.GetSessionLayer().subLayerPaths)
            if self._liveId not in paths:
                stage.GetSessionLayer().subLayerPaths.insert(0, self._liveId)
        if self._scalpPath:
            self.recordScalpCenter(self._scalpPath, stage)
        self.dll.Pomade_CommitterReattach(self._committer)
        self._detached = False
        self.enqueueCommit()
        self.publish(PUBLISH_ALL)
        return True

    # -- publication -------------------------------------------------------

    def publish(self, dirtyMask=PUBLISH_PENDING):
        """Pomade_Publish; returns the number of scene indices refreshed.

        The default is the model's own pending bits (see PUBLISH_PENDING).
        0 is a normal answer in a headless process or a usdview with the
        scene index disabled; -1 is the error.
        """
        if self._model is None:
            return 0
        count = self.dll.Pomade_Publish(self._model, int(dirtyMask))
        if count < 0:
            self._status("Pomade: " + self.lastError())
            return 0
        self._state.generation = int(self.modelVersion)
        self._notify(self._publishListeners)
        # Dock buttons and viewport gestures share this publication path.
        # Updating the view here keeps a successful model publish visible
        # immediately instead of waiting for the idle pump or dock timer.
        self.refreshViewport()
        return count

    def publishAll(self):
        return self.publish(PUBLISH_ALL)

    def clearGeneratedCurves(self):
        """Remove the live guide preview without changing authored tubes.

        The model owns the clear/suppression state and its undo snapshot;
        this wrapper only publishes the native operation and mirrors the
        panel state.
        """
        if self._model is None:
            return False
        entry = getattr(self.dll, "Pomade_ClearGeneratedCurves", None)
        if entry is None:
            self._status("Pomade: clear generated curves is unavailable in "
                         "this library")
            return False
        ok = int(entry(self._model)) == pomadeLib.POMADE_OK
        if not ok:
            self._status("Pomade: " + self.lastError())
            return False
        self.enqueueCommit()
        self.publish()
        return True

    def refillGeneratedCurves(self):
        """Explicitly restore the generated guide preview at full density."""
        if self._model is None:
            return False
        # GenerateGuides is the explicit user action: unlike RefillGuides,
        # it clears the native post-Clear suppression latch.
        ok = self.dll.Pomade_GenerateGuides(
            self._model, ctypes.c_float(1.0)) == pomadeLib.POMADE_OK
        if not ok:
            self._status("Pomade: " + self.lastError())
            return False
        # GenerateGuides re-enables generation, while visibility remains the
        # separate native preference set by the Show checkbox.
        self.generatedCurvesVisible()
        self.enqueueCommit()
        self.publish()
        return True

    def setGeneratedCurvesVisible(self, visible):
        """Persist guide visibility, using the optional native display hook."""
        visible = bool(visible)
        if self._model is None:
            self._state.showGeneratedCurves = visible
            return True
        entry = getattr(self.dll, "Pomade_SetGeneratedCurvesVisible", None)
        if entry is None:
            self._status("Pomade: generated-curve visibility is unavailable "
                         "in this library")
            return False
        if int(entry(self._model, 1 if visible else 0)) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return False
        self._state.showGeneratedCurves = visible
        self.publish()
        return True

    def generatedCurvesVisible(self):
        """Return the native guide visibility, when the model exposes it."""
        if self._model is None:
            return bool(self._state.showGeneratedCurves)
        entry = getattr(self.dll, "Pomade_GetGeneratedCurvesVisible", None)
        if entry is None:
            return bool(self._state.showGeneratedCurves)
        visible = ctypes.c_int(0)
        if int(entry(self._model, ctypes.byref(visible))) != pomadeLib.POMADE_OK:
            return bool(self._state.showGeneratedCurves)
        self._state.showGeneratedCurves = bool(visible.value)
        return bool(visible.value)

    def setAmplifiedHair(self, visible):
        """Show or hide the amplified hair through the model (DK-06).

        The model owns the swap (plan/17 section 3.2), so both dock
        controls -- the Output row and the Display checkbox -- come here;
        writing the state field alone left both ticked over guides only.
        """
        visible = bool(visible)
        if self._model is None:
            self._state.showAmplifiedHair = visible
            return True
        entry = getattr(self.dll, "Pomade_SetAmplifiedHair", None)
        if entry is None:
            self._status("Pomade: amplified hair is unavailable in this "
                         "library")
            return False
        if int(entry(self._model, 1 if visible else 0)) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return False
        self._state.showAmplifiedHair = visible
        self.publish()
        return True

    # -- Output description ----------------------------------------------

    def outputSettingsAvailable(self):
        """Whether this model library exposes committed Output settings."""
        return (self._model is not None and
                getattr(self.dll, "Pomade_GetOutputSettings", None) is not None
                and getattr(self.dll, "Pomade_SetOutputSettings", None)
                is not None)

    def outputSettings(self):
        """Return ``(enabled, densityMultiplier, strandWidth)``."""
        enabled = bool(getattr(self._state, "outputEnabled", False))
        multiplier = float(getattr(self._state,
                                   "outputDensityMultiplier", 1.0))
        width = float(getattr(self._state, "outputStrandWidth", 0.01))
        if self._model is None:
            return enabled, multiplier, width
        entry = getattr(self.dll, "Pomade_GetOutputSettings", None)
        if entry is None:
            return enabled, multiplier, width
        nativeEnabled = ctypes.c_int(0)
        nativeMultiplier = ctypes.c_float(multiplier)
        nativeWidth = ctypes.c_float(width)
        if int(entry(self._model, ctypes.byref(nativeEnabled),
                     ctypes.byref(nativeMultiplier),
                     ctypes.byref(nativeWidth))) != pomadeLib.POMADE_OK:
            return enabled, multiplier, width
        enabled = bool(nativeEnabled.value)
        if self._pendingOutputReveal and enabled:
            enabled = bool(getattr(self._state, "outputEnabled", False))
        multiplier = max(float(nativeMultiplier.value), 0.0)
        width = max(float(nativeWidth.value), 0.0)
        self._state.outputEnabled = enabled
        self._state.outputDensityMultiplier = multiplier
        self._state.outputStrandWidth = width
        return enabled, multiplier, width

    def _blockedOutputPath(self, stage):
        """The reserved Output prim an artist owns, or "".

        The same rule as the committer's enqueue guard (pomadeCommit.cpp
        `_BlockedOutputPath`): Output, OutputCurves and OutputRegionMap
        are the tool's only while they carry the outputOwned marker.
        """
        if stage is None:
            return ""
        markerName = "usdGen:pomade:outputOwned"
        for suffix in ("Output", "OutputCurves", "OutputRegionMap"):
            prim = stage.GetPrimAtPath(self._groomPath + "/" + suffix)
            if not prim:
                continue
            marker = prim.GetAttribute(markerName)
            if not marker or marker.Get() is not True:
                return str(prim.GetPath())
        return ""

    def _outputCollision(self):
        """Reject artist-owned Output prims before native output is enabled."""
        stage = self._stage if self._stage is not None else self._apiStage()
        blocked = self._blockedOutputPath(stage)
        if blocked:
            self._status("Pomade: cannot build Output description; "
                         "%s is not Pomade-owned (preserving it)" % blocked)
            return False
        return True

    def _nativeOutputEnabled(self):
        """Whether the MODEL has Output on (what the next build authors).

        Read straight from the library: outputSettings() also mirrors
        into the tool state and folds in the pending reveal, and the
        enqueue guard must neither write state nor be fooled by it.
        """
        entry = getattr(self.dll, "Pomade_GetOutputSettings", None) \
            if self._model is not None else None
        if entry is None:
            return False
        enabled = ctypes.c_int(0)
        multiplier = ctypes.c_float(0.0)
        width = ctypes.c_float(0.0)
        if int(entry(self._model, ctypes.byref(enabled),
                     ctypes.byref(multiplier),
                     ctypes.byref(width))) != pomadeLib.POMADE_OK:
            return False
        return bool(enabled.value)

    def setOutputSettings(self, enabled=None, densityMultiplier=None,
                          strandWidth=None, publish=True, enqueue=True):
        """Apply Output description settings and mirror native state."""
        current = self.outputSettings()
        enabled = current[0] if enabled is None else bool(enabled)
        densityMultiplier = (current[1] if densityMultiplier is None else
                             max(float(densityMultiplier), 1e-6))
        strandWidth = (current[2] if strandWidth is None else
                       max(float(strandWidth), 0.0))
        if enabled and not self._outputCollision():
            return False
        entry = getattr(self.dll, "Pomade_SetOutputSettings", None) \
            if self._model is not None else None
        if self._model is not None and entry is None:
            self._status("Pomade: Output description is unavailable in this "
                         "library")
            return False
        if entry is not None and int(entry(
                self._model, 1 if enabled else 0,
                ctypes.c_float(densityMultiplier),
                ctypes.c_float(strandWidth))) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return False
        if enabled and not current[0]:
            self._pendingOutputReveal = True
        elif not enabled:
            self._pendingOutputReveal = False
        self._state.outputEnabled = (False if self._pendingOutputReveal and
                                     enabled else enabled)
        self._state.outputDensityMultiplier = densityMultiplier
        self._state.outputStrandWidth = strandWidth
        if enqueue and self._committer is not None:
            if not self.enqueueCommit():
                return False
        if publish and self._model is not None:
            self.publish()
        return True

    def buildOutputDescription(self):
        """Commit sparse guides, the region map, and their hair description."""
        _enabled, multiplier, width = self.outputSettings()
        if not self.setOutputSettings(True, multiplier, width,
                                      enqueue=False):
            return False
        amplified = getattr(self.dll, "Pomade_SetAmplifiedHair", None) \
            if self._model is not None else None
        if amplified is not None:
            if int(amplified(self._model, 1)) != pomadeLib.POMADE_OK:
                self._status("Pomade: " + self.lastError())
                return False
            self._state.showAmplifiedHair = True
            self.publish()
        if not self.enqueueCommit():
            return False
        self._status("Pomade: Sparse groom and PTex bake queued (%g density, %g width)"
                     % (multiplier, width))
        return True

    def refreshViewport(self):
        api = self._api
        if api is None:
            return
        try:
            api.UpdateViewport()
        except AttributeError:
            pass

    # -- gestures ----------------------------------------------------------

    def beginGesture(self, label):
        if self._model is None:
            return False
        if self.dll.Pomade_BeginGesture(
                self._model, str(label).encode("utf-8")) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return False
        if self._gestureDepth == 0:
            self.cancelDescriptionCooks()
        self._gestureDepth += 1
        return True

    def cancelDescriptionCooks(self):
        """Abandon the cook the last swap started (plan/18 section 7 G7).

        The swap dirties <groom>/Guides, the dirty router dirties the
        linked description, and its session cooks. By the time the artist
        presses again that cook describes a groom the model has left, and
        the amplified tiles it would publish are hidden for the length of
        the gesture anyway. Bumping the description session's cancellation
        token drops the request before the engine sees it. Returns the
        number of sessions whose token moved (0 with no committer, no
        description, or nothing queued).
        """
        if self._committer is None:
            return 0
        entry = getattr(self.dll, "Pomade_CommitterCancelCooks", None)
        if entry is None:
            return 0
        moved = int(entry(self._committer))
        return max(moved, 0)

    def endGesture(self):
        if self._model is None or self._gestureDepth <= 0:
            return False
        ok = self.dll.Pomade_EndGesture(self._model) == pomadeLib.POMADE_OK
        self._gestureDepth = max(self._gestureDepth - 1, 0)
        if not ok:
            self._status("Pomade: " + self.lastError())
        return ok

    def cancelGesture(self):
        """Restore the press-time base; returns the dirty bits to publish."""
        if self._model is None or self._gestureDepth <= 0:
            return 0
        dirty = ctypes.c_uint(0)
        ok = self.dll.Pomade_CancelGesture(
            self._model, ctypes.byref(dirty)) == pomadeLib.POMADE_OK
        self._gestureDepth = max(self._gestureDepth - 1, 0)
        if not ok:
            self._status("Pomade: " + self.lastError())
            return 0
        return int(dirty.value)

    def endGestureIfChanged(self, changed):
        """Close the open gesture, leaving no undo step when nothing changed.

        A click that selected, a slider press that never moved, a panel
        write of the value already held: each opened a bracket, and
        Pomade_EndGesture keeps the step Begin pushed whether or not the
        model moved, so Ctrl+Z would spend a keypress on nothing. Cancel
        drops that step and restores the press-time base; its dirty bits
        are published because the viewport may have drawn the gesture.
        Returns True when an undo step was kept.
        """
        if changed:
            return self.endGesture()
        dirty = self.cancelGesture()
        if dirty:
            self.publish(dirty)
        return False

    # -- undo / redo -------------------------------------------------------

    def undo(self):
        """Step back one undo step; False (and says why) when there is none."""
        return self._stepHistory(True)

    def redo(self):
        """Re-apply the step undo left; False (and says why) when none."""
        return self._stepHistory(False)

    def _stepHistory(self, backward):
        if self._model is None:
            return False
        dll = self.dll
        word = "undo" if backward else "redo"
        if self.gestureActive:
            # Parity G14, for session gestures too: a dock slider drag
            # (Density, Edge bias) holds a session bracket the viewport's
            # own drag guard never sees, and Ctrl+Z still reaches us. The
            # model refuses as well; saying why here beats "undo failed".
            self._status("Pomade: finish the drag before %s" % word,
                         "warning")
            return False
        # Pomade_Undo/Redo on an empty stack is a no-op SUCCESS, so the
        # depth is the only honest answer to "is there anything to undo":
        # without it the session published and committed an unchanged
        # model and the artist got no word at all.
        depth = self.undoDepth() if backward else self.redoDepth()
        if depth <= 0:
            self._status("Nothing to %s" % word, "warning")
            return False
        label = self.undoLabel(0 if backward else -1)
        guidesBefore = self._guideCount()
        dirty = ctypes.c_uint(0)
        entry = dll.Pomade_Undo if backward else dll.Pomade_Redo
        if entry(self._model, ctypes.byref(dirty)) != pomadeLib.POMADE_OK:
            self._status("Pomade: %s failed: %s" % (word, self.lastError()),
                         "error")
            return False
        bits = int(dirty.value)
        # A step recorded before the guides were grown (a stub build, a
        # panel edit from before SS-02) restores an empty guide set, and
        # walkthrough step 12 lost all 38 guides to one Ctrl+Z. Regrow them
        # from the restored tubes whenever the artist had guides on screen;
        # RefillGuides stays a no-op after an explicit Clear.
        if guidesBefore > 0 and dll.Pomade_RefillGuides(
                self._model, ctypes.c_float(1.0)) == pomadeLib.POMADE_OK:
            bits |= pomadeLib.POMADE_DIRTY_GUIDES
        self.publish(bits)
        self.enqueueCommit()
        verb = "Undo" if backward else "Redo"
        self._status("%s: %s" % (verb, label) if label else verb)
        return True

    def _guideCount(self):
        if self._model is None:
            return 0
        entry = getattr(self.dll, "Pomade_GetGuideCounts", None)
        if entry is None:
            return 0
        guides = ctypes.c_int(0)
        if entry(self._model, ctypes.byref(guides), None) \
                != pomadeLib.POMADE_OK:
            return 0
        return int(guides.value)

    def undoDepth(self):
        """Steps Ctrl+Z can take (0 with no model)."""
        if self._model is None:
            return 0
        return max(int(self.dll.Pomade_GetUndoDepth(self._model)), 0)

    def redoDepth(self):
        """Steps Ctrl+Y can take (0 with no model)."""
        if self._model is None:
            return 0
        return max(int(self.dll.Pomade_GetRedoDepth(self._model)), 0)

    def redoLabel(self):
        """What Ctrl+Y would redo, or "" when the redo stack is empty."""
        return self.undoLabel(-1)

    def undoLabel(self, depth=0):
        if self._model is None:
            return ""
        buf = ctypes.create_string_buffer(256)
        if self.dll.Pomade_GetUndoLabel(self._model, int(depth), buf,
                                       256) != pomadeLib.POMADE_OK:
            return ""
        return buf.value.decode("utf-8", "replace")

    # -- picking -----------------------------------------------------------

    def pickItem(self, camera, x, y, radiusPx, kindMask):
        """K11: the selection item under a pixel, or None.

        Returns {"kind", "id", "subId", "subSubId"} with the STABLE ids the
        selection stores, which is why this is Pomade_PickItem and not
        Pomade_Pick (whose index is a candidate ordinal).
        """
        if self._model is None or camera is None:
            return None
        hit = ctypes.c_int(0)
        kind = ctypes.c_uint(0)
        ident = ctypes.c_int(-1)
        subId = ctypes.c_int(-1)
        subSubId = ctypes.c_int(-1)
        if self.dll.Pomade_PickItem(
                self._model, camera.viewProjArray(), camera.width,
                camera.height, ctypes.c_float(x), ctypes.c_float(y),
                ctypes.c_float(radiusPx), ctypes.c_uint(int(kindMask)),
                ctypes.byref(hit), ctypes.byref(kind), ctypes.byref(ident),
                ctypes.byref(subId),
                ctypes.byref(subSubId)) != pomadeLib.POMADE_OK:
            return None
        if not hit.value:
            return None
        return {"kind": int(kind.value), "id": int(ident.value),
                "subId": int(subId.value), "subSubId": int(subSubId.value)}

    def raycast(self, origin, direction):
        """K1: the scalp hit along a world ray, or None.

        This replaces the per-move StageView.pick the P2 controller used
        (plan/18 finding F6): it costs a BVH walk instead of a Hydra pick
        pass, and it answers face and UV directly.
        """
        if self._model is None:
            return None
        o = (ctypes.c_float * 3)(*[float(v) for v in origin])
        d = (ctypes.c_float * 3)(*[float(v) for v in direction])
        hit = ctypes.c_int(0)
        face = ctypes.c_int(-1)
        uv = (ctypes.c_float * 2)(0.0, 0.0)
        xyz = (ctypes.c_float * 3)(0.0, 0.0, 0.0)
        nrm = (ctypes.c_float * 3)(0.0, 1.0, 0.0)
        if self.dll.Pomade_Raycast(self._model, o, d, ctypes.byref(hit),
                                  ctypes.byref(face), uv, xyz,
                                  nrm) != pomadeLib.POMADE_OK:
            return None
        if not hit.value:
            return None
        return {"face": int(face.value), "u": float(uv[0]),
                "v": float(uv[1]),
                "point": (float(xyz[0]), float(xyz[1]), float(xyz[2])),
                "normal": (float(nrm[0]), float(nrm[1]), float(nrm[2]))}

    def setHover(self, kind=0, ident=-1, subId=-1, subSubId=-1):
        if self._model is None:
            return False
        return self.dll.Pomade_SetHover(
            self._model, ctypes.c_uint(int(kind)), int(ident), int(subId),
            int(subSubId)) == pomadeLib.POMADE_OK

    def selectRect(self, camera, x0, y0, x1, y1, kindMask,
                   mode=pomadeLib.POMADE_SELECT_SET):
        if self._model is None or camera is None:
            return False
        return self.dll.Pomade_SelectRect(
            self._model, camera.viewProjArray(), camera.width, camera.height,
            ctypes.c_float(x0), ctypes.c_float(y0), ctypes.c_float(x1),
            ctypes.c_float(y1), ctypes.c_uint(int(kindMask)),
            int(mode)) == pomadeLib.POMADE_OK

    def selectPolygon(self, camera, points, kindMask,
                      mode=pomadeLib.POMADE_SELECT_SET):
        """Select against physical-pixel (x, y) lasso points."""
        if self._model is None or camera is None or len(points) < 3:
            return False
        flat = [float(value) for point in points for value in point]
        xy = (ctypes.c_float * len(flat))(*flat)
        return self.dll.Pomade_SelectPolygon(
            self._model, camera.viewProjArray(), camera.width, camera.height,
            xy, int(len(points)), ctypes.c_uint(int(kindMask)),
            int(mode)) == pomadeLib.POMADE_OK

    def select(self, kind, ids, subIds=None, subSubIds=None,
               mode=pomadeLib.POMADE_SELECT_SET):
        if self._model is None:
            return False
        n = len(ids)
        arrays = []
        for source in (ids, subIds, subSubIds):
            if source is None:
                arrays.append(None)
            else:
                arrays.append((ctypes.c_int * n)(*[int(v) for v in source]))
        entry = {pomadeLib.POMADE_SELECT_SET: self.dll.Pomade_SelectSet,
                 pomadeLib.POMADE_SELECT_ADD: self.dll.Pomade_SelectAdd,
                 pomadeLib.POMADE_SELECT_TOGGLE: self.dll.Pomade_SelectToggle,
                 }[int(mode)]
        return entry(self._model, ctypes.c_uint(int(kind)), arrays[0],
                     arrays[1], arrays[2], n) == pomadeLib.POMADE_OK

    def clearSelection(self, kindMask=0):
        if self._model is None:
            return False
        return self.dll.Pomade_SelectClear(
            self._model, ctypes.c_uint(int(kindMask))) == pomadeLib.POMADE_OK

    def readSelection(self, kind):
        """[(id, subId, subSubId)] of one kind, ascending."""
        if self._model is None:
            return []
        count = ctypes.c_int(0)
        if self.dll.Pomade_ReadSelection(self._model, ctypes.c_uint(int(kind)),
                                        None, None, None, 0,
                                        ctypes.byref(count)) \
                != pomadeLib.POMADE_OK:
            return []
        n = int(count.value)
        if n <= 0:
            return []
        ids = (ctypes.c_int * n)()
        subIds = (ctypes.c_int * n)()
        subSubIds = (ctypes.c_int * n)()
        if self.dll.Pomade_ReadSelection(self._model, ctypes.c_uint(int(kind)),
                                        ids, subIds, subSubIds, n,
                                        ctypes.byref(count)) \
                != pomadeLib.POMADE_OK:
            return []
        return [(int(ids[i]), int(subIds[i]), int(subSubIds[i]))
                for i in range(min(n, int(count.value)))]

    def selectionCount(self, kindMask=0):
        if self._model is None:
            return 0
        return int(self.dll.Pomade_GetSelectionCount(
            self._model, ctypes.c_uint(int(kindMask))))

    # -- commit + bake -----------------------------------------------------

    def enqueueCommit(self):
        """Ask the worker for a layer of the model's latest version.

        Also the warnings row's "Retry commit": every enqueue is a fresh
        attempt, so the last failure is forgotten here and a repeat of it
        is reported again rather than swallowed.
        """
        if self._committer is None:
            return False
        self._commitError = ""
        # A scalp with no tube yet (a graph still being drawn) is a model
        # the committer refuses to build ("snapshot holds no tubes").
        # Before anything reached the stage that refusal is not news -- the
        # groom has simply not started -- and reporting it would turn the
        # sync pill red on every graph stroke. Once a groom is on the
        # stage an empty model IS news (the stage keeps the old tubes), so
        # that enqueue goes through and its failure is reported.
        if self.committedVersion == 0 and self._tubeCount() == 0:
            return True
        # The committer's artist-owned-output guard needs the composed
        # stage, and no UsdStage crosses the C ABI, so the ctypes path runs
        # the same rule here, over the stage the session already holds
        # (SS-05). Without it an artist's Output prim would be overwritten
        # by the live layer's stronger opinions without a word.
        if self._nativeOutputEnabled():
            stage = self._stage if self._stage is not None \
                else self._apiStage()
            blocked = self._blockedOutputPath(stage)
            if blocked:
                self._commitFailed(
                    "refusing to overwrite artist-owned output at %s"
                    % blocked)
                return False
        if self.dll.Pomade_CommitterEnqueue(self._committer, 0, 0, 0,
                                           None) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError(), "error")
            return False
        self._wakeIdle()
        return True

    def _tubeCount(self):
        """Live tubes in the model, or -1 when the library cannot say."""
        entry = getattr(self.dll, "Pomade_GetTubeCount", None) \
            if self._model is not None else None
        if entry is None:
            return -1
        return int(entry(self._model))

    def _commitFailed(self, reason):
        """Record why the commit failed; say so once per distinct reason.

        The pump sees a persistent failure (a live layer that is gone)
        on every idle slot; one status line per reason is the artist's
        cue, the sync pill and the warnings row keep standing after it.
        """
        reason = str(reason or "unknown reason")
        if reason != self._commitError:
            self._status("Pomade: commit failed: " + reason, "error")
        self._commitError = reason

    def _takeCommitDiagnostic(self):
        """The committer's parked failure text, taken (cleared) once."""
        entry = getattr(self.dll, "Pomade_CommitterTakeDiagnostic", None)
        if entry is None or self._committer is None:
            return ""
        buffer = ctypes.create_string_buffer(4096)
        length = int(entry(self._committer, buffer, 4096))
        if length <= 0:
            return ""
        return buffer.value.decode("utf-8", "replace")

    def stageLastError(self):
        """Pomade_StageGetLastError: the error buffer of pomadeApiStage.h.

        The per-tube and hierarchy entries (Pomade_BakeEnqueueLevels among
        them) write this one, not the model buffer lastError() reads.
        """
        entry = getattr(self.dll, "Pomade_StageGetLastError", None) \
            if self._state.lib is not None else None
        if entry is None:
            return ""
        text = entry()
        return text.decode("utf-8", "replace") if text else ""

    def rasterise(self):
        """K3 over the current graph (gesture end only, plan/17 4.2)."""
        if self._model is None:
            return False
        if self.dll.Pomade_Rasterise(self._model) != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return False
        return True

    def ensureRegionTubes(self):
        """Give every closed region its L1 tube stub (plan/18 section 7 G14).

        plan/17 section 5.1: "each closed region gets a tube stub". Until
        one exists the groom cannot be committed at all -- the committer
        refuses a snapshot with no tubes -- so an artist who only drew a
        graph would save nothing. Called at the end of a graph edit, once K3
        has claimed the faces Pomade_BuildTubeFromRegion needs (the rasterise
        also re-attaches the existing tubes to the renumbered regions), and
        while that edit's undo bracket is still open: outside a bracket each
        build (and the refill below) pushes its own undo step, and the
        artist's first Ctrl+Z would take back only the stub.

        Returns the number of stubs built. The model holds one L1 tube per
        region, so a scalp with five regions ends with five L1 roots.
        """
        if self._model is None:
            return 0
        regions, _uncovered, _intersected = self.regionStats()
        if regions <= 0:
            return 0
        from . import pomadeTube
        # Zero is the BuildTubeFromRegion Auto sentinel: it makes one
        # column per authored region CV instead of averaging a radial default.
        # Existing roots are skipped below, so changing this preference never
        # regenerates or reshapes a sculpted tube.
        ringVerts = pomadeTube.regionRingVerts(
            self._state.panels.get("tube", {}).get(
                "ringCvCount", pomadeTube.DEFAULT_REGION_RING_VERTS))
        built = 0
        for regionId in range(regions):
            if int(self.dll.Pomade_TubeForRegion(self._model, regionId)) >= 0:
                continue                 # this region already has its tube
            # A region that claims no face (it lost its interior to a link,
            # or K3 has not reached it) refuses; the rest still build.
            if self.dll.Pomade_BuildTubeFromRegion(
                    self._model, regionId, pomadeTube.DEFAULT_RINGS,
                    ringVerts,
                    ctypes.c_float(pomadeTube.DEFAULT_LENGTH)) \
                    == pomadeLib.POMADE_OK:
                built += 1
        if built:
            # A new stub has no guides until something refills them, and
            # nothing else does until a Fill parameter is touched: the
            # artist closed a region and saw bare tubes. The native
            # post-Clear latch keeps this a no-op after an explicit Clear.
            self.dll.Pomade_RefillGuides(self._model, ctypes.c_float(1.0))
            self._status("Pomade: %d tube stub%s built"
                         % (built, "" if built == 1 else "s"))
        return built

    @property
    def bakeTexelResolution(self):
        """Per-face bake resolution in force, in texels per side (0 = auto)."""
        return self._bakeTexels

    @property
    def bakeDir(self):
        """Where this session's bake worker writes its versioned maps."""
        return self._bakeDir

    # The bake takes the per-face resolution as an exponent, so a forced
    # resolution is always a power of two. The ceiling is the bake's own
    # clamp; the floor is the coarsest resolution its automatic plan will
    # give a face it has to classify at all (plan/17 section 4.5), and it
    # is what keeps every applied value inside the panel's choices.
    MIN_BAKE_RES_LOG2 = 2
    MAX_BAKE_RES_LOG2 = 12

    def setBakeTexelResolution(self, texels):
        """Force the bake's per-face texel resolution; 0 restores auto.

        `texels` is texels per face side. The C ABI takes log2 of it, so a
        value that is not a power of two rounds DOWN to one, and the
        result is clamped to 2^MIN_BAKE_RES_LOG2 .. 2^MAX_BAKE_RES_LOG2.
        Returns the resolution now in force, which is what the panel
        must display: reporting the number that was typed rather than
        the one that was applied is how a knob starts lying about the
        bake.
        """
        value = int(texels)
        applied = 0
        if value > 0:
            log2 = 0
            while (1 << (log2 + 1)) <= value:
                log2 += 1
            log2 = max(self.MIN_BAKE_RES_LOG2,
                       min(log2, self.MAX_BAKE_RES_LOG2))
            applied = 1 << log2
        changed = self._bakeTexels != applied
        self._bakeTexels = applied
        pushed = self._applyBakeOptions()
        if (changed and pushed and self._committer is not None and
                self.outputSettings()[0]):
            self.enqueueCommit()
        return applied

    def _applyBakeOptions(self):
        """Push the session's bake options onto the worker's context.

        The worker reads them at enqueue, so this only has to run before
        the next rebake() -- but it runs at bake-context creation too, so
        a resolution chosen with no scalp bound is not silently dropped.
        """
        if self._bake is None:
            return False
        resLog2 = -1
        if self._bakeTexels > 0:
            resLog2 = self._bakeTexels.bit_length() - 1
        if self.dll.Pomade_BakeSetOptions(self._bake, resLog2,
                                         self._bakeLevels) \
                != pomadeLib.POMADE_OK:
            self._status("Pomade: " + self.lastError())
            return False
        return True

    def rebake(self):
        """Enqueue a region-map bake that carries the hierarchy."""
        if self._bake is None:
            return False
        entry = getattr(self.dll, "Pomade_BakeEnqueueLevels", None)
        status = (entry(self._bake) if entry is not None
                  else self.dll.Pomade_BakeEnqueue(self._bake))
        if status != pomadeLib.POMADE_OK:
            # Pomade_BakeEnqueueLevels is a pomadeApiStage.h entry and sets
            # the STAGE error buffer; the model buffer would print some
            # older, unrelated failure (SS-05).
            reason = self.stageLastError() if entry is not None else ""
            self._status("Pomade: region map bake failed: " +
                         (reason or self.lastError() or "unknown reason"),
                         "error")
            return False
        self._state.mapVersion = int(self.dll.Pomade_GetMapVersion(self._model))
        self._bakeInFlight = True
        self._wakeIdle()
        return True

    def pump(self):
        """One idle slot: committer swap, then drain finished bakes.

        Never called during a gesture -- the controller owns that rule -- and
        the gesture flag it passes the committer is the real one, not the
        constant zero the P2 controller passed (plan/18 section 7 G7).
        Returns a dict the status strip reads.
        """
        swapped = False
        baked = 0
        if self._committer is not None:
            code = int(self.dll.Pomade_CommitterSwap(
                self._committer, self._liveId.encode("utf-8"),
                1 if self.gestureActive else 0))
            self._lastSwapCode = code
            swapped = code == pomadeLib.POMADE_COMMITTER_SWAPPED
            if code == pomadeLib.POMADE_COMMITTER_ERROR:
                # Reported once (the same reason on the next slot is
                # silent); the pill keeps showing it until a swap lands.
                self._commitFailed(self.lastError() or
                                   "the live layer could not be swapped")
            elif swapped:
                self._commitError = ""
            if swapped and self._pendingOutputReveal:
                self._pendingOutputReveal = False
                self.outputSettings()
        if self._bake is not None:
            version = ctypes.c_ulonglong(0)
            path = ctypes.create_string_buffer(4096)
            while self.dll.Pomade_BakeTakeCompleted(
                    self._bake, ctypes.byref(version), path, 4096) == 1:
                filePath = path.value.decode("utf-8")
                if self.dll.Pomade_BakeSwap(
                        self._bake, version, self._liveId.encode("utf-8"),
                        self._mapPath.encode("utf-8"),
                        filePath.encode("utf-8")) == pomadeLib.POMADE_OK:
                    self._state.bakedVersion = int(version.value)
                    self._mapFile = filePath
                    baked += 1
            pending = int(self.dll.Pomade_BakePendingVersion(self._bake))
            completed = int(self.dll.Pomade_BakeCompletedVersion(self._bake))
            self._bakeInFlight = pending > completed
        # Pending work BEFORE the diagnostic: the worker parks the failed
        # version and its reason together, so a failure landing after this
        # read still leaves `pending` True and the next slot takes it; read
        # the other way round, the idle timer could stop with the reason
        # still parked and the artist would never hear of it.
        pendingWork = self.hasPendingWork()
        if self._committer is not None:
            # A failed build parks its reason in the committer; take it
            # once per idle slot so the artist hears of it exactly once.
            diagnostic = self._takeCommitDiagnostic()
            if diagnostic:
                self._commitError = ""
                self._commitFailed(diagnostic)
        return {"swapped": swapped, "swapCode": self._lastSwapCode,
                "baked": baked, "pending": pendingWork}

    def hasPendingWork(self):
        """True while the pump still has work (plan/18 section 3.3).

        Not spelled `pending`: that name belongs to the pending VERSION the
        status strip prints, and a bool answering to it read as a number
        the first time the dock tried.
        """
        if self._committer is None:
            return False
        if self._bakeInFlight:
            return True
        pending = self.pendingVersion
        if pending <= self.committedVersion:
            return False
        # Nothing the pump can finish (SS-05): a swap that errors will
        # error again on the next slot, and the worker never retries a
        # failed version on its own. Only a fresh enqueue is new work, and
        # it wakes the pump itself.
        if self._lastSwapCode == pomadeLib.POMADE_COMMITTER_ERROR:
            return False
        failed = self.failedVersion
        return not (failed and failed >= pending)

    # -- status ------------------------------------------------------------

    @property
    def modelVersion(self):
        if self._model is None:
            return 0
        return int(self.dll.Pomade_GetVersion(self._model))

    @property
    def committedVersion(self):
        if self._committer is None:
            return 0
        return int(self.dll.Pomade_CommitterCommittedVersion(self._committer))

    @property
    def pendingVersion(self):
        if self._committer is None:
            return 0
        return int(self.dll.Pomade_CommitterPendingVersion(self._committer))

    @property
    def failedVersion(self):
        """The version the committer failed to build (0 when none)."""
        entry = getattr(self.dll, "Pomade_CommitterFailedVersion", None) \
            if self._committer is not None else None
        if entry is None:
            return 0
        return int(entry(self._committer))

    @property
    def commitError(self):
        """Why the latest commit did not reach the stage, or ""."""
        return self._commitError

    @property
    def lastSwapMs(self):
        if self._committer is None:
            return 0.0
        return float(self.dll.Pomade_CommitterLastSwapMs(self._committer))

    @property
    def partialMode(self):
        if self._committer is None:
            return 0
        return int(self.dll.Pomade_CommitterPartialMode(self._committer))

    def fallbackReason(self):
        if self._model is None:
            return ""
        text = self.dll.Pomade_GetDeviceFallbackReason(self._model)
        return text.decode("utf-8") if text else ""

    def graphCounts(self):
        if self._model is None:
            return (0, 0, 0)
        nodes = ctypes.c_int(0)
        edges = ctypes.c_int(0)
        regions = ctypes.c_int(0)
        self.dll.Pomade_GetGraphCounts(self._model, ctypes.byref(nodes),
                                      ctypes.byref(edges),
                                      ctypes.byref(regions))
        return (int(nodes.value), int(edges.value), int(regions.value))

    def regionStats(self):
        if self._model is None:
            return (0, 0, 0)
        regions = ctypes.c_int(0)
        uncovered = ctypes.c_int(0)
        intersected = ctypes.c_int(0)
        self.dll.Pomade_GetRegionStats(self._model, ctypes.byref(regions),
                                      ctypes.byref(uncovered),
                                      ctypes.byref(intersected))
        return (int(regions.value), int(uncovered.value),
                int(intersected.value))

    def status(self):
        """Everything the HUD and the status strip read, in one dict."""
        nodes, edges, regions = self.graphCounts()
        _r, uncovered, intersected = self.regionStats()
        return {
            "active": self._model is not None,
            "detached": self._detached,
            "committerDetached": self._detached,
            "scalpMissing": self._scalpMissing,
            "gestureActive": self.gestureActive,
            "modelVersion": self.modelVersion,
            "committedVersion": self.committedVersion,
            "pendingVersion": self.pendingVersion,
            "mapVersion": int(self._state.mapVersion),
            "bakedVersion": int(self._state.bakedVersion),
            "lastSwapMs": self.lastSwapMs,
            "partialMode": self.partialMode,
            "swapCode": self._lastSwapCode,
            # SS-05: the sync pill reads 'Commit failed: <reason>' and the
            # warnings list offers 'Retry commit' while this is set.
            "commitError": self._commitError,
            "failedVersion": self.failedVersion,
            "fallbackReason": self.fallbackReason(),
            "nodes": nodes, "edges": edges, "regions": regions,
            "uncovered": uncovered, "intersected": intersected,
            "lastMoveMs": float(getattr(self._state, "lastMoveMs", 0.0)),
            # Where the fallback ladder stands (plan/18 section 3.7); the
            # status strip prints it, and 0 means full fidelity.
            "ladderStep": int(getattr(self._state, "ladderStep", 0)),
            # The Edit strip names the steps Ctrl+Z / Ctrl+Y would take.
            "undoLabel": self.undoLabel(0),
            "redoLabel": self.redoLabel(),
            "undoDepth": self.undoDepth(),
            "redoDepth": self.redoDepth(),
        }

    # -- file commands -----------------------------------------------------

    def _commitPending(self):
        """True while the committer owes the stage a newer version."""
        if self._committer is None:
            return False
        return self.pendingVersion > self.committedVersion

    def flushLatest(self, timeout=10.0, tick=None):
        """Swap the newest model layer in before a file save copies it.

        Waits for the committer only: a region-map bake can take seconds
        on a dense scalp and the save copies whichever map is already
        baked, so waiting on it too made Save block the UI for up to ten
        seconds. `tick` is called on every wait loop -- the dock passes
        QApplication.processEvents so the viewport keeps painting -- and
        the wait gives up after `timeout` seconds, saying so.
        """
        if self._committer is None:
            return True
        if self._detached:
            self._status("Pomade: the groom is detached from the stage; bind "
                         "a scalp mesh before saving", "error")
            return False
        if not self.enqueueCommit():
            return False
        timeout = max(float(timeout), 0.0)
        deadline = time.monotonic() + timeout
        while True:
            self.pump()
            if not self._commitPending():
                return True
            if self._commitError:
                # The build failed or the swap cannot land (SS-05): the
                # version the save wants will never arrive, so waiting
                # out the timeout would only hide the reason.
                self._status("Pomade: the latest groom commit failed (%s); "
                             "nothing was saved" % self._commitError,
                             "error")
                return False
            if time.monotonic() >= deadline:
                break
            if tick is not None:
                tick()
            time.sleep(0.005)
        self._status("Pomade: timed out after %.1f s waiting for the latest "
                     "groom commit; nothing was saved" % timeout, "error")
        return False

    def saveGroom(self, filePath, stage=None, addToRootLayer=False,
                  timeout=10.0, tick=None):
        """Write the live layer to a `.usdc`, place it in the stage, and
        drop the latest baked map beside it.

        A name without an extension gets `.usdc`. By default the file is
        sublayered under the live overlay in the SESSION layer, which is
        what this usdview session composes and nothing on disk remembers.
        `addToRootLayer` adds it to the scene's root layer instead (a path
        relative to that layer) and saves the root layer, so reopening the
        scene brings the groom back and Resume can hydrate it (SS-03).

        The Python twin of PomadeSaveGroomAndMaps (pomadeCommit.cpp): that one
        is C++-only because it needs a UsdStagePtr, and the C ABI carries no
        stage. Same contract, with the map named after the groom file
        (`./<stem>.regionMap.ptx`) so two grooms saved into one directory
        keep their own maps, and relative so a moved groom keeps resolving.
        """
        from pxr import Sdf
        stage = stage if stage is not None else (self._stage
                                                 or self._apiStage())
        if stage is None or self._liveLayer is None:
            self._status("Pomade: nothing to save (no live layer)", "error")
            return False
        filePath = str(filePath)
        extension = os.path.splitext(filePath)[1]
        if not extension:
            filePath += ".usdc"
        elif extension.lower() != ".usdc":
            self._status("Pomade: grooms are saved as .usdc, not %s (%s)"
                         % (extension, filePath), "error")
            return False
        if not self.flushLatest(timeout, tick):
            return False
        layer = None
        if os.path.isfile(filePath):
            layer = Sdf.Layer.FindOrOpen(filePath)
        if layer is None:
            layer = Sdf.Layer.CreateNew(filePath)
        if layer is None:
            self._status("Pomade: cannot open or create %s" % filePath,
                         "error")
            return False
        layer.TransferContent(self._liveLayer)
        mapWarning = self._saveMapBeside(layer, filePath)
        if not layer.Save():
            self._status("Pomade: cannot save %s" % filePath, "error")
            return False
        placed = self._placeSavedGroom(stage, layer, filePath,
                                       addToRootLayer)
        if placed is None:
            return False
        text = "Pomade: saved %s%s" % (filePath, placed)
        if mapWarning:
            self._status("%s; %s" % (text, mapWarning), "warning")
        else:
            self._status(text)
        return True

    def _placeSavedGroom(self, stage, layer, filePath, addToRootLayer):
        """Sublayer a saved groom; the status suffix, or None on failure.

        The live layer stays the strongest session sublayer either way: it
        is the overlay the tool edits, and the saved file slots in under
        it (plan/17 section 3.2).
        """
        session = stage.GetSessionLayer()
        root = stage.GetRootLayer()
        suffix = ""
        if addToRootLayer and root.anonymous:
            addToRootLayer = False
            suffix = (" (the scene has no file of its own, so the groom is "
                      "in this session only)")
        if not addToRootLayer:
            ordered = [self._liveId, layer.identifier]
            ordered += [p for p in session.subLayerPaths
                        if p not in (self._liveId, layer.identifier)]
            session.subLayerPaths = ordered
            return suffix
        # One placement only: the same file composed from both stacks would
        # be one groom opened twice.
        if layer.identifier in session.subLayerPaths:
            session.subLayerPaths = [p for p in session.subLayerPaths
                                     if p != layer.identifier]
        target = os.path.normcase(os.path.abspath(filePath))
        present = False
        for entry in root.subLayerPaths:
            try:
                resolved = root.ComputeAbsolutePath(entry)
            except Exception:
                resolved = entry
            if os.path.normcase(os.path.abspath(resolved)) == target:
                present = True
                break
        if not present:
            root.subLayerPaths.insert(0, self._relativeToLayer(root,
                                                               filePath))
        # The root layer is saved so a reopen (which reloads it from disk)
        # still names the groom; unsaved scene edits go with it, which the
        # dock's confirmation says before it asks for this.
        if root.dirty and not root.Save():
            self._status("Pomade: saved %s but cannot write %s to add it to "
                         "the scene" % (filePath, root.identifier), "error")
            return None
        return " and added it to %s" % os.path.basename(root.identifier)

    @staticmethod
    def _relativeToLayer(layer, filePath):
        """`filePath` as an asset path relative to `layer`'s own file."""
        path = os.path.abspath(filePath)
        base = os.path.dirname(os.path.abspath(layer.realPath or
                                               layer.identifier))
        try:
            relative = os.path.relpath(path, base)
        except ValueError:
            # Another drive: there is no relative spelling.
            return path.replace("\\", "/")
        relative = relative.replace("\\", "/")
        if not relative.startswith("."):
            relative = "./" + relative
        return relative

    def _saveMapBeside(self, layer, filePath):
        """Copy the latest baked map beside the groom and point at it.

        Returns a warning for the save's status line, or "". Nothing is
        saved here; saveGroom saves the layer once afterwards.
        """
        from pxr import Sdf
        if not self._mapFile:
            return "no region map baked yet"
        if not os.path.isfile(self._mapFile):
            return "the baked region map %s is gone" % self._mapFile
        stem = os.path.splitext(os.path.basename(filePath))[0]
        name = "%s.regionMap.ptx" % stem
        dest = os.path.join(os.path.dirname(os.path.abspath(filePath)), name)
        try:
            shutil.copyfile(self._mapFile, dest)
        except OSError as exc:
            return "cannot copy the region map: %s" % exc
        spec = Sdf.CreatePrimInLayer(layer, Sdf.Path(self._mapPath))
        attr = spec.attributes.get("usdGen:map:file")
        if attr is None:
            attr = Sdf.AttributeSpec(spec, "usdGen:map:file",
                                     Sdf.ValueTypeNames.Asset)
        attr.default = Sdf.AssetPath("./" + name)
        return ""

    def exportCenterCurves(self, filePath, level=0):
        """Write the center curves of one level (0 = every tube) to a file.

        Honest about the result (SS-04): a bare name gets `.usda`, a
        directory or a non-USD extension is refused up front, and success
        is reported only when Export() said so AND the file is on disk.
        Every failure is one 'error' status line; nothing raises.
        """
        from pxr import Sdf, Usd
        from . import pomadeBridge
        if self._model is None:
            self._status("Pomade: no model to export", "error")
            return False
        path = str(filePath)
        # The folder test comes first: a folder has no extension, and
        # suffixing it would quietly write <folder>.usda beside it.
        if not path:
            self._status("Pomade: no file to export to", "error")
            return False
        if os.path.isdir(path) or path.endswith(("/", "\\")):
            self._status("Pomade: cannot write %s: it is a folder" % path,
                         "error")
            return False
        if not os.path.splitext(path)[1]:
            path += ".usda"
        if os.path.isdir(path):
            self._status("Pomade: cannot write %s: it is a folder" % path,
                         "error")
            return False
        ext = os.path.splitext(path)[1][1:]
        if Sdf.FileFormat.FindByExtension(ext) is None:
            # Export would happily write usda text into x.txt, which
            # Import then cannot open.
            self._status("Pomade: cannot write %s: .%s is not a USD format "
                         "(use .usda or .usdc)" % (path, ext), "error")
            return False
        try:
            tubeIds = self._tubeIdsAtLevel(level)
        except (RuntimeError, ValueError) as exc:
            self._status("Pomade: cannot read the tubes to export: %s"
                         % _errorText(exc, self.lastError()), "error")
            return False
        if not tubeIds:
            self._status("Pomade: no tubes to export" if int(level) <= 0
                         else "Pomade: no tubes to export at level %d"
                         % int(level), "warning")
            return False
        try:
            out = Usd.Stage.CreateInMemory()
            pomadeBridge.exportCenterCurves(out, self.dll, self._model,
                                           tubeIds, "/PomadeExport")
            written = bool(out.GetRootLayer().Export(path))
        except (RuntimeError, ValueError) as exc:
            # Sdf raises Tf errors (RuntimeError) for an unwritable path.
            self._status("Pomade: cannot write %s: %s"
                         % (path, _errorText(exc, self.lastError())),
                         "error")
            return False
        if not written or not os.path.isfile(path):
            self._status("Pomade: cannot write %s" % path, "error")
            return False
        self._status("Pomade: exported %d center curve(s) to %s"
                     % (len(tubeIds), path))
        return True

    def importCurves(self, filePath, parentTubeId=0):
        """Import every BasisCurves in a file as locked child tubes.

        Atomic and honest (SS-04): the file is prechecked with
        Sdf.Layer.FindOrOpen (a missing file or a non-USD one is a status
        line, not Tf noise), every curve is validated before the model is
        touched, and the tubes are added inside one "Import curves"
        gesture, so a failure part-way (a C++ refusal on the third curve)
        cancels back to the press-time model and leaves no undo step.
        Returns True only when every curve landed.
        """
        from pxr import Sdf, Usd, UsdGeom
        from . import pomadeBridge
        if self._model is None:
            self._status("Pomade: no model to import into", "error")
            return False
        path = str(filePath)
        parentTubeId = int(parentTubeId)
        if not os.path.isfile(path):
            self._status("Pomade: cannot import %s: %s"
                         % (path, "it is a folder" if os.path.isdir(path)
                            else "no such file"), "error")
            return False
        try:
            layer = Sdf.Layer.FindOrOpen(path)
            stage = Usd.Stage.Open(layer) if layer is not None else None
        except RuntimeError as exc:
            self._status("Pomade: cannot import %s: %s"
                         % (path, _errorText(exc, "")), "error")
            return False
        if stage is None:
            self._status("Pomade: cannot import %s: not a USD file" % path,
                         "error")
            return False
        curves = []
        try:
            for prim in stage.Traverse():
                if prim.IsA(UsdGeom.BasisCurves):
                    curves.extend(pomadeBridge.readBasisCurves(
                        stage, prim.GetPath()))
        except (RuntimeError, ValueError) as exc:
            self._status("Pomade: cannot import %s: %s"
                         % (path, _errorText(exc, "")), "error")
            return False
        if not curves:
            self._status("Pomade: %s carries no BasisCurves" % path, "error")
            return False
        if not self.beginGesture("Import curves"):
            # beginGesture already said why (another edit is open).
            self._status("Pomade: cannot import %s while another edit is "
                         "open" % path, "error")
            return False
        try:
            ids = pomadeBridge.importCurvesAsLockedTubes(
                self.dll, self._model, parentTubeId, curves)
        except (RuntimeError, ValueError) as exc:
            detail = _errorText(exc, self.lastError())
            self._rollBackGesture()
            self._status("Pomade: cannot import %s: %s" % (path, detail),
                         "error")
            return False
        except BaseException:
            self._rollBackGesture()
            raise
        self.endGesture()
        self.publish(PUBLISH_ALL)
        self.enqueueCommit()
        self._status("Pomade: Imported %d curve%s under tube %d"
                     % (len(ids), "" if len(ids) == 1 else "s",
                        parentTubeId))
        return True

    def _rollBackGesture(self):
        """Cancel the open gesture and publish what it restored."""
        dirty = self.cancelGesture()
        if dirty:
            self.publish(dirty)

    def _tubeIdsAtLevel(self, level):
        from . import pomadeBridge, pomadeHierarchy
        ids = pomadeBridge.readTubeIds(self.dll, self._model)
        if int(level) <= 0:
            return ids
        return [t for t in ids
                if pomadeHierarchy.tubeLevel(self.dll, self._model, t) ==
                int(level)]
