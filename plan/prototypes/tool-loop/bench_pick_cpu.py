"""CPU screen-space CV picking: project every guide CV with numpy and take the
nearest to the cursor.  This is the route that needs no GL and no HdxPickTask.
Mirrors usdRig gizmoSnap.NearestPoint / curvenetUI._NearestControlPoint but
vectorised."""
import sys, time
import numpy as np
from pxr import Gf

N = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
W, H = 1920, 1080
rng = np.random.default_rng(0)
pts = (rng.random((N, 3), dtype=np.float32) * 2.0 - 1.0).astype(np.float32)

cam = Gf.Camera()
cam.SetPerspectiveFromAspectRatioAndFieldOfView(W / float(H), 60.0,
                                                Gf.Camera.FOVHorizontal)
xf = Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0, 0, 5))
cam.transform = xf
frustum = cam.frustum
view = np.array(frustum.ComputeViewMatrix(), dtype=np.float64).reshape(4, 4)
proj = np.array(frustum.ComputeProjectionMatrix(), dtype=np.float64).reshape(4, 4)
vp = (view @ proj).astype(np.float32)          # USD is row-vector: p * V * P

def bench(label, fn, reps=51):
    fn()
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter(); r = fn(); ts.append(time.perf_counter() - t0)
    ts.sort()
    print("  %-56s best %8.2f us  median %8.2f us"
          % (label, ts[0]*1e6, ts[len(ts)//2]*1e6), flush=True)
    return r

cursor = np.array([W * 0.5, H * 0.5], dtype=np.float32)
RADIUS = 12.0 * 2.0     # SNAP_PIXELS=12 logical, dpr 2 -> physical

def project():
    h = np.empty((N, 4), dtype=np.float32)
    h[:, :3] = pts; h[:, 3] = 1.0
    clip = h @ vp
    w = clip[:, 3]
    ndc = clip[:, :3] / w[:, None]
    sx = (ndc[:, 0] * 0.5 + 0.5) * W
    sy = (1.0 - (ndc[:, 1] * 0.5 + 0.5)) * H
    return sx, sy, ndc[:, 2], w

def nearest_full():
    sx, sy, z, w = project()
    dx = sx - cursor[0]; dy = sy - cursor[1]
    d2 = dx*dx + dy*dy
    d2[(w <= 0) | (z < -1) | (z > 1)] = np.inf
    i = int(np.argmin(d2))
    return (i, float(np.sqrt(d2[i]))) if d2[i] < RADIUS*RADIUS else None

def footprint():
    sx, sy, z, w = project()
    dx = sx - cursor[0]; dy = sy - cursor[1]
    m = (dx*dx + dy*dy < RADIUS*RADIUS*100) & (w > 0)
    return np.flatnonzero(m)

print("N = %d CVs, viewport %dx%d" % (N, W, H))
bench("project all CVs to pixels (numpy)", project)
r = bench("nearest CV to the cursor (project + argmin)", nearest_full)
print("        nearest:", r)
f = bench("brush footprint mask (radius %d px)" % int(RADIUS*10), footprint)
print("        footprint size:", len(f))

# incremental: cache the projection while the camera does not move
sx, sy, z, w = project()
def nearest_cached():
    dx = sx - cursor[0]; dy = sy - cursor[1]
    d2 = dx*dx + dy*dy
    return int(np.argmin(d2))
bench("nearest CV with a CACHED projection (camera static)", nearest_cached)

# every-Nth-CV subsample (guides only: root+tip of each curve)
CVPC = 10
roots = pts[::CVPC]
def nearest_roots():
    h = np.empty((roots.shape[0], 4), dtype=np.float32)
    h[:, :3] = roots; h[:, 3] = 1.0
    clip = h @ vp
    ndc = clip[:, :3] / clip[:, 3:4]
    sx2 = (ndc[:, 0]*0.5+0.5)*W; sy2 = (1.0-(ndc[:, 1]*0.5+0.5))*H
    d2 = (sx2-cursor[0])**2 + (sy2-cursor[1])**2
    return int(np.argmin(d2))
bench("nearest guide ROOT only (N/%d points)" % CVPC, nearest_roots)

# pure python fallback for the same job
pl = [tuple(map(float, p)) for p in pts[:min(N, 100000)]]
m = [list(vp[i]) for i in range(4)]
def nearest_pure():
    best = 1e30; bi = -1
    cx, cy = float(cursor[0]), float(cursor[1])
    for i, (x, y, zz) in enumerate(pl):
        cw = x*m[0][3] + y*m[1][3] + zz*m[2][3] + m[3][3]
        if cw <= 0: continue
        px = (x*m[0][0] + y*m[1][0] + zz*m[2][0] + m[3][0]) / cw
        py = (x*m[0][1] + y*m[1][1] + zz*m[2][1] + m[3][1]) / cw
        sxp = (px*0.5+0.5)*W; syp = (1.0-(py*0.5+0.5))*H
        d = (sxp-cx)**2 + (syp-cy)**2
        if d < best: best = d; bi = i
    return bi
bench("nearest CV, PURE PYTHON loop (no numpy)", nearest_pure, reps=3)
