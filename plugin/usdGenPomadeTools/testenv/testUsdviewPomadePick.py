# testUsdviewPomadePick -- T3: TN-3 pick accuracy. K11 (Pomade_Pick over
# the TubeVert kind) vs StageView.pick at 100 pixels; a disagreement is
# allowed only where K11's hit is occluded (deeper than GL's hit).
#
#   testusdview --testScript plugin/usdGenPomadeTools/testenv/testUsdviewPomadePick.py \
#               <any-stage.usda>
#
# The stage file only anchors the session: the script authors its own
# tube mesh (/TN3/Tube, an exact copy of the model's tube-0
# tessellation) and camera (/TN3Camera) in memory and deactivates every
# other root prim, so GL and K11 observe identical geometry and the
# gate is self-contained. Pixels anchor at projected tube-vert
# positions (90 uniform + 10 deepest-forced-occlusion), so K11 (radius
# 0.5 px) must always hit; GL misses resample. Proven here:
#   * the stage mesh reproduces the model verts bit-exactly;
#   * K11 hits all 100 GL-hit pixels, and its reported distPx matches an
#     independent Gf projection within 0.05 px (pins the viewProj
#     convention against Hydra rather than against itself);
#   * no K11 hit is ever in front of GL's hit surface (2 cm view-space
#     tolerance for chord sagitta + float noise);
#   * 10 far-background pixels miss on both sides.
import ctypes
import math
import os
import random
import sys

failures = 0

PIXELS = 100
BACKGROUND = 10
RADIUS_PX = 0.5
DIST_EPS_PX = 0.05
FRONT_EPS_VIEW = 0.02  # view-space units (tube radius is 0.5)


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def run(stage, view):
    global failures
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if here:
        sys.path.insert(0, os.path.normpath(os.path.join(
            here, "..", "python")))
    try:
        from usdGenPomadeTools import pomadeLib
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools: %s" % exc)
        return 1
    try:
        lib = pomadeLib.Library()
    except OSError as exc:
        print("FAIL: %s" % exc)
        return 1
    dll = lib.dll
    from pxr import Gf, Sdf, UsdGeom, Vt

    rng = random.Random(11)

    # -- model: one straight tube --------------------------------------
    ctx = ctypes.c_void_p(None)
    if dll.Pomade_Create(ctypes.byref(ctx)) != 0:
        print("FAIL: model creates")
        return 1
    if dll.Pomade_BuildTestTube(ctx, 17, 16, 0.5, 4.0) != 0:
        print("FAIL: test tube builds")
        return 1
    nv = dll.Pomade_GetVertexCount(ctx)
    nq = dll.Pomade_GetQuadCount(ctx)
    if nv <= 0 or nv % 16 != 0 or nq != (nv // 16 - 1) * 16:
        print("FAIL: ring-major census (verts=%d quads=%d)" % (nv, nq))
        return 1
    rings = nv // 16
    buf = (ctypes.c_float * (3 * nv))()
    if dll.Pomade_ReadTubePoints(ctx, buf, len(buf)) != 0:
        print("FAIL: tube points read")
        return 1
    verts = [(buf[3 * i], buf[3 * i + 1], buf[3 * i + 2])
             for i in range(nv)]
    print("info: tube-0 mesh is %d verts, %d quads" % (nv, nq))

    # -- stage: the identical mesh + a camera, nothing else pickable ---
    mesh = UsdGeom.Mesh.Define(stage, Sdf.Path("/TN3/Tube"))
    mesh.CreatePointsAttr(Vt.Vec3fArray([Gf.Vec3f(*v) for v in verts]))
    mesh.CreateFaceVertexCountsAttr(Vt.IntArray([4] * nq))
    faces = []
    for r in range(rings - 1):
        for s in range(16):
            s1 = (s + 1) % 16
            faces += [r * 16 + s, r * 16 + s1,
                      (r + 1) * 16 + s1, (r + 1) * 16 + s]
    mesh.CreateFaceVertexIndicesAttr(Vt.IntArray(faces))
    # Subdivision would move the surface off the verts K11 picks.
    mesh.CreateSubdivisionSchemeAttr("none")
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    mesh.CreateExtentAttr(Vt.Vec3fArray([Gf.Vec3f(*lo), Gf.Vec3f(*hi)]))
    back = mesh.GetPointsAttr().Get()
    check(len(back) == nv and all(
        tuple(back[i]) == verts[i] for i in range(nv)),
        "the stage mesh reproduces the model verts bit-exactly")
    for prim in stage.GetPseudoRoot().GetChildren():
        if prim.GetPath() != Sdf.Path("/TN3"):
            prim.SetActive(False)

    cx = sum(v[0] for v in verts) / nv
    cy = sum(v[1] for v in verts) / nv
    cz = sum(v[2] for v in verts) / nv
    extent = max(hi[i] - lo[i] for i in range(3))
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TN3Camera"))
    cam.CreateFocalLengthAttr(50.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
    eye = Gf.Vec3d(cx + 0.45 * extent, cy + 0.12 * extent,
                   cz + 0.89 * extent)
    # Camera-to-world by hand (Gf SetLookAt answers view-style): the
    # camera looks down local -Z at the centroid, +Y up.
    zAxis = (eye - Gf.Vec3d(cx, cy, cz)).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0, 1, 0), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    cam.AddTransformOp().Set(mat)

    # -- camera + viewport + K11 viewProj ------------------------------
    model = view._dataModel
    model.viewSettings.cameraPrim = stage.GetPrimAtPath("/TN3Camera")
    if view.getActiveSceneCamera() is None:
        print("FAIL: the scene camera did not activate")
        return 1
    gfCam, _aspect = view.resolveCamera()
    frustum = gfCam.frustum
    viewMat = frustum.ComputeViewMatrix()
    projMat = frustum.ComputeProjectionMatrix()
    # Gf composes row-vector style (clip^T = p^T*V*P), which is exactly
    # K11's projector (gpu/picking.cu: clip[j] = sum p[i]*M[i*4+j]), so
    # M = V*P flattened row-major feeds Pomade_Pick directly. The
    # projection math below is written with explicit indices (no Gf
    # operator overloading) so the convention is auditable; check (a)
    # pins it against Hydra anyway (a wrong viewProj fails loudly).
    m = viewMat * projMat
    vp = (ctypes.c_float * 16)(*[m.GetRow(i)[j] for i in range(4)
                                 for j in range(4)])
    _vx, _vy, vw, vh = view.computeWindowViewport()
    w, h = int(vw), int(vh)
    check(w >= 200 and h >= 200,
          "viewport is usable (%dx%d)" % (w, h))
    if w < 200 or h < 200:
        return 1
    print("info: viewport %dx%d, focal %.1f" % (w, h, gfCam.focalLength))

    mR = [[m.GetRow(i)[j] for j in range(4)] for i in range(4)]
    vR = [[viewMat.GetRow(i)[j] for j in range(4)] for i in range(4)]

    def project(p):
        x, y, z = p
        cx = x * mR[0][0] + y * mR[1][0] + z * mR[2][0] + mR[3][0]
        cy = x * mR[0][1] + y * mR[1][1] + z * mR[2][1] + mR[3][1]
        cz = x * mR[0][2] + y * mR[1][2] + z * mR[2][2] + mR[3][2]
        cw = x * mR[0][3] + y * mR[1][3] + z * mR[2][3] + mR[3][3]
        if cw <= 0.0:
            return None
        inv = 1.0 / cw
        nx, ny = cx * inv, cy * inv
        return ((nx * 0.5 + 0.5) * w, (1.0 - (ny * 0.5 + 0.5)) * h,
                cz * inv)

    def viewZ(p):
        x, y, z = p
        cz = x * vR[0][2] + y * vR[1][2] + z * vR[2][2] + vR[3][2]
        cw = x * vR[0][3] + y * vR[1][3] + z * vR[2][3] + vR[3][3]
        return cz / cw

    screen = [project(v) for v in verts]
    front = sum(1 for s in screen if s is not None)
    cc = project((cx, cy, cz))
    print("info: %d/%d verts in front; centroid at %s" % (front, nv, cc))
    if any(s is None for s in screen):
        print("FAIL: all verts project in front of the camera")
        return 1

    # -- warmup: GL must be able to hit the tube -----------------------
    tubePath = Sdf.Path("/TN3/Tube")
    glPick = None
    for _ in range(20):
        px = sum(s[0] for s in screen) / nv
        py = sum(s[1] for s in screen) / nv
        inBounds, pickFrustum = view.computePickFrustum(px, py)
        if not inBounds:
            break
        hits = view.pick(pickFrustum)
        if hits:
            glPick = hits
            break
    check(glPick is not None, "GL hits the tube at its centroid")
    if glPick is None:
        return 1

    # -- 100 pixels ----------------------------------------------------
    order = list(range(nv))
    rng.shuffle(order)
    deep = sorted(range(nv), key=lambda i: viewZ(verts[i]))[: max(nv // 10,
                                                                  1)]
    # 90 uniform + 10 forced-deep (occlusion-branch exercise); the pool
    # cycles until 100 GL-hit pixels are collected.
    pool = [order[i % nv] for i in range(PIXELS - 10)]
    pool += [deep[i % len(deep)] for i in range(10)]
    rng.shuffle(pool)
    wanted = []
    guard = 0
    while len(wanted) < PIXELS and guard < 4 * nv:
        vi = pool[guard % len(pool)]
        guard += 1
        px, py, _z = screen[vi]
        if not (0 <= px < w and 0 <= py < h):
            continue
        inBounds, pickFrustum = view.computePickFrustum(px, py)
        if not inBounds or not view.pick(pickFrustum):
            continue
        wanted.append(vi)
    check(len(wanted) >= PIXELS,
          "%d GL-hit pixels collected (want %d)" % (len(wanted), PIXELS))
    if len(wanted) < PIXELS:
        return 1
    hit = ctypes.c_int(0)
    kind = ctypes.c_uint(0)
    index = ctypes.c_int(0)
    sub = ctypes.c_int(0)
    dist = ctypes.c_float(0)
    depth = ctypes.c_float(0)
    agree = 0
    occluded = 0
    maxDistErr = 0.0
    maxFront = 0.0
    for slot, vi in enumerate(wanted):
        # Pre-collected GL-hit pixels: a re-pick must hit identically
        # (deterministic scene, deterministic camera).
        px, py, _z = screen[vi]
        inBounds, pickFrustum = view.computePickFrustum(px, py)
        hits = view.pick(pickFrustum) if inBounds else []
        if not hits:
            check(False, "GL re-pick hits pixel %d" % slot)
            return 1
        if hits[0].hitPrimPath != tubePath:
            check(False, "pixel %d hits the tube (got %s)"
                  % (slot, hits[0].hitPrimPath))
            return 1
        rc = dll.Pomade_Pick(ctx, vp, w, h, ctypes.c_float(px),
                            ctypes.c_float(py), ctypes.c_float(RADIUS_PX),
                            1, ctypes.byref(hit), ctypes.byref(kind),
                            ctypes.byref(index), ctypes.byref(sub),
                            ctypes.byref(dist), ctypes.byref(depth))
        if rc != 0 or not hit.value:
            check(False, "K11 hits vertex-anchored pixel %d" % slot)
            return 1
        ex, ey, _z = screen[index.value]
        distErr = abs(math.hypot(ex - px, ey - py) - dist.value)
        maxDistErr = max(maxDistErr, distErr)
        if distErr > DIST_EPS_PX:
            check(False, "K11 distPx matches Gf (pixel %d err %.4f)"
                  % (slot, distErr))
            return 1
        hp = hits[0].hitPoint
        front = viewZ(verts[index.value]) - viewZ((hp[0], hp[1], hp[2]))
        maxFront = max(maxFront, front)
        if front > FRONT_EPS_VIEW:
            check(False, "pixel %d: K11 %.3f in front of GL"
                  % (slot, front))
            return 1
        if front < -FRONT_EPS_VIEW:
            occluded += 1
        else:
            agree += 1
    check(agree + occluded == PIXELS, "all %d pixels verdicted" % PIXELS)
    print("info: agree %d, occluded-allowed %d, max distPx err %.4f px, "
          "max K11-front %.4f" % (agree, occluded, maxDistErr, maxFront))

    # -- background: both sides miss -----------------------------------
    # The tube fills most of the frame, so corners at several insets.
    bgOK = 0
    candidates = []
    for inset in (0.02, 0.05, 0.10, 0.15, 0.20, 0.25):
        candidates += [(inset, inset), (1 - inset, inset),
                       (inset, 1 - inset), (1 - inset, 1 - inset)]
    for fx, fy in candidates:
        if bgOK >= BACKGROUND:
            break
        px, py = fx * w, fy * h
        inBounds, pickFrustum = view.computePickFrustum(px, py)
        if not inBounds or view.pick(pickFrustum):
            continue
        rc = dll.Pomade_Pick(ctx, vp, w, h, ctypes.c_float(px),
                            ctypes.c_float(py), ctypes.c_float(RADIUS_PX),
                            1, ctypes.byref(hit), ctypes.byref(kind),
                            ctypes.byref(index), ctypes.byref(sub),
                            ctypes.byref(dist), ctypes.byref(depth))
        if rc == 0 and not hit.value:
            bgOK += 1
    check(bgOK >= BACKGROUND, "%d background pixels miss both" % bgOK)

    dll.Pomade_Destroy(ctx)
    print("testUsdviewPomadePick: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    model = getattr(appController, "_dataModel", appController)
    view = getattr(appController, "_stageView", None)
    if view is None:
        print("FAIL: no stage view (TN-3 needs a live usdview)")
        return 1
    return run(model.stage, view)


if __name__ == "__main__":
    # No live view on a direct run; TN-3 is T3-only by nature.
    print("SKIP: testUsdviewPomadePick needs testusdview (no live view here)")
    sys.exit(0)
