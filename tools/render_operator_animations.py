#!/usr/bin/env python3
"""Encode real parameter sweeps from live usdGen Storm renders as looping GIFs."""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess

from operator_recipes import RECIPES
from render_operator_examples import ROOT, SCRATCH, MEDIA, MEDIA_JSON, render, scene, slug


# Seven actual engine evaluations per clip; reverse playback reuses those same
# captured PNGs.  Values keep upstream topology, seed, camera and light fixed.
SWEEPS = {
    "UsdGenClump": ("float usdGen:clump:amount = 0.72", "float usdGen:clump:amount = {}",
                    [0, .12, .24, .36, .48, .60, .72], "clump:amount"),
    "UsdGenCurl": ("float usdGen:radius = 0.018", "float usdGen:radius = {}",
                   [0, .003, .006, .009, .012, .015, .018], "radius"),
    "UsdGenNoise": ("float usdGen:noise:magnitude = 0.012", "float usdGen:noise:magnitude = {}",
                    [0, .002, .004, .006, .008, .010, .012], "noise:magnitude"),
    "UsdGenLength": ("float usdGen:length:value = 0.145", "float usdGen:length:value = {}",
                     [.07, .09, .11, .13, .15, .17, .19], "length:value"),
    "UsdGenWind": ("float usdGen:constStrength = 0.085", "float usdGen:constStrength = {}",
                   [0, .016, .032, .048, .064, .080, .096], "constStrength"),
    "UsdGenWidth": ("float usdGen:width = 0.0012", "float usdGen:width = {}",
                    [.0004, .00055, .0007, .00085, .001, .0011, .0012], "width"),
}


def gif_for(op_id: str, width: int) -> Path:
    needle, pattern, values, parameter = SWEEPS[op_id]
    recipe = RECIPES[op_id]
    original = recipe["ops"]
    if original.count(needle) != 1:
        raise ValueError(f"{op_id}: expected exactly one {needle!r}, got {original.count(needle)}")
    frame_dir = SCRATCH / "animation" / slug(op_id)
    frame_dir.mkdir(parents=True, exist_ok=True)
    for i, value in enumerate(values):
        ops = original.replace(needle, pattern.format(f"{value:.6g}"))
        stage_path = frame_dir / f"stage-{i:02d}.usda"
        stage_path.write_text(scene(ops=ops,
                                    extra_prims=recipe.get("extra_prims", ""),
                                    description_overrides=recipe.get("description_overrides", ""),
                                    sky_asset="../../../../../examples/maps/StinsonBeach.hdr"),
                              encoding="utf-8")
        render(stage_path, frame_dir / f"frame-{i:02d}.png", width)
    # A genuine ping-pong of seven rendered states; copying reverse frames
    # cannot invent intermediate states.
    order = list(range(len(values))) + list(range(len(values)-2, 0, -1))
    for i, original_index in enumerate(order):
        shutil.copyfile(frame_dir / f"frame-{original_index:02d}.png",
                        frame_dir / f"loop-{i:02d}.png")
    palette = frame_dir / "palette.png"
    gif = MEDIA / f"{slug(op_id)}.gif"
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-framerate", "5",
                    "-i", str(frame_dir / "loop-%02d.png"), "-vf", "palettegen",
                    str(palette)], check=True)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-framerate", "5",
                    "-i", str(frame_dir / "loop-%02d.png"), "-i", str(palette),
                    "-lavfi", "paletteuse=dither=bayer:bayer_scale=3",
                    "-loop", "0", str(gif)], check=True)
    registry = json.loads(MEDIA_JSON.read_text(encoding="utf-8"))
    registry["operators"][op_id]["animation"] = gif.relative_to(ROOT).as_posix()
    registry["operators"][op_id]["animationProvenance"] = (
        f"Seven live Storm scene-index captures sweep usdGen:{parameter} with fixed seed, camera and lighting; reverse playback reuses captured frames.")
    MEDIA_JSON.write_text(json.dumps(registry, indent=2)+"\n", encoding="utf-8")
    return gif


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--op", action="append", choices=sorted(SWEEPS))
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--width", type=int, default=768)
    args = parser.parse_args()
    if not args.all and not args.op:
        parser.error("pass --all or at least one --op")
    MEDIA.mkdir(parents=True, exist_ok=True)
    for op_id in (SWEEPS if args.all else args.op):
        if op_id == "UsdGenWind":
            print("DEFER UsdGenWind: run python tools/render_wind_diagnostic.py "
                  "to regenerate its comparison and sweep", file=sys.stderr)
            continue
        print(gif_for(op_id, args.width), flush=True)


if __name__ == "__main__":
    main()
