# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Encode a companion fur-only GIF without merging identical genuine poses.

This companion permits naturally identical decoded frames. The primary
encode_collide_gif.py retains its separate 100-unique-frame requirement.
No renderer, geometry generator or source-image mutation is involved.
"""
from __future__ import annotations

import argparse
import hashlib
import inspect
import json
import os
from pathlib import Path
import tempfile

from PIL import GifImagePlugin, Image, __version__ as PILLOW_VERSION


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def frame_durations() -> list[int]:
    # GIF centiseconds approximate 100/24 seconds with 83 x 40 + 17 x 50 ms.
    return [50 if (i + 1) * 17 // 100 > i * 17 // 100 else 40 for i in range(100)]


def writer_capabilities() -> dict:
    capabilities = {"globalHeader": "GifImagePlugin._get_global_header",
                    "frameData": "GifImagePlugin._write_frame_data",
                    "fullCanvasFrames": True, "coalescingBypassed": True}
    try:
        header = getattr(GifImagePlugin, "_get_global_header")
        writer = getattr(GifImagePlugin, "_write_frame_data")
        if not callable(header) or not callable(writer):
            raise TypeError("GIF writer functions are not callable")
        inspect.signature(header).bind(None, {})
        inspect.signature(writer).bind(None, None, (0, 0), {})
    except (AttributeError, TypeError, ValueError) as exc:
        raise RuntimeError("Pillow %s lacks the required explicit GIF frame writer; "
                           "refusing a coalescing fallback: %s" % (PILLOW_VERSION, exc)) from exc
    return capabilities


def write_gif(images: list[Path], target: Path) -> dict:
    capabilities = writer_capabilities()
    images = [Path(path) for path in images]
    target = Path(target)
    if len(images) != 100 or len({path.resolve() for path in images}) != 100:
        raise ValueError("Expected 100 distinct ordered source PNG files")
    if target.suffix.lower() != ".gif" or target.exists():
        raise ValueError("Choose a fresh .gif output path")
    if any(path.suffix.lower() != ".png" for path in images):
        raise ValueError("All source frames must be PNGs")
    source_hashes = [sha256(path) for path in images]
    durations = frame_durations()
    atlas = Image.new("RGB", (1600, 1600))
    palette = None
    partial = None
    dimensions = None
    source_rgb_hashes, expected_rgb_hashes = [], []
    try:
        for index, path in enumerate(images):
            with Image.open(path) as source:
                if source.format != "PNG":
                    raise ValueError("Source file is not a PNG: %s" % path.name)
                if dimensions is None:
                    dimensions = source.size
                    if min(dimensions) <= 0 or max(dimensions) > 65535:
                        raise ValueError("Source dimensions do not fit GIF")
                elif source.size != dimensions:
                    raise ValueError("Source frame dimensions differ")
                with source.convert("RGB") as rgb:
                    with rgb.resize((160, 160), Image.Resampling.LANCZOS) as thumbnail:
                        atlas.paste(thumbnail, ((index % 10) * 160, (index // 10) * 160))
        palette = atlas.quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                                 dither=Image.Dither.NONE)
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix=".collide-comparison-", suffix=".gif",
                                         dir=target.parent)
        partial = Path(temporary)
        with os.fdopen(fd, "wb") as output:
            for index, path in enumerate(images):
                with Image.open(path) as source, source.convert("RGB") as rgb:
                    source_rgb_hashes.append(hashlib.sha256(rgb.tobytes()).hexdigest())
                    with rgb.quantize(palette=palette,
                                      dither=Image.Dither.FLOYDSTEINBERG) as frame:
                        # A shared opaque palette and full-canvas records
                        # preserve every sample, including repeated pixels.
                        frame.info.clear()
                        with frame.convert("RGB") as expected:
                            expected_rgb_hashes.append(hashlib.sha256(expected.tobytes()).hexdigest())
                        if index == 0:
                            for block in GifImagePlugin._get_global_header(frame, {"loop": 0}):
                                output.write(block)
                        GifImagePlugin._write_frame_data(output, frame, (0, 0),
                            {"duration": durations[index], "disposal": 2,
                             "optimize": False, "include_color_table": False})
            output.write(b";")
        decoded_hashes, decoded_durations = [], []
        with Image.open(partial) as gif:
            count = gif.n_frames
            if count != 100 or gif.size != dimensions:
                raise RuntimeError("Decoded GIF frame-count/dimensions gate failed")
            for index in range(count):
                gif.seek(index)
                with gif.convert("RGB") as rgb:
                    decoded_hashes.append(hashlib.sha256(rgb.tobytes()).hexdigest())
                decoded_durations.append(int(gif.info.get("duration", 0)))
        if decoded_hashes != expected_rgb_hashes:
            raise RuntimeError("Decoded GIF differs from the ordered quantized source pixels")
        if decoded_durations != durations or sum(decoded_durations) != 4170:
            raise RuntimeError("Decoded GIF timing gate failed")
        if [sha256(path) for path in images] != source_hashes:
            raise RuntimeError("Source PNGs changed during encoding")
        if target.exists():
            raise FileExistsError("Output appeared during encoding; refusing to replace it")
        partial.rename(target)
        partial = None
        return {"gif": str(target), "gifSha256": sha256(target), "gifFrames": count,
                "gifUniqueFrames": len(set(decoded_hashes)), "gifDurationMs": sum(durations),
                "gifFps": 24, "dimensions": list(dimensions), "durationsMs": durations,
                "sourcePngs": [{"timeCode": i, "name": path.name, "sha256": digest,
                                "durationMs": durations[i], "rgbSha256": source_rgb_hashes[i]}
                               for i, (path, digest) in enumerate(zip(images, source_hashes))],
                "quantizedSourceRgbSha256": expected_rgb_hashes,
                "decodedRgbSha256": decoded_hashes, "decodedMatchesQuantizedSources": True,
                "identicalFramesPreserved": True, "uniqueFrameRequirement": False,
                "encoding": "shared 256-color all-frame thumbnail median-cut palette; explicit Floyd–Steinberg dithering; one full GIF image record per source sample",
                "encoderSha256": sha256(Path(__file__)), "encodeOnly": True,
                "pillowVersion": PILLOW_VERSION, "privateGifWriter": capabilities,
                "renderAttempted": False}
    finally:
        atlas.close()
        if palette is not None:
            palette.close()
        if partial is not None:
            partial.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--filename-pattern", default="collide-cut-final-800-{frame:03d}.000.png",
                        help="Source basename pattern with {frame} for time codes 0 through 99")
    parser.add_argument("--original-provenance", type=Path)
    args = parser.parse_args()
    qa_path = args.output.with_suffix(".qa.json")
    if args.output.exists() or qa_path.exists():
        raise FileExistsError("Choose a fresh candidate GIF and QA output")
    images = [args.source_dir / args.filename_pattern.format(frame=i) for i in range(100)]
    qa = write_gif(images, args.output)
    if args.original_provenance:
        qa["originalProvenance"] = json.loads(args.original_provenance.read_text())
        qa["originalProvenanceSha256"] = sha256(args.original_provenance)
    qa_path.write_text(json.dumps(qa, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: qa[key] for key in
                     ("gif", "gifSha256", "gifFrames", "gifUniqueFrames", "gifDurationMs")}))


if __name__ == "__main__":
    main()
