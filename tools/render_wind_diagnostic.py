#!/usr/bin/env python3
"""Bake native Wind billow at real times and render calm/billowing comparisons.

The strands in both panels come from usdGen's compiler and scheduler. Only
the native Wind operator's enabled state and cook time differ; annotations
are composited after MoonRay renders the baked BasisCurves.
"""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont, ImageStat


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "examples" / "docs" / "operators" / "wind.usda"
SCRATCH = ROOT / "renders" / "docs" / "wind-diagnostic"
MEDIA = ROOT / "docs" / "site" / "media"
# 24 time codes/second; low and high rates of .24/.96 Hz complete one and
# four cycles across 100 frames. Every GIF frame is a distinct engine cook.
FRAME_CODES = tuple(range(100))
LOOP_CODE = 100
STILL_CODE = 30
PREVIEW_CODES = (0, 30)
SKY_SOURCE = "@../../maps/StinsonBeach.hdr@"
SKY_SCRATCH = "@../../../examples/maps/StinsonBeach.hdr@"
ENABLED = "bool usdGen:enabled = true"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for name in ("segoeui.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def cooked_points(path: Path, env: dict[str, str]) -> tuple[list[int], list[tuple[float, float, float]]]:
    usdcat = shutil.which("usdcat", path=env.get("PATH"))
    if not usdcat:
        raise FileNotFoundError("usdcat is unavailable in the viewer environment")
    result = subprocess.run([usdcat, str(path)], cwd=ROOT, env=env,
                            text=True, capture_output=True, check=True)
    text = result.stdout
    baked_scope = re.search(r'def BasisCurves "Baked_Fur"', text)
    if not baked_scope:
        raise RuntimeError(f"Cannot find baked groom in usdcat output for {path}")
    text = text[baked_scope.start():]
    counts_match = re.search(r"int\[\] curveVertexCounts = (\[[^\]]*\])", text, re.S)
    points_match = re.search(r"point3f\[\] points = (\[[^\]]*\])", text, re.S)
    if not counts_match or not points_match:
        raise RuntimeError(f"Cannot find cooked curves in usdcat output for {path}")
    counts = ast.literal_eval(counts_match.group(1))
    points = [tuple(point) for point in ast.literal_eval(points_match.group(1))]
    if sum(counts) != len(points):
        raise RuntimeError("Baked point and count arrays disagree")
    return counts, points


def usdcat_points_literal(path: Path, env: dict[str, str]) -> str:
    usdcat = shutil.which("usdcat", path=env.get("PATH"))
    if not usdcat:
        raise FileNotFoundError("usdcat is unavailable in the viewer environment")
    result = subprocess.run([usdcat, str(path)], cwd=ROOT, env=env,
                            text=True, capture_output=True, check=True)
    text = result.stdout
    baked_scope = re.search(r'def BasisCurves "Baked_Fur"', text)
    if not baked_scope:
        raise RuntimeError(f"Cannot find baked groom in {path}")
    text = text[baked_scope.start():]
    points = re.search(r"point3f\[\] points = (\[[^\]]*\])", text, re.S)
    if not points:
        raise RuntimeError(f"Cannot find baked points in {path}")
    return points.group(1)


def time_sampled_stage(timed: dict[int, Path], env: dict[str, str],
                       reuse: bool) -> Path:
    stage = SCRATCH / "wind-time-sampled.usda"
    if (reuse and stage.exists()
            and stage.stat().st_mtime >= max(path.stat().st_mtime for path in timed.values())):
        return stage
    with stage.open("w", encoding="utf-8") as file:
        file.write('''#usda 1.0
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
(
    defaultPrim = "World"
    subLayers = [@./wind-time-00-baked.usdc@]
    timeCodesPerSecond = 24
    framesPerSecond = 24
    startTimeCode = 0
    endTimeCode = 99
)
over "World" {
    over "Groom" {
        over "Baked_Fur" {
            point3f[] points.timeSamples = {
''')
        for code, baked in timed.items():
            file.write(f"                {code}: {usdcat_points_literal(baked, env)},\n")
        file.write('''            }
        }
    }
}
''')
    return stage


def rendered_sequence(stage: Path, env: dict[str, str], reuse: bool,
                      width: int) -> dict[int, Path]:
    from render_pathtraced import RENDERER, run_logged
    recorder_name = shutil.which("usdrecord", path=env.get("PATH"))
    if not recorder_name:
        raise FileNotFoundError("usdrecord is unavailable in viewer environment")
    recorder = Path(recorder_name)
    if recorder.suffix.lower() == ".cmd" and recorder.with_suffix("").exists():
        recorder = recorder.with_suffix("")
    targets = {code: SCRATCH / f"wind-batch-{width}-{code:03d}.000.png"
               for code in FRAME_CODES}
    def frame_ready(path: Path) -> bool:
        if not path.exists() or path.stat().st_mtime < stage.stat().st_mtime:
            return False
        try:
            with Image.open(path) as frame:
                if frame.width != width:
                    return False
                frame.load()
            return True
        except (OSError, ValueError):
            return False

    pending = [code for code, path in targets.items()
               if not reuse or not frame_ready(path)]
    if pending:
        # One usdrecord process retains the MoonRay delegate while it advances
        # through actual authored point samples. Resume from the first missing
        # frame if interrupted; existing earlier frames stay untouched.
        start = min(pending)
        command = [sys.executable, str(recorder), "--renderer", RENDERER,
                   "--imageWidth", str(width), "--disableCameraLight",
                   "--camera", "/World/Cam", "--frames", f"{start}:99",
                   str(stage), str(SCRATCH / f"wind-batch-{width}-###.###.png")]
        run_logged(command, SCRATCH / f"wind-batch-{width}.render.log", env)
    raw_hashes = set()
    for code, path in targets.items():
        if not path.exists() or path.stat().st_size < 1024:
            raise RuntimeError(f"Missing batch renderer frame {code}: {path}")
        with Image.open(path) as frame:
            sample = frame.convert("RGB").resize((128, 85))
            if max(ImageStat.Stat(sample).stddev) < 5:
                raise RuntimeError(f"Nearly flat renderer output at frame {code}")
        raw_hashes.add(sha256(path))
    if len(raw_hashes) != len(FRAME_CODES):
        raise RuntimeError(f"Only {len(raw_hashes)} distinct raw renders for 100 frames")
    return targets


def validate_motion(calm_path: Path, timed: dict[int, Path],
                    env: dict[str, str], loop_path: Path | None = None) -> dict[str, object]:
    counts, calm = cooked_points(calm_path, env)
    starts, ends = [], []
    offset = 0
    for count in counts:
        starts.append(offset)
        ends.append(offset + count - 1)
        offset += count
    metrics: dict[str, object] = {"curves": len(counts), "cvs": len(calm)}
    point_hashes: set[str] = set()
    first_tip_x: list[float] | None = None
    largest_temporal_delta = 0.0
    largest_bend_from_linear = 0.0
    mean_tip_values = []
    for code, path in timed.items():
        other_counts, points = cooked_points(path, env)
        if other_counts != counts or len(points) != len(calm):
            raise RuntimeError(f"Frame {code} changed groom topology")
        point_digest = hashlib.sha256()
        for point in points:
            point_digest.update(struct.pack("<fff", *point))
        point_hashes.add(point_digest.hexdigest())
        root_delta = max(abs(points[i][axis] - calm[i][axis])
                         for i in starts for axis in range(3))
        cross_axis_delta = max(abs(points[i][axis] - calm[i][axis])
                               for i in range(len(calm)) for axis in (1, 2))
        if root_delta > 1e-6 or cross_axis_delta > 1e-6:
            raise RuntimeError(f"Frame {code} moved roots or cross-axis points")
        tip_x = [points[i][0] - calm[i][0] for i in ends]
        mean_tip_values.append(sum(tip_x) / len(tip_x))
        if first_tip_x is None:
            first_tip_x = tip_x
        largest_temporal_delta = max(largest_temporal_delta,
            max(abs(a - b) for a, b in zip(tip_x, first_tip_x)))
        offset = 0
        for count, tip in zip(counts, tip_x):
            for local in range(1, count - 1):
                index = offset + local
                displacement = points[index][0] - calm[index][0]
                linear = tip * local / (count - 1)
                largest_bend_from_linear = max(largest_bend_from_linear,
                                               abs(displacement - linear))
            offset += count
    if largest_temporal_delta < 0.015:
        raise RuntimeError(f"Billow did not animate enough: {largest_temporal_delta:.5f}m")
    if largest_bend_from_linear < 0.005:
        raise RuntimeError(f"Wind still looks like straight shear: {largest_bend_from_linear:.5f}m")
    if len(point_hashes) != len(timed):
        raise RuntimeError(f"Only {len(point_hashes)} distinct cooked geometries for "
                           f"{len(timed)} requested time frames")
    metrics["distinctCookedPointHashes"] = len(point_hashes)
    if loop_path is not None:
        loop_counts, loop_points = cooked_points(loop_path, env)
        first_counts, first_points = cooked_points(timed[FRAME_CODES[0]], env)
        if loop_counts != first_counts:
            raise RuntimeError("Loop endpoint changed topology")
        loop_error = max(abs(a[axis] - b[axis])
                         for a, b in zip(first_points, loop_points)
                         for axis in range(3))
        if loop_error > 1e-5:
            raise RuntimeError(f"Frame 100 fails periodic seam: {loop_error}m")
        metrics["loopEndpointErrorM"] = round(loop_error, 8)
    metrics["maxRootDeltaM"] = 0.0
    metrics["maxTipTemporalDeltaM"] = round(largest_temporal_delta, 6)
    metrics["maxBendFromLinearM"] = round(largest_bend_from_linear, 6)
    metrics["meanTipDisplacementRangeM"] = [round(min(mean_tip_values), 6),
                                            round(max(mean_tip_values), 6)]
    return metrics


def panel(calm: Image.Image, windy: Image.Image, code: int) -> Image.Image:
    if calm.size != windy.size:
        raise ValueError("Both captures must have exactly the same camera framing")
    width, height = calm.size
    header, footer, gap = 86, 55, 8
    canvas = Image.new("RGB", (width * 2 + gap, height + header + footer), "#121c27")
    canvas.paste(calm.convert("RGB"), (0, header))
    canvas.paste(windy.convert("RGB"), (width + gap, header))
    draw = ImageDraw.Draw(canvas)
    draw.rectangle((width, header, width + gap - 1, header + height - 1), fill="#121c27")
    draw.text((23, 13), "CALM", font=font(31), fill="#e7eff4")
    draw.text((23, 51), "Wind disabled", font=font(20), fill="#b9c8d1")
    draw.text((width + gap + 23, 13), "BILLOW", font=font(31), fill="#e7eff4")
    draw.text((width + gap + 23, 51), f"t = {code / 24:.2f} s   ·   low + high",
              font=font(19), fill="#b9c8d1")
    arrow_y = header + height + 25
    draw.text((23, header + height + 12), "Same roots and camera  ·  native Wind  ·  +X direction",
              font=font(19), fill="#aebec9")
    draw.line((canvas.width - 164, arrow_y, canvas.width - 35, arrow_y),
              fill="#79d4e7", width=4)
    draw.polygon([(canvas.width - 35, arrow_y),
                  (canvas.width - 50, arrow_y - 9),
                  (canvas.width - 50, arrow_y + 9)], fill="#79d4e7")
    return canvas


def animation_frame(rendered: Image.Image, code: int) -> Image.Image:
    frame = rendered.convert("RGB").copy()
    draw = ImageDraw.Draw(frame)
    label = f"WIND  ·  FRAME {code:03d}  ·  {code / 24:.2f} s"
    draw.rounded_rectangle((14, 13, 312, 48), radius=8, fill="#13202a")
    draw.text((26, 20), label, font=font(20), fill="#e6eff3")
    return frame


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview", action="store_true",
                        help="Two real times at low resolution, saved under ignored renders/")
    parser.add_argument("--bake-only", action="store_true",
                        help="Verify actual time-varying geometry without renderer work")
    parser.add_argument("--reuse-bakes", action="store_true",
                        help="Reuse fresh ignored bakes and captures while iterating")
    parser.add_argument("--gif-width", type=int, default=800,
                        help="Width of each full-frame GIF render (default 800)")
    args = parser.parse_args()
    if args.gif_width < 400:
        parser.error("--gif-width must be at least 400")
    sys.path.insert(0, str(ROOT / "tools" / "docs"))
    from render_pathtraced import bake, render, viewer_environment

    SCRATCH.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text(encoding="utf-8")
    if source.count(ENABLED) != 1 or source.count(SKY_SOURCE) != 1:
        raise RuntimeError("Wind scene needs one enabled native operator and one sky asset")
    windy_stage = SCRATCH / "wind-native.usda"
    calm_stage = SCRATCH / "wind-calm.usda"
    windy_source = source.replace(SKY_SOURCE, SKY_SCRATCH)
    previous_source = windy_stage.read_text(encoding="utf-8") if windy_stage.exists() else ""
    normalize_timeline = lambda value: re.sub(
        r"(?m)^    endTimeCode = [0-9]+$", "    endTimeCode = <timeline>", value)
    cache_compatible = (args.reuse_bakes and previous_source
                        and normalize_timeline(previous_source) == normalize_timeline(windy_source))
    windy_stage.write_text(windy_source, encoding="utf-8")
    calm_stage.write_text(source.replace(ENABLED, "bool usdGen:enabled = false")
                          .replace(SKY_SOURCE, SKY_SCRATCH), encoding="utf-8")
    env = viewer_environment()
    calm_baked = SCRATCH / "wind-calm-baked.usdc"
    if not cache_compatible or not calm_baked.exists():
        bake(calm_stage, ["/World/Groom/Fur"], "/World/Looks/Fur", calm_baked, env, 0.0)
    codes = PREVIEW_CODES if args.preview else FRAME_CODES
    if not args.preview and STILL_CODE not in codes:
        raise RuntimeError("Still frame must be among the cooked GIF frames")
    timed: dict[int, Path] = {}
    for code in codes:
        baked = SCRATCH / f"wind-time-{code:02d}-baked.usdc"
        if not cache_compatible or not baked.exists():
            bake(windy_stage, ["/World/Groom/Fur"], "/World/Looks/Fur",
                 baked, env, float(code))
        timed[code] = baked
    loop_baked = None
    if not args.preview:
        loop_baked = SCRATCH / "wind-time-100-baked.usdc"
        if not cache_compatible or not loop_baked.exists():
            bake(windy_stage, ["/World/Groom/Fur"], "/World/Looks/Fur",
                 loop_baked, env, float(LOOP_CODE))
    metrics = validate_motion(calm_baked, timed, env, loop_baked)
    if args.bake_only:
        print(json.dumps({"metrics": metrics}, indent=2))
        return

    def capture(baked: Path, code: str, width: int) -> Image.Image:
        target = SCRATCH / f"wind-{code}-{width}.png"
        if (not args.reuse_bakes or not target.exists()
                or target.stat().st_mtime < baked.stat().st_mtime):
            render(baked, "/World/Cam", target, width, env)
        with Image.open(target) as image:
            return image.convert("RGB")

    if args.preview:
        calm_image = capture(calm_baked, "calm", 360)
        images = {code: capture(baked, f"time-{code:02d}", 360)
                  for code, baked in timed.items()}
        output = SCRATCH / "wind-native-preview.png"
        panel(calm_image, images[PREVIEW_CODES[-1]], PREVIEW_CODES[-1]).save(output)
        print(json.dumps({"preview": str(output), "metrics": metrics}, indent=2))
        return

    MEDIA.mkdir(parents=True, exist_ok=True)
    still = MEDIA / "wind.png"
    animation = MEDIA / "wind.gif"
    sampled_stage = time_sampled_stage(timed, env, args.reuse_bakes)
    images = rendered_sequence(sampled_stage, env, args.reuse_bakes, args.gif_width)
    large_calm = capture(calm_baked, "calm", 640)
    with Image.open(images[STILL_CODE]) as captured:
        large_wind = captured.convert("RGB").resize((640, round(captured.height * 640 / captured.width)),
                                                    Image.Resampling.LANCZOS)
    panel(large_calm, large_wind, STILL_CODE).save(still)
    frames_rgb = []
    for code in FRAME_CODES:
        with Image.open(images[code]) as captured:
            frames_rgb.append(animation_frame(captured, code))
    # A small contact sheet gives every frame one vote in the common palette
    # without holding a full-resolution 100-frame stack in one giant image.
    thumb_size = (160, round(frames_rgb[0].height * 160 / frames_rgb[0].width))
    contact = Image.new("RGB", (thumb_size[0], thumb_size[1] * len(frames_rgb)))
    for index, frame in enumerate(frames_rgb):
        contact.paste(frame.resize(thumb_size), (0, index * thumb_size[1]))
    palette = contact.quantize(colors=256, method=Image.Quantize.FASTOCTREE)
    frames = [frame.quantize(palette=palette, dither=Image.Dither.NONE)
              for frame in frames_rgb]
    # GIF stores centiseconds. 83 x 40ms + 17 x 50ms = 4170ms, within
    # 3.3ms of the 100/24-second native period. Distribute longer frames.
    durations_ms = [40 + (10 if ((index + 1) * 17) // 100 > (index * 17) // 100 else 0)
                    for index in range(100)]
    frames[0].save(animation, save_all=True, append_images=frames[1:],
                   duration=durations_ms, loop=0, disposal=2, optimize=False)
    print(json.dumps({
        "scene": SOURCE.relative_to(ROOT).as_posix(),
        "generator": "tools/render_wind_diagnostic.py",
        "image": still.relative_to(ROOT).as_posix(),
        "animation": animation.relative_to(ROOT).as_posix(),
        "sceneSha256": sha256(SOURCE),
        "imageSha256": sha256(still),
        "animationSha256": sha256(animation),
        "metrics": metrics,
        "timeCodes": FRAME_CODES,
        "timeCodesPerSecond": 24,
        "renderer": "MoonRay / HdMoonrayRendererDebugPlugin",
        "provenance": "Native low/high-frequency Wind billow was evaluated by the usdGen compiler/scheduler at 100 distinct real time codes (0..99) at 24fps; time code 100 closes the 4.1667-second cycle. Native baked points were authored verbatim as USD time samples and MoonRay path-traced every state from an elevated camera. The GIF contains only the moving Wind patch and a frame/time label; the still pairs it with the same groom with Wind disabled. All upstream geometry, seeds, camera and lighting are fixed."
    }, indent=2))


if __name__ == "__main__":
    main()
