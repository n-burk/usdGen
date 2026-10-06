#!/usr/bin/env python3
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Check the explicit 100-frame Collide/Wind animation deliverables.

This separate check requires Pillow; the offline manual checker remains
standard-library-only. Run after regenerating both native animation examples.
Decoded image uniqueness includes any frame labels; native geometry uniqueness
must be established separately by the renderer's bake validation. Only an
explicit Collide secondary visibility comparison may contain repeated pixels;
this check does not establish its geometry or visibility parity.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
OPERATORS = ("UsdGenCollide", "UsdGenWind")


def animation_records(operator, record):
    """Match published variants; always retain Collide's strict primary gate."""
    variants = record.get("variants") or []
    # Wind's paired variants replace its primary visual in the page template.
    # Collide must retain its primary acceptance gate even with a companion.
    records = [(record, False)] if operator == "UsdGenCollide" or not variants else []
    records.extend((variant, True) for variant in variants if variant.get("animation"))
    return records


def check_operator(operator, record, image_module, *, primary_record=None, secondary=False):
    comparison = record.get("visibilityOnlyComparison")
    allow_repeats = False
    if comparison is not None:
        if (operator != "UsdGenCollide" or not secondary or
                comparison != "/World/Shield" or primary_record is None or
                record["scene"] != primary_record["scene"] or
                record["animation"] == primary_record["animation"]):
            raise ValueError("repeated pixels require a distinct Collide secondary /World/Shield comparison of the primary public scene")
        unique_metadata = record.get("animationUniqueFrames")
        if type(unique_metadata) is not int or not 1 <= unique_metadata <= 100:
            raise ValueError("comparison must record animationUniqueFrames as an integer from 1 to 100")
        allow_repeats = True
    scene = ROOT / record["scene"]
    source = scene.read_text(encoding="utf-8")
    for field, expected in (("startTimeCode", 0), ("endTimeCode", 99)):
        match = re.search(r"^\s*" + field + r"\s*=\s*([0-9.+-]+)\s*$",
                          source, re.MULTILINE)
        if not match or float(match.group(1)) != expected:
            raise ValueError("scene must author %s = %d" % (field, expected))
    animation = ROOT / record["animation"]
    with image_module.open(animation) as gif:
        frame_count = getattr(gif, "n_frames", 1)
        if gif.format != "GIF" or frame_count != 100:
            raise ValueError("expected a GIF with exactly 100 encoded frames; got %s/%d" %
                             (gif.format, frame_count))
        hashes = set()
        durations = []
        for frame in range(frame_count):
            gif.seek(frame)
            with gif.convert("RGBA") as rgba:
                hashes.add(hashlib.sha256(rgba.tobytes()).hexdigest())
            durations.append(int(gif.info.get("duration", 0)))
    if any(duration <= 0 for duration in durations) or sum(durations) != 4170:
        raise ValueError("expected positive frame durations totaling 4170 ms; got %d ms" % sum(durations))
    if allow_repeats and len(hashes) != unique_metadata:
        raise ValueError("comparison decoded uniqueness differs from animationUniqueFrames: %d != %d" %
                         (len(hashes), unique_metadata))
    if not allow_repeats and len(hashes) != 100:
        raise ValueError("expected 100 distinct decoded frame images; got %d" % len(hashes))
    name = operator + (": " + record["label"] if record.get("label") else "")
    print("ok: %s: 100 encoded frames, %d distinct decoded images, 4170 ms, scene timeline 0–99%s" %
          (name, len(hashes), "; explicit visibility comparison (parity requires separate evidence)" if allow_repeats else ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--op", action="append", choices=OPERATORS,
                        help="check only one operator; default checks both")
    args = parser.parse_args()
    try:
        from PIL import Image
    except ImportError:
        print("FAIL: check_operator_gifs.py requires Pillow", file=sys.stderr)
        return 1
    records = json.loads((ROOT / "docs/reference/media.json").read_text(encoding="utf-8"))
    failures = 0
    for operator in args.op or OPERATORS:
        record = records["operators"][operator]
        for variant, secondary in animation_records(operator, record):
            name = operator + (": " + variant["label"] if variant.get("label") else "")
            try:
                check_operator(operator, variant, Image, primary_record=record, secondary=secondary)
            except (OSError, ValueError, KeyError, EOFError) as error:
                print("FAIL: %s: %s" % (name, error), file=sys.stderr)
                failures += 1
    return int(bool(failures))


if __name__ == "__main__":
    sys.exit(main())
