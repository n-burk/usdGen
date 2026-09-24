# usdGenTonicTools.tonicBridge -- Qt-free P5 bridge helpers (plan/17 §5.7).
#
# The Tonic <-> Houdini braid round trip without any Houdini-specific code:
# export tube centers as BasisCurves to a file layer, import swept meshes or
# curves back as L3 locked tubes under a chosen parent. Everything here is
# plain Python + ctypes: no pxr, Qt or numpy at import (the headless tests
# load this file by path, and the pxr authors only run inside usdview).
from __future__ import annotations

import ctypes
import heapq
import math


# -- pure helpers (no dll, no stage) --------------------------------------
#
# Points are (x, y, z) sequences of floats; faces are index lists. All of
# these run headlessly and are covered by testUsdGenTonicToolsBridge.py.

def validateRingVerts(ringVerts):
    """The ring CV count, or ValueError when it cannot form a ring."""
    ringVerts = int(ringVerts)
    if ringVerts < 3:
        raise ValueError(
            "tonicBridge: want at least 3 ring verts, got %d" % ringVerts)
    return ringVerts


def validateSectionParams(ts):
    """Ascending [0, 1] section parameters, or ValueError."""
    params = [float(t) for t in ts]
    if len(params) < 2:
        raise ValueError("tonicBridge: want at least 2 sections, got %d"
                         % len(params))
    for prev, cur in zip(params, params[1:]):
        if not 0.0 <= prev <= cur <= 1.0 or cur <= prev:
            raise ValueError(
                "tonicBridge: section params must ascend in [0, 1], got %r"
                % (params,))
    if params[0] != 0.0 or params[-1] != 1.0:
        raise ValueError(
            "tonicBridge: section params must span [0, 1], got %r"
            % (params,))
    return params


def polylineLength(points):
    """The polyline arc length (0.0 for fewer than two points)."""
    total = 0.0
    for a, b in zip(points, points[1:]):
        total += math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))
    return total


def resamplePolyline(points, count):
    """`count` arc-length-uniform samples of the polyline (pure Python).

    K4 resamples the same way on import, so the round trip compares
    resampled-to-resampled rather than CV-to-CV.
    """
    count = int(count)
    if count < 2:
        raise ValueError("tonicBridge: want at least 2 samples, got %d"
                         % count)
    pts = [(float(x), float(y), float(z)) for x, y, z in points]
    if len(pts) < 2:
        raise ValueError("tonicBridge: want at least 2 points, got %d"
                         % len(pts))
    total = polylineLength(pts)
    if total <= 0.0:
        return [pts[0]] * count
    step = total / (count - 1)
    out = [pts[0]]
    segStart = 0.0
    segIndex = 0
    for sample in range(1, count - 1):
        target = sample * step
        while (segIndex + 1 < len(pts) - 1
               and segStart + _segLen(pts[segIndex], pts[segIndex + 1])
               < target):
            segStart += _segLen(pts[segIndex], pts[segIndex + 1])
            segIndex += 1
        a, b = pts[segIndex], pts[segIndex + 1]
        seg = _segLen(a, b)
        frac = 0.0 if seg <= 0.0 else (target - segStart) / seg
        frac = min(1.0, max(0.0, frac))
        out.append(tuple(x + (y - x) * frac for x, y in zip(a, b)))
    out.append(pts[-1])
    return out


def _segLen(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def ringCentroid(ring):
    """The mean point of a swept-mesh ring (what K4 fits the center to)."""
    pts = [(float(x), float(y), float(z)) for x, y, z in ring]
    if not pts:
        raise ValueError("tonicBridge: cannot take the centroid of no points")
    n = float(len(pts))
    return (sum(p[0] for p in pts) / n,
            sum(p[1] for p in pts) / n,
            sum(p[2] for p in pts) / n)


def ringRadius(ring, centroid=None):
    """The mean ring-point distance to the centroid (the tube radius)."""
    center = centroid or ringCentroid(ring)
    pts = [(float(x), float(y), float(z)) for x, y, z in ring]
    if not pts:
        raise ValueError("tonicBridge: cannot take the radius of no points")
    return sum(_segLen(p, center) for p in pts) / len(pts)


def sweptRingsToCenters(rings):
    """Per-ring centroids: the center polyline a swept import carries."""
    if len(rings) < 2:
        raise ValueError("tonicBridge: want at least 2 rings, got %d"
                         % len(rings))
    return [ringCentroid(ring) for ring in rings]


def maxDeviation(pointsA, pointsB):
    """The max per-sample distance between two equal-length polylines."""
    if len(pointsA) != len(pointsB):
        raise ValueError("tonicBridge: cannot compare %d points against %d"
                         % (len(pointsA), len(pointsB)))
    if not pointsA:
        return 0.0
    return max(_segLen(a, b) for a, b in zip(pointsA, pointsB))


def roundTripError(exported, reimported, samples=64):
    """The resampled max deviation after an export/import round trip.

    Both sides are resampled to `samples` points first: export writes
    CVs, import refits them, so CV counts legitimately differ.
    """
    a = resamplePolyline(exported, samples)
    b = resamplePolyline(reimported, samples)
    return maxDeviation(a, b)


def pointToTriangleSquared(point, a, b, c):
    """Squared distance from `point` to triangle (a, b, c)."""
    px, py, pz = (float(v) for v in point)
    ax, ay, az = (float(v) for v in a)
    ab = (b[0] - ax, b[1] - ay, b[2] - az)
    ac = (c[0] - ax, c[1] - ay, c[2] - az)
    ap = (px - ax, py - ay, pz - az)
    d1 = ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2]
    d2 = ac[0] * ap[0] + ac[1] * ap[1] + ac[2] * ap[2]
    if d1 <= 0.0 and d2 <= 0.0:
        return ap[0] ** 2 + ap[1] ** 2 + ap[2] ** 2
    bp = (px - b[0], py - b[1], pz - b[2])
    d3 = ab[0] * bp[0] + ab[1] * bp[1] + ab[2] * bp[2]
    d4 = ac[0] * bp[0] + ac[1] * bp[1] + ac[2] * bp[2]
    if d3 >= 0.0 and d4 <= d3:
        return bp[0] ** 2 + bp[1] ** 2 + bp[2] ** 2
    vc = d1 * d4 - d3 * d2
    if vc <= 0.0 and d1 >= 0.0 and d3 <= 0.0:
        frac = d1 / (d1 - d3)
        q = (ab[0] * frac, ab[1] * frac, ab[2] * frac)
        d = (ap[0] - q[0], ap[1] - q[1], ap[2] - q[2])
        return d[0] ** 2 + d[1] ** 2 + d[2] ** 2
    cp = (px - c[0], py - c[1], pz - c[2])
    d5 = ab[0] * cp[0] + ab[1] * cp[1] + ab[2] * cp[2]
    d6 = ac[0] * cp[0] + ac[1] * cp[1] + ac[2] * cp[2]
    if d6 >= 0.0 and d5 <= d6:
        return cp[0] ** 2 + cp[1] ** 2 + cp[2] ** 2
    vb = d5 * d2 - d1 * d6
    if vb <= 0.0 and d2 >= 0.0 and d6 <= 0.0:
        frac = d2 / (d2 - d6)
        q = (ac[0] * frac, ac[1] * frac, ac[2] * frac)
        d = (ap[0] - q[0], ap[1] - q[1], ap[2] - q[2])
        return d[0] ** 2 + d[1] ** 2 + d[2] ** 2
    va = d3 * d6 - d5 * d4
    if va <= 0.0 and (d4 - d3) >= 0.0 and (d5 - d6) >= 0.0:
        d43 = d4 - d3
        d56 = d5 - d6
        frac = d43 / (d43 + d56)
        q = (b[0] + (c[0] - b[0]) * frac - ax,
             b[1] + (c[1] - b[1]) * frac - ay,
             b[2] + (c[2] - b[2]) * frac - az)
        d = (ap[0] - q[0], ap[1] - q[1], ap[2] - q[2])
        return d[0] ** 2 + d[1] ** 2 + d[2] ** 2
    denom = 1.0 / max(1e-30, va + vb + vc)
    v = vb * denom
    w = vc * denom
    q = (ab[0] * v + ac[0] * w, ab[1] * v + ac[1] * w, ab[2] * v + ac[2] * w)
    d = (ap[0] - q[0], ap[1] - q[1], ap[2] - q[2])
    return d[0] ** 2 + d[1] ** 2 + d[2] ** 2


def meshAdjacency(faceVertexIndices, faceVertexCounts):
    """Vertex -> set(vertex) adjacency over the mesh edges."""
    adjacency = {}
    offset = 0
    for count in faceVertexCounts:
        ring = [int(v) for v in
                faceVertexIndices[offset:offset + count]]
        offset += count
        for a, b in zip(ring, ring[1:] + ring[:1]):
            adjacency.setdefault(a, set()).add(b)
            adjacency.setdefault(b, set()).add(a)
    for key in list(adjacency):
        adjacency[key] = sorted(adjacency[key])
    return adjacency


def surfaceDistance(point, points, faces):
    """Distance from `point` to the mesh (faces index `points`).

    Polygonal faces fan-triangulate from vertex 0, so quads validate
    whole rather than half.
    """
    best = float("inf")
    for face in faces:
        for k in range(1, len(face) - 1):
            a = points[face[0]]
            b = points[face[k]]
            c = points[face[k + 1]]
            dist2 = pointToTriangleSquared(point, a, b, c)
            if dist2 < best:
                best = dist2
    return math.sqrt(best) if best < float("inf") else float("inf")


def geodesicPath(points, adjacency, root, tip, maxLength=float("inf")):
    """Dijkstra shortest path (vertex ids) from `root` to `tip`.

    The P5 geodesic check: an imported curve is surface-bound when every
    sample sits within tolerance of the scalp and the root-to-tip walk
    stays on the mesh. Returns [] when `tip` is unreachable.
    """
    root, tip = int(root), int(tip)
    dist = {root: 0.0}
    prev = {}
    settled = set()
    queue = [(0.0, root)]
    while queue:
        cost, vert = heapq.heappop(queue)
        if vert in settled:
            continue
        settled.add(vert)
        if vert == tip:
            break
        if cost > maxLength:
            continue
        for other in adjacency.get(vert, ()):
            step = _segLen(points[vert], points[other])
            found = cost + step
            if found < dist.get(other, float("inf")) and found <= maxLength:
                dist[other] = found
                prev[other] = vert
                heapq.heappush(queue, (found, other))
    if tip not in dist:
        return []
    path = [tip]
    while path[-1] != root:
        path.append(prev[path[-1]])
    path.reverse()
    return path


def validateCurveOnSurface(curve, points, faces, tolerance):
    """Max surface distance of `curve` samples; None entries are skipped.

    The geodesic half of the P5 exit: an import candidate validates when
    this stays under the caller-supplied tolerance (the plan's TN bar for
    the braid round trip is mesh-exact roots, tube-exact middles).
    """
    tolerance = float(tolerance)
    if tolerance < 0.0:
        raise ValueError("tonicBridge: tolerance must be >= 0, got %r"
                         % (tolerance,))
    worst = 0.0
    for sample in curve:
        if sample is None:
            continue
        worst = max(worst, surfaceDistance(sample, points, faces))
    return worst


def basisCurvesSpec(name, curves, widths=None, basis="catmullRom"):
    """Pure-data spec for one BasisCurves prim (the pxr author consumes it).

    `curves` is a list of point lists; `widths` is None (uniform 1.0) or
    one width per curve. Returns a dict of plain lists, so the headless
    test can check the export shape without a stage.
    """
    if basis not in ("catmullRom", "bezier", "bspline"):
        raise ValueError("tonicBridge: unknown basis %r" % (basis,))
    if not curves:
        raise ValueError("tonicBridge: want at least one curve")
    counts = []
    flat = []
    for curve in curves:
        pts = [(float(x), float(y), float(z)) for x, y, z in curve]
        if len(pts) < 2:
            raise ValueError("tonicBridge: want at least 2 points per curve")
        counts.append(len(pts))
        flat.extend(pts)
    if widths is None:
        widthList = [1.0] * len(curves)
    else:
        widthList = [float(w) for w in widths]
        if len(widthList) != len(curves):
            raise ValueError("tonicBridge: want one width per curve, got %d "
                             "for %d curves" % (len(widthList), len(curves)))
    return {"name": str(name), "basis": basis, "vertexCounts": counts,
            "points": flat, "widths": widthList}


def centerCurvesSpec(tubeCenters, radius=1.0):
    """The export-center-curves spec: one curve per tube id, in id order."""
    ids = sorted(tubeCenters)
    if not ids:
        raise ValueError("tonicBridge: nothing to export")
    curves = [tubeCenters[tubeId] for tubeId in ids]
    spec = basisCurvesSpec("TonicCenters", curves,
                           [radius] * len(curves))
    spec["tubeIds"] = ids
    return spec


# -- the bridge C ABI the wrappers below need ------------------------------
# (name, quoted C signature, kernel). Same requireEntry convention as the
# hierarchy half in tonicHierarchy: these entries shipped with the rest of
# the hierarchy ABI, so requireEntry's NotImplementedError only fires
# against a DLL built before P5 landed.

REQUIRED_C_API = (
    ("Tonic_ReadTubeIds",
     "int Tonic_ReadTubeIds(void *model, int *out, int outCap, "
     "int *outCount)",
     "hierarchy"),
    ("Tonic_GetTubeCenterCount",
     "int Tonic_GetTubeCenterCount(void *model, int tubeId)",
     "hierarchy"),
    ("Tonic_GetTubeCenterCV",
     "int Tonic_GetTubeCenterCV(void *model, int tubeId, int cv, "
     "float *out3)",
     "hierarchy"),
    ("Tonic_GetTubeCenterHandle",
     "int Tonic_GetTubeCenterHandle(void *model, int tubeId, int cv, "
     "float *out3)",
     "displayed center-CV handle"),
    ("Tonic_GetTubeSectionCount",
     "int Tonic_GetTubeSectionCount(void *model, int tubeId)",
     "bridge"),
    ("Tonic_GetTubeSection",
     "int Tonic_GetTubeSection(void *model, int tubeId, int ring, "
     "float *outT, float *outUV, int uvLen, int *outCount, "
     "float *outScale, float *outTwist)",
     "bridge"),
    ("Tonic_ImportLockedTube",
     "int Tonic_ImportLockedTube(void *model, int parentId, "
     "const float *cx, const float *cy, const float *cz, int nCv, "
     "const float *secT, const float *secU, const float *secV, int nSec, "
     "int ringVerts, int *outTubeId)",
     "K4"),
    ("Tonic_ImportSweptMesh",
     "int Tonic_ImportSweptMesh(void *model, int parentId, "
     "const float *points, int ringCount, int ringVerts, int *outTubeId)",
     "K4"),
    ("Tonic_IsTubeImported",
     "int Tonic_IsTubeImported(void *model, int tubeId)",
     "bridge"),
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
            "P5 C ABI: missing %s -- %s" % (name, cApiQuote(name)))
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


def _check(rc, name):
    if int(rc) != 0:
        raise RuntimeError("%s failed with code %d" % (name, int(rc)))
    return int(rc)


# -- C wrappers (dll, model) ------------------------------------------------

def readTubeIds(dll, model):
    """All tube ids in the model, ascending (two-call probe)."""
    entry = requireEntry(dll, "Tonic_ReadTubeIds")
    entry.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                      ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    count = ctypes.c_int(0)
    _check(entry(model, None, 0, ctypes.byref(count)), "Tonic_ReadTubeIds")
    out = (ctypes.c_int * max(1, count.value))()
    _check(entry(model, out, count.value, ctypes.byref(count)),
           "Tonic_ReadTubeIds")
    return [out[i] for i in range(count.value)]


def tubeCenterCount(dll, model, tubeId):
    """The center-CV count of `tubeId` (-1 when unknown)."""
    entry = requireEntry(dll, "Tonic_GetTubeCenterCount")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int]
    entry.restype = ctypes.c_int
    return int(entry(model, int(tubeId)))


def tubeCenterCV(dll, model, tubeId, cv):
    """Center CV `cv` of `tubeId` as an (x, y, z) tuple."""
    entry = requireEntry(dll, "Tonic_GetTubeCenterCV")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float)]
    entry.restype = ctypes.c_int
    out = (ctypes.c_float * 3)()
    _check(entry(model, int(tubeId), int(cv), out), "Tonic_GetTubeCenterCV")
    return (out[0], out[1], out[2])


def tubeCenterHandle(dll, model, tubeId, cv):
    """Displayed center-CV handle; tubeCenterCV remains authored/raw."""
    entry = requireEntry(dll, "Tonic_GetTubeCenterHandle")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float)]
    entry.restype = ctypes.c_int
    out = (ctypes.c_float * 3)()
    _check(entry(model, int(tubeId), int(cv), out),
           "Tonic_GetTubeCenterHandle")
    return (out[0], out[1], out[2])


def tubeCenterHandles(dll, model, tubeId):
    """Every displayed center-CV handle, root first."""
    count = tubeCenterCount(dll, model, tubeId)
    if count < 0:
        raise RuntimeError("Tonic_GetTubeCenterCount: unknown tube %d" %
                           tubeId)
    return [tubeCenterHandle(dll, model, tubeId, cv) for cv in range(count)]


def tubeCenters(dll, model, tubeId):
    """Every center CV of `tubeId`, root first."""
    count = tubeCenterCount(dll, model, tubeId)
    if count < 0:
        raise RuntimeError("Tonic_GetTubeCenterCount: unknown tube %d"
                           % tubeId)
    return [tubeCenterCV(dll, model, tubeId, cv) for cv in range(count)]


def tubeSectionCount(dll, model, tubeId):
    """The section-ring count of `tubeId` (-1 when unknown)."""
    entry = requireEntry(dll, "Tonic_GetTubeSectionCount")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int]
    entry.restype = ctypes.c_int
    return int(entry(model, int(tubeId)))


def tubeSection(dll, model, tubeId, ring):
    """Ring `ring` of `tubeId` as (t, [(u, v)], scale, twist)."""
    entry = requireEntry(dll, "Tonic_GetTubeSection")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float),
                      ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int),
                      ctypes.POINTER(ctypes.c_float),
                      ctypes.POINTER(ctypes.c_float)]
    entry.restype = ctypes.c_int
    count = ctypes.c_int(0)
    _check(entry(model, int(tubeId), int(ring), None, None, 0,
                 ctypes.byref(count), None, None),
           "Tonic_GetTubeSection")
    uv = (ctypes.c_float * max(2, count.value * 2))()
    t = ctypes.c_float(0.0)
    scale = ctypes.c_float(1.0)
    twist = ctypes.c_float(0.0)
    _check(entry(model, int(tubeId), int(ring), ctypes.byref(t), uv,
                 len(uv), ctypes.byref(count), ctypes.byref(scale),
                 ctypes.byref(twist)),
           "Tonic_GetTubeSection")
    return (t.value, [(uv[2 * i], uv[2 * i + 1])
                      for i in range(count.value)],
            scale.value, twist.value)


def isTubeImported(dll, model, tubeId):
    """True when `tubeId` is a bridge import (unknown tubes read False)."""
    entry = requireEntry(dll, "Tonic_IsTubeImported")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int]
    entry.restype = ctypes.c_int
    return bool(entry(model, int(tubeId)))


def importLockedTube(dll, model, parentId, centers, sections):
    """Import explicit centers + sections as a locked child; returns its id.

    `sections` is [(t, [(u, v)])]; ring sizes must agree. The C++ derives
    scale/twist (1/0) and locks both propagation gates (§5.7 L3 import).
    """
    entry = requireEntry(dll, "Tonic_ImportLockedTube")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float),
                      ctypes.POINTER(ctypes.c_float),
                      ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float),
                      ctypes.POINTER(ctypes.c_float),
                      ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                      ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    pts = [(float(x), float(y), float(z)) for x, y, z in centers]
    if len(pts) < 2:
        raise ValueError("tonicBridge: want at least 2 center CVs, got %d"
                         % len(pts))
    if not sections:
        raise ValueError("tonicBridge: want at least one section")
    ringVerts = validateRingVerts(len(sections[0][1]))
    for t, ring in sections:
        if len(ring) != ringVerts:
            raise ValueError("tonicBridge: ragged section rings (%d vs %d)"
                             % (len(ring), ringVerts))
    params = validateSectionParams([t for t, _ in sections])
    cx = (ctypes.c_float * len(pts))(*[p[0] for p in pts])
    cy = (ctypes.c_float * len(pts))(*[p[1] for p in pts])
    cz = (ctypes.c_float * len(pts))(*[p[2] for p in pts])
    secT = (ctypes.c_float * len(params))(*params)
    secU = (ctypes.c_float * (len(params) * ringVerts))(
        *[u for _, ring in sections for u, _ in ring])
    secV = (ctypes.c_float * (len(params) * ringVerts))(
        *[v for _, ring in sections for _, v in ring])
    outId = ctypes.c_int(-1)
    _check(entry(model, int(parentId), cx, cy, cz, len(pts), secT, secU,
                 secV, len(params), ringVerts, ctypes.byref(outId)),
           "Tonic_ImportLockedTube")
    return outId.value


def importSweptMesh(dll, model, parentId, rings):
    """Import ring-major swept-mesh rings as a locked child; returns its id.

    The C++ fits centers to ring centroids and sections to the K4-derived
    charts (plan §5.7: sections come from the mesh's rings).
    """
    entry = requireEntry(dll, "Tonic_ImportSweptMesh")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                      ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    if len(rings) < 2:
        raise ValueError("tonicBridge: want at least 2 rings, got %d"
                         % len(rings))
    ringVerts = validateRingVerts(len(rings[0]))
    for ring in rings:
        if len(ring) != ringVerts:
            raise ValueError("tonicBridge: ragged swept rings (%d vs %d)"
                             % (len(ring), ringVerts))
    flat = (ctypes.c_float * (len(rings) * ringVerts * 3))(
        *[float(v) for ring in rings for p in ring for v in p])
    outId = ctypes.c_int(-1)
    _check(entry(model, int(parentId), flat, len(rings), ringVerts,
                 ctypes.byref(outId)),
           "Tonic_ImportSweptMesh")
    return outId.value


# -- pxr authors (usdview only; pxr imports stay local) ----------------------
# The T1 round trip calls these against a plain Usd stage: no usdview, no
# window, just the staged package + the DLL on the CTest environment.

def authorBasisCurves(stage, parentPath, spec):
    """Author one BasisCurves prim from a basisCurvesSpec dict; returns it."""
    from pxr import UsdGeom
    parent = stage.DefinePrim(str(parentPath), "Scope")
    prim = stage.DefinePrim(parent.GetPath().AppendChild(spec["name"]),
                            "BasisCurves")
    curves = UsdGeom.BasisCurves(prim)
    curves.CreateTypeAttr("catmullRom" if spec["basis"] == "catmullRom"
                          else spec["basis"])
    curves.CreateCurveVertexCountsAttr(spec["vertexCounts"])
    curves.CreatePointsAttr(spec["points"])
    curves.CreateWidthsAttr(spec["widths"])
    curves.SetWidthsInterpolation("uniform")
    return prim


def readBasisCurves(stage, curvesPath):
    """The (points, vertexCounts) of the BasisCurves at `curvesPath`."""
    from pxr import UsdGeom
    prim = stage.GetPrimAtPath(str(curvesPath))
    if not prim:
        raise RuntimeError("tonicBridge: no prim at %s" % curvesPath)
    curves = UsdGeom.BasisCurves(prim)
    # An authored-but-empty prim reads back None rather than an empty
    # array; a hand-edited file can disagree between counts and points.
    # Both are the file's fault, so say which prim (ValueError), instead
    # of a TypeError from iterating None deep inside an import.
    rawPoints = curves.GetPointsAttr().Get()
    rawCounts = curves.GetCurveVertexCountsAttr().Get()
    if rawPoints is None or rawCounts is None:
        raise ValueError("tonicBridge: %s has no points or no "
                         "curveVertexCounts" % curvesPath)
    points = [tuple(float(v) for v in p) for p in rawPoints]
    counts = [int(v) for v in rawCounts]
    if any(c < 0 for c in counts) or sum(counts) != len(points):
        raise ValueError("tonicBridge: %s curveVertexCounts sum to %d but "
                         "it has %d points"
                         % (curvesPath, sum(counts), len(points)))
    curves_out = []
    offset = 0
    for count in counts:
        curves_out.append(points[offset:offset + count])
        offset += count
    return curves_out


def exportCenterCurves(stage, dll, model, tubeIds, parentPath, radius=1.0):
    """Export tube centers as BasisCurves; returns (prim, spec)."""
    centers = {}
    for tubeId in tubeIds:
        centers[int(tubeId)] = tubeCenters(dll, model, int(tubeId))
    spec = centerCurvesSpec(centers, radius)
    return authorBasisCurves(stage, parentPath, spec), spec


def importCurvesAsLockedTubes(dll, model, parentId, curves, ringVerts=8,
                              sectionsPerCurve=4):
    """Import USD curves as single-curve locked tubes; returns [tubeIds].

    Each curve becomes one tube whose sections are unit rings about the
    resampled centers (§5.7: a single-curve tube from a curve).

    Every curve is validated and resampled BEFORE the first tube is
    added (SS-04): a bad third curve used to raise after two tubes had
    landed, leaving a half import. ValueError names the offending curve;
    a RuntimeError can still come from the C++ side mid-loop, which the
    session's gesture bracket rolls back.
    """
    ringVerts = validateRingVerts(ringVerts)
    if int(sectionsPerCurve) < 2:
        raise ValueError("tonicBridge: want at least 2 sections per curve")
    curves = list(curves)
    if not curves:
        raise ValueError("tonicBridge: no curves to import")
    allCenters = [_importCenters(index, curve)
                  for index, curve in enumerate(curves)]
    step = 1.0 / (int(sectionsPerCurve) - 1)
    ring = [(math.cos(2.0 * math.pi * k / ringVerts),
             math.sin(2.0 * math.pi * k / ringVerts))
            for k in range(ringVerts)]
    sections = [(min(1.0, s * step), list(ring))
                for s in range(int(sectionsPerCurve))]
    ids = []
    for centers in allCenters:
        ids.append(importLockedTube(dll, model, parentId, centers, sections))
    return ids


def _importCenters(index, curve):
    """The resampled centers of curve `index`, or ValueError naming it."""
    try:
        pts = [(float(x), float(y), float(z)) for x, y, z in curve]
    except (TypeError, ValueError):
        raise ValueError("tonicBridge: curve %d has a point that is not "
                         "three numbers" % index) from None
    if len(pts) < 2:
        raise ValueError("tonicBridge: curve %d has %d point(s); a tube "
                         "needs at least 2" % (index, len(pts)))
    if not all(math.isfinite(v) for p in pts for v in p):
        raise ValueError("tonicBridge: curve %d has a non-finite point"
                         % index)
    if polylineLength(pts) <= 0.0:
        raise ValueError("tonicBridge: curve %d has zero length" % index)
    return resamplePolyline(pts, len(pts))


def roundTripStatus(tubeId, error, tolerance):
    """One human line for the round-trip report (HUD/status reuse)."""
    verdict = "PASS" if error <= tolerance else "FAIL"
    return ("%s: T%d round-trip error %.6g (tolerance %.6g)"
            % (verdict, tubeId, error, tolerance))
