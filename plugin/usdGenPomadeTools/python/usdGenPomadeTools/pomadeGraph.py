# usdGenPomadeTools.pomadeGraph -- Qt-free Graph-mode kernels (plan/17 P2).
#
# Pure-numpy helpers plus the status/HUD text builders behind Graph mode.
# Everything here imports headlessly (no pxr, no Qt); the C++ model owns the
# graph itself, and pomadeLoops.GraphLoop owns the gesture. Covered by
# testUsdGenPomadeToolsGraph.py from day one.
from __future__ import annotations

import numpy as np

# The live-map value of an uncovered face (primvar + bake channel 0 agree).
REGION_UNCOVERED = -1

# HUD overlay colours (the scene index tints the same way; keep in sync with
# pomadeSceneIndex.cpp).
COLOR_UNCOVERED = (0.35, 0.05, 0.05)
COLOR_INTERSECTED = (1.0, 0.0, 1.0)
COLOR_UNWELDED_NODE = (1.0, 0.5, 0.1)


def douglasPeucker(points, eps):
    """Simplify a polyline (N x 3 array) to tolerance `eps` (rest units).

    Returns the kept row indices, always including the first and last rows.
    This previews what Pomade_GraphStroke does in C++; the committed chain
    comes from the C API, never from this function.
    """
    pts = np.asarray(points, dtype=np.float64)
    if pts.ndim != 2 or pts.shape[1] != 3:
        raise ValueError("douglasPeucker: want an N x 3 array, got %r"
                         % (pts.shape,))
    keep = np.zeros(len(pts), dtype=bool)
    if len(pts) == 0:
        return keep
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        first, last = stack.pop()
        if last <= first + 1:
            continue
        a = pts[first]
        b = pts[last]
        ab = b - a
        denom = float(np.dot(ab, ab))
        seg = pts[first + 1:last] - a
        if denom > 0.0:
            t = np.clip(seg.dot(ab) / denom, 0.0, 1.0)
            dist = np.linalg.norm(seg - t[:, None] * ab, axis=1)
        else:
            dist = np.linalg.norm(seg, axis=1)
        if dist.size == 0:
            continue
        split = int(np.argmax(dist)) + first + 1
        if float(dist.max()) > float(eps):
            keep[split] = True
            stack.append((first, split))
            stack.append((split, last))
    return keep


def pixelsToRest(pixels, distance, viewportHeightPx, fovYDeg):
    """Convert a screen snap radius to rest units at `distance` from camera.

    `pixels` is the snap radius in physical pixels (default 8, plan/17
    section 5.1), `fovYDeg` the vertical field of view in degrees. Returns
    0.0 for degenerate input rather than dividing by zero.
    """
    try:
        pixels = float(pixels)
        distance = float(distance)
        height = float(viewportHeightPx)
        fov = float(fovYDeg)
    except (TypeError, ValueError):
        return 0.0
    if not (height > 0.0 and fov > 0.0 and fov < 180.0 and distance > 0.0):
        return 0.0
    worldHeight = 2.0 * distance * np.tan(np.radians(fov) * 0.5)
    return max(pixels * worldHeight / height, 0.0)


def snapRadiusRest(session, state, camera=None, point=None):
    """The Graph snap radius in rest (world) units.

    The panel's number (`state.snapRadiusPx`) is screen pixels; the ABI's
    radius is rest units. The camera converts one into the other with a
    measured world-per-pixel at `point` (where the artist is pointing),
    else at the scalp centre, so the same 8 px means the same thing zoomed
    in and zoomed out. With no camera the session's recorded display scale
    (world units per pixel at the scalp, plan/18 section 2.4a) stands in;
    with neither, the model's own world radius. Pixels are never handed to
    the ABI as rest units.
    """
    px = float(getattr(state, "snapRadiusPx", 0.0) or 0.0)
    perPixel = 0.0
    if camera is not None:
        where = point if point is not None else getattr(
            session, "scalpCenter", None)
        if where is not None:
            perPixel = float(camera.worldPerPixel(where) or 0.0)
    if not perPixel > 0.0:
        recorded = getattr(session, "displayScale", None)
        if callable(recorded):
            try:
                perPixel = float(recorded())
            except (TypeError, ValueError):
                perPixel = 0.0
    if perPixel > 0.0 and px > 0.0:
        return max(perPixel * px, 1e-6)
    dll = getattr(session, "dll", None)
    model = getattr(session, "model", None)
    if model is None:
        model = getattr(session, "ctx", None)
    if dll is None or model is None:
        return 0.0
    return float(dll.Pomade_GetSnapRadius(model))


def hudStatus(counts, stats, mapVersion, bakedVersion, stagedVersion=None,
              fallbackReason=""):
    """One-line Graph-mode HUD status.

    `counts` is (nodes, edges, regions), `stats` is (regions, uncovered,
    intersected). The map/stage skew shows in amber in the UI; here it is a
    "(map v<m> behind v<n>)" suffix, empty when they match. A non-empty
    `fallbackReason` (from Pomade_GetDeviceFallbackReason) appends the
    P6 CPU-only banner.
    """
    nodes, edges, regions = (int(v) for v in counts)
    _, uncovered, intersected = (int(v) for v in stats)
    line = ("Graph: %d nodes %d edges %d regions | uncovered %d "
            "intersected %d" % (nodes, edges, regions, uncovered,
                                intersected))
    if stagedVersion is not None and int(bakedVersion) < int(stagedVersion):
        line += " (map v%d behind v%d)" % (int(bakedVersion),
                                           int(stagedVersion))
    elif mapVersion is not None and bakedVersion is not None \
            and int(bakedVersion) < int(mapVersion):
        line += " (map v%d baking v%d)" % (int(bakedVersion), int(mapVersion))
    if fallbackReason:
        line += " (CPU-only: %s)" % fallbackReason
    return line


def regionPalette(regionId):
    """Deterministic region colour, mirroring PomadeRegionColor (G1)."""
    import math
    h = math.fmod(float(regionId) * 0.61803398875, 1.0)
    s, v = 0.65, 0.95
    c = v * s
    hh = h * 6.0
    x = c * (1.0 - abs(math.fmod(hh, 2.0) - 1.0))
    sector = int(hh) % 6
    r, g, b = 0.0, 0.0, 0.0
    if sector == 0:
        r, g = c, x
    elif sector == 1:
        r, g = x, c
    elif sector == 2:
        g, b = c, x
    elif sector == 3:
        g, b = x, c
    elif sector == 4:
        r, b = x, c
    else:
        r, b = c, x
    m = v - c
    return (r + m, g + m, b + m)
