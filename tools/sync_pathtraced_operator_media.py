#!/usr/bin/env python3
"""Record verified MoonRay operator captures without replacing other media data.

Run after ``tools/docs/render_pathtraced.py --batch-operators`` completes.
The hash check prevents an older Storm still from being mislabeled as a baked,
path-traced result. Utility fixtures, Collide/Wind comparisons and animations are
managed by their own capture scripts.
"""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / "renders" / "docs" / "pathtrace"
REGISTRY = ROOT / "docs" / "reference" / "media.json"
PATCH_STEMS = (
    "bend", "clump", "curl", "curve-source", "direction", "displace",
    "expr-op", "grow", "length", "noise", "reference-source", "resample",
    "scale", "scatter", "sculpt-layer", "smooth", "straighten", "wave",
    "width",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    for stem in PATCH_STEMS:
        scene = ROOT / "examples" / "docs" / "operators" / f"{stem}.usda"
        published = ROOT / "docs" / "site" / "media" / f"{stem}.png"
        baked = SCRATCH / f"{stem}-baked.usdc"
        rendered = SCRATCH / f"{stem}-moonray.png"
        if not all(path.is_file() for path in (scene, published, baked, rendered)):
            raise FileNotFoundError(f"Incomplete path-traced capture for {stem}")
        image_hash = digest(published)
        if image_hash != digest(rendered):
            raise ValueError(f"Published image differs from MoonRay capture: {stem}")
        matches = [record for record in registry["operators"].values()
                   if record.get("scene") == scene.relative_to(ROOT).as_posix()]
        if len(matches) != 1:
            raise ValueError(f"Expected one media record for {stem}, got {len(matches)}")
        record = matches[0]
        record.update({
            "renderer": "MoonRay / HdMoonrayRendererDebugPlugin",
            "generator": "tools/docs/render_pathtraced.py",
            "provenance": "The checked-in procedural USD scene was cooked by the usdGen compiler and scheduler into native BasisCurves using tools/docs/bake_groom.cpp, then path-traced by MoonRay with the authored Preview material. No illustrative geometry was substituted.",
            "sceneSha256": digest(scene),
            "imageSha256": image_hash,
        })
    for record in registry["operators"].values():
        image = record.get("image")
        if image:
            image_path = ROOT / image
            if image_path.is_file():
                record["imageSha256"] = digest(image_path)
        scene = record.get("scene")
        if scene:
            scene_path = ROOT / scene
            if scene_path.is_file():
                record["sceneSha256"] = digest(scene_path)
        animation = record.get("animation")
        if animation:
            animation_path = ROOT / animation
            if animation_path.is_file():
                record["animationSha256"] = digest(animation_path)
    REGISTRY.write_text(json.dumps(registry, indent=2) + "\n", encoding="utf-8")
    print(f"Verified and recorded {len(PATCH_STEMS)} path-traced operator stills")


if __name__ == "__main__":
    main()
