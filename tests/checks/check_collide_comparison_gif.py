# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Prove repeated genuine frames keep their timing and pixels in the companion."""
import importlib.util
from pathlib import Path
import tempfile
from unittest.mock import patch

from PIL import Image

source = Path(__file__).resolve().parents[2] / "tools/docs/encode_collide_comparison_gif.py"
spec = importlib.util.spec_from_file_location("encode_collide_comparison_gif", source)
encoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(encoder)


def check() -> None:
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        paths = []
        for index in range(100):
            path = folder / ("frame-%03d.png" % index)
            with Image.new("RGB", (12, 8), (50, 100, 150)) as image:
                image.save(path)
            paths.append(path)
        originals = [encoder.sha256(path) for path in paths]
        qa = encoder.write_gif(paths, folder / "identical.gif")
        assert qa["gifFrames"] == 100 and qa["gifUniqueFrames"] == 1
        assert qa["gifDurationMs"] == 4170
        assert qa["durationsMs"] == encoder.frame_durations()
        assert qa["pillowVersion"] and qa["privateGifWriter"]["coalescingBypassed"]
        assert [entry["timeCode"] for entry in qa["sourcePngs"]] == list(range(100))
        assert [encoder.sha256(path) for path in paths] == originals
        with Image.open(folder / "identical.gif") as gif:
            for index in range(100):
                gif.seek(index)
                with gif.convert("RGB") as rgb:
                    assert set(rgb.getdata()) == {(50, 100, 150)}
        # A separate repeated-run sequence also proves changes remain ordered.
        for index in range(50, 100):
            with Image.new("RGB", (12, 8), (150, 100, 50)) as image:
                image.save(paths[index])
        qa = encoder.write_gif(paths, folder / "two-poses.gif")
        assert qa["gifFrames"] == 100 and qa["gifUniqueFrames"] == 2
        assert qa["gifDurationMs"] == 4170 and qa["durationsMs"] == encoder.frame_durations()
        assert qa["decodedRgbSha256"][:50] == [qa["decodedRgbSha256"][0]] * 50
        assert qa["decodedRgbSha256"][50:] == [qa["decodedRgbSha256"][50]] * 50
        assert qa["decodedRgbSha256"][0] != qa["decodedRgbSha256"][50]
        with Image.open(folder / "two-poses.gif") as gif:
            for index in range(100):
                gif.seek(index)
                expected = (50, 100, 150) if index < 50 else (150, 100, 50)
                with gif.convert("RGB") as rgb:
                    assert set(rgb.getdata()) == {expected}
                assert gif.info["duration"] == encoder.frame_durations()[index]
        with patch.object(encoder.GifImagePlugin, "_write_frame_data", None):
            try:
                encoder.write_gif(paths, folder / "unsupported.gif")
            except RuntimeError as exc:
                assert "refusing a coalescing fallback" in str(exc)
            else:
                raise AssertionError("Unsupported private writer was accepted")
            assert not (folder / "unsupported.gif").exists()
        try:
            encoder.write_gif(paths, folder / "two-poses.gif")
        except ValueError:
            pass
        else:
            raise AssertionError("Existing output was overwritten")
    print("PASS: 100 identical records, 4170 ms, exact decoded pixels, ordered repeated runs, immutable sources")


if __name__ == "__main__":
    check()
