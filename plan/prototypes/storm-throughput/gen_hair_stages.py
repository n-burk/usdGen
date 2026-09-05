#!/usr/bin/env python3
"""Generate the Storm hair throughput benchmark stages.

Produces, under --out:
  hair_1prim.usdc            100k x 8-CV cubic/bspline curves in ONE BasisCurves prim
  hair_32chunks.usdc         same curves split across 32 BasisCurves prims
  hair_1000prims.usdc        same curves split across 1000 BasisCurves prims
  hair_32chunks_anim.usdc    32 chunks, `points` time-sampled over N frames (deform)
  hair_32chunks_density.usdc 32 chunks + a `density` variant layer set that changes
                             curveVertexCounts/points length (topology scrub)
  hair_1prim_linear.usdc     1 prim, linear curves (index-buffer comparison)

Every stage uses the SAME curve data so the only variable is prim granularity.
Each prim authors `extent` (required for GPU frustum culling: without it
HdStPopulateConstantPrimvars leaves the bbox at [FLT_MAX,-FLT_MAX] and culling is
disabled -- pxr/imaging/hdSt/primUtils.cpp:887-891).

Run:
  PYTHONPATH=<usd>/lib/python3.12/site-packages python3 gen_hair_stages.py --out ./stages
"""
import argparse, math, os, random, sys, time

from pxr import Usd, UsdGeom, Sdf, Gf, Vt


def make_curves(n_curves, n_cv, seed=0, t=0.0):
    """Return (points, widths_vertex, displayColor_uniform) for n_curves strands
    growing off a unit-square 'scalp' in +Y, with a time-varying bend."""
    rng = random.Random(seed)
    pts = [None] * (n_curves * n_cv)
    cols = [None] * n_curves
    k = 0
    side = int(math.sqrt(n_curves)) + 1
    for c in range(n_curves):
        # deterministic root on a grid + jitter
        gx = (c % side) / float(side) - 0.5
        gz = (c // side) / float(side) - 0.5
        jx = (rng.random() - 0.5) * (1.0 / side)
        jz = (rng.random() - 0.5) * (1.0 / side)
        rx, rz = gx + jx, gz + jz
        length = 0.25 + 0.15 * rng.random()
        bend = 0.35 * math.sin(t * 2.0 * math.pi + c * 0.01)
        for i in range(n_cv):
            u = i / float(n_cv - 1)
            pts[k] = Gf.Vec3f(rx + bend * u * u,
                              u * length,
                              rz + 0.1 * bend * u)
            k += 1
        cols[c] = Gf.Vec3f(0.25 + 0.4 * rng.random(),
                           0.15 + 0.2 * rng.random(),
                           0.08 + 0.1 * rng.random())
    return Vt.Vec3fArray(pts), Vt.Vec3fArray(cols)


def extent_of(points):
    mn = Gf.Vec3f(1e30, 1e30, 1e30)
    mx = Gf.Vec3f(-1e30, -1e30, -1e30)
    for p in points:
        for a in range(3):
            if p[a] < mn[a]: mn[a] = p[a]
            if p[a] > mx[a]: mx[a] = p[a]
    return Vt.Vec3fArray([mn, mx])


def author_chunk(stage, path, points, cols, n_curves, n_cv, curve_type,
                 width=0.0015, time_samples=None):
    bc = UsdGeom.BasisCurves.Define(stage, path)
    bc.CreateTypeAttr().Set(curve_type)
    if curve_type == "cubic":
        bc.CreateBasisAttr().Set("bspline")
    bc.CreateWrapAttr().Set("nonperiodic")
    bc.CreateCurveVertexCountsAttr().Set(Vt.IntArray([n_cv] * n_curves))
    # constant width keeps the vertex BAR to points-only; see report section on
    # varying-primvar expansion cost.
    bc.CreateWidthsAttr().Set(Vt.FloatArray([width]))
    bc.GetWidthsAttr().SetMetadata("interpolation", "constant")
    dc = bc.CreateDisplayColorPrimvar("uniform")
    dc.Set(cols)
    if time_samples is None:
        bc.CreatePointsAttr().Set(points)
        bc.CreateExtentAttr().Set(extent_of(points))
    else:
        pa = bc.CreatePointsAttr()
        ea = bc.CreateExtentAttr()
        for (tc, pts) in time_samples:
            pa.Set(pts, tc)
            ea.Set(extent_of(pts), tc)
    return bc


def build(out, n_curves, n_cv, chunk_specs, frames):
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    print("generating %d curves x %d CV ..." % (n_curves, n_cv), flush=True)
    points, cols = make_curves(n_curves, n_cv, seed=1, t=0.0)
    print("  base data built in %.1fs" % (time.time() - t0), flush=True)

    for (name, nchunks, curve_type) in chunk_specs:
        p = os.path.join(out, name)
        st = Usd.Stage.CreateNew(p) if not os.path.exists(p) else Usd.Stage.Open(p)
        st.GetRootLayer().Clear()
        UsdGeom.SetStageUpAxis(st, UsdGeom.Tokens.y)
        root = UsdGeom.Xform.Define(st, "/Hair")
        st.SetDefaultPrim(root.GetPrim())
        per = n_curves // nchunks
        for c in range(nchunks):
            lo, hi = c * per, (c + 1) * per if c < nchunks - 1 else n_curves
            n = hi - lo
            sub = Vt.Vec3fArray(list(points[lo * n_cv:hi * n_cv]))
            subc = Vt.Vec3fArray(list(cols[lo:hi]))
            author_chunk(st, "/Hair/chunk_%04d" % c, sub, subc, n, n_cv, curve_type)
        st.GetRootLayer().Save()
        print("  wrote %s (%d prims, %.1f MB)" %
              (p, nchunks, os.path.getsize(p) / 1e6), flush=True)

    # animated variant: 32 chunks, points time-sampled
    p = os.path.join(out, "hair_32chunks_anim.usdc")
    st = Usd.Stage.CreateNew(p) if not os.path.exists(p) else Usd.Stage.Open(p)
    st.GetRootLayer().Clear()
    UsdGeom.SetStageUpAxis(st, UsdGeom.Tokens.y)
    root = UsdGeom.Xform.Define(st, "/Hair")
    st.SetDefaultPrim(root.GetPrim())
    st.SetStartTimeCode(1); st.SetEndTimeCode(frames)
    st.SetTimeCodesPerSecond(24)
    nchunks = 32
    per = n_curves // nchunks
    # precompute the deformed frames once for the whole scalp
    allframes = []
    for f in range(frames):
        pts, _ = make_curves(n_curves, n_cv, seed=1, t=f / float(frames))
        allframes.append((f + 1, pts))
        print("    frame %d/%d" % (f + 1, frames), flush=True)
    for c in range(nchunks):
        lo, hi = c * per, (c + 1) * per if c < nchunks - 1 else n_curves
        n = hi - lo
        ts = [(tc, Vt.Vec3fArray(list(pp[lo * n_cv:hi * n_cv])))
              for (tc, pp) in allframes]
        subc = Vt.Vec3fArray(list(cols[lo:hi]))
        author_chunk(st, "/Hair/chunk_%04d" % c, None, subc, n, n_cv, "cubic",
                     time_samples=ts)
    st.GetRootLayer().Save()
    print("  wrote %s (%d prims, %d frames, %.1f MB)" %
          (p, nchunks, frames, os.path.getsize(p) / 1e6), flush=True)

    # density scrub variant: 32 chunks at 3 densities, as separate root layers
    for pct in (100, 50, 25):
        p = os.path.join(out, "hair_32chunks_density%03d.usdc" % pct)
        st = Usd.Stage.CreateNew(p) if not os.path.exists(p) else Usd.Stage.Open(p)
        st.GetRootLayer().Clear()
        UsdGeom.SetStageUpAxis(st, UsdGeom.Tokens.y)
        root = UsdGeom.Xform.Define(st, "/Hair")
        st.SetDefaultPrim(root.GetPrim())
        live = n_curves * pct // 100
        per = live // 32
        for c in range(32):
            lo, hi = c * per, (c + 1) * per if c < 31 else live
            n = hi - lo
            sub = Vt.Vec3fArray(list(points[lo * n_cv:hi * n_cv]))
            subc = Vt.Vec3fArray(list(cols[lo:hi]))
            author_chunk(st, "/Hair/chunk_%04d" % c, sub, subc, n, n_cv, "cubic")
        st.GetRootLayer().Save()
        print("  wrote %s (%d%% density, %.1f MB)" %
              (p, pct, os.path.getsize(p) / 1e6), flush=True)

    print("total %.1fs" % (time.time() - t0))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="./stages")
    ap.add_argument("--curves", type=int, default=100000)
    ap.add_argument("--cv", type=int, default=8)
    ap.add_argument("--frames", type=int, default=8)
    a = ap.parse_args()
    build(a.out, a.curves, a.cv,
          [("hair_1prim.usdc",        1,    "cubic"),
           ("hair_32chunks.usdc",     32,   "cubic"),
           ("hair_1000prims.usdc",    1000, "cubic"),
           ("hair_1prim_linear.usdc", 1,    "linear")],
          a.frames)
