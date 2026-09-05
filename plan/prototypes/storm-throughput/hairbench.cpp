// hairbench -- counter-accurate Storm hair throughput harness.
//
// This is the half of the benchmark protocol that testusdview cannot do: read
// HdPerfLog counters (there is no pxr.Hd Python module in 26.08, so drawCalls /
// drawBatches / vboRelocated / copyBufferCpuToGpu are unreachable from the
// usdview driver).  It needs a display (garch on Linux is GLX-only in 26.08),
// so it compiles but does not run on a headless host.
//
//   cmake -S . -B build -DCMAKE_PREFIX_PATH=$USD && cmake --build build
//   HD_ENABLE_PERFLOG=1 ./build/hairbench stages/hair_32chunks.usdc [refineLevel]
//
// Counters printed per phase come from pxr/imaging/hd/tokens.h:160-203 and
// pxr/imaging/hdSt/tokens.h:115-121.
#include "pxr/pxr.h"
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/tf/stopwatch.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/glf/contextCaps.h"
#include "pxr/imaging/glf/glContext.h"
#include "pxr/imaging/glf/testGLContext.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdSt/tokens.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/bboxCache.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

static const TfToken kCounters[] = {
    HdPerfTokens->drawCalls,
    HdPerfTokens->drawBatches,
    HdPerfTokens->rebuildBatches,
    HdPerfTokens->dirtyLists,
    HdPerfTokens->dirtyListsRebuilt,
    HdPerfTokens->bufferSourcesResolved,
    HdPerfTokens->sourcesCommitted,
    HdPerfTokens->computationsCommitted,
    HdPerfTokens->bufferArrayRangeMigrated,
    HdPerfTokens->vboRelocated,
    HdPerfTokens->garbageCollected,
    HdPerfTokens->garbageCollectedVbo,
    HdPerfTokens->instBasisCurvesTopology,
    HdPerfTokens->instBasisCurvesTopologyRange,
    HdPerfTokens->instPrimvarRange,
    HdPerfTokens->basisCurvesTopology,
    HdPerfTokens->nonUniformSize,
    HdPerfTokens->ssboSize,
    HdPerfTokens->gpuMemoryUsed,
    HdStPerfTokens->copyBufferCpuToGpu,
    HdStPerfTokens->copyBufferGpuToGpu,
    HdStPerfTokens->drawItemsCacheHit,
    HdStPerfTokens->drawItemsCacheMiss,
    HdStPerfTokens->drawItemsFetched,
};

static void DumpCounters(const char *label) {
    HdPerfLog &log = HdPerfLog::GetInstance();
    printf("--- counters after %s ---\n", label);
    for (TfToken const &t : kCounters) {
        double v = log.GetCounter(t);
        if (v != 0.0) printf("  %-32s %.0f\n", t.GetText(), v);
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: hairbench <stage.usd[c]> [refineLevel]\n"); return 1; }
    const int refineLevel = (argc > 2) ? atoi(argv[2]) : 2;
    const int W = 1920, H = 1080;
    const int WARM = 10, N = 60;

    if (getenv("HD_ENABLE_PERFLOG") == nullptr)
        fprintf(stderr, "WARNING: HD_ENABLE_PERFLOG=1 is not set; all counters "
                        "will read 0 (pxr/imaging/hd/perfLog.cpp:22-27)\n");

    GlfTestGLContext::RegisterGLContextCallbacks();
    GlfGLContext::MakeCurrent(GlfTestGLContext::Create(GlfTestGLContextSharedPtr()));
    GlfContextCaps::InitInstance();
    HdPerfLog::GetInstance().Enable();

    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
    if (!stage) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    size_t nPrims = 0, nCurves = 0, nCVs = 0;
    for (UsdPrim const &p : stage->Traverse()) {
        if (UsdGeomBasisCurves bc = UsdGeomBasisCurves(p)) {
            VtIntArray c; bc.GetCurveVertexCountsAttr().Get(&c);
            if (c.empty()) continue;
            nPrims++; nCurves += c.size();
            for (int v : c) nCVs += v;
        }
    }
    printf("stage=%s prims=%zu curves=%zu cvs=%zu points=%.1f MB refineLevel=%d\n",
           argv[1], nPrims, nCurves, nCVs, nCVs * 12.0 / 1e6, refineLevel);

    UsdImagingGLEngine::Parameters params;
    params.rendererPluginId = TfToken("HdStormRendererPlugin");
    UsdImagingGLEngine engine(params);

    UsdGeomBBoxCache bboxCache(UsdTimeCode::EarliestTime(),
        {UsdGeomTokens->default_, UsdGeomTokens->render, UsdGeomTokens->proxy});
    GfBBox3d bbox = bboxCache.ComputeWorldBound(stage->GetPseudoRoot());
    GfRange3d r = bbox.ComputeAlignedRange();
    GfVec3d c = r.GetMidpoint();
    double d = r.GetSize().GetLength() * 1.6;

    GfCamera cam;
    cam.SetPerspectiveFromAspectRatioAndFieldOfView(
        double(W)/H, 45.0, GfCamera::FOVVertical);
    GfMatrix4d xf(1.0);
    xf.SetLookAt(c + GfVec3d(0, 0, d), c, GfVec3d(0, 1, 0));
    cam.SetTransform(xf.GetInverse());
    engine.SetCameraState(cam.GetFrustum().ComputeViewMatrix(),
                          cam.GetFrustum().ComputeProjectionMatrix());
    engine.SetRenderViewport(GfVec4d(0, 0, W, H));
    engine.SetRendererAov(HdAovTokens->color);

    UsdImagingGLRenderParams rp;
    rp.frame = UsdTimeCode::EarliestTime();
    rp.complexity = 1.0 + 0.1 * refineLevel;   // engine.cpp:2318-2351
    rp.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
    rp.enableLighting = true;
    rp.clearColor = GfVec4f(0.1f, 0.1f, 0.1f, 1.f);

    GlfSimpleLight light;
    light.SetPosition(GfVec4f(float(c[0]), float(c[1] + d), float(c[2] + d), 1.f));
    GlfSimpleMaterial mat;
    engine.SetLightingState({light}, mat, GfVec4f(0.2f, 0.2f, 0.2f, 1.f));

    auto drawOnce = [&]() {
        engine.Render(stage->GetPseudoRoot(), rp);
        while (!engine.IsConverged()) engine.Render(stage->GetPseudoRoot(), rp);
        glFinish();
    };

    for (int i = 0; i < WARM; ++i) drawOnce();
    DumpCounters("warmup");
    HdPerfLog::GetInstance().ResetCounters();

    std::vector<double> ms;
    for (int i = 0; i < N; ++i) {
        TfStopwatch sw; sw.Start(); drawOnce(); sw.Stop();
        ms.push_back(sw.GetMilliseconds());
    }
    std::sort(ms.begin(), ms.end());
    printf("STATIC  median %.3f ms   p90 %.3f   min %.3f  (N=%d)\n",
           ms[N/2], ms[int(N*0.9)], ms[0], N);
    DumpCounters("N static frames (counters are per-N-frames)");

    // ---- deform: step time samples, points-only invalidation ------------
    double t0 = stage->GetStartTimeCode(), t1 = stage->GetEndTimeCode();
    if (t1 > t0) {
        HdPerfLog::GetInstance().ResetCounters();
        ms.clear();
        int span = int(t1 - t0) + 1;
        for (int i = 0; i < N; ++i) {
            rp.frame = UsdTimeCode(t0 + (i % span));
            TfStopwatch sw; sw.Start(); drawOnce(); sw.Stop();
            ms.push_back(sw.GetMilliseconds());
        }
        std::sort(ms.begin(), ms.end());
        printf("DEFORM  median %.3f ms   p90 %.3f   min %.3f\n",
               ms[N/2], ms[int(N*0.9)], ms[0]);
        DumpCounters("N deform frames");
    }

    // ---- live points edit on ONE prim, every frame ----------------------
    UsdGeomBasisCurves first;
    for (UsdPrim const &p : stage->Traverse())
        if ((first = UsdGeomBasisCurves(p))) break;
    if (first) {
        rp.frame = UsdTimeCode::EarliestTime();
        VtVec3fArray base; first.GetPointsAttr().Get(&base);
        HdPerfLog::GetInstance().ResetCounters();
        ms.clear();
        for (int i = 0; i < N; ++i) {
            VtVec3fArray p = base;
            const float dx = 0.002f * ((i % 5) - 2);
            for (GfVec3f &v : p) v[0] += dx;
            TfStopwatch sw; sw.Start();
            first.GetPointsAttr().Set(p);
            drawOnce();
            sw.Stop();
            ms.push_back(sw.GetMilliseconds());
        }
        std::sort(ms.begin(), ms.end());
        printf("EDIT-1  median %.3f ms   p90 %.3f  (points=%zu)\n",
               ms[N/2], ms[int(N*0.9)], base.size());
        DumpCounters("N single-prim point edits");

        // ---- density scrub: topology dirty on ONE prim, every frame -----
        VtIntArray cbase; first.GetCurveVertexCountsAttr().Get(&cbase);
        const int ncv = cbase.empty() ? 8 : cbase[0];
        HdPerfLog::GetInstance().ResetCounters();
        ms.clear();
        for (int i = 0; i < N; ++i) {
            const size_t live =
                std::max<size_t>(1, cbase.size() * (50 + 50 * (i % 4) / 3) / 100);
            VtIntArray cc(live, ncv);
            VtVec3fArray pp(base.cbegin(), base.cbegin() + live * ncv);
            TfStopwatch sw; sw.Start();
            first.GetCurveVertexCountsAttr().Set(cc);
            first.GetPointsAttr().Set(pp);
            drawOnce();
            sw.Stop();
            ms.push_back(sw.GetMilliseconds());
        }
        std::sort(ms.begin(), ms.end());
        printf("SCRUB   median %.3f ms   p90 %.3f\n", ms[N/2], ms[int(N*0.9)]);
        DumpCounters("N topology scrubs");
    }
    return 0;
}
