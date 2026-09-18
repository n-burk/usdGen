#!/usr/bin/env python3
"""Box-downsample a supersampled render by an integer factor, averaging in
LINEAR light.

    python downsample_linear.py <in.png> <out.png> <factor>

Used by record_usd.ps1 -Supersample to build offline anti-aliasing references.
Averaging sRGB code values directly would darken every edge -- a mid-grey
sRGB value is not the mid-point of the two luminances it sits between -- and
edges are the whole point of the reference, so the transfer function is undone
before the box filter and reapplied after.

READ THIS BEFORE COMPARING THE RESULT TO A 1x RENDER. usdrecord writes RGBA,
and on a scene with no backdrop the hair's alpha is its coverage. This filter
keeps colour and coverage in separate channels -- the RGB it writes is the
average colour OF THE COVERED SAMPLES (straight, not premultiplied), exactly as
the input encodes it. So comparing `mean(RGB)` between a 1x render and its
reference compares two different things: in the 1x image the uncovered pixels
contribute zeros and drag the mean down, while here they contribute nothing at
all. Compare `mean(alpha)` for coverage, or `mean(RGB * alpha)` for the value
composited over black -- both round-trip exactly through this filter. Getting
this wrong makes a correct renderer look like it is losing half its energy.
"""
import sys

import numpy as np
from PIL import Image


def srgb_to_linear(x):
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(x):
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * x ** (1.0 / 2.4) - 0.055)


def main():
    src, dst, factor = sys.argv[1], sys.argv[2], int(sys.argv[3])
    im = Image.open(src)
    has_alpha = im.mode in ("RGBA", "LA") or "transparency" in im.info
    im = im.convert("RGBA" if has_alpha else "RGB")
    a = np.asarray(im, dtype=np.float64) / 255.0

    h, w, c = a.shape
    if h % factor or w % factor:
        # usdrecord derives height from the camera aperture, so it is not
        # guaranteed to be a multiple of the factor. Trim rather than resample
        # again: at most factor-1 pixels, off the bottom/right edge.
        a = a[:h - (h % factor), :w - (w % factor)]
        h, w = a.shape[0], a.shape[1]

    rgb = srgb_to_linear(a[..., :3])
    if c == 4:
        # Composite weighting: a transparent supersample carries no colour, so
        # averaging colour without weighting by alpha would let the background
        # bleed into every edge.
        alpha = a[..., 3:4]
        num = (rgb * alpha).reshape(h // factor, factor, w // factor, factor, 3)
        den = alpha.reshape(h // factor, factor, w // factor, factor, 1)
        num = num.sum(axis=(1, 3))
        den = den.sum(axis=(1, 3))
        rgb = np.divide(num, den, out=np.zeros_like(num), where=den > 1e-9)
        out_a = den / float(factor * factor)
        out = np.concatenate([linear_to_srgb(rgb), out_a], axis=2)
    else:
        rgb = rgb.reshape(h // factor, factor, w // factor, factor, 3).mean(axis=(1, 3))
        out = linear_to_srgb(rgb)

    out = np.clip(out, 0.0, 1.0)
    Image.fromarray((out * 255.0 + 0.5).astype(np.uint8)).save(dst)
    print("downsampled %s -> %s (%dx, linear box)" % (src, dst, factor))


if __name__ == "__main__":
    main()
