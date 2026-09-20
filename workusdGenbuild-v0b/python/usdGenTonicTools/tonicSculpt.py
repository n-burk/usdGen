# usdGenTonicTools.tonicSculpt -- Qt-free Sculpt-mode helpers (plan/17 P4).
#
# Pure-numpy brush previews over center curves and guides plus the status
# text builders behind Sculpt mode (plan/17 section 5.5, contract 8).
# Everything here imports headlessly (no pxr, no Qt); the C++ model owns the
# curves, and the gizmo UI owns the event filter. Covered by
# testUsdGenTonicToolsHierarchy.py from day one.
#
# The preview math mirrors the C++ kernels (same formulas as tonicMath.py);
# the committed values always come from the C API, never from these
# functions. This module is self-contained on purpose: file-path test loads
# never import siblings, so the falloff below duplicates
# tonicMath.brushFalloff instead of importing it.
from __future__ import annotations

import ctypes

import numpy as np

# Brushes over center curves and guides (never over amplified hair: that
# stays the M5 shelf, plan/17 section 5.8 / D-open-3).
BRUSHES = ("grab", "smooth", "comb", "lengthen", "twist")

BRUSH_LABELS = {
    "grab": "Grab",
    "smooth": "Smooth",
    "comb": "Comb",
    "lengthen": "Lengthen",
    "twist": "Twist",
}

DEFAULT_BRUSH = "grab"
DEFAULT_RADIUS_PX = 24.0
DEFAULT_PRESERVE_LENGTH = True


def validateBrush(brush):
    """The brush id; raises ValueError when unknown."""
    if brush not in BRUSHES:
        raise ValueError("validateBrush: want one of %r, got %r"
                         % (BRUSHES, brush))
    return brush


def brushLabel(brush):
    """The display label for `brush`."""
    return BRUSH_LABELS[validateBrush(brush)]


def brushFalloff(distPx, radiusPx, t=None, center=0.5, tRadius=0.0):
    """Sculpt-brush falloff (mirrors tonicMath.brushFalloff exactly).

    Screen-disc weight (1 at the brush center, smooth to 0 at the rim)
    times the t-range weight, so falloff applies both by screen radius
    and by `t` (plan/17 section 5.5). Kept local so file-path loads stay
    sibling-free; proven equal to the tonicMath kernel in the T1 test.
    """
    d = np.asarray(distPx, dtype=np.float64)
    r = float(radiusPx)
    if r > 0.0:
        s = np.clip(1.0 - d / r, 0.0, 1.0)
        spatial = s * s * (3.0 - 2.0 * s)
    else:
        spatial = np.where(d == 0.0, 1.0, 0.0)
    if t is None:
        return spatial
    ta = np.asarray(t, dtype=np.float64)
    tRadius = float(tRadius)
    if not tRadius > 0.0:
        tPart = np.where(ta == float(center), 1.0, 0.0)
    else:
        w = np.clip(1.0 - np.abs(ta - float(center)) / tRadius, 0.0, 1.0)
        tPart = w * w * (3.0 - 2.0 * w)
    return spatial * tPart


def _asColumn(points, what):
    pts = np.asarray(points, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 3 or len(pts) < 2:
        raise ValueError("%s: want an N x 3 array with N >= 2, got %r"
                         % (what, pts.shape))
    return pts


def _asWeights(weights, count, what):
    w = np.asarray(weights, dtype=np.float64).reshape(count)
    if np.any(~np.isfinite(w)):
        raise ValueError("%s: weights must be finite" % what)
    return np.clip(w, 0.0, 1.0)


def _arcLength(pts):
    return float(np.linalg.norm(np.diff(pts, axis=0), axis=1).sum())


def _preserveLength(orig, deformed):
    """Rescale `deformed` root-pinned to `orig`'s arc length (the K6 rule)."""
    total = _arcLength(orig)
    now = _arcLength(deformed)
    if not total > 1e-12 or not now > 1e-12:
        return deformed.copy()
    return deformed[:1] + (deformed - deformed[:1]) * (total / now)


def mirrorXPoints(points):
    """Mirror an (N, 3) column across the x = 0 symmetry plane."""
    pts = np.asarray(points, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 3:
        raise ValueError("mirrorXPoints: want an N x 3 array, got %r"
                         % (pts.shape,))
    out = pts.copy()
    out[:, 0] = -out[:, 0]
    return out


def mirrorXDelta(delta):
    """Mirror a stroke delta across x = 0 (symmetric sculpt)."""
    d = np.asarray(delta, dtype=np.float64).reshape(3)
    return np.array([-d[0], d[1], d[2]])


def grabPreview(points, weights, delta, preserveLength=True):
    """Preview the grab brush: CVs follow `delta` scaled by `weights`.

    Length-preserving by default (re-enforced root-pinned after the move,
    the K6 rule); pass preserveLength=False for a raw offset preview.
    """
    pts = _asColumn(points, "grabPreview")
    w = _asWeights(weights, len(pts), "grabPreview")
    d = np.asarray(delta, dtype=np.float64).reshape(3)
    out = pts + d * w[:, None]
    return _preserveLength(pts, out) if preserveLength else out


def smoothPreview(points, weights, strength=1.0, preserveLength=True):
    """Preview the smooth brush: one Laplacian pass blended by `weights`.

    The raw pass pins both ends (mirroring tonicTube.relaxPreview with
    per-CV strength). With preserveLength (the default) the root stays
    pinned and the tip re-seats to recover the arc length, since pinning
    both ends and rescaling cannot both hold.
    """
    pts = _asColumn(points, "smoothPreview")
    w = _asWeights(weights, len(pts), "smoothPreview")
    strength = float(strength)
    if not 0.0 <= strength <= 1.0:
        raise ValueError("smoothPreview: want strength in [0, 1], got %r"
                         % (strength,))
    avg = 0.5 * (pts[:-2] + pts[2:])
    out = pts.copy()
    out[1:-1] += (avg - out[1:-1]) * (w[1:-1] * strength)[:, None]
    return _preserveLength(pts, out) if preserveLength else out


def combPreview(points, weights, direction, preserveLength=True):
    """Preview the comb brush: CVs pushed along `direction` by `weights`.

    A zero direction is a no-op preview (never NaN).
    """
    pts = _asColumn(points, "combPreview")
    w = _asWeights(weights, len(pts), "combPreview")
    d = np.asarray(direction, dtype=np.float64).reshape(3)
    norm = float(np.linalg.norm(d))
    if not norm > 1e-12:
        return pts.copy()
    out = pts + (d / norm) * w[:, None]
    return _preserveLength(pts, out) if preserveLength else out


def lengthenPreview(points, newLength):
    """Preview the lengthen brush: root pinned, rescaled to `newLength`.

    Lengthen is the explicit exception to the length-preserving default:
    it sets length rather than keeping it. Shorten via newLength below
    the current length. Raises ValueError on a non-positive target.
    """
    pts = _asColumn(points, "lengthenPreview")
    target = float(newLength)
    if not target > 0.0:
        raise ValueError("lengthenPreview: want newLength > 0, got %r"
                         % (newLength,))
    total = _arcLength(pts)
    if not total > 1e-12:
        return pts.copy()
    return pts[:1] + (pts - pts[:1]) * (target / total)


def twistPreview(points, weights, angle, preserveLength=True):
    """Preview the twist brush: offsets rotated about the chord axis.

    Each CV's offset from the root rotates about the root-to-tip axis by
    `angle` (radians) scaled by its weight. A degenerate chord is a no-op
    preview. Length-preserving by default.
    """
    pts = _asColumn(points, "twistPreview")
    w = _asWeights(weights, len(pts), "twistPreview")
    axis = pts[-1] - pts[0]
    norm = float(np.linalg.norm(axis))
    if not norm > 1e-12:
        return pts.copy()
    axis = axis / norm
    # Rodrigues' rotation of each offset about the chord axis.
    off = pts - pts[0]
    parallel = np.outer(off.dot(axis), axis)
    perp = off - parallel
    cross = np.cross(axis, perp)
    theta = float(angle) * w
    ct = np.cos(theta)[:, None]
    st = np.sin(theta)[:, None]
    out = pts[0] + parallel + perp * ct + cross * st
    out[0] = pts[0]
    return _preserveLength(pts, out) if preserveLength else out


def sculptStatus(brush, radiusPx, tRadius, preserveLength, mirrorX,
                 target="centers"):
    """The one-line Sculpt-mode HUD status."""
    brush = validateBrush(brush)
    if target not in ("centers", "guides"):
        raise ValueError("sculptStatus: want target centers|guides, got %r"
                         % (target,))
    flags = []
    flags.append("length-preserving" if preserveLength else "raw length")
    if mirrorX:
        flags.append("mirror-X")
    return ("Sculpt %s over %s (r=%.0f px, t=%.2f; %s)."
            % (BRUSH_LABELS[brush], target, float(radiusPx), float(tRadius),
               ", ".join(flags)))


def propagationPlan(lockParents, lockChildren):
    """Which hierarchy passes a sculpt stroke drives (plan/17 section 2.4).

    Returns {"topDown": ..., "bottomUp": ...}: a parent edit re-derives
    descendants top-down (K6) unless lockChildren; a child edit refreshes
    ancestors bottom-up (K7) unless lockParents. Both default off, so both
    passes run.
    """
    return {"topDown": not bool(lockChildren),
            "bottomUp": not bool(lockParents)}


# The Sculpt C ABI the stroke wrapper needs (name, quoted C signature,
# kernels). Same rule as tonicHierarchy.REQUIRED_C_API: no C++ is touched
# by P4-python; tonicLib binds this when the C++ lands.
REQUIRED_C_API = (
    ("Tonic_SculptStroke",
     "int Tonic_SculptStroke(void *model, int tubeId, const char *brush, "
     "const int *cvIds, const float *deltas, int cvCount, "
     "int preserveLength, int mirrorX)",
     "K6/K7"),
)


def cApiQuote(name):
    """The quoted C signature for `name`, or "" when unknown."""
    for entry, signature, _ in REQUIRED_C_API:
        if entry == name:
            return signature
    return ""


def requireEntry(dll, name):
    """The C entry `name` on `dll`, or NotImplementedError quoting it."""
    try:
        entry = getattr(dll, name, None)
    except AttributeError:
        entry = None
    if entry is None:
        raise NotImplementedError(
            "P4 C ABI: missing %s -- %s" % (name, cApiQuote(name)))
    return entry


def missingEntries(dll):
    """The REQUIRED_C_API names `dll` does not export yet."""
    missing = []
    for name, _, _ in REQUIRED_C_API:
        try:
            found = getattr(dll, name, None) is not None
        except AttributeError:
            found = False
        if not found:
            missing.append(name)
    return missing


def stroke(dll, model, tubeId, brush, cvIds, deltas, preserveLength=True,
           mirrorX=False):
    """Apply one sculpt stroke over `tubeId`'s center CVs (K6/K7).

    `cvIds` selects the center CVs, `deltas` is their flattened (dx, dy,
    dz) offsets. Length-preserving and single-sided by default.
    """
    entry = requireEntry(dll, "Tonic_SculptStroke")
    brush = validateBrush(brush)
    ids = [int(v) for v in cvIds]
    flat = [float(v) for v in deltas]
    if len(flat) != 3 * len(ids):
        raise ValueError("stroke: want 3 deltas per cv id, got %d ids and "
                         "%d floats" % (len(ids), len(flat)))
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p,
                      ctypes.POINTER(ctypes.c_int),
                      ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                      ctypes.c_int, ctypes.c_int]
    entry.restype = ctypes.c_int
    arr = (ctypes.c_int * len(ids))(*ids) if ids else None
    vec = (ctypes.c_float * len(flat))(*flat) if flat else None
    rc = int(entry(model, int(tubeId), brush.encode("ascii"), arr, vec,
                   len(ids), 1 if preserveLength else 0,
                   1 if mirrorX else 0))
    if rc != 0:
        raise RuntimeError("Tonic_SculptStroke failed with code %d" % rc)
    return rc
