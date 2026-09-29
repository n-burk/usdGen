#!/usr/bin/env python3
# testPomadeGolden -- the per-level publication renders (plan/18 section 4
# "Image", G15 "no pomade goldens in tests/golden").
#
#   python plugin/usdGenPomadeTools/testenv/testPomadeGolden.py
#
# with the plugin path and PYTHONPATH the other pomade tests use; ctest
# supplies them. No usdview: bin/record_pomade.py makes its own offscreen
# GL context, so this is a headless render test like testUsdGenStormLook.
#
# Each scene is rendered WITH THE TOOL LIVE -- the groom hydrated into a
# PomadeModel, level 2 focused, the model activated and published -- and
# compared against tests/golden/pomade-<scene>.png. A plain usdrecord of
# the same file shows only the committed hair; every pixel of the tubes,
# the level colours, the center curves and the CV dots in these goldens
# arrived through the Pomade scene index, which is what makes them worth
# keeping (plan/18 F1/F2).
#
# Tolerance is plan/10 section 5.4's, the same pair testUsdGenStormLook
# gates the Storm look with: mean absolute difference <= 2/255 and fewer
# than 0.5 per cent of pixels differing by more than 8/255. The reasoning
# carries over unchanged -- one host, one GPU, one driver, a fixed camera
# and a fixed complexity, so the only legitimate run-to-run movement is
# MSAA coverage on silhouette pixels -- and it is if anything generous
# here: the Pomade overlay is flat-shaded geometry with no texture
# filtering and no lighting model of its own, so the interior of every
# tube is exactly reproducible and only its edges can move at all.
# Measured on this host (RTX 4090, 2026-09-19): three consecutive runs
# against a freshly written golden came out bit-identical twice and at
# meanAbs 0.000002 with 0.0031 per cent of pixels over 8/255 once, so
# the gate leaves about 4 000x headroom on the mean and 160x on the
# fraction. Anything that moves either number appreciably is a real
# change in what the tool draws, not noise.
#
# Goldens are never written by a normal run (plan/10 section 5.4): a
# missing golden FAILS. USDGEN_REGEN_GOLDEN=1 rewrites them and their
# metadata sidecars.
#
# Exit: 0 pass, 1 fail, 77 skip (no numpy/PIL, or no GL context here).
import datetime
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
GOLDEN_DIR = os.path.join(ROOT, "tests", "golden")

# (scene file, golden name, camera, focused level, groom prim, tool live).
#
# The braid names no camera, so record_pomade.py generates one that fits
# the stage and the live model together: that file's own camera was
# framed for the committed hair, and the tubes the tool publishes are
# large enough to fill it edge to edge. The ponytail, recorded without
# the tool, has nothing to overflow its camera and keeps it.
#
# The braid is the tool-live frame -- the committer wrote it, so
# PomadeHydrateModel reads the whole L1/L2/L3 hierarchy back and the
# scene index stages it (plan/18 section 4 "Image"). The ponytail is
# recorded with the tool OFF because the tool cannot open that file at
# all: its guides are hand-posed example content, not K9 output, it
# carries no UsdGenScalpGraph shell, and PomadeHydrateModel refuses it by
# design (the header of examples/pomade-ponytail.usda says so). Its
# golden gates what that file is for instead -- the plan/17 P3 exit,
# "examples/pomade-ponytail.usda renders with amplified hair".
SCENES = (
    ("pomade-braid-hierarchy.usda", "pomade-braid-hierarchy.png", "", 2,
     "/PomadeGroom", True),
    ("pomade-ponytail.usda", "pomade-ponytail.png", "/World/Cam", 2,
     "/World/PomadeGroom", False),
)
WIDTH = 512
COMPLEXITY = "veryhigh"
MEAN_ABS_MAX = 2.0 / 255.0
FRAC_GT_8_255_MAX = 0.005

failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def info(text):
    print("info: %s" % text)


def loadRecorder():
    path = os.path.join(ROOT, "bin", "record_pomade.py")
    spec = importlib.util.spec_from_file_location("record_pomade", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


# The background FrameRecorder leaves is TRANSPARENT, not a colour, so
# every comparison composites over one opaque grey first. Reading the
# RGB straight would call a transparent frame black and would let an
# alpha-only regression through unnoticed.
BACKDROP = 128.0


def flatten(path):
    """The PNG composited over BACKDROP, as an HxWx3 float array."""
    import numpy
    from PIL import Image
    with Image.open(path) as image:
        rgba = numpy.asarray(image.convert("RGBA"), dtype=numpy.float32)
    alpha = rgba[:, :, 3:4] / 255.0
    return rgba[:, :, :3] * alpha + BACKDROP * (1.0 - alpha)


def compare(goldenPath, renderPath):
    """(meanAbs, fracOver, sizesMatch) over the two PNGs, in 0..1 units."""
    import numpy
    golden = flatten(goldenPath)
    render = flatten(renderPath)
    if golden.shape != render.shape:
        return (1.0, 1.0, False)
    diff = numpy.abs(golden - render) / 255.0
    return (float(diff.mean()),
            float((diff > 8.0 / 255.0).mean()),
            True)


def contrast(path):
    """Spread of the composited frame: 0 means nothing was drawn."""
    import numpy
    pixels = flatten(path)
    return float(pixels.max() - pixels.min()) / 255.0


def writeSidecar(goldenPath, scene, tubes, guides):
    """Metadata beside a regenerated golden, as bin/regen_goldens.sh writes."""
    with open(goldenPath, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    try:
        sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                      text=True).strip()
    except (OSError, subprocess.SubprocessError):
        sha = "unknown"
    payload = {
        "golden": os.path.basename(goldenPath),
        "scene": scene,
        "git_sha": sha,
        "host": os.environ.get("COMPUTERNAME") or os.uname().nodename,
        "created_utc": datetime.datetime.now(
            datetime.timezone.utc).isoformat(),
        "command": "USDGEN_REGEN_GOLDEN=1 ctest -R testPomadeGolden "
                   "(bin/record_pomade.py, width %d, complexity %s, L2 focus)"
                   % (WIDTH, COMPLEXITY),
        "model": {"tubes": tubes, "guides": guides},
        "tolerance": {"mean_abs_max": MEAN_ABS_MAX,
                      "frac_gt_8_255_max": FRAC_GT_8_255_MAX},
        "png_sha256": digest,
    }
    with open(goldenPath + ".json", "w") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")


def main():
    try:
        import numpy                                   # noqa: F401
        from PIL import Image                          # noqa: F401
    except ImportError as exc:
        print("SKIP: testPomadeGolden needs numpy and Pillow (%s)" % exc)
        return 77
    try:
        recorder = loadRecorder()
    except OSError as exc:
        print("SKIP: cannot load bin/record_pomade.py (%s)" % exc)
        return 77

    regen = bool(os.environ.get("USDGEN_REGEN_GOLDEN"))
    scratch = tempfile.mkdtemp(prefix="usdGenPomadeGolden")
    for sceneName, goldenName, camera, level, groom, tool in SCENES:
        scene = os.path.join(ROOT, "examples", sceneName)
        render = os.path.join(scratch, goldenName)
        golden = os.path.join(GOLDEN_DIR, goldenName)
        try:
            tubes, guides, stagedL1 = recorder.record(
                scene, render, camera=camera, width=WIDTH,
                complexity=COMPLEXITY, level=level, groom=groom, tool=tool)
        except ImportError as exc:
            print("SKIP: the render environment is incomplete (%s)" % exc)
            return 77
        except RuntimeError as exc:
            check(False, "%s records with the tool live (%s)"
                  % (sceneName, exc))
            continue
        if tool:
            check(stagedL1 >= 1,
                  "%s: the scene index staged %d L1 tube(s) for the frame "
                  "(model holds %d tube(s), %d guide(s))"
                  % (sceneName, stagedL1, tubes, guides))
        else:
            info("%s: recorded with the tool off (see SCENES)" % sceneName)
        check(os.path.isfile(render), "%s: a frame was written" % sceneName)
        if not os.path.isfile(render):
            continue
        spread = contrast(render)
        info("%s: contrast %.5f over the grey backdrop" % (sceneName, spread))
        check(spread > 8.0 / 255.0,
              "%s: the frame has something in it" % sceneName)

        if regen:
            os.makedirs(GOLDEN_DIR, exist_ok=True)
            with open(render, "rb") as src, open(golden, "wb") as dst:
                dst.write(src.read())
            writeSidecar(golden, sceneName, tubes, guides)
            print("GOLDEN WRITTEN %s" % golden)
            continue
        if not os.path.isfile(golden):
            check(False, "%s: golden missing: %s -- regenerate with "
                         "USDGEN_REGEN_GOLDEN=1 ctest -R testPomadeGolden "
                         "(plan/10 section 5.4)" % (sceneName, golden))
            continue
        meanAbs, fracOver, sameSize = compare(golden, render)
        check(sameSize, "%s: the render is the golden's size" % sceneName)
        info("%s: meanAbs %.6f frac>8/255 %.6f" % (sceneName, meanAbs,
                                                   fracOver))
        check(meanAbs <= MEAN_ABS_MAX,
              "%s: mean abs diff <= 2/255 (%.6f)" % (sceneName, meanAbs))
        check(fracOver < FRAC_GT_8_255_MAX,
              "%s: under 0.5%% of pixels differ by more than 8/255 (%.6f)"
              % (sceneName, fracOver))

    print("testPomadeGolden: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
