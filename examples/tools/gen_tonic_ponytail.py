#!/usr/bin/env python3
"""Writes examples/tonic-ponytail.usda (plan/17 P3 exit).

A committed-style Tonic groom: one tube rooted in a scalp patch, 25
hand-posed ponytail guides, and a usdGen description that amplifies them
(scatter -> interp -> frizz -> width), so record_usd shows the amplified
hair cook. The Guides are example content, NOT K9 output: hydrate fails
closed on this file by design (the bit-equality assert only holds for
kernel-generated guides).

Usage: python examples/tools/gen_tonic_ponytail.py   (from the repo root)
"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = os.path.dirname(HERE)


def fmt(v):
    s = "%g" % v
    return s


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


def mat4(values):
    rows = []
    for r in range(4):
        rows.append("(%s)" % ", ".join(
            fmt(values[r * 4 + c]) for c in range(4)))
    return "(%s)" % ", ".join(rows)


def look_at(eye, target):
    """A USD camera transform (row major; the camera looks down -Z).

    The make_examples.py spelling: rows are (right, up, -forward, eye).
    """
    fwd = normalize(tuple(t - e for t, e in zip(target, eye)))
    right = normalize(cross(fwd, (0.0, 1.0, 0.0)))
    up = cross(right, fwd)
    return (tuple(right) + (0.0,),
            tuple(up) + (0.0,),
            tuple(-c for c in fwd) + (0.0,),
            tuple(eye) + (1.0,))


# -- scalp: a 10x10 grid over [-1, 1]^2 ---------------------------------------
SIZE, CELLS = 2.0, 10
N = CELLS + 1
skin_pts = []
for iz in range(N):
    for ix in range(N):
        skin_pts.append((-SIZE / 2 + SIZE * ix / CELLS, 0.0,
                         -SIZE / 2 + SIZE * iz / CELLS))
skin_counts = [4] * (CELLS * CELLS)
skin_indices = []
for iz in range(CELLS):
    for ix in range(CELLS):
        # +Y winding (the make_examples.py grid_mesh convention).
        v00 = iz * N + ix
        skin_indices += [v00, v00 + N, v00 + N + 1, v00 + 1]
skin_st = [(ix / CELLS, iz / CELLS) for iz in range(N) for ix in range(N)]

# -- ponytail guides: 5x5 roots gathering to a tie, then falling back --------
GUIDE_CVS = 8
roots = []
for gz in range(5):
    for gx in range(5):
        roots.append(((gx - 2) * 0.09, (gz - 2) * 0.09))


def guide_points(gx, gz):
    p0 = (gx, 0.0, 0.3 + gz)
    p1 = (gx * 0.9, 0.18, 0.32 + gz * 0.9)
    p2 = (gx * 0.7, 0.38, 0.38 + gz * 0.7)
    p3 = (gx * 0.45, 0.55, 0.42 + gz * 0.4)
    p4 = (gx * 0.5, 0.72, 0.55 + gz * 0.4)
    p5 = (gx * 0.6, 0.88, 0.68 + gz * 0.45)
    p6 = (gx * 0.7, 1.02, 0.80 + gz * 0.5)
    p7 = (gx * 0.8, 1.12, 0.92 + gz * 0.55)
    return [p0, p1, p2, p3, p4, p5, p6, p7]


guides = [guide_points(gx, gz) for gx, gz in roots]
NG = len(guides)
guide_pts = [p for g in guides for p in g]
guide_min = [min(p[i] for p in guide_pts) for i in range(3)]
guide_max = [max(p[i] for p in guide_pts) for i in range(3)]
width_profile = [0.008, 0.0072, 0.0064, 0.0056, 0.0048, 0.004, 0.0032, 0.0024]
widths = width_profile * NG

# Root frames (UsdGenCurveAPI row-major): N=(1,0,0), B=(0,0,1), T=(0,1,0)
# at each root position (example content, not K10 output).
frames = []
for g in guides:
    rx, ry, rz = g[0]
    frames += [1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, rx, ry, rz, 1]

# -- tube: center up the bundle, sections spanning it --------------------------
center = [(0, 0, 0.3), (0, 0.3, 0.36), (0, 0.6, 0.48), (0, 0.9, 0.70),
          (0, 1.12, 0.92)]
RINGS = len(center)
RING_VERTS = 8
section_t = [r / (RINGS - 1) for r in range(RINGS)]
section_r = [0.24, 0.20, 0.12, 0.14, 0.16]
section_cvs = []
for r in range(RINGS):
    for s in range(RING_VERTS):
        a = 2.0 * math.pi * s / RING_VERTS
        section_cvs.append((section_r[r] * math.cos(a),
                            section_r[r] * math.sin(a)))

cam = look_at((2.3, 1.5, 3.0), (0.0, 0.6, 0.4))
cam_rows = ", ".join(vec(row) for row in cam)

text = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    doc = "Tonic P3: a committed-style ponytail groom (one tube, 25 guides) amplified by a usdGen description (scatter -> interp -> frizz -> width)."
)

# TONIC PONYTAIL (plan/17 P3 exit)
#
#   TonicGroom/Tube_0     the authored tube (center + sections + fill)
#   TonicGroom/Guides     25 hand-posed ponytail guides (purpose "guide")
#   Groom/Hair            the usdGen description; GuideInterpolate amplifies
#                         the Tonic guides into full-density strands
#
# The Guides are example content, NOT K9 kernel output, so TonicHydrateModel
# fails closed on this file by design (the bit-equality assert only holds
# for kernel-generated guides). Recording shows the amplified hair cook.
#
# View it:   .\\bin\\launch_usdview.ps1 examples\\tonic-ponytail.usda
# Record it: .\\bin\\record_usd.ps1 -Scene examples\\tonic-ponytail.usda `
#                -Output out.png -Camera /World/Cam -Complexity veryhigh
# Regenerate: python examples\\tools\\gen_tonic_ponytail.py

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
        color3f[] primvars:displayColor = [(0.22, 0.2, 0.19)]
    }

    def UsdGenTonicGroom "TonicGroom"
    {
        token usdGen:tonic:version = "1"

        def Scope "Tubes"
        {
            def UsdGenTube "Tube_0"
            {
                int usdGen:tonic:regionId = 0
                int usdGen:tonic:level = 1
                point3f[] usdGen:tonic:centerPoints = [%s]
                float[] usdGen:tonic:sectionT = [%s]
                int usdGen:tonic:sectionCvCount = 8
                float2[] usdGen:tonic:sectionCvs = [
%s
                ]
                int usdGen:tonic:childIndex = -1
                point3f[] usdGen:tonic:centerDeltas = []
                float2[] usdGen:tonic:sectionDeltas = []
                int usdGen:tonic:subdivide:count = 0
                int usdGen:tonic:subdivide:seed = 0
                token usdGen:tonic:subdivide:splitMode = "none"
                float usdGen:tonic:fill:density = 100
                int usdGen:tonic:fill:cvCount = 8
                float[] usdGen:tonic:fill:lengthProfile = []
                int usdGen:tonic:fill:seed = 0
                float usdGen:tonic:fill:edgeBias = 0
                bool usdGen:tonic:locked = 0
                bool usdGen:tonic:lockParents = 0
                bool usdGen:tonic:lockChildren = 0
            }
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
    cam_rows,
    arr([str(v) for v in skin_counts], per_line=24),
    arr([str(v) for v in skin_indices], per_line=20),
    arr([vec(p) for p in skin_pts], per_line=3),
    arr([vec(p) for p in skin_pts], per_line=3),
    arr([vec(p) for p in skin_st], per_line=8),
    ", ".join(vec(p) for p in center),
    ", ".join(fmt(t) for t in section_t),
    arr([vec(p) for p in section_cvs], per_line=4),
    vec(guide_min),
    vec(guide_max),
    arr(["8"] * NG, per_line=25),
    arr([vec(p) for p in guide_pts], per_line=2),
    arr([fmt(w) for w in widths], per_line=8),
    arr([str(1000 + i) for i in range(NG)], per_line=25),
    ", ".join(["0"] * NG),
    ", ".join(["0"] * NG),
    ", ".join(["1"] * NG),
    arr([mat4(frames[i:i + 16]) for i in range(0, len(frames), 16)],
        per_line=1),
)

path = os.path.join(EXAMPLES, "tonic-ponytail.usda")
with open(path, "w", newline="\n") as f:
    f.write(text)
print("wrote %s" % path)
