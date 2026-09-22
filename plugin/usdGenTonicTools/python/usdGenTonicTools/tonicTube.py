# usdGenTonicTools.tonicTube -- Qt-free Tube-mode helpers (plan/17 P3).
#
# Pure-numpy previews plus the status/HUD text builders behind Tube mode.
# Everything here imports headlessly (no pxr, no Qt); the C++ model owns
# the tube itself, and the gizmo UI owns the event filter. Covered by
# testUsdGenTonicToolsTube.py from day one.
#
# The preview math mirrors the C++ kernels exactly (same formulas as
# tonicTube.h); the committed values always come from the C API, never
# from these functions.
from __future__ import annotations

import numpy as np

# Pick kinds, mirroring TonicPickKind in tonicTube.h (a kind mask is an OR).
PICK_TUBE_VERT = 1 << 0
PICK_CENTER_CV = 1 << 1
PICK_SECTION_CV = 1 << 2
PICK_GRAPH_NODE = 1 << 3
PICK_GUIDE = 1 << 4
PICK_GRAPH_EDGE = 1 << 5
PICK_REGION = 1 << 6
PICK_SECTION_RING = 1 << 7
# A hierarchy level is a selection kind, not a screen candidate: it is
# chosen from the breadcrumb and selects every tube at that level, so
# PICK_ALL (what a press asks for) stops at the eight pickable kinds.
PICK_LEVEL = 1 << 8
PICK_ALL = 0xFF
SELECT_ALL = 0x1FF

PICK_KIND_NAMES = {
    PICK_TUBE_VERT: "tube vertex",
    PICK_CENTER_CV: "center CV",
    PICK_SECTION_CV: "section CV",
    PICK_GRAPH_NODE: "graph node",
    PICK_GUIDE: "guide",
    PICK_GRAPH_EDGE: "graph edge",
    PICK_REGION: "region",
    PICK_SECTION_RING: "section ring",
    PICK_LEVEL: "hierarchy level",
}

# The default test-tube shape (mirrors tonicTessellate.h).
DEFAULT_RINGS = 5
DEFAULT_RING_VERTS = 8
# Region-authored roots use the native auto sentinel: the backend resolves it
# to the region loop's CV count, preserving the artist's contour corners.
# Keep DEFAULT_RING_VERTS for generic test-tube math and legacy callers.
DEFAULT_REGION_RING_VERTS = 0
MIN_REGION_RING_VERTS = 3
MAX_REGION_RING_VERTS = 32
REGION_RING_VERT_CHOICES = ((DEFAULT_REGION_RING_VERTS,) +
                            tuple(range(MIN_REGION_RING_VERTS,
                                        MAX_REGION_RING_VERTS + 1)))
DEFAULT_RADIUS = 0.5
DEFAULT_LENGTH = 4.0


def regionRingVerts(value):
    """Sanitise a Region-root ring request for Tonic_BuildTubeFromRegion.

    Zero is the native Auto value (match the authored region CVs). Explicit
    requests are constrained to the build ABI's useful 3..32 range, so a
    persisted or scripted 1/2 never reaches the backend as a degenerate
    ring.
    """
    try:
        requested = int(value)
    except (TypeError, ValueError):
        return DEFAULT_REGION_RING_VERTS
    if requested <= DEFAULT_REGION_RING_VERTS:
        return DEFAULT_REGION_RING_VERTS
    return min(max(requested, MIN_REGION_RING_VERTS),
               MAX_REGION_RING_VERTS)


def softWeights(count, cv, radius):
    """Soft-selection weights over `count` center CVs for a drag of `cv`.

    Mirrors TonicSoftWeight: 1 at the dragged CV, smoothstep to 0 at
    |t - center| == radius. A radius <= 0 selects the dragged CV exactly.
    Returns a (count,) float64 array.
    """
    count = int(count)
    if count <= 0:
        raise ValueError("softWeights: want count >= 1, got %r" % (count,))
    if not 0 <= int(cv) < count:
        raise ValueError("softWeights: cv %r out of range(%d)"
                         % (cv, count))
    center = float(cv) / float(count - 1) if count > 1 else 0.0
    t = np.linspace(0.0, 1.0, count)
    radius = float(radius)
    if not radius > 0.0:
        w = np.zeros(count)
        w[int(cv)] = 1.0
        return w
    w = 1.0 - np.abs((t - center) / radius)
    w = np.clip(w, 0.0, 1.0)
    return w * w * (3.0 - 2.0 * w)


def transformRingPreview(ring, scale=1.0, twist=0.0, offset=(0.0, 0.0)):
    """Preview the ring gizmo: scale, twist (radians) and plane offset.

    `ring` is an (N, 2) array of section-plane CVs. The transform order
    matches K5 placement: scale, then twist, then offset. Returns the (N, 2)
    preview; the committed ring comes from the C API.
    """
    pts = np.asarray(ring, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 2:
        raise ValueError("transformRingPreview: want an N x 2 array, got %r"
                         % (pts.shape,))
    if not float(scale) > 0.0:
        raise ValueError("transformRingPreview: want scale > 0, got %r"
                         % (scale,))
    pts = pts * float(scale)
    tw = float(twist)
    if tw != 0.0:
        ct, st = float(np.cos(tw)), float(np.sin(tw))
        uu = pts[:, 0] * ct - pts[:, 1] * st
        vv = pts[:, 0] * st + pts[:, 1] * ct
        pts = np.column_stack([uu, vv])
    off = np.asarray(offset, dtype=np.float64).reshape(2)
    return pts + off


def relaxPreview(centers, strength=1.0):
    """One Laplacian relax pass over a (N, 3) center column (ends pinned).

    Mirrors TonicModel::RelaxCenter with iterations=1. Returns the relaxed
    (N, 3) array.
    """
    pts = np.asarray(centers, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 3 or len(pts) < 3:
        raise ValueError("relaxPreview: want an N x 3 array with N >= 3, "
                         "got %r" % (pts.shape,))
    strength = float(strength)
    if not 0.0 <= strength <= 1.0:
        raise ValueError("relaxPreview: want strength in [0, 1], got %r"
                         % (strength,))
    out = pts.copy()
    avg = 0.5 * (pts[:-2] + pts[2:])
    out[1:-1] += (avg - out[1:-1]) * strength
    return out


def lengthScale(centers, newLength):
    """The uniform scale taking a center column to `newLength` arc length.

    Mirrors TonicModel::SetTubeLength (root pinned, offsets scaled).
    Returns 0.0 for a degenerate column or length rather than dividing.
    """
    pts = np.asarray(centers, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 3 or len(pts) < 2:
        return 0.0
    try:
        newLength = float(newLength)
    except (TypeError, ValueError):
        return 0.0
    if not newLength > 0.0:
        return 0.0
    total = float(np.linalg.norm(np.diff(pts, axis=0), axis=1).sum())
    if not total > 1e-12:
        return 0.0
    return newLength / total


def sectionInsertSpan(sectionT, t):
    """The bracketing span (k, f) for inserting a ring at `t`.

    `sectionT` is the ascending section parameter list. Returns (k, f) with
    the new ring between sections k and k+1 at fraction f. Raises
    ValueError when `t` is not strictly inside the range (the C++ rule).
    """
    ts = [float(v) for v in sectionT]
    t = float(t)
    if len(ts) < 2:
        raise ValueError("sectionInsertSpan: want >= 2 sections")
    if not (t > ts[0] and t < ts[-1]):
        raise ValueError("sectionInsertSpan: t=%r must sit strictly inside "
                         "(%r, %r)" % (t, ts[0], ts[-1]))
    k = 0
    while k + 1 < len(ts) - 1 and ts[k + 1] < t:
        k += 1
    dt = ts[k + 1] - ts[k]
    f = (t - ts[k]) / dt if dt > 0.0 else 0.0
    return k, min(max(f, 0.0), 1.0)


def tubeStatus(centerCount, sectionCount, ringVerts, regionId):
    """The one-line Tube-mode HUD status."""
    root = ("unrooted (disc fill)" if int(regionId) < 0
            else "rooted in region %d" % int(regionId))
    return ("Tube: %d center CVs, %d sections x %d CVs, %s."
            % (int(centerCount), int(sectionCount), int(ringVerts), root))


def tubeEditHint(subMode):
    """One concise, mode-specific affordance for the Tube status/panel."""
    if str(subMode) == "ring":
        return "Scale selected sections: drag the outer ring handle."
    if str(subMode) == "section":
        return "Edit section CVs: select a CV, then drag in its ring plane."
    return ""


def pickStatus(kind, index, subIndex, distPx):
    """The one-line pick HUD status for a K11 hit."""
    name = PICK_KIND_NAMES.get(int(kind), "unknown")
    if int(kind) == PICK_SECTION_CV and int(subIndex) >= 0:
        where = "ring %d slot %d" % (int(index), int(subIndex))
    elif int(kind) == PICK_GUIDE and int(subIndex) >= 0:
        where = "guide %d cv %d" % (int(index), int(subIndex))
    else:
        where = "%d" % int(index)
    return "Pick: %s %s (%.1f px)." % (name, where, float(distPx))
