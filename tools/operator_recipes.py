"""Source-grounded scene recipes for the operator manual's patch renders.

Each ``ops`` value is the entire content of a Description's Ops scope, in
reverse sibling order: the visually demonstrated operator comes first and
Scatter or a source comes last. ``extra_prims`` is inserted beneath /World.
Only recipes tagged ``live`` should be called a live render of that operator.
"""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import math


def _op(typ: str, name: str, attrs: str = "") -> str:
    body = "\n".join("    " + line for line in attrs.strip().splitlines())
    return f'def {typ} "{name}" {{\n{body}\n}}'


def _chain(*operators: str) -> str:
    return "\n".join("            " + line for op in operators for line in op.splitlines())


SCATTER = _op("UsdGenScatter", "scatter", """
int usdGen:seed = 41
float usdGen:density = 11000
""")
GROW = _op("UsdGenGrow", "grow", """
int usdGen:seed = 19
int usdGen:segments = 18
float usdGen:length = 0.18
float2 usdGen:lengthRandom = (0.84, 1.05)
token usdGen:direction = "vector"
vector3f usdGen:directionVector = (0.62, 0.56, 0.27)
""")
WIDTH = _op("UsdGenWidth", "width", """
float usdGen:width = 0.0011
float2[] usdGen:width:knots = [(0, 1), (0.5, 0.62), (1, 0.018)]
""")
BASE_BEND = _op("UsdGenBend", "baseBend", """
float usdGen:angle = 11
float2[] usdGen:angle:knots = [(0, 0), (0.45, 0.35), (1, 1)]
float2 usdGen:angleRandom = (0.68, 1.18)
""")
BASE_NOISE = _op("UsdGenNoise", "baseNoise", """
int usdGen:seed = 37
float usdGen:noise:magnitude = 0.0018
float usdGen:noise:frequency = 3.5
float2[] usdGen:noise:magnitude:knots = [(0, 0), (0.5, 0.24), (1, 1)]
""")


def _fur(*stylers: str) -> str:
    return _chain(WIDTH, *stylers, BASE_NOISE, BASE_BEND, GROW, SCATTER)


def _curve_set(name: str, nx=12, nz=12, cvs=12, guide=False) -> str:
    """A self-contained C3 curve set with stable IDs and rest points."""
    points = []
    for iz in range(nz):
        for ix in range(nx):
            x = 0.04 + ix * 0.92 / max(nx - 1, 1)
            z = 0.04 + iz * 0.92 / max(nz - 1, 1)
            for k in range(cvs):
                t = k / (cvs - 1)
                # A combed, slightly bowed source shape: clearly different
                # from the procedural straight Grow recipe.
                points.append((x + 0.105*t + 0.018*math.sin(math.pi*t),
                               0.17*t, z + 0.018*t*t))
    counts = ", ".join(str(cvs) for _ in range(nx*nz))
    positions = ",\n        ".join(f"({x:.6f}, {y:.6f}, {z:.6f})" for x,y,z in points)
    ids = ", ".join(str(i) for i in range(nx*nz))
    role = "guide" if guide else "hair"
    return f'''def BasisCurves "{name}" (prepend apiSchemas = ["UsdGenCurveAPI"]) {{
    uniform token purpose = "guide"
    uniform token type = "cubic"
    uniform token basis = "bspline"
    uniform token wrap = "pinned"
    int[] curveVertexCounts = [{counts}]
    point3f[] points = [
        {positions}
    ]
    point3f[] primvars:rest = [
        {positions}
    ] (interpolation = "vertex")
    uint64[] primvars:usdGen:curveId = [{ids}] (interpolation = "uniform")
    token primvars:usdGen:role = "{role}" (interpolation = "constant")
}}'''


SOURCE_NX = SOURCE_NZ = 24
SOURCE_CURVES = _curve_set("Imported", nx=SOURCE_NX, nz=SOURCE_NZ)
REFERENCE_CURVES = _curve_set("ReferenceSnapshot", nx=28, nz=28)
SOURCE = _op("UsdGenCurveSource", "source", """
rel usdGen:curves = </World/Imported>
bool usdGen:useRest = false
token usdGen:idSource = "primvar"
token usdGen:rebind = "onError"
""")
REFERENCE = _op("UsdGenReferenceSource", "source", """
rel usdGen:reference = </World/ReferenceSnapshot>
""")


def _sculpt_attrs() -> str:
    ids = [iz * SOURCE_NX + ix for iz in range(9, 15) for ix in range(9, 15)]
    offsets = [i*12 for i in range(len(ids)+1)]
    deltas = []
    for ident in ids:
        for k in range(12):
            t = k/11
            # A local combed tuft in the center of the source patch.
            sign = -1 if ident % 2 else 1
            deltas.append((0.035*sign*t*t, 0.045*t*t, 0.02*t*t))
    fmt = lambda seq: ", ".join(str(x) for x in seq)
    vecs = ", ".join(f"({x:.5f}, {y:.5f}, {z:.5f})" for x,y,z in deltas)
    return f'''uint64[] usdGen:sculpt:curveIds = [{fmt(ids)}]
int[] usdGen:sculpt:cvOffsets = [{fmt(offsets)}]
vector3f[] usdGen:sculpt:deltas = [{vecs}]
float usdGen:sculpt:weight = 1'''


PART_LINE = '''def BasisCurves "PartLine" (prepend apiSchemas = ["UsdGenCurveAPI"]) {
    uniform token purpose = "guide"
    uniform token type = "cubic"
    uniform token basis = "bspline"
    uniform token wrap = "pinned"
    int[] curveVertexCounts = [8]
    point3f[] points = [(0.5, 0, 0), (0.5, 0, 0.14), (0.5, 0, 0.29), (0.5, 0, 0.43), (0.5, 0, 0.57), (0.5, 0, 0.71), (0.5, 0, 0.86), (0.5, 0, 1)]
    uint64[] primvars:usdGen:curveId = [1] (interpolation = "uniform")
    token primvars:usdGen:role = "guide" (interpolation = "constant")
}'''


COLLIDER = '''def Mesh "Shield" {
    uniform token purpose = "guide"
    uniform token subdivisionScheme = "none"
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 2, 3, 4, 7, 6, 5, 0, 4, 5, 1, 1, 5, 6, 2, 2, 6, 7, 3, 3, 7, 4, 0]
    point3f[] points = [(0.43, 0.08, 0.35), (0.83, 0.08, 0.35), (0.83, 0.08, 0.74), (0.43, 0.08, 0.74), (0.43, 0.22, 0.35), (0.83, 0.22, 0.35), (0.83, 0.22, 0.74), (0.43, 0.22, 0.74)]
}'''


def _recipe(ops="", caption="", kind="live", **extras):
    return {"ops": ops, "caption": caption, "kind": kind,
            "extra_prims": extras.pop("extra_prims", ""),
            "description_overrides": extras.pop("description_overrides", ""),
            **extras}


RECIPES = {
    "UsdGenScatter": _recipe(_chain(WIDTH, _op("UsdGenGrow", "grow", """
int usdGen:segments = 7
float usdGen:length = 0.075
token usdGen:direction = "surfaceNormal"
"""), _op("UsdGenScatter", "scatter", """
int usdGen:seed = 41
float usdGen:density = 2600
""")), "Evenly distributed root sites shown as short upright fur."),
    "UsdGenGrow": _recipe(_fur(), "Scatter roots grown into 18-CV strands with a combed lean."),
    "UsdGenClump": _recipe(_fur(_op("UsdGenClump", "clump", """
float usdGen:clump:amount = 0.72
float usdGen:clump:density = 36
float usdGen:clump:size = 0.12
float2[] usdGen:clump:profile:knots = [(0, 0), (0.3, 0.3), (1, 1)]
""")), "Clump centers draw nearby tips together while the roots stay separate."),
    "UsdGenNoise": _recipe(_fur(_op("UsdGenNoise", "noise", """
int usdGen:seed = 7
float usdGen:noise:magnitude = 0.012
float usdGen:noise:frequency = 13
int usdGen:noise:octaves = 2
float2[] usdGen:noise:magnitude:knots = [(0, 0), (0.5, 0.35), (1, 1)]
""")), "Tip-weighted noise breaks up the smooth combed profile."),
    "UsdGenLength": _recipe(_fur(_op("UsdGenLength", "length", """
token usdGen:length:mode = "set"
float usdGen:length:value = 0.145
float2 usdGen:length:random = (0.85, 1.12)
""")), "Set mode gives each strand a shorter absolute length with stable variation."),
    "UsdGenWidth": _recipe(_chain(_op("UsdGenWidth", "width", """
float usdGen:width = 0.0012
float2[] usdGen:width:knots = [(0, 1), (0.6, 0.48), (1, 0.006)]
"""), BASE_NOISE, BASE_BEND, GROW, SCATTER), "A fine root profile tapers nearly to zero at the tip."),
    "UsdGenDirection": _recipe(_fur(_op("UsdGenDirection", "direction", """
vector3f usdGen:direction = (0, 1, 0)
float usdGen:amount = 0.9
float2[] usdGen:direction:knots = [(0, 0.08), (1, 1)]
""")), "Direction combs the leaning input toward a more upright target."),
    "UsdGenSmooth": _recipe(_fur(_op("UsdGenSmooth", "smooth", """
float usdGen:strength = 0.68
int usdGen:iterations = 3
token usdGen:mode = "alongCurve"
"""), _op("UsdGenNoise", "noise", """
float usdGen:noise:magnitude = 0.028
float usdGen:noise:frequency = 18
int usdGen:noise:octaves = 2
""")), "Smooth relaxes the strong upstream noise along each strand."),
    "UsdGenResample": _recipe(_fur(_op("UsdGenResample", "resample", """
int usdGen:cvCount = 7
token usdGen:distribution = "uniform"
"""), _op("UsdGenCurl", "curl", """
float usdGen:radius = 0.018
float usdGen:frequency = 12
""")), "Seven uniformly spaced CVs simplify an upstream curl.", kind="comparison"),
    "UsdGenScale": _recipe(_fur(_op("UsdGenScale", "scale", """
float usdGen:scale = 0.72
float2[] usdGen:scale:knots = [(0, 1), (1, 0.82)]
""")), "Scale shortens offsets from each fixed root, most at the tips."),
    "UsdGenCurl": _recipe(_fur(_op("UsdGenCurl", "curl", """
float usdGen:radius = 0.018
float usdGen:frequency = 14
float usdGen:phaseRandom = 0.7
""")), "A radius ramp and varied phase make defined coils."),
    "UsdGenBend": _recipe(_fur(_op("UsdGenBend", "bend", """
float usdGen:angle = 55
float2[] usdGen:angle:knots = [(0, 0), (1, 1)]
float2 usdGen:angleRandom = (0.7, 1.3)
""")), "Bend turns the tips while preserving attachment at the roots."),
    "UsdGenStraighten": _recipe(_fur(_op("UsdGenStraighten", "straighten", """
float usdGen:tangentStraightness = 0.8
float usdGen:normalStraightness = 0.5
"""), _op("UsdGenCurl", "curl", """
float usdGen:radius = 0.015
float usdGen:frequency = 9
""")), "Straighten reduces an upstream curl's curvature.", kind="comparison"),
    "UsdGenDisplace": _recipe(_fur(_op("UsdGenDisplace", "displace", """
float usdGen:displace:amount = 1
float usdGen:displace:base = 0
float usdGen:displace:scale = 1
float usdGen:displace:offset = 0.035
token usdGen:mode = "height"
""")), "A constant height offset lifts all CVs, including roots, along the rest normal."),
    "UsdGenWave": _recipe(_fur(_op("UsdGenWave", "wave", """
float usdGen:frequencyU = 10
float usdGen:frequencyN = 13
float usdGen:amplitudeU = 0.018
float usdGen:amplitudeN = 0.012
""")), "Independent U and normal waves ripple the strands."),
    "UsdGenPart": _recipe(_fur(_op("UsdGenPart", "part", """
rel usdGen:part:curves = </World/PartLine>
float usdGen:part:radius = 0.2
float usdGen:part:strength = 0.18
""")), "Part leaves geometry unchanged and assigns partId 0/1 on either side of the center line; distant roots receive -1.", kind="data_plane", extra_prims=PART_LINE),
    "UsdGenExprOp": _recipe(_fur(_op("UsdGenExprOp", "expression", """
string usdGen:expr:source = "[$t * $t * 0.06, 0, -$t * 0.025]"
token usdGen:expr:returnType = "displacement"
token usdGen:mode = "cv"
""")), "An expression adds an increasing tip displacement in X and Z."),
    "UsdGenWind": _recipe(_fur(_op("UsdGenWind", "wind", """
vector3f usdGen:direction = (1, 0, 0)
float usdGen:constStrength = 0.085
float usdGen:gustStrength = 0
float usdGen:stiffness = 0.18
""")), "Steady wind bends tips toward +X while planted roots remain fixed."),
    "UsdGenCurveSource": _recipe(_chain(WIDTH, SOURCE), "Authored C3 curves enter the stack through CurveSource.", extra_prims=SOURCE_CURVES),
    "UsdGenReferenceSource": _recipe(_chain(WIDTH, REFERENCE), "A baked in-stage C3 snapshot enters through ReferenceSource.", extra_prims=REFERENCE_CURVES),
    "UsdGenSculptLayer": _recipe(_chain(WIDTH, _op("UsdGenSculptLayer", "sculpt", _sculpt_attrs()), SOURCE), "Stable-ID sculpt deltas lift and fan a selected tuft of imported curves.", extra_prims=SOURCE_CURVES),
    "UsdGenCollide": _recipe(_fur(_op("UsdGenCollide", "collide", """
rel usdGen:colliders = </World/Shield>
float usdGen:pushAmount = 1
float usdGen:offset = 0.006
int usdGen:iterations = 3
token usdGen:resolveType = "flexible"
""")), "Strands penetrating the shield are pushed outside it.", extra_prims=COLLIDER),
    "UsdGenGuideInterpolate": _recipe(caption="Dense strands follow a sparse authored guide set, with region-aware blending.", kind="existing_scene", scene_path="examples/guide-interpolate-plane.usda"),
    "UsdGenDeform": _recipe(caption="At frame 24, animated driver curves bend the groom through an RBF field.", kind="existing_scene", scene_path="examples/rbf-guides-plane.usda", frame=24),
    "UsdGenFreeze": _recipe(_chain(WIDTH, _op("UsdGenFreeze", "freeze", """
token usdGen:frozen:mode = "frozen"
"""), GROW, SCATTER), caption="The captured strands retain their shape and width while the live input changes; Freeze can also install an explicit curve snapshot.", kind="storyboard"),
    "UsdGenWidthBlend": _recipe(caption="Two ordered curve streams keep their geometry while WidthBlend interpolates only the width plane between them at weight 0.5.", kind="internal"),
    "UsdGenInstance": _recipe(caption="Instance is a reserved schema type. Its prototype controls are documented for orientation, but this build has no executable Instance kernel.", kind="reserved"),
}

assert len(RECIPES) == 27
