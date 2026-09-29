# usdGenPomadeTools.pomadeCamera -- the viewport camera, resolved once per
# gesture (plan/18 section 3.1, section 3.2).
#
# Qt-free and pxr-free at import: the whole class is arithmetic over a
# row-major float[16], so a T0 test can build one from a hand-written
# frustum and check every projection without a window. `resolve()` is the
# one function that touches usdview, and it only reads the StageView.
#
# CONVENTION. Pomade_Pick, Pomade_SelectRect and Pomade_SelectPolygon take
# `viewProj` in the row-major USD spelling that gpu/picking.cu projects
# with, i.e. clip[j] = sum_i p[i] * M[i*4 + j] with p = (x, y, z, 1) as a
# ROW vector. Gf composes the same way (`view * proj`), so the matrix this
# module carries is exactly what the C ABI wants, flattened row by row.
# Pixels are top-left origin and PHYSICAL (device) pixels, which is what
# StageView.computeWindowViewport reports and what StageView's own mouse
# handlers scale event coordinates into.
from __future__ import annotations

import ctypes


def matMul(a, b):
    """Row-major 4x4 product a*b, both as flat 16-tuples."""
    out = [0.0] * 16
    for i in range(4):
        for j in range(4):
            out[i * 4 + j] = (a[i * 4 + 0] * b[0 * 4 + j] +
                              a[i * 4 + 1] * b[1 * 4 + j] +
                              a[i * 4 + 2] * b[2 * 4 + j] +
                              a[i * 4 + 3] * b[3 * 4 + j])
    return tuple(out)


def matInverse(m):
    """Gauss-Jordan inverse of a row-major 4x4; None when singular."""
    a = [[float(m[r * 4 + c]) for c in range(4)] + [0.0] * 4
         for r in range(4)]
    for r in range(4):
        a[r][4 + r] = 1.0
    for col in range(4):
        pivot = max(range(col, 4), key=lambda r: abs(a[r][col]))
        if abs(a[pivot][col]) < 1e-20:
            return None
        if pivot != col:
            a[col], a[pivot] = a[pivot], a[col]
        scale = 1.0 / a[col][col]
        for k in range(col, 8):
            a[col][k] *= scale
        for r in range(4):
            if r == col:
                continue
            factor = a[r][col]
            if factor == 0.0:
                continue
            for k in range(col, 8):
                a[r][k] -= factor * a[col][k]
    return tuple(a[r][4 + c] for r in range(4) for c in range(4))


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _length(v):
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5


def _normalize(v):
    n = _length(v)
    if n <= 0.0:
        return (0.0, 0.0, 0.0)
    return (v[0] / n, v[1] / n, v[2] / n)


class PomadeCamera:
    """One resolved view: the K11 matrix plus the pixel rectangle it maps to.

    Immutable by contract. The controller builds one at press and hands the
    same object to every move of that gesture, so a camera the artist nudges
    mid-drag cannot move the geometry under the cursor (plan/17 section 1.3
    "resolve camera once").
    """

    __slots__ = ("_m", "_inv", "_w", "_h", "_ratio")

    def __init__(self, viewProj, width, height, pixelRatio=1.0):
        m = tuple(float(v) for v in viewProj)
        if len(m) != 16:
            raise ValueError("pomadeCamera: want 16 floats, got %d" % len(m))
        self._m = m
        self._inv = matInverse(m)
        self._w = max(int(width), 1)
        self._h = max(int(height), 1)
        ratio = float(pixelRatio) if pixelRatio else 1.0
        self._ratio = ratio if ratio > 0.0 else 1.0

    @property
    def pixelRatio(self):
        """Physical pixels per Qt logical pixel (devicePixelRatioF).

        Pixel tolerances and handle sizes are LOGICAL numbers; anything
        comparing them against this camera's physical pixels multiplies by
        this first, so a HiDPI display keeps the same hand-sized targets.
        """
        return self._ratio

    @property
    def viewProj(self):
        return self._m

    @property
    def width(self):
        return self._w

    @property
    def height(self):
        return self._h

    @property
    def invertible(self):
        return self._inv is not None

    def viewProjArray(self):
        """The matrix as a ctypes float[16], ready for Pomade_Pick."""
        return (ctypes.c_float * 16)(*self._m)

    def worldToPixels(self, point):
        """(x, y, ndcZ) for a world point, or None when it is behind.

        x/y are top-left-origin physical pixels; ndcZ is the clip-space
        depth in [-1, 1], which is what Pomade_Pick reports back.
        """
        m = self._m
        x, y, z = (float(point[0]), float(point[1]), float(point[2]))
        cx = x * m[0] + y * m[4] + z * m[8] + m[12]
        cy = x * m[1] + y * m[5] + z * m[9] + m[13]
        cz = x * m[2] + y * m[6] + z * m[10] + m[14]
        cw = x * m[3] + y * m[7] + z * m[11] + m[15]
        if cw <= 0.0:
            return None
        inv = 1.0 / cw
        ndcX = cx * inv
        ndcY = cy * inv
        return ((ndcX * 0.5 + 0.5) * self._w,
                (1.0 - (ndcY * 0.5 + 0.5)) * self._h,
                cz * inv)

    def pixelsToWorld(self, x, y, depth=0.0):
        """The world point at pixel (x, y) and clip depth `depth`."""
        if self._inv is None:
            return None
        inv = self._inv
        ndcX = 2.0 * float(x) / self._w - 1.0
        ndcY = 1.0 - 2.0 * float(y) / self._h
        ndcZ = float(depth)
        px = (ndcX * inv[0] + ndcY * inv[4] + ndcZ * inv[8] + inv[12])
        py = (ndcX * inv[1] + ndcY * inv[5] + ndcZ * inv[9] + inv[13])
        pz = (ndcX * inv[2] + ndcY * inv[6] + ndcZ * inv[10] + inv[14])
        pw = (ndcX * inv[3] + ndcY * inv[7] + ndcZ * inv[11] + inv[15])
        if abs(pw) < 1e-20:
            return None
        return (px / pw, py / pw, pz / pw)

    def rayThrough(self, x, y):
        """(origin, direction) in world space for pixel (x, y).

        Built by unprojecting the near and far plane, so it is right for an
        orthographic camera too (where every ray shares a direction but not
        an origin). This is what Pomade_Raycast takes.
        """
        near = self.pixelsToWorld(x, y, -1.0)
        far = self.pixelsToWorld(x, y, 1.0)
        if near is None or far is None:
            return None
        direction = _normalize(_sub(far, near))
        if direction == (0.0, 0.0, 0.0):
            return None
        return (near, direction)

    def worldPerPixel(self, point):
        """World units spanned by one pixel at `point`'s depth.

        Measured, not derived from a field of view: project the point, step
        one pixel sideways at the same depth, unproject, take the distance.
        That is exact for perspective and orthographic alike, which is what
        the Graph snap radius (a pixel number in the panel, a rest-unit
        number in the ABI) needs.
        """
        projected = self.worldToPixels(point)
        if projected is None:
            return 0.0
        px, py, depth = projected
        here = self.pixelsToWorld(px, py, depth)
        over = self.pixelsToWorld(px + 1.0, py, depth)
        if here is None or over is None:
            return 0.0
        return _length(_sub(over, here))


def viewProjFromFrustum(frustum):
    """The row-major float[16] a Gf.Frustum projects with.

    Gf multiplies row vectors, so view * proj composed in that order and
    flattened row by row is already the picking convention; no transpose.
    """
    m = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()
    return tuple(m.GetRow(i)[j] for i in range(4) for j in range(4))


def resolve(stageView):
    """A PomadeCamera for the StageView's current camera, or None.

    Called once per press. `resolveCamera()` is the same entry usdview's
    own draw uses, so the matrix here is the one Hydra rasterised with, and
    `computeWindowViewport()` reports the physical pixel rectangle that
    matrix maps onto.
    """
    if stageView is None:
        return None
    try:
        gfCamera, _aspect = stageView.resolveCamera()
        viewProj = viewProjFromFrustum(gfCamera.frustum)
        _x, _y, width, height = stageView.computeWindowViewport()
    except Exception:
        return None
    if width < 1 or height < 1:
        return None
    try:
        ratio = float(stageView.devicePixelRatioF())
    except (AttributeError, RuntimeError, TypeError, ValueError):
        ratio = 1.0
    return PomadeCamera(viewProj, width, height, ratio)
