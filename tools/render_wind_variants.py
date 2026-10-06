#!/usr/bin/env python3
"""Bake and render dense native Wind with Clump enabled and disabled.

The two authored scenes differ only by the native Clump enabled value. Every
animation frame is a separate usdGen compiler/scheduler cook at its USD time
code; only labels are composited after MoonRay renders the baked curves.
"""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
from typing import Callable

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat

import render_wind_diagnostic as wind


ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / "renders" / "docs" / "wind-variants"
MEDIA = ROOT / "docs" / "site" / "media"
FRAME_CODES = tuple(range(100))
PREVIEW_CODES = (0, 25)
VARIANTS = ("clumped", "unclumped")
SKY_SOURCE = "@../../maps/StinsonBeach.hdr@"
SKY_SCRATCH = "@../../../examples/maps/StinsonBeach.hdr@"


def source_path(variant: str) -> Path:
    return ROOT / "examples" / "docs" / "operators" / f"wind-{variant}.usda"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pin_viewer_runtime(env: dict[str, str], runtime_dir: Path) -> dict[str, str]:
    """Use one copied usdGen DLL/resource tree for USD and MoonRay children."""
    original = str((ROOT / "build").resolve())
    frozen = str(runtime_dir.resolve())
    for key in ("PATH", "PXR_PLUGINPATH_NAME", "PYTHONPATH"):
        value = env.get(key, "")
        if original.lower() not in value.lower():
            raise RuntimeError(f"Viewer {key} lacks the expected build tree")
        env[key] = value.replace(original, frozen)
        if original.lower() in env[key].lower():
            raise RuntimeError(f"Viewer {key} still points into mutable build")
    required = (
        runtime_dir / "usdGen.dll", runtime_dir / "usdGenImaging.dll",
        runtime_dir / "usd" / "usdGenSchema" / "resources" / "plugInfo.json",
        runtime_dir / "usd" / "usdGenImaging" / "resources" / "plugInfo.json",
    )
    if any(not path.is_file() for path in required):
        raise RuntimeError("Frozen viewer runtime is missing a required DLL or plugin")
    return env


def write_state(variant: str, data: dict[str, object]) -> None:
    target = SCRATCH / f"wind-{variant}-state.json"
    temporary = target.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    temporary.replace(target)


def prepare_source(variant: str, cache_identity: dict[str, str]) -> bool:
    source = source_path(variant).read_text(encoding="utf-8")
    if source.count(SKY_SOURCE) != 1:
        raise RuntimeError(f"Expected one sky path in {variant} scene")
    mapped = source.replace(SKY_SOURCE, SKY_SCRATCH)
    target = SCRATCH / f"wind-{variant}-source.usda"
    runtime = SCRATCH / f"wind-{variant}-cache-identity.json"
    compatible = (target.exists() and target.read_text(encoding="utf-8") == mapped
                  and runtime.exists() and
                  json.loads(runtime.read_text(encoding="utf-8")) == cache_identity)
    if not compatible:
        target.write_text(mapped, encoding="utf-8")
        runtime.write_text(json.dumps(cache_identity, indent=2) + "\n",
                           encoding="utf-8")
    return compatible


def bake_frames(variant: str, codes: tuple[int, ...], env: dict[str, str],
                reuse: bool, compatible: bool, baker: Path,
                cache_identity: dict[str, str],
                progress: Callable[[int, int], None] | None = None) -> dict[int, Path]:
    from render_pathtraced import run_logged

    source = SCRATCH / f"wind-{variant}-source.usda"
    result = {}
    for code in codes:
        target = SCRATCH / f"wind-{variant}-time-{code:03d}-baked.usdc"
        marker = target.with_suffix(".cache.json")
        expected = {"sceneSha256": digest(source_path(variant)),
                    "timeCode": code, "cacheIdentity": cache_identity}
        valid = False
        if reuse and compatible and target.is_file() and marker.is_file():
            try:
                recorded = json.loads(marker.read_text(encoding="utf-8"))
                valid = (all(recorded.get(key) == value
                             for key, value in expected.items()) and
                         recorded.get("bakedSha256") == digest(target))
            except (OSError, ValueError, KeyError):
                valid = False
        if not valid:
            run_logged([str(baker), str(source), "/World/Groom/Fur",
                        str(target), "/World/Looks/Fur", str(float(code))],
                       target.with_suffix(".bake.log"), env)
            recorded = {**expected, "bakedSha256": digest(target)}
            temporary = marker.with_suffix(".json.tmp")
            temporary.write_text(json.dumps(recorded, indent=2) + "\n",
                                 encoding="utf-8")
            temporary.replace(marker)
            print(f"baked {variant} time {code}: {target}", flush=True)
        result[code] = target
        if progress:
            progress(len(result), code)
    return result


def inspect_bake(path: Path, env: dict[str, str], clumped: bool
                 ) -> tuple[list[int], list[tuple[float, float, float]],
                            dict[str, str], dict[str, object]]:
    usdcat = shutil.which("usdcat", path=env.get("PATH"))
    if not usdcat:
        raise FileNotFoundError("usdcat missing from viewer environment")
    output = subprocess.run([usdcat, str(path)], cwd=ROOT, env=env,
                            text=True, capture_output=True, check=True).stdout
    scope = re.search(r'def BasisCurves "Baked_Fur"', output)
    if not scope:
        raise RuntimeError(f"No baked BasisCurves in {path}")
    text = output[scope.start():]
    counts_match = re.search(r"int\[\] curveVertexCounts = (\[[^\]]*\])", text, re.S)
    points_match = re.search(r"point3f\[\] points = (\[[^\]]*\])", text, re.S)
    if not counts_match or not points_match:
        raise RuntimeError(f"Missing baked topology or points in {path}")
    counts = ast.literal_eval(counts_match.group(1))
    points = [tuple(point) for point in ast.literal_eval(points_match.group(1))]
    if len(counts) != 6500 or len(points) != 117000 or set(counts) != {18}:
        raise RuntimeError(f"Wrong dense topology in {path}")
    specs = {
        "clumpId_0": ("int", "uniform", 6500, 1),
        "clumpCenter_0": ("float3", "uniform", 6500, 3),
        "clumpCenterId_0": ("int2", "uniform", 6500, 2),
        "clumpWeight_0": ("float", "vertex", 117000, 1),
        "rest": ("point3f", "vertex", 117000, 3),
        "usdGen:curveId": ("uint64", "uniform", 6500, 1),
        "usdGen:rootFrame": ("matrix4d", "uniform", 6500, 16),
    }
    metadata = {}
    details: dict[str, object] = {}
    for name, (expected_type, interpolation, count, arity) in specs.items():
        pattern = (rf"(?m)^\s*(\w+)\[\] primvars:{re.escape(name)}\s*=\s*"
                   rf"(\[[^\]]*\])\s*\((.*?)\)")
        match = re.search(pattern, text, re.S)
        if name.startswith("clump") and not clumped:
            if match:
                raise RuntimeError(f"Disabled Clump unexpectedly emitted {name} in {path}")
            continue
        if (not match or match.group(1) != expected_type or
                f'interpolation = "{interpolation}"' not in match.group(3)):
            raise RuntimeError(f"Missing or wrongly typed {name} in {path}")
        values = ast.literal_eval(match.group(2))
        if len(values) != count:
            raise RuntimeError(f"Wrong {name} cardinality in {path}: {len(values)}")
        if name == "clumpId_0":
            details["clumpIds"] = values
            details["clumpCount"] = len(set(values) - {-1})
        elif name == "clumpWeight_0":
            details["tipWeights"] = values[17::18]
            details["positiveWeightCvs"] = sum(value > 0 for value in values)
            details["maxWeight"] = max(values)
        elif name == "clumpCenterId_0":
            details["distinctCenterIds"] = len(set(values))
            details["exactCenterIdPairs"] = values
        if name == "clumpCenterId_0":
            flattened = [word for pair in values for word in pair]
            if len(flattened) != count * arity or any(
                    word < -(1 << 31) or word >= (1 << 31) for word in flattened):
                raise RuntimeError(f"Invalid exact int2 center IDs in {path}")
            payload = struct.pack(f"<{len(flattened)}i", *flattened)
        elif name == "usdGen:curveId":
            payload = struct.pack(f"<{len(values)}Q", *values)
        else:
            payload = repr(values).encode("utf-8")
        metadata[name] = hashlib.sha256(payload).hexdigest()
    return counts, points, metadata, details


def geometry_proof(variant: str, frames: dict[int, Path],
                   env: dict[str, str]) -> dict[str, object]:
    point_hashes = set()
    roots0 = None
    tips0 = None
    max_root_delta = 0.0
    max_tip_delta = 0.0
    metadata_by_frame = {}
    for code, path in frames.items():
        counts, points, metadata, details = inspect_bake(
            path, env, variant == "clumped")
        metadata_by_frame[str(code)] = metadata
        if code == next(iter(frames)):
            first_details = {key: value for key, value in details.items()
                             if key not in ("clumpIds", "tipWeights", "exactCenterIdPairs")}
            if variant == "clumped" and len(frames) == 100:
                ids_path = SCRATCH / "wind-clumped-exact-center-ids.json"
                ids_path.write_text(json.dumps(details["exactCenterIdPairs"],
                                               separators=(",", ":")) + "\n",
                                    encoding="utf-8")
                first_details["exactCenterIdPairsFile"] = str(ids_path)
                first_details["exactCenterIdPairsSha256"] = digest(ids_path)
            if variant == "clumped" and (
                    first_details.get("positiveWeightCvs", 0) == 0 or
                    first_details.get("clumpCount", 0) < 2):
                raise RuntimeError("Clump produced no effective native groups/weights")
        point_hashes.add(hashlib.sha256(repr(points).encode("utf-8")).hexdigest())
        roots = points[::18]
        tips = points[17::18]
        if roots0 is None:
            roots0, tips0 = roots, tips
        else:
            max_root_delta = max(max_root_delta, max(
                max(abs(a - b) for a, b in zip(start, current))
                for start, current in zip(roots0, roots)))
            max_tip_delta = max(max_tip_delta, max(
                max(abs(a - b) for a, b in zip(start, current))
                for start, current in zip(tips0, tips)))
    if max_root_delta > 1e-6:
        raise RuntimeError(f"{variant}: Wind moved roots by {max_root_delta}")
    if len(point_hashes) != len(frames):
        raise RuntimeError(f"{variant}: only {len(point_hashes)} unique native states")
    for name in next(iter(metadata_by_frame.values())):
        if len({frame[name] for frame in metadata_by_frame.values()}) != 1:
            raise RuntimeError(f"{variant}: rest/Clump metadata {name} drifted across frames")
    return {"variant": variant, "curves": 6500, "cvs": 117000,
            "distinctNativePointHashes": len(point_hashes),
            "maxRootDeltaM": max_root_delta, "maxTipTemporalDeltaM": max_tip_delta,
            "metadataSchema": {
                "clumpId_0": {"type": "int", "interpolation": "uniform", "arity": 1,
                              "count": 6500},
                "clumpCenter_0": {"type": "float3", "interpolation": "uniform", "arity": 3,
                                  "count": 6500},
                "clumpCenterId_0": {"type": "int2", "interpolation": "uniform", "arity": 2,
                                    "count": 6500},
                "clumpWeight_0": {"type": "float", "interpolation": "vertex", "arity": 1,
                                  "count": 117000},
            } if variant == "clumped" else {},
            "metadataSha256ByFrame": metadata_by_frame,
            "metadataStatistics": first_details}


def pair_proof(env: dict[str, str]) -> dict[str, float]:
    samples = {}
    for variant in VARIANTS:
        for code in PREVIEW_CODES:
            _, points, metadata, details = inspect_bake(
                SCRATCH / f"wind-{variant}-time-{code:03d}-baked.usdc",
                env, variant == "clumped")
            samples[(variant, code)] = (points, metadata, details)
    for code in PREVIEW_CODES:
        left = samples[("clumped", code)][1]
        right = samples[("unclumped", code)][1]
        for name in ("rest", "usdGen:curveId", "usdGen:rootFrame"):
            if left[name] != right[name]:
                raise RuntimeError(f"Pair {name} differs at time {code}")
    clumped = samples[("clumped", PREVIEW_CODES[-1])][0]
    unclumped = samples[("unclumped", PREVIEW_CODES[-1])][0]
    root_delta = max(max(abs(a - b) for a, b in zip(clumped[index], unclumped[index]))
                     for index in range(0, len(clumped), 18))
    tip_delta = max(max(abs(a - b) for a, b in zip(clumped[index], unclumped[index]))
                    for index in range(17, len(clumped), 18))
    if root_delta > 1e-6 or tip_delta < .001:
        raise RuntimeError(f"Native Clump comparison failed: roots {root_delta}, tips {tip_delta}")
    details = samples[("clumped", PREVIEW_CODES[-1])][2]
    memberships = details["clumpIds"]
    weights = details["tipWeights"]
    variances = {}
    selected = 0
    for variant in VARIANTS:
        start = samples[(variant, PREVIEW_CODES[0])][0]
        finish = samples[(variant, PREVIEW_CODES[-1])][0]
        groups: dict[int, list[float]] = {}
        for curve, group in enumerate(memberships):
            if group >= 0 and weights[curve] > .2:
                tip = curve * 18 + 17
                groups.setdefault(group, []).append(finish[tip][0] - start[tip][0])
        selected = sum(len(values) for values in groups.values())
        total = 0.0
        for values in groups.values():
            average = sum(values) / len(values)
            total += sum((value - average) ** 2 for value in values)
        variances[variant] = total / selected
    return {"maxRootDifferenceM": root_delta,
            "maxTipPoseDifferenceM": tip_delta,
            "weightedGroupCurves": selected,
            "clumpedWithinGroupTipDeltaVariance": variances["clumped"],
            "unclumpedWithinGroupTipDeltaVariance": variances["unclumped"],
            "coherenceVarianceRatio": variances["clumped"] / variances["unclumped"]}


def preview_fingerprints(env: dict[str, str]) -> dict[str, object]:
    result: dict[str, object] = {}
    for variant in VARIANTS:
        frames = {}
        for code in PREVIEW_CODES:
            path = SCRATCH / f"wind-{variant}-time-{code:03d}-baked.usdc"
            _, points, metadata, _ = inspect_bake(path, env, variant == "clumped")
            frames[str(code)] = {
                "pointsSha256": hashlib.sha256(repr(points).encode("utf-8")).hexdigest(),
                "metadataSha256": metadata,
            }
        result[variant] = {"sceneSha256": digest(source_path(variant)),
                           "frames": frames}
    return result


def render_one(stage: Path, target: Path, width: int,
               env: dict[str, str], reuse: bool) -> Image.Image:
    from render_pathtraced import render

    if not (reuse and target.exists() and target.stat().st_mtime >= stage.stat().st_mtime):
        render(stage, "/World/Cam", target, width, env)
    with Image.open(target) as image:
        image.load()
        if image.width != width:
            raise RuntimeError(f"Wrong render width: {target}")
        return image.convert("RGB")


def label(image: Image.Image, variant: str, code: int) -> Image.Image:
    result = image.copy()
    draw = ImageDraw.Draw(result)
    text = f"{variant.upper()}  ·  FRAME {code:03d}  ·  {code / 24:.2f} s"
    label_font = wind.font(max(16, round(image.width / 40)))
    box = draw.textbbox((0, 0), text, font=label_font)
    height = box[3] - box[1]
    draw.rounded_rectangle((14, 13, 34 + box[2], 27 + height), radius=8,
                           fill="#13202a")
    draw.text((24, 17), text, font=label_font, fill="#e6eff3")
    return result


def preview(variants: tuple[str, ...], env: dict[str, str],
            bake_env: dict[str, str], width: int,
            reuse: bool, baker: Path,
            cache_identity: dict[str, str]) -> dict[str, object]:
    columns = []
    proofs = {}
    for variant in variants:
        compatible = prepare_source(variant, cache_identity)
        baked = bake_frames(variant, PREVIEW_CODES, bake_env,
                            reuse, compatible, baker, cache_identity)
        proofs[variant] = geometry_proof(variant, baked, env)
        images = [label(render_one(baked[code], SCRATCH /
                        f"wind-{variant}-preview-{width}-{code:03d}.png",
                        width, env, reuse), variant, code) for code in PREVIEW_CODES]
        column = Image.new("RGB", (width, sum(image.height for image in images)))
        y = 0
        for image in images:
            column.paste(image, (0, y))
            y += image.height
        columns.append(column)
    sheet = Image.new("RGB", (sum(column.width for column in columns),
                              max(column.height for column in columns)))
    x = 0
    for column in columns:
        sheet.paste(column, (x, 0))
        x += column.width
    if len(variants) == 2:
        proofs["pair"] = pair_proof(env)
    path = SCRATCH / f"wind-dense-pair-preview-{width}.png"
    sheet.save(path)
    return {"preview": str(path), "geometry": proofs}


def sampled_stage(variant: str, baked: dict[int, Path],
                  env: dict[str, str], reuse: bool) -> Path:
    stage = SCRATCH / f"wind-{variant}-time-sampled.usda"
    if reuse and stage.exists() and stage.stat().st_mtime >= max(
            path.stat().st_mtime for path in baked.values()):
        return stage
    with stage.open("w", encoding="utf-8") as file:
        file.write(f'''#usda 1.0
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
(
    defaultPrim = "World"
    subLayers = [@./wind-{variant}-time-000-baked.usdc@]
    timeCodesPerSecond = 24
    framesPerSecond = 24
    startTimeCode = 0
    endTimeCode = 99
)
over "World" {{
    over "Groom" {{
        over "Baked_Fur" {{
            point3f[] points.timeSamples = {{
''')
        for code in FRAME_CODES:
            file.write(f"                {code}: {wind.usdcat_points_literal(baked[code], env)},\n")
        file.write('''            }
        }
    }
}
''')
    return stage


def batch_render(variant: str, stage: Path, env: dict[str, str],
                 width: int, reuse: bool) -> dict[int, Path]:
    from render_pathtraced import RENDERER, run_logged

    executable = shutil.which("usdrecord", path=env.get("PATH"))
    if not executable:
        raise FileNotFoundError("usdrecord missing from viewer environment")
    recorder = Path(executable)
    if recorder.suffix.lower() == ".cmd" and recorder.with_suffix("").exists():
        recorder = recorder.with_suffix("")
    targets = {code: SCRATCH / f"wind-{variant}-{width}-{code:03d}.000.png"
               for code in FRAME_CODES}
    def ready(path: Path) -> bool:
        if not reuse or not path.exists() or path.stat().st_mtime < stage.stat().st_mtime:
            return False
        try:
            with Image.open(path) as image:
                image.load()
                return image.width == width
        except (OSError, ValueError):
            return False
    pending = [code for code, path in targets.items() if not ready(path)]
    if pending:
        start = min(pending)
        command = [sys.executable, str(recorder), "--renderer", RENDERER,
                   "--imageWidth", str(width), "--disableCameraLight",
                   "--camera", "/World/Cam", "--frames", f"{start}:99",
                   str(stage), str(SCRATCH / f"wind-{variant}-{width}-###.###.png")]
        run_logged(command, SCRATCH / f"wind-{variant}-{width}.render.log", env)
    if len({digest(path) for path in targets.values()}) != 100:
        raise RuntimeError(f"{variant}: raw MoonRay frames are not all distinct")
    return targets


def encode(variant: str, rendered: dict[int, Path], width: int) -> dict[str, object]:
    MEDIA.mkdir(parents=True, exist_ok=True)
    frames_rgb = []
    for code in FRAME_CODES:
        with Image.open(rendered[code]) as image:
            frames_rgb.append(label(image.convert("RGB"), variant, code))
    still = MEDIA / f"wind-{variant}.png"
    frames_rgb[30].save(still)
    thumb = (160, round(frames_rgb[0].height * 160 / width))
    contact = Image.new("RGB", (thumb[0], thumb[1] * 100))
    for code, frame in enumerate(frames_rgb):
        contact.paste(frame.resize(thumb), (0, code * thumb[1]))
    palette = contact.quantize(colors=256, method=Image.Quantize.FASTOCTREE)
    frames = [frame.quantize(palette=palette, dither=Image.Dither.NONE)
              for frame in frames_rgb]
    durations = [40 + (10 if ((code + 1) * 17) // 100 > (code * 17) // 100 else 0)
                 for code in FRAME_CODES]
    output = MEDIA / f"wind-{variant}.gif"
    frames[0].save(output, save_all=True, append_images=frames[1:],
                   duration=durations, loop=0, disposal=2, optimize=False)
    with Image.open(output) as animation:
        if (animation.n_frames != 100 or animation.mode != "P" or
                len(animation.getpalette()) // 3 != 256):
            raise RuntimeError(f"{variant}: GIF lacks 100 frames or full palette")
        decoded = set()
        actual_durations = []
        for code in FRAME_CODES:
            animation.seek(code)
            decoded.add(hashlib.sha256(animation.convert("RGBA").tobytes()).hexdigest())
            actual_durations.append(animation.info["duration"])
        if len(decoded) != 100 or sum(actual_durations) != 4170:
            raise RuntimeError(f"{variant}: GIF lacks distinct frames or correct timing")
    return {"scene": str(source_path(variant).relative_to(ROOT)).replace("\\", "/"),
            "sceneSha256": digest(source_path(variant)),
            "image": str(still.relative_to(ROOT)).replace("\\", "/"),
            "imageSha256": digest(still),
            "animation": str(output.relative_to(ROOT)).replace("\\", "/"),
            "animationSha256": digest(output),
            "durationMs": sum(actual_durations), "frames": 100,
            "distinctDecodedFrames": len(decoded), "paletteEntries": 256}


def verify_independent_renders(env: dict[str, str], width: int = 800
                               ) -> dict[str, object]:
    from render_pathtraced import render

    samples = {}
    rows = []
    for variant in VARIANTS:
        for code, wrong in ((50, 0), (99, 50)):
            baked = SCRATCH / f"wind-{variant}-time-{code:03d}-baked.usdc"
            batch = SCRATCH / f"wind-{variant}-{width}-{code:03d}.000.png"
            wrong_batch = SCRATCH / f"wind-{variant}-{width}-{wrong:03d}.000.png"
            independent = SCRATCH / f"wind-{variant}-independent-{width}-{code:03d}.png"
            render(baked, "/World/Cam", independent, width, env)
            with (Image.open(batch) as batch_image,
                  Image.open(wrong_batch) as wrong_image,
                  Image.open(independent) as independent_image):
                images = [item.convert("RGB") for item in
                          (batch_image, wrong_image, independent_image)]
            if not (images[0].size == images[1].size == images[2].size):
                raise RuntimeError(f"{variant} frame {code}: independent size differs")
            def difference(a: Image.Image, b: Image.Image) -> float:
                return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3
            same = difference(images[0], images[2])
            wrong_score = difference(images[1], images[2])
            if same >= wrong_score:
                raise RuntimeError(
                    f"{variant} frame {code}: batch time sample failed "
                    f"(same {same:.4f}, wrong {wrong_score:.4f})")
            rows.append((images[0], images[2]))
            samples[f"{variant}:{code}"] = {
                "batchSha256": digest(batch),
                "independentSha256": digest(independent),
                "batchVsIndependentMeanAbsoluteRgb": same,
                "wrongFrameTimeCode": wrong,
                "wrongVsIndependentMeanAbsoluteRgb": wrong_score,
            }
    height = sum(left.height for left, _ in rows)
    sheet = Image.new("RGB", (width * 2, height))
    y = 0
    for left, right in rows:
        sheet.paste(left, (0, y))
        sheet.paste(right, (width, y))
        y += left.height
    sheet_path = SCRATCH / "wind-independent-render-qa.png"
    sheet.save(sheet_path)
    report = {"status": "passed", "frameCodes": [50, 99],
              "comparisonSheet": str(sheet_path),
              "comparisonSheetSha256": digest(sheet_path), "samples": samples}
    path = SCRATCH / "wind-independent-render-qa.json"
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
    return report


def finalize_pair(env: dict[str, str]) -> dict[str, object] | None:
    states = {}
    for variant in VARIANTS:
        path = SCRATCH / f"wind-{variant}-state.json"
        if not path.exists():
            return None
        state = json.loads(path.read_text(encoding="utf-8"))
        if state.get("status") != "complete":
            return None
        if state["sceneSha256"] != digest(source_path(variant)):
            raise RuntimeError(f"{variant}: final state has stale scene hash")
        if (state["geometry"]["distinctNativePointHashes"] != 100 or
                len(state["geometry"]["metadataSha256ByFrame"]) != 100 or
                state["media"]["frames"] != 100 or
                state["media"]["distinctDecodedFrames"] != 100):
            raise RuntimeError(f"{variant}: final state lacks 100 native and decoded frames")
        for key, hash_key in (("image", "imageSha256"),
                              ("animation", "animationSha256")):
            asset = ROOT / state["media"][key]
            if not asset.is_file() or digest(asset) != state["media"][hash_key]:
                raise RuntimeError(f"{variant}: final {key} hash changed")
        stage = SCRATCH / f"wind-{variant}-time-sampled.usda"
        if digest(stage) != state["sampledStageSha256"]:
            raise RuntimeError(f"{variant}: time-sampled stage hash changed")
        states[variant] = state
    if states["clumped"]["cacheIdentity"] != states["unclumped"]["cacheIdentity"]:
        raise RuntimeError("Wind pair was baked against different executable/DLL identities")
    qa_path = SCRATCH / "wind-independent-render-qa.json"
    if not qa_path.is_file():
        raise RuntimeError("Independent MoonRay time-sample QA has not run")
    qa = json.loads(qa_path.read_text(encoding="utf-8"))
    if qa.get("status") != "passed" or qa.get("frameCodes") != [50, 99]:
        raise RuntimeError("Independent MoonRay time-sample QA did not pass")
    for variant in VARIANTS:
        for code in (50, 99):
            sample = qa["samples"][f"{variant}:{code}"]
            batch = SCRATCH / f"wind-{variant}-800-{code:03d}.000.png"
            independent = SCRATCH / f"wind-{variant}-independent-800-{code:03d}.png"
            if (digest(batch) != sample["batchSha256"] or
                    digest(independent) != sample["independentSha256"]):
                raise RuntimeError(f"{variant} frame {code}: QA image hash changed")
    pair = pair_proof(env)
    result = {"status": "complete", "timeCodes": [0, 99],
              "timeCodesPerSecond": 24, "pairProof": pair,
              "independentRenderQa": qa,
              "variants": states}
    target = SCRATCH / "wind-pair-final.json"
    temporary = target.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    temporary.replace(target)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=(*VARIANTS, "both"), default="both")
    parser.add_argument("--preview", action="store_true")
    parser.add_argument("--preview-time-code", type=int, choices=PREVIEW_CODES,
                        help="Render one actual preview time code for one variant")
    parser.add_argument("--bake-only", action="store_true")
    parser.add_argument("--reuse-bakes", action="store_true")
    parser.add_argument("--width", type=int)
    parser.add_argument("--baker", type=Path,
                        help="Offline usdGenBakeGroom executable; defaults to docs build")
    parser.add_argument("--runtime-dir", type=Path,
                        help="Directory containing usdGen and usdGenImaging runtime DLLs")
    parser.add_argument("--pin-viewer-runtime", action="store_true",
                        help="Use runtime-dir for viewer DLLs and plugin resources too")
    parser.add_argument("--finalize-pair", action="store_true",
                        help="Validate both completed variants and write the pair manifest")
    parser.add_argument("--verify-independent-renders", action="store_true",
                        help="Render independent frame 50/99 scenes and compare with batch")
    baseline_group = parser.add_mutually_exclusive_group()
    baseline_group.add_argument("--save-isolated-baseline", action="store_true",
                                help="Save isolated native frame 0/25 point and metadata hashes")
    baseline_group.add_argument("--verify-isolated-baseline", action="store_true",
                                help="Require final native frame 0/25 bakes to match isolated hashes")
    args = parser.parse_args()
    variants = VARIANTS if args.variant == "both" else (args.variant,)
    if (args.save_isolated_baseline or args.verify_isolated_baseline) and not (
            args.preview and args.bake_only and len(variants) == 2):
        parser.error("Preview baseline flags require --variant both --preview --bake-only")
    if args.preview_time_code is not None and (not args.preview or len(variants) != 1):
        parser.error("--preview-time-code requires --preview and one variant")
    if len(variants) == 2:
        clumped = source_path("clumped").read_text(encoding="utf-8")
        unclumped = source_path("unclumped").read_text(encoding="utf-8")
        toggle = 'def UsdGenClump "clump" {\n                bool usdGen:enabled = true'
        if clumped.count(toggle) != 1 or clumped.replace(toggle, toggle.replace("true", "false")) != unclumped:
            raise RuntimeError("Dense Wind scenes must differ only by native Clump enabled")
    width = args.width or (480 if args.preview else 800)
    if width < 320:
        parser.error("--width must be at least 320")
    SCRATCH.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(ROOT / "tools" / "docs"))
    from render_pathtraced import baker_path, viewer_environment
    env = viewer_environment()
    baker = (args.baker or baker_path()).resolve()
    runtime_dir = (args.runtime_dir or ROOT / "build").resolve()
    if not baker.is_file():
        raise FileNotFoundError(f"Baker executable missing: {baker}")
    cache_identity = {"bakerSha256": digest(baker)}
    for library in ("usdGen.dll", "usdGenImaging.dll"):
        path = runtime_dir / library
        if not path.is_file():
            raise FileNotFoundError(f"Runtime library missing: {path}")
        cache_identity[library + "Sha256"] = digest(path)
    if args.pin_viewer_runtime:
        env = pin_viewer_runtime(env, runtime_dir)
    # The isolated baker loads its matching engine DLLs. usdrecord must use
    # the viewer's normal plugin path/DLL pair; mixing an isolated DLL with
    # the installed scene-index plugins registers each TfType twice.
    bake_env = env.copy()
    bake_env["PATH"] = os.pathsep.join(
        (str(baker.parent), str(runtime_dir), env["PATH"]))
    if args.finalize_pair:
        result = finalize_pair(env)
        if result is None:
            raise RuntimeError("Both variant checkpoints must be complete first")
        print(json.dumps({"pairManifest": str(SCRATCH / "wind-pair-final.json"),
                          "pairProof": result["pairProof"]}, indent=2))
        return
    if args.verify_independent_renders:
        if width != 800 or len(variants) != 2:
            parser.error("Independent renderer QA requires both variants at width 800")
        print(json.dumps(verify_independent_renders(env, width), indent=2))
        return
    if args.preview:
        if args.preview_time_code is not None and not args.bake_only:
            variant = variants[0]
            code = args.preview_time_code
            compatible = prepare_source(variant, cache_identity)
            baked = bake_frames(variant, (code,), bake_env, args.reuse_bakes,
                                compatible, baker, cache_identity)
            image = label(render_one(baked[code], SCRATCH /
                f"wind-{variant}-main-preview-{width}-{code:03d}-raw.png",
                width, env, args.reuse_bakes), variant, code)
            path = SCRATCH / f"wind-{variant}-main-preview-{width}-{code:03d}.png"
            image.save(path)
            print(json.dumps({"preview": str(path), "previewSha256": digest(path),
                              "nativeGeometry": geometry_proof(variant, baked, env)},
                             indent=2))
            return
        if args.bake_only:
            result = {}
            for variant in variants:
                compatible = prepare_source(variant, cache_identity)
                baked = bake_frames(variant, PREVIEW_CODES, bake_env,
                                    args.reuse_bakes, compatible, baker,
                                    cache_identity)
                result[variant] = geometry_proof(variant, baked, env)
            if len(variants) == 2:
                result["pair"] = pair_proof(env)
                if args.save_isolated_baseline:
                    baseline = {"cacheIdentity": cache_identity,
                                "variants": preview_fingerprints(env)}
                    path = SCRATCH / "wind-isolated-baseline.json"
                    path.write_text(json.dumps(baseline, indent=2) + "\n",
                                    encoding="utf-8")
                    result["isolatedBaseline"] = str(path)
                if args.verify_isolated_baseline:
                    path = SCRATCH / "wind-isolated-baseline.json"
                    isolated = json.loads(path.read_text(encoding="utf-8"))
                    current = preview_fingerprints(env)
                    if isolated["variants"] != current:
                        raise RuntimeError("Final native bakes differ from latest isolated baseline")
                    result["isolatedNativeIdentityVerified"] = True
            print(json.dumps({"geometry": result}, indent=2))
        else:
            print(json.dumps(preview(variants, env, bake_env, width, args.reuse_bakes,
                                     baker, cache_identity), indent=2))
        return
    if args.bake_only:
        parser.error("--bake-only requires --preview")
    result = {}
    for variant in variants:
        prior_path = SCRATCH / f"wind-{variant}-state.json"
        if args.reuse_bakes and prior_path.is_file():
            prior = json.loads(prior_path.read_text(encoding="utf-8"))
            prior_media = prior.get("media", {})
            if (prior.get("status") == "complete" and
                    prior.get("sceneSha256") == digest(source_path(variant)) and
                    prior.get("generatorSha256") == digest(Path(__file__)) and
                    prior.get("cacheIdentity") == cache_identity and
                    prior.get("renderWidth") == width and
                    prior_media.get("frames") == 100 and
                    prior_media.get("distinctDecodedFrames") == 100 and
                    all((ROOT / prior_media.get(key, "missing")).is_file() and
                        digest(ROOT / prior_media[key]) == prior_media.get(hash_key)
                        for key, hash_key in (("image", "imageSha256"),
                                              ("animation", "animationSha256")))):
                result[variant] = prior_media
                print(f"{variant}: verified complete checkpoint; skipping", flush=True)
                continue
        state: dict[str, object] = {
            "status": "preparing", "variant": variant,
            "scene": str(source_path(variant).relative_to(ROOT)).replace("\\", "/"),
            "sceneSha256": digest(source_path(variant)),
            "generatorSha256": digest(Path(__file__)),
            "cacheIdentity": cache_identity,
            "frameRange": [0, 99], "timeCodesPerSecond": 24,
            "renderWidth": width,
        }
        write_state(variant, state)
        compatible = prepare_source(variant, cache_identity)
        state.update(status="baking", cachedBakesCompatible=compatible,
                     completedBakeCount=0)
        write_state(variant, state)
        def bake_progress(count: int, code: int) -> None:
            state.update(completedBakeCount=count, lastBakeTimeCode=code)
            write_state(variant, state)
        baked = bake_frames(variant, FRAME_CODES, bake_env, args.reuse_bakes,
                            compatible, baker, cache_identity, bake_progress)
        geometry = geometry_proof(variant, baked, env)
        loop = bake_frames(variant, (100,), bake_env, args.reuse_bakes,
                           compatible, baker, cache_identity)[100]
        _, start_points = wind.cooked_points(baked[0], env)
        _, end_points = wind.cooked_points(loop, env)
        seam = max(max(abs(a - b) for a, b in zip(start, end))
                   for start, end in zip(start_points, end_points))
        if seam > 1e-5:
            raise RuntimeError(f"{variant}: frame 100 does not close the Wind loop ({seam})")
        geometry["frame100LoopErrorM"] = seam
        state.update(status="baked", geometry=geometry)
        write_state(variant, state)
        stage = sampled_stage(variant, baked, env, args.reuse_bakes)
        state.update(status="rendering", sampledStageSha256=digest(stage),
                     completedRenderCount=sum(
                         (SCRATCH / f"wind-{variant}-{width}-{code:03d}.000.png").is_file()
                         for code in FRAME_CODES))
        write_state(variant, state)
        frames = batch_render(variant, stage, env, width, args.reuse_bakes)
        state.update(status="rendered", distinctRawFrameHashes=100)
        write_state(variant, state)
        result[variant] = {**encode(variant, frames, width), "geometry": geometry,
                           "sampledStageSha256": digest(stage)}
        state.update(status="complete", media=result[variant])
        write_state(variant, state)
        print(json.dumps({variant: result[variant]}, indent=2), flush=True)
    if len(variants) == 2:
        print("Both variants rendered; run independent renderer QA before pair finalization",
              flush=True)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
