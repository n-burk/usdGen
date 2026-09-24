# Stroke / map preview for the brush tool.
#
# Two display paths, chosen per call:
#
# * The viewport overlay (the normal usdview path). When the usdGenImaging
#   brush C ABI loads AND an attribute-preview scene index is live in a render
#   chain (brushApi.previewIndexCount() > 0: the UsdGenAttributePreview
#   SceneIndexPlugin appends one per renderer), SetPreview pushes faceVarying
#   displayColor for binding.surfacePath straight into Hydra
#   (UsdGenBrush_PreviewSet). Nothing is written to the stage: no layer churn
#   per move, nothing to restore on clear, nothing for undo or a save to pick
#   up. The first Set on a surface dirties primvars/displayColor, later Sets
#   only its primvarValue (plan/08-tools.md section 2.3).
#
# * The session displayColor (no ABI, or no live index: the Qt-free T1
#   suite). A faceVarying displayColor goes through a colour map into the
#   stage's SESSION layer whatever the edit target is (exprPreview.py's rule:
#   a viewing aid is never saved with the layer being authored). The paint
#   primvar itself is written only by the bake. Clearing restores whatever
#   session displayColor the stroke found on press (CapturePrior); weaker
#   (root-layer) opinions always show through again untouched.
#
# Colours are channel 0 of the grid's corners (cornerBuffer, numpy) through
# the heat/gray ramps -- the same ramps the C ABI's StrokePreviewColors uses.
#
# Qt-free, like exprPreview, so the T1 suite drives it headless.

from pxr import Gf, Sdf, Usd, UsdGeom, Vt

try:
    from . import brushApi, brushMap
except ImportError:  # file-path test load
    import brushApi
    import brushMap

COLOR_MAPS = (
    ("Heat", "heat"),
    ("Gray", "gray"),
)

DEFAULT_RANGE = (0.0, 1.0)


def heatColor(t):
    """Black -> red -> yellow -> white ramp at t in [0, 1]."""
    t = min(1.0, max(0.0, float(t)))
    if t < 0.5:
        k = t * 2.0
        return (k, 0.0, 0.0)
    if t < 0.75:
        k = (t - 0.5) * 4.0
        return (1.0, k, 0.0)
    k = (t - 0.75) * 4.0
    return (1.0, 1.0, k)


def grayColor(t):
    t = min(1.0, max(0.0, float(t)))
    return (t, t, t)


def mapColor(colorMap, t):
    if colorMap == "gray":
        return grayColor(t)
    return heatColor(t)


def _normalise(value, valueRange):
    lo, hi = float(valueRange[0]), float(valueRange[1])
    if hi <= lo:
        return 0.0 if value < lo else 1.0
    return min(1.0, max(0.0, (float(value) - lo) / (hi - lo)))


def _faceVertexValues(grid):
    """Corner-sampled per-face-vertex scalars. (ok, [faces x 4] floats).

    Channel 0: the preview shows the density channel even on a colour map,
    exactly as the groom preview colours strands by one channel. The
    numpy-less path only."""
    rows = []
    for face in range(grid.numFaces()):
        ok, corners = grid.cornerValues(face, 0)
        if not ok:
            return (False, [])
        rows.append(corners)
    return (True, rows)


def _checkGrid(stage, binding, grid, colorMap):
    """(ok, layer, surface, faces, error): the Set/Patch preamble."""
    if stage is None:
        return (False, None, None, 0, "brush preview needs a stage")
    if binding is None:
        return (False, None, None, 0, "brush preview needs a binding")
    if grid is None:
        return (False, None, None, 0, "brush preview needs a stroke grid")
    if colorMap not in ("heat", "gray"):
        return (False, None, None, 0,
                "brush preview ramp %r is unknown" % (colorMap,))
    layer = stage.GetSessionLayer()
    if layer is None:
        return (False, None, None, 0, "the stage has no session layer")
    surface = stage.GetPrimAtPath(binding.surfacePath)
    if not surface or not surface.IsValid() or not UsdGeom.Mesh(surface):
        return (False, None, None, 0, "the bound surface %s is gone"
                % binding.surfacePath)
    try:
        faces = grid.numFaces()
    except Exception:
        return (False, None, None, 0,
                "brush preview needs a stroke grid")
    counts = UsdGeom.Mesh(surface).GetFaceVertexCountsAttr().Get()
    if not counts or faces != len(counts):
        return (False, None, None, 0,
                "the stroke grid has %d faces for a %d-face mesh"
                % (faces, len(counts or [])))
    return (True, layer, surface, faces, "")


def overlayActive():
    """True when previews go to the Hydra overlay rather than the stage."""
    return brushApi.available() and brushApi.previewIndexCount() > 0


# Surface path -> the last faceVarying rgb (numpy (N, 3) float32) pushed to
# the overlay. The overlay is write-only from Python, so the client
# remembers what it pushed: HasPreview / PreviewColors / ShownColors read it.
_OVERLAID = {}

# Surface paths whose SESSION displayColor this module authored (the no-overlay
# path). ClearPreview only ever removes a session spec named here, so a
# displayColor someone else put in the session layer survives.
_SESSION_AUTHORED = set()


def _gridColors(grid, colorMap, valueRange, faces=None):
    """numpy (N*4, 3) float32 corner colours (channel 0), or None."""
    corners = brushMap.cornerBufferOf(grid, faces)
    if corners is None:
        return None
    return brushMap.rampColors(corners[:, :, 0].reshape(-1), colorMap,
                               valueRange[0], valueRange[1])


def _setSessionColors(stage, layer, surface, binding, colors):
    with Sdf.ChangeBlock():
        with Usd.EditContext(stage, layer):
            api = UsdGeom.PrimvarsAPI(surface)
            display = api.CreatePrimvar(
                "displayColor", Sdf.ValueTypeNames.Color3fArray,
                binding.interpolation)
            display.GetAttr().Set(colors)
    _SESSION_AUTHORED.add(str(binding.surfacePath))


def SetPreview(stage, binding, grid, colorMap="heat",
               valueRange=DEFAULT_RANGE):
    """Show the grid on the bound surface. (ok, info).

    The Hydra overlay when one is live (info "overlay"), else a
    faceVarying session displayColor in one Sdf.ChangeBlock (info
    "session"). False, leaving any previous preview in place, for a
    missing stage/binding/grid or a grid that does not match the mesh.
    `grid` may also be a brushMap.LiveStroke (its working grid shows)."""
    if isinstance(grid, brushMap.LiveStroke):
        grid = grid.workingGrid()
    ok, layer, surface, _faces, error = _checkGrid(
        stage, binding, grid, colorMap)
    if not ok:
        return (False, error)
    try:
        rgb = _gridColors(grid, colorMap, valueRange)
    except Exception as exc:
        return (False, "the stroke grid failed to sample (%s)" % exc)
    if rgb is not None and overlayActive():
        if not brushApi.previewSet(binding.surfacePath, rgb):
            return (False, "the preview overlay refused the colours")
        _OVERLAID[str(binding.surfacePath)] = rgb.copy()
        return (True, "overlay")
    if rgb is not None:
        colors = Vt.Vec3fArray.FromNumpy(rgb)
    else:
        ok, rows = _faceVertexValues(grid)
        if not ok:
            return (False, "the stroke grid failed to sample")
        colors = Vt.Vec3fArray([
            Gf.Vec3f(*mapColor(colorMap, _normalise(c, valueRange)))
            for corners in rows for c in corners])
    try:
        _setSessionColors(stage, layer, surface, binding, colors)
    except Exception as exc:
        return (False, "brush preview failed: %s" % exc)
    return (True, "session")


def PatchPreview(stage, binding, grid, faces, colorMap="heat",
                 valueRange=DEFAULT_RANGE):
    """Repaint only `faces` of the preview. (ok, info).

    The overlay path re-Sets (one faceVarying array, dirtied by value).
    The session path reads the current session displayColor back (a
    mid-stroke external edit survives), resamples only the dirty faces
    (cornerBuffer(faces)) and writes it back; anything unexpected -- no
    preview yet, a size mismatch -- becomes a full SetPreview, so callers
    never prime."""
    if isinstance(grid, brushMap.LiveStroke):
        grid = grid.workingGrid()
    if overlayActive():
        return SetPreview(stage, binding, grid, colorMap, valueRange)
    ok, layer, surface, faceCount, error = _checkGrid(
        stage, binding, grid, colorMap)
    if not ok:
        return (False, error)
    dirty = sorted(set(
        f for f in (faces or [])
        if type(f) is int and 0 <= f < faceCount))
    if not dirty:
        return (True, "")
    spec = layer.GetAttributeAtPath(
        binding.surfacePath.AppendProperty("primvars:displayColor"))
    if spec is None:
        return SetPreview(stage, binding, grid, colorMap, valueRange)
    try:
        raw = UsdGeom.PrimvarsAPI(surface).GetPrimvar(
            "displayColor").GetAttr().Get()
    except Exception:
        raw = None
    if raw is None or len(raw) != faceCount * 4:
        return SetPreview(stage, binding, grid, colorMap, valueRange)
    try:
        import numpy as np
    except ImportError:
        np = None
    try:
        if np is not None:
            current = np.array(raw, dtype=np.float32).reshape((-1, 4, 3))
            rgb = _gridColors(grid, colorMap, valueRange, dirty)
            current[np.asarray(dirty, dtype=np.int64)] = rgb.reshape(
                (-1, 4, 3))
            out = Vt.Vec3fArray.FromNumpy(current.reshape((-1, 3)))
        else:
            colors = list(raw)
            for face in dirty:
                ok, corners = grid.cornerValues(face, 0)
                if not ok:
                    return SetPreview(stage, binding, grid, colorMap,
                                      valueRange)
                for i, c in enumerate(corners):
                    colors[face * 4 + i] = Gf.Vec3f(*mapColor(
                        colorMap, _normalise(c, valueRange)))
            out = Vt.Vec3fArray(colors)
    except Exception:
        return SetPreview(stage, binding, grid, colorMap, valueRange)
    try:
        _setSessionColors(stage, layer, surface, binding, out)
    except Exception as exc:
        return (False, "brush preview failed: %s" % exc)
    return (True, "session")


_DISPLAY_NAME = "primvars:displayColor"


class DisplayColorPrior(object):
    """The session displayColor a stroke found on press (for restore)."""

    def __init__(self, had=False, value=None, interpolation=None):
        self.had = had
        self.value = value
        self.interpolation = interpolation


def CapturePrior(stage, binding):
    """Snapshot the session displayColor spec, for ClearPreview to put back.

    None on the overlay path: the overlay never touches the stage, so
    there is nothing to restore."""
    if overlayActive():
        return None
    prior = DisplayColorPrior()
    if stage is None or binding is None:
        return prior
    layer = stage.GetSessionLayer()
    if layer is None:
        return prior
    spec = layer.GetAttributeAtPath(
        binding.surfacePath.AppendProperty("primvars:displayColor"))
    if spec is None:
        return prior
    prior.had = True
    prior.value = spec.default
    # Interpolation is attribute metadata on the spec, not a property.
    prior.interpolation = (spec.GetInfo("interpolation")
                           if spec.HasInfo("interpolation") else None)
    return prior


def _sessionHas(stage, binding):
    layer = stage.GetSessionLayer()
    if layer is None:
        return False
    return layer.GetAttributeAtPath(
        binding.surfacePath.AppendProperty(
            "primvars:displayColor")) is not None


def HasPreview(stage, binding):
    """True when an overlay or the session preview displayColor is up."""
    if stage is None or binding is None:
        return False
    if str(binding.surfacePath) in _OVERLAID:
        return True
    return (str(binding.surfacePath) in _SESSION_AUTHORED
            and _sessionHas(stage, binding))


def ClearPreview(stage, binding, prior=None):
    """Remove the preview for binding's surface. (ok, info).

    Clears the Hydra overlay (harmless when none is up) and the session
    displayColor this module authored, restoring `prior` when one was
    captured. ok is True when afterwards no preview of ours remains, or
    exactly the captured prior is back."""
    if stage is None or binding is None:
        return (True, "")
    overlaid = False
    if brushApi.available():
        overlaid = brushApi.previewClear(binding.surfacePath)
    _OVERLAID.pop(str(binding.surfacePath), None)
    layer = stage.GetSessionLayer()
    if layer is None:
        return (True, "overlay cleared" if overlaid else "")
    restore = prior is not None and getattr(prior, "had", False)
    ours = str(binding.surfacePath) in _SESSION_AUTHORED
    _SESSION_AUTHORED.discard(str(binding.surfacePath))
    if not ours and not restore:
        # Only the overlay was ours; a session displayColor someone else
        # authored stays exactly as it is.
        return (True, "overlay cleared" if overlaid else "cleared")
    if (ours and _sessionHas(stage, binding)) or restore:
        with Sdf.ChangeBlock():
            primSpec = layer.GetPrimAtPath(binding.surfacePath)
            if primSpec is not None:
                spec = primSpec.properties.get(_DISPLAY_NAME)
                if spec is not None:
                    primSpec.RemoveProperty(spec)
            if restore:
                with Usd.EditContext(stage, layer):
                    surface = stage.GetPrimAtPath(binding.surfacePath)
                    if surface and surface.IsValid():
                        api = UsdGeom.PrimvarsAPI(surface)
                        display = api.CreatePrimvar(
                            "displayColor", Sdf.ValueTypeNames.Color3fArray,
                            prior.interpolation or "faceVarying")
                        display.GetAttr().Set(prior.value)
    if restore:
        return (_sessionHas(stage, binding), "prior restored")
    return (not _sessionHas(stage, binding),
            "overlay cleared" if overlaid else "cleared")


def ClearAllPreviews(stage=None):
    """Drop every preview this process shows (stage replace, palette close).

    Clears every Hydra overlay (the registry is process-global and keyed by
    prim path, so a stale overlay would land on a same-path prim of the
    next stage) and forgets what was pushed. With `stage`, the session
    displayColors this module authored on it are removed too. The number of
    overlays dropped."""
    dropped = brushApi.previewClearAll() if brushApi.available() else 0
    _OVERLAID.clear()
    authored = list(_SESSION_AUTHORED)
    _SESSION_AUTHORED.clear()
    layer = stage.GetSessionLayer() if stage is not None else None
    if layer is not None and authored:
        try:
            with Sdf.ChangeBlock():
                for path in authored:
                    primSpec = layer.GetPrimAtPath(path)
                    if primSpec is None:
                        continue
                    spec = primSpec.properties.get(_DISPLAY_NAME)
                    if spec is not None:
                        primSpec.RemoveProperty(spec)
        except Exception:
            pass
    return dropped


def ShownColors(stage, binding):
    """What the viewport shows for binding's surface: [(r, g, b)] per
    face-vertex, or None when no preview is up.

    The overlay path answers with the last colours SetPreview/PatchPreview
    pushed (None once cleared); the session path reads the stage."""
    if stage is None or binding is None:
        return None
    pushed = _OVERLAID.get(str(binding.surfacePath))
    if pushed is not None:
        return [tuple(float(x) for x in row) for row in pushed.tolist()]
    return _stageColors(stage, binding)


def PreviewColors(stage, binding):
    """The previewed colours as [(r, g, b)], or None when absent.

    An overlay up for the surface wins (its last pushed colours, exactly
    what Hydra shows); otherwise the composed stage displayColor."""
    return ShownColors(stage, binding)


def _stageColors(stage, binding):
    if stage is None or binding is None:
        return None
    surface = stage.GetPrimAtPath(binding.surfacePath)
    if not surface or not surface.IsValid():
        return None
    primvar = UsdGeom.PrimvarsAPI(surface).GetPrimvar("displayColor")
    if not primvar or not primvar.HasValue():
        return None
    raw = primvar.GetAttr().Get()
    if raw is None:
        return None
    try:
        import numpy as np
        return [tuple(float(x) for x in row)
                for row in np.asarray(raw, dtype=np.float64).reshape(
                    (-1, 3)).tolist()]
    except ImportError:
        return [(float(c[0]), float(c[1]), float(c[2])) for c in list(raw)]
