#!/usr/bin/env python3
"""PW-4 pixel diff tool.

usage: diff_png.py <a.png> <b.png>
Prints sha256 of each file, mean absolute pixel difference, and the fraction
of pixels differing by more than 8/255 per channel.
"""
import hashlib, sys
import numpy as np
from PIL import Image

def load(p):
    im = Image.open(p).convert("RGB")
    return np.asarray(im, dtype=np.int16), hashlib.sha256(open(p, "rb").read()).hexdigest()

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, ha = load(sys.argv[1])
    b, hb = load(sys.argv[2])
    print(f"A={sys.argv[1]} sha256={ha} shape={a.shape}")
    print(f"B={sys.argv[2]} sha256={hb} shape={b.shape}")
    if a.shape != b.shape:
        print("SHAPE MISMATCH")
        sys.exit(1)
    d = np.abs(a - b)
    print(f"meanAbsDiff={d.mean():.4f} maxAbsDiff={d.max()} "
          f"fracPixelsDiff>8={float((d.max(axis=2) > 8).mean()):.6f}")

if __name__ == "__main__":
    main()
