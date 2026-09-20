#!/usr/bin/env python3
"""Writes examples/tonic-reference.usda (plan/17 section 7).

The Wish-scale reference scene: a scalp grid partitioned into L1 regions,
an L1/L2/L3 tube hierarchy, guides on the deepest level, and a usdGen
description that amplifies them. At --wish scale this is the scene the TN
gates run on (60 K-face scalp, 80 L1 regions, 400 L2 tubes, 2400 L3 tubes,
12 000 guides x 16 CVs); the default run is a tiny structural stub with the
same prim layout so diffs stay reviewable.

The Guides are example content, NOT K9 kernel output, so TonicHydrateModel
fails closed on this file by design (as on tonic-ponytail.usda).

Usage (from the repo root):
  python examples/tools/make_tonic_reference.py [--tiny|--wish] [--out FILE]
  python examples/tools/make_tonic_reference.py --grid 8 --regions 4 \\
      --l2 2 --l3 2 --guides 64 --cv 8
"""
import argparse
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = os.path.dirname(HERE)

SIZE = 2.0  # scalp spans [-1, 1]^2, the ponytail convention

# Region palette (regionPalette in tonicGraph.py: golden-angle hues).
PALETTE = [
    (0.85, 0.30, 0.30), (0.30, 0.65, 0.85), (0.45, 0.80, 0.35),
    (0.90, 0.70, 0.25), (0.65, 0.45, 0.85), (0.90, 0.50, 0.70),
    (0.35, 0.75, 0.70), (0.80, 0.80, 0.40),
]


def fmt(v):
    return "%g" % v


def vec(v):
    return "(%s)" % ", ".join(fmt(x) for x in v)


def arr(values, per_line=6, indent="        "):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append(indent + ", ".join(values[i:i + per_line]))
    return ",\n".join(lines)


def normalize(v):
    n = math.sqrt(sum(x * x for x in v))
    return tuple(x / n for x in v)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def look_at(eye, target):
    """A USD camera transform (row major; the camera looks down -Z)."""
    fwd = normalize(tuple(t - e for t, e in zip(target, eye)))
    right = normalize(cross(fwd, (0.0, 1.0, 0.0)))
    up = cross(right, fwd)
    return (tuple(right) + (0.0,),
            tuple(up) + (0.0,),
            tuple(-c for c in fwd) + (0.0,),
            tuple(eye) + (1.0,))


def partition(regions):
    """Split `regions` into a (px, pz) grid, px * pz == regions."""
    px = int(math.sqrt(regions))
    px = max(px, 1)
    while regions % px != 0 and px > 1:
        px -= 1
    if regions % px != 0:
        px = 1
    return px, regions // px


def build(args):
    rng = random.Random(args.seed)
    n = args.grid
    faces = n * n
    px, pz = partition(args.regions)
    assert px * pz == args.regions, "regions must factor into a grid"

    # -- scalp: an n x n grid over [-1, 1]^2 -------------------------------
    step = SIZE / n
    skin_pts = []
    for iz in range(n + 1):
        for ix in range(n + 1):
            skin_pts.append((-SIZE / 2 + step * ix, 0.0,
                             -SIZE / 2 + step * iz))
    nn = n + 1
    skin_counts = [4] * faces
    skin_indices = []
    for iz in range(n):
        for ix in range(n):
            v00 = iz * nn + ix
            skin_indices += [v00, v00 + nn, v00 + nn + 1, v00 + 1]
    skin_st = [(ix / n, iz / n) for iz in range(nn) for ix in range(nn)]

    def region_of(ix, iz):
        return min(ix * px // n, px - 1) * pz + min(iz * pz // n, pz - 1)

    face_region = [region_of(ix, iz) for ix in range(n) for iz in range(n)]
    region_faces = [[] for _ in range(args.regions)]
    for f, r in enumerate(face_region):
        region_faces[r].append(f)

    def face_center(f):
        base = f * 4
        cx = sum(skin_pts[skin_indices[base + k]][0] for k in range(4)) / 4.0
        cz = sum(skin_pts[skin_indices[base + k]][2] for k in range(4)) / 4.0
        return (cx, 0.0, cz)

    # -- scalp graph: shared nodes at the partition corners ----------------
    # Node (ax, az) sits at grid coords (ax * n / px, az * n / pz); the
    # face/uv encoding is the TonicFacePosition one (u = z - iz, v = x - ix
    # in index units), verified by testUsdGenTonicGraph.
    node_ids = {}
    node_face, node_uv = [], []
    for ax in range(px + 1):
        for az in range(pz + 1):
            gx = ax * n / px
            gz = az * n / pz
            ix = min(int(gx), n - 1)
            iz = min(int(gz), n - 1)
            node_ids[(ax, az)] = len(node_face)
            node_face.append(ix * n + iz)
            node_uv.append((gz - iz, gx - ix))
    edges = []
    for ax in range(px):
        for az in range(pz + 1):
            edges.append((node_ids[(ax, az)], node_ids[(ax + 1, az)]))
    for ax in range(px + 1):
        for az in range(pz):
            edges.append((node_ids[(ax, az)], node_ids[(ax, az + 1)]))
    region_loops = []
    for ax in range(px):
        for az in range(pz):
            region_loops.append([node_ids[(ax, az)], node_ids[(ax + 1, az)],
                                 node_ids[(ax + 1, az + 1)],
                                 node_ids[(ax, az + 1)]])
    region_colors = [PALETTE[r % len(PALETTE)] for r in range(args.regions)]

    # -- tubes: L1 per region, L2/L3 nested --------------------------------
    # Stored shape only (center + sections); children carry zero deltas so
    # the groom looks identical right after the split (section 2.4).
    tubes_l1, tubes_l2, tubes_l3 = [], [], []
    tube_id = 0

    def tube_shape(cx, cz, height, radius):
        ncv = 5
        center = [(cx, height * i / (ncv - 1), cz + 0.15 * height * i / ncv)
                  for i in range(ncv)]
        sect_t = [i / (ncv - 1) for i in range(ncv)]
        cvs = []
        for i in range(ncv):
            r = radius * (1.0 - 0.4 * i / (ncv - 1))
            for s in range(8):
                a = 2.0 * math.pi * s / 8
                cvs.append((r * math.cos(a), r * math.sin(a)))
        return center, sect_t, cvs

    for r in range(args.regions):
        cx = sum(face_center(f)[0] for f in region_faces[r])
        cx /= max(len(region_faces[r]), 1)
        cz = sum(face_center(f)[2] for f in region_faces[r])
        cz /= max(len(region_faces[r]), 1)
        l1 = {"id": tube_id, "region": r, "level": 1, "child": -1,
              "shape": tube_shape(cx, cz, 0.9, 0.16), "children": []}
        tube_id += 1
        for _ in range(args.l2):
            jx = (rng.random() - 0.5) * 0.1
            jz = (rng.random() - 0.5) * 0.1
            l2 = {"id": tube_id, "region": r, "level": 2,
                  "child": len(l1["children"]),
                  "shape": tube_shape(cx + jx, cz + jz, 0.9, 0.09),
                  "children": []}
            tube_id += 1
            for _ in range(args.l3):
                kx = (rng.random() - 0.5) * 0.06
                kz = (rng.random() - 0.5) * 0.06
                l3 = {"id": tube_id, "region": r, "level": 3,
                      "child": len(l2["children"]),
                      "shape": tube_shape(cx + jx + kx, cz + jz + kz, 0.9,
                                          0.05),
                      "children": []}
                tube_id += 1
                l2["children"].append(l3)
                tubes_l3.append(l3)
            l1["children"].append(l2)
            tubes_l2.append(l2)
        tubes_l1.append(l1)
    deepest = tubes_l3 or tubes_l2 or tubes_l1
    deep_level = 3 if tubes_l3 else (2 if tubes_l2 else 1)

    def tube_usda(t, indent):
        center, sect_t, cvs = t["shape"]
        pad = " " * indent
        lines = [
            '%sdef UsdGenTube "tube_%d"' % (pad, t["id"]),
            "%s{" % pad,
            "%s    int usdGen:tonic:regionId = %d" % (pad, t["region"]),
            "%s    int usdGen:tonic:level = %d" % (pad, t["level"]),
            "%s    point3f[] usdGen:tonic:centerPoints = [%s]"
            % (pad, ", ".join(vec(p) for p in center)),
            "%s    float[] usdGen:tonic:sectionT = [%s]"
            % (pad, ", ".join(fmt(x) for x in sect_t)),
            "%s    int usdGen:tonic:sectionCvCount = 8" % pad,
            "%s    float2[] usdGen:tonic:sectionCvs = [%s]"
            % (pad, ", ".join(vec(p) for p in cvs)),
            "%s    int usdGen:tonic:childIndex = %d" % (pad, t["child"]),
            "%s    point3f[] usdGen:tonic:centerDeltas = []" % pad,
            "%s    float2[] usdGen:tonic:sectionDeltas = []" % pad,
            "%s    int usdGen:tonic:subdivide:count = 4" % pad,
            "%s    int usdGen:tonic:subdivide:seed = %d" % (pad, t["id"]),
            '%s    token usdGen:tonic:subdivide:splitMode = "kmeans"' % pad,
            "%s    float usdGen:tonic:fill:density = 100" % pad,
            "%s    int usdGen:tonic:fill:cvCount = %d" % (pad, args.cv),
            "%s    float[] usdGen:tonic:fill:lengthProfile = []" % pad,
            "%s    int usdGen:tonic:fill:seed = %d" % (pad, t["id"]),
            "%s    float usdGen:tonic:fill:edgeBias = 0" % pad,
            "%s    bool usdGen:tonic:locked = 0" % pad,
            "%s    bool usdGen:tonic:lockParents = 0" % pad,
            "%s    bool usdGen:tonic:lockChildren = 0" % pad,
        ]
        for c in t["children"]:
            lines.append(tube_usda(c, indent + 4))
        lines.append("%s}" % pad)
        return "\n".join(lines)

    # -- guides on the deepest level (example content, not K9 output) ------
    guides, guide_tube, guide_region = [], [], []
    per = max(args.guides // max(len(deepest), 1), 1)
    gid = 0
    for t in deepest:
        for _ in range(per):
            if gid >= args.guides:
                break
            (cx, _, cz) = t["shape"][0][2]
            rx = cx + (rng.random() - 0.5) * 0.1
            rz = cz + (rng.random() - 0.5) * 0.1
            pts = [(rx + 0.08 * (i / (args.cv - 1)) * (rng.random() - 0.3),
                    1.1 * i / (args.cv - 1),
                    rz + 0.25 * (i / (args.cv - 1)))
                   for i in range(args.cv)]
            guides.append(pts)
            guide_tube.append(t["id"])
            guide_region.append(t["region"])
            gid += 1
    guide_pts = [p for g in guides for p in g]
    gmin = [min(p[i] for p in guide_pts) for i in range(3)]
    gmax = [max(p[i] for p in guide_pts) for i in range(3)]
    widths = [0.006 * (1.0 - 0.7 * (i % args.cv) / (args.cv - 1))
              for i in range(len(guide_pts))]
    frames = []
    for g in guides:
        rx, ry, rz = g[0]
        frames += [1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, rx, ry, rz, 1]

    def mat4(values):
        return "(%s)" % ", ".join(
            "(%s)" % ", ".join(fmt(values[r * 4 + c]) for c in range(4))
            for r in range(4))

    cam = look_at((2.6, 1.8, 3.4), (0.0, 0.5, 0.2))
    cam_rows = ", ".join(vec(row) for row in cam)
    # Triple-quoted in the scene (the guide-interpolate spelling); kept out
    # of the template because the template itself is triple-quoted.
    ptex_source = '"""ptex("regionMap")"""'
    tube_verts = (len(tubes_l1) + len(tubes_l2) + len(tubes_l3)) * 5 * 8
    scale_note = ("WISH SCALE" if args.wish else
                  "STUB SCALE (regenerate with --wish for the S7 scene)")

    text = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    doc = "Tonic S7 reference scene (%s): %d-face scalp, %d L1 regions, %d tubes, %d guides x %d CVs."
)

# TONIC REFERENCE (plan/17 section 7)
#
#   %s: %d x %d-face scalp, %d regions (%d x %d), %d L1 / %d L2 / %d L3
#   tubes (~%d stored tube verts), %d guides x %d CVs, amplified by a usdGen
#   description (scatter -> interp -> frizz -> width).
#
# The Guides are example content, NOT K9 kernel output, so TonicHydrateModel
# fails closed on this file by design. The TN gates (TN-1..TN-7) run on the
# --wish scale; the default stub keeps the same prim layout at diffable size.
#
# View it:   .\\bin\\launch_usdview.ps1 examples\\tonic-reference.usda
# Record it: .\\bin\\record_usd.ps1 -Scene examples\\tonic-reference.usda `
#                -Output out.png -Camera /World/Cam -Complexity veryhigh
# Regenerate (stub): python examples\\tools\\make_tonic_reference.py
# Regenerate (wish): python examples\\tools\\make_tonic_reference.py --wish

def Xform "World"
{
    def Camera "Cam"
    {
        uniform token[] xformOpOrder = ["xformOp:transform"]
        matrix4d xformOp:transform = ( %s )
        float2 clippingRange = (0.05, 50)
        float focalLength = 35
        float horizontalAperture = 24
        float verticalAperture = 18
    }

    def DistantLight "Key"
    {
        float inputs:intensity = 10
        bool inputs:normalize = 1
        color3f inputs:color = (1, 0.98, 0.94)
        float3 xformOp:rotateXYZ = (-10, 40, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }

    def DomeLight "Sky"
    {
        asset inputs:texture:file = @./maps/StinsonBeach.hdr@
        float inputs:intensity = 2
        color3f inputs:color = (1, 1, 1)
    }

    def Mesh "Skin" (
        prepend apiSchemas = ["UsdGenRestAPI"]
    )
    {
        float3[] extent = [(-1, -0.01, -1), (1, 0.01, 1)]
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [
%s
        ]
        int[] faceVertexIndices = [
%s
        ]
        point3f[] points = [
%s
        ]
        point3f[] primvars:rest = [
%s
        ] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:st = [
%s
        ] (
            interpolation = "vertex"
        )
        int[] primvars:usdGen:tonicRegion = [
%s
        ] (
            interpolation = "uniform"
        )
        color3f[] primvars:displayColor = [(0.22, 0.2, 0.19)]
    }

    def UsdGenTonicGroom "TonicGroom"
    {
        token usdGen:tonic:version = "1"
        rel usdGen:tonic:scalp = </World/Skin>
        rel usdGen:tonic:description = </World/Groom/Hair>

        def UsdGenScalpGraph "Graph"
        {
            int[] usdGen:tonic:nodeFaceIds = [%s]
            float2[] usdGen:tonic:nodeUVs = [%s]
            int2[] usdGen:tonic:edges = [%s]
            int[] usdGen:tonic:regionNodeCounts = [%s]
            int[] usdGen:tonic:regionNodeIndices = [%s]
            color3f[] usdGen:tonic:regionColors = [%s]
            int2[] usdGen:tonic:linkedRegions = []
            float usdGen:tonic:snapRadius = 0.1
        }

        def Scope "Tubes"
        {
%s
        }

        def BasisCurves "Guides" (
            prepend apiSchemas = ["UsdGenCurveAPI"]
        )
        {
            float3[] extent = [%s, %s]
            uniform token purpose = "guide"
            uniform token type = "cubic"
            uniform token basis = "bspline"
            uniform token wrap = "pinned"
            int[] curveVertexCounts = [
%s
            ]
            point3f[] points = [
%s
            ]
            float[] widths = [
%s
            ] (
                interpolation = "vertex"
            )
            uint64[] primvars:usdGen:curveId = [
%s
            ] (
                interpolation = "uniform"
            )
            int[] primvars:usdGen:tubeId = [%s] (
                interpolation = "uniform"
            )
            int[] primvars:usdGen:regionId = [%s] (
                interpolation = "uniform"
            )
            int[] primvars:usdGen:hierarchyLevel = [%s] (
                interpolation = "uniform"
            )
            matrix4d[] primvars:usdGen:rootFrame = [
%s
            ]
            color3f[] primvars:displayColor = [(0.95, 0.35, 0.1)]
        }

        def UsdGenPtexMap "RegionMap"
        {
            asset usdGen:map:file = @./maps/tonic-regionMap.ptx@
            token usdGen:map:filter = "nearest"
            int usdGen:map:firstChannel = 0
            int usdGen:map:channelCount = 1
            float2 usdGen:map:clamp = (0, 0)
        }

        def UsdGenExpression "RegionExpr"
        {
            string usdGen:expr:source = %s
            rel input:regionMap = </World/TonicGroom/RegionMap>
            custom float outputs:result
        }
    }

    def Xform "Groom"
    {
        def UsdGenDescription "Hair"
        {
            rel usdGen:surface = </World/Skin>
            uniform token usdGen:curve:basis = "bspline"
            float usdGen:width:default = 0.004

            def Scope "Ops"
            {
                def UsdGenWidth "width"
                {
                    float usdGen:width = 0.0055
                    float2[] usdGen:width:knots = [(0, 1), (0.7, 0.75), (1, 0.15)]
                    bool usdGen:replace = true
                }

                def UsdGenNoise "frizz"
                {
                    int usdGen:seed = 3
                    float usdGen:noise:magnitude = 0.012
                    float2[] usdGen:noise:magnitude:knots = [(0, 0), (1, 1)]
                    float usdGen:noise:frequency = 6
                    int usdGen:noise:octaves = 2
                    float usdGen:preserveLength = 1
                }

                def UsdGenGuideInterpolate "interp"
                {
                    int usdGen:seed = 11
                    rel usdGen:guides = </World/TonicGroom/Guides>
                    int usdGen:cvCount = 12
                    int usdGen:maxGuides = 3
                    float usdGen:influenceRadius = 0.9
                    float usdGen:influenceDecay = 1.5
                    uniform token usdGen:blendMethod = "extrudeAndBlend"
                    float usdGen:blendInSkinSpace = 1
                    float usdGen:randomizeGuide = 0.15
                    # Stub literal: the committed form connects usdGen:region
                    # to </World/TonicGroom/RegionExpr.outputs:result> once
                    # the bake writes ./maps/tonic-regionMap.ptx (S2.2).
                    float usdGen:region = 0
                    float usdGen:regionCrossover = 0
                }

                def UsdGenScatter "scatter"
                {
                    int usdGen:seed = 5
                    float usdGen:density = 3500
                }
            }
        }
    }
}
""" % (
        scale_note, faces, args.regions, tube_id, len(guides), args.cv,
        scale_note, n, n, args.regions, px, pz, len(tubes_l1), len(tubes_l2),
        len(tubes_l3), tube_verts, len(guides), args.cv,
        cam_rows,
        arr([str(v) for v in skin_counts], per_line=24),
        arr([str(v) for v in skin_indices], per_line=20),
        arr([vec(p) for p in skin_pts], per_line=3),
        arr([vec(p) for p in skin_pts], per_line=3),
        arr([vec(p) for p in skin_st], per_line=8),
        arr([str(v) for v in face_region], per_line=24),
        ", ".join(str(v) for v in node_face),
        ", ".join(vec(p) for p in node_uv),
        ", ".join(vec(e) for e in edges),
        ", ".join(str(len(loop)) for loop in region_loops),
        ", ".join(str(v) for loop in region_loops for v in loop),
        ", ".join(vec(c) for c in region_colors),
        "\n".join(tube_usda(t, 12) for t in tubes_l1),
        vec(gmin), vec(gmax),
        arr([str(args.cv)] * len(guides), per_line=25),
        arr([vec(p) for p in guide_pts], per_line=2),
        arr([fmt(w) for w in widths], per_line=8),
        arr([str(1000 + i) for i in range(len(guides))], per_line=25),
        ", ".join(str(v) for v in guide_tube),
        ", ".join(str(v) for v in guide_region),
        ", ".join([str(deep_level)] * len(guides)),
        arr([mat4(frames[i:i + 16]) for i in range(0, len(frames), 16)],
            per_line=1),
        ptex_source,
    )
    return text, {"faces": faces, "regions": args.regions, "tubes": tube_id,
                  "guides": len(guides), "cv": args.cv,
                  "tubeVerts": tube_verts}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=None,
                        help="output path (default examples/tonic-reference.usda)")
    parser.add_argument("--grid", type=int, default=8,
                        help="scalp grid faces per side (default 8)")
    parser.add_argument("--regions", type=int, default=4,
                        help="L1 regions, factored into a grid (default 4)")
    parser.add_argument("--l2", type=int, default=2,
                        help="L2 children per L1 (default 2)")
    parser.add_argument("--l3", type=int, default=2,
                        help="L3 children per L2 (default 2)")
    parser.add_argument("--guides", type=int, default=64,
                        help="total guides on the deepest level (default 64)")
    parser.add_argument("--cv", type=int, default=8,
                        help="CVs per guide (default 8)")
    parser.add_argument("--seed", type=int, default=17,
                        help="layout seed (default 17)")
    parser.add_argument("--wish", action="store_true",
                        help="S7 reference scale (60 K faces, 80/400/2400 "
                             "tubes, 12000 guides x 16 CVs)")
    args = parser.parse_args()
    if args.wish:
        args.grid = 245
        args.regions = 80
        args.l2 = 5
        args.l3 = 6
        args.guides = 12000
        args.cv = 16
    text, census = build(args)
    path = args.out or os.path.join(EXAMPLES, "tonic-reference.usda")
    with open(path, "w", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d faces, %d regions, %d tubes, %d guides x %d CVs)"
          % (path, census["faces"], census["regions"], census["tubes"],
             census["guides"], census["cv"]))


if __name__ == "__main__":
    main()
