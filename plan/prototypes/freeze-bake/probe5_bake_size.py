#!/usr/bin/env python3
"""
Gap G part (c): where does frozen data land, and what does it cost?
100k curves x 8 CVs, with and without 24 time samples of points, written to
.usdc / .usda / in-memory session layer; measures authoring time, Export time,
file size, and reopen+read-back time.
"""
import os, gc, resource, shutil, sys, tempfile, time
from pxr import Sdf, Usd, UsdGeom, Vt

N = int(os.environ.get("N_CURVES", "100000"))
CVS = int(os.environ.get("CVS", "8"))
NSAMPLES = int(os.environ.get("NSAMPLES", "24"))
OUT = os.environ.get("OUTDIR") or tempfile.mkdtemp(prefix="bake")


def rss():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0


def arrays(n, cvs):
    pts = Vt.Vec3fArray([( (i//cvs % 100)*0.01, (i%cvs)*0.02, (i//cvs//100)*0.01)
                         for i in range(n*cvs)])
    return dict(
        points=pts,
        counts=Vt.IntArray([cvs]*n),
        widths=Vt.FloatArray([0.002]*(n*cvs)),
        ids=Vt.IntArray(list(range(n))),
        skinprim=Vt.IntArray([i % 977 for i in range(n)]),
        skinuv=Vt.Vec2fArray([((i%31)/31.0, (i%17)/17.0) for i in range(n)]))


def author(layer, path, A, timeSamples, includeRest=True):
    with Sdf.ChangeBlock():
        spec = Sdf.CreatePrimInLayer(layer, path)
        spec.specifier = Sdf.SpecifierDef
        spec.typeName = "BasisCurves"
        def a(name, tn, val=None, interp=None):
            s = Sdf.AttributeSpec(spec, name, tn)
            if val is not None:
                s.default = val
            if interp:
                s.SetInfo("interpolation", interp)
            return s
        pspec = a("points", Sdf.ValueTypeNames.Point3fArray,
                  None if timeSamples else A["points"])
        a("curveVertexCounts", Sdf.ValueTypeNames.IntArray, A["counts"])
        a("widths", Sdf.ValueTypeNames.FloatArray, A["widths"], "vertex")
        a("type", Sdf.ValueTypeNames.Token, "cubic")
        a("basis", Sdf.ValueTypeNames.Token, "bspline")
        a("wrap", Sdf.ValueTypeNames.Token, "pinned")
        a("primvars:usdGen:curveId", Sdf.ValueTypeNames.IntArray, A["ids"],
          "uniform")
        a("primvars:skinprim", Sdf.ValueTypeNames.IntArray, A["skinprim"],
          "uniform")
        a("primvars:skinprimuv", Sdf.ValueTypeNames.TexCoord2fArray,
          A["skinuv"], "uniform")
        if includeRest:
            a("primvars:rest", Sdf.ValueTypeNames.Point3fArray, A["points"],
              "vertex")
    if timeSamples:
        base = A["points"]
        for t in range(NSAMPLES):
            shifted = Vt.Vec3fArray([(p[0], p[1] + 0.001*t, p[2]) for p in base])
            layer.SetTimeSample(Sdf.Path(path + ".points"), float(t), shifted)


def run(fmt, timeSamples, includeRest=True, tag=""):
    A = arrays(N, CVS)
    name = "bake_%s%s%s.%s" % (
        "anim" if timeSamples else "static",
        "" if includeRest else "_norest", tag, fmt)
    p = os.path.join(OUT, name)
    gc.collect()
    t0 = time.perf_counter()
    layer = Sdf.Layer.CreateNew(p)
    author(layer, "/Groom/Frozen", A, timeSamples, includeRest)
    t1 = time.perf_counter()
    layer.Save()
    t2 = time.perf_counter()
    size = os.path.getsize(p)
    del layer, A
    gc.collect()
    t3 = time.perf_counter()
    l2 = Sdf.Layer.FindOrOpen(p)
    st = Usd.Stage.Open(p)
    pts = UsdGeom.BasisCurves(
        st.GetPrimAtPath("/Groom/Frozen")).GetPointsAttr().Get(0.0)
    t4 = time.perf_counter()
    assert len(pts) == N*CVS, len(pts)
    return dict(name=name, author_ms=(t1-t0)*1e3, save_ms=(t2-t1)*1e3,
                size=size, reopen_ms=(t4-t3)*1e3)


def main():
    print("N=%d CVS=%d samples=%d  outdir=%s" % (N, CVS, NSAMPLES, OUT))
    rows = []
    for fmt in ("usdc", "usda"):
        for ts in (False, True):
            if fmt == "usda" and ts and N > 20000:
                continue     # usda + 24 samples of 800k points is minutes
            rows.append(run(fmt, ts))
    rows.append(run("usdc", False, includeRest=False))
    print()
    print("%-30s %10s %10s %14s %12s" %
          ("bake", "author ms", "save ms", "bytes", "reopen ms"))
    for r in rows:
        print("%-30s %10.1f %10.1f %14s %12.1f" %
              (r["name"], r["author_ms"], r["save_ms"],
               "{:,}".format(r["size"]), r["reopen_ms"]))
    print("\npeak rss %.1f MB" % rss())

    # in-memory session-layer freeze, for comparison: no file at all
    A = arrays(N, CVS)
    stage = Usd.Stage.CreateInMemory()
    UsdGeom.Xform.Define(stage, "/Groom")
    t0 = time.perf_counter()
    author(stage.GetSessionLayer(), "/Groom/Frozen", A, False)
    t1 = time.perf_counter()
    ident = stage.GetSessionLayer().ExportToString()
    t2 = time.perf_counter()
    print("session-layer freeze: author %.1f ms, "
          "ExportToString %.1f ms / %s bytes (usda text)"
          % ((t1-t0)*1e3, (t2-t1)*1e3, "{:,}".format(len(ident))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
