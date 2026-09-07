// PW-3 (S-9) driver: refineLevel 2 <-> 1 toggle via renderParams.complexity
// (1.2 -> refine 2, 1.1 -> refine 1; usdImagingGL/engine.cpp:2317-2350).
//
// Modes:
//   steady  : fixed complexity for all frames
//   toggle  : complexity = hi for frames [0,K), lo for [K,2K), hi for [2K,3K), ...
//             (K = toggleEvery; default 10). Per-frame ms printed on stdout.
//
// usage: bench_refine mode scene.usda camPath [w h cx_hi cx_lo toggleEvery frames deform]
#include "eglctx.h"
#include "pxr/pxr.h"
#include "pxr/base/tf/stopwatch.h"
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/glf/simpleLight.h"
#include "pxr/imaging/glf/simpleMaterial.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s {steady|toggle} scene.usda camPath [w h cx_hi cx_lo toggleEvery frames deform]\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    const int W   = argc > 4 ? atoi(argv[4]) : 1280;
    const int H   = argc > 5 ? atoi(argv[5]) : 720;
    const float cxHi = argc > 6 ? (float)atof(argv[6]) : 1.2f;
    const float cxLo = argc > 7 ? (float)atof(argv[7]) : 1.1f;
    const int every  = argc > 8 ? atoi(argv[8]) : 10;
    const int frames = argc > 9 ? atoi(argv[9]) : 60;
    const bool deform = argc > 10 ? atoi(argv[10]) != 0 : false;

    if (!eglctx::MakeHeadlessGLContext()) return 3;
    GarchGLApiLoad();
    printf("GL_VERSION=%s\n", (const char*)glGetString(GL_VERSION));

    TfStopwatch openSw; openSw.Start();
    UsdStageRefPtr stage = UsdStage::Open(argv[2]);
    if (!stage) return 4;
    openSw.Stop();
    const double openMs = openSw.GetSeconds() * 1000.0;

    UsdImagingGLEngine::Parameters p;
    p.rendererPluginId = TfToken("HdStormRendererPlugin");
    UsdImagingGLEngine engine(p);
    engine.SetEnablePresentation(false);
    engine.SetRendererAov(HdAovTokens->color);
    engine.SetRenderBufferSize(GfVec2i(W, H));
    engine.SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), W, H)));

    UsdGeomCamera usdCam(stage->GetPrimAtPath(SdfPath(argv[3])));
    GfCamera gfCam = usdCam.GetCamera(UsdTimeCode::EarliestTime());
    GfFrustum frustum = gfCam.GetFrustum();
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());

    GlfSimpleLightVector lights;
    GlfSimpleLight l(GfVec4f(frustum.GetPosition()[0], frustum.GetPosition()[1],
                             frustum.GetPosition()[2], 1.0f));
    l.SetTransform(frustum.ComputeViewInverse());
    l.SetAmbient(GfVec4f(0.01f, 0.01f, 0.01f, 1.0f));
    lights.push_back(l);
    GlfSimpleMaterial mat;
    engine.SetLightingState(lights, mat, GfVec4f(0.01f, 0.01f, 0.01f, 1.0f));

    UsdImagingGLRenderParams rp;
    rp.frame = UsdTimeCode::EarliestTime();
    rp.clearColor = GfVec4f(0, 0, 0, 1);
    rp.enableLighting = true;

    UsdGeomBasisCurves curves(stage->GetPrimAtPath(SdfPath("/World/Hair")));
    UsdAttribute pointsAttr;
    VtVec3fArray basePts;
    size_t numCurves = 0, numCVs = 0;
    if (curves) {
        pointsAttr = curves.GetPointsAttr();
        pointsAttr.Get(&basePts);
        VtIntArray counts; curves.GetCurveVertexCountsAttr().Get(&counts);
        numCurves = counts.size(); numCVs = basePts.size();
    }
    printf("SCENE mode=%s curves=%zu cvs=%zu stageOpen=%.1fms\n", mode.c_str(),
           numCurves, numCVs, openMs);
    glFinish();

    const UsdPrim& root = stage->GetPseudoRoot();
    for (int f = 0; f < frames; ++f) {
        const float cx = (mode == "toggle")
            ? ((f / every) % 2 == 0 ? cxHi : cxLo)
            : cxHi;
        rp.complexity = cx;
        TfStopwatch sw; sw.Start();
        if (deform && f > 0) {
            const float t = 0.02f * f;
            VtVec3fArray pts(basePts);
            for (size_t i = 0; i < pts.size(); ++i) {
                pts[i] = basePts[i] + GfVec3f(0.01f * sinf(t + basePts[i][1] * 3.0f), 0.0f, 0.0f);
            }
            pointsAttr.Set(pts);
        }
        engine.Render(root, rp);
        glFinish();
        sw.Stop();
        printf("F=%03d cx=%.1f ms=%.3f\n", f, cx, sw.GetSeconds() * 1000.0);
    }
    return 0;
}
