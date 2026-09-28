"""Record real Storm viewport frames while the felt groom cooks asynchronously.

Run with bin/launch_usdview.ps1 -TestScript tools/capture_progressive_felt.py
    examples/felt/felt_sphere.usda --allow-async --renderer GL

USDGEN_PROGRESS_DIR selects the output directory. The script first renders the
complete authored groom to warm Storm, then edits only the session layer: it
deactivates the three descriptions together while incrementing their source
seeds, renders the bare emitter, and reactivates them together. All recorded
frames are StageView framebuffer reads from that cook.
"""

import json
import os
import time
from pathlib import Path


def testUsdviewInputFunction(controller):
    from OpenGL import GL
    from pxr import Sdf, Usd
    from pxr.UsdAppUtils.complexityArgs import RefinementComplexities
    from pxr.Usdviewq.qt import QtWidgets
    from pxr.Usdviewq.common import RenderModes
    from usdGenTools import brushApi

    output = Path(os.environ.get("USDGEN_PROGRESS_DIR", "renders/progressive-felt"))
    output.mkdir(parents=True, exist_ok=True)
    stage = controller._dataModel.stage
    stage.SetEditTarget(stage.GetSessionLayer())
    settings = controller._dataModel.viewSettings
    settings.cameraPrim = stage.GetPrimAtPath("/Camera")
    assert settings.cameraPrim, "Felt camera missing"
    settings.showHUD = settings.showBBoxes = settings.displayGuide = False
    settings.renderMode = RenderModes.SMOOTH_SHADED
    settings.enableSceneMaterials = settings.enableSceneLights = True
    settings.ambientLightOnly = settings.domeLightEnabled = False
    settings.complexity = RefinementComplexities.VERY_HIGH
    settings.colorCorrectionMode = "sRGB"

    view = controller._stageView
    assert view.allowAsync, "Pass --allow-async to testusdview"
    view.DrawAxis = lambda matrix: None
    view.SetPhysicalWindowSize(640, 640)
    app = QtWidgets.QApplication.instance()
    if view.GetCurrentRendererId() != "HdStormRendererPlugin":
        assert view.SetRendererPlugin("HdStormRendererPlugin")

    def render():
        view.update()
        app.processEvents()
        GL.glFinish()

    def counters():
        return int(brushApi.groomCookCount()), int(brushApi.groomPublishCount())

    # Shader, light, buffer, and texture compilation is outside the recording.
    # Wait for publication to settle so the trigger is an actual new cook.
    warm_start = time.monotonic()
    last_publish = -1
    stable_since = warm_start
    while True:
        render()
        _, published = counters()
        now = time.monotonic()
        if published != last_publish:
            stable_since = now
            last_publish = published
        if now - stable_since >= 2.0 and now - warm_start >= 3.0 and view._renderer.IsConverged():
            break
        if now - warm_start > 180:
            raise TimeoutError("Initial felt groom did not settle")
        time.sleep(0.05)
    print("warm complete: %.3fs, cooks=%d, publishes=%d" %
          (time.monotonic() - warm_start, *counters()), flush=True)

    warm_file = output / "warm.png"
    assert view.grabFrameBuffer().save(str(warm_file))
    names = ("Felt", "FeltCurls", "Flyaways")
    descriptions = [stage.GetPrimAtPath("/FeltSphere/Groom/" + name)
                    for name in names]
    originals = []
    for prim in descriptions:
        assert prim, "Description missing"
        scatter = stage.GetPrimAtPath(str(prim.GetPath()) + "/Ops/scatter")
        density = scatter.GetAttribute("usdGen:density")
        seed = scatter.GetAttribute("usdGen:seed")
        assert density and seed, "Scatter parameters missing"
        originals.append((prim, seed, float(density.Get()), int(seed.Get())))

    with Sdf.ChangeBlock():
        for prim, seed, _, old_seed in originals:
            seed.Set(old_seed + 1009)
            prim.SetActive(False)
    bare_start = time.monotonic()
    stable_since = bare_start
    last_publish = counters()[1]
    while True:
        render()
        _, published = counters()
        now = time.monotonic()
        if published != last_publish:
            stable_since = now
            last_publish = published
        if now - stable_since >= 1.0 and now - bare_start >= 1.0:
            break
        if now - bare_start > 45:
            raise TimeoutError("Bare emitter did not settle")
        time.sleep(0.04)
    bare_file = output / "bare.png"
    assert view.grabFrameBuffer().save(str(bare_file))
    before = counters()
    print("bare emitter: cooks=%d, publishes=%d" % before, flush=True)

    with Sdf.ChangeBlock():
        for prim, _, _, _ in originals:
            prim.SetActive(True)
    start = time.monotonic()
    frames = []
    stable_since = start
    last_publish = before[1]
    last_save = -1.0
    max_seconds = float(os.environ.get("USDGEN_PROGRESS_TIMEOUT", "180"))
    while True:
        render()
        now = time.monotonic()
        elapsed = now - start
        cooked, published = counters()
        if published != last_publish:
            stable_since = now
            last_publish = published
        # Capture the actual framebuffer on each publication and at a regular
        # cadence. PNG I/O is included in wall-clock timing for the next frame.
        if published != before[1] or elapsed - last_save >= 0.25 or not frames:
            if published != (frames[-1]["publishes"] if frames else -1) or elapsed - last_save >= 0.25:
                index = len(frames)
                path = output / ("frame_%04d.png" % index)
                image = view.grabFrameBuffer()
                assert image.save(str(path)), "Frame save failed: " + str(path)
                row = {"file": path.name, "seconds": round(elapsed, 6),
                       "cooks": cooked, "publishes": published,
                       "width": image.width(), "height": image.height(),
                       "converged": bool(view._renderer.IsConverged())}
                frames.append(row)
                last_save = elapsed
                print("frame %04d  %.3fs  cooks=%d  publishes=%d" %
                      (index, elapsed, cooked, published), flush=True)
        if elapsed >= 3.0 and now - stable_since >= 2.5 and view._renderer.IsConverged():
            break
        if elapsed >= max_seconds:
            print("capture timeout at %.3fs" % elapsed, flush=True)
            break
        time.sleep(0.03)

    report = {"scene": stage.GetRootLayer().realPath,
              "renderer": "HdStormRendererPlugin", "async": True,
              "camera": "/Camera", "warmSeconds": round(bare_start - warm_start, 6),
              "bareSeconds": round(start - bare_start, 6),
              "originalDensity": {name: original[2] for name, original in zip(names, originals)},
              "seedOffset": 1009, "countersBefore": before,
              "countersAfter": counters(), "elapsedSeconds": round(time.monotonic() - start, 6),
              "frames": frames}
    (output / "capture.json").write_text(json.dumps(report, indent=2) + "\n")
    print("saved %d real viewport frames, %.3fs" %
          (len(frames), report["elapsedSeconds"]), flush=True)
    hold = float(os.environ.get("USDGEN_PROGRESS_HOLD", "0"))
    if hold > 0:
        print("holding completed viewport for %.1fs" % hold, flush=True)
        end = time.monotonic() + hold
        while time.monotonic() < end:
            app.processEvents()
            time.sleep(0.05)
