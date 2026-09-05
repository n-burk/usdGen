#!/usr/bin/env python3
"""
Gap G part (c) refined: separate array *construction* from USD authoring, and
compare the three landing places for a freeze:
  1. session layer (in memory, dies with the usdview session)
  2. a sidecar .usdc pulled in as a sublayer of the session layer
  3. a sidecar .usdc pulled in as a payload on a session-layer over
Plus: the cost of dropping an undo stash (deallocating the frozen buffers).
"""
import gc, os, resource, sys, tempfile, time
from pxr import Sdf, Usd, UsdGeom, Vt

N = int(os.environ.get("N_CURVES", "100000"))
CVS = int(os.environ.get("CVS", "8"))
NS = int(os.environ.get("NSAMPLES", "24"))
OUT = os.environ.get("OUTDIR") or tempfile.mkdtemp(prefix="bake2")
os.makedirs(OUT, exist_ok=True)


def t():
    return time.perf_counter()


def rss():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0


print("N=%d CVS=%d NS=%d out=%s" % (N, CVS, NS, OUT))

# --- build the buffers ONCE, outside every timer -------------------------
t0 = t()
base = Vt.Vec3fArray([((i // CVS % 100) * 0.01, (i % CVS) * 0.02,
                       (i // CVS // 100) * 0.01) for i in range(N * CVS)])
print("python array construction (1 sample): %.1f ms" % ((t() - t0) * 1e3))
t0 = t()
samples = [Vt.Vec3fArray([(p[0], p[1] + 0.001 * k, p[2]) for p in base])
           for k in range(NS)]
print("python array construction (%d samples): %.1f ms  rss %.0f MB"
      % (NS, (t() - t0) * 1e3, rss()))
counts = Vt.IntArray([CVS] * N)
widths = Vt.FloatArray([0.002] * (N * CVS))
ids = Vt.IntArray(list(range(N)))
vel = Vt.Vec3fArray([(0.0, 0.1, 0.0)] * (N * CVS))


def author(layer, path, pts=None, ts=None, velocities=False):
    with Sdf.ChangeBlock():
        s = Sdf.CreatePrimInLayer(layer, path)
        s.specifier = Sdf.SpecifierDef
        s.typeName = "BasisCurves"
        def a(n, tn, v=None, interp=None):
            sp = Sdf.AttributeSpec(s, n, tn)
            if v is not None:
                sp.default = v
            if interp:
                sp.SetInfo("interpolation", interp)
        a("points", Sdf.ValueTypeNames.Point3fArray, pts)
        a("curveVertexCounts", Sdf.ValueTypeNames.IntArray, counts)
        a("widths", Sdf.ValueTypeNames.FloatArray, widths, "vertex")
        a("primvars:usdGen:curveId", Sdf.ValueTypeNames.IntArray, ids,
          "uniform")
        a("primvars:rest", Sdf.ValueTypeNames.Point3fArray, base, "vertex")
        if velocities:
            a("velocities", Sdf.ValueTypeNames.Vector3fArray, vel)
    if ts:
        for k, arr in enumerate(ts):
            layer.SetTimeSample(Sdf.Path(path + ".points"), float(k), arr)


rows = []


def measure(tag, fn):
    gc.collect()
    t0 = t()
    size = fn()
    rows.append((tag, (t() - t0) * 1e3, size))


def mk(name):
    p = os.path.join(OUT, name)
    if os.path.exists(p):
        os.remove(p)
    return p


# 1. session layer, static
stage = Usd.Stage.CreateInMemory()
UsdGeom.Xform.Define(stage, "/Groom")
measure("session layer, static (author only)",
        lambda: (author(stage.GetSessionLayer(), "/Groom/F1", base), 0)[1])

# 2. sidecar .usdc, static
def sidecar_static():
    p = mk("f_static.usdc")
    L = Sdf.Layer.CreateNew(p)
    author(L, "/Groom/F1", base)
    L.Save()
    return os.path.getsize(p)
measure("sidecar .usdc static (author+save)", sidecar_static)

# 3. sidecar .usdc, 24 time samples (arrays prebuilt)
def sidecar_anim():
    p = mk("f_anim.usdc")
    L = Sdf.Layer.CreateNew(p)
    author(L, "/Groom/F1", None, samples)
    L.Save()
    return os.path.getsize(p)
measure("sidecar .usdc 24 samples (author+save)", sidecar_anim)

# 4. sidecar .usdc, static + velocities
def sidecar_vel():
    p = mk("f_vel.usdc")
    L = Sdf.Layer.CreateNew(p)
    author(L, "/Groom/F1", base, None, velocities=True)
    L.Save()
    return os.path.getsize(p)
measure("sidecar .usdc static+velocities", sidecar_vel)

print()
print("%-42s %10s %14s" % ("bake", "ms", "bytes"))
for tag, ms, size in rows:
    print("%-42s %10.1f %14s" % (tag, ms, "{:,}".format(size)))

# --- composition cost of the three landing places ------------------------
print()
root = mk("root.usda")
rl = Sdf.Layer.CreateNew(root)
Sdf.CreatePrimInLayer(rl, "/Groom").specifier = Sdf.SpecifierDef
rl.Save()

for mode in ("sublayer", "reference", "payload"):
    st = Usd.Stage.Open(root)
    sess = st.GetSessionLayer()
    t0 = t()
    if mode == "sublayer":
        sess.subLayerPaths.append(os.path.join(OUT, "f_static.usdc"))
    else:
        st.SetEditTarget(Usd.EditTarget(sess))
        over = st.OverridePrim("/Groom/F1")
        arc = (over.GetReferences() if mode == "reference"
               else over.GetPayload() if False else over.GetPayloads())
        arc.AddReference(os.path.join(OUT, "f_static.usdc"), "/Groom/F1") \
            if mode == "reference" else \
            arc.AddPayload(os.path.join(OUT, "f_static.usdc"), "/Groom/F1")
    t1 = t()
    prim = st.GetPrimAtPath("/Groom/F1")
    pts = UsdGeom.BasisCurves(prim).GetPointsAttr().Get() if prim else None
    t2 = t()
    ok = pts is not None and len(pts) == N * CVS
    print("%-12s compose %7.1f ms   first points Get %7.1f ms   ok=%s"
          % (mode, (t1 - t0) * 1e3, (t2 - t1) * 1e3, ok))
    # undo = drop the arc
    t3 = t()
    if mode == "sublayer":
        del sess.subLayerPaths[-1]
    else:
        st.RemovePrim("/Groom/F1")
    t4 = t()
    print("%-12s undo (drop arc)  %7.1f ms   gone=%s"
          % (mode, (t4 - t3) * 1e3, not st.GetPrimAtPath("/Groom/F1")))
    del st
    gc.collect()

# --- cost of dropping the undo stash -------------------------------------
print()
stage2 = Usd.Stage.CreateInMemory()
UsdGeom.Xform.Define(stage2, "/Groom")
sess = stage2.GetSessionLayer()
author(sess, "/Groom/F2", base)
stash = Sdf.Layer.CreateAnonymous("stash")
Sdf.CreatePrimInLayer(stash, "/Groom")
t0 = t()
Sdf.CopySpec(sess, "/Groom/F2", stash, "/Groom/F2")
t1 = t()
rssA = rss()
parent = sess.GetPrimAtPath("/Groom")
del parent.nameChildren["F2"]
t2 = t()
del stash
gc.collect()
t3 = t()
print("undo stash: CopySpec %.2f ms | remove-with-stash-alive %.2f ms | "
      "drop stash (dealloc) %.2f ms | rss after copy %.0f MB"
      % ((t1-t0)*1e3, (t2-t1)*1e3, (t3-t2)*1e3, rssA))
print("peak rss %.0f MB" % rss())
