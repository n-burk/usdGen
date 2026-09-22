# usdGenTonicTools.tonicSession -- model, committer and bake lifetime
# (plan/18 section 3.1).
#
# Qt-free: pure ctypes over tonicLib/tonicLibStage plus pxr for the live
# sublayer and the file commands. Everything the viewport controller and the
# workspace dock do to the model goes through here, so there is one place
# that knows a model exists, one place that publishes it, and one place that
# swaps a committed layer in.
#
# Three rules this module exists to keep:
#   * Tonic_Publish is the ONLY way the viewport learns about a change
#     (plan/18 section 2.1), so every mutating helper here publishes.
#   * the stage is never touched from inside a gesture (plan/17 section 1.3):
#     release enqueues, the idle pump swaps.
#   * the gesture bracket is one undo step (plan/18 section 3.2), so a
#     200-sample drag pushes one snapshot instead of 200.
from __future__ import annotations

import ctypes
import os
import shutil
import tempfile
import time

from . import tonicLib

# What Tonic_Publish is asked for outside a gesture. Inside one the session
# passes TONIC_DIRTY_PENDING so only the leaf locators the model marked are
# republished -- that precision is the whole point of the V0 dirty bitmask,
# and asking for everything on every move would spend the move budget on
# levels nobody touched.
PUBLISH_PENDING = tonicLib.TONIC_DIRTY_PENDING
PUBLISH_ALL = tonicLib.TONIC_DIRTY_ALL

DEFAULT_GROOM_PATH = "/TonicGroom"


def bakeDirectory():
    """Where this process's bake worker writes its versioned maps.

    Per PROCESS, not per box. The worker sweeps stale `<base>.v<n>.ptx`
    files out of its directory (plan/18 section 7 G5) and map versions
    restart at 1 in every model, so two Tonic processes that shared one
    directory would delete each other's live map: two usdviews on one
    workstation, or two T3 tests whose bake threads overlap while the
    first process is still tearing down. USDGENTONIC_BAKE_DIR overrides
    it outright, which is how a test pins the directory it will inspect.
    """
    override = os.environ.get("USDGENTONIC_BAKE_DIR")
    if override:
        return override
    return os.path.join(tempfile.gettempdir(), "usdGenTonicBake",
                        "p%d" % os.getpid())


class TonicSession:
    """The live Tonic model plus its commit and bake workers.

    `state` is the shared TonicToolState; `usdviewApi` may be None, which is
    how the headless T0 test drives a session with a fake library.
    """

    def __init__(self, state, usdviewApi=None, statusFn=None):
        self._state = state
        self._api = usdviewApi
        self._statusFn = statusFn
        self._model = None
        self._committer = None
        self._bake = None
        self._stage = None
        # The anonymous live layer must stay referenced here: the session
        # layer holds it only by identifier, and SdfLayer::Find (which
        # Tonic_CommitterSwap uses) can only find a layer that is alive.
        self._liveLayer = None
        self._liveId = ""
        self._groomPath = DEFAULT_GROOM_PATH
        self._descPath = ""
        self._scalpPath = ""
        self._mapPath = DEFAULT_GROOM_PATH + "/RegionMap"
        self._mapFile = ""
        self._detached = False
        self._gestureDepth = 0
        self._lastSwapCode = tonicLib.TONIC_COMMITTER_NOTHING_PENDING
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
        self._publishHook = None
        # The viewport owns the Qt timer; this callback is the Qt-free wake
        # seam used whenever a worker item is queued from a dock action.
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
        return self.dll.Tonic_SetDisplayScale(
            self._model, ctypes.c_float(value)) == tonicLib.TONIC_OK

    def displayScale(self):
        """What the model currently scales its overlays by; 0.0 if unset."""
        if self._model is None:
            return 0.0
        out = ctypes.c_float(0.0)
        if self.dll.Tonic_GetDisplayScale(
                self._model, ctypes.byref(out)) != tonicLib.TONIC_OK:
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
    def gestureActive(self):
        return self._gestureDepth > 0

    @property
    def dll(self):
        lib = self._state.lib
        return lib.dll if lib is not None else None

    @property
    def stageLib(self):
        """The tonicLibStage binding over the same DLL, bound on demand.

        The per-tube half of the ABI (plan/18 section 7 G2) lives in
        tonicApiStage.h, so the Tube and Fill loops and their panels need
        it; binding it here keeps one StageLibrary per session instead of
        one per caller. None before the library loads.
        """
        if self._stageLib is None and self._state.lib is not None:
            from . import tonicLibStage
            self._stageLib = tonicLibStage.StageLibrary(self._state.lib)
        return self._stageLib

    def report(self, text):
        """One status line to usdview (or to the test's recorder)."""
        self._status(text)

    def setStatusSink(self, sink):
        """Send status lines to `sink` instead of usdview's status bar.

        usdview's PrintStatus goes to a widget; a test needs the text, and
        so will the workspace's message area.
        """
        self._statusFn = sink

    def setPublishHook(self, hook):
        """Call `hook` after every publish that reached the indices.

        The workspace dock wires its refresh here (plan/18 section 3.5):
        the status strip and the warnings are derived from the model, so
        they go stale exactly when the viewport would.
        """
        self._publishHook = hook

    def setIdleHook(self, hook):
        """Wake the owning viewport after deferred work is queued.

        The session stays Qt-free.  The viewport supplies ``scheduleIdle``
        once its timer is ready and removes it before teardown.
        """
        self._idleHook = hook

    def _wakeIdle(self):
        hook = self._idleHook
        if hook is not None:
            hook()

    def _status(self, text):
        if self._statusFn is not None:
            self._statusFn(text)
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
            self._state.lib = tonicLib.Library()
        return self._state.lib

    def _createModel(self):
        dll = self.dll
        model = ctypes.c_void_p(None)
        if dll.Tonic_Create(ctypes.byref(model)) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return None
        return model

    def _startWorkers(self, stage):
        """Committer + live sublayer + bake worker. True on success."""
        from pxr import Sdf
        dll = self.dll
        committer = ctypes.c_void_p(None)
        desc = self._descPath.encode("utf-8") if self._descPath else None
        if dll.Tonic_CommitterCreate(self._model,
                                     self._groomPath.encode("utf-8"),
                                     desc,
                                     ctypes.byref(committer)) \
                != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return False
        self._committer = committer
        if self._scalpPath:
            dll.Tonic_CommitterSetScalpPath(committer,
                                            self._scalpPath.encode("utf-8"))
        # The model needs the groom path too, for one reason that has
        # nothing to do with committing: the scene index hides the
        # committed <groom>/Guides prim while this model is live
        # (plan/18 section 2.4a). That prim is the PREVIOUS commit's
        # curves -- plain white BasisCurves with no displayColor -- and
        # Storm draws them straight over the tubes the tool is editing.
        # It is a Hydra visibility opinion only, so the amplifier still
        # reads them off the stage and no cook is starved.
        setGroom = getattr(dll, "Tonic_SetGroomPath", None)
        if setGroom is not None:
            setGroom(self._model, self._groomPath.encode("utf-8"))
        self._liveLayer = Sdf.Layer.CreateAnonymous("usdGenTonic-live")
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
        if dll.Tonic_BakeCreate(self._model, outDir.encode("utf-8"), None,
                                ctypes.byref(bake)) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
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
        self._mapPath = self._groomPath + "/RegionMap"
        if not self.bindScalpFromStage(self._scalpPath, stage):
            self.dll.Tonic_Destroy(model)
            self._model = None
            return False
        dll = self.dll
        dll.Tonic_SetSnapRadius(model, ctypes.c_float(0.05))
        dll.Tonic_SetMirrorX(model, 1 if self._state.mirrorX else 0)
        if not self._startWorkers(stage):
            return False
        if dll.Tonic_Activate(model) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
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
        Tonic_Hydrate (plan/18 section 7 G3), the scalp binding is re-made
        from the stage, and the committer starts from the hydrated version.
        """
        from . import tonicLibStage
        self._ensureLibrary()
        stage = stage if stage is not None else self._apiStage()
        if stage is None:
            self._status("Tonic: no stage to hydrate from")
            return False
        prim = stage.GetPrimAtPath(str(groomPath))
        if not prim:
            self._status("Tonic: no groom at %s" % groomPath)
            return False
        if self._model is not None:
            self.deactivate()
        model = self._createModel()
        if model is None:
            return False
        self._model = model
        self._groomPath = str(groomPath)
        self._mapPath = self._groomPath + "/RegionMap"
        identifier = layerOrStage or stage.GetRootLayer().identifier
        stageLib = tonicLibStage.StageLibrary(self._state.lib)
        try:
            tubes, guides, imported = stageLib.hydrate(
                model, identifier, self._groomPath)
        except RuntimeError as exc:
            self._status("Tonic: %s" % exc)
            self.dll.Tonic_Destroy(model)
            self._model = None
            return False
        # Tonic_Hydrate has ALREADY bound the scalp and restored the scalp
        # graph, its region loops and the per-face region ids from the
        # committed ScalpGraph prim. Re-binding here would throw all of
        # that away -- TonicModel::BindScalp clears _graph, _maps and
        # _loops on every call -- and it is why a hydrated groom used to
        # come up with a uniformly dark-red scalp: every face rasterised
        # as uncovered because there were no region loops left to cover
        # it. The path is recorded for the committer and nothing else.
        scalpPath = self._scalpPathFromGroom(stage, prim)
        if scalpPath:
            self._scalpPath = scalpPath
            self.recordScalpCenter(scalpPath, stage)
        if not self._startWorkers(stage):
            return False
        if self.dll.Tonic_Activate(model) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return False
        self._state.activated = True
        self._state.groomRoot = self._groomPath
        # Hydrate restores native Output settings from the committed groom;
        # mirror them before the Output panel is rebuilt.
        self.outputSettings()
        self.publish(PUBLISH_ALL)
        self._status("Tonic: hydrated %d tube(s), %d guide(s), %d imported"
                     % (tubes, guides, imported))
        return True

    @staticmethod
    def _scalpPathFromGroom(stage, groomPrim):
        """The scalp the committed groom points at, or ""."""
        for name in ("usdGen:tonic:scalp", "usdGen:scalp"):
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
        """(flatPoints, counts, indices) for the mesh at `scalpPath`, or None."""
        prim = stage.GetPrimAtPath(str(scalpPath))
        if not prim:
            self._status("Tonic: no prim at %s" % scalpPath)
            return None
        points = prim.GetAttribute("points").Get() or []
        counts = prim.GetAttribute("faceVertexCounts").Get() or []
        indices = prim.GetAttribute("faceVertexIndices").Get() or []
        if not points or not counts:
            self._status("Tonic: %s is not a mesh with points" % scalpPath)
            return None
        flat = [float(c) for p in points for c in (p[0], p[1], p[2])]
        return flat, counts, indices

    def recordScalpCenter(self, scalpPath, stage=None):
        """Note the scalp's bounding-box centre without touching the model.

        The centre is what the camera frames on; binding is what clears the
        scalp graph. Hydrate needs the first and must not do the second.
        """
        stage = stage if stage is not None else self._apiStage()
        if stage is None:
            return False
        mesh = self._scalpMesh(scalpPath, stage)
        if mesh is None:
            return False
        flat = mesh[0]
        self._scalpCenter = tuple(
            0.5 * (min(flat[a::3]) + max(flat[a::3])) for a in range(3))
        return True

    def bindScalpFromStage(self, scalpPath, stage=None):
        """Bind the mesh at `scalpPath` as the model's scalp.

        NOTE: TonicModel::BindScalp clears the scalp graph, its region
        loops and the rasterised face ids. Call it to attach a scalp, never
        to re-attach one the model already holds.
        """
        stage = stage if stage is not None else self._apiStage()
        if stage is None:
            self._status("Tonic: no stage")
            return False
        mesh = self._scalpMesh(scalpPath, stage)
        if mesh is None:
            return False
        flat, counts, indices = mesh
        pts = (ctypes.c_float * len(flat))(*flat)
        cnt = (ctypes.c_int * len(counts))(*[int(c) for c in counts])
        idx = (ctypes.c_int * len(indices))(*[int(i) for i in indices])
        if self.dll.Tonic_BindScalp(self._model, pts, len(flat), cnt,
                                    len(counts), idx,
                                    len(indices)) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return False
        self._scalpCenter = tuple(
            0.5 * (min(flat[a::3]) + max(flat[a::3])) for a in range(3))
        return True

    def deactivate(self):
        """Tear the model down; the stage keeps whatever was committed."""
        dll = self.dll
        if dll is not None:
            if self._bake is not None:
                dll.Tonic_BakeDestroy(self._bake)
            if self._committer is not None:
                dll.Tonic_CommitterDestroy(self._committer)
            if self._model is not None:
                dll.Tonic_Deactivate(self._model)
                dll.Tonic_Destroy(self._model)
        self._removeLiveSublayer()
        self._removeBakeDirectory()
        self._model = None
        self._committer = None
        self._bake = None
        self._liveLayer = None
        self._liveId = ""
        self._gestureDepth = 0
        self._scalpCenter = None
        self._state.activated = False

    def _removeBakeDirectory(self):
        """Take this process's per-run bake directory away with it.

        Only the directory this session made, and only once the worker
        is destroyed (deactivate does that first), so nothing is racing
        for the files. An explicit USDGENTONIC_BAKE_DIR belongs to
        whoever set it and is left alone.
        """
        if not self._bakeDir or os.environ.get("USDGENTONIC_BAKE_DIR"):
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
        self.dll.Tonic_CommitterDetach(self._committer)
        self._removeLiveSublayer()
        self._stage = None
        self._detached = True
        return True

    def reattach(self, stage=None):
        """A new stage arrived: re-host the live sublayer and re-commit."""
        if self._committer is None:
            return False
        stage = stage if stage is not None else self._apiStage()
        if stage is None:
            return False
        self._stage = stage
        if self._liveLayer is not None:
            paths = list(stage.GetSessionLayer().subLayerPaths)
            if self._liveId not in paths:
                stage.GetSessionLayer().subLayerPaths.insert(0, self._liveId)
        self.dll.Tonic_CommitterReattach(self._committer)
        self._detached = False
        if self._scalpPath:
            self.bindScalpFromStage(self._scalpPath, stage)
        self.enqueueCommit()
        self.publish(PUBLISH_ALL)
        return True

    # -- publication -------------------------------------------------------

    def publish(self, dirtyMask=PUBLISH_PENDING):
        """Tonic_Publish; returns the number of scene indices refreshed.

        The default is the model's own pending bits (see PUBLISH_PENDING).
        0 is a normal answer in a headless process or a usdview with the
        scene index disabled; -1 is the error.
        """
        if self._model is None:
            return 0
        count = self.dll.Tonic_Publish(self._model, int(dirtyMask))
        if count < 0:
            self._status("Tonic: " + self.lastError())
            return 0
        self._state.generation = int(self.modelVersion)
        if self._publishHook is not None:
            self._publishHook()
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
        entry = getattr(self.dll, "Tonic_ClearGeneratedCurves", None)
        if entry is None:
            self._status("Tonic: clear generated curves is unavailable in "
                         "this library")
            return False
        ok = int(entry(self._model)) == tonicLib.TONIC_OK
        if not ok:
            self._status("Tonic: " + self.lastError())
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
        ok = self.dll.Tonic_GenerateGuides(
            self._model, ctypes.c_float(1.0)) == tonicLib.TONIC_OK
        if not ok:
            self._status("Tonic: " + self.lastError())
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
        entry = getattr(self.dll, "Tonic_SetGeneratedCurvesVisible", None)
        if entry is None:
            self._status("Tonic: generated-curve visibility is unavailable "
                         "in this library")
            return False
        if int(entry(self._model, 1 if visible else 0)) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return False
        self._state.showGeneratedCurves = visible
        self.publish()
        return True

    def generatedCurvesVisible(self):
        """Return the native guide visibility, when the model exposes it."""
        if self._model is None:
            return bool(self._state.showGeneratedCurves)
        entry = getattr(self.dll, "Tonic_GetGeneratedCurvesVisible", None)
        if entry is None:
            return bool(self._state.showGeneratedCurves)
        visible = ctypes.c_int(0)
        if int(entry(self._model, ctypes.byref(visible))) != tonicLib.TONIC_OK:
            return bool(self._state.showGeneratedCurves)
        self._state.showGeneratedCurves = bool(visible.value)
        return bool(visible.value)

    # -- Output description ----------------------------------------------

    def outputSettingsAvailable(self):
        """Whether this model library exposes committed Output settings."""
        return (self._model is not None and
                getattr(self.dll, "Tonic_GetOutputSettings", None) is not None
                and getattr(self.dll, "Tonic_SetOutputSettings", None)
                is not None)

    def outputSettings(self):
        """Return ``(enabled, densityMultiplier, strandWidth)``."""
        enabled = bool(getattr(self._state, "outputEnabled", False))
        multiplier = float(getattr(self._state,
                                   "outputDensityMultiplier", 1.0))
        width = float(getattr(self._state, "outputStrandWidth", 0.01))
        if self._model is None:
            return enabled, multiplier, width
        entry = getattr(self.dll, "Tonic_GetOutputSettings", None)
        if entry is None:
            return enabled, multiplier, width
        nativeEnabled = ctypes.c_int(0)
        nativeMultiplier = ctypes.c_float(multiplier)
        nativeWidth = ctypes.c_float(width)
        if int(entry(self._model, ctypes.byref(nativeEnabled),
                     ctypes.byref(nativeMultiplier),
                     ctypes.byref(nativeWidth))) != tonicLib.TONIC_OK:
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

    def _outputCollision(self):
        """Reject artist-owned Output prims before native output is enabled."""
        stage = self._stage if self._stage is not None else self._apiStage()
        if stage is None:
            return True
        markerName = "usdGen:tonic:outputOwned"
        for suffix in ("Output", "OutputCurves", "OutputRegionMap"):
            prim = stage.GetPrimAtPath(self._groomPath + "/" + suffix)
            if not prim:
                continue
            marker = prim.GetAttribute(markerName)
            if not marker or marker.Get() is not True:
                self._status("Tonic: cannot build Output description; "
                             "%s is not Tonic-owned (preserving it)"
                             % prim.GetPath())
                return False
        return True

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
        entry = getattr(self.dll, "Tonic_SetOutputSettings", None) \
            if self._model is not None else None
        if self._model is not None and entry is None:
            self._status("Tonic: Output description is unavailable in this "
                         "library")
            return False
        if entry is not None and int(entry(
                self._model, 1 if enabled else 0,
                ctypes.c_float(densityMultiplier),
                ctypes.c_float(strandWidth))) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
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
        amplified = getattr(self.dll, "Tonic_SetAmplifiedHair", None) \
            if self._model is not None else None
        if amplified is not None:
            if int(amplified(self._model, 1)) != tonicLib.TONIC_OK:
                self._status("Tonic: " + self.lastError())
                return False
            self._state.showAmplifiedHair = True
            self.publish()
        if not self.enqueueCommit():
            return False
        self._status("Tonic: Sparse groom and PTex bake queued (%g density, %g width)"
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
        if self.dll.Tonic_BeginGesture(
                self._model, str(label).encode("utf-8")) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
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
        entry = getattr(self.dll, "Tonic_CommitterCancelCooks", None)
        if entry is None:
            return 0
        moved = int(entry(self._committer))
        return max(moved, 0)

    def endGesture(self):
        if self._model is None or self._gestureDepth <= 0:
            return False
        ok = self.dll.Tonic_EndGesture(self._model) == tonicLib.TONIC_OK
        self._gestureDepth = max(self._gestureDepth - 1, 0)
        if not ok:
            self._status("Tonic: " + self.lastError())
        return ok

    def cancelGesture(self):
        """Restore the press-time base; returns the dirty bits to publish."""
        if self._model is None or self._gestureDepth <= 0:
            return 0
        dirty = ctypes.c_uint(0)
        ok = self.dll.Tonic_CancelGesture(
            self._model, ctypes.byref(dirty)) == tonicLib.TONIC_OK
        self._gestureDepth = max(self._gestureDepth - 1, 0)
        if not ok:
            self._status("Tonic: " + self.lastError())
            return 0
        return int(dirty.value)

    # -- undo / redo -------------------------------------------------------

    def undo(self):
        if self._model is None:
            return False
        dirty = ctypes.c_uint(0)
        if self.dll.Tonic_Undo(self._model,
                               ctypes.byref(dirty)) != tonicLib.TONIC_OK:
            self._status("Tonic: nothing to undo")
            return False
        self.publish(int(dirty.value))
        self.enqueueCommit()
        return True

    def redo(self):
        if self._model is None:
            return False
        dirty = ctypes.c_uint(0)
        if self.dll.Tonic_Redo(self._model,
                               ctypes.byref(dirty)) != tonicLib.TONIC_OK:
            self._status("Tonic: nothing to redo")
            return False
        self.publish(int(dirty.value))
        self.enqueueCommit()
        return True

    def undoLabel(self, depth=0):
        if self._model is None:
            return ""
        buf = ctypes.create_string_buffer(256)
        if self.dll.Tonic_GetUndoLabel(self._model, int(depth), buf,
                                       256) != tonicLib.TONIC_OK:
            return ""
        return buf.value.decode("utf-8", "replace")

    # -- picking -----------------------------------------------------------

    def pickItem(self, camera, x, y, radiusPx, kindMask):
        """K11: the selection item under a pixel, or None.

        Returns {"kind", "id", "subId", "subSubId"} with the STABLE ids the
        selection stores, which is why this is Tonic_PickItem and not
        Tonic_Pick (whose index is a candidate ordinal).
        """
        if self._model is None or camera is None:
            return None
        hit = ctypes.c_int(0)
        kind = ctypes.c_uint(0)
        ident = ctypes.c_int(-1)
        subId = ctypes.c_int(-1)
        subSubId = ctypes.c_int(-1)
        if self.dll.Tonic_PickItem(
                self._model, camera.viewProjArray(), camera.width,
                camera.height, ctypes.c_float(x), ctypes.c_float(y),
                ctypes.c_float(radiusPx), ctypes.c_uint(int(kindMask)),
                ctypes.byref(hit), ctypes.byref(kind), ctypes.byref(ident),
                ctypes.byref(subId),
                ctypes.byref(subSubId)) != tonicLib.TONIC_OK:
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
        if self.dll.Tonic_Raycast(self._model, o, d, ctypes.byref(hit),
                                  ctypes.byref(face), uv, xyz,
                                  nrm) != tonicLib.TONIC_OK:
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
        return self.dll.Tonic_SetHover(
            self._model, ctypes.c_uint(int(kind)), int(ident), int(subId),
            int(subSubId)) == tonicLib.TONIC_OK

    def selectRect(self, camera, x0, y0, x1, y1, kindMask,
                   mode=tonicLib.TONIC_SELECT_SET):
        if self._model is None or camera is None:
            return False
        return self.dll.Tonic_SelectRect(
            self._model, camera.viewProjArray(), camera.width, camera.height,
            ctypes.c_float(x0), ctypes.c_float(y0), ctypes.c_float(x1),
            ctypes.c_float(y1), ctypes.c_uint(int(kindMask)),
            int(mode)) == tonicLib.TONIC_OK

    def selectPolygon(self, camera, points, kindMask,
                      mode=tonicLib.TONIC_SELECT_SET):
        """Select against physical-pixel (x, y) lasso points."""
        if self._model is None or camera is None or len(points) < 3:
            return False
        flat = [float(value) for point in points for value in point]
        xy = (ctypes.c_float * len(flat))(*flat)
        return self.dll.Tonic_SelectPolygon(
            self._model, camera.viewProjArray(), camera.width, camera.height,
            xy, int(len(points)), ctypes.c_uint(int(kindMask)),
            int(mode)) == tonicLib.TONIC_OK

    def select(self, kind, ids, subIds=None, subSubIds=None,
               mode=tonicLib.TONIC_SELECT_SET):
        if self._model is None:
            return False
        n = len(ids)
        arrays = []
        for source in (ids, subIds, subSubIds):
            if source is None:
                arrays.append(None)
            else:
                arrays.append((ctypes.c_int * n)(*[int(v) for v in source]))
        entry = {tonicLib.TONIC_SELECT_SET: self.dll.Tonic_SelectSet,
                 tonicLib.TONIC_SELECT_ADD: self.dll.Tonic_SelectAdd,
                 tonicLib.TONIC_SELECT_TOGGLE: self.dll.Tonic_SelectToggle,
                 }[int(mode)]
        return entry(self._model, ctypes.c_uint(int(kind)), arrays[0],
                     arrays[1], arrays[2], n) == tonicLib.TONIC_OK

    def clearSelection(self, kindMask=0):
        if self._model is None:
            return False
        return self.dll.Tonic_SelectClear(
            self._model, ctypes.c_uint(int(kindMask))) == tonicLib.TONIC_OK

    def readSelection(self, kind):
        """[(id, subId, subSubId)] of one kind, ascending."""
        if self._model is None:
            return []
        count = ctypes.c_int(0)
        if self.dll.Tonic_ReadSelection(self._model, ctypes.c_uint(int(kind)),
                                        None, None, None, 0,
                                        ctypes.byref(count)) \
                != tonicLib.TONIC_OK:
            return []
        n = int(count.value)
        if n <= 0:
            return []
        ids = (ctypes.c_int * n)()
        subIds = (ctypes.c_int * n)()
        subSubIds = (ctypes.c_int * n)()
        if self.dll.Tonic_ReadSelection(self._model, ctypes.c_uint(int(kind)),
                                        ids, subIds, subSubIds, n,
                                        ctypes.byref(count)) \
                != tonicLib.TONIC_OK:
            return []
        return [(int(ids[i]), int(subIds[i]), int(subSubIds[i]))
                for i in range(min(n, int(count.value)))]

    def selectionCount(self, kindMask=0):
        if self._model is None:
            return 0
        return int(self.dll.Tonic_GetSelectionCount(
            self._model, ctypes.c_uint(int(kindMask))))

    # -- commit + bake -----------------------------------------------------

    def enqueueCommit(self):
        """Ask the worker for a layer of the model's latest version."""
        if self._committer is None:
            return False
        if self.dll.Tonic_CommitterEnqueue(self._committer, 0, 0, 0,
                                           None) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return False
        self._wakeIdle()
        return True

    def rasterise(self):
        """K3 over the current graph (gesture end only, plan/17 4.2)."""
        if self._model is None:
            return False
        if self.dll.Tonic_Rasterise(self._model) != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return False
        return True

    def ensureRegionTubes(self):
        """Give every closed region its L1 tube stub (plan/18 section 7 G14).

        plan/17 section 5.1: "each closed region gets a tube stub". Until
        one exists the groom cannot be committed at all -- the committer
        refuses a snapshot with no tubes -- so an artist who only drew a
        graph would save nothing. Called after a graph gesture, once K3 has
        claimed the faces Tonic_BuildTubeFromRegion needs (the rasterise
        also re-attaches the existing tubes to the renumbered regions).

        Returns the number of stubs built. The model holds one L1 tube per
        region, so a scalp with five regions ends with five L1 roots.
        """
        if self._model is None:
            return 0
        regions, _uncovered, _intersected = self.regionStats()
        if regions <= 0:
            return 0
        from . import tonicTube
        # Zero is the BuildTubeFromRegion Auto sentinel: it makes one
        # column per authored region CV instead of averaging a radial default.
        # Existing roots are skipped below, so changing this preference never
        # regenerates or reshapes a sculpted tube.
        ringVerts = tonicTube.regionRingVerts(
            self._state.panels.get("tube", {}).get(
                "ringCvCount", tonicTube.DEFAULT_REGION_RING_VERTS))
        built = 0
        for regionId in range(regions):
            if int(self.dll.Tonic_TubeForRegion(self._model, regionId)) >= 0:
                continue                 # this region already has its tube
            # A region that claims no face (it lost its interior to a link,
            # or K3 has not reached it) refuses; the rest still build.
            if self.dll.Tonic_BuildTubeFromRegion(
                    self._model, regionId, tonicTube.DEFAULT_RINGS,
                    ringVerts,
                    ctypes.c_float(tonicTube.DEFAULT_LENGTH)) \
                    == tonicLib.TONIC_OK:
                built += 1
        if built:
            self._status("Tonic: %d tube stub%s built"
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
        if self.dll.Tonic_BakeSetOptions(self._bake, resLog2,
                                         self._bakeLevels) \
                != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return False
        return True

    def rebake(self):
        """Enqueue a region-map bake that carries the hierarchy."""
        if self._bake is None:
            return False
        entry = getattr(self.dll, "Tonic_BakeEnqueueLevels", None)
        status = (entry(self._bake) if entry is not None
                  else self.dll.Tonic_BakeEnqueue(self._bake))
        if status != tonicLib.TONIC_OK:
            self._status("Tonic: " + self.lastError())
            return False
        self._state.mapVersion = int(self.dll.Tonic_GetMapVersion(self._model))
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
            code = self.dll.Tonic_CommitterSwap(
                self._committer, self._liveId.encode("utf-8"),
                1 if self.gestureActive else 0)
            self._lastSwapCode = int(code)
            swapped = code == tonicLib.TONIC_COMMITTER_SWAPPED
            if swapped and self._pendingOutputReveal:
                self._pendingOutputReveal = False
                self.outputSettings()
        if self._bake is not None:
            version = ctypes.c_ulonglong(0)
            path = ctypes.create_string_buffer(4096)
            while self.dll.Tonic_BakeTakeCompleted(
                    self._bake, ctypes.byref(version), path, 4096) == 1:
                filePath = path.value.decode("utf-8")
                if self.dll.Tonic_BakeSwap(
                        self._bake, version, self._liveId.encode("utf-8"),
                        self._mapPath.encode("utf-8"),
                        filePath.encode("utf-8")) == tonicLib.TONIC_OK:
                    self._state.bakedVersion = int(version.value)
                    self._mapFile = filePath
                    baked += 1
            pending = int(self.dll.Tonic_BakePendingVersion(self._bake))
            completed = int(self.dll.Tonic_BakeCompletedVersion(self._bake))
            self._bakeInFlight = pending > completed
        return {"swapped": swapped, "swapCode": self._lastSwapCode,
                "baked": baked, "pending": self.hasPendingWork()}

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
        return self.pendingVersion > self.committedVersion

    # -- status ------------------------------------------------------------

    @property
    def modelVersion(self):
        if self._model is None:
            return 0
        return int(self.dll.Tonic_GetVersion(self._model))

    @property
    def committedVersion(self):
        if self._committer is None:
            return 0
        return int(self.dll.Tonic_CommitterCommittedVersion(self._committer))

    @property
    def pendingVersion(self):
        if self._committer is None:
            return 0
        return int(self.dll.Tonic_CommitterPendingVersion(self._committer))

    @property
    def lastSwapMs(self):
        if self._committer is None:
            return 0.0
        return float(self.dll.Tonic_CommitterLastSwapMs(self._committer))

    @property
    def partialMode(self):
        if self._committer is None:
            return 0
        return int(self.dll.Tonic_CommitterPartialMode(self._committer))

    def fallbackReason(self):
        if self._model is None:
            return ""
        text = self.dll.Tonic_GetDeviceFallbackReason(self._model)
        return text.decode("utf-8") if text else ""

    def graphCounts(self):
        if self._model is None:
            return (0, 0, 0)
        nodes = ctypes.c_int(0)
        edges = ctypes.c_int(0)
        regions = ctypes.c_int(0)
        self.dll.Tonic_GetGraphCounts(self._model, ctypes.byref(nodes),
                                      ctypes.byref(edges),
                                      ctypes.byref(regions))
        return (int(nodes.value), int(edges.value), int(regions.value))

    def regionStats(self):
        if self._model is None:
            return (0, 0, 0)
        regions = ctypes.c_int(0)
        uncovered = ctypes.c_int(0)
        intersected = ctypes.c_int(0)
        self.dll.Tonic_GetRegionStats(self._model, ctypes.byref(regions),
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
            "gestureActive": self.gestureActive,
            "modelVersion": self.modelVersion,
            "committedVersion": self.committedVersion,
            "pendingVersion": self.pendingVersion,
            "mapVersion": int(self._state.mapVersion),
            "bakedVersion": int(self._state.bakedVersion),
            "lastSwapMs": self.lastSwapMs,
            "partialMode": self.partialMode,
            "swapCode": self._lastSwapCode,
            "fallbackReason": self.fallbackReason(),
            "nodes": nodes, "edges": edges, "regions": regions,
            "uncovered": uncovered, "intersected": intersected,
            "lastMoveMs": float(getattr(self._state, "lastMoveMs", 0.0)),
            # Where the fallback ladder stands (plan/18 section 3.7); the
            # status strip prints it, and 0 means full fidelity.
            "ladderStep": int(getattr(self._state, "ladderStep", 0)),
        }

    # -- file commands -----------------------------------------------------

    def flushLatest(self, timeout=10.0):
        """Swap the newest model layer before a file save can copy it."""
        if self._committer is None:
            return True
        if not self.enqueueCommit():
            return False
        deadline = time.monotonic() + max(float(timeout), 0.1)
        while self.hasPendingWork() and time.monotonic() < deadline:
            self.pump()
            if self.hasPendingWork():
                time.sleep(0.01)
        if self.hasPendingWork():
            self._status("Tonic: timed out waiting for the latest groom "
                         "commit")
            return False
        return True

    def saveGroom(self, filePath, stage=None):
        """Write the live layer to a `.usdc` and re-parent it under the live
        sublayer, then drop the latest baked map beside it.

        The Python twin of TonicSaveGroomAndMaps (tonicCommit.cpp): that one
        is C++-only because it needs a UsdStagePtr, and the C ABI carries no
        stage. Same contract, including the relative `./regionMap.ptx` the
        V0b note requires so a moved groom keeps resolving.
        """
        from pxr import Sdf
        stage = stage if stage is not None else (self._stage
                                                 or self._apiStage())
        if stage is None or self._liveLayer is None:
            self._status("Tonic: nothing to save (no live layer)")
            return False
        if not self.flushLatest():
            return False
        filePath = str(filePath)
        if not filePath.lower().endswith(".usdc"):
            self._status("Tonic: grooms are .usdc, never .usda (S42)")
            return False
        layer = Sdf.Layer.FindOrOpen(filePath) or Sdf.Layer.CreateNew(filePath)
        if layer is None:
            self._status("Tonic: cannot open or create %s" % filePath)
            return False
        layer.TransferContent(self._liveLayer)
        if not layer.Save():
            self._status("Tonic: cannot save %s" % filePath)
            return False
        session = stage.GetSessionLayer()
        ordered = [self._liveId, layer.identifier]
        ordered += [p for p in session.subLayerPaths
                    if p not in (self._liveId, layer.identifier)]
        session.subLayerPaths = ordered
        if self._mapFile and os.path.isfile(self._mapFile):
            self._saveMapBeside(layer, filePath)
        self._status("Tonic: saved %s" % filePath)
        return True

    def _saveMapBeside(self, layer, filePath):
        import shutil
        from pxr import Sdf
        dest = os.path.join(os.path.dirname(os.path.abspath(filePath)),
                            "regionMap.ptx")
        try:
            shutil.copyfile(self._mapFile, dest)
        except OSError as exc:
            self._status("Tonic: cannot copy the region map: %s" % exc)
            return
        spec = Sdf.CreatePrimInLayer(layer, Sdf.Path(self._mapPath))
        attr = spec.attributes.get("usdGen:map:file")
        if attr is None:
            attr = Sdf.AttributeSpec(spec, "usdGen:map:file",
                                     Sdf.ValueTypeNames.Asset)
        attr.default = Sdf.AssetPath("./regionMap.ptx")
        layer.Save()

    def exportCenterCurves(self, filePath, level=0):
        """Write the center curves of one level (0 = every tube) to a file."""
        from pxr import Usd
        from . import tonicBridge
        if self._model is None:
            self._status("Tonic: no model to export")
            return False
        tubeIds = self._tubeIdsAtLevel(level)
        if not tubeIds:
            self._status("Tonic: no tubes at level %d" % level)
            return False
        out = Usd.Stage.CreateInMemory()
        tonicBridge.exportCenterCurves(out, self.dll, self._model, tubeIds,
                                       "/TonicExport")
        out.GetRootLayer().Export(str(filePath))
        self._status("Tonic: exported %d center curve(s) to %s"
                     % (len(tubeIds), filePath))
        return True

    def importCurves(self, filePath, parentTubeId=0):
        """Import every BasisCurves in a file as locked child tubes."""
        from pxr import Usd, UsdGeom
        from . import tonicBridge
        if self._model is None:
            self._status("Tonic: no model to import into")
            return False
        stage = Usd.Stage.Open(str(filePath))
        if stage is None:
            self._status("Tonic: cannot open %s" % filePath)
            return False
        curves = []
        for prim in stage.Traverse():
            if not prim.IsA(UsdGeom.BasisCurves):
                continue
            curves.extend(tonicBridge.readBasisCurves(stage, prim.GetPath()))
        if not curves:
            self._status("Tonic: %s carries no BasisCurves" % filePath)
            return False
        ids = tonicBridge.importCurvesAsLockedTubes(self.dll, self._model,
                                                    int(parentTubeId), curves)
        self.publish(PUBLISH_ALL)
        self.enqueueCommit()
        self._status("Tonic: imported %d curve(s) as tubes %r"
                     % (len(curves), ids))
        return True

    def _tubeIdsAtLevel(self, level):
        from . import tonicBridge, tonicHierarchy
        ids = tonicBridge.readTubeIds(self.dll, self._model)
        if int(level) <= 0:
            return ids
        return [t for t in ids
                if tonicHierarchy.tubeLevel(self.dll, self._model, t) ==
                int(level)]
