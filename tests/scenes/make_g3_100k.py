#!/usr/bin/env python3
"""Deterministic G3 @100k curve fixture (plan/07 §2 shape, plan/02 §2.19.1).

curveId = UsdGenHash64(seed, faceIndex, k, kSaltScatter) — SplitMix64 masked
to 2**64; hairId = float(uint32(H(curveId, 0, 0, 0) >> 32) / 2**32).
Regenerate: python3 tests/scenes/make_g3_100k.py  (output gitignored)
"""
import os

M = 1 << 64
G1, G2, G3 = 0x9E3779B97F4A7C15, 0xBF58476D1CE4E5B9, 0x94D049BB133111EB
SALT_SCATTER = 0x5CA1E3D7F2B96A41


def mix(z):
    z = (z ^ (z >> 30)) * G2 % M
    z = (z ^ (z >> 27)) * G3 % M
    return z ^ (z >> 31)


def hash64(a, b, c, salt):
    return mix(((a + b + c + salt) % M + G1) % M)


def rows(f, values, fmt, per=6, trailer=""):
    for k in range(0, len(values), per):
        f.write("                " + ", ".join(fmt % v for v in values[k:k + per])
                + (",\n" if k + per < len(values) else trailer))


def main():
    nx, ny, segs, seed = 400, 250, 4, 42
    ncv = segs + 1
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "scene_g3_100k.usda")

    pts, widths, hair_t, hair_ids, curve_ids, cvc = [], [], [], [], [], []
    for j in range(ny):
        for i in range(nx):
            face = j * nx + i
            y = (j + 0.5) * 0.1
            for k in range(ncv):
                cid = hash64(seed, face, k, SALT_SCATTER)
                pts.append((i * 0.1 + k * 0.25, y, 0.0))
                widths.append(0.02)
                hair_t.append(k / segs)
                hair_ids.append(((hash64(cid, 0, 0, 0) >> 32) & 0xFFFFFFFF)
                                / float(1 << 32))
                if k == 0:
                    curve_ids.append(cid)
                    cvc.append(ncv)

    with open(out, "w") as f:
        w = f.write
        w('#usda 1.0\n(\n    defaultPrim = "groom"\n    upAxis = "Z"\n)\n\n')
        w('def Xform "groom"\n{\n    def Scope "description"\n    {\n')
        w('        custom token usdGen:graph:terminal = "curves"\n    }\n')
        w('    def Scope "surface"\n    {\n')
        w('        custom token usdGen:restType = "none"\n    }\n')
        w('    def Curves "curves"\n    {\n')
        w('        uniform token type = "nonrational_bspline"\n')
        w('        uniform token[] basis = ["linear", "linear"]\n')
        w('        uniform int[] wraps = [0, 0]\n')
        w("        point3f[] points = [\n")
        rows(f, pts, "(<%g, %g, %g>)", per=4, trailer="\n")
        w("        ]\n")
        w("        float[] widths = [\n")
        rows(f, widths, "%.6g", per=10, trailer="\n")
        w("        ]\n")
        w("        float[] usdGen:hairT = [\n")
        rows(f, hair_t, "%.6g", per=10, trailer="\n")
        w("        ]\n")
        w("        float[] usdGen:hairId = [\n")
        rows(f, hair_ids, "%.9g", per=8, trailer="\n")
        w("        ]\n")
        w("        uint64[] usdGen:curveId = [\n")
        rows(f, curve_ids, "%d", per=6, trailer="\n")
        w("        ] (\n            custom = true\n        )\n    }\n}\n")
    print("wrote", out, os.path.getsize(out), "bytes;", len(curve_ids),
          "curves,", len(pts), "points; seed", seed)


if __name__ == "__main__":
    main()
