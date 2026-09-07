#include "eglctx.h"
#include "pxr/pxr.h"
#include "pxr/base/tf/stopwatch.h"
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/glf/simpleLight.h"
#include "pxr/imaging/glf/simpleMaterial.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char** argv)
{
    // argv: scene camPath width height complexity frames deformFlag
    if (argc < 3) { fprintf(stderr, "usage: %s scene.usda camPath [w h complexity frames deform]\n", argv[0]); return 2; }
    const int W  = argc > 3 ? atoi(argv[3]) : 1280;
    const int H  = argc > 4 ? atoi(argv[4]) : 720;
    const float cx = argc > 5 ? (float)atof(argv[5]) : 1.5f;
    const int frames = argc > 6 ? atoi(argv[6]) : 60;
    const bool deform = argc > 7 ? atoi(argv[7]) != 0 : false;

    if (!eglctx::MakeHeadlessGLContext()) return 3;
    GarchGLApiLoad();

    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
    if (!stage) return 4;

    UsdImagingGLEngine::Parameters p;
    p.rendererPluginId = TfToken("HdStormRendererPlugin");
    UsdImagingGLEngine engine(p);
    engine.SetEnablePresentation(false);
    engine.SetRendererAov(HdAovTokens->color);
    engine.SetRenderBufferSize(GfVec2i(W, H));
    engine.SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0,0), W, H)));

    UsdGeomCamera usdCam(stage->GetPrimAtPath(SdfPath(argv[2])));
    GfCamera gfCam = usdCam.GetCamera(UsdTimeCode::EarliestTime());
    GfFrustum frustum = gfCam.GetFrustum();
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());

    GlfSimpleLightVector lights;
    GlfSimpleLight l(GfVec4f(frustum.GetPosition()[0], frustum.GetPosition()[1],
                             frustum.GetPosition()[2], 1.0f));
    l.SetTransform(frustum.ComputeViewInverse());
    l.SetAmbient(GfVec4f(0.01f,0.01f,0.01f,1.0f));
    lights.push_back(l);
    GlfSimpleMaterial mat;
    engine.SetLightingState(lights, mat, GfVec4f(0.01f,0.01f,0.01f,1.0f));

    UsdImagingGLRenderParams rp;
    rp.frame = UsdTimeCode::EarliestTime();
    rp.complexity = cx;
    rp.clearColor = GfVec4f(0,0,0,1);
    rp.enableLighting = true;

    UsdGeomBasisCurves curves(stage->GetPrimAtPath(SdfPath("/World/Hair")));
    UsdAttribute pointsAttr = curves.GetPointsAttr();
    VtVec3fArray basePts;
    size_t numCurves = 0, numCVs = 0;
    if (curves) {
        pointsAttr.Get(&basePts);
        VtIntArray counts; curves.GetCurveVertexCountsAttr().Get(&counts);
        numCurves = counts.size(); numCVs = basePts.size();
    }

    const UsdPrim& root = stage->GetPseudoRoot();
    // warmup
    for (int i = 0; i < 8; ++i) { engine.Render(root, rp); }
    glFinish();

    TfStopwatch sw;
    VtVec3fArray pts = basePts;
    for (int f = 0; f < frames; ++f) {
        if (deform) {
            const float t = 0.02f * f;
            for (size_t i = 0; i < pts.size(); ++i) {
                pts[i] = basePts[i] + GfVec3f(0.01f*sinf(t + basePts[i][1]*3.0f), 0.0f, 0.0f);
            }
        }
        sw.Start();
        if (deform) { pointsAttr.Set(pts); }
        engine.Render(root, rp);
        glFinish();
        sw.Stop();
    }
    const double ms = sw.GetSeconds() * 1000.0 / frames;
    printf("RESULT scene=%s res=%dx%d complexity=%.1f curves=%zu cvs=%zu deform=%d "
           "frames=%d ms_per_frame=%.3f fps=%.1f\n",
           argv[1], W, H, cx, numCurves, numCVs, (int)deform, frames, ms, 1000.0/ms);
    return 0;
}
