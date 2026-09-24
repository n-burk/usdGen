# Bind and bake for the brush tool: the PaintMap and the primvar commit.
#
# Bind is the structural half: given a surface mesh it finds or defines a
# UsdGenPaintMap (under <mapsParent>, default /BrushMaps), points its
# usdGen:paint:surface at the mesh, and records the primvar name,
# interpolation, storage, resolution and default the stroke loop paints
# with. Bake is the release half: the stroke's committed grid is
# corner-sampled onto the mesh's face-vertex primvar in the current edit
# target, inside one Sdf.ChangeBlock, with one undo entry.
#
# Rules, all from plan/08-tools.md: Define lives OUTSIDE the change block
# (section 4.1 step 4: Define inside Sdf.ChangeBlock fails); the release
# write is one block into the current edit target and the tool calls no
# Commit (section 2.4); undo of a structural bind is SetActive(False),
# never RemovePrim (section 7.4); element counts stay fixed during a drag
# (section 3.1: bake only ever rewrites primvar VALUES, never the mesh).
#
# The (u, v) convention is brushPick's: quad corners v0..v3 sit at
# (0,0),(1,0),(1,1),(0,1), so corner i of the grid bakes onto the i-th
# face-vertex. channels is tool-side (the schema carries no channel
# count): 1 bakes float[], 3 bakes color3f[], and a bake onto an
# existing primvar of the other shape fails closed.
#
# Descriptions own the paint maps: setup defines a UsdGenDescription
# on the selected mesh, and every bind under it lands in <desc>/Maps,
# one PaintMap per mask preset, so each description carries its own masks.
#
# Arrays cross the USD boundary through numpy only (Vt.*Array.FromNumpy and
# numpy.asarray over the VtArray): per-element VtArray access interleaved
# with ~1M Python calls produced "impossible" AttributeErrors on some
# builds, and it was the per-move cost of the live groom. Grids hand over
# their corners as cornerBuffer() (brushMap), so a live write is one array
# conversion, and BaseGridFromStage returns a corner-backed CornerGrid.
# Pure-Python paths remain for a numpy-less interpreter.
#
# Bind resolution: None picks brushPick.suggestResolution for the mesh (auto
# texel density; Binding.resolutionAuto records it).
#
# Qt-free, like exprAuthor, so the T1 suite drives it headless.

import collections
import math

from pxr import Gf, Sdf, Usd, UsdGeom, Vt

PAINT_MAP_TYPE = "UsdGenPaintMap"

SURFACE_REL = "usdGen:paint:surface"
PRIMVAR_ATTR = "usdGen:paint:primvar"
INTERP_ATTR = "usdGen:paint:interpolation"
STORAGE_ATTR = "usdGen:paint:storage"
RESOLUTION_ATTR = "usdGen:paint:resolution"
DEFAULT_ATTR = "usdGen:map:default"

DEFAULT_MAPS_PARENT = "/BrushMaps"
DEFAULT_PRIMVAR = "usdGen:paint:density"
DEFAULT_INTERPOLATION = "faceVarying"

INTERPOLATIONS = ("constant", "uniform", "varying", "vertex", "faceVarying")
STORAGES = ("primvar", "file")

DESCRIPTION_TYPE = "UsdGenDescription"
DESCRIPTION_SURFACE_REL = "usdGen:surface"

DEFAULT_GROOM_PARENT = "/Groom"
DEFAULT_DESCRIPTION_BASE = "Description"
DESCRIPTION_MAPS_SCOPE = "Maps"
DESCRIPTION_EXPRESSIONS_SCOPE = "Expressions"


MaskPreset = collections.namedtuple(
    "MaskPreset",
    ("id", "label", "mapName", "primvar", "defaultValue", "channels"))

# Scalar grooming masks in the XGen igroom attribute sense: one preset is
# one PaintMap plus the primvar it bakes, readable in expressions through
# ptex("<input>") once the expression's input:<name> targets the map. v1
# presets are all single-channel; the default is the unpainted value,
# and every default is 1.0 so wiring a fresh map is neutral (the
# connection multiplies the map by the op's live value: paint
# darkens from full effect, or flood to 0 first to paint
# additively).
MASK_PRESETS = (
    MaskPreset("density", "Density", "densityPaint",
               "usdGen:paint:density", 1.0, 1),
    MaskPreset("length", "Length", "lengthPaint",
               "usdGen:paint:length", 1.0, 1),
    MaskPreset("width", "Width", "widthPaint",
               "usdGen:paint:width", 1.0, 1),
    MaskPreset("clump", "Clump", "clumpPaint",
               "usdGen:paint:clump", 1.0, 1),
    MaskPreset("curl", "Curl", "curlPaint",
               "usdGen:paint:curl", 1.0, 1),
)

MASK_PRESET_IDS = tuple(p.id for p in MASK_PRESETS)


def MaskPresetFor(presetId):
    """The MaskPreset named `presetId`, or None when it names none."""
    for preset in MASK_PRESETS:
        if preset.id == presetId:
            return preset
    return None


def BindMaskPreset(stage, surfacePath, presetId, mapsParent=None,
                   undoStack=None, interpolation=None, storage=None,
                   resolution=None):
    """Bind `surfacePath` to the PaintMap of mask preset `presetId`.

    (binding, error), through BindSurface: an unknown preset fails
    closed without touching the stage. A None interpolation/storage/
    resolution takes the BindSurface default; pass the live binding's to
    keep a preset switch on the same surface options.
    """
    preset = MaskPresetFor(presetId)
    if preset is None:
        return (None, "unknown mask preset %r" % (presetId,))
    return BindSurface(stage, surfacePath, mapName=preset.mapName,
                       mapsParent=mapsParent, primvar=preset.primvar,
                       interpolation=(interpolation
                                      if interpolation is not None
                                      else DEFAULT_INTERPOLATION),
                       storage=(storage if storage is not None
                                else "primvar"),
                       resolution=resolution,
                       channels=preset.channels,
                       defaultValue=preset.defaultValue,
                       undoStack=undoStack)


class Binding(object):
    """A bound surface: where the stroke paints and where it bakes."""

    def __init__(self, surfacePath, mapPath, primvar=DEFAULT_PRIMVAR,
                 interpolation=DEFAULT_INTERPOLATION, storage="primvar",
                 resolution=16, channels=1, defaultValue=0.0,
                 created=False, resolutionAuto=False, resolutionInfo=""):
        self.surfacePath = Sdf.Path(str(surfacePath))
        self.mapPath = Sdf.Path(str(mapPath))
        self.primvar = str(primvar)
        self.interpolation = str(interpolation)
        self.storage = str(storage)
        self.resolution = int(resolution)
        self.channels = int(channels)
        self.defaultValue = float(defaultValue)
        # True when the bind defined the PaintMap (structural, undoable).
        self.created = bool(created)
        # True when the resolution came from brushPick.suggestResolution.
        self.resolutionAuto = bool(resolutionAuto)
        self.resolutionInfo = str(resolutionInfo)

    def propName(self):
        # plan/08 section 3.1 rows 6-8: primvars:usdGen:paint:<name>.
        return "primvars:" + self.primvar

    def __repr__(self):
        return ("Binding(surface=%r, map=%r, primvar=%r)" % (
            str(self.surfacePath), str(self.mapPath), self.primvar))


def _meshFaceCount(meshPrim):
    countsAttr = UsdGeom.Mesh(meshPrim).GetFaceVertexCountsAttr()
    if not countsAttr:
        return (False, 0, "the surface mesh has no faceVertexCounts")
    counts = countsAttr.Get()
    if not counts:
        return (False, 0, "the surface mesh has no faces")
    if any(c != 4 for c in counts):
        return (False, 0, "the brush paints quad meshes only in v1 "
                "(a face is not a quad)")
    return (True, len(counts), "")


class UndoStack(object):
    """One undo entry per bake or bind, with redo. Cleared on stage swap."""

    def __init__(self, limit=200):
        self._undo = []
        self._redo = []
        self._limit = max(1, int(limit))

    def push(self, label, undoFn, redoFn):
        self._undo.append((label, undoFn, redoFn))
        del self._redo[:]
        while len(self._undo) > self._limit:
            del self._undo[0]

    def canUndo(self):
        return bool(self._undo)

    def canRedo(self):
        return bool(self._redo)

    def undoLabel(self):
        return self._undo[-1][0] if self._undo else ""

    def redoLabel(self):
        return self._redo[-1][0] if self._redo else ""

    def undo(self):
        if not self._undo:
            return (False, "nothing to undo")
        label, undoFn, redoFn = self._undo.pop()
        try:
            undoFn()
        except Exception as exc:
            return (False, "undo %r failed: %s" % (label, exc))
        self._redo.append((label, undoFn, redoFn))
        return (True, "")

    def redo(self):
        if not self._redo:
            return (False, "nothing to redo")
        label, undoFn, redoFn = self._redo.pop()
        try:
            redoFn()
        except Exception as exc:
            return (False, "redo %r failed: %s" % (label, exc))
        self._undo.append((label, undoFn, redoFn))
        return (True, "")

    def clear(self):
        del self._undo[:]
        del self._redo[:]


def _suggestedResolution(surface):
    """(resolution, info) for the mesh: brushPick's auto texel density."""
    try:
        from . import brushPick
    except ImportError:  # file-path test load
        import brushPick
    snapshot, error = brushPick.snapshotMesh(surface)
    if snapshot is None:
        return (16, "auto density unavailable (%s): 16 px/face" % error)
    return brushPick.suggestResolution(snapshot)


def BindSurface(stage, surfacePath, mapName="densityPaint",
                mapsParent=None, primvar=DEFAULT_PRIMVAR,
                interpolation=DEFAULT_INTERPOLATION, storage="primvar",
                resolution=None, channels=1, defaultValue=0.0,
                undoStack=None):
    """Bind the mesh at `surfacePath` to a UsdGenPaintMap. (binding, error).

    resolution None picks the texels per face edge from the mesh
    (brushPick.suggestResolution; binding.resolutionAuto is True).

    Defines <mapsParent>/<mapName> when it is missing (outside any change
    block), then writes the surface relationship and the paint properties
    into the current edit target inside one Sdf.ChangeBlock. Rebinding the
    same map to a DIFFERENT surface fails closed rather than stealing it."""
    if stage is None:
        return (None, "brush bind needs a stage")
    if interpolation not in INTERPOLATIONS:
        return (None, "brush interpolation %r is unknown" % (interpolation,))
    if storage not in STORAGES:
        return (None, "brush storage %r is unknown" % (storage,))
    if resolution is not None and (
            isinstance(resolution, bool) or not isinstance(resolution, int)
            or resolution < 1 or resolution > 256
            or (resolution & (resolution - 1)) != 0):
        return (None, "brush resolution must be a power of two in [1, 256]")
    if channels not in (1, 3):
        return (None, "brush channels must be 1 or 3")
    try:
        defaultValue = float(defaultValue)
    except (TypeError, ValueError):
        return (None, "brush default must be a number")
    if defaultValue != defaultValue:  # NaN
        return (None, "brush default must be finite")

    surfacePath = Sdf.Path(str(surfacePath))
    surface = stage.GetPrimAtPath(surfacePath)
    if not surface or not surface.IsValid():
        return (None, "no such surface prim: %s" % surfacePath)
    if not UsdGeom.Mesh(surface):
        return (None, "brush bind needs a UsdGeomMesh, got %s "
                "at %s" % (surface.GetTypeName(), surfacePath))
    ok, _faces, error = _meshFaceCount(surface)
    if not ok:
        return (None, error)
    resolutionAuto = resolution is None
    resolutionInfo = ""
    if resolutionAuto:
        resolution, resolutionInfo = _suggestedResolution(surface)

    mapsParent = Sdf.Path(str(mapsParent or DEFAULT_MAPS_PARENT))
    mapPath = mapsParent.AppendChild(str(mapName))
    existing = stage.GetPrimAtPath(mapPath)
    created = False
    if existing and existing.IsValid():
        if existing.GetTypeName() != PAINT_MAP_TYPE:
            return (None, "%s exists and is a %s, not a %s"
                    % (mapPath, existing.GetTypeName(), PAINT_MAP_TYPE))
        rel = existing.GetRelationship(SURFACE_REL)
        targets = rel.GetForwardedTargets() if rel else []
        if targets and Sdf.Path(targets[0].GetPrimPath()) != surfacePath:
            return (None, "%s is already bound to %s"
                    % (mapPath, targets[0].GetPrimPath()))
    else:
        # Structural: Define outside the change block (plan/08 4.1).
        parent = stage.GetPrimAtPath(mapsParent)
        if not parent or not parent.IsValid():
            stage.DefinePrim(mapsParent, "Scope")
        stage.DefinePrim(mapPath, PAINT_MAP_TYPE)
        typed = stage.GetPrimAtPath(mapPath)
        if typed.GetTypeName() != PAINT_MAP_TYPE:
            return (None, "%s did not take type %s (is the usdGen "
                    "schema plugin loaded?)" % (mapPath, PAINT_MAP_TYPE))
        created = True

    with Sdf.ChangeBlock():
        with Usd.EditContext(stage, stage.GetEditTarget()):
            prim = stage.GetPrimAtPath(mapPath)
            prim.GetRelationship(SURFACE_REL).SetTargets([surfacePath])
            prim.GetAttribute(PRIMVAR_ATTR).Set(str(primvar))
            prim.GetAttribute(INTERP_ATTR).Set(str(interpolation))
            prim.GetAttribute(STORAGE_ATTR).Set(str(storage))
            prim.GetAttribute(RESOLUTION_ATTR).Set(int(resolution))
            prim.GetAttribute(DEFAULT_ATTR).Set(float(defaultValue))

    binding = Binding(surfacePath, mapPath, primvar, interpolation,
                      storage, resolution, channels, defaultValue,
                      created=created, resolutionAuto=resolutionAuto,
                      resolutionInfo=resolutionInfo)
    if undoStack is not None and created:
        layer = stage.GetEditTarget().GetLayer()
        undoStack.push(
            "Bind %s" % mapName,
            _makeDeactivate(stage, layer, mapPath),
            _makeDeactivate(stage, layer, mapPath, active=True))
    return (binding, "")


def _makeDeactivate(stage, layer, path, active=False):
    def apply():
        with Usd.EditContext(stage, layer):
            prim = stage.GetPrimAtPath(path)
            if prim and prim.IsValid():
                prim.SetActive(active)
    return apply


def _numpy():
    try:
        import numpy
    except ImportError:
        return None
    return numpy


def _gridOf(grid):
    """A LiveStroke hands over its working grid; any grid passes through."""
    working = getattr(grid, "workingGrid", None)
    return working() if callable(working) else grid


def _bakeArray(binding, grid):
    """Corner-sampled face-vertex values for the whole mesh. (ok, list).

    The numpy-less path: [(c0[, c1, c2]) per face-vertex]."""
    values = []
    for face in range(grid.numFaces()):
        for channel in range(grid.channels()):
            ok, corners = grid.cornerValues(face, channel)
            if not ok:
                return (False, [])
            values.append(corners)
    # Interleave to per-vertex tuples: face-vertex i of each face takes
    # corner i of every channel.
    flat = []
    for face in range(grid.numFaces()):
        for corner in range(4):
            flat.append(tuple(values[face * grid.channels() + c][corner]
                              for c in range(grid.channels())))
    return (True, flat)


def _cornersToVt(corners, channels):
    """numpy (F, 4, C) -> Vt.FloatArray / Vt.Vec3fArray in one conversion."""
    np = _numpy()
    arr = np.ascontiguousarray(corners, dtype=np.float32)
    if channels == 1:
        return Vt.FloatArray.FromNumpy(arr.reshape(-1))
    return Vt.Vec3fArray.FromNumpy(arr.reshape((-1, 3)))


def _gridToVt(grid, channels):
    """The whole grid's corners as a primvar value. Raises on failure."""
    if _numpy() is not None:
        try:
            from . import brushMap
        except ImportError:  # file-path test load
            import brushMap
        return _cornersToVt(brushMap.cornerBufferOf(grid), channels)
    ok, flat = _bakeArray(None, grid)
    if not ok:
        raise ValueError("the grid failed to sample")
    return _toVt(flat, channels)


def _primvarTypeName(channels):
    if channels == 1:
        return Sdf.ValueTypeNames.FloatArray
    return Sdf.ValueTypeNames.Color3fArray


def _toVt(values, channels):
    if channels == 1:
        return Vt.FloatArray([v[0] for v in values])
    return Vt.Vec3fArray([Gf.Vec3f(v[0], v[1], v[2]) for v in values])


def BakeStroke(stage, binding, grid, undoStack=None, label="Bake paint stroke"):
    """Commit the stroke grid to the edit target's paint primvar.

    Corner-samples every face (brushPick's convention: corner i onto the
    i-th face-vertex) and writes the values -- and only the values -- in
    one Sdf.ChangeBlock. (True, "") or (False, reason); pushes one undo
    entry that restores the primvar's previous spec byte for byte."""
    if stage is None:
        return (False, "brush bake needs a stage")
    if binding is None:
        return (False, "brush bake needs a binding (bind a surface first)")
    if grid is None:
        return (False, "brush bake needs a stroke grid")
    grid = _gridOf(grid)
    surface = stage.GetPrimAtPath(binding.surfacePath)
    if not surface or not surface.IsValid() or not UsdGeom.Mesh(surface):
        return (False, "the bound surface %s is gone"
                % binding.surfacePath)
    ok, faces, error = _meshFaceCount(surface)
    if not ok:
        return (False, error)
    if grid.numFaces() != faces:
        return (False, "the stroke grid has %d faces for a %d-face mesh"
                % (grid.numFaces(), faces))
    if grid.channels() != binding.channels:
        return (False, "the stroke grid has %d channels for a %d-channel "
                "binding" % (grid.channels(), binding.channels))
    if binding.interpolation != "faceVarying":
        return (False, "v1 bakes faceVarying primvars only, not %r"
                % (binding.interpolation,))

    try:
        newValue = _gridToVt(grid, binding.channels)
    except Exception as exc:
        return (False, "the stroke grid failed to sample (%s)" % exc)
    layer = stage.GetEditTarget().GetLayer()
    propPath = binding.surfacePath.AppendProperty(binding.propName())
    # The previous spec in THIS layer, for the undo entry (None = absent).
    oldSpec = layer.GetAttributeAtPath(propPath)
    oldValue = oldSpec.default if oldSpec is not None else None
    oldInterp = _specInterpolation(oldSpec)
    # A primvar of the other shape already here fails closed: no silent
    # float[] -> color3f[] rewrite.
    if oldSpec is not None and oldSpec.typeName != _primvarTypeName(
            binding.channels):
        return (False, "%s is already a %s primvar, not a %s one"
                % (propPath, oldSpec.typeName,
                   _primvarTypeName(binding.channels)))

    def apply():
        with Sdf.ChangeBlock():
            with Usd.EditContext(stage, layer):
                api = UsdGeom.PrimvarsAPI(surface)
                primvar = api.CreatePrimvar(
                    binding.primvar, _primvarTypeName(binding.channels),
                    binding.interpolation)
                primvar.GetAttr().Set(newValue)

    def revert():
        with Sdf.ChangeBlock():
            with Usd.EditContext(stage, layer):
                if oldValue is None:
                    primSpec = layer.GetPrimAtPath(binding.surfacePath)
                    if primSpec is not None:
                        spec = primSpec.properties.get(binding.propName())
                        if spec is not None:
                            primSpec.RemoveProperty(spec)
                else:
                    api = UsdGeom.PrimvarsAPI(surface)
                    primvar = api.CreatePrimvar(
                        binding.primvar, _primvarTypeName(binding.channels),
                        oldInterp or binding.interpolation)
                    primvar.GetAttr().Set(oldValue)

    try:
        apply()
    except Exception as exc:
        return (False, "brush bake failed: %s" % exc)
    if undoStack is not None:
        undoStack.push(label, revert, apply)
    return (True, "")


def CaptureLivePrior(stage, binding):
    """The session layer's current paint spec, for the live restore.

    (hadSpec, value, interp): hadSpec False means the session carries
    no opinion and the clear removes the scratch. Mirrors the bake's
    undo capture, but on the session layer and without an undo entry."""
    if stage is None or binding is None:
        return (False, None, None)
    session = stage.GetSessionLayer()
    propPath = binding.surfacePath.AppendProperty(binding.propName())
    oldSpec = session.GetAttributeAtPath(propPath)
    if oldSpec is None:
        return (False, None, None)
    return (True, oldSpec.default, _specInterpolation(oldSpec))


def _specInterpolation(spec):
    """The primvar interpolation authored on an attribute spec, or None.

    Interpolation is attribute METADATA, not a sibling property."""
    if spec is None or not spec.HasInfo("interpolation"):
        return None
    return spec.GetInfo("interpolation")


def PatchLivePrimvar(stage, binding, grid, faces):
    """Patch only `faces` of the session scratch primvar. (ok, error).

    The WriteLivePrimvar fast path: reads the current session primvar
    back as one numpy array (a mid-stroke external edit survives),
    overwrites the dirty faces' corners (cornerBuffer(faces)), writes
    it back with one FromNumpy. Falls back to the full write when no
    scratch exists yet or anything looks off, so callers never prime."""
    if stage is None or binding is None or grid is None:
        return (False, "live groom needs a stage, a binding and a grid")
    grid = _gridOf(grid)
    try:
        faceCount = grid.numFaces()
        channels = grid.channels()
    except Exception:
        return (False, "live groom needs a stroke grid")
    if channels != binding.channels:
        return (False, "the stroke grid has %d channels for a %d-channel "
                "binding" % (channels, binding.channels))
    dirty = sorted(set(
        f for f in (faces or [])
        if type(f) is int and 0 <= f < faceCount))
    if not dirty:
        return (True, "")
    session = stage.GetSessionLayer()
    propPath = binding.surfacePath.AppendProperty(binding.propName())
    if session.GetAttributeAtPath(propPath) is None:
        return WriteLivePrimvar(stage, binding, grid)
    surface = stage.GetPrimAtPath(binding.surfacePath)
    if not surface or not surface.IsValid():
        return (False, "live groom needs the bound surface")
    np = _numpy()
    try:
        raw = UsdGeom.PrimvarsAPI(surface).GetPrimvar(
            binding.primvar).GetAttr().Get()
    except Exception:
        raw = None
    if raw is None or len(raw) != faceCount * 4:
        return WriteLivePrimvar(stage, binding, grid)
    try:
        if np is not None:
            try:
                from . import brushMap
            except ImportError:  # file-path test load
                import brushMap
            current = np.array(raw, dtype=np.float32).reshape(
                (faceCount, 4, channels))
            current[np.asarray(dirty, dtype=np.int64)] = \
                brushMap.cornerBufferOf(grid, dirty)
            newValue = _cornersToVt(current, channels)
        else:
            flat = list(raw)
            for face in dirty:
                cornersByChannel = []
                for channel in range(channels):
                    ok, corners = grid.cornerValues(face, channel)
                    if not ok:
                        return WriteLivePrimvar(stage, binding, grid)
                    cornersByChannel.append(corners)
                for corner in range(4):
                    vertex = tuple(cornersByChannel[c][corner]
                                   for c in range(channels))
                    flat[face * 4 + corner] = (
                        vertex[0] if channels == 1
                        else Gf.Vec3f(*vertex))
            newValue = (Vt.FloatArray(flat) if channels == 1
                        else Vt.Vec3fArray(flat))
    except Exception:
        return WriteLivePrimvar(stage, binding, grid)
    try:
        with Sdf.ChangeBlock():
            with Usd.EditContext(stage, session):
                api = UsdGeom.PrimvarsAPI(surface)
                primvar = api.CreatePrimvar(
                    binding.primvar, _primvarTypeName(channels),
                    binding.interpolation)
                primvar.GetAttr().Set(newValue)
    except Exception as exc:
        return (False, "live groom write failed: %s" % exc)
    return (True, "")


def WriteLivePrimvar(stage, binding, grid):
    """Corner-sample the working grid into the session layer. (ok, error).

    The groom cooks from the composed primvar, so this scratch is what
    moves the curves during the drag; release bakes for real and
    clears it. No undo entry: the release bake owns the stroke's undo.
    The caller holds a live gesture, so the grid already matches."""
    if stage is None or binding is None or grid is None:
        return (False, "live groom needs a stage, a binding and a grid")
    grid = _gridOf(grid)
    surface = stage.GetPrimAtPath(binding.surfacePath)
    if not surface or not surface.IsValid():
        return (False, "live groom needs the bound surface")
    try:
        newValue = _gridToVt(grid, binding.channels)
    except Exception as exc:
        return (False, "the working grid failed to sample (%s)" % exc)
    try:
        with Sdf.ChangeBlock():
            with Usd.EditContext(stage, stage.GetSessionLayer()):
                api = UsdGeom.PrimvarsAPI(surface)
                primvar = api.CreatePrimvar(
                    binding.primvar, _primvarTypeName(binding.channels),
                    binding.interpolation)
                primvar.GetAttr().Set(newValue)
    except Exception as exc:
        return (False, "live groom write failed: %s" % exc)
    return (True, "")


def ClearLivePrimvar(stage, binding, prior):
    """Restore the session layer to its pre-stroke paint spec. (ok, error).

    prior is CaptureLivePrior's tuple. Removing the scratch (rather
    than leaving it) keeps undo honest: the release bake that follows
    captures the true previous spec."""
    if stage is None or binding is None:
        return (False, "live groom clear needs a stage and a binding")
    hadSpec, value, interp = prior
    try:
        with Sdf.ChangeBlock():
            with Usd.EditContext(stage, stage.GetSessionLayer()):
                if not hadSpec:
                    primSpec = stage.GetSessionLayer().GetPrimAtPath(
                        binding.surfacePath)
                    if primSpec is not None:
                        spec = primSpec.properties.get(binding.propName())
                        if spec is not None:
                            primSpec.RemoveProperty(spec)
                else:
                    surface = stage.GetPrimAtPath(binding.surfacePath)
                    api = UsdGeom.PrimvarsAPI(surface)
                    primvar = api.CreatePrimvar(
                        binding.primvar, _primvarTypeName(binding.channels),
                        interp or binding.interpolation)
                    primvar.GetAttr().Set(value)
    except Exception as exc:
        return (False, "live groom clear failed: %s" % exc)
    return (True, "")


def _upsampleCornersToTexels(cornersByFace, faces, res, channels,
                             default):
    """Bilinear face corners up to a flat texel list.

    cornersByFace[f][i][c]: corner i of face f, channel c (plain
    floats), sampled at texel GRID points (s/(res-1)): corner
    texels carry their corners verbatim, so bake -> base -> bake
    round-trips exactly (a res-1 grid holds the face mean).
    numpy-vectorized, with a pure-Python fallback for
    numpy-less interpreters (forced in tests by poisoning
    sys.modules['numpy']). Layout matches BrushMap._index exactly.
    Non-finite corners stamp the whole face to default, matching
    setTexel's per-texel skip (a NaN corner NaNs every texel built
    from it)."""
    try:
        import numpy as np
    except ImportError:
        np = None
    if np is not None:
        if res <= 1:
            steps = np.full(1, 0.5)
        else:
            steps = np.arange(res, dtype=np.float64) / float(res - 1)
        uu = steps[None, :]
        vv = steps[:, None]
        weights = np.stack([(1.0 - uu) * (1.0 - vv), uu * (1.0 - vv),
                            uu * vv, (1.0 - uu) * vv])
        corners = np.asarray(cornersByFace, dtype=np.float64)
        finite = np.isfinite(corners).reshape((faces, -1)).all(axis=1)
        corners = corners.reshape((faces, 4, channels))
        out = np.einsum("its,fic->ftsc", weights, corners)
        out[~finite] = default
        return out.reshape((-1,)).tolist()
    flat = [default] * (faces * res * res * channels)
    for face in range(faces):
        corners = cornersByFace[face]
        valid = True
        for i in range(4):
            for c in range(channels):
                if not math.isfinite(corners[i][c]):
                    valid = False
                    break
            if not valid:
                break
        if not valid:
            continue
        base = face * res * res * channels
        for t in range(res):
            v = t / (res - 1) if res > 1 else 0.5
            iv = 1.0 - v
            row = base + t * res * channels
            for s in range(res):
                u = s / (res - 1) if res > 1 else 0.5
                iu = 1.0 - u
                o = row + s * channels
                for c in range(channels):
                    top = corners[0][c] * iu + corners[1][c] * u
                    bottom = corners[3][c] * iu + corners[2][c] * u
                    flat[o + c] = top * iv + bottom * v
    return flat


def BaseGridFromStage(stage, binding):
    """The press-time base grid: the baked primvar upsampled, else default.

    Reads the composed paint primvar (faceVarying, 4 values per face) and
    bilinearly interpolates the corners onto every texel centre, so a new
    stroke accumulates over previous bakes instead of wiping them. A
    missing primvar, a wrong-sized one, or a non-faceVarying interpolation
    falls back to a default fill -- documented, never silent to the caller
    only in that the stroke still starts. (grid, error)."""
    try:
        from . import brushMap
    except ImportError:  # file-path test load
        import brushMap
    if stage is None:
        return (None, "brush base needs a stage")
    if binding is None:
        return (None, "brush base needs a binding")
    surface = stage.GetPrimAtPath(binding.surfacePath)
    if not surface or not surface.IsValid() or not UsdGeom.Mesh(surface):
        return (None, "the bound surface %s is gone"
                % binding.surfacePath)
    ok, faces, error = _meshFaceCount(surface)
    if not ok:
        return (None, error)
    spec = brushMap.BrushMapSpec(faces, binding.resolution,
                                 binding.channels, binding.defaultValue,
                                 False)
    primvar = UsdGeom.PrimvarsAPI(surface).GetPrimvar(binding.primvar)
    raw = None
    if (primvar and primvar.HasValue()
            and primvar.GetInterpolation() == "faceVarying"):
        raw = primvar.GetAttr().Get()
    np = _numpy()
    if np is not None:
        # The corner-backed grid: one numpy read, no upsample, no
        # per-element VtArray access.
        corners = None
        if raw is not None and len(raw) == 4 * faces:
            try:
                corners = np.array(raw, dtype=np.float64).reshape(
                    (faces, 4, binding.channels))
            except (TypeError, ValueError):
                corners = None
        if corners is not None:
            finite = np.isfinite(corners).reshape((faces, -1)).all(axis=1)
            corners[~finite] = float(binding.defaultValue)
        return brushMap.CornerGrid.create(spec, corners)
    grid, error = brushMap.BrushMap.create(spec)
    if grid is None:
        return (None, error)
    if raw is None:
        return (grid, "")
    # Bulk-convert out of the USD array in one C-level pass, then run
    # the upsample over plain Python floats: per-element VtArray
    # indexing interleaved with ~1M Python calls corrupts the eval
    # state on some builds (flaky, impossible AttributeErrors).
    raw = list(raw)
    if len(raw) != 4 * faces:
        return (grid, "")
    nan = float("nan")
    cornersByFace = []
    for face in range(faces):
        quad = []
        for i in range(4):
            v = raw[4 * face + i]
            try:
                if binding.channels == 1:
                    quad.append((float(v),))
                else:
                    quad.append((float(v[0]), float(v[1]), float(v[2])))
            except (TypeError, ValueError, IndexError):
                quad.append((nan,) * binding.channels)
        cornersByFace.append(quad)
    flat = _upsampleCornersToTexels(cornersByFace, faces,
                                     binding.resolution,
                                     binding.channels,
                                     binding.defaultValue)
    grid.setTexelsBulk(flat)
    return (grid, "")


def BakedValues(stage, binding):
    """The composed paint primvar as a float list, or None when absent."""
    if stage is None or binding is None:
        return None
    surface = stage.GetPrimAtPath(binding.surfacePath)
    if not surface or not surface.IsValid():
        return None
    primvar = UsdGeom.PrimvarsAPI(surface).GetPrimvar(binding.primvar)
    if not primvar or not primvar.HasValue():
        return None
    raw = primvar.GetAttr().Get()
    if raw is None:
        return None
    np = _numpy()
    if np is not None:
        return np.asarray(raw, dtype=np.float64).reshape(-1).tolist()
    raw = list(raw)
    flat = []
    for v in raw:
        try:
            flat.extend([float(c) for c in v])
        except TypeError:
            flat.append(float(v))
    return flat


def ListDescriptions(stage):
    """Every UsdGenDescription prim path on `stage`, in path order."""
    if stage is None:
        return []
    found = []
    for prim in Usd.PrimRange(stage.GetPseudoRoot()):
        if prim.GetTypeName() == DESCRIPTION_TYPE:
            found.append(prim.GetPath())
    return sorted(found, key=str)


def DescriptionForSurface(stage, surfacePath):
    """The one description growing from the mesh, else None. (path, error).

    Exactly-one wins: no match (a mesh-only stage) and ambiguous
    matches (a multi-groom stage) both return None, so the caller
    mints a fresh path or asks the user instead of guessing."""
    if stage is None:
        return (None, "")
    try:
        want = Sdf.Path(str(surfacePath))
    except (TypeError, ValueError, RuntimeError):
        return (None, "")
    found = []
    for desc in ListDescriptions(stage):
        surface, _error = DescriptionSurface(stage, desc)
        if surface is not None and surface == want:
            found.append(desc)
    if len(found) == 1:
        return (found[0], "")
    return (None, "")


def DescriptionSurface(stage, descPath):
    """The mesh a description grows from. (surfacePath, error)."""
    if stage is None:
        return (None, "brush descriptions need a stage")
    descPath = Sdf.Path(str(descPath))
    prim = stage.GetPrimAtPath(descPath)
    if not prim or not prim.IsValid():
        return (None, "no such description prim: %s" % descPath)
    if prim.GetTypeName() != DESCRIPTION_TYPE:
        return (None, "%s is a %s, not a %s"
                % (descPath, prim.GetTypeName(), DESCRIPTION_TYPE))
    rel = prim.GetRelationship(DESCRIPTION_SURFACE_REL)
    targets = rel.GetForwardedTargets() if rel else []
    if not targets:
        return (None, "%s names no %s" % (descPath, DESCRIPTION_SURFACE_REL))
    surfacePath = Sdf.Path(targets[0].GetPrimPath())
    surface = stage.GetPrimAtPath(surfacePath)
    if not surface or not surface.IsValid() or not UsdGeom.Mesh(surface):
        return (None, "%s names %s, which is not a UsdGeomMesh"
                % (descPath, surfacePath))
    return (surfacePath, "")


def UniqueDescriptionPath(stage, groomParent=None, base=None):
    """An unused <groomParent>/<base>[N] path. (path, error)."""
    if stage is None:
        return (None, "brush descriptions need a stage")
    groomParent = Sdf.Path(str(groomParent or DEFAULT_GROOM_PARENT))
    base = base or DEFAULT_DESCRIPTION_BASE
    name = base if Sdf.Path.IsValidIdentifier(base) \
        else DEFAULT_DESCRIPTION_BASE
    candidate = name
    index = 1
    while stage.GetPrimAtPath(groomParent.AppendChild(candidate)):
        index += 1
        candidate = "%s%d" % (name, index)
    return (groomParent.AppendChild(candidate), "")


# The five-op chain, terminal first: execution runs bottom-up over
# the Ops children (reverse prim order, like the clump-ptex-plane
# example), so width is defined first and scatter last. Clump/curl
# shapes come from the scalp-scale head-hair example, width from the
# clump-ptex-plane one. (name, type, ((attr, type, value, uniform),
# ...)).
SETUP_OPERATORS = (
    ("width", "UsdGenWidth", (
        ("usdGen:width", Sdf.ValueTypeNames.Float, 0.005, False),
        ("usdGen:width:knots", Sdf.ValueTypeNames.Float2Array,
         (Gf.Vec2f(0.0, 1.0), Gf.Vec2f(0.6, 0.8),
          Gf.Vec2f(1.0, 0.15)), False),
        ("usdGen:replace", Sdf.ValueTypeNames.Bool, True, False),
    )),
    ("curl", "UsdGenCurl", (
        ("usdGen:seed", Sdf.ValueTypeNames.Int, 13, False),
        ("usdGen:radius", Sdf.ValueTypeNames.Float, 0.02, False),
        ("usdGen:frequency", Sdf.ValueTypeNames.Float, 2.0, False),
    )),
    ("clump", "UsdGenClump", (
        ("usdGen:seed", Sdf.ValueTypeNames.Int, 71, False),
        ("usdGen:clump:amount", Sdf.ValueTypeNames.Float, 0.8, False),
        ("usdGen:clump:profile:knots", Sdf.ValueTypeNames.Float2Array,
         (Gf.Vec2f(0.0, 0.25), Gf.Vec2f(1.0, 1.0)), False),
    )),
    ("grow", "UsdGenGrow", (
        ("usdGen:seed", Sdf.ValueTypeNames.Int, 19, False),
        ("usdGen:segments", Sdf.ValueTypeNames.Int, 5, False),
        ("usdGen:length", Sdf.ValueTypeNames.Float, 0.25, False),
        ("usdGen:lengthRandom", Sdf.ValueTypeNames.Float2,
         Gf.Vec2f(0.7, 1.3), False),
        ("usdGen:direction", Sdf.ValueTypeNames.Token,
         "surfaceNormal", True),
    )),
    ("scatter", "UsdGenScatter", (
        ("usdGen:seed", Sdf.ValueTypeNames.Int, 41, False),
        ("usdGen:density", Sdf.ValueTypeNames.Float, 600.0, False),
    )),
)
SETUP_DESCRIPTION_ATTRS = (
    ("usdGen:curve:basis", Sdf.ValueTypeNames.Token, "bspline", True),
    ("usdGen:width:default", Sdf.ValueTypeNames.Float, 0.004, False),
)


# Pipeline order, first to last: scatter roots, grow shoots, clump
# gathers, curl coils, width finishes. Execution runs bottom-up over
# the Ops children, so the prim order reads this back to front.
PIPELINE_ORDER = ("scatter", "grow", "clump", "curl", "width")


def _OperatorPrimType(typeName):
    """True when a prim under <desc>/Ops is an operator.

    Maps and expressions live in their own sibling scopes, so
    under Ops every UsdGen-typed prim is an operator and Scope
    prims are grouping. (The lanes re-filter with the schema
    IsA anyway; this only decides what setup treats as an
    operator.)"""
    return typeName is not None and typeName.startswith("UsdGen")


def _StageHasPrimSpec(stage, path):
    """True when any layer of the stack specifies `path`.

    This is how setup tells "off" from "missing": a deactivated prim
    still reads back valid (just inactive), and prims beneath a
    deactivated scope read back invalid, so a bare GetPrimAtPath cannot
    distinguish either case from absence. An off op is adopted, never
    redefined.
    """
    for layer in stage.GetLayerStack():
        if layer.GetPrimAtPath(path) is not None:
            return True
    return False


def _MoveSpecToEnd(stage, layer, parentPath, name):
    """Move the `name` child spec of `parentPath` last. (ok, error).

    USD has no reorder metadata: reorder IS remove + re-insert (the
    operation behind DidReorderPrims). The round-trip through a temp
    scope preserves every opinion on the moved spec; a missing
    edit-target spec is introduced as an over first (ordering-only),
    so cross-layer chains normalize too. A failed move restores the
    op from the temp rather than stranding it there."""

    def spec(path):
        return layer.GetPrimAtPath(path)

    origPath = parentPath.AppendChild(name)
    tempScope = parentPath.AppendChild("__usdGenOrder")
    tempPath = tempScope.AppendChild(name)
    try:
        if spec(parentPath) is None:
            return (False, "no %s in %s"
                    % (parentPath, layer.identifier))
        if spec(origPath) is None:
            # An ordering-only over, authored at the Sdf level (no
            # stage-level OverridePrim: plan/08 section 4.1).
            Sdf.CreatePrimInLayer(layer, origPath)
        if spec(tempPath) is not None:
            return (False, "stale reorder temp at %s (not touching it)"
                    % tempPath)
        if spec(tempScope) is None:
            Sdf.CreatePrimInLayer(layer, tempScope)
        Sdf.CopySpec(layer, origPath, layer, tempPath)
        del spec(parentPath).nameChildren[name]
        Sdf.CopySpec(layer, tempPath, layer, origPath)
    except Exception as exc:
        try:
            if (spec(origPath) is None
                    and spec(tempPath) is not None):
                Sdf.CopySpec(layer, tempPath, layer, origPath)
        except Exception:
            pass
        return (False, "could not move %s: %s" % (origPath, exc))
    try:
        del spec(tempScope).nameChildren[name]
        if not list(spec(tempScope).nameChildren):
            del spec(parentPath).nameChildren[tempScope.name]
    except Exception as exc:
        return (False, "could not clean the reorder temp: %s" % exc)
    return (True, "")


# Retired order metadata. The lanes derive bottom-up now and ignore
# these; setup strips the edit-target-owned opinions on sight so
# week-old descriptions stop carrying them (opinions in other layers
# are left alone -- and equally ignored).
_LEGACY_ORDER_ATTRS = ("usdGen:operatorOrder",)


def _StripLegacyOrderAttr(stage, descPath):
    """Remove retired order metadata the edit target owns. (ok, e, ch)."""
    desc = stage.GetPrimAtPath(descPath)
    if not desc or not desc.IsValid():
        return (False, "no description at %s" % descPath, False)
    layer = stage.GetEditTarget().GetLayer()
    changed = False
    with Sdf.ChangeBlock():
        with Usd.EditContext(stage, stage.GetEditTarget()):
            for attrName in _LEGACY_ORDER_ATTRS:
                propPath = descPath.AppendProperty(attrName)
                if layer.GetPropertyAtPath(propPath) is None:
                    continue
                if desc.RemoveProperty(attrName):
                    changed = True
    return (True, "", changed)


def _NormalizeOpsChildOrder(stage, descPath):
    """Order the Ops children pipeline-bottom-up. (ok, error, changed).

    Fresh chains are created terminal-first, so this is usually a
    no-op; upgrades (whose new ops append after the old ones) and
    hand-reordered chains are normalized by moving canonical-op
    specs within the edit-target layer -- the strongest layer wins
    child order, so the edit target fully determines it.
    Adopt-if-correct: a chain whose canonical ops already read
    pipeline-bottom-up is left untouched, foreign ops and their
    slots with it; only a misordered chain is rewritten, canonical
    ops moving after any foreign ones (foreign ops then execute
    last). Inactive ops keep their specs (reactivation plus a
    re-setup heals their slots). Shares the setup undo entry."""
    opsPath = descPath.AppendChild("Ops")
    ops = stage.GetPrimAtPath(opsPath)
    if not ops or not ops.IsValid() or not ops.IsActive():
        if (ops and ops.IsValid()) or _StageHasPrimSpec(stage, opsPath):
            # The whole scope is off: nothing cooks, nothing to order.
            return (True, "", False)
        return (False, "no Ops scope under %s" % descPath, False)
    canonical = [c.GetName() for c in ops.GetChildren()
                 if _OperatorPrimType(c.GetTypeName())
                 and c.GetName() in PIPELINE_ORDER]
    want = [n for n in reversed(PIPELINE_ORDER) if n in canonical]
    if canonical == want:
        return (True, "", False)
    # No Sdf.ChangeBlock around the moves: each is a remove + re-insert
    # through Sdf.CopySpec, which plan/08 section 4.1 keeps out of change
    # blocks (namespace edits inside one confuse the stage's recomposition).
    layer = stage.GetEditTarget().GetLayer()
    for name in want:
        ok, error = _MoveSpecToEnd(stage, layer, opsPath, name)
        if not ok:
            return (False, error, True)
    return (True, "", True)


def _EnsureOperators(stage, descPath, createdPaths=None):
    """The five-op chain under <desc>/Ops, terminal first.

    (ok, error, createdAnything). `createdPaths` (a list) receives every
    prim this call DEFINED -- the Ops scope and/or single ops -- and
    nothing for a reorder or legacy-attr strip, so the caller's undo can
    deactivate exactly what setup added. Existing ops are adopted untouched,
    never re-tuned; a same-named prim of another type fails closed.
    Deactivated ops stay off (adopted, never redefined). Ops
    children are normalized last, so a re-run on an old two-op
    description gains the missing ops in pipeline-bottom-up slots
    (createdAnything covers the reorder too)."""
    opsPath = descPath.AppendChild("Ops")
    created = False
    ops = stage.GetPrimAtPath(opsPath)
    if not ops or not ops.IsValid():
        if _StageHasPrimSpec(stage, opsPath):
            opsActive = False
        else:
            stage.DefinePrim(opsPath, "Scope")
            created = True
            if createdPaths is not None:
                createdPaths.append(opsPath)
            opsActive = True
    else:
        opsActive = True
    for name, typeName, attrs in SETUP_OPERATORS:
        opPath = opsPath.AppendChild(name)
        existing = stage.GetPrimAtPath(opPath)
        if not opsActive or not (existing and existing.IsValid()):
            if _StageHasPrimSpec(stage, opPath):
                continue
        if existing and existing.IsValid():
            if existing.GetTypeName() != typeName:
                return (False, "%s exists and is a %s, not a %s"
                        % (opPath, existing.GetTypeName(), typeName),
                        created)
            continue
        # Structural: Define outside the change block (plan/08 4.1).
        stage.DefinePrim(opPath, typeName)
        typed = stage.GetPrimAtPath(opPath)
        if typed.GetTypeName() != typeName:
            return (False, "%s did not take type %s (is the usdGen "
                    "schema plugin loaded?)" % (opPath, typeName),
                    created)
        created = True
        if createdPaths is not None:
            createdPaths.append(opPath)
        with Sdf.ChangeBlock():
            with Usd.EditContext(stage, stage.GetEditTarget()):
                prim = stage.GetPrimAtPath(opPath)
                for attrName, attrType, value, uniform in attrs:
                    attr = prim.CreateAttribute(attrName, attrType)
                    if uniform:
                        attr.SetVariability(Sdf.VariabilityUniform)
                    if not attr.Set(value):
                        return (False, "could not set %s on %s"
                                % (attrName, opPath), created)
    with Sdf.ChangeBlock():
        with Usd.EditContext(stage, stage.GetEditTarget()):
            prim = stage.GetPrimAtPath(descPath)
            for attrName, attrType, value, uniform in SETUP_DESCRIPTION_ATTRS:
                if prim.GetAttribute(attrName).HasValue():
                    continue
                attr = prim.CreateAttribute(attrName, attrType)
                if uniform:
                    attr.SetVariability(Sdf.VariabilityUniform)
                if not attr.Set(value):
                    return (False, "could not set %s on %s"
                            % (attrName, descPath), created)
    ok, error, orderChanged = _NormalizeOpsChildOrder(stage, descPath)
    if not ok:
        return (False, error, created)
    ok, error, stripped = _StripLegacyOrderAttr(stage, descPath)
    if not ok:
        return (False, error, created)
    return (True, "", created or orderChanged or stripped)


def SetupDescription(stage, surfacePath, descPath=None, groomParent=None,
                     undoStack=None):
    """Define a ready-to-cook UsdGenDescription on the mesh. (path, error).

    EnsureDescription plus the Scatter -> Grow -> Clump -> Curl ->
    Width chain in pipeline-bottom-up prim order (a re-run
    upgrades an old two-op description in place): an
    operator-less description can never capture or commit. One undo
    entry covers a fresh description; an adopted one gets an Ops entry
    only when operators were added under it. An auto path first adopts
    the one description already growing from the mesh, so setup on a
    groomed stage never mints a duplicate (and paint-to never has to
    ask which one to paint)."""
    if stage is None:
        return (None, "brush descriptions need a stage")
    if descPath is None:
        adopted, _error = DescriptionForSurface(stage, surfacePath)
        if adopted is not None:
            descPath = adopted
        else:
            descPath, error = UniqueDescriptionPath(stage, groomParent)
            if descPath is None:
                return (None, error)
    existed = bool(stage.GetPrimAtPath(Sdf.Path(str(descPath))))
    descPath, error = EnsureDescription(
        stage, surfacePath, descPath=descPath, undoStack=undoStack)
    if descPath is None:
        return (None, error)
    createdPaths = []
    ok, error, _changed = _EnsureOperators(stage, descPath, createdPaths)
    if not ok:
        return (None, error)
    # Undo deactivates only what this setup DEFINED: the Ops scope when it
    # was new, else the single ops added under a pre-existing scope. A
    # reorder or legacy-attr strip alone is a normalization with no undo
    # entry -- deactivating a pre-existing Ops scope for it (paint-to runs
    # setup for every wired preset) would switch the artist's chain off.
    if undoStack is not None and createdPaths and existed:
        layer = stage.GetEditTarget().GetLayer()
        opsPath = descPath.AppendChild("Ops")
        targets = [opsPath] if opsPath in createdPaths else list(createdPaths)
        off = [_makeDeactivate(stage, layer, path) for path in targets]
        on = [_makeDeactivate(stage, layer, path, active=True)
              for path in targets]

        def undo():
            with Sdf.ChangeBlock():
                for step in off:
                    step()

        def redo():
            with Sdf.ChangeBlock():
                for step in on:
                    step()

        undoStack.push("Setup %s operators" % descPath.name, undo, redo)
    return (descPath, "")


EXPRESSION_TYPE = "UsdGenExpression"

# preset id -> (op name, op type, param attr, expression name, map name,
# source template). The template scales the map by the op's CURRENT
# value, read live, so setup-time retunes survive the connection (a
# connected value wins over the authored one). Density is DELIBERATELY
# absent: UsdGenScatter owns its topology and rejects connected
# parameters (expressionTargets.cpp), so no expression can mask it --
# that needs scatter-side acceptance sampling in the engine. Every
# other target evaluates at primitive domain (expressionTargets.cpp
# restricts only the groom-wide toggles), so one ptex() lookup per
# strand root drives them all.
PAINT_WIRING = {
    "length": ("grow", "UsdGenGrow", "usdGen:length",
               "lengthScale", "lengthPaint", 'ptex("lengthPaint") * %r'),
    "width": ("width", "UsdGenWidth", "usdGen:width",
              "widthScale", "widthPaint", 'ptex("widthPaint") * %r'),
    "clump": ("clump", "UsdGenClump", "usdGen:clump:amount",
              "clumpAmountScale", "clumpPaint",
              'ptex("clumpPaint") * %r'),
    "curl": ("curl", "UsdGenCurl", "usdGen:radius",
             "curlRadiusScale", "curlPaint", 'ptex("curlPaint") * %r'),
}


EVALUATION_KEY = "usdGen:evaluation"
EVALUATION_PRIMITIVE = "primitive"


def _setupOpDefault(opName, param):
    for name, _typeName, attrs in SETUP_OPERATORS:
        if name != opName:
            continue
        for attrName, _attrType, value, _uniform in attrs:
            if attrName == param:
                return float(value)
    return 1.0


def EnsurePaintWiring(stage, descPath, presetId, undoStack=None):
    """Connect the preset's map to its op param. (ok, info).

    <desc>/Expressions/<expr> holds `ptex("<map>") * <current value>`
    with input:<map> targeting the Maps PaintMap, and the op param
    gets a .connect opinion to the expression (the clump-ptex-plane
    arrangement). Adopt-if-identical makes repeated paint-tos
    idempotent; a same-named foreign expression or a foreign
    connection fails closed, and the paint-to still binds. ptex() reads
    the map at the strand root, which only compiles at primitive (or
    point) domain, so the param also gets a usdGen:evaluation =
    "primitive" opinion; both capture lanes read that key."""
    if stage is None:
        return (False, "brush paint wiring needs a stage")
    if presetId not in PAINT_WIRING:
        return (False, "preset %r has no wired op in v1" % (presetId,))
    opName, opType, param, exprName, mapName, template = \
        PAINT_WIRING[presetId]
    try:
        descPath = Sdf.Path(str(descPath))
    except (TypeError, ValueError, RuntimeError):
        return (False, "description path %r is not a path" % (descPath,))
    desc = stage.GetPrimAtPath(descPath)
    if not desc or not desc.IsValid():
        return (False, "no such description prim: %s" % descPath)
    mapPath = descPath.AppendChild(DESCRIPTION_MAPS_SCOPE).AppendChild(
        mapName)
    paintMap = stage.GetPrimAtPath(mapPath)
    if not paintMap or not paintMap.IsValid():
        return (False, "no PaintMap at %s (bind the preset first)"
                % mapPath)
    opPath = descPath.AppendChild("Ops").AppendChild(opName)
    op = stage.GetPrimAtPath(opPath)
    if not op or not op.IsValid() or op.GetTypeName() != opType:
        return (False, "no %s op %s to wire" % (opType, opPath))
    try:
        current = float(op.GetAttribute(param).Get())
    except (TypeError, ValueError, AttributeError):
        current = _setupOpDefault(opName, param)
    # Art precision, not binary dust: float knobs come back as
    # doubles (0.8 -> 0.8000000119), and the expression only
    # scales paint.
    source = template % (round(current, 6),)
    inputName = "input:" + mapName
    exprPath = descPath.AppendChild(
        DESCRIPTION_EXPRESSIONS_SCOPE).AppendChild(exprName)
    existing = stage.GetPrimAtPath(exprPath)
    createdExpr = False
    if existing and existing.IsValid():
        if existing.GetTypeName() != EXPRESSION_TYPE:
            return (False, "%s exists and is a %s, not a %s"
                    % (exprPath, existing.GetTypeName(), EXPRESSION_TYPE))
        oldSource = existing.GetAttribute("usdGen:expr:source")
        oldSource = oldSource.Get() if oldSource else None
        oldRel = existing.GetRelationship(inputName)
        oldTargets = oldRel.GetTargets() if oldRel else []
        if oldSource != source or [str(t) for t in oldTargets] != \
                [str(mapPath)]:
            return (False, "%s differs from the paint wiring "
                    "(not touching it)" % exprPath)
    else:
        # Structural: Define outside the change block (plan/08 4.1).
        exprScope = descPath.AppendChild(DESCRIPTION_EXPRESSIONS_SCOPE)
        scope = stage.GetPrimAtPath(exprScope)
        if not scope or not scope.IsValid():
            stage.DefinePrim(exprScope, "Scope")
        stage.DefinePrim(exprPath, EXPRESSION_TYPE)
        typed = stage.GetPrimAtPath(exprPath)
        if typed.GetTypeName() != EXPRESSION_TYPE:
            return (False, "%s did not take type %s (is the usdGen "
                    "schema plugin loaded?)" % (exprPath, EXPRESSION_TYPE))
        createdExpr = True
        with Sdf.ChangeBlock():
            with Usd.EditContext(stage, stage.GetEditTarget()):
                prim = stage.GetPrimAtPath(exprPath)
                prim.CreateAttribute("usdGen:expr:source",
                                     Sdf.ValueTypeNames.String).Set(source)
                prim.CreateRelationship(inputName).SetTargets([mapPath])
                prim.CreateAttribute("outputs:result",
                                     Sdf.ValueTypeNames.Float,
                                     custom=True)
    with Sdf.ChangeBlock():
        with Usd.EditContext(stage, stage.GetEditTarget()):
            attr = stage.GetPrimAtPath(opPath).GetAttribute(param)
            oldConnections = list(attr.GetConnections())
            # CustomData nests ("usdGen:evaluation" lives at
            # ["usdGen"]["evaluation"]); ByKey would hide that here.
            customData = attr.GetCustomData() or {}
            try:
                oldEval = customData["usdGen"]["evaluation"]
            except (KeyError, TypeError, IndexError):
                oldEval = None
            if [str(c) for c in oldConnections] == [str(exprPath)]:
                connected = False
            elif oldConnections:
                return (False, "%s is already connected elsewhere "
                        "(not stealing it)" % opPath.AppendProperty(param))
            else:
                attr.AddConnection(exprPath)
                connected = True
            evaluationChanged = oldEval != EVALUATION_PRIMITIVE
            if evaluationChanged:
                attr.SetCustomDataByKey(EVALUATION_KEY,
                                        EVALUATION_PRIMITIVE)
    wiringChanged = createdExpr or connected or evaluationChanged
    if undoStack is not None and wiringChanged:
        layer = stage.GetEditTarget().GetLayer()

        def applyWiring():
            with Usd.EditContext(stage, layer):
                wired = stage.GetPrimAtPath(opPath).GetAttribute(param)
                wired.SetConnections([exprPath])
                wired.SetCustomDataByKey(EVALUATION_KEY,
                                         EVALUATION_PRIMITIVE)
                if createdExpr:
                    stage.GetPrimAtPath(exprPath).SetActive(True)

        def revertWiring():
            with Usd.EditContext(stage, layer):
                wired = stage.GetPrimAtPath(opPath).GetAttribute(param)
                wired.SetConnections(oldConnections)
                if oldEval is None:
                    wired.ClearCustomDataByKey(EVALUATION_KEY)
                else:
                    wired.SetCustomDataByKey(EVALUATION_KEY, oldEval)
                if createdExpr:
                    stage.GetPrimAtPath(exprPath).SetActive(False)

        undoStack.push("Wire %s paint" % presetId, revertWiring,
                       applyWiring)
    return (True, "wired %s paint to %s.%s"
            % (presetId, opName, param))


def EnsureDescription(stage, surfacePath, descPath=None, groomParent=None,
                      undoStack=None):
    """Define a UsdGenDescription growing from the mesh. (path, error).

    `descPath` defaults to an unused /Groom/Description[N]. An existing
    UsdGenDescription there is adopted when it names no other surface;
    anything else fails closed rather than stealing it. Structural
    (Define outside any change block), undoable via SetActive(False)
    like a bind."""
    if stage is None:
        return (None, "brush descriptions need a stage")
    surfacePath = Sdf.Path(str(surfacePath))
    surface = stage.GetPrimAtPath(surfacePath)
    if not surface or not surface.IsValid():
        return (None, "no such surface prim: %s" % surfacePath)
    if not UsdGeom.Mesh(surface):
        return (None, "a description grows from a UsdGeomMesh, got %s "
                "at %s" % (surface.GetTypeName(), surfacePath))
    if descPath is None:
        descPath, error = UniqueDescriptionPath(stage, groomParent)
        if descPath is None:
            return (None, error)
    else:
        try:
            descPath = Sdf.Path(str(descPath))
        except (TypeError, ValueError, RuntimeError):
            return (None, "description path %r is not a path" % (descPath,))
        if descPath.isEmpty or not descPath.IsPrimPath() \
                or not str(descPath).startswith("/"):
            return (None, "description path %r must be an absolute prim "
                    "path" % (str(descPath),))
    existing = stage.GetPrimAtPath(descPath)
    created = False
    if existing and existing.IsValid():
        if existing.GetTypeName() != DESCRIPTION_TYPE:
            return (None, "%s exists and is a %s, not a %s"
                    % (descPath, existing.GetTypeName(), DESCRIPTION_TYPE))
        current, _error = DescriptionSurface(stage, descPath)
        if current is not None and current != surfacePath:
            return (None, "%s already grows from %s" % (descPath, current))
    else:
        # Structural: Define outside the change block (plan/08 4.1).
        parent = stage.GetPrimAtPath(descPath.GetParentPath())
        if not parent or not parent.IsValid():
            stage.DefinePrim(descPath.GetParentPath(), "Scope")
        stage.DefinePrim(descPath, DESCRIPTION_TYPE)
        typed = stage.GetPrimAtPath(descPath)
        if typed.GetTypeName() != DESCRIPTION_TYPE:
            return (None, "%s did not take type %s (is the usdGen "
                    "schema plugin loaded?)" % (descPath, DESCRIPTION_TYPE))
        created = True

    with Sdf.ChangeBlock():
        with Usd.EditContext(stage, stage.GetEditTarget()):
            prim = stage.GetPrimAtPath(descPath)
            prim.GetRelationship(DESCRIPTION_SURFACE_REL).SetTargets(
                [surfacePath])

    if undoStack is not None and created:
        layer = stage.GetEditTarget().GetLayer()
        undoStack.push(
            "Setup %s" % descPath.name,
            _makeDeactivate(stage, layer, descPath),
            _makeDeactivate(stage, layer, descPath, active=True))
    return (descPath, "")

