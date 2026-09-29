"""Make a labelled, real-time GIF from capture_progressive_felt.py output.

Usage: python tools/encode_progressive_gif.py renders/progressive-felt
Raw viewport PNGs and capture.json are never changed.
"""

import json
import os
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def _ui_font(size):
    candidates = []
    env = os.environ.get("USDGEN_UI_FONT")
    if env:
        candidates.append(Path(env))
    candidates.extend([
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
        Path("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"),
        Path("C:/Windows/Fonts/segoeui.ttf"),
    ])
    for path in candidates:
        if path.is_file():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


def main():
    directory = Path(sys.argv[1])
    data = json.loads((directory / "capture.json").read_text())
    rows = data["frames"]
    assert rows, "No captured frames"
    font = _ui_font(19)
    small = _ui_font(13)

    def annotate(source, elapsed, subtitle):
        image = Image.open(source).convert("RGB")
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle((12, 11, 310, 65), radius=8, fill=(16, 19, 24))
        draw.text((23, 15), "Storm  |  async tile generation", font=font,
                  fill=(245, 244, 235))
        draw.text((23, 41), "%s  |  elapsed %.2fs" % (subtitle, elapsed),
                  font=small, fill=(205, 209, 216))
        return image

    images = [annotate(directory / "bare.png", 0.0, "bare emitter")]
    durations = [600]
    for i, row in enumerate(rows):
        images.append(annotate(directory / row["file"], row["seconds"],
                               "publication count %d" % row["publishes"]))
        if i + 1 < len(rows):
            # GIF centiseconds, rounded from measured wall-clock frame spacing.
            delta = rows[i + 1]["seconds"] - row["seconds"]
            durations.append(max(20, round(delta * 1000 / 10) * 10))
        else:
            durations.append(2200)

    # A single palette prevents color drift as hair coverage changes.
    palette_source = images[-1].quantize(colors=256)
    encoded = [im.quantize(palette=palette_source, dither=Image.Dither.FLOYDSTEINBERG)
               for im in images]
    path = directory / "progressive-felt.gif"
    encoded[0].save(path, save_all=True, append_images=encoded[1:],
                    duration=durations, loop=0, optimize=True, disposal=2)
    print("GIF: %s (%d frames, %.2fs playback)" %
          (path, len(encoded), sum(durations) / 1000))

    picks = [0, 1, max(1, len(images) // 4), max(1, len(images) // 2),
             max(1, 3 * len(images) // 4), len(images) - 1]
    preview = Image.new("RGB", (640 * 3, 640 * 2), (16, 19, 24))
    for i, index in enumerate(picks):
        preview.paste(images[index], ((i % 3) * 640, (i // 3) * 640))
    preview.save(directory / "contact.png")


if __name__ == "__main__":
    main()
