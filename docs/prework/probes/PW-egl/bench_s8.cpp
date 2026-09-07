// PW-2 (S-8) driver: variant A vs B on a deforming 100k x 8 CV groom.
// Measures:
//   - first-frame wall time (UsdStage open excluded): SDR parse + GLSL codegen
//     + GPU shader compile + Storm resource generation + initial VBO upload
//   - steady-state per-frame draw time (frames 2..N), with per-frame points
//     Set() (deforming). Optionally re-publishes hairTangent every frame
//     (variant B's real cost, SC-2) when repubTangent != 0.
//
// usage: bench_s8 scene.usda camPath [w h complexity frames deform repubTangent]
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
    if (argc < 3) {
        fprintf(stderr, "usage: %s scene.usda camPath [w h complexity frames deform repubTangent]\n", argv[0]);
        return 2;
    }
    const int W    = argc > 3 ? atoi(argv[3]) : 1280;
    const int H    = argc > 4 ? atoi(argv[4]) : 720;
    const float cx = argc > 5 ? (float)atof(argv[5]) : 1.2f;
    const int frames = argc > 6 ? atoi(argv[6]) : 60;
    const bool deform = argc > 7 ? atoi(argv[7]) != 0 : true;
    const bool repubTangent = argc > 8 ? atoi(argv[8]) != 0 : false;

    if (!eglctx::MakeHeadlessGLContext()) return 3;
    GarchGLApiLoad();
    printf("GL_VERSION=%s\n", (const char*)glGetString(GL_VERSION));

    TfStopwatch openSw;
    openSw.Start();
    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
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

    UsdGeomCamera usdCam(stage->GetPrimAtPath(SdfPath(argv[2])));
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
    rp.complexity = cx;
    rp.clearColor = GfVec4f(0, 0, 0, 1);
    rp.enableLighting = true;

    UsdGeomBasisCurves curves(stage->GetPrimAtPath(SdfPath("/World/Hair")));
    UsdAttribute pointsAttr, tangentAttr;
    VtVec3fArray basePts, baseTan;
    size_t numCurves = 0, numCVs = 0;
    if (curves) {
        pointsAttr = curves.GetPointsAttr();
        pointsAttr.Get(&basePts);
        VtIntArray counts; curves.GetCurveVertexCountsAttr().Get(&counts);
        numCurves = counts.size(); numCVs = basePts.size();
        if (repubTangent) {
            tangentAttr = curves.GetPrim().GetAttribute(TfToken("primvars:hairTangent"));
            tangentAttr.Get(&baseTan);
        }
    }
    printf("SCENE curves=%zu cvs=%zu repubTangent=%d (tangentAuthored=%s)\n",
           numCurves, numCVs, (int)repubTangent,
           repubTangent ? (!baseTan.empty() ? "yes" : "NO") : "-");
    glFinish();

    const UsdPrim& root = stage->GetPseudoRoot();
    std::vector<double> frameMs;
    for (int f = 0; f < frames; ++f) {
        TfStopwatch sw;
        sw.Start();
        if (deform && f > 0) {
            const float t = 0.02f * f;
            VtVec3fArray pts(basePts);
            for (size_t i = 0; i < pts.size(); ++i) {
                pts[i] = basePts[i] + GfVec3f(0.01f * sinf(t + basePts[i][1] * 3.0f), 0.0f, 0.0f);
            }
            pointsAttr.Set(pts);
            if (repubTangent) {
                // Perturb tangents slightly each frame so the value actually
                // changes -> forces the real DirtyPrimvar path (identical
                // Set() calls are deduped by usdImaging).
                VtVec3fArray t2(baseTan);
                const float off = 0.0001f * f;
                for (auto& tv : t2) tv[0] += off;
                tangentAttr.Set(t2);
            }
        }
        engine.Render(root, rp);
        glFinish();
        sw.Stop();
        frameMs.push_back(sw.GetSeconds() * 1000.0);
    }
    const double firstMs = frameMs.front();
    double sum = 0.0, mn = 1e30, mx = 0.0;
    for (size_t i = 1; i < frameMs.size(); ++i) { sum += frameMs[i]; mn = std::min(mn, frameMs[i]); mx = std::max(mx, frameMs[i]); }
    const double steady = sum / (frameMs.size() - 1);
    printf("RESULT scene=%s res=%dx%d complexity=%.1f curves=%zu cvs=%zu deform=%d repubTangent=%d frames=%d "
           "stageOpen=%.1fms firstFrame(sdrCompile)=%.2fms steadyMs=%.3f steadyMin=%.3f steadyMax=%.3f fps=%.1f\n",
           argv[1], W, H, cx, numCurves, numCVs, (int)deform, (int)repubTangent, frames,
           openMs, firstMs, steady, mn, mx, steady > 0 ? 1000.0 / steady : 0.0);
    return 0;
}
