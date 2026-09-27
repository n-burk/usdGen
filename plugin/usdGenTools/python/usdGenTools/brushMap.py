# Tool-side paint maps and stroke accumulation for the usdview brush tool.
#
# Two layers live here.
#
# 1. The Python twin of libs/usdGen/usdGen/maps/attributeMap.h: BrushMap,
#    BrushDab, BrushStroke and applyDab carry the same per-face grid, the
#    same dab validation, the same Set/Add/Smooth/Erase application with the
#    inner (hardness) radius, and the same press/move/release/abort contract
#    (plan/08-tools.md section 2.1: every move recomputes from the press-time
#    base, never incrementally). Same formulas, float64 intermediates: values
#    agree with the C++ map to float precision but are NOT bit-identical by
#    contract, so no test asserts bitwise equality across the language
#    boundary. BrushStroke's smooth is the face-local texel stencil of the
#    C++ map (a no-op on bilinear corner data, see LiveStroke).
#
# 2. LiveStroke, the one-call stroke driver brushLoop uses. When the
#    usdGenImaging brush C ABI loads (brushApi.py) the whole move -- the dab
#    and its interpolated trail, the footprint spill onto neighbouring faces,
#    the cross-face corner smooth, touched-face tracking, corner extraction
#    and preview colours -- runs in C++ (usdGenImaging/usdGenBrushApi.h). The
#    pure-Python path below does exactly the same work, slower, so the
#    Qt-free T1 suite and a DLL-less interpreter behave identically.
#
#    Smooth: only four corners per quad persist (the faceVarying bake), and
#    a bilinear patch is its own 3x3 texel mean, so a face-local stencil
#    never changes anything the bake keeps. LiveStroke smooths corners across
#    faces instead: each face-corner the footprint reaches blends, by the dab
#    weight, toward the mean of its mesh vertex (every face-corner sharing it)
#    and that vertex's edge neighbours; affected faces re-upsample from their
#    corners.
#
#    Face mask: a snapshot with a faceMask (a GeomSubset binding) paints
#    only those parent faces. Dabs on a masked face are rejected, the
#    footprint skips masked faces (brushPick.DabFootprint), and the corner
#    smoother's adjacency is built over paintable faces only, so masked
#    faces keep their base values exactly -- the C ABI's rule.
#
# CornerGrid is the corner-backed grid the fast paths hand around (four
# corners per face, upsampled on read). It exposes the BrushMap surface
# (numFaces, clone, fill, fillChannel, cornerValues, getTexel/setTexel,
# sample, ...) and densifies into a BrushMap the first time something writes
# an interior texel. Every grid offers cornerBuffer() -> numpy (F, 4, C).
#
# Qt-free and pxr-free, so the T1 suite drives it in a plain interpreter.

import ctypes
import math

try:
    from . import brushApi
except ImportError:  # file-path test load
    import brushApi


# Mirrors UsdGenBrushMode / UsdGenBrushFalloff / UsdGenAttributeMapInterp.
MODE_SET = "set"
MODE_ADD = "add"
MODE_SMOOTH = "smooth"
MODE_ERASE = "erase"
MODES = (MODE_SET, MODE_ADD, MODE_SMOOTH, MODE_ERASE)

FALLOFF_CONSTANT = "constant"
FALLOFF_LINEAR = "linear"
FALLOFF_SMOOTH = "smooth"
FALLOFFS = (FALLOFF_CONSTANT, FALLOFF_LINEAR, FALLOFF_SMOOTH)

INTERP_NEAREST = "nearest"
INTERP_BILINEAR = "bilinear"

# 2^28 floats, the same allocation guard as the C++ Create.
_MAX_FLOATS = 1 << 28
# A wild move stamps a bounded trail (C++ AddMove).
_MAX_MOVE_STAMPS = 1024


def _isFinite(value):
    return isinstance(value, (int, float)) and math.isfinite(float(value))


def _isPowerOfTwo(value):
    return (isinstance(value, int) and value > 0
            and (value & (value - 1)) == 0)


def _clamp01(value):
    return min(1.0, max(0.0, value))


def _falloffCurve(falloff, t):
    # t runs 1 at the inner radius to 0 at the rim (C++ FalloffCurve).
    if falloff == FALLOFF_CONSTANT:
        return 1.0
    if falloff == FALLOFF_LINEAR:
        return t
    return t * t * (3.0 - 2.0 * t)


def brushWeight(falloff, hardness, dist, radius):
    """The dab weight at `dist` for a dab of `radius` (UsdGenBrushWeight).

    1 inside hardness * radius, the falloff curve from there to the rim,
    0 past it."""
    if not radius > 0.0 or not dist <= radius:
        return 0.0
    h = min(1.0, max(0.0, float(hardness)))
    inner = h * radius
    if dist <= inner:
        return 1.0
    span = radius - inner
    if not span > 0.0:
        return 1.0
    t = min(1.0, max(0.0, 1.0 - (dist - inner) / span))
    return _falloffCurve(falloff, t)


def _falloffWeight(falloff, t):
    # Kept for callers of the pre-hardness helper: t = 1 - dist / radius.
    return _falloffCurve(falloff, t)


def _cornerST(corner, res):
    s = res - 1 if corner in (1, 2) else 0
    t = res - 1 if corner >= 2 else 0
    return (s, t)


_CORNER_UV = ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0))


def _numpy():
    try:
        import numpy
    except ImportError:
        return None
    return numpy


class BrushMapSpec(object):
    """Faces/resolution/channels/default/clamp01, as in attributeMap.h."""

    def __init__(self, numFaces=0, resolution=16, channels=1,
                 defaultValue=0.0, clamp01=False):
        self.numFaces = numFaces
        self.resolution = resolution
        self.channels = channels
        self.defaultValue = defaultValue
        self.clamp01 = clamp01


def _validateSpec(spec):
    if spec.numFaces is None or int(spec.numFaces) <= 0:
        return "attribute map needs numFaces >= 1"
    if (not _isPowerOfTwo(int(spec.resolution or 0))
            or spec.resolution < 1 or spec.resolution > 256):
        return ("attribute map resolution must be a power of two in "
                "[1, 256]")
    if spec.channels not in (1, 3):
        return "attribute map channels must be 1 or 3"
    if not _isFinite(spec.defaultValue):
        return "attribute map default must be finite"
    floats = spec.numFaces * spec.resolution * spec.resolution \
        * spec.channels
    if floats > _MAX_FLOATS:
        return "attribute map grid exceeds 2^28 floats"
    return ""


class BrushMap(object):
    """One per-face texel grid for a surface mesh (tool-side twin)."""

    def __init__(self):
        self._spec = BrushMapSpec()
        self._texels = []

    @staticmethod
    def create(spec):
        """(map, error): None map + a message for an invalid spec."""
        error = _validateSpec(spec)
        if error:
            return (None, error)
        floats = spec.numFaces * spec.resolution * spec.resolution \
            * spec.channels
        default = float(spec.defaultValue)
        if spec.clamp01:
            default = _clamp01(default)
        made = BrushMap()
        made._spec = BrushMapSpec(int(spec.numFaces), int(spec.resolution),
                                  int(spec.channels), default,
                                  bool(spec.clamp01))
        made._texels = [default] * floats
        return (made, "")

    def numFaces(self):
        return self._spec.numFaces

    def resolution(self):
        return self._spec.resolution

    def channels(self):
        return self._spec.channels

    def defaultValue(self):
        return self._spec.defaultValue

    def clamp01(self):
        return self._spec.clamp01

    def floatCount(self):
        return len(self._texels)

    def clone(self):
        made = BrushMap()
        spec = self._spec
        made._spec = BrushMapSpec(spec.numFaces, spec.resolution,
                                  spec.channels, spec.defaultValue,
                                  spec.clamp01)
        made._texels = list(self._texels)
        return made

    def _index(self, face, s, t, channel):
        res = self._spec.resolution
        return ((face * res + t) * res + s) * self._spec.channels + channel

    def _inRange(self, face, s, t, channel):
        res = self._spec.resolution
        return (0 <= face < self._spec.numFaces
                and 0 <= s < res and 0 <= t < res
                and 0 <= channel < self._spec.channels)

    def getTexel(self, face, s, t, channel):
        """(ok, value): False + 0.0 when out of range."""
        if not self._inRange(face, s, t, channel):
            return (False, 0.0)
        return (True, self._texels[self._index(face, s, t, channel)])

    def setTexel(self, face, s, t, channel, value):
        """False for an out-of-range address or a non-finite value."""
        if not _isFinite(value):
            return False
        if not self._inRange(face, s, t, channel):
            return False
        value = float(value)
        if self._spec.clamp01:
            value = _clamp01(value)
        self._texels[self._index(face, s, t, channel)] = value
        return True

    def _texelClamped(self, face, s, t, channel):
        res = self._spec.resolution
        s = min(res - 1, max(0, s))
        t = min(res - 1, max(0, t))
        return self._texels[self._index(face, s, t, channel)]

    def sample(self, face, u, v, channel, interp=INTERP_BILINEAR):
        """(ok, value): bilinear/nearest at face-local (u, v), clamped.

        Grid-node addressing: texel (s, t) sits exactly at
        (s / (res - 1), t / (res - 1)), the same lattice the base
        upsample writes and the dab kernel stamps, so the exact face
        corners are the corner texels and paint lands under the
        cursor instead of half a texel off it."""
        if not (0 <= face < self._spec.numFaces):
            return (False, 0.0)
        if not (0 <= channel < self._spec.channels):
            return (False, 0.0)
        if not _isFinite(u) or not _isFinite(v):
            return (False, 0.0)
        u = _clamp01(float(u))
        v = _clamp01(float(v))
        res = self._spec.resolution
        if interp == INTERP_NEAREST or res == 1:
            s = min(res - 1, int(math.floor(u * (res - 1) + 0.5)))
            t = min(res - 1, int(math.floor(v * (res - 1) + 0.5)))
            return (True, self._texelClamped(face, s, t, channel))
        x = u * (res - 1)
        y = v * (res - 1)
        s0 = int(math.floor(x))
        t0 = int(math.floor(y))
        fx = min(1.0, max(0.0, x - s0))
        fy = min(1.0, max(0.0, y - t0))
        v00 = self._texelClamped(face, s0, t0, channel)
        v10 = self._texelClamped(face, s0 + 1, t0, channel)
        v01 = self._texelClamped(face, s0, t0 + 1, channel)
        v11 = self._texelClamped(face, s0 + 1, t0 + 1, channel)
        return (True, (v00 * (1.0 - fx) + v10 * fx) * (1.0 - fy)
                + (v01 * (1.0 - fx) + v11 * fx) * fy)

    def fill(self, value):
        if not _isFinite(value):
            return
        value = float(value)
        if self._spec.clamp01:
            value = _clamp01(value)
        self._texels = [value] * len(self._texels)

    def fillChannel(self, channel, value):
        """Set every texel of one channel. False unless the channel is in
        range and the value finite (clamp01 honored like fill)."""
        if not _isFinite(value):
            return False
        if not 0 <= channel < self._spec.channels:
            return False
        value = float(value)
        if self._spec.clamp01:
            value = _clamp01(value)
        channels = self._spec.channels
        count = len(self._texels) // channels
        self._texels[channel::channels] = [value] * count
        return True

    def setTexelsBulk(self, values):
        """Replace every texel from a flat list. False unless the length
        matches faces*res*res*channels exactly; entries are stored
        verbatim, so callers pre-validate finiteness (setTexel's
        per-texel skip would defeat the bulk call)."""
        spec = self._spec
        want = (spec.numFaces * spec.resolution * spec.resolution
                * spec.channels)
        if len(values) != want:
            return False
        self._texels = list(values)
        return True

    def cornerValues(self, face, channel):
        """Grid-node samples at (0,0),(1,0),(1,1),(0,1): the faceVarying bake.

        Corner order matches the quad vertex order the pick (u, v) assumes:
        v0=(0,0), v1=(1,0), v2=(1,1), v3=(0,1), so corner i bakes onto the
        i-th face-vertex. (ok, [4 floats])."""
        values = []
        for u, v in _CORNER_UV:
            ok, value = self.sample(face, u, v, channel, INTERP_BILINEAR)
            if not ok:
                return (False, [])
            values.append(value)
        return (True, values)

    def cornerBuffer(self, faces=None):
        """numpy float64 (len(faces) or F, 4, C): the corner texels."""
        np = _numpy()
        spec = self._spec
        res, channels = spec.resolution, spec.channels
        rows = range(spec.numFaces) if faces is None else list(faces)
        if np is None:
            raise RuntimeError("cornerBuffer needs numpy")
        tex = np.asarray(self._texels, dtype=np.float64).reshape(
            (spec.numFaces, res, res, channels))
        sel = tex if faces is None else tex[np.asarray(rows, dtype=np.int64)]
        out = np.empty((len(sel), 4, channels), dtype=np.float64)
        for corner in range(4):
            s, t = _cornerST(corner, res)
            out[:, corner, :] = sel[:, t, s, :]
        return out

    def faceMean(self, face, channel):
        """Exact mean of one face/channel, for coarse readouts. (ok, mean)."""
        if not (0 <= face < self._spec.numFaces):
            return (False, 0.0)
        if not (0 <= channel < self._spec.channels):
            return (False, 0.0)
        res = self._spec.resolution
        total = 0.0
        for t in range(res):
            for s in range(res):
                total += self._texels[self._index(face, s, t, channel)]
        return (True, total / (res * res))


def _upsampleFace(dense, face, corners, c0, c1):
    """Bilinear corners[i][c] -> every texel of `face`, channels [c0, c1)."""
    spec = dense._spec
    res, channels = spec.resolution, spec.channels
    tex = dense._texels
    base = face * res * res * channels
    for t in range(res):
        v = t / (res - 1) if res > 1 else 0.5
        row = base + t * res * channels
        for s in range(res):
            u = s / (res - 1) if res > 1 else 0.5
            o = row + s * channels
            for c in range(c0, c1):
                top = corners[0][c] * (1.0 - u) + corners[1][c] * u
                bottom = corners[3][c] * (1.0 - u) + corners[2][c] * u
                tex[o + c] = top * (1.0 - v) + bottom * v


class CornerGrid(object):
    """A corner-backed per-face grid: four corners per face, bilinear inside.

    Exactly what a BrushMap upsampled from the same corners reads like
    (texel (s, t) = bilinear at (s/(res-1), t/(res-1))), without storing the
    texels. The first interior write densifies it into a BrushMap that it
    then delegates to."""

    def __init__(self, corners, resolution, defaultValue=0.0):
        np = _numpy()
        self._corners = np.array(corners, dtype=np.float64)
        if self._corners.ndim != 3 or self._corners.shape[1] != 4:
            raise ValueError("corners must be (faces, 4, channels)")
        self._res = int(resolution)
        self._default = float(defaultValue)
        self._dense = None

    @staticmethod
    def create(spec, corners=None):
        """(grid, error), like BrushMap.create; corners (F, 4, C) or None."""
        np = _numpy()
        if np is None:
            return (None, "CornerGrid needs numpy")
        error = _validateSpec(spec)
        if error:
            return (None, error)
        if corners is None:
            corners = np.full((int(spec.numFaces), 4, int(spec.channels)),
                              float(spec.defaultValue))
        corners = np.asarray(corners, dtype=np.float64)
        if corners.shape != (int(spec.numFaces), 4, int(spec.channels)):
            return (None, "corner buffer shape %r does not match the spec"
                    % (corners.shape,))
        return (CornerGrid(corners, spec.resolution, spec.defaultValue), "")

    # -- the dense fallback --------------------------------------------------

    def dense(self):
        """The BrushMap this grid is (densifying it on first call)."""
        if self._dense is None:
            spec = BrushMapSpec(self.numFaces(), self._res, self.channels(),
                                self._default, False)
            made, _error = BrushMap.create(spec)
            flat = _upsampleNumpy(self._corners, self._res)
            made._texels = flat.reshape((-1,)).tolist()
            self._dense = made
        return self._dense

    # -- the BrushMap surface ------------------------------------------------

    def numFaces(self):
        return int(self._corners.shape[0])

    def resolution(self):
        return self._res

    def channels(self):
        return int(self._corners.shape[2])

    def defaultValue(self):
        return self._default

    def clamp01(self):
        return False

    def floatCount(self):
        return self.numFaces() * self._res * self._res * self.channels()

    def clone(self):
        if self._dense is not None:
            return self._dense.clone()
        return CornerGrid(self._corners.copy(), self._res, self._default)

    def _cornerAt(self, face, corner, channel):
        return float(self._corners[face, corner, channel])

    def getTexel(self, face, s, t, channel):
        if self._dense is not None:
            return self._dense.getTexel(face, s, t, channel)
        res = self._res
        if not (0 <= face < self.numFaces() and 0 <= s < res
                and 0 <= t < res and 0 <= channel < self.channels()):
            return (False, 0.0)
        u = s / (res - 1) if res > 1 else 0.5
        v = t / (res - 1) if res > 1 else 0.5
        return (True, self._bilinear(face, u, v, channel))

    def _bilinear(self, face, u, v, channel):
        c = self._corners[face, :, channel]
        top = float(c[0]) * (1.0 - u) + float(c[1]) * u
        bottom = float(c[3]) * (1.0 - u) + float(c[2]) * u
        return top * (1.0 - v) + bottom * v

    def setTexel(self, face, s, t, channel, value):
        return self.dense().setTexel(face, s, t, channel, value)

    def sample(self, face, u, v, channel, interp=INTERP_BILINEAR):
        if self._dense is not None or interp == INTERP_NEAREST:
            return self.dense().sample(face, u, v, channel, interp)
        if not (0 <= face < self.numFaces()):
            return (False, 0.0)
        if not (0 <= channel < self.channels()):
            return (False, 0.0)
        if not _isFinite(u) or not _isFinite(v):
            return (False, 0.0)
        if self._res == 1:
            return self.getTexel(face, 0, 0, channel)
        # Bilinear over grid nodes of a bilinear patch is the patch itself.
        return (True, self._bilinear(face, _clamp01(float(u)),
                                     _clamp01(float(v)), channel))

    def fill(self, value):
        if self._dense is not None:
            return self._dense.fill(value)
        if not _isFinite(value):
            return
        self._corners[...] = float(value)

    def fillChannel(self, channel, value):
        if self._dense is not None:
            return self._dense.fillChannel(channel, value)
        if not _isFinite(value) or not 0 <= channel < self.channels():
            return False
        self._corners[:, :, channel] = float(value)
        return True

    def setTexelsBulk(self, values):
        return self.dense().setTexelsBulk(values)

    def cornerValues(self, face, channel):
        if self._dense is not None:
            return self._dense.cornerValues(face, channel)
        if not (0 <= face < self.numFaces()
                and 0 <= channel < self.channels()):
            return (False, [])
        if self._res == 1:
            ok, value = self.getTexel(face, 0, 0, channel)
            return (True, [value] * 4)
        return (True, [float(x) for x in self._corners[face, :, channel]])

    def cornerBuffer(self, faces=None):
        if self._dense is not None:
            return self._dense.cornerBuffer(faces)
        np = _numpy()
        out = self._corners
        if self._res == 1:
            mean = out.mean(axis=1, keepdims=True)
            out = np.repeat(mean, 4, axis=1)
        if faces is None:
            return out.copy()
        return out[np.asarray(list(faces), dtype=np.int64)].copy()

    def faceMean(self, face, channel):
        if self._dense is not None:
            return self._dense.faceMean(face, channel)
        if not (0 <= face < self.numFaces()
                and 0 <= channel < self.channels()):
            return (False, 0.0)
        return (True, float(self._corners[face, :, channel].mean()))


def _upsampleNumpy(corners, res):
    """(F, 4, C) corners -> (F, res, res, C) texels, BrushMap layout."""
    np = _numpy()
    if res <= 1:
        steps = np.full(1, 0.5)
    else:
        steps = np.arange(res, dtype=np.float64) / float(res - 1)
    uu = steps[None, :]
    vv = steps[:, None]
    weights = np.stack([(1.0 - uu) * (1.0 - vv), uu * (1.0 - vv),
                        uu * vv, (1.0 - uu) * vv])
    return np.einsum("its,fic->ftsc", weights,
                     np.asarray(corners, dtype=np.float64))


def denseOf(grid):
    """The BrushMap behind any grid (a CornerGrid densifies in place)."""
    if isinstance(grid, CornerGrid):
        return grid.dense()
    return grid


def cornerBufferOf(grid, faces=None):
    """numpy (F, 4, C) corners of any grid object, or None without numpy.

    Grids without cornerBuffer (a caller's own duck type) are read
    through cornerValues."""
    np = _numpy()
    if np is None:
        return None
    getter = getattr(grid, "cornerBuffer", None)
    if getter is not None:
        return np.asarray(getter(faces), dtype=np.float64)
    rows = range(grid.numFaces()) if faces is None else list(faces)
    channels = grid.channels()
    out = np.empty((len(rows), 4, channels), dtype=np.float64)
    for k, face in enumerate(rows):
        for c in range(channels):
            ok, corners = grid.cornerValues(face, c)
            if not ok:
                raise ValueError("face %d failed to sample" % face)
            out[k, :, c] = corners
    return out


class BrushDab(object):
    """One brush stamp; radius in face-uv units, strength in [0, 1].

    hardness in [0, 1] is the inner-radius fraction: full weight inside
    hardness * radius, the falloff curve from there to the rim."""

    def __init__(self, face=-1, u=0.0, v=0.0, radius=0.1, strength=0.5,
                 value=1.0, channel=-1, mode=MODE_SET,
                 falloff=FALLOFF_SMOOTH, hardness=0.0):
        self.face = face
        self.u = u
        self.v = v
        self.radius = radius
        self.strength = strength
        self.value = value
        self.channel = channel
        self.mode = mode
        self.falloff = falloff
        self.hardness = hardness

    def copy(self):
        return BrushDab(self.face, self.u, self.v, self.radius,
                        self.strength, self.value, self.channel,
                        self.mode, self.falloff, self.hardness)


def applyDab(liveMap, dab):
    """Apply one validated dab to a live map, in place (the move kernel).

    A CornerGrid densifies first (the kernel writes interior texels)."""
    liveMap = denseOf(liveMap)
    channels = liveMap.channels()
    res = liveMap.resolution()
    first = 0 if dab.channel < 0 else dab.channel
    last = channels if dab.channel < 0 else dab.channel + 1
    if dab.strength == 0.0:
        return
    mode = dab.mode
    target = dab.value
    if mode == MODE_ERASE:
        # Erase is Set toward the map default.
        mode = MODE_SET
        target = liveMap.defaultValue()
    hardness = getattr(dab, "hardness", 0.0) or 0.0

    if not dab.radius > 0.0:
        s = min(res - 1, max(0, int(math.floor(dab.u * (res - 1) + 0.5))))
        t = min(res - 1, max(0, int(math.floor(dab.v * (res - 1) + 0.5))))
        for c in range(first, last):
            ok, current = liveMap.getTexel(dab.face, s, t, c)
            if not ok:
                continue
            if mode == MODE_SET:
                liveMap.setTexel(dab.face, s, t, c,
                                 current + (target - current)
                                 * dab.strength)
            elif mode == MODE_ADD:
                liveMap.setTexel(dab.face, s, t, c,
                                 current + dab.value * dab.strength)
            # Smooth of a single texel with no footprint is the identity.
        return

    radius = dab.radius
    span = res - 1 if res > 1 else 1
    sLo = max(0, int(math.floor((dab.u - radius) * span)))
    sHi = min(res - 1, int(math.ceil((dab.u + radius) * span)))
    tLo = max(0, int(math.floor((dab.v - radius) * span)))
    tHi = min(res - 1, int(math.ceil((dab.v + radius) * span)))

    # Smooth reads the 3x3 neighbourhood mean from the pre-dab map, folded
    # once up front in the C++ order (dt-outer, ds-inner). A bad face paints
    # nothing in any mode, hence the up-front exit.
    if dab.face < 0 or dab.face >= liveMap.numFaces():
        return
    means = None
    if mode == MODE_SMOOTH:
        tex = liveMap._texels
        step = liveMap._spec.channels
        base = dab.face * res * res * step
        means = {}
        for t in range(tLo, tHi + 1):
            tUp = t - 1 if t > 0 else 0
            tDn = t + 1 if t < res - 1 else res - 1
            rowUp = base + tUp * res * step
            rowMid = base + t * res * step
            rowDn = base + tDn * res * step
            for s in range(sLo, sHi + 1):
                sLf = s - 1 if s > 0 else 0
                sRt = s + 1 if s < res - 1 else res - 1
                oLf, oMid, oRt = sLf * step, s * step, sRt * step
                for c in range(first, last):
                    mean = (tex[rowUp + oLf + c]
                            + tex[rowUp + oMid + c]
                            + tex[rowUp + oRt + c]
                            + tex[rowMid + oLf + c]
                            + tex[rowMid + oMid + c]
                            + tex[rowMid + oRt + c]
                            + tex[rowDn + oLf + c]
                            + tex[rowDn + oMid + c]
                            + tex[rowDn + oRt + c]) / 9.0
                    means[(s, t, c)] = mean

    for t in range(tLo, tHi + 1):
        for s in range(sLo, sHi + 1):
            cu = s / span if res > 1 else 0.5
            cv = t / span if res > 1 else 0.5
            dist = math.sqrt((cu - dab.u) ** 2 + (cv - dab.v) ** 2)
            if dist > radius:
                continue
            k = dab.strength * brushWeight(dab.falloff, hardness, dist,
                                           radius)
            if k == 0.0:
                continue
            for c in range(first, last):
                ok, current = liveMap.getTexel(dab.face, s, t, c)
                if not ok:
                    continue
                if mode == MODE_SET:
                    liveMap.setTexel(dab.face, s, t, c,
                                     current + (target - current) * k)
                elif mode == MODE_ADD:
                    liveMap.setTexel(dab.face, s, t, c,
                                     current + dab.value * k)
                else:
                    mean = means[(s, t, c)]
                    liveMap.setTexel(dab.face, s, t, c,
                                     current + (mean - current) * k)


class MeshAdjacency(object):
    """Vertex -> face-corners and vertex -> edge-neighbour vertices.

    faceMask (paintable face ids, or None for all) limits both maps to
    paintable faces, like the C ABI's masked mesh."""

    def __init__(self, faces, faceMask=None):
        self.faces = faces
        corners = {}
        nbrs = {}
        for f, quad in enumerate(faces):
            if faceMask is not None and f not in faceMask:
                continue
            for i in range(4):
                p = quad[i]
                corners.setdefault(p, []).append((f, i))
                ring = nbrs.setdefault(p, set())
                ring.add(quad[(i + 1) % 4])
                ring.add(quad[(i + 3) % 4])
        self.corners = corners
        self.nbrs = dict((p, sorted(q for q in ring if q != p))
                         for p, ring in nbrs.items())


class BrushAccumulator(object):
    """Non-accumulating stroke application (UsdGenBrushAccumulator's twin).

    Overlapping stamps along a move must not re-apply Set: tex +=
    (value - tex) * k saturates every texel under the trail after a few
    stamps, washing the falloff out (strength 1 paints a hard disc). Set /
    Erase / Smooth keep a per-texel max weight over the current SEGMENT (a
    run of dabs with the same mode, target and channel) and write
    segmentBase + (target - segmentBase) * wmax; Add accumulates per stamp
    and ends the segment. The corner smoother keeps the same model per
    face-corner (cornerW over a snapshot of the segment-start corners)."""

    def __init__(self, liveMap):
        self.map = denseOf(liveMap)
        self.flush()

    def flush(self):
        """End the texel segment and the corner segment."""
        self._key = None
        self._base = None
        self._wmax = None
        self.cornerActive = False
        self.cornerChannel = -1
        self.cornerBase = None
        self.cornerW = None

    def _flushTexels(self):
        self._key = None
        self._base = None
        self._wmax = None

    def apply(self, dab):
        """Apply one validated stamp (texel level)."""
        live = self.map
        self.cornerActive = False
        if dab.mode == MODE_ADD:
            self._flushTexels()
            applyDab(live, dab)
            return
        if dab.strength == 0.0:
            return
        if dab.face < 0 or dab.face >= live.numFaces():
            return
        target = (live.defaultValue() if dab.mode == MODE_ERASE
                  else float(dab.value))
        key = (dab.mode, None if dab.mode == MODE_SMOOTH else target,
               dab.channel)
        spec = live._spec
        res, channels = spec.resolution, spec.channels
        if self._key != key:
            self._key = key
            self._base = list(live._texels)
            self._wmax = [0.0] * (spec.numFaces * res * res)
        base = self._base
        tex = live._texels
        first = 0 if dab.channel < 0 else dab.channel
        last = channels if dab.channel < 0 else dab.channel + 1
        smooth = dab.mode == MODE_SMOOTH
        faceBase = dab.face * res * res

        def baseAt(s, t, c):
            s = min(res - 1, max(0, s))
            t = min(res - 1, max(0, t))
            return base[(faceBase + t * res + s) * channels + c]

        def stamp(s, t, w):
            i = faceBase + t * res + s
            if not w > self._wmax[i]:
                return
            self._wmax[i] = w
            for c in range(first, last):
                b = baseAt(s, t, c)
                goal = target
                if smooth:
                    goal = (baseAt(s - 1, t - 1, c) + baseAt(s, t - 1, c)
                            + baseAt(s + 1, t - 1, c) + baseAt(s - 1, t, c)
                            + baseAt(s, t, c) + baseAt(s + 1, t, c)
                            + baseAt(s - 1, t + 1, c) + baseAt(s, t + 1, c)
                            + baseAt(s + 1, t + 1, c)) / 9.0
                value = b + (goal - b) * w
                if spec.clamp01:
                    value = _clamp01(value)
                tex[i * channels + c] = value

        if not dab.radius > 0.0:
            if smooth:
                return
            s = min(res - 1, max(0, int(math.floor(dab.u * (res - 1) + 0.5))))
            t = min(res - 1, max(0, int(math.floor(dab.v * (res - 1) + 0.5))))
            stamp(s, t, dab.strength)
            return
        radius = dab.radius
        span = res - 1 if res > 1 else 1
        sLo = max(0, int(math.floor((dab.u - radius) * span)))
        sHi = min(res - 1, int(math.ceil((dab.u + radius) * span)))
        tLo = max(0, int(math.floor((dab.v - radius) * span)))
        tHi = min(res - 1, int(math.ceil((dab.v + radius) * span)))
        hardness = getattr(dab, "hardness", 0.0) or 0.0
        for t in range(tLo, tHi + 1):
            for s in range(sLo, sHi + 1):
                cu = s / span if res > 1 else 0.5
                cv = t / span if res > 1 else 0.5
                dist = math.sqrt((cu - dab.u) ** 2 + (cv - dab.v) ** 2)
                if dist > radius:
                    continue
                w = dab.strength * brushWeight(dab.falloff, hardness, dist,
                                               radius)
                if w > 0.0:
                    stamp(s, t, w)


def smoothCorners(liveMap, dab, stamps, adjacency, painter=None):
    """The cross-face corner smooth (the C++ StrokeData::SmoothCorners).

    stamps: [(face, u, v, radiusUV)] of the dab's footprint. painter is the
    stroke's BrushAccumulator (its corner segment keeps the max weights);
    None smooths this one dab on its own. Returns the faces whose corners
    changed."""
    dense = denseOf(liveMap)
    if painter is None:
        painter = BrushAccumulator(dense)
    spec = dense._spec
    res, channels = spec.resolution, spec.channels
    c0 = 0 if dab.channel < 0 else dab.channel
    c1 = channels if dab.channel < 0 else dab.channel + 1
    hardness = getattr(dab, "hardness", 0.0) or 0.0
    tex = dense._texels

    def corner(face, i, c):
        s, t = _cornerST(i, res)
        return tex[((face * res + t) * res + s) * channels + c]

    if not painter.cornerActive or painter.cornerChannel != dab.channel:
        painter._flushTexels()
        painter.cornerActive = True
        painter.cornerChannel = dab.channel
        painter.cornerBase = [[[corner(f, i, c) for c in range(channels)]
                               for i in range(4)]
                              for f in range(spec.numFaces)]
        painter.cornerW = [[0.0] * 4 for _f in range(spec.numFaces)]
    cornerBase = painter.cornerBase
    hits = []
    for face, u, v, radius in stamps:
        if not radius > 0.0:
            continue
        for i, (cu, cv) in enumerate(_CORNER_UV):
            dist = math.sqrt((cu - u) ** 2 + (cv - v) ** 2)
            k = dab.strength * brushWeight(dab.falloff, hardness, dist,
                                           radius)
            if k > painter.cornerW[face][i]:
                painter.cornerW[face][i] = k
                hits.append((face, i, k))
    if not hits:
        return []

    memo = {}

    def vertexMean(p):
        got = memo.get(p)
        if got is None:
            members = adjacency.corners.get(p, [])
            got = [0.0] * channels
            for f, i in members:
                for c in range(c0, c1):
                    got[c] += cornerBase[f][i][c]
            if members:
                got = [x / len(members) for x in got]
            memo[p] = got
        return got

    updates = []
    for face, i, k in hits:
        p = adjacency.faces[face][i]
        ring = [p] + adjacency.nbrs.get(p, [])
        values = []
        for c in range(c0, c1):
            target = sum(vertexMean(q)[c] for q in ring) / len(ring)
            cur = cornerBase[face][i][c]
            value = cur + (target - cur) * k
            if spec.clamp01:
                value = _clamp01(value)
            values.append(value)
        updates.append((face, i, values))
    order = []
    cornersByFace = {}
    for face, i, values in updates:
        if face not in cornersByFace:
            order.append(face)
            cornersByFace[face] = [[corner(face, j, c)
                                    for c in range(channels)]
                                   for j in range(4)]
        for c, value in zip(range(c0, c1), values):
            cornersByFace[face][i][c] = value
    for face in order:
        _upsampleFace(dense, face, cornersByFace[face], c0, c1)
    return order


class BrushStroke(object):
    """Press-moves-release accumulation over a press-time base map."""

    def __init__(self, base):
        self._base = base
        self._dabs = []

    def _validateDab(self, dab):
        if self._base is None:
            return "brush stroke has no base map"
        if not isinstance(dab.face, int) or not (
                0 <= dab.face < self._base.numFaces()):
            return ("brush dab face %r is outside the base map"
                    % (dab.face,))
        if not _isFinite(dab.u) or not _isFinite(dab.v):
            return "brush dab (u, v) must be finite"
        if not _isFinite(dab.radius) or dab.radius < 0.0:
            return "brush dab radius must be finite and >= 0"
        if (not _isFinite(dab.strength) or dab.strength < 0.0
                or dab.strength > 1.0):
            return "brush dab strength must be finite and in [0, 1]"
        if not _isFinite(dab.value):
            return "brush dab value must be finite"
        hardness = getattr(dab, "hardness", 0.0)
        if not _isFinite(hardness) or hardness < 0.0 or hardness > 1.0:
            return "brush dab hardness must be finite and in [0, 1]"
        if (not isinstance(dab.channel, int) or dab.channel < -1
                or dab.channel >= self._base.channels()):
            return "brush dab channel is outside the base map"
        if dab.mode not in MODES:
            return "brush dab mode %r is unknown" % (dab.mode,)
        if dab.falloff not in FALLOFFS:
            return "brush dab falloff %r is unknown" % (dab.falloff,)
        return ""

    def addDab(self, dab):
        """Record one dab. (True, "") or (False, reason)."""
        error = self._validateDab(dab)
        if error:
            return (False, error)
        self._dabs.append(dab.copy())
        return (True, "")

    def addMove(self, face, u, v, radius, strength, value, channel,
                mode, falloff, spacing=0.5, hardness=0.0):
        """Interpolated dabs from the previous centre to (face, u, v).

        Same-face segments are stamped so no gap exceeds spacing * radius;
        a face change (v1 never interpolates across faces) or no previous
        dab records one dab. A rejected move records nothing."""
        if not _isFinite(spacing) or not 0.0 < spacing <= 1.0:
            return (False, "brush move spacing must be finite and in "
                    "(0, 1]")
        dab = BrushDab(face, u, v, radius, strength, value, channel,
                       mode, falloff, hardness)
        error = self._validateDab(dab)
        if error:
            return (False, error)
        if not (self._dabs and self._dabs[-1].face == face
                and radius > 0.0 and _isFinite(u) and _isFinite(v)):
            self._dabs.append(dab)
            return (True, "")
        prev = self._dabs[-1]
        dx = u - prev.u
        dy = v - prev.v
        dist = math.sqrt(dx * dx + dy * dy)
        step = spacing * radius
        steps = int(math.ceil(dist / step)) if step > 0.0 else 1
        steps = min(_MAX_MOVE_STAMPS, max(1, steps))
        for i in range(1, steps + 1):
            t = float(i) / steps
            stamp = dab.copy()
            stamp.u = prev.u + dx * t
            stamp.v = prev.v + dy * t
            self._dabs.append(stamp)
        return (True, "")

    def dabCount(self):
        return len(self._dabs)

    def dab(self, index):
        return self._dabs[index]

    def preview(self, expand=None):
        """The whole map recomputed from the base plus every dab, in order.

        Pure: no state accumulates, so a dropped move cannot corrupt it and
        calling it twice yields identical maps. None with no base. Dabs go
        through a BrushAccumulator: overlapping Set / Erase / Smooth stamps
        take the max weight instead of re-applying.
        expand(dab) maps one recorded dab to its stamps (the loop's
        multi-face footprint); None paints each dab as recorded."""
        if self._base is None:
            return None
        live = denseOf(self._base).clone()
        painter = BrushAccumulator(live)
        for dab in self._dabs:
            stamps = expand(dab) if expand is not None else (dab,)
            for stamp in stamps:
                painter.apply(stamp)
        return live

    def commit(self, expand=None):
        """The release path: the same recomputed map the tool bakes once."""
        return self.preview(expand=expand)

    def abort(self):
        """The escape path: forget every dab; the base was never touched."""
        del self._dabs[:]


# -- LiveStroke: the one-call stroke driver ----------------------------------

def _pickModule():
    try:
        from . import brushPick
    except ImportError:  # file-path test load
        import brushPick
    return brushPick


class LiveStroke(object):
    """One stroke over a bound surface: dabs in, working/committed grids out.

    Created per gesture from the mesh snapshot and the press-time base
    grid. Native (C++) when the brush ABI loads, pure Python otherwise;
    both paths paint, spill and smooth identically (see the module
    header)."""

    def __init__(self):
        self.native = False
        self._lib = None
        self._handle = None
        self._mesh = None
        self._numFaces = 0
        self._channels = 1
        self._resolution = 1
        self._default = 0.0
        # Python path.
        self._snapshot = None
        self._base = None
        self._working = None
        self._stroke = None
        self._footprint = None
        self._adjacency = None
        self._touched = set()
        # The snapshot's paintable faces (None: all), both paths.
        self._faceMask = None
        # Every accepted dab() call as a BrushDab (call level: a move's
        # interpolated trail is not expanded here). Status lines and
        # tests read it; it never drives painting.
        self.calls = []

    @staticmethod
    def create(snapshot, base, native=None):
        """(LiveStroke, error). native None: C++ when available."""
        if base is None:
            return (None, "a live stroke needs a base grid")
        try:
            faces = base.numFaces()
            channels = base.channels()
            res = base.resolution()
            default = base.defaultValue()
        except Exception as exc:
            return (None, "the base grid is unusable (%s)" % exc)
        if snapshot is not None and len(snapshot.faces) != faces:
            return (None, "the base grid has %d faces for a %d-face mesh"
                    % (faces, len(snapshot.faces)))
        live = LiveStroke()
        live._numFaces = faces
        live._channels = channels
        live._resolution = res
        live._default = float(default)
        live._faceMask = getattr(snapshot, "faceMask", None)
        useNative = brushApi.available() if native is None else bool(native)
        if useNative:
            ok, error = live._initNative(snapshot, base)
            if ok:
                return (live, "")
            if native:
                return (None, error)
        live._initPython(snapshot, base)
        return (live, "")

    def _initNative(self, snapshot, base):
        lib = brushApi.load()
        if lib is None:
            return (False, brushApi.reason())
        np = _numpy()
        mesh = brushApi.meshFor(snapshot) if snapshot is not None else None
        if snapshot is not None and mesh is None:
            return (False, "the native mesh did not build")
        try:
            corners = np.ascontiguousarray(
                cornerBufferOf(base), dtype=np.float32)
        except Exception as exc:
            return (False, "the base grid failed to sample (%s)" % exc)
        handle = lib.UsdGenBrush_StrokeCreate(
            mesh.handle if mesh is not None else None, self._numFaces,
            self._resolution, self._channels, brushApi.floatPtr(corners),
            self._default)
        if not handle:
            return (False, "the native stroke did not build")
        self.native = True
        self._lib = lib
        self._handle = handle
        self._mesh = mesh
        return (True, "")

    def _initPython(self, snapshot, base):
        self.native = False
        self._snapshot = snapshot
        self._base = denseOf(base).clone()
        self._working = self._base.clone()
        self._painter = BrushAccumulator(self._working)
        self._stroke = BrushStroke(self._base)
        if snapshot is not None:
            self._footprint = _pickModule().DabFootprint(snapshot)
            self._adjacency = MeshAdjacency(snapshot.faces, self._faceMask)

    # -- lifetime ------------------------------------------------------------

    def close(self):
        if self._handle and self._lib is not None:
            self._lib.UsdGenBrush_StrokeDestroy(self._handle)
        self._handle = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    # -- the move ------------------------------------------------------------

    def dab(self, face, u, v, radiusUV, hardness, strength, value, channel,
            mode, falloff, isMove=False, spacing=0.5):
        """Record one primary dab and paint it (plus trail and spill).

        (stampsApplied, "") or (-1, reason); a rejected dab records
        nothing -- a dab on a face outside the snapshot's faceMask
        included."""
        if mode not in MODES:
            return (-1, "brush dab mode %r is unknown" % (mode,))
        if falloff not in FALLOFFS:
            return (-1, "brush dab falloff %r is unknown" % (falloff,))
        if self._faceMask is not None and face not in self._faceMask:
            return (-1, "brush dab face %r is outside the bound face "
                    "subset" % (face,))
        if self.native:
            if not self._handle:
                return (-1, "the stroke is closed")
            try:
                got = self._lib.UsdGenBrush_StrokeDab(
                    self._handle, int(face), float(u), float(v),
                    float(radiusUV), float(hardness), float(strength),
                    float(value), int(channel),
                    brushApi.MODE_CODES[mode], brushApi.FALLOFF_CODES[falloff],
                    1 if isMove else 0, float(spacing))
            except (TypeError, ValueError, ctypes.ArgumentError) as exc:
                return (-1, "brush dab rejected (%s)" % exc)
            if got < 0:
                return (-1, "brush dab rejected (code %d)" % got)
            self._record(face, u, v, radiusUV, hardness, strength, value,
                         channel, mode, falloff)
            return (got, "")
        before = self._stroke.dabCount()
        if isMove:
            ok, error = self._stroke.addMove(
                face, u, v, radiusUV, strength, value, channel, mode,
                falloff, spacing, hardness)
        else:
            ok, error = self._stroke.addDab(BrushDab(
                face, u, v, radiusUV, strength, value, channel, mode,
                falloff, hardness))
        if not ok:
            return (-1, error)
        stamps = 0
        for index in range(before, self._stroke.dabCount()):
            stamps += self._applyPrimary(self._painter,
                                         self._stroke.dab(index), True)
        self._record(face, u, v, radiusUV, hardness, strength, value,
                     channel, mode, falloff)
        return (stamps, "")

    def _record(self, face, u, v, radiusUV, hardness, strength, value,
                channel, mode, falloff):
        self.calls.append(BrushDab(int(face), float(u), float(v),
                                   float(radiusUV), float(strength),
                                   float(value), int(channel), mode,
                                   falloff, float(hardness)))

    def _expand(self, dab):
        if self._snapshot is None or self._footprint is None \
                or not dab.radius > 0.0:
            return [(dab.face, dab.u, dab.v, dab.radius)]
        pick = _pickModule()
        point = pick.facePoint(self._snapshot, dab.face, dab.u, dab.v)
        if point is None:
            return [(dab.face, dab.u, dab.v, dab.radius)]
        return self._footprint.expand(dab.face, dab.u, dab.v, point,
                                      dab.radius) \
            or [(dab.face, dab.u, dab.v, dab.radius)]

    def _applyPrimary(self, painter, dab, track):
        stamps = self._expand(dab)
        if dab.mode == MODE_SMOOTH and self._adjacency is not None:
            if dab.strength > 0.0:
                changed = smoothCorners(painter.map, dab, stamps,
                                        self._adjacency, painter)
                if track:
                    self._touched.update(changed)
            return len(stamps)
        for face, u, v, radius in stamps:
            stamp = dab.copy()
            stamp.face, stamp.u, stamp.v, stamp.radius = face, u, v, radius
            painter.apply(stamp)
            if track and dab.strength > 0.0:
                self._touched.add(face)
        return len(stamps)

    # -- readouts ------------------------------------------------------------

    def numFaces(self):
        return self._numFaces

    def channels(self):
        return self._channels

    def resolution(self):
        return self._resolution

    def dabCount(self):
        if self.native:
            return max(0, self._lib.UsdGenBrush_StrokeDabCount(self._handle))
        return self._stroke.dabCount()

    def takeTouched(self):
        """Faces changed since the last take, ascending."""
        if self.native:
            np = _numpy()
            total = self._lib.UsdGenBrush_StrokeTakeTouched(
                self._handle, None, 0)
            if total <= 0:
                return []
            out = np.empty(total, dtype=np.int32)
            self._lib.UsdGenBrush_StrokeTakeTouched(
                self._handle, brushApi.intPtr(out), total)
            return [int(f) for f in out]
        touched = sorted(self._touched)
        self._touched = set()
        return touched

    def workingCorners(self):
        """numpy (F, 4, C) float64 of the working grid; None on a C error."""
        if self.native:
            np = _numpy()
            out = np.empty((self._numFaces, 4, self._channels),
                           dtype=np.float32)
            got = self._lib.UsdGenBrush_StrokeWorkingCorners(
                self._handle, brushApi.floatPtr(out))
            if got != out.size:
                return None
            return out.astype(np.float64)
        return cornerBufferOf(self._working)

    def workingGrid(self):
        """The working grid (BrushMap surface) for preview / live writes."""
        if self.native:
            corners = self.workingCorners()
            if corners is None:
                return None
            return CornerGrid(corners, self._resolution, self._default)
        return self._working

    def commitGrid(self):
        """Base + every recorded dab, recomputed from scratch."""
        if self.native:
            np = _numpy()
            out = np.empty((self._numFaces, 4, self._channels),
                           dtype=np.float32)
            got = self._lib.UsdGenBrush_StrokeCommitCorners(
                self._handle, brushApi.floatPtr(out))
            if got != out.size:
                return None  # release reports it and keeps the stroke live
            return CornerGrid(out.astype(np.float64), self._resolution,
                              self._default)
        grid = self._base.clone()
        painter = BrushAccumulator(grid)
        for index in range(self._stroke.dabCount()):
            self._applyPrimary(painter, self._stroke.dab(index), False)
        return grid

    def previewColors(self, channel=0, colorMap="heat", lo=0.0, hi=1.0):
        """numpy (F*4, 3) float32 corner colours of the working grid."""
        np = _numpy()
        if self.native:
            out = np.empty((self._numFaces * 4, 3), dtype=np.float32)
            got = self._lib.UsdGenBrush_StrokePreviewColors(
                self._handle, int(channel),
                brushApi.COLORMAP_CODES.get(colorMap, 0), float(lo),
                float(hi), brushApi.floatPtr(out))
            if got >= 0:
                return out
        corners = self.workingCorners()
        if corners is None:
            return None
        return rampColors(corners[:, :, channel].reshape(-1),
                          colorMap, lo, hi)

    def abort(self):
        """Forget every dab; the working grid returns to the base."""
        del self.calls[:]
        if self.native:
            self._lib.UsdGenBrush_StrokeAbort(self._handle)
            return
        self._stroke.abort()
        self._working = self._base.clone()
        self._painter = BrushAccumulator(self._working)
        self._touched = set(range(self._numFaces))


def rampColors(values, colorMap="heat", lo=0.0, hi=1.0):
    """numpy float32 (N, 3): brushPreview's heat/gray ramps, vectorised."""
    np = _numpy()
    values = np.asarray(values, dtype=np.float64)
    lo, hi = float(lo), float(hi)
    if hi <= lo:
        t = np.where(values < lo, 0.0, 1.0)
    else:
        t = np.clip((values - lo) / (hi - lo), 0.0, 1.0)
    out = np.zeros((len(t), 3), dtype=np.float32)
    if colorMap == "gray":
        out[:, 0] = t
        out[:, 1] = t
        out[:, 2] = t
        return out
    low = t < 0.5
    mid = (t >= 0.5) & (t < 0.75)
    high = t >= 0.75
    out[low, 0] = t[low] * 2.0
    out[mid, 0] = 1.0
    out[mid, 1] = (t[mid] - 0.5) * 4.0
    out[high, 0] = 1.0
    out[high, 1] = 1.0
    out[high, 2] = (t[high] - 0.75) * 4.0
    return out
