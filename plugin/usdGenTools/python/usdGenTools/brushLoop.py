# The brush gesture loop: press / move / release / cancel over the stage.
#
# plan/08-tools.md section 2.1, the four phases: press resolves the camera
# (handed in, resolved once by the viewport filter), picks the surface,
# snapshots the mesh and the base grid, and records the first dab; moves
# recompute from the press-time base and repaint the map overlay (the
# preview scene index; no authored-layer traffic); release bakes once into
# the edit target; Escape aborts with no authored write ever having happened.
#
# Live groom: a move writes the session-scratch paint primvar from the
# working corners when LIVE_GROOM_MIN_INTERVAL passed since the last live
# write, and the viewport arms a single-shot timer that calls
# flushLiveGroom() so the LAST move of a burst lands even when the pointer
# stops. Release clears the scratch before the bake.
#
# One driver per stroke: brushMap.LiveStroke (native when the brush DLL
# loads, pure Python otherwise) records every primary dab, stamps it --
# footprint spill onto neighbour faces included -- into its working grid,
# and reports the faces each call touched. The loop runs no per-stamp
# Python: one live.dab() per press/move, then takeTouched() feeds the
# overlay and live-primvar patches. Release bakes live.commitGrid() (base
# plus every recorded dab, recomputed), so the bake is exactly what the
# working grid showed; the base is never mutated, which is what makes a
# stroke idempotent and abortable.
#
# GeomSubsets: a mesh or a face GeomSubset binds (brushAuthor.ResolveSurface).
# A subset binding paints its parent mesh's primvar, and press masks the
# snapshot to the subset's current faces, so picks outside it miss and no
# stamp, spill or smooth writes another face; flood and every bake keep the
# other faces' composed values.
#
# Qt-free: the viewport filter feeds this loop camera-resolved picks, and
# the T1 suite drives it with a hand-built camera and stage. No Qt import.

import math
import time

try:
    from . import brushAuthor, brushMap, brushPick, brushPreview, brushState
except ImportError:  # file-path test load
    import brushAuthor
    import brushMap
    import brushPick
    import brushPreview
    import brushState

# Dab spacing along a move: no gap exceeds spacing * radius (C++ default).
MOVE_SPACING = 0.5

# Live groom cadence. Moves write at most every LIVE_GROOM_MIN_INTERVAL
# (the cook is async, so the write only has to be cheap); the viewport's
# single-shot flush (LIVE_GROOM_FLUSH_MS) lands the trailing move.
# pollLiveGroom() (headless callers) flushes after LIVE_GROOM_PAUSE of
# stillness or once the interval elapsed. Release bakes regardless.
LIVE_GROOM_MIN_INTERVAL = 0.05
LIVE_GROOM_FLUSH_MS = 60
LIVE_GROOM_PAUSE = 0.06


class FirstDab(object):
    """Where a stroke started (face, u, v): status lines and T3 read it."""

    __slots__ = ("face", "u", "v")

    def __init__(self, face, u, v):
        self.face, self.u, self.v = int(face), float(u), float(v)


class BrushGesture(object):
    """The live stroke: camera, mesh, base and the LiveStroke driver."""

    def __init__(self, camera, snapshot, base, live, prior, first=None):
        self.camera = camera
        self.snapshot = snapshot
        self.base = base
        self.live = live
        # The session displayColor found on press (None on the overlay
        # path), for a no-overlay release/cancel restore.
        self.prior = prior
        self.first = first
        self.lastAttempt = None
        self.lastHit = None
        self.livePrior = None
        self.lastLive = 0.0
        self.lastMoveTime = 0.0
        # Successful live-primvar writes during this stroke.
        self.liveWrites = 0
        # Dirty-face accumulators for the incremental session writes:
        # moves add, successful writes clear. A failed write keeps its
        # faces, so the next success heals the gap instead of dropping
        # a move's paint from the viewport.
        self.previewDirty = set()
        self.previewKey = None
        self.liveDirty = set()

    @property
    def working(self):
        """The live working grid (BrushMap surface), for preview/primvar."""
        return self.live.workingGrid()

    @property
    def stroke(self):
        """Compatibility: status code reads gesture.stroke.dabCount()."""
        return self.live

    def dabCount(self):
        return self.live.dabCount()

    def close(self):
        """Free the driver's native handles. Never raises."""
        try:
            self.live.close()
        except Exception:
            pass


class BrushLoop(object):
    """One press-move-release stroke at a time, over a BrushToolState."""

    def __init__(self, state):
        self._state = state
        if getattr(state, "undoStack", None) is None:
            state.undoStack = brushAuthor.UndoStack()
        self._stage = None
        # Live-primvar writes over the loop's life (T3 counts these: a
        # gesture drops on release, the counter survives it).
        self.liveGroomWrites = 0

    @property
    def state(self):
        return self._state

    def setStage(self, stage):
        """The stage usdview now shows; drops any live gesture, the undos
        and every map overlay (the overlay registry is process-global and
        keyed by prim path, so an old overlay would land on a same-path
        prim of the new stage)."""
        gesture = getattr(self._state, "gesture", None)
        if gesture is not None:
            self._state.gesture = None
            gesture.close()
        self._state.undoStack.clear()
        try:
            brushPreview.ClearAllPreviews(self._stage)
        except Exception:
            pass
        self._stage = stage

    def gestureActive(self):
        return getattr(self._state, "gesture", None) is not None

    def dabCount(self):
        gesture = getattr(self._state, "gesture", None)
        return gesture.dabCount() if gesture is not None else 0

    # -- bind -----------------------------------------------------------

    @staticmethod
    def _isSurfacePrim(prim):
        """A mesh, or a GeomSubset (BindSurface validates it: a non-face
        subset or one off a Mesh fails with its reason, not a skip)."""
        from pxr import UsdGeom
        return bool(UsdGeom.Mesh(prim)) or bool(UsdGeom.Subset(prim))

    def bindFromSelection(self, stage, paths, **options):
        """Bind the first mesh or face GeomSubset selected. (ok, info).

        A subset binds its parent mesh's primvar and paints only its
        faces. With an active description the selection is ignored: the
        bind goes to the description's surface and Maps scope instead."""
        if stage is None:
            return (False, "brush bind needs a stage")
        self._stage = stage
        active = getattr(self._state, "activeDescription", "")
        if active:
            return self.paintToDescription(stage, active)
        if "resolution" not in options:
            options["resolution"] = self._bindResolution()
        for path in paths or []:
            prim = stage.GetPrimAtPath(path)
            if not prim or not prim.IsValid():
                continue
            if not self._isSurfacePrim(prim):
                continue
            binding, error = brushAuthor.BindSurface(
                stage, prim.GetPath(), undoStack=self._state.undoStack,
                **options)
            if binding is None:
                return (False, error)
            old = self._state.binding
            self._state.binding = binding
            self._showBindingDisplay(stage, old)
            return (True, "bound %s -> %s" % (binding.targetPath,
                                              binding.mapPath))
        return (False, "select a mesh (or a face GeomSubset) to bind the "
                "brush to")

    def _bindResolution(self):
        """None (auto texel density) or the manual power-of-two size."""
        if getattr(self._state, "resolutionAuto", True):
            return None
        try:
            return int(self._state.resolution)
        except (TypeError, ValueError):
            return None

    def setupDescriptionFromSelection(self, stage, paths):
        """Define a ready-to-cook description on the first mesh. (ok, info).

        A selected face GeomSubset grows the description from its faces
        only (usdGen:surface targets the subset). The new description
        becomes the paint target: its Maps scope holds the active
        preset's PaintMap."""
        if stage is None:
            return (False, "brush descriptions need a stage")
        if self.gestureActive():
            return (False, "finish the live stroke first")
        self._stage = stage
        for path in paths or []:
            prim = stage.GetPrimAtPath(path)
            if not prim or not prim.IsValid():
                continue
            if not self._isSurfacePrim(prim):
                continue
            descPath, error = brushAuthor.SetupDescription(
                stage, prim.GetPath(), undoStack=self._state.undoStack)
            if descPath is None:
                return (False, error)
            ok, info = self.paintToDescription(stage, descPath)
            if ok:
                return (True, "setup %s" % descPath)
            return (False, "setup %s, but %s" % (descPath, info))
        return (False, "select a mesh (or a face GeomSubset) to grow the "
                "description from")

    def paintToDescription(self, stage, descPath):
        """Paint the active preset into `descPath`'s Maps scope. (ok, info).

        The description names the surface (its usdGen:surface) and owns
        the maps; the selection is ignored. The description stays active
        for later binds even when this bind fails. Paint-to upgrades
        the chain first (a no-op on a current description), so wiring
        never fails for a missing op on an old one."""
        if stage is None:
            return (False, "brush descriptions need a stage")
        if self.gestureActive():
            return (False, "finish the live stroke first")
        self._stage = stage
        from pxr import Sdf
        try:
            descPath = Sdf.Path(str(descPath))
        except (TypeError, ValueError, RuntimeError):
            return (False, "description path %r is not a path"
                    % (descPath,))
        surfacePath, error = brushAuthor.DescriptionSurface(
            stage, descPath)
        if surfacePath is None:
            return (False, error)
        self._state.activeDescription = str(descPath)
        preset = brushAuthor.MaskPresetFor(
            getattr(self._state, "maskPreset", "density"))
        if preset is None:
            return (False, "unknown mask preset %r"
                    % (getattr(self._state, "maskPreset", None),))
        binding, error = brushAuthor.BindSurface(
            stage, surfacePath, mapName=preset.mapName,
            mapsParent=descPath.AppendChild(
                brushAuthor.DESCRIPTION_MAPS_SCOPE),
            primvar=preset.primvar, channels=preset.channels,
            defaultValue=preset.defaultValue,
            resolution=self._bindResolution(),
            undoStack=self._state.undoStack)
        if binding is None:
            return (False, "the bind into %s failed: %s" % (descPath, error))
        old = self._state.binding
        self._state.binding = binding
        self._showBindingDisplay(stage, old)
        wired = ""
        if preset.id in brushAuthor.PAINT_WIRING:
            upgraded, upgradeError = brushAuthor.SetupDescription(
                stage, surfacePath, descPath=descPath,
                undoStack=self._state.undoStack)
            if upgraded is None:
                return (True, "painting to %s; paint wiring skipped "
                        "(chain upgrade failed: %s)"
                        % (descPath, upgradeError))
            okWire, infoWire = brushAuthor.EnsurePaintWiring(
                stage, descPath, preset.id,
                undoStack=self._state.undoStack)
            if okWire:
                wired = "; %s" % infoWire
            else:
                wired = "; paint wiring skipped (%s)" % infoWire
        return (True, "painting to %s%s" % (descPath, wired))

    def showBoundMap(self, stage):
        """Draw the bound map's current baked values. (ok, info).

        The persistent map overlay shows the bound primvar
        (BaseGridFromStage, so undos and rebinds redraw exactly what the
        next stroke would accumulate over). Honors previewMap -- with the
        overlay off there is nothing to show."""
        if stage is None:
            return (False, "brush preview needs a stage")
        binding = self._state.binding
        if binding is None:
            return (False, "no surface bound")
        if not getattr(self._state, "previewMap", True):
            return (True, "")
        base, error = brushAuthor.BaseGridFromStage(stage, binding)
        if base is None:
            return (False, error)
        return brushPreview.SetPreview(
            stage, binding, base, self._state.colorMap,
            self._state.valueRange)

    def setPreviewMap(self, stage, on):
        """The palette's eye toggle: show or hide the map overlay. (ok, info).

        Mid-stroke the working grid is shown instead of the baked map, so
        turning the overlay on during a drag shows the live stroke."""
        self._state.previewMap = bool(on)
        binding = self._state.binding
        if binding is None:
            return (True, "")
        if self._state.previewMap:
            gesture = getattr(self._state, "gesture", None)
            if gesture is not None:
                return self._refreshPreview(stage, gesture)
            return self.showBoundMap(stage)
        return brushPreview.ClearPreview(stage, binding)

    def _showBindingDisplay(self, stage, old):
        """Draw the current binding's map, clearing a left surface."""
        binding = self._state.binding
        if (old is not None and binding is not None
                and old.surfacePath != binding.surfacePath):
            brushPreview.ClearPreview(stage, old)
        self.showBoundMap(stage)

    def rebindPreset(self, stage, presetId):
        """Rebind the brush to another mask preset's map. (ok, info).

        The palette's mask-preset row lands here: the same surface
        stays bound, the PaintMap/primvar/wiring switch to the preset,
        and the map display redraws from the newly bound map --
        switching types switches what is drawn AND what the next
        stroke edits. Unbound with an active description, it paints to
        that description (which binds the preset); unbound without one,
        it only records the preset for the next bind. A live stroke
        refuses: the gesture owns the binding until release or Escape."""
        if stage is None:
            return (False, "brush bind needs a stage")
        if self.gestureActive():
            return (False, "finish the live stroke first")
        preset = brushAuthor.MaskPresetFor(presetId)
        if preset is None:
            return (False, "unknown mask preset %r" % (presetId,))
        self._stage = stage
        binding = self._state.binding
        if binding is None:
            self._state.maskPreset = preset.id
            active = getattr(self._state, "activeDescription", "")
            if active:
                return self.paintToDescription(stage, active)
            return (True, "preset %s (bind a surface to paint it)"
                    % preset.id)
        if (binding.primvar == preset.primvar
                and binding.mapPath.name == preset.mapName):
            self._state.maskPreset = preset.id
            self._showBindingDisplay(stage, binding)
            return (True, "already painting %s" % preset.id)
        newBinding, error = brushAuthor.BindMaskPreset(
            stage, binding.targetPath, preset.id,
            mapsParent=binding.mapPath.GetParentPath(),
            undoStack=self._state.undoStack,
            interpolation=binding.interpolation,
            storage=binding.storage, resolution=binding.resolution)
        if newBinding is None:
            return (False, error)
        self._state.maskPreset = preset.id
        self._state.binding = newBinding
        self._showBindingDisplay(stage, binding)
        wired = ""
        active = getattr(self._state, "activeDescription", "")
        if active and preset.id in brushAuthor.PAINT_WIRING:
            okWire, infoWire = brushAuthor.EnsurePaintWiring(
                stage, active, preset.id,
                undoStack=self._state.undoStack)
            if okWire:
                wired = "; %s" % infoWire
            else:
                wired = "; paint wiring skipped (%s)" % infoWire
        return (True, "painting %s%s" % (preset.id, wired))

    def flood(self, stage):
        """Fill the bound map with the brush value. (ok, info).

        One undoable bake over every texel (the channel row scopes it:
        all channels, or one) -- of the subset's faces only for a
        GeomSubset binding (BakeStroke keeps every other face); the
        display redraws from the flooded primvar, and the bake cooks like
        any other. Refuses mid-stroke: the gesture owns the base until
        release or Escape."""
        if stage is None:
            return (False, "brush flood needs a stage")
        binding = self._state.binding
        if binding is None:
            return (False, "brush flood needs a binding "
                    "(bind a surface first)")
        if self.gestureActive():
            return (False, "finish the live stroke first")
        try:
            value = float(self._state.value)
        except (TypeError, ValueError):
            return (False, "brush value %r is not a number"
                    % (self._state.value,))
        if not math.isfinite(value):
            return (False, "brush value %r is not finite"
                    % (self._state.value,))
        lo, hi = self._state.valueRange
        value = min(hi, max(lo, value))
        channel = self._state.channel
        if channel != -1 and not 0 <= channel < binding.channels:
            return (False, "channel %r is out of range for a %d-channel "
                    "binding" % (channel, binding.channels))
        ok, error = brushAuthor.RefreshBindingFaces(stage, binding)
        if not ok:
            return (False, error)
        grid, error = brushAuthor.BaseGridFromStage(stage, binding)
        if grid is None:
            return (False, error)
        if channel == -1:
            grid.fill(value)
        elif not grid.fillChannel(channel, value):
            return (False, "channel %r is out of range for a %d-channel "
                    "binding" % (channel, binding.channels))
        ok, error = brushAuthor.BakeStroke(
            stage, binding, grid, undoStack=self._state.undoStack,
            label="Flood %s" % binding.primvar)
        if not ok:
            return (False, error)
        self._stage = stage
        self.showBoundMap(stage)
        return (True, "flooded %s with %.3f" % (binding.primvar, value))

    # -- the gesture ----------------------------------------------------

    def _dabArgs(self, face, u, v, snapshot=None):
        """LiveStroke.dab positional args for a hit at (face, u, v).

        (face, u, v, radiusUV, hardness, strength, value, channel, mode,
        falloff). The world radius converts to face-UV through the picked
        face's edge length, floored to the corner-reach minimum here (the
        driver does not floor): the bake only keeps corners, so a dab
        smaller than ~0.7 of a face could vanish between them."""
        mode = brushState.BRUSH_MODES.get(self._state.activeBrush, "set")
        try:
            world = float(self._state.radiusWorld)
        except (TypeError, ValueError):
            world = 0.0
        if not math.isfinite(world) or world < 0.0:
            world = 0.0
        edge = brushPick.faceEdgeLen(snapshot, face)
        radius = world / edge if edge > 1e-12 else world
        radius = max(radius, brushPick.MIN_FACE_RADIUS)
        return (face, u, v, radius, self._hardness(),
                self._state.strength, self._state.value,
                self._state.channel, mode, self._state.falloff)

    def _hardness(self):
        try:
            hardness = float(getattr(self._state, "hardness", 0.0))
        except (TypeError, ValueError):
            return 0.0
        if not math.isfinite(hardness):
            return 0.0
        return min(1.0, max(0.0, hardness))

    def _refreshPreview(self, stage, gesture, faces=None):
        if not getattr(self._state, "previewMap", True):
            return (True, "")
        if faces is None:
            return brushPreview.SetPreview(
                stage, self._state.binding, gesture.working,
                self._state.colorMap, self._state.valueRange)
        key = (self._state.colorMap, self._state.valueRange)
        if gesture.previewKey != key:
            gesture.previewKey = key
            gesture.previewDirty = set()
            return brushPreview.SetPreview(
                stage, self._state.binding, gesture.working,
                self._state.colorMap, self._state.valueRange)
        gesture.previewDirty |= set(faces)
        ok, error = brushPreview.PatchPreview(
            stage, self._state.binding, gesture.working,
            gesture.previewDirty, self._state.colorMap,
            self._state.valueRange)
        if ok:
            gesture.previewDirty = set()
        return (ok, error)

    @staticmethod
    def _dabFailed(result):
        """(failed, error) from a LiveStroke.dab (count, error) result."""
        try:
            count, error = result
        except (TypeError, ValueError):
            return (True, "the brush driver returned %r" % (result,))
        if error or count is None or count < 0:
            return (True, error or "the brush driver refused the dab")
        return (False, "")

    def press(self, stage, camera, x, y):
        """Start a stroke at physical pixel (x, y). (captured, info).

        False leaves no gesture behind: a miss, an unbound tool, a
        disarmed filter and a bad mesh all read as "not a stroke", so the
        viewport filter passes the event through to usdview."""
        if not getattr(self._state, "strokesArmed", True):
            return (False, "strokes are disarmed")
        if self._state.binding is None:
            return (False, "no surface bound")
        if self.gestureActive():
            return (False, "a stroke is already live")
        if camera is None or not camera.invertible:
            return (False, "no camera")
        self._stage = stage
        binding = self._state.binding
        # A subset binding masks the snapshot to the subset's CURRENT faces
        # (the pick misses, and the footprint and smooth stop, outside them).
        ok, error = brushAuthor.RefreshBindingFaces(stage, binding)
        if not ok:
            return (False, error)
        surface = stage.GetPrimAtPath(binding.surfacePath)
        snapshot, error = brushPick.snapshotMesh(
            surface, getattr(binding, "faces", None))
        if snapshot is None:
            return (False, error)
        base, error = brushAuthor.BaseGridFromStage(stage,
                                                    self._state.binding)
        if base is None:
            return (False, error)
        if snapshot.faceCount() != base.numFaces():
            return (False, "the mesh has %d faces for a %d-face base grid"
                    % (snapshot.faceCount(), base.numFaces()))
        hit = brushPick.pickPixels(snapshot, camera, x, y)
        if hit is None:
            return (False, "miss")
        face, u, v, _point = hit
        live = None
        try:
            live, error = brushMap.LiveStroke.create(snapshot, base)
            if live is None:
                return (False, error or "the brush driver failed to open")
            failed, error = self._dabFailed(live.dab(
                *self._dabArgs(face, u, v, snapshot), isMove=False,
                spacing=MOVE_SPACING))
            if failed:
                live.close()
                return (False, error)
            touched = set(live.takeTouched())
        except Exception as exc:
            if live is not None:
                try:
                    live.close()
                except Exception:
                    pass
            return (False, "the press hit an internal error (%s)" % exc)
        prior = brushPreview.CapturePrior(stage, self._state.binding)
        gesture = BrushGesture(camera, snapshot, base, live, prior,
                               FirstDab(face, u, v))
        gesture.lastAttempt = (x, y)
        gesture.lastHit = (x, y)
        gesture.liveDirty |= touched
        gesture.lastMoveTime = time.time()
        ok, error = self._refreshPreview(stage, gesture, touched)
        if not ok:
            gesture.close()
            return (False, error)
        self._state.gesture = gesture
        gesture.livePrior = brushAuthor.CaptureLivePrior(
            stage, self._state.binding)
        return (True, "face %d at (%.3f, %.3f)" % (face, u, v))

    def move(self, stage, x, y):
        """Extend the live stroke to physical pixel (x, y). (ok, info)."""
        gesture = getattr(self._state, "gesture", None)
        if gesture is None:
            return (False, "no stroke is live")
        if brushPick.movedPixels((x, y), gesture.lastAttempt) \
                < brushPick.PICK_MOVE_PIXELS:
            return (True, "throttled")
        gesture.lastAttempt = (x, y)
        hit = brushPick.pickPixels(gesture.snapshot, gesture.camera, x, y)
        if hit is None:
            return (True, "miss")
        gesture.lastHit = (x, y)
        face, u, v, _point = hit
        try:
            failed, error = self._dabFailed(gesture.live.dab(
                *self._dabArgs(face, u, v, gesture.snapshot), isMove=True,
                spacing=MOVE_SPACING))
            if failed:
                return (False, error)
            touched = set(gesture.live.takeTouched())
        except Exception as exc:
            # The working grid may be corrupt; drop the stroke rather
            # than bake garbage. The cancel itself must not fail.
            try:
                self.cancel(stage)
            except Exception:
                pass
            return (False, "a move hit an internal error (%s); the "
                    "stroke was dropped" % exc)
        ok, error = self._refreshPreview(stage, gesture, touched)
        if not ok:
            return (False, error)
        gesture.liveDirty |= touched
        now = time.time()
        gesture.lastMoveTime = now
        live = ""
        if (getattr(self._state, "liveGroom", True) and gesture.liveDirty
                and now - gesture.lastLive >= LIVE_GROOM_MIN_INTERVAL):
            wrote, _info = self._writeLiveGroom(stage, gesture)
            if wrote:
                live = "; live groom"
        return (True, "%d dabs%s" % (gesture.dabCount(), live))

    def _writeLiveGroom(self, stage, gesture):
        """Session-scratch the working grid so the curves track the drag.

        Best-effort: the release bake is the source of truth.
        A failed write keeps its faces, so the next success heals
        the gap instead of dropping paint from the groom."""
        if stage is None or gesture is None:
            return (False, "no stage")
        try:
            ok, error = brushAuthor.PatchLivePrimvar(
                stage, self._state.binding, gesture.working,
                gesture.liveDirty)
        except Exception as exc:
            return (False, "the live groom write hit an internal error "
                    "(%s)" % exc)
        if not ok:
            return (False, error)
        gesture.lastLive = time.time()
        gesture.liveDirty = set()
        gesture.liveWrites += 1
        self.liveGroomWrites += 1
        return (True, "")

    def flushLiveGroom(self, stage):
        """Write the pending live groom now. (written, info).

        The viewport's single-shot timer lands here ~LIVE_GROOM_FLUSH_MS
        after the last move, so the trailing move of a burst always
        reaches the groom even when the pointer stops."""
        gesture = getattr(self._state, "gesture", None)
        if gesture is None:
            return (False, "no stroke is live")
        if not getattr(self._state, "liveGroom", True):
            return (False, "live groom is off")
        if not gesture.liveDirty:
            return (False, "clean")
        faces = len(gesture.liveDirty)
        ok, error = self._writeLiveGroom(stage, gesture)
        if not ok:
            return (False, error)
        return (True, "%d live faces" % faces)

    def pollLiveGroom(self, stage):
        """Flush the live groom when due. (cooked, info).

        Headless-safe (tests call it directly): flushes a dirty grid once
        the pointer paused LIVE_GROOM_PAUSE or LIVE_GROOM_MIN_INTERVAL
        passed since the last write; otherwise reports why not."""
        gesture = getattr(self._state, "gesture", None)
        if gesture is None:
            return (False, "no stroke is live")
        if not getattr(self._state, "liveGroom", True):
            return (False, "live groom is off")
        if not gesture.liveDirty:
            return (False, "clean")
        now = time.time()
        if (now - gesture.lastMoveTime < LIVE_GROOM_PAUSE
                and now - gesture.lastLive < LIVE_GROOM_MIN_INTERVAL):
            return (False, "dragging")
        return self.flushLiveGroom(stage)

    def release(self, stage):
        """Bake the live stroke once and drop it. (baked, info).

        An empty stroke bakes nothing. A failed bake KEEPS the gesture
        live so the artist can fix the cause and release again; the
        overlay stays up until the bake lands or Escape drops it. A landed
        bake redraws the overlay from the baked primvar when previewMap is
        on, and clears it otherwise."""
        gesture = getattr(self._state, "gesture", None)
        if gesture is None:
            return (False, "no stroke is live")
        if gesture.dabCount() == 0:
            brushAuthor.ClearLivePrimvar(
                stage, self._state.binding, gesture.livePrior)
            self._state.gesture = None
            gesture.close()
            self._restoreMapDisplay(stage, gesture)
            return (False, "empty stroke")
        # Clear first: the bake then captures the true previous spec,
        # so undo restores the pre-stroke primvar, not the scratch.
        brushAuthor.ClearLivePrimvar(
            stage, self._state.binding, gesture.livePrior)
        try:
            grid = gesture.live.commitGrid()
        except Exception as exc:
            # The commit recomputes from the untouched base plus the
            # dab record, so a retry is safe: keep the gesture live and
            # the preview up, exactly like a failed bake below.
            self._refreshPreview(stage, gesture)
            return (False, "the bake hit an internal error (%s); the "
                    "stroke is still live -- release again to retry"
                    % exc)
        if grid is None:
            self._refreshPreview(stage, gesture)
            return (False, "the brush driver produced no commit grid; "
                    "release again to retry")
        ok, error = brushAuthor.BakeStroke(
            stage, self._state.binding, grid,
            undoStack=self._state.undoStack)
        if not ok:
            # The gesture stays live for a retry; put the preview back up
            # so the viewport keeps showing the uncommitted stroke.
            self._refreshPreview(stage, gesture)
            return (False, error)
        count = gesture.dabCount()
        self._state.gesture = None
        gesture.close()
        # The map stays drawn: redraw the overlay from the baked primvar
        # (the map's truth, not the commit grid's discarded texel detail).
        self._restoreMapDisplay(stage, gesture)
        return (True, "baked %d dabs" % count)

    def _restoreMapDisplay(self, stage, gesture):
        """After a stroke ends: the baked map when shown, else no overlay."""
        try:
            if getattr(self._state, "previewMap", True):
                self.showBoundMap(stage)
            else:
                brushPreview.ClearPreview(stage, self._state.binding,
                                           getattr(gesture, "prior", None))
        except Exception:
            pass

    def cancel(self, stage):
        """Escape: forget the stroke, restore the prior display, bake nothing."""
        gesture = getattr(self._state, "gesture", None)
        if gesture is None:
            return False
        self._state.gesture = None
        try:
            gesture.live.abort()
        except Exception:
            pass
        gesture.close()
        try:
            brushAuthor.ClearLivePrimvar(
                stage, self._state.binding, gesture.livePrior)
        except Exception:
            pass
        # Cancel restores the pre-stroke display: the baked map when the
        # overlay is on (nothing of the stroke survives), none otherwise.
        self._restoreMapDisplay(stage, gesture)
        return True

    # -- undo / redo ----------------------------------------------------

    def undo(self, stage=None):
        """Undo the last brush bake on `stage` (the palette's). (ok, info)."""
        if stage is not None:
            self._stage = stage
        ok, error = self._state.undoStack.undo()
        if ok:
            self._refreshMapDisplay(stage)
        return (ok, error)

    def redo(self, stage=None):
        """Redo the last undone bake on `stage`. (ok, info)."""
        if stage is not None:
            self._stage = stage
        ok, error = self._state.undoStack.redo()
        if ok:
            self._refreshMapDisplay(stage)
        return (ok, error)

    def _refreshMapDisplay(self, stage=None):
        """Redraw the bound map after an undo/redo. Best-effort."""
        try:
            self.showBoundMap(stage if stage is not None else self._stage)
        except Exception:
            pass
