# The viewport camera for the brush tool, resolved once per gesture.
#
# plan/08-tools.md section 2.1: press resolves the camera once; every move of
# that gesture reuses the same matrix, so a camera the artist nudges mid-drag
# cannot move the geometry under the cursor.
#
# The matrix is the row-major USD spelling Gf composes (`view * proj`,
# flattened row by row), the same convention usdGenPomadeTools.pomadeCamera
# carries; pixels are top-left-origin PHYSICAL pixels, what
# StageView.computeWindowViewport reports and what the event filter scales
# Qt's logical coordinates into. Kept independent of the Pomade module on
# purpose: this is the non-Pomade paint tool, and it must not move with it.
#
# Qt-free and pxr-free at import: pure arithmetic over a float[16], so the T1
# suite checks every projection from a hand-written frustum. resolve() is the
# one function that touches usdview, and it only reads the StageView.

import math


def matMul(a, b):
    """Row-major 4x4 product a*b, both as flat 16-tuples."""
    out = [0.0] * 16
    for i in range(4):
        for j in range(4):
            out[i * 4 + j] = (a[i * 4 + 0] * b[0 * 4 + j]
                              + a[i * 4 + 1] * b[1 * 4 + j]
                              + a[i * 4 + 2] * b[2 * 4 + j]
                              + a[i * 4 + 3] * b[3 * 4 + j])
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
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _normalize(v):
    n = _length(v)
    if n <= 0.0:
        return (0.0, 0.0, 0.0)
    return (v[0] / n, v[1] / n, v[2] / n)


class BrushCamera(object):
    """One resolved view: the picking matrix plus its pixel rectangle."""

    def __init__(self, viewProj, width, height):
        m = tuple(float(v) for v in viewProj)
        if len(m) != 16:
            raise ValueError("brushCamera: want 16 floats, got %d" % len(m))
        self._m = m
        self._inv = matInverse(m)
        self._w = max(int(width), 1)
        self._h = max(int(height), 1)

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

    def worldToPixels(self, point):
        """(x, y, ndcZ) for a world point, or None when it is behind."""
        m = self._m
        x, y, z = (float(point[0]), float(point[1]), float(point[2]))
        cx = x * m[0] + y * m[4] + z * m[8] + m[12]
        cy = x * m[1] + y * m[5] + z * m[9] + m[13]
        cz = x * m[2] + y * m[6] + z * m[10] + m[14]
        cw = x * m[3] + y * m[7] + z * m[11] + m[15]
        if cw <= 0.0:
            return None
        inv = 1.0 / cw
        return ((cx * inv * 0.5 + 0.5) * self._w,
                (1.0 - (cy * inv * 0.5 + 0.5)) * self._h,
                cz * inv)

    def pixelsToWorld(self, x, y, depth=0.0):
        """The world point at physical pixel (x, y) and clip depth."""
        if self._inv is None:
            return None
        inv = self._inv
        ndcX = 2.0 * float(x) / self._w - 1.0
        ndcY = 1.0 - 2.0 * float(y) / self._h
        ndcZ = float(depth)
        px = ndcX * inv[0] + ndcY * inv[4] + ndcZ * inv[8] + inv[12]
        py = ndcX * inv[1] + ndcY * inv[5] + ndcZ * inv[9] + inv[13]
        pz = ndcX * inv[2] + ndcY * inv[6] + ndcZ * inv[10] + inv[14]
        pw = ndcX * inv[3] + ndcY * inv[7] + ndcZ * inv[11] + inv[15]
        if abs(pw) < 1e-20:
            return None
        return (px / pw, py / pw, pz / pw)

    def rayThrough(self, x, y):
        """(origin, direction) in world space for physical pixel (x, y).

        Built by unprojecting the near and far plane, so it is right for an
        orthographic camera too."""
        near = self.pixelsToWorld(x, y, -1.0)
        far = self.pixelsToWorld(x, y, 1.0)
        if near is None or far is None:
            return None
        direction = _normalize(_sub(far, near))
        if direction == (0.0, 0.0, 0.0):
            return None
        return (near, direction)


def viewProjFromFrustum(frustum):
    """The row-major float[16] a Gf.Frustum projects with (view * proj)."""
    m = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()
    return tuple(m.GetRow(i)[j] for i in range(4) for j in range(4))


def resolve(stageView):
    """A BrushCamera for the StageView's current camera, or None.

    resolveCamera() is the same entry usdview's own draw uses, so this is
    the matrix Hydra rasterised with; computeWindowViewport() is the
    physical pixel rectangle it maps onto."""
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
        return BrushCamera(viewProj, width, height)
    except ValueError:
        return None
