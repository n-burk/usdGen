# usdGenPomadeTools.pomadeMath -- Qt-free kernels (plan/17 P0 seed).
#
# Pure-numpy helpers over plain arrays. P0 seeds the two weight functions the
# P4/P5 gizmos need (soft selection along the center, brush falloff); they are
# small, documented, and covered by testUsdGenPomadeToolsMath.py from day one
# so later modes inherit tested kernels instead of inventing them.
from __future__ import annotations

import numpy as np


def smoothstep(edge0, edge1, x):
    """Hermite smoothstep of `x` between `edge0` and `edge1`.

    Returns 0 at/below edge0, 1 at/above edge1, smooth in between. Accepts
    scalars or numpy arrays; never divides by zero (a degenerate edge pair
    returns the upper step).
    """
    edge0 = float(edge0)
    edge1 = float(edge1)
    xa = np.asarray(x, dtype=np.float64)
    if not edge1 > edge0:
        return np.where(xa < edge1, 0.0, 1.0)
    t = np.clip((xa - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def softSelection(t, center, radius):
    """Soft-selection weight along the center parameter `t` (0..1).

    1 at `center`, falling smoothly to 0 at |t - center| == radius, 0 beyond.
    A non-positive radius selects only the exact center.
    """
    ta = np.asarray(t, dtype=np.float64)
    radius = float(radius)
    if not radius > 0.0:
        return np.where(ta == float(center), 1.0, 0.0)
    d = np.abs(ta - float(center)) / radius
    w = np.clip(1.0 - d, 0.0, 1.0)
    return w * w * (3.0 - 2.0 * w)


def brushFalloff(distPx, radiusPx, t=None, center=0.5, tRadius=0.0):
    """Sculpt-brush falloff: a screen disc times a t-range (plan/17 S5.5).

    `distPx` is the screen distance of each candidate CV from the brush
    center, `radiusPx` the brush radius. The spatial weight is 1 at the
    center and falls smoothly to 0 at the rim (0 beyond); a non-positive
    radius selects only the exact center. When `t` (per-CV curve
    parameters) is given, the spatial weight is multiplied by
    softSelection(t, center, tRadius), so the brush is bounded both on
    screen and along the curve. Accepts scalars or numpy arrays.
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
    return spatial * softSelection(t, center, tRadius)


def polylineLength(points):
    """Arc length of an (N, 3) polyline; 0.0 when degenerate."""
    pts = np.asarray(points, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 3:
        raise ValueError("polylineLength: want an N x 3 array, got %r"
                         % (pts.shape,))
    if len(pts) < 2:
        return 0.0
    return float(np.linalg.norm(np.diff(pts, axis=0), axis=1).sum())


def resamplePolyline(points, count):
    """Arc-length resample of an (N, 3) polyline to `count` rows.

    Ends are pinned; a zero-length column resamples to its first point
    repeated (never NaN). Mirrors the K7 resample-then-average rule.
    """
    pts = np.asarray(points, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 3 or len(pts) < 2:
        raise ValueError("resamplePolyline: want an N x 3 array with N >= 2, "
                         "got %r" % (pts.shape,))
    count = int(count)
    if count < 2:
        raise ValueError("resamplePolyline: want count >= 2, got %r"
                         % (count,))
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    total = float(seg.sum())
    if not total > 1e-12:
        return np.repeat(pts[:1], count, axis=0)
    s = np.concatenate([[0.0], np.cumsum(seg)]) / total
    keep = [0]
    for k in range(1, len(s)):
        if s[k] > s[keep[-1]]:
            keep.append(k)
    sk = s[keep]
    pk = pts[keep]
    targets = np.linspace(0.0, 1.0, count)
    return np.column_stack([np.interp(targets, sk, pk[:, c])
                            for c in range(3)])


def averagePolylines(curves, count):
    """K7 preview: resample each (N, 3) curve to `count` rows and average.

    Idempotent: averaging one curve reproduces its resample, and averaging
    the average with its inputs is a fixed point. The committed parent
    comes from the C API, never from this function.
    """
    seq = list(curves)
    if not seq:
        raise ValueError("averagePolylines: want at least one curve")
    acc = None
    for curve in seq:
        grid = resamplePolyline(curve, count)
        acc = grid if acc is None else acc + grid
    return acc / float(len(seq))


def enforceLength(points, targetLength):
    """Rescale an (N, 3) polyline to `targetLength`, root pinned.

    Mirrors the K6 length-preserving rule (child offsets scaled in the
    parent's frames). A degenerate column is returned unchanged; raises
    ValueError on a non-positive target.
    """
    pts = np.asarray(points, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 3 or len(pts) < 2:
        raise ValueError("enforceLength: want an N x 3 array with N >= 2, "
                         "got %r" % (pts.shape,))
    target = float(targetLength)
    if not target > 0.0:
        raise ValueError("enforceLength: want targetLength > 0, got %r"
                         % (targetLength,))
    total = polylineLength(pts)
    if not total > 1e-12:
        return pts.copy()
    return pts[:1] + (pts - pts[:1]) * (target / total)


def lerpRing(a, b, f):
    """Interpolate two section rings (N x 2 arrays) by `f` in [0, 1]."""
    aa = np.asarray(a, dtype=np.float64)
    ba = np.asarray(b, dtype=np.float64)
    if aa.shape != ba.shape:
        raise ValueError("lerpRing: ring shapes differ %r vs %r"
                         % (aa.shape, ba.shape))
    f = float(np.clip(float(f), 0.0, 1.0))
    return (1.0 - f) * aa + f * ba
