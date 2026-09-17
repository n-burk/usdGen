# A testusdview script that photographs and times the supersampled viewport.
#
# Not a ctest (it renders a 474k-strand groom and writes PNGs): it exists so
# the interactive supersampling switch can be judged the same way the offline
# references are, without an interactive session.
#
#   testusdview --testScript plugin/usdGenTools/testenv/shotUsdGenToolsSupersample.py \
#               examples/head-hair-closeup.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the T2 tools test uses.
# Environment:
#   USDGEN_SHOT_DIR      where the PNGs go (default: working directory)
#   USDGEN_SHOT_FACTORS  comma-separated factors to shoot (default 1,2,4)
#   USDGEN_SHOT_CAMERA   camera prim path (default /World/TempleCam)
#   USDGEN_SHOT_SIZE     WxH physical window size (default 1280x960)
#   USDGEN_SHOT_FRAMES   timed frames per factor after warmup (default 12)

import os
import sys
import time


def _Size():
    raw = os.environ.get("USDGEN_SHOT_SIZE", "1280x960")
    w, h = raw.lower().split("x")
    return int(w), int(h)


def _Factors():
    raw = os.environ.get("USDGEN_SHOT_FACTORS", "1,2,4")
    return [int(f) for f in raw.split(",") if f.strip()]


def testUsdviewInputFunction(appController):
    from pxr.Usdviewq.qt import QtWidgets
    from pxr.Usdviewq.common import RenderModes
    from OpenGL import GL
    import usdGenTools.supersample as supersample

    app = QtWidgets.QApplication.instance()
    stageView = appController._stageView
    dataModel = appController._dataModel
    viewSettings = dataModel.viewSettings

    outputDir = os.environ.get("USDGEN_SHOT_DIR") or os.getcwd()
    width, height = _Size()
    frames = int(os.environ.get("USDGEN_SHOT_FRAMES", "12"))
    cameraPath = os.environ.get("USDGEN_SHOT_CAMERA", "/World/TempleCam")

    if not supersample.IsInstalled():
        print("FAIL: usdGenTools did not install the supersample patch")
        sys.exit(1)

    stageView.SetPhysicalWindowSize(width, height)

    # Match bin/record_usd.ps1: highest complexity, the scene's own lights
    # rather than usdview's camera headlight, no HUD over the image.
    viewSettings.showHUD = False
    viewSettings.renderMode = RenderModes.SMOOTH_SHADED
    viewSettings.ambientLightOnly = False
    viewSettings.domeLightEnabled = False
    viewSettings.enableSceneLights = True
    viewSettings.enableSceneMaterials = True
    viewSettings.complexity = _VeryHigh()
    camera = dataModel.stage.GetPrimAtPath(cameraPath)
    if not camera:
        print("FAIL: no camera at " + cameraPath)
        sys.exit(1)
    viewSettings.cameraPrim = camera
    app.processEvents()

    for factor in _Factors():
        supersample.SetFactor(factor)
        # Warm up: first paint of a factor allocates the target and, at 1x,
        # is the first image of the session.
        for _ in range(3):
            _Repaint(stageView, app, GL)

        times = []
        paintTimes = []
        for _ in range(frames):
            start = time.perf_counter()
            _Repaint(stageView, app, GL)
            times.append((time.perf_counter() - start) * 1000.0)
            # What usdview's own HUD would report: the wall time of paintGL,
            # which does not wait for the GPU.
            paintTimes.append(stageView._renderTime * 1000.0)
        times.sort()
        paintTimes.sort()

        image = stageView.grabFrameBuffer()
        name = os.path.join(outputDir, "ss%d.png" % factor)
        if not image.save(name):
            print("FAIL: could not write " + name)
            sys.exit(1)
        print("ss%d: %dx%d  frame min %.2f ms  median %.2f ms  "
              "(paintGL median %.2f ms)  -> %s"
              % (factor, image.width(), image.height(), times[0],
                 times[len(times) // 2], paintTimes[len(paintTimes) // 2],
                 name))


def _Repaint(stageView, app, GL):
    """One full frame: StageView.updateGL only schedules a paint."""
    stageView.update()
    app.processEvents()
    GL.glFinish()


def _VeryHigh():
    from pxr.UsdAppUtils.complexityArgs import RefinementComplexities
    return RefinementComplexities.VERY_HIGH
