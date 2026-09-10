#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""M1 storm-lane: generate the benchUsdGenStorm timing-gate stages (plan/09
S-1 / S-5 / S-6 / S-12).

Emits (into --out DIR):
  hair_1prim.usda          all curves in ONE UsdGeom.BasisCurves prim
  hair_32chunks.usda       same curves split across K prims (K=32 default,
                           override with --prims for the S-12 sweep variants)
  hair_1000prims.usda      same curves split across min(1000, N) prims
  hair_32chunks_anim.usda  same prims with vertex-interp widths animated per
                           frame over timeCode 0..frames-1

All prims pinned to cubic / bspline / wrapType="non", vertex-interpolation
widths. /World/Cam frames the strand ball; framing distance scales with the
curve count. defaultPrim=/World. Deterministic for a given --seed (plain
random.Random, no wall-clock or hash-order dependence).
"""
import argparse
import math
import os
import random

from pxr import Gf, Sdf, Tf, Usd, UsdGeom, Vt


def strand_ball(curves, cv, rng):
    """Procedural hair on a unit scalp hemisphere.

    Returns a list of `curves` strands, each a list of `cv` (x, y, z) tuples.
    Deterministic given rng.
    """
    strands = []
    for _ in range(curves):
        # root: on the upper hemisphere (z >= 0.15) of radius 1
        z = rng.uniform(0.15, 1.0)
        phi = rng.uniform(0.0, 2.0 * math.pi)
        r = math.sqrt(max(0.0, 1.0 - z * z))
        root = (r * math.cos(phi), r * math.sin(phi), z)
        # grow radially outward with a small random sway
        d = [root[0] + rng.uniform(-0.25, 0.25),
             root[1] + rng.uniform(-0.25, 0.25),
             root[2] + rng.uniform(-0.25, 0.25) + 0.35]
        n = math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) or 1.0
        d = [c / n for c in d]
        length = rng.uniform(0.45, 0.75)
        curl = rng.uniform(0.01, 0.05)
        phase = rng.uniform(0.0, 2.0 * math.pi)
        pts = []
        for j in range(cv):
            t = j / float(cv - 1) if cv > 1 else 0.0
            s = t * length
            w = curl * math.sin(phase + 3.0 * s)
            pts.append((root[0] + d[0] * s + w,
                        root[1] + d[1] * s + w * 0.5,
                        root[2] + d[2] * s + w * 0.25))
        strands.append(pts)
    return strands


def chunk_widths(nvert, cv, frame, frames, start_width):
    """Vertex-interp widths; when frames > 0 a per-frame breathing envelope."""
    env = 0.5 + 0.5 * math.sin(2.0 * math.pi * frame / float(frames - 1)) \
        if frames > 1 else 1.0
    base = start_width * (0.5 + 0.5 * env)
    w = Vt.FloatArray(nvert)
    for i in range(nvert):
        j = (i % cv) / float(cv - 1) if cv > 1 else 0.0
        w[i] = max(base * (1.0 - 0.8 * j), 1e-4)
    return w


def define_chunk(stage, path, strands, cv, frames=0, start_width=0.02):
    """One UsdGeom.BasisCurves prim holding `strands`."""
    curves = UsdGeom.BasisCurves.Define(stage, path)
    nvert = cv * len(strands)
    pts = Vt.Vec3fArray(nvert)
    k = 0
    for strand in strands:
        for p in strand:
            pts[k] = Gf.Vec3f(p[0], p[1], p[2])
            k += 1
    curves.CreatePointsAttr(pts)
    curves.CreateCurveVerticesAttr(Vt.IntArray([cv * i for i in range(len(strands))]))
    curves.CreateTypeAttr(UsdGeom.Tokens.cubic)
    curves.CreateBasisAttr(UsdGeom.Tokens.bspline)
    curves.CreateWrapAttr(UsdGeom.Tokens.non)
    width_attr = curves.CreateWidthsAttr()
    width_attr.Set(chunk_widths(nvert, cv, 0, 0, start_width))
    for t in range(frames):
        width_attr.Set(chunk_widths(nvert, cv, t, frames, start_width), t)
    return curves


def add_world(stage, curves):
    world = UsdGeom.Xform.Define(stage, "/World").GetPrim()
    stage.SetDefaultPrim(world)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    # framing scales with strand count (sqrt compression: ~5 units for 4k,
    # ~18 for 100k, ~34 for 500k)
    dist = 2.0 + 0.05 * math.sqrt(curves)
    cam = UsdGeom.Camera.Define(stage, "/World/Cam")
    # camera xform (local->world, Gf row-vector convention): columns are the
    # camera axes, last row the eye; camera looks down its local -z axis
    import numpy as _np  # shipped with USD python bindings
    eye_v = _np.array([0.0, -dist, 0.35])
    z_ax = eye_v - _np.array([0.0, 0.0, 0.4])
    z_ax /= _np.linalg.norm(z_ax)
    up = _np.array([0.0, 0.0, 1.0])
    x_ax = _np.cross(up, z_ax)
    if _np.linalg.norm(x_ax) < 1e-9:
        x_ax = _np.array([1.0, 0.0, 0.0])
    x_ax /= _np.linalg.norm(x_ax)
    y_ax = _np.cross(z_ax, x_ax)
    m = Gf.Matrix4d(*x_ax, 0.0, *y_ax, 0.0, *z_ax, 0.0, *eye_v, 1.0)
    xform = cam.GetPrim().CreateAttribute(
        "xformOp:transform", Sdf.ValueTypeNames.Matrix4d)
    xform.Set(m)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, dist * 10.0))
    cam.CreateFocalLengthAttr(20.955)  # ~50mm-equivalent horizontal FOV
    return world


def chunks(strands, k):
    n = len(strands)
    k = max(1, min(k, n))
    per = (n + k - 1) // k
    return [strands[i * per:(i + 1) * per] for i in range(k)
            if strands[i * per:(i + 1) * per]]


def write_stage(path, strands, cv, k, frames=0):
    stage = Usd.Stage.CreateNew(path)
    add_world(stage, len(strands))
    if frames > 0:
        stage.SetStartTimeCode(0)
        stage.SetEndTimeCode(frames - 1)
    for idx, part in enumerate(chunks(strands, k)):
        define_chunk(stage, "/World/Chunks/chunk_%04d" % idx, part, cv,
                     frames=frames)
    stage.GetRootLayer().Save()
    print("wrote %s (%d curves, %d prims%s)" % (
        path, len(strands), min(k, len(strands)),
        ", animated" if frames else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--curves", type=int, default=4000)
    ap.add_argument("--cv", type=int, default=8)
    ap.add_argument("--prims", type=int, default=32,
                    help="prim count for the chunk/anim stages "
                         "(tilesweep variants: 128, 196)")
    ap.add_argument("--frames", type=int, default=0,
                    help="width-animation frames (0 -> default 2-cycle)")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    # one shared geometry pass so all four files describe the same strands
    strands = strand_ball(args.curves, args.cv, random.Random(args.seed))
    write_stage(os.path.join(args.out, "hair_1prim.usda"),
                strands, args.cv, 1)
    write_stage(os.path.join(args.out, "hair_32chunks.usda"),
                strands, args.cv, args.prims)
    write_stage(os.path.join(args.out, "hair_1000prims.usda"),
                strands, args.cv, 1000)
    write_stage(os.path.join(args.out, "hair_32chunks_anim.usda"),
                strands, args.cv, args.prims, frames=max(args.frames, 2))


main()
