# Surface picking for the brush tool: a mouse ray to (face, u, v).
#
# plan/08-tools.md section 1.7: screen-space picking needs no GL, and the CPU
# answer is ~8x cheaper than a Hydra pick at 100k CVs. The tool intersects
# the ray with the bound mesh itself, on the CPU, over a snapshot taken once
# per gesture (plan/08 section 2.1: press reads the base arrays; per-move
# work touches nothing but them).
#
# Two implementations, one answer. When the usdGenImaging brush C ABI loads
# (brushApi.py) the snapshot carries a native mesh handle (uniform-grid face
# index, 3D DDA, Moller-Trumbore per quad triangle pair) and pickFace,
# DabFootprint.expand and suggestResolution run in C++. Otherwise the
# pure-Python uniform grid below does the same work (candidates walk in
# face-id order, so its answers are bitwise-identical to brute force).
#
# v1 limits, documented rather than silent (the engine bake worker shares
# the first: attributeBake.h):
#   - Quad meshes only. Each quad is two triangles; a non-quad face fails
#     the bind, so the pick never sees one.
#   - Degenerate inputs fall back to brute force rather than failing the
#     stroke. The loop still recomputes the pick only past PICK_MOVE_PIXELS.
#
# The face-local (u, v) convention, shared with the bake (brushAuthor.py):
# quad corners v0..v3 sit at (0,0),(1,0),(1,1),(0,1); triangle (v0,v1,v2)
# and (v0,v2,v3) barycentrics map onto that square. Corner i of the map
# grid therefore bakes onto the i-th face-vertex.
#
# Face mask (a binding to a face GeomSubset, plan/02-schema.md section 2.20):
# the snapshot is always the whole PARENT mesh with parent face ids, and
# faceMask names the paintable faces. Every face still occludes the pick,
# but a nearest hit on a masked face is a miss; the footprint never spills
# onto a masked face (usdGenBrushApi.h, "Face mask").
#
# Qt-free. Needs pxr only for the UsdGeomMesh snapshot helper; the
# intersection itself is arithmetic over the snapshot, so the T1 suite
# checks it without a stage.

import math

try:
    from . import brushApi
except ImportError:  # file-path test load
    import brushApi


# Recompute the pick only past this many PHYSICAL pixels of cursor travel
# (plan/08 section 3.3, copied from usdRig's snap throttle).
PICK_MOVE_PIXELS = 3.0


class MeshSnapshot(object):
    """Points + quad faces of the bound surface, read once per gesture."""

    def __init__(self, points, faces, faceMask=None):
        # points: [(x, y, z)] in world space; faces: [(v0, v1, v2, v3)].
        self.points = points
        self.faces = faces
        # The paintable face ids (a frozenset), or None for every face.
        # Fixed at construction: the native mesh bakes it in.
        self.faceMask = (None if faceMask is None
                         else frozenset(int(f) for f in faceMask))
        # The lazily built _FaceGrid, or None until the first pick.
        self.faceGrid = None
        # The lazily built brushApi.NativeMesh (False: tried, unavailable).
        self.nativeMesh = None

    def faceCount(self):
        return len(self.faces)

    def paintable(self, face):
        """True when the face mask lets a stroke paint `face`."""
        return self.faceMask is None or face in self.faceMask


def _paintable(snapshot, face):
    """paintable() for any snapshot-like object (tests hand in their own)."""
    mask = getattr(snapshot, "faceMask", None)
    return mask is None or face in mask


def snapshotMesh(meshPrim, faces=None):
    """(snapshot, error): the mesh's world-space points and quad faces.

    faces: the paintable parent-mesh face ids (a GeomSubset binding), or
    None for every face. Fails closed for a non-mesh prim, missing
    points, any non-quad face, or a face id the mesh does not have."""
    if meshPrim is None or not meshPrim.IsValid():
        return (None, "brush bind needs a valid surface prim")
    from pxr import Gf, Usd, UsdGeom
    mesh = UsdGeom.Mesh(meshPrim)
    if not mesh:
        return (None, "brush bind needs a UsdGeomMesh, got %s"
                % meshPrim.GetTypeName())
    pointsAttr = mesh.GetPointsAttr()
    countsAttr = mesh.GetFaceVertexCountsAttr()
    indicesAttr = mesh.GetFaceVertexIndicesAttr()
    if not pointsAttr or not countsAttr or not indicesAttr:
        return (None, "the surface mesh has no points/faces")
    points = pointsAttr.Get()
    counts = countsAttr.Get()
    indices = indicesAttr.Get()
    if not points or not counts or not indices:
        return (None, "the surface mesh has no points/faces")
    if any(c != 4 for c in counts):
        return (None, "the brush paints quad meshes only in v1 "
                "(a face is not a quad)")
    if len(indices) != 4 * len(counts):
        return (None, "the surface mesh face indices do not match "
                "its face counts")
    if faces is not None:
        try:
            faces = frozenset(int(f) for f in faces)
        except (TypeError, ValueError):
            return (None, "the face subset is not a list of face ids")
        if any(f < 0 or f >= len(counts) for f in faces):
            return (None, "the face subset names a face the %d-face mesh "
                    "does not have" % len(counts))
    xform = UsdGeom.Xformable(meshPrim).ComputeLocalToWorldTransform(
        Usd.TimeCode.Default())
    try:
        import numpy as np
    except ImportError:
        np = None
    if np is not None:
        # One C-level pass each way: no per-element VtArray access.
        pts = np.asarray(points, dtype=np.float64).reshape((-1, 3))
        m = np.array(xform, dtype=np.float64).reshape((4, 4))
        homo = pts @ m[:3, :3] + m[3, :3]
        w = pts @ m[:3, 3] + m[3, 3]
        if np.all(np.abs(w - 1.0) < 1e-12):
            worldArr = homo
        else:
            worldArr = homo / w[:, None]
        idx = np.asarray(indices, dtype=np.int64).reshape((-1, 4))
        if idx.size and (idx.min() < 0 or idx.max() >= len(pts)):
            return (None, "the surface mesh has an out-of-range "
                    "face-vertex index")
        world = [tuple(p) for p in worldArr.tolist()]
        quads = [tuple(q) for q in idx.tolist()]
        return (MeshSnapshot(world, quads, faces), "")
    world = []
    for p in points:
        q = xform.Transform(Gf.Vec3d(p))
        world.append((float(q[0]), float(q[1]), float(q[2])))
    quads = []
    for f in range(len(counts)):
        quad = (int(indices[4 * f]), int(indices[4 * f + 1]),
                int(indices[4 * f + 2]), int(indices[4 * f + 3]))
        if any(v < 0 or v >= len(world) for v in quad):
            return (None, "the surface mesh has an out-of-range "
                    "face-vertex index")
        quads.append(quad)
    return (MeshSnapshot(world, quads, faces), "")


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _rayTriangle(origin, direction, v0, v1, v2):
    """Moller-Trumbore: (distance, w0, w1, w2) or None on a miss."""
    edge1 = _sub(v1, v0)
    edge2 = _sub(v2, v0)
    pvec = _cross(direction, edge2)
    det = _dot(edge1, pvec)
    if abs(det) < 1e-20:
        return None
    inv = 1.0 / det
    tvec = _sub(origin, v0)
    b1 = _dot(tvec, pvec) * inv
    if b1 < 0.0 or b1 > 1.0:
        return None
    qvec = _cross(tvec, edge1)
    b2 = _dot(direction, qvec) * inv
    if b2 < 0.0 or b1 + b2 > 1.0:
        return None
    dist = _dot(edge2, qvec) * inv
    if dist <= 0.0:
        return None
    return (dist, 1.0 - b1 - b2, b1, b2)


class _FaceGrid(object):
    """Uniform-grid face buckets over a snapshot (the pick index).

    Built once per gesture; raycast()/nearby() return sorted face-id
    candidates, and callers test those in order with the exact
    brute-force kernels, so answers are bitwise-identical to brute
    force while triangle tests drop from all faces to the few along
    the ray or in the disk. Each face's insert range pads one cell
    per side, so float wobble at cell boundaries (entry rounding, a
    ray through a grid vertex) can only add candidates, never drop a
    hit. Raises on empty input and past a build budget (pathological
    spans fall back to brute force); callers treat any failure as
    "no index"."""

    _MAX_CELLS = 64
    _BUDGET_PER_FACE = 48
    _BUDGET_BASE = 32768

    def __init__(self, snapshot):
        faces = snapshot.faces
        points = snapshot.points
        if not faces or not points:
            raise ValueError("the pick index needs faces and points")
        cells = int(round(len(faces) ** (1.0 / 3.0)))
        self._cells = min(self._MAX_CELLS, max(1, cells))
        bmin = [float("inf")] * 3
        bmax = [float("-inf")] * 3
        for p in points:
            for k in range(3):
                v = float(p[k])
                if v < bmin[k]:
                    bmin[k] = v
                if v > bmax[k]:
                    bmax[k] = v
        extent = max(bmax[k] - bmin[k] for k in range(3))
        pad = max(1e-6, extent * 1e-9)
        self._bmin = tuple(bmin[k] - pad for k in range(3))
        self._size = tuple(
            (bmax[k] + pad - self._bmin[k]) / self._cells
            for k in range(3))
        budget = (self._BUDGET_PER_FACE * len(faces)
                  + self._BUDGET_BASE)
        buckets = {}
        for face, (i0, i1, i2, i3) in enumerate(faces):
            corners = [points[i0], points[i1], points[i2], points[i3]]
            lo, hi = [], []
            for k in range(3):
                vals = [float(c[k]) for c in corners]
                lo.append(self._cell(min(vals), k) - 1)
                hi.append(self._cell(max(vals), k) + 1)
            for iz in range(max(0, lo[2]),
                            min(self._cells - 1, hi[2]) + 1):
                for iy in range(max(0, lo[1]),
                                min(self._cells - 1, hi[1]) + 1):
                    for ix in range(max(0, lo[0]),
                                    min(self._cells - 1, hi[0]) + 1):
                        budget -= 1
                        if budget < 0:
                            raise ValueError(
                                "the pick index blew its budget")
                        buckets.setdefault(
                            (ix, iy, iz), []).append(face)
        self._buckets = buckets

    def _cell(self, value, axis):
        at = int((value - self._bmin[axis]) / self._size[axis])
        return min(self._cells - 1, max(0, at))

    def raycast(self, origin, direction):
        """Sorted face ids along the ray: [] on a clean miss, None to
        fall back to brute force (an unusable ray)."""
        try:
            o = [float(origin[k]) for k in range(3)]
            d = [float(direction[k]) for k in range(3)]
        except (TypeError, ValueError, IndexError):
            return None
        if any(v != v or v in (float("inf"), float("-inf"))
               for v in o + d):
            return None
        if d[0] == 0.0 and d[1] == 0.0 and d[2] == 0.0:
            return None
        tmin, tmax = 0.0, float("inf")
        for k in range(3):
            blo = self._bmin[k]
            bhi = blo + self._size[k] * self._cells
            if d[k] == 0.0:
                if o[k] < blo or o[k] > bhi:
                    return []
            else:
                t0 = (blo - o[k]) / d[k]
                t1 = (bhi - o[k]) / d[k]
                if t0 > t1:
                    t0, t1 = t1, t0
                tmin = max(tmin, t0)
                tmax = min(tmax, t1)
        if tmin > tmax:
            return []
        cell = [self._cell(o[k] + d[k] * tmin, k) for k in range(3)]
        step, tmaxv, tdelta = [], [], []
        for k in range(3):
            if d[k] > 0.0:
                step.append(1)
                edge = self._bmin[k] + self._size[k] * (cell[k] + 1)
                tmaxv.append((edge - o[k]) / d[k])
                tdelta.append(self._size[k] / d[k])
            elif d[k] < 0.0:
                step.append(-1)
                edge = self._bmin[k] + self._size[k] * cell[k]
                tmaxv.append((edge - o[k]) / d[k])
                tdelta.append(-self._size[k] / d[k])
            else:
                step.append(0)
                tmaxv.append(float("inf"))
                tdelta.append(float("inf"))
        out = set()
        guard = 12 * self._cells + 8
        t = tmin
        while t <= tmax and guard > 0:
            guard -= 1
            bucket = self._buckets.get((cell[0], cell[1], cell[2]))
            if bucket:
                out.update(bucket)
            # Step the nearest boundary (ties: x, then y, then z).
            if tmaxv[0] <= tmaxv[1] and tmaxv[0] <= tmaxv[2]:
                k = 0
            elif tmaxv[1] <= tmaxv[2]:
                k = 1
            else:
                k = 2
            t = tmaxv[k]
            tmaxv[k] += tdelta[k]
            cell[k] += step[k]
        return sorted(out)

    def nearby(self, point, radius):
        """Sorted face ids whose cells overlap the sphere, None to
        fall back (an unusable point or radius)."""
        try:
            p = [float(point[k]) for k in range(3)]
            r = float(radius)
        except (TypeError, ValueError, IndexError):
            return None
        if any(v != v or v in (float("inf"), float("-inf")) for v in p):
            return None
        if r != r or r in (float("inf"), float("-inf")) or r < 0.0:
            return None
        lo = [self._cell(p[k] - r, k) for k in range(3)]
        hi = [self._cell(p[k] + r, k) for k in range(3)]
        out = set()
        for iz in range(lo[2], hi[2] + 1):
            for iy in range(lo[1], hi[1] + 1):
                for ix in range(lo[0], hi[0] + 1):
                    bucket = self._buckets.get((ix, iy, iz))
                    if bucket:
                        out.update(bucket)
        return sorted(out)


def _gridFor(snapshot):
    """The snapshot's pick index, built lazily; None means brute force."""
    try:
        grid = getattr(snapshot, "faceGrid", None)
    except Exception:
        return None
    if grid is not None:
        return grid
    try:
        grid = _FaceGrid(snapshot)
    except Exception:
        return None
    try:
        snapshot.faceGrid = grid
    except Exception:
        pass
    return grid


def _testFace(snapshot, face, origin, direction):
    """(dist, u, v, point) for one quad, or None on a miss."""
    i0, i1, i2, i3 = snapshot.faces[face]
    v0 = snapshot.points[i0]
    v1 = snapshot.points[i1]
    v2 = snapshot.points[i2]
    v3 = snapshot.points[i3]
    # Triangle A (v0, v1, v2): u = w1 + w2, v = w2.
    hit = _rayTriangle(origin, direction, v0, v1, v2)
    if hit is not None:
        dist, _w0, w1, w2 = hit
        uv = (w1 + w2, w2)
    else:
        # Triangle B (v0, v2, v3): u = w1, v = w1 + w2.
        hit = _rayTriangle(origin, direction, v0, v2, v3)
        if hit is None:
            return None
        dist, _w0, w1, w2 = hit
        uv = (w1, w1 + w2)
    u = min(1.0, max(0.0, uv[0]))
    v = min(1.0, max(0.0, uv[1]))
    point = (origin[0] + direction[0] * dist,
             origin[1] + direction[1] * dist,
             origin[2] + direction[2] * dist)
    return (dist, u, v, point)


def _pickBrute(snapshot, origin, direction):
    """pickFace by brute force: the exactness oracle for the index."""
    best = None
    for face in range(len(snapshot.faces)):
        got = _testFace(snapshot, face, origin, direction)
        if got is None:
            continue
        if best is None or got[0] < best[0]:
            best = (got[0], face, got[1], got[2], got[3])
    if best is None or not _paintable(snapshot, best[1]):
        return None
    return (best[1], best[2], best[3], best[4])


def pickFace(snapshot, origin, direction):
    """Nearest quad under the ray: (face, u, v, point) or None.

    Both windings hit: a scalp is painted from the outside, but a test
    camera may sit anywhere, and a one-sided pick would read as a tool
    bug. (u, v) is face-local per the module convention. A uniform-grid
    index narrows the candidates; the triangle tests run in face-id
    order, so the answer matches brute force bit for bit. A nearest hit
    on a face outside the snapshot's faceMask is a miss (it occludes)."""
    if snapshot is None or origin is None or direction is None:
        return None
    native = brushApi.meshFor(snapshot)
    if native is not None:
        try:
            return native.pick(origin, direction)
        except (TypeError, ValueError, IndexError):
            return None
    grid = _gridFor(snapshot)
    candidates = None
    if grid is not None:
        try:
            candidates = grid.raycast(origin, direction)
        except Exception:
            candidates = None
    if candidates is None:
        return _pickBrute(snapshot, origin, direction)
    best = None
    for face in candidates:
        got = _testFace(snapshot, face, origin, direction)
        if got is None:
            continue
        if best is None or got[0] < best[0]:
            best = (got[0], face, got[1], got[2], got[3])
    if best is None or not _paintable(snapshot, best[1]):
        return None
    return (best[1], best[2], best[3], best[4])


def pickPixels(snapshot, camera, x, y):
    """pickFace for a physical pixel through a BrushCamera, or None."""
    if camera is None:
        return None
    ray = camera.rayThrough(x, y)
    if ray is None:
        return None
    return pickFace(snapshot, ray[0], ray[1])


def movedPixels(a, b):
    """Physical-pixel distance between two (x, y) points."""
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2)


# A hovered hit always shows at least this many physical pixels of ring,
# so a zero-radius brush still marks the cursor.
RING_MIN_PIXELS = 1.5

# Corner-reach floor, in face-local UV: the bake only keeps
# corners, so a dab smaller than ~0.7 of a face can vanish
# between them. Dabs floor here (and the ring draws the
# floored disk), so every dab paints visibly.
MIN_FACE_RADIUS = 0.75


def ringPixels(snapshot, camera, x, y, radiusWorld):
    """(cx, cy, rPixels) for the cursor ring, or None when it must hide.

    The centre is the picked hit reprojected (the cursor, up to float
    wobble); the radius is the world-space brush disk at the hit
    depth, floored to the corner-reach minimum like the dab itself,
    so the ring draws what the dab paints. None means no hit, or
    an unusable camera or radius: the caller hides the ring.
    Everything is physical pixels."""
    if snapshot is None or camera is None:
        return None
    try:
        radius = float(radiusWorld)
    except (TypeError, ValueError):
        return None
    if radius != radius or radius in (float("inf"), float("-inf")):
        return None
    hit = pickPixels(snapshot, camera, x, y)
    if hit is None:
        return None
    face, _u, _v, point = hit
    if face < 0 or face >= len(snapshot.faces):
        return None
    projected = camera.worldToPixels(point)
    if projected is None:
        return None
    cx, cy, ndcZ = projected
    worldR = max(radius, 0.0)
    edge = faceEdgeLen(snapshot, face)
    if edge > 1e-12:
        worldR = max(worldR, MIN_FACE_RADIUS * edge)
    worldPerPixel = None
    a = camera.pixelsToWorld(cx, cy, ndcZ)
    b = camera.pixelsToWorld(cx + 1.0, cy, ndcZ)
    if a is not None and b is not None:
        wpp = math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2
                         + (a[2] - b[2]) ** 2)
        if wpp > 1e-12:
            worldPerPixel = wpp
    if worldPerPixel is None:
        return (cx, cy, RING_MIN_PIXELS)
    return (cx, cy, max(RING_MIN_PIXELS, worldR / worldPerPixel))


def _distSq(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2
            + (a[2] - b[2]) ** 2)


def _bboxDistSq(bmin, bmax, point):
    """Squared distance from a point to an xyz box (0 inside)."""
    total = 0.0
    for k in range(3):
        if point[k] < bmin[k]:
            total += (bmin[k] - point[k]) ** 2
        elif point[k] > bmax[k]:
            total += (point[k] - bmax[k]) ** 2
    return total


def _closestOnTriangle(point, a, b, c):
    """Closest point on triangle abc: (distSq, w0, w1, w2).

    Real-Time Collision Detection 5.1.5 (Voronoi regions). Degenerate
    triangles fall back to the nearest vertex rather than dividing
    by a zero area."""
    ab = _sub(b, a)
    ac = _sub(c, a)
    ap = _sub(point, a)
    d1 = _dot(ab, ap)
    d2 = _dot(ac, ap)
    if d1 <= 0.0 and d2 <= 0.0:
        return (_distSq(point, a), 1.0, 0.0, 0.0)
    bp = _sub(point, b)
    d3 = _dot(ab, bp)
    d4 = _dot(ac, bp)
    if d3 >= 0.0 and d4 <= d3:
        return (_distSq(point, b), 0.0, 1.0, 0.0)
    vc = d1 * d4 - d3 * d2
    if vc <= 0.0 and d1 >= 0.0 and d3 <= 0.0:
        denom = d1 - d3
        v = d1 / denom if denom != 0.0 else 0.0
        q = (a[0] + ab[0] * v, a[1] + ab[1] * v, a[2] + ab[2] * v)
        return (_distSq(point, q), 1.0 - v, v, 0.0)
    cp = _sub(point, c)
    d5 = _dot(ab, cp)
    d6 = _dot(ac, cp)
    if d6 >= 0.0 and d5 <= d6:
        return (_distSq(point, c), 0.0, 0.0, 1.0)
    vb = d5 * d2 - d1 * d6
    if vb <= 0.0 and d2 >= 0.0 and d6 <= 0.0:
        denom = d2 - d6
        w = d2 / denom if denom != 0.0 else 0.0
        q = (a[0] + ac[0] * w, a[1] + ac[1] * w, a[2] + ac[2] * w)
        return (_distSq(point, q), 1.0 - w, 0.0, w)
    va = d3 * d6 - d5 * d4
    if va <= 0.0 and (d4 - d3) >= 0.0 and (d5 - d6) >= 0.0:
        denom = (d4 - d3) + (d5 - d6)
        w = (d4 - d3) / denom if denom != 0.0 else 0.0
        q = (b[0] + (c[0] - b[0]) * w,
             b[1] + (c[1] - b[1]) * w,
             b[2] + (c[2] - b[2]) * w)
        return (_distSq(point, q), 0.0, 1.0 - w, w)
    denom = va + vb + vc
    if denom == 0.0:
        best = min(((_distSq(point, p), i)
                    for i, p in enumerate((a, b, c))))
        w = [0.0, 0.0, 0.0]
        w[best[1]] = 1.0
        return (best[0], w[0], w[1], w[2])
    v = vb / denom
    w = vc / denom
    q = (a[0] + ab[0] * v + ac[0] * w,
         a[1] + ab[1] * v + ac[1] * w,
         a[2] + ab[2] * v + ac[2] * w)
    return (_distSq(point, q), 1.0 - v - w, v, w)


def quadClosest(snapshot, face, point):
    """(dist, u, v) of the closest point on quad `face`, or None.

    The (u, v) convention matches pickFace: the two triangles map
    onto the unit square the same way the raycast does."""
    if snapshot is None or point is None:
        return None
    if face < 0 or face >= len(snapshot.faces):
        return None
    i0, i1, i2, i3 = snapshot.faces[face]
    pts = snapshot.points
    v0, v1, v2, v3 = pts[i0], pts[i1], pts[i2], pts[i3]
    dA, _w0, w1, w2 = _closestOnTriangle(point, v0, v1, v2)
    dB, _u0, u1, u2 = _closestOnTriangle(point, v0, v2, v3)
    if dA <= dB:
        u, v = w1 + w2, w2
        dist = math.sqrt(dA)
    else:
        u, v = u1, u1 + u2
        dist = math.sqrt(dB)
    return (dist, min(1.0, max(0.0, u)), min(1.0, max(0.0, v)))


def _planeUV(point, o, e1, e2):
    """Unclamped plane coordinates (w1, w2) of point over o + w1 e1 + w2 e2."""
    d = _sub(point, o)
    a, b, c = _dot(e1, e1), _dot(e1, e2), _dot(e2, e2)
    det = a * c - b * b
    if not abs(det) > 1e-30:
        return None
    r1, r2 = _dot(e1, d), _dot(e2, d)
    return ((r1 * c - r2 * b) / det, (r2 * a - r1 * b) / det)


def quadCentreUV(snapshot, face, point):
    """The dab centre in quad `face`'s own (u, v) frame, NOT clamped.

    Plane coordinates over the triangle the closest point falls in,
    extended linearly past the border (exact on planar quads). A spill
    stamp is centred here, so its falloff continues across the face
    border instead of restarting at full weight on it. None when
    unusable."""
    if snapshot is None or point is None:
        return None
    if face < 0 or face >= len(snapshot.faces):
        return None
    i0, i1, i2, i3 = snapshot.faces[face]
    pts = snapshot.points
    v0, v1, v2, v3 = pts[i0], pts[i1], pts[i2], pts[i3]
    dA = _closestOnTriangle(point, v0, v1, v2)[0]
    dB = _closestOnTriangle(point, v0, v2, v3)[0]
    if dA <= dB:
        w = _planeUV(point, v0, _sub(v1, v0), _sub(v2, v0))
        return None if w is None else (w[0] + w[1], w[1])
    w = _planeUV(point, v0, _sub(v2, v0), _sub(v3, v0))
    return None if w is None else (w[0], w[0] + w[1])


def facePoint(snapshot, face, u, v):
    """Bilinear world point of face-local (u, v), or None off-range."""
    if snapshot is None:
        return None
    if face < 0 or face >= len(snapshot.faces):
        return None
    try:
        u, v = float(u), float(v)
    except (TypeError, ValueError):
        return None
    if u != u or v != v:
        return None
    i0, i1, i2, i3 = snapshot.faces[face]
    pts = snapshot.points
    v0, v1, v2, v3 = pts[i0], pts[i1], pts[i2], pts[i3]
    iu, iv = 1.0 - u, 1.0 - v
    return tuple(v0[k] * iu * iv + v1[k] * u * iv
                 + v2[k] * u * v + v3[k] * iu * v
                 for k in range(3))


def faceEdgeLen(snapshot, face):
    """Longest world edge of `face`, or 0.0 when unusable."""
    try:
        quad = snapshot.faces[face]
        pts = snapshot.points
    except (AttributeError, TypeError, IndexError):
        return 0.0
    try:
        longest = 0.0
        for k in range(4):
            longest = max(longest, math.sqrt(_distSq(
                pts[quad[k]], pts[quad[(k + 1) % 4]])))
    except (TypeError, ValueError, IndexError, ArithmeticError):
        return 0.0
    return longest if longest == longest else 0.0


class DabFootprint(object):
    """A dab's world-space footprint over a snapshot, cached per stroke.

    Built once per gesture (face bounds plus edge lengths are one
    pass); expand() then answers per dab, so a dab near an edge spills
    onto its neighbours instead of stopping at the picked face."""

    # Faces smaller than this have no usable parametrisation; the
    # footprint skips them rather than dividing by their edge.
    _MIN_EDGE = 1e-12

    def __init__(self, snapshot):
        self._snapshot = snapshot
        self._bounds = []
        self._edges = []
        if snapshot is None:
            return
        for index, (i0, i1, i2, i3) in enumerate(snapshot.faces):
            corners = [snapshot.points[i] for i in (i0, i1, i2, i3)]
            bmin = tuple(min(c[k] for c in corners) for k in range(3))
            bmax = tuple(max(c[k] for c in corners) for k in range(3))
            self._bounds.append((bmin, bmax))
            self._edges.append(faceEdgeLen(snapshot, index))

    def expand(self, face, u, v, point, radiusUv):
        """[(face, u, v, radius)] the footprint covers; picked face first.

        The picked face keeps its exact (u, v, radius); every other
        paintable face within the world-space disk gets the dab centre
        in its own UNCLAMPED (u, v) frame (quadCentreUV: past the
        border, so the falloff continues across it) and the radius
        rescaled by its own edge length. Degenerate and masked faces
        are skipped; bad inputs yield the primary alone."""
        primary = [(face, u, v, radiusUv)]
        snapshot = self._snapshot
        if snapshot is None or point is None:
            return primary
        try:
            radius = float(radiusUv)
        except (TypeError, ValueError):
            return primary
        if radius != radius or radius < 0.0 or radius == float("inf"):
            return primary
        if face < 0 or face >= len(snapshot.faces):
            return primary
        try:
            px, py, pz = (float(point[0]), float(point[1]),
                          float(point[2]))
        except (TypeError, ValueError, IndexError):
            return primary
        worldR = radius * self._edges[face]
        native = brushApi.meshFor(snapshot)
        if native is not None:
            got = native.footprint(face, u, v, (px, py, pz), worldR)
            if got:
                # The primary keeps the caller's exact radius (the C++
                # recomputes it as worldR / edge, equal up to rounding).
                return [primary[0]] + got[1:]
        limit = worldR + 1e-9
        limitSq = limit * limit
        out = [primary[0]]
        others = None
        grid = _gridFor(snapshot)
        if grid is not None:
            try:
                others = grid.nearby((px, py, pz), limit)
            except Exception:
                others = None
        if others is None:
            others = range(len(snapshot.faces))
        for other in others:
            if other == face or not _paintable(snapshot, other):
                continue
            edge = self._edges[other]
            if edge <= self._MIN_EDGE:
                continue
            bmin, bmax = self._bounds[other]
            if _bboxDistSq(bmin, bmax, (px, py, pz)) > limitSq:
                continue
            hit = quadClosest(snapshot, other, (px, py, pz))
            if hit is None or hit[0] > limit:
                continue
            centre = quadCentreUV(snapshot, other, (px, py, pz))
            cu, cv = centre if centre is not None else (hit[1], hit[2])
            out.append((other, cu, cv, worldR / edge))
        return out


# Texels per face edge the auto-density bind aims for, and the default
# budget of the whole map in texels.
SUGGEST_TEXELS_ACROSS = 32
SUGGEST_BUDGET_TEXELS = 4000000


def suggestResolution(snapshot, budgetTexels=SUGGEST_BUDGET_TEXELS):
    """(resolution, info) for an auto-density bind of the snapshot's mesh.

    Only four corners per face persist in the bake, so texels only buy
    in-stroke detail: SUGGEST_TEXELS_ACROSS per face edge, halved until
    faces * res^2 fits the budget, floored at 4. info is a short human
    string ("32 px/face (median edge 0.25) -> 2.1 MTexel"). The C++ ABI
    answers when it loads; this is the same rule in Python."""
    if snapshot is None or not snapshot.faces:
        return (16, "no mesh: 16 px/face")
    try:
        budget = int(budgetTexels)
    except (TypeError, ValueError):
        budget = SUGGEST_BUDGET_TEXELS
    if budget <= 0:
        budget = SUGGEST_BUDGET_TEXELS
    native = brushApi.meshFor(snapshot)
    if native is not None:
        res, info = native.suggestResolution(budget)
        if res > 0:
            return (res, info)
    faces = len(snapshot.faces)
    res = SUGGEST_TEXELS_ACROSS
    while res > 4 and faces * res * res > budget:
        res //= 2
    means = []
    pts = snapshot.points
    for quad in snapshot.faces:
        total = 0.0
        for k in range(4):
            total += math.sqrt(_distSq(pts[quad[k]], pts[quad[(k + 1) % 4]]))
        means.append(total / 4.0)
    means.sort()
    median = means[len(means) // 2]
    return (res, "%d px/face (median edge %.3g) -> %.1f MTexel"
            % (res, median, faces * res * res / 1.0e6))
