# usdGenPomadeTools.pomadeFill -- Qt-free Fill-mode helpers (plan/17 P3).
#
# Pure-numpy previews plus the status/HUD text builders behind Fill mode.
# Everything here imports headlessly (no pxr, no Qt); the C++ model owns
# the roots and guides. Covered by testUsdGenPomadeToolsTube.py from day one.
#
# The preview math mirrors the C++ kernels exactly (same formulas as
# pomadeTube.h); the committed values always come from the C API, never
# from these functions.
from __future__ import annotations

import numpy as np

# The live drag density (plan/17 section 5.3): preview at 25%, full on
# release. Mirrors PomadeModel's default preview fraction.
DEFAULT_PREVIEW_FRACTION = 0.25


def previewCount(full, fraction):
    """Guides drawn at `fraction` of the full `full` count.

    Mirrors the C++ rule int(full * fraction + 0.5) exactly, including the
    round-half-up on the positive range.
    """
    full = int(full)
    fraction = float(fraction)
    if full < 0:
        raise ValueError("previewCount: want full >= 0, got %r" % (full,))
    fraction = min(max(fraction, 0.0), 1.0)
    return int(full * fraction + 0.5)


def frozenPrefix(existing, target):
    """Roots kept when refilling to `target` with freeze-roots on.

    Mirrors the C++ rule: the kept prefix is min(existing, target); the
    tail is re-sampled.
    """
    return min(max(int(existing), 0), max(int(target), 0))


def validateLengthProfile(pairs):
    """Check flattened (position, value) ramp pairs.

    Returns the (P, 2) float64 array. Raises ValueError on an odd float
    count (the C++ rule); an empty list means uniform length.
    """
    flat = [float(v) for v in pairs]
    if len(flat) % 2 != 0:
        raise ValueError("validateLengthProfile: want (position, value) "
                         "pairs, got %d floats" % len(flat))
    if not flat:
        return np.zeros((0, 2))
    return np.asarray(flat, dtype=np.float64).reshape(-1, 2)


def evalLengthProfile(pairs, u):
    """Evaluate the length ramp at radial fraction `u` in [0, 1].

    Mirrors PomadeEvalLengthProfile: piecewise-linear with clamped ends,
    empty pairs evaluate to 1.
    """
    ramp = validateLengthProfile(pairs)
    u = float(u)
    if len(ramp) == 0:
        return 1.0
    if len(ramp) == 1 or u <= ramp[0, 0]:
        return float(ramp[0, 1])
    for i in range(1, len(ramp)):
        if u <= ramp[i, 0]:
            p0, v0 = ramp[i - 1]
            p1, v1 = ramp[i]
            if not p1 > p0:
                return float(v1)
            f = (u - p0) / (p1 - p0)
            return float(v0 + (v1 - v0) * f)
    return float(ramp[-1, 1])


def edgeBiasRemap(r, bias):
    """Remap a root radius `r` in [0, 1] by `bias` in [-1, 1].

    Mirrors the K9 rule r' = r^(1 - bias/2): +1 pushes roots toward the
    tube wall, -1 pulls them to the center.
    """
    r = min(max(float(r), 0.0), 1.0)
    bias = float(bias)
    if not -1.0 <= bias <= 1.0:
        raise ValueError("edgeBiasRemap: want bias in [-1, 1], got %r"
                         % (bias,))
    if r == 0.0:
        return 0.0
    return float(r ** (1.0 - 0.5 * bias))


def fillStatus(density, cvCount, edgeBias, seed, profilePairs, frozen):
    """The one-line Fill-mode HUD status."""
    ramp = ("uniform" if len(list(profilePairs)) == 0
            else "%d knots" % (len(list(profilePairs)) // 2))
    freeze = ", frozen" if frozen else ""
    return ("Fill: density %.1f, %d CVs, edge bias %+.2f, seed %d, "
            "length %s%s."
            % (float(density), int(cvCount), float(edgeBias), int(seed),
               ramp, freeze))


def refillPlan(full, fraction, existing, frozen):
    """Plan one refill: (target, kept) guide counts.

    `full` is the full-density count, `fraction` the refill fraction,
    `existing` the stored root count, `frozen` the freeze-roots flag.
    `target` is the post-refill count, `kept` the frozen prefix.
    """
    target = previewCount(full, fraction)
    kept = frozenPrefix(existing, target) if frozen else 0
    return target, kept
