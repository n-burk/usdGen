#!/usr/bin/env python3
"""Writes examples/metahuman-hair-parity.usda: routes the real, converted
MetaHuman groom in examples/production/metahuman-hair/male_hair_01/ through usdGen
(UsdGenCurveSource reading its native BasisCurves) so the strands get the
default UsdGenHairStrands material, the furTauP/N density bake and the
synthetic scalp shadow, instead of the flat UsdPreviewSurface preview that
asset ships with. Does not modify anything under examples/production/.

    python examples/tools/make_metahuman_parity.py

Also writes examples/metahuman-hair-parity-render.usda, a thin sublayer
wrapper that adds a backdrop sphere for offline renders only (see
make_head_hair.py's header for why: a backdrop authored directly in the
viewable scene blows out usdview's FreeCamera bbox-derived near/far).
"""

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = os.path.dirname(HERE)
SOURCE_ASSET = "./production/metahuman-hair/male_hair_01/hair.usda"


def fmt(v):
    s = "%.6g" % v
    return "0" if s in ("-0", "0") else s


def normalize(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v) if n else v


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def look_at_zup(eye, target):
    """A USD camera transform for a Z-up stage (local -Z is the view
    direction, local +Y is "up" on screen, built from the world +Z axis)."""
    f = normalize(tuple(t - e for t, e in zip(target, eye)))
    r = normalize(cross(f, (0.0, 0.0, 1.0)))
    u = cross(r, f)
    rows = [r + (0.0,), u + (0.0,), tuple(-c for c in f) + (0.0,), tuple(eye) + (1.0,)]
    return "( " + ", ".join("(" + ", ".join(fmt(c) for c in row) + ")" for row in rows) + " )"


def ellipsoid(rx, ry, rz, rings, segments):
    """Rings x segments ellipsoid, real triangle-fan poles (see
    make_head_hair.py's ellipsoid() for why not a degenerate quad ring).
    Poles on the Z axis to match this stage's up axis."""
    def normal_at(x, y, z):
        return normalize((x / (rx * rx), y / (ry * ry), z / (rz * rz)))

    apex_n = (0.0, 0.0, rz)
    apex_s = (0.0, 0.0, -rz)
    points = [apex_n]
    normals = [normal_at(*apex_n)]

    def add_row(j):
        theta = math.pi * j / rings
        z = rz * math.cos(theta)
        s = math.sin(theta)
        row = []
        for i in range(segments):
            phi = 2.0 * math.pi * i / segments
            x = rx * s * math.cos(phi)
            y = ry * s * math.sin(phi)
            row.append(len(points))
            points.append((x, y, z))
            normals.append(normal_at(x, y, z))
        return row

    rows = [add_row(j) for j in range(1, rings)]
    apex_s_index = len(points)
    points.append(apex_s)
    normals.append(normal_at(*apex_s))

    counts, indices = [], []

    def tri(a, b, c):
        indices.extend((a, b, c))
        counts.append(3)

    def quad(a, b, c, d):
        indices.extend((a, b, c, d))
        counts.append(4)

    row0 = rows[0]
    for i in range(segments):
        i2 = (i + 1) % segments
        tri(0, row0[i2], row0[i])

    for j in range(len(rows) - 1):
        a_row, b_row = rows[j], rows[j + 1]
        for i in range(segments):
            i2 = (i + 1) % segments
            quad(a_row[i], a_row[i2], b_row[i2], b_row[i])

    lastRow = rows[-1]
    for i in range(segments):
        i2 = (i + 1) % segments
        tri(apex_s_index, lastRow[i], lastRow[i2])

    return points, normals, counts, indices


def arr(values, per_line=6, indent="        "):
    def vec(v):
        return "(" + ", ".join(fmt(c) for c in v) + ")"
    items = [vec(v) if isinstance(v, (tuple, list)) else fmt(v) for v in values]
    lines = []
    for i in range(0, len(items), per_line):
        lines.append(indent + ", ".join(items[i:i + per_line]))
    return "[\n" + ",\n".join(lines) + "\n" + indent[:-4] + "]"


# World-space (post /World transform: Z-up, cm) bounding boxes, measured with
# UsdGeom.BBoxCache against examples/production/metahuman-hair/male_hair_01/hair.usda:
#   Scalp X [-14.94, 14.94]  Y [-12.69, 8.32]  Z [124.26, 159.73]
#   Hair  X [-8.45, 8.38]    Y [-9.60, 8.90]   Z [136.68, 160.51]
# Face/front is toward -Y (the existing preview Cam sits at y=-110 looking
# toward +Y), crown at z~160, hairline roughly z~148-152 on the side.
TEMPLE_EYE = (36.0, -48.0, 168.0)
TEMPLE_TARGET = (9.0, -5.0, 151.0)
HEAD_EYE = (0.0, -95.0, 152.0)
HEAD_TARGET = (0.0, 2.0, 148.0)


def light_transform_zup(eye, target, up_w, side_w, toward_w, place_at):
    """DistantLight orientation (local -Z is the travel direction), built
    the same way as make_head_hair.py's light_transform_from_camera but for
    this stage's Z-up convention: "up" is world +Z, camera "right" is
    computed against it."""
    f = normalize(tuple(t - e for t, e in zip(target, eye)))
    r = normalize(cross(f, (0.0, 0.0, 1.0)))
    u = cross(r, f)
    back = tuple(-c for c in f)
    side = tuple(-c for c in r)
    source_dir = normalize(tuple(a * toward_w + b * up_w + c * side_w
                                  for a, b, c in zip(back, u, side)))
    travel_dir = tuple(-c for c in source_dir)
    lr = normalize(cross(travel_dir, (0.0, 0.0, 1.0)))
    lu = cross(lr, travel_dir)
    rows = [lr + (0.0,), lu + (0.0,), tuple(-c for c in travel_dir) + (0.0,), tuple(place_at) + (1.0,)]
    return "( " + ", ".join("(" + ", ".join(fmt(c) for c in row) + ")" for row in rows) + " )"


KEY_XFORM = light_transform_zup(TEMPLE_EYE, TEMPLE_TARGET, up_w=0.55, side_w=0.6, toward_w=0.6,
                                 place_at=(-10.0, -70.0, 210.0))
RIM_XFORM = light_transform_zup(TEMPLE_EYE, TEMPLE_TARGET, up_w=0.3, side_w=-0.6, toward_w=-0.7,
                                 place_at=(30.0, 40.0, 130.0))

DOME_TEXTURE = "./maps/StinsonBeach.hdr"

BD_R = 260.0
bd_points, bd_normals, bd_counts, bd_indices = ellipsoid(BD_R, BD_R, BD_R, 12, 16)


def write_backdrop_mesh_body(points, normals, counts, indices, extra_indent="        "):
    p = extra_indent
    lo = [min(v[i] for v in points) for i in range(3)]
    hi = [max(v[i] for v in points) for i in range(3)]
    out = []
    out.append(p + "uniform token subdivisionScheme = \"none\"")
    out.append(p + "float3[] extent = " + arr([tuple(lo), tuple(hi)], per_line=2, indent=p + "    "))
    out.append(p + "int[] faceVertexCounts = " + arr(counts, per_line=16, indent=p + "    "))
    out.append(p + "int[] faceVertexIndices = " + arr(indices, per_line=16, indent=p + "    "))
    out.append(p + "point3f[] points = " + arr(points, per_line=4, indent=p + "    "))
    out.append(p + "normal3f[] normals = " + arr(normals, per_line=4, indent=p + "    ") +
               " (\n" + p + "    interpolation = \"vertex\"\n" + p + ")")
    return "\n".join(out)


scene = []
scene.append('#usda 1.0')
scene.append('(')
scene.append('    defaultPrim = "World"')
scene.append('    subLayers = [')
scene.append('        @%s@' % SOURCE_ASSET)
scene.append('    ]')
scene.append(')')
scene.append('')
scene.append('# Routes examples/production/metahuman-hair/male_hair_01/hair.usda\'s converted')
scene.append('# strands through usdGen instead of the flat UsdPreviewSurface preview that')
scene.append('# asset ships with, so they get the default UsdGenHairStrands material, the')
scene.append('# furTauP/N density bake and the synthetic scalp shadow. Nothing under')
scene.append('# examples/production/ is modified -- this sublayers it and overrides in place.')
scene.append('#')
scene.append('# /World/Hair (the original BasisCurves) is kept ACTIVE (usdGen:curves reads')
scene.append('# it live) but set invisible and stripped of its HairPreviewSurface binding,')
scene.append('# so it is not drawn twice; the new Groom (outside /World, at the stage root,')
scene.append('# so it is not double-transformed by /World\'s 100x Y-up-to-Z-up-cm matrix)')
scene.append('# publishes the actual visible strands.')
scene.append('#')
scene.append('# View it:   .\\bin\\launch_usdview.ps1 examples\\metahuman-hair-parity.usda')
scene.append('# Record it: .\\bin\\render_ue_parity.ps1 -Label current')
scene.append('')
scene.append('over "World"')
scene.append('{')
scene.append('    over "Scalp" (')
scene.append('        prepend apiSchemas = ["UsdGenRestAPI"]')
scene.append('    )')
scene.append('    {')
scene.append('    }')
scene.append('')
scene.append('    over "Hair"')
scene.append('    {')
scene.append('        uniform token visibility = "invisible"')
scene.append('        rel material:binding = None')
scene.append('    }')
scene.append('')
scene.append('    over "Looks"')
scene.append('    {')
scene.append('        over "SkinPreview"')
scene.append('        {')
scene.append('            over "Surface"')
scene.append('            {')
scene.append('                color3f inputs:diffuseColor = (0.80, 0.62, 0.52)')
scene.append('            }')
scene.append('        }')
scene.append('    }')
scene.append('')
scene.append('    # Nested inside World (not a stage-root sibling): UsdGenCurveSource reads')
scene.append('    # /World/Hair\'s points verbatim, in /World/Hair\'s own LOCAL space (it does')
scene.append('    # not resolve them to world space first), and the Description publishes in')
scene.append('    # its own local space, which Storm then carries through its ancestors\' own')
scene.append('    # transforms same as any other prim. A Description outside /World would')
scene.append('    # publish untransformed -- pre-/World\'s-100x-Y-up-to-Z-up-cm-matrix -- and')
scene.append('    # render effectively invisible (confirmed: that was tried first and gave a')
scene.append('    # a bald head with no visible strands even though the cook itself succeeded,')
scene.append('    # 40/40 chunks published, per TF_DEBUG USDGEN_COMMIT).')
scene.append('    def Xform "Groom"')
scene.append('    {')
scene.append('        def UsdGenDescription "Hair" (')
scene.append('            prepend apiSchemas = ["UsdGenLookAPI", "MaterialBindingAPI"]')
scene.append('        )')
scene.append('        {')
scene.append('            rel usdGen:surface = </World/Scalp>')
scene.append('            uniform token usdGen:curve:basis = "bspline"')
scene.append('')
scene.append('            # The look drives the whole coat colour, and there is')
scene.append('            # deliberately NO material bound here: the publisher builds the')
scene.append('            # synthetic default UsdGenHairStrands material from this look, so')
scene.append('            # it carries BOTH ends of the ramp. An explicitly bound material')
scene.append('            # replaces that synthetic one, and since only rootColor reaches')
scene.append('            # the shader as a primvar (baked into displayColor) while tipColor')
scene.append('            # is a material input, binding one silently pins every strand\'s')
scene.append('            # tip half to the Sdr default tipColor (0.21, 0.115, 0.045), a')
scene.append('            # light blond. On a groom viewed down the strands that tip colour')
scene.append('            # is most of what you see, so the look appeared to have almost no')
scene.append('            # authority -- measured: halving rootColor moved the coat 7.5%,')
scene.append('            # and the shader albedo read a max of exactly 0.2086 with the look')
scene.append('            # set to pure black.')
scene.append('            #')
scene.append('            # Both colours are melanin colours, so the choice stays a')
scene.append('            # physical one: rootColor is UE GetHairColorFromMelanin at')
scene.append('            # melanin 0.19 / redness 0.15, tipColor the same at melanin')
scene.append('            # 0.10 -- tips carry less pigment and read as the lighter')
scene.append('            # flyaways in the target photograph.')
scene.append('            color3f usdGen:look:rootColor = (0.153, 0.086, 0.030)')
scene.append('            color3f usdGen:look:tipColor = (0.266, 0.176, 0.083)')
scene.append('')
scene.append('            def Scope "Ops"')
scene.append('            {')
scene.append('                # The only Op: reads /World/Hair\'s native BasisCurves verbatim.')
scene.append('                # usdGen:rebind = "always" because the source curves carry')
scene.append('                # primvars:rest and primvars:usdGen:curveId but no')
scene.append('                # primvars:usdGen:rootFrame -- root frames must be re-derived')
scene.append('                # from the bound Scalp surface, not read from the source.')
scene.append('                def UsdGenCurveSource "source"')
scene.append('                {')
scene.append('                    rel usdGen:curves = </World/Hair>')
scene.append('                    uniform token usdGen:idSource = "primvar"')
scene.append('                    uniform token usdGen:rebind = "always"')
scene.append('                }')
scene.append('            }')
scene.append('        }')
scene.append('')
scene.append('        # No Material prim: see the note on the look above. Everything the')
scene.append('        # synthetic default material cannot carry is left at its Sdr default,')
scene.append('        # which for this groom is the right call anyway -- roughness 0.35 is')
scene.append('        # inside UE\'s own 0.3-0.5 hair range and within 4% of the measured')
scene.append('        # peak of the R lobe for this key/view pair (lit R of 0.089 at 0.30')
scene.append('        # against 0.085 at 0.35, scratch bsdf_check.py, an independent')
scene.append('        # transcription of HairBsdf.ush), and useMelanin defaults to 0 so the')
scene.append('        # melanin colour never overrides the look.')
scene.append('    }')
scene.append('}')
scene.append('')
scene.append('def Camera "TempleCam"')
scene.append('{')
scene.append('    uniform token[] xformOpOrder = ["xformOp:transform"]')
scene.append('    matrix4d xformOp:transform = %s' % look_at_zup(TEMPLE_EYE, TEMPLE_TARGET))
scene.append('    float2 clippingRange = (1, 2000)')
scene.append('    float focalLength = 62')
scene.append('    float horizontalAperture = 24')
scene.append('    float verticalAperture = 18')
scene.append('}')
scene.append('')
scene.append('def Camera "HeadCam"')
scene.append('{')
scene.append('    uniform token[] xformOpOrder = ["xformOp:transform"]')
scene.append('    matrix4d xformOp:transform = %s' % look_at_zup(HEAD_EYE, HEAD_TARGET))
scene.append('    float2 clippingRange = (1, 2000)')
scene.append('    float focalLength = 55')
scene.append('    float horizontalAperture = 24')
scene.append('    float verticalAperture = 18')
scene.append('}')
scene.append('')
scene.append('# Same recipe as head-hair-closeup.usda (inputs:normalize = 1 -- HdSt')
scene.append('# otherwise multiplies intensity by the DistantLight\'s tiny default solid')
scene.append('# angle -- key from upper front-side of TempleCam, weak rim, DomeLight fill).')
scene.append('def DistantLight "Key"')
scene.append('{')
scene.append('    uniform token[] xformOpOrder = ["xformOp:transform"]')
scene.append('    matrix4d xformOp:transform = %s' % KEY_XFORM)
scene.append('    # Exposure: 0.465x the values this rig was first written with. At the')
scene.append('    # original 3.8 / 0.9 / 1.0 the lit forehead came out at 0.86 linear with')
scene.append('    # 70% of its red channel clipped, so every gap in the coat read as glare')
scene.append('    # and the hair could not be judged against it. The response is linear in')
scene.append('    # the light scale -- there is no tonemap anywhere in this path -- so the')
scene.append('    # scale was solved directly from a 0.25x render: 0.40 linear, sRGB 0.67.')
scene.append('    # The key : rim : sky ratio is unchanged.')
scene.append('    float inputs:intensity = 1.77')
scene.append('    bool inputs:normalize = 1')
scene.append('    color3f inputs:color = (1, 0.98, 0.94)')
scene.append('}')
scene.append('')
scene.append('def DistantLight "Rim"')
scene.append('{')
scene.append('    uniform token[] xformOpOrder = ["xformOp:transform"]')
scene.append('    matrix4d xformOp:transform = %s' % RIM_XFORM)
scene.append('    float inputs:intensity = 0.42')
scene.append('    bool inputs:normalize = 1')
scene.append('    color3f inputs:color = (0.85, 0.9, 1)')
scene.append('}')
scene.append('')
scene.append('def DomeLight "Sky"')
scene.append('{')
scene.append('    asset inputs:texture:file = @%s@' % DOME_TEXTURE)
scene.append('    float inputs:intensity = 0.47')
scene.append('    color3f inputs:color = (1, 1, 1)')
scene.append('}')

out_text = "\n".join(scene)
path = os.path.join(EXAMPLES, "metahuman-hair-parity.usda")
with open(path, "w", newline="\n") as f:
    f.write(out_text)
    f.write("\n")
print("wrote", path)

# ---------------------------------------------------------------------------
# Render-only wrapper: adds the backdrop sphere (see make_head_hair.py).
# ---------------------------------------------------------------------------
render_scene = []
render_scene.append('#usda 1.0')
render_scene.append('(')
render_scene.append('    defaultPrim = "World"')
render_scene.append('    subLayers = [')
render_scene.append('        @./metahuman-hair-parity.usda@')
render_scene.append('    ]')
render_scene.append(')')
render_scene.append('')
render_scene.append('def Mesh "Backdrop" (')
render_scene.append('    prepend apiSchemas = ["MaterialBindingAPI"]')
render_scene.append(')')
render_scene.append('{')
render_scene.append('    uniform token orientation = "leftHanded"')
render_scene.append(write_backdrop_mesh_body(bd_points, bd_normals, bd_counts, bd_indices))
render_scene.append('    rel material:binding = </Looks/Backdrop>')
render_scene.append('}')
render_scene.append('')
render_scene.append('def Scope "Looks"')
render_scene.append('{')
render_scene.append('    def Material "Backdrop"')
render_scene.append('    {')
render_scene.append('        token outputs:surface.connect = </Looks/Backdrop/Surface.outputs:surface>')
render_scene.append('')
render_scene.append('        def Shader "Surface"')
render_scene.append('        {')
render_scene.append('            uniform token info:id = "UsdPreviewSurface"')
render_scene.append('            color3f inputs:diffuseColor = (0, 0, 0)')
render_scene.append('            color3f inputs:emissiveColor = (0.82, 0.82, 0.82)')
render_scene.append('            float inputs:roughness = 1')
render_scene.append('            token outputs:surface')
render_scene.append('        }')
render_scene.append('    }')
render_scene.append('}')

render_path = os.path.join(EXAMPLES, "metahuman-hair-parity-render.usda")
with open(render_path, "w", newline="\n") as f:
    f.write("\n".join(render_scene))
    f.write("\n")
print("wrote", render_path)
