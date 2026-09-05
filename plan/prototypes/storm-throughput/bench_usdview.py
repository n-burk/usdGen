"""testusdview driver for the Storm hair throughput benchmark.

RUN (on a workstation with a display; this host has none):

  export USD=/path/to/OpenUSD_install
  export PYTHONPATH=$USD/lib/python3.12/site-packages:$PYTHONPATH
  export HD_ENABLE_PERFLOG=1            # REQUIRED: HdPerfLog is off by default
                                        # (pxr/imaging/hd/perfLog.cpp:22-27)
  export HAIRBENCH_OUT=/tmp/hairbench   # where the json/csv lands
  $USD/bin/testusdview --renderer GL --testScript bench_usdview.py \
      --camera /Cam --viewportSize 1920 1080 stages/hair_32chunks.usdc

  # counter trace (heavy, run separately, one stage at a time):
  TF_DEBUG=HD_COUNTER_CHANGED  ... 2> counters.log
  TF_DEBUG=HDST_DRAW_BATCH     ... 2> batches.log
  TF_DEBUG=HD_RPRIM_UPDATED    ... 2> rprim.log
  TF_DEBUG=HD_DIRTY_LIST       ... 2> dirtylist.log

  # phase breakdown (Resolve / Resize / Reallocate / Copy / Flush inside
  # HdStResourceRegistry::_Commit, pxr/imaging/hdSt/resourceRegistry.cpp:861-1010)
  ... --traceToFile trace.json --traceFormat chrome

Every number this script prints is WALL CLOCK on the calling thread around
StageView.updateGL(), which runs HdEngine::Execute synchronously; add
GL.glFinish() (enabled below) so the GPU is drained before the stop timestamp.
"""
import json, os, sys, time

from pxr import Gf, Sdf, Usd, UsdGeom, Tf, Vt

try:
    from OpenGL import GL
    _HAVE_GL = True
except ImportError:
    _HAVE_GL = False

OUT = os.environ.get("HAIRBENCH_OUT", "/tmp/hairbench")
WARMUP = int(os.environ.get("HAIRBENCH_WARMUP", "10"))
FRAMES = int(os.environ.get("HAIRBENCH_FRAMES", "60"))

# usdview complexity -> HdDisplayStyle refineLevel
# pxr/usdImaging/usdAppUtils/complexityArgs.py:40-43,
# pxr/usdImaging/usdImagingGL/engine.cpp:2318-2351
COMPLEXITIES = [("low", 1.0), ("medium", 1.1), ("high", 1.2), ("veryhigh", 1.3)]


def _finish():
    if _HAVE_GL:
        GL.glFinish()


def _draw(appController):
    sv = appController._stageView
    sv.updateGL()
    _finish()


def timed_frames(appController, n, label, mutate=None):
    """Draw n frames, optionally calling mutate(i) before each. Returns stats."""
    sv = appController._stageView
    for _ in range(WARMUP):
        if mutate:
            mutate(-1)
        _draw(appController)
    samples = []
    for i in range(n):
        if mutate:
            t_edit0 = time.perf_counter()
            mutate(i)
            t_edit1 = time.perf_counter()
        else:
            t_edit0 = t_edit1 = 0.0
        t0 = time.perf_counter()
        _draw(appController)
        t1 = time.perf_counter()
        samples.append(((t1 - t0) * 1000.0, (t_edit1 - t_edit0) * 1000.0))
    draw = sorted(s[0] for s in samples)
    edit = sorted(s[1] for s in samples)
    st = {
        "label": label, "n": n,
        "draw_ms_median": draw[len(draw)//2],
        "draw_ms_min": draw[0], "draw_ms_p90": draw[int(len(draw)*0.9)],
        "edit_ms_median": edit[len(edit)//2],
        # StageView keeps the renderer's own timing for the last frame
        "stageview_renderTime_ms": sv._renderTime * 1000.0,
    }
    print("[hairbench] %-38s draw median %8.3f ms  p90 %8.3f  min %8.3f  | "
          "edit median %7.3f ms" % (label, st["draw_ms_median"], st["draw_ms_p90"],
                                    st["draw_ms_min"], st["edit_ms_median"]),
          flush=True)
    return st


def scene_facts(stage):
    n_prims = n_curves = n_cv = 0
    for p in stage.Traverse():
        if p.IsA(UsdGeom.BasisCurves):
            bc = UsdGeom.BasisCurves(p)
            c = bc.GetCurveVertexCountsAttr().Get()
            if not c:
                continue
            n_prims += 1
            n_curves += len(c)
            n_cv += sum(c)
    return {"basisCurves_prims": n_prims, "curves": n_curves, "cvs": n_cv,
            "points_MB": n_cv * 12 / 1e6}


def render_stats(appController):
    r = appController._stageView._renderer
    try:
        d = r.GetRenderStats()          # HdStRenderDelegate::GetRenderStats
        return {str(k): (float(v) if isinstance(v, (int, float)) else str(v))
                for k, v in d.items()}
    except Exception as e:
        return {"error": str(e)}


def testUsdviewInputFunction(appController):
    os.makedirs(OUT, exist_ok=True)
    dm = appController._dataModel
    stage = dm.stage
    sv = appController._stageView
    results = {"stage": stage.GetRootLayer().identifier,
               "facts": scene_facts(stage),
               "renderer": sv.rendererDisplayName if hasattr(sv, "rendererDisplayName") else "?",
               "runs": []}
    print("[hairbench] %s" % json.dumps(results["facts"]), flush=True)

    # frame the hair once so every measurement uses the same camera
    appController._resetView()
    sv.updateView(resetCam=True, forceComputeBBox=True)
    _draw(appController)

    # ---- 1. static draw cost vs refineLevel -------------------------------
    # RefinementComplexities.{LOW,MEDIUM,HIGH,VERY_HIGH} -> complexity value
    # 1.0/1.1/1.2/1.3 -> HdDisplayStyle refineLevel 0/1/2/3
    # (pxr/usdImaging/usdAppUtils/complexityArgs.py:40-43,
    #  pxr/usdImaging/usdImagingGL/engine.cpp:2318-2351)
    from pxr.UsdAppUtils.complexityArgs import RefinementComplexities as RC
    for comp, refine in ((RC.LOW, 0), (RC.MEDIUM, 1), (RC.HIGH, 2),
                         (RC.VERY_HIGH, 3)):
        appController._setComplexity(comp)
        _draw(appController)
        st = timed_frames(appController, FRAMES,
                          "static complexity=%s (refineLevel %d)" % (comp.id, refine))
        st["refineLevel"] = refine
        st["renderStats"] = render_stats(appController)
        results["runs"].append(st)

    # back to a fixed complexity for the update tests: refineLevel 2 = the
    # ribbon/ROUND shader key (hdSt/basisCurves.cpp:329-340)
    appController._setComplexity(RC.HIGH)
    _draw(appController)

    # ---- 2. deform: step time samples (points-only dirty) -----------------
    t0c, t1c = stage.GetStartTimeCode(), stage.GetEndTimeCode()
    if t1c > t0c:
        span = int(t1c - t0c) + 1
        def step(i):
            dm.currentFrame = Usd.TimeCode(t0c + (i % span))
        st = timed_frames(appController, FRAMES, "deform (time samples)", mutate=step)
        results["runs"].append(st)

    # ---- 3. live points edit: author points on chunk 0 every frame --------
    curves = [p for p in stage.Traverse() if p.IsA(UsdGeom.BasisCurves)]
    if curves:
        bc0 = UsdGeom.BasisCurves(curves[0])
        base = Vt.Vec3fArray(bc0.GetPointsAttr().Get())
        n = len(base)
        def jiggle(i):
            d = 0.002 * ((i % 5) - 2)
            bc0.GetPointsAttr().Set(
                Vt.Vec3fArray([Gf.Vec3f(p[0] + d, p[1], p[2]) for p in base]))
        st = timed_frames(appController, min(FRAMES, 30),
                          "live points edit, 1 prim (%d pts)" % n, mutate=jiggle)
        results["runs"].append(st)

        # all prims every frame -- the worst case for a groom operator
        bases = [(UsdGeom.BasisCurves(p),
                  Vt.Vec3fArray(UsdGeom.BasisCurves(p).GetPointsAttr().Get()))
                 for p in curves]
        def jiggle_all(i):
            d = 0.002 * ((i % 5) - 2)
            for bc, b in bases:
                bc.GetPointsAttr().Set(
                    Vt.Vec3fArray([Gf.Vec3f(p[0] + d, p[1], p[2]) for p in b]))
        st = timed_frames(appController, min(FRAMES, 20),
                          "live points edit, ALL %d prims" % len(curves),
                          mutate=jiggle_all)
        results["runs"].append(st)

        # ---- 4. displayColor-only edit (DirtyPrimvar, no DirtyPoints) -----
        pv0 = UsdGeom.PrimvarsAPI(bc0).GetPrimvar("displayColor")
        if pv0:
            cbase = Vt.Vec3fArray(pv0.Get())
            def recolor(i):
                f = 0.5 + 0.5 * ((i % 4) / 3.0)
                pv0.Set(Vt.Vec3fArray([Gf.Vec3f(c[0]*f, c[1], c[2]) for c in cbase]))
            st = timed_frames(appController, min(FRAMES, 30),
                              "live displayColor edit, 1 prim", mutate=recolor)
            results["runs"].append(st)

        # ---- 5. density scrub: TOPOLOGY change every frame ----------------
        cnt0 = Vt.IntArray(bc0.GetCurveVertexCountsAttr().Get())
        ncv = cnt0[0] if len(cnt0) else 8
        ncur = len(cnt0)
        def scrub(i):
            live = max(1, int(ncur * (0.5 + 0.5 * ((i % 4) / 3.0))))
            bc0.GetCurveVertexCountsAttr().Set(Vt.IntArray([ncv] * live))
            bc0.GetPointsAttr().Set(Vt.Vec3fArray(list(base)[:live * ncv]))
        st = timed_frames(appController, min(FRAMES, 20),
                          "density scrub (topology dirty), 1 prim", mutate=scrub)
        results["runs"].append(st)

    results["finalRenderStats"] = render_stats(appController)
    name = os.path.basename(stage.GetRootLayer().identifier).replace(".usdc", "")
    path = os.path.join(OUT, "hairbench_%s.json" % name)
    with open(path, "w") as f:
        json.dump(results, f, indent=2)
    print("[hairbench] wrote %s" % path, flush=True)
