#include "eglctx.h"
#include "pxr/pxr.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/usdImaging/usdAppUtils/frameRecorder.h"
#include <cstdio>
#include <string>
#include <cstdlib>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s scene.usda out.png camPath [width]\n", argv[0]); return 2; }
    if (!eglctx::MakeHeadlessGLContext()) return 3;
    GarchGLApiLoad();
    printf("GL_VERSION=%s\n", (const char*)glGetString(GL_VERSION));

    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
    if (!stage) { fprintf(stderr, "cannot open %s\n", argv[1]); return 4; }

    UsdAppUtilsFrameRecorder rec(TfToken(), /*gpuEnabled*/true);
    if (!rec.SetRendererPlugin(TfToken("HdStormRendererPlugin"))) {
        fprintf(stderr, "SetRendererPlugin(Storm) failed\n"); return 5; }
    rec.SetImageWidth(argc > 4 ? (size_t)atoi(argv[4]) : 800);
    rec.SetComplexity(1.5f);          // -> refineLevel 2 (ribbon + ROUND)
    rec.SetColorCorrectionMode(TfToken("sRGB"));
    rec.SetCameraLightEnabled(getenv("HAIR_CAMLIGHT") ? atoi(getenv("HAIR_CAMLIGHT")) != 0 : true);
    if (getenv("HAIR_COMPLEXITY")) rec.SetComplexity((float)atof(getenv("HAIR_COMPLEXITY")));

    UsdGeomCamera cam(stage->GetPrimAtPath(SdfPath(argv[3])));
    bool ok = rec.Record(stage, cam, UsdTimeCode::EarliestTime(), argv[2]);
    printf("Record -> %s : %s\n", argv[2], ok ? "OK" : "FAILED");
    return ok ? 0 : 6;
}
