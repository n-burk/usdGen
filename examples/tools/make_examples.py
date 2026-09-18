#!/usr/bin/env python3
"""Writes the procedural example scenes in examples/.

    python examples/tools/make_examples.py

The scenes are plain text so they diff well; the grid mesh and the guide
curves are generated here instead of by hand. The Ptex maps they read are
baked by examples/tools/bake_maps.ps1 (usdGenBakePtex) from the meshes this
script writes, so run this first.
"""

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = os.path.dirname(HERE)

# Key + a DomeLight fill so the physically based default hair material (a UE
# port) doesn't render near-black under only usdrecord's camera headlight.
# inputs:normalize = 1 is required on the DistantLight: HdSt otherwise
# multiplies intensity by its (tiny, 0.53deg-default) solid angle, so an
# un-normalized "intensity 3" comes out around 2e-4 and the light is
# effectively invisible next to the headlight. The HDRI is copied into
# examples/maps/ (not referenced by its original absolute usd-install path)
# so the scene resolves for anyone who clones this repo alone.
#
# Angle matters as much as magnitude here: these strands grow mostly straight
# up (usdGen:direction "surfaceNormal"/a near-vertical directionVector), and
# a hair BSDF's R/TT/TRT lobes are weakest when light travels ALONG the fibre
# axis -- a steep overhead key (tried first: rotateXYZ (-35, 25, 0), travel
# direction ~55% down the Y axis) stayed near-black even at intensity 10-50.
# Flattening the key to mostly-horizontal/grazing (rotateXYZ (-10, 40, 0),
# travel direction ~17% down Y) lights the same strands as warm brown at
# intensity 6 -- confirmed by an A/B render, not guessed.
LIGHTS = '''    def DistantLight "Key"
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
'''


def fmt(v):
    s = "%.6g" % v
    return "0" if s in ("-0", "0") else s


def vec(v):
    return "(" + ", ".join(fmt(c) for c in v) + ")"


def arr(values, per_line=6, indent="        "):
    items = [vec(v) if isinstance(v, (tuple, list)) else fmt(v) for v in values]
    lines = []
    for i in range(0, len(items), per_line):
        lines.append(indent + ", ".join(items[i:i + per_line]))
    return "[\n" + ",\n".join(lines) + "\n" + indent[:-4] + "]"


def normalize(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v) if n else v


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def look_at(eye, target):
    """A USD camera transform (row major; the camera looks down -Z)."""
    f = normalize(tuple(t - e for t, e in zip(target, eye)))
    r = normalize(cross(f, (0.0, 1.0, 0.0)))
    u = cross(r, f)
    rows = [r + (0.0,), u + (0.0,), tuple(-c for c in f) + (0.0,), tuple(eye) + (1.0,)]
    return "( " + ", ".join("(" + ", ".join(fmt(c) for c in row) + ")" for row in rows) + " )"


def hash01(*keys):
    """Small deterministic hash in [0, 1)."""
    h = 1469598103934665603
    for k in keys:
        h ^= (int(k) & 0xFFFFFFFF)
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        h ^= h >> 29
    return (h & 0xFFFFFF) / float(0x1000000)


def grid_mesh(name, size, cells, indent="    "):
    """A size x size plane in XZ centred on the origin, normal +Y."""
    n = cells + 1
    points, st = [], []
    for j in range(n):
        for i in range(n):
            x = -size / 2 + size * i / cells
            z = -size / 2 + size * j / cells
            points.append((x, 0.0, z))
            st.append((i / cells, j / cells))
    counts, indices = [], []
    for j in range(cells):
        for i in range(cells):
            a = j * n + i
            # (x, z), (x, z + 1), (x + 1, z + 1), (x + 1, z): normal +Y.
            indices += [a, a + n, a + n + 1, a + 1]
            counts.append(4)
    ind = indent + "    "
    return f'''{indent}def Mesh "{name}" (
{indent}    prepend apiSchemas = ["UsdGenRestAPI"]
{indent})
{indent}{{
{ind}uniform token subdivisionScheme = "none"
{ind}int[] faceVertexCounts = {arr(counts, 24, ind + "    ")}
{ind}int[] faceVertexIndices = {arr(indices, 20, ind + "    ")}
{ind}point3f[] points = {arr(points, 6, ind + "    ")}
{ind}point3f[] primvars:rest = {arr(points, 6, ind + "    ")} (
{ind}    interpolation = "vertex"
{ind})
{ind}texCoord2f[] primvars:st = {arr(st, 8, ind + "    ")} (
{ind}    interpolation = "vertex"
{ind})
{ind}color3f[] primvars:displayColor = [(0.22, 0.2, 0.19)]
{indent}}}
'''


# ---------------------------------------------------------------------------
# guide-interpolate-plane.usda
# ---------------------------------------------------------------------------

def guides(size=2.0, per_side=5, cvs=8):
    """Sparse guides in a grid: a swirl that leans around the centre, with
    per-guide length and a sideways wave, so partings and blends are easy to
    read."""
    counts, points, widths, ids = [], [], [], []
    span = size * 0.8
    for j in range(per_side):
        for i in range(per_side):
            g = j * per_side + i
            jitter = (hash01(g, 1) - 0.5) * 0.08, (hash01(g, 2) - 0.5) * 0.08
            x = -span / 2 + span * i / (per_side - 1) + jitter[0]
            z = -span / 2 + span * j / (per_side - 1) + jitter[1]
            root = (x, 0.0, z)
            # Lean: tangential around the centre, with alternating rings.
            centred = abs(x) + abs(z) > 1e-6
            tangent = normalize((-z, 0.0, x)) if centred else (1.0, 0.0, 0.0)
            radial = normalize((x, 0.0, z)) if centred else (0.0, 0.0, 1.0)
            sign = 1.0 if (i + j) % 2 == 0 else -1.0
            lean = normalize(tuple(0.8 * sign * t + 0.35 * r for t, r in zip(tangent, radial)))
            side = normalize(cross((0.0, 1.0, 0.0), lean))
            length = 0.32 + 0.2 * hash01(g, 3)
            wave = 0.02 + 0.03 * hash01(g, 4)
            bend = 0.35 + 0.35 * hash01(g, 5)
            counts.append(cvs)
            ids.append(1000 + g)
            for k in range(cvs):
                t = k / (cvs - 1)
                up = length * t * (1.0 - 0.25 * bend * t)
                out = length * bend * t ** 1.8
                wig = wave * math.sin(t * math.pi * 2.0) * t
                points.append(tuple(root[d] + (0.0, up, 0.0)[d] + lean[d] * out + side[d] * wig
                                    for d in range(3)))
                widths.append(0.008 * (1.0 - 0.7 * t))
    return counts, points, widths, ids


def write_guide_example():
    counts, points, widths, ids = guides()
    cam = look_at((2.35, 1.75, 2.6), (0.0, 0.12, 0.0))
    mesh = grid_mesh("Skin", 2.0, 10)
    text = f'''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    doc = "Guide-curve groom: strands grown between sparse guide curves, parted by a region field (a geoSampler voronoi over the guide roots, or a Ptex region map)."
)

# GUIDE-BASED GROOM (as opposed to scatter -> grow)
#
#   scatter -> interp -> frizz -> width
#
# `interp` is a UsdGenGuideInterpolate: every scattered root grows a strand by
# blending the shapes of its nearest guides (usdGen:maxGuides of them, weighted
# (1 - d / influenceRadius)^influenceDecay), each guide shape turned from the
# guide's root normal onto the strand's. /World/Guides is the sparse guide set
# (25 BasisCurves, a swirl with alternating lean). It is authored with purpose
# "guide": usdview draws it with Display > Guide.
#
# REGIONS
#   usdGen:region is a per-strand id. A guide only shapes strands of its own
#   region; its own region is the value at its root. The variant set `region`
#   on /World/Groom/Hair switches what drives it:
#
#   voronoi  (default) Expressions/voronoiRegion:
#                geoSampler("guideCurves", "$index")
#            geoSampler iterates the prims (curves) of the geometry its
#            input:guideCurves relationship names, and returns the element
#            expression "$index" at the one nearest the strand root: a voronoi
#            cell id seeded by the guide roots. Each guide owns its cell;
#            usdGen:regionCrossover = 0.3 lets neighbours soften the borders.
#   ptexMap  Expressions/mapRegion:  ptex("regionMap")
#            input:regionMap names Maps/regionMap, a UsdGenPtexMap whose file
#            (maps/guide_regions.ptx) splits the skin into five regions. Several
#            guides share a region, so strands interpolate inside it and part
#            hard at its border (crossover 0).
#   smooth   nothing connected: one region, plain interpolation.
#
# DIRTY PROPAGATION
#   Everything an expression samples is named by a relationship on the
#   expression prim, so it is tracked: edit a guide point (or retarget
#   input:guideCurves) and the regions and the strands recook.
#
# View it:   .\\bin\\launch_usdview.ps1 examples\\guide-interpolate-plane.usda
# Record it: .\\bin\\record_usd.ps1 -Scene examples\\guide-interpolate-plane.usda `
#                -Output out.png -Camera /World/Cam -Complexity veryhigh
# Regenerate: python examples\\tools\\make_examples.py, then
#             .\\examples\\tools\\bake_maps.ps1

def Xform "World"
{{
    def Camera "Cam"
    {{
        uniform token[] xformOpOrder = ["xformOp:transform"]
        matrix4d xformOp:transform = {cam}
        float2 clippingRange = (0.05, 50)
        float focalLength = 35
        float horizontalAperture = 24
        float verticalAperture = 18
    }}

{LIGHTS}
{mesh}
    def BasisCurves "Guides" (
        prepend apiSchemas = ["UsdGenCurveAPI"]
    )
    {{
        uniform token purpose = "guide"
        uniform token type = "cubic"
        uniform token basis = "bspline"
        uniform token wrap = "pinned"
        int[] curveVertexCounts = {arr(counts, 25, "            ")}
        point3f[] points = {arr(points, 4, "            ")}
        float[] widths = {arr(widths, 8, "            ")} (
            interpolation = "vertex"
        )
        uint64[] primvars:usdGen:curveId = {arr(ids, 25, "            ")} (
            interpolation = "uniform"
        )
        color3f[] primvars:displayColor = [(0.95, 0.35, 0.1)]
    }}

    def Xform "Groom"
    {{
        def UsdGenDescription "Hair" (
            prepend variantSets = "region"
            variants = {{
                string region = "voronoi"
            }}
        )
        {{
            rel usdGen:surface = </World/Skin>
            uniform token usdGen:curve:basis = "bspline"
            float usdGen:width:default = 0.004

            def Scope "Ops"
            {{
                def UsdGenWidth "width"
                {{
                    float usdGen:width = 0.0055
                    float2[] usdGen:width:knots = [(0, 1), (0.7, 0.75), (1, 0.15)]
                    bool usdGen:replace = true
                }}

                def UsdGenNoise "frizz"
                {{
                    int usdGen:seed = 3
                    float usdGen:noise:magnitude = 0.012
                    float2[] usdGen:noise:magnitude:knots = [(0, 0), (1, 1)]
                    float usdGen:noise:frequency = 6
                    int usdGen:noise:octaves = 2
                    float usdGen:preserveLength = 1
                }}

                def UsdGenGuideInterpolate "interp"
                {{
                    int usdGen:seed = 11
                    rel usdGen:guides = </World/Guides>
                    int usdGen:cvCount = 12
                    int usdGen:maxGuides = 3
                    float usdGen:influenceRadius = 0.75
                    float usdGen:influenceDecay = 1.5
                    uniform token usdGen:blendMethod = "extrudeAndBlend"
                    float usdGen:blendInSkinSpace = 1
                    float usdGen:randomizeGuide = 0.15
                    float usdGen:region = 0
                    float usdGen:regionCrossover = 0
                }}

                def UsdGenScatter "scatter"
                {{
                    int usdGen:seed = 5
                    float usdGen:density = 3500
                }}
            }}

            def Scope "Expressions"
            {{
                # A voronoi over the guide roots. input:guideCurves is what
                # geoSampler("guideCurves", ...) reads; "$index" is evaluated
                # at the nearest prim (curve root) of that geometry.
                def UsdGenExpression "voronoiRegion"
                {{
                    string usdGen:expr:source = """geoSampler("guideCurves", "$index")"""
                    rel input:guideCurves = </World/Guides>
                    custom float outputs:result
                }}

                # A painted (here: baked) region map, read at the strand root.
                def UsdGenExpression "mapRegion"
                {{
                    string usdGen:expr:source = """ptex("regionMap")"""
                    rel input:regionMap = </World/Groom/Hair/Maps/regionMap>
                    custom float outputs:result
                }}
            }}

            def Scope "Maps"
            {{
                def UsdGenPtexMap "regionMap"
                {{
                    asset usdGen:map:file = @./maps/guide_regions.ptx@
                    token usdGen:map:filter = "nearest"
                    uniform token usdGen:map:channel = "r"
                    float2 usdGen:map:clamp = (0, 0)
                }}
            }}

            variantSet "region" = {{
                "voronoi" {{
                    over "Ops"
                    {{
                        over "interp"
                        {{
                            float usdGen:region.connect = </World/Groom/Hair/Expressions/voronoiRegion>
                            float usdGen:regionCrossover = 0.3
                        }}
                    }}
                }}
                "ptexMap" {{
                    over "Ops"
                    {{
                        over "interp"
                        {{
                            float usdGen:region.connect = </World/Groom/Hair/Expressions/mapRegion>
                            int usdGen:maxGuides = 4
                            float usdGen:regionCrossover = 0
                        }}
                    }}
                }}
                "smooth" {{
                }}
            }}
        }}
    }}
}}
'''
    path = os.path.join(EXAMPLES, "guide-interpolate-plane.usda")
    with open(path, "w", newline="\n") as fh:
        fh.write(text)
    print("wrote", path)


# ---------------------------------------------------------------------------
# clump-ptex-plane.usda
# ---------------------------------------------------------------------------

def write_clump_example():
    cam = look_at((2.2, 1.45, 2.45), (0.0, 0.12, 0.0))
    mesh = grid_mesh("Skin", 2.0, 12)
    text = f'''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    doc = "Clumping driven by Ptex maps: a region map decides which strands clump together, a second map how tightly."
)

# PTEX CLUMP MAPS
#
#   scatter -> grow -> frizz -> clump -> width
#
# `clump` is a UsdGenClump with two connected controls, each an expression
# reading a UsdGenPtexMap through the expression prim's input:<name>
# relationship:
#
#   usdGen:clump:map    <- ptex("clumpRegions")
#       maps/clump_regions.ptx holds one random value per voronoi cell (about
#       90 cells over the skin, with wobbly borders). Strands whose roots read
#       the same value form one clump, centred on the member nearest the
#       cell's centroid: the map's cells ARE the clumps (XGen's clump map).
#   usdGen:clump:amount <- 0.3 + 0.65 * ptex("clumpTightness")
#       maps/clump_tightness.ptx has six large patches of random value, so
#       some areas clump tightly and others stay loose.
#
# usdGen:clump:levels = 2 splits every map clump into density-scattered
# sub-clumps, pulled at tightnessReduction strength toward sub-centres that
# have already been clumped once. The clumpId_0 / clumpId_1 primvars carry the
# cell per strand for shading.
#
# Maps are baked from this file's skin by examples/tools/bake_maps.ps1
# (usdGenBakePtex). Edit usdGen:map:file, or the maps themselves plus
# usdGen:map:* on the map prims, and the clumps recook.
#
# View it:   .\\bin\\launch_usdview.ps1 examples\\clump-ptex-plane.usda
# Record it: .\\bin\\record_usd.ps1 -Scene examples\\clump-ptex-plane.usda `
#                -Output out.png -Camera /World/Cam -Complexity veryhigh

def Xform "World"
{{
    def Camera "Cam"
    {{
        uniform token[] xformOpOrder = ["xformOp:transform"]
        matrix4d xformOp:transform = {cam}
        float2 clippingRange = (0.05, 50)
        float focalLength = 35
        float horizontalAperture = 24
        float verticalAperture = 18
    }}

{LIGHTS}
{mesh}
    def Xform "Groom"
    {{
        def UsdGenDescription "Fur"
        {{
            rel usdGen:surface = </World/Skin>
            uniform token usdGen:curve:basis = "bspline"
            float usdGen:width:default = 0.004

            def Scope "Ops"
            {{
                def UsdGenWidth "width"
                {{
                    float usdGen:width = 0.005
                    float2[] usdGen:width:knots = [(0, 1), (0.6, 0.8), (1, 0.15)]
                    bool usdGen:replace = true
                }}

                def UsdGenClump "clump"
                {{
                    int usdGen:seed = 23
                    float usdGen:clump:amount = 0.8
                    float usdGen:clump:amount.connect = </World/Groom/Fur/Expressions/clumpTightness>
                    float usdGen:clump:map = 0
                    float usdGen:clump:map.connect = </World/Groom/Fur/Expressions/clumpRegions>
                    float2[] usdGen:clump:profile:knots = [(0, 0), (0.35, 0.45), (1, 1)]
                    uniform token usdGen:clump:method = "linearBlend"
                    float usdGen:clump:size = 0.6
                    int usdGen:clump:levels = 2
                    float usdGen:clump:density = 12
                    float usdGen:clump:sizeReduction = 0.5
                    float usdGen:clump:tightnessReduction = 0.6
                    float usdGen:clump:stray:rate = 0.04
                    float usdGen:clump:stray:amount = 0.8
                    float usdGen:clump:stray:falloff = 0.7
                    float usdGen:preserveLength = 1
                }}

                def UsdGenNoise "frizz"
                {{
                    int usdGen:seed = 7
                    float usdGen:noise:magnitude = 0.035
                    float2[] usdGen:noise:magnitude:knots = [(0, 0), (0.5, 0.5), (1, 1)]
                    float usdGen:noise:frequency = 3
                    int usdGen:noise:octaves = 2
                    float usdGen:preserveLength = 1
                }}

                def UsdGenGrow "grow"
                {{
                    int usdGen:seed = 19
                    int usdGen:segments = 12
                    float usdGen:length = 0.34
                    float2 usdGen:lengthRandom = (0.8, 1.15)
                    uniform token usdGen:direction = "vector"
                    vector3f usdGen:directionVector = (0.12, 1, 0.08)
                }}

                def UsdGenScatter "scatter"
                {{
                    int usdGen:seed = 41
                    float usdGen:density = 4000
                }}
            }}

            def Scope "Expressions"
            {{
                # Clump membership: the region map's value at the root.
                def UsdGenExpression "clumpRegions"
                {{
                    string usdGen:expr:source = """ptex("clumpRegions")"""
                    rel input:clumpRegions = </World/Groom/Fur/Maps/clumpRegions>
                    custom float outputs:result
                }}

                # Pull strength: a coarse map remapped into [0.3, 0.95].
                def UsdGenExpression "clumpTightness"
                {{
                    string usdGen:expr:source = """0.3 + 0.65 * ptex("clumpTightness")"""
                    rel input:clumpTightness = </World/Groom/Fur/Maps/clumpTightness>
                    custom float outputs:result
                }}
            }}

            def Scope "Maps"
            {{
                def UsdGenPtexMap "clumpRegions"
                {{
                    asset usdGen:map:file = @./maps/clump_regions.ptx@
                    token usdGen:map:filter = "nearest"
                    uniform token usdGen:map:channel = "r"
                    float2 usdGen:map:clamp = (0, 0)
                }}

                def UsdGenPtexMap "clumpTightness"
                {{
                    asset usdGen:map:file = @./maps/clump_tightness.ptx@
                    token usdGen:map:filter = "bilinear"
                    uniform token usdGen:map:channel = "r"
                }}
            }}
        }}
    }}
}}
'''
    path = os.path.join(EXAMPLES, "clump-ptex-plane.usda")
    with open(path, "w", newline="\n") as fh:
        fh.write(text)
    print("wrote", path)


# ---------------------------------------------------------------------------
# rbf-guides-plane.usda
# ---------------------------------------------------------------------------

RBF_FRAMES = 48


def smoothstep(e0, e1, x):
    t = min(1.0, max(0.0, (x - e0) / (e1 - e0)))
    return t * t * (3.0 - 2.0 * t)


def driver_pose(root, height, segments, frame):
    """One driver curve at `frame`: a column of `segments` equal segments that
    curls away from the vertical as a wind wave passes along +X. Frame 1 is
    the straight rest pose."""
    x, _, z = root
    phase = 2.0 * math.pi * (frame - 1) / RBF_FRAMES
    amplitude = 1.1 * smoothstep(1.0, 9.0, frame)
    bend = amplitude * math.sin(2.0 * phase - 2.2 * x + 0.6 * z)
    # The wind blows along +X and veers a little with the wave.
    veer = 0.45 * math.sin(phase + 1.3 * z)
    side = normalize((1.0, 0.0, veer))
    step = height / segments
    points = [tuple(root)]
    for k in range(segments):
        angle = bend * (k + 0.5) / segments
        prev = points[-1]
        up = (0.0, math.cos(angle), 0.0)
        points.append(tuple(prev[d] + step * (up[d] + side[d] * math.sin(angle))
                            for d in range(3)))
    return points


def write_rbf_example():
    per_side, segments, height, span = 4, 4, 0.46, 1.8
    roots = [(-span / 2 + span * i / (per_side - 1), 0.0, -span / 2 + span * j / (per_side - 1))
             for j in range(per_side) for i in range(per_side)]
    counts = [segments + 1] * len(roots)
    rest = [p for r in roots for p in driver_pose(r, height, segments, 1)]
    samples = []
    for frame in range(1, RBF_FRAMES + 1):
        pose = [p for r in roots for p in driver_pose(r, height, segments, frame)]
        samples.append("            %d: %s," % (frame, arr(pose, 5, "                ")))
    time_samples = "\n".join(samples)
    ids = [2000 + g for g in range(len(roots))]
    widths = [0.012] * len(rest)
    cam = look_at((2.4, 1.3, 2.7), (0.0, 0.18, 0.0))
    mesh = grid_mesh("Skin", 2.0, 10)
    text = f'''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    startTimeCode = 1
    endTimeCode = {RBF_FRAMES}
    timeCodesPerSecond = 24
    framesPerSecond = 24
    doc = "Hair deformed by animated driver curves through an RBF: UsdGenDeform with usdGen:guides."
)

# RBF DEFORMATION BY ANIMATED CURVES
#
#   scatter -> grow -> frizz -> width -> deform
#
# (Ops lists operators last-first: the bottom sibling runs first.)
#
# /World/Drivers is one BasisCurves prim of 16 upright driver curves (5 CVs
# each, drawn orange) whose points are animated over frames 1-{RBF_FRAMES}: a wind
# wave travels along +X and curls them. Their Default-time points are the rest
# pose (UsdGenCurveAPI publishes it; frame 1 is the same pose).
#
# `deform` is a UsdGenDeform with usdGen:guides = </World/Drivers>. Every
# driver CV is an RBF sample: bound at its rest position, moved to its
# position at the current frame. The cubic RBF (with an affine term) through
# those 80 samples is a smooth displacement field over the whole groom, and
# every CV of the styled strands moves by it, so the hair between and around
# the drivers bends with them while keeping its own shape. With
# usdGen:lockRoots (the default) each strand is shifted back by the field at
# its root, so the roots stay on the skin.
#
#   usdGen:rbfSamples  the most driver CVs used (farthest-point sampled)
#   usdGen:lockRoots   keep roots where they are
#   usdGen:mask        blend the deformation in per strand
#
# The drivers must span 3D (they cannot all lie in one plane). Animate their
# points: a transform on the driver prim moves rest and pose together. This is
# the CPU lane; the CUDA lane's UsdGenDeform samples the bound surface instead.
#
# View it:   .\\bin\\launch_usdview.ps1 examples\\rbf-guides-plane.usda   (press play)
# Record it: .\\bin\\record_usd.ps1 -Scene examples\\rbf-guides-plane.usda `
#                -Output out.###.png -Camera /World/Cam -Complexity veryhigh -Frame 20
# Regenerate: python examples\\tools\\make_examples.py

def Xform "World"
{{
    def Camera "Cam"
    {{
        uniform token[] xformOpOrder = ["xformOp:transform"]
        matrix4d xformOp:transform = {cam}
        float2 clippingRange = (0.05, 50)
        float focalLength = 35
        float horizontalAperture = 24
        float verticalAperture = 18
    }}

{LIGHTS}
{mesh}
    def BasisCurves "Drivers" (
        prepend apiSchemas = ["UsdGenCurveAPI"]
    )
    {{
        uniform token type = "linear"
        uniform token wrap = "nonperiodic"
        int[] curveVertexCounts = {arr(counts, 16, "            ")}
        point3f[] points = {arr(rest, 5, "            ")}
        point3f[] points.timeSamples = {{
{time_samples}
        }}
        float[] widths = {arr(widths, 20, "            ")} (
            interpolation = "vertex"
        )
        uniform token primvars:usdGen:role = "guide"
        uint64[] primvars:usdGen:curveId = {arr(ids, 16, "            ")} (
            interpolation = "uniform"
        )
        color3f[] primvars:displayColor = [(1, 0.45, 0.08)]
    }}

    def Xform "Groom"
    {{
        def UsdGenDescription "Hair"
        {{
            rel usdGen:surface = </World/Skin>
            uniform token usdGen:curve:basis = "bspline"
            float usdGen:width:default = 0.004

            def Scope "Ops"
            {{
                # Last in the stack: only this operator reads the animated
                # drivers, so it is the only one that re-runs per frame.
                def UsdGenDeform "deform"
                {{
                    rel usdGen:guides = </World/Drivers>
                    int usdGen:rbfSamples = 100
                    bool usdGen:lockRoots = true
                }}

                def UsdGenWidth "width"
                {{
                    float usdGen:width = 0.005
                    float2[] usdGen:width:knots = [(0, 1), (0.7, 0.75), (1, 0.2)]
                    bool usdGen:replace = true
                }}

                def UsdGenNoise "frizz"
                {{
                    int usdGen:seed = 5
                    float usdGen:noise:magnitude = 0.018
                    float2[] usdGen:noise:magnitude:knots = [(0, 0), (1, 1)]
                    float usdGen:noise:frequency = 5
                    int usdGen:noise:octaves = 2
                    float usdGen:preserveLength = 1
                }}

                def UsdGenGrow "grow"
                {{
                    int usdGen:seed = 13
                    int usdGen:segments = 10
                    float usdGen:length = 0.36
                    float2 usdGen:lengthRandom = (0.85, 1.1)
                    uniform token usdGen:direction = "surfaceNormal"
                }}

                def UsdGenScatter "scatter"
                {{
                    int usdGen:seed = 29
                    float usdGen:density = 3000
                }}
            }}
        }}
    }}
}}
'''
    path = os.path.join(EXAMPLES, "rbf-guides-plane.usda")
    with open(path, "w", newline="\n") as fh:
        fh.write(text)
    print("wrote", path)


if __name__ == "__main__":
    write_guide_example()
    write_clump_example()
    write_rbf_example()
