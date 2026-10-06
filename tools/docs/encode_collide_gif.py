# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Encode accepted render PNGs with a shared palette; never render or cook."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_gif(images: list[Path], target: Path) -> dict:
    if len(images) != 100:
        raise ValueError("Expected exactly 100 accepted PNGs")
    source_hashes = [sha256(path) for path in images]
    # Every pose contributes equally. The atlas retains both sphere shading
    # and fur colors without a frame-local palette changing during playback.
    atlas = Image.new("RGB", (1600, 1600))
    for index, path in enumerate(images):
        with Image.open(path) as image:
            atlas.paste(image.convert("RGB").resize((160, 160), Image.Resampling.LANCZOS),
                        ((index % 10) * 160, (index // 10) * 160))
    palette = atlas.quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                             dither=Image.Dither.NONE)
    frames = []
    durations = [50 if ((i + 1) * 17 // 100 > i * 17 // 100) else 40
                 for i in range(100)]
    try:
        for path in images:
            with Image.open(path) as image:
                frames.append(image.convert("RGB").quantize(
                    palette=palette, dither=Image.Dither.FLOYDSTEINBERG))
        target.parent.mkdir(parents=True, exist_ok=True)
        frames[0].save(target, save_all=True, append_images=frames[1:], loop=0,
                       duration=durations, disposal=2, optimize=False)
    finally:
        for frame in frames:
            frame.close()
        palette.close()
        atlas.close()
    decoded_hashes, decoded_durations = [], []
    sheet = Image.new("RGB", (1200, 900), "white")
    selected = [0, 13, 18, 25, 27, 31, 50, 75, 99]
    with Image.open(target) as gif:
        count = gif.n_frames
        size = gif.size
        for index in range(count):
            gif.seek(index)
            rgb = gif.convert("RGB")
            decoded_hashes.append(hashlib.sha256(rgb.tobytes()).hexdigest())
            decoded_durations.append(int(gif.info.get("duration", 0)))
            if index in (0, 27, 99):
                rgb.save(target.parent / f"decoded-{index:03d}.png")
            if index in selected:
                slot = selected.index(index)
                thumb = rgb.copy()
                thumb.thumbnail((400, 270), Image.Resampling.LANCZOS)
                x, y = (slot % 3) * 400, (slot // 3) * 300
                sheet.paste(thumb, (x, y))
                ImageDraw.Draw(sheet).text((x + 8, y + 276), f"Frame {index}", fill="black")
            rgb.close()
    sheet.save(target.parent / "decoded-contact-sheet.png")
    if count != 100 or len(set(decoded_hashes)) != 100 or decoded_durations != durations:
        raise RuntimeError("Decoded GIF frame uniqueness or timing gate failed")
    if [sha256(path) for path in images] != source_hashes:
        raise RuntimeError("Source PNGs changed during encoding")
    return {"gif": str(target), "gifSha256": sha256(target), "gifFrames": count,
            "gifUniqueFrames": len(set(decoded_hashes)), "gifDurationMs": sum(durations),
            "gifFps": 24, "dimensions": list(size), "durationsMs": durations,
            "sourcePngs": [{"name": path.name, "sha256": digest}
                           for path, digest in zip(images, source_hashes)],
            "decodedRgbSha256": decoded_hashes,
            "encoding": "shared 256-color all-frame thumbnail median-cut palette; explicit Floyd–Steinberg dithering",
            "encoderSha256": sha256(Path(__file__)), "encodeOnly": True,
            "renderAttempted": False}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--original-provenance", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError("Choose a fresh candidate GIF output")
    paths = [args.source_dir / f"collide-cut-final-800-{i:03d}.000.png"
             for i in range(100)]
    qa = write_gif(paths, args.output)
    qa["driverSha256"] = sha256(Path(__file__).with_name("render_collide_cut_accepted.py"))
    if args.original_provenance:
        qa["originalProvenance"] = json.loads(args.original_provenance.read_text())
        qa["originalProvenanceSha256"] = sha256(args.original_provenance)
    args.output.with_suffix(".qa.json").write_text(json.dumps(qa, indent=2) + "\n")
    print(json.dumps({key: qa[key] for key in
                      ("gif", "gifSha256", "gifFrames", "gifUniqueFrames", "gifDurationMs")}))


if __name__ == "__main__":
    main()
