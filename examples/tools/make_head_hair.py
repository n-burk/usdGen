#!/usr/bin/env python3
"""Writes examples/head-hair-closeup.usda: a head-sized ellipsoid scalp with a
dense, dark, combed-back groom that fades to bare skin at a "hairline" band,
built to compare against a character-asset reference photo (the host renderer parity work).

    python examples/tools/make_head_hair.py

Standalone generator (does not touch examples/tools/make_examples.py): it
follows the same "plain text so it diffs well" convention, computing an
ellipsoid mesh (with real triangle-fan poles) and its vertex normals.
"""

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = os.path.dirname(HERE)


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


# ---------------------------------------------------------------------------
# Ellipsoid mesh: rings x segments quads. Row j=0 is the north pole (crown),
# row j=rings is the south pole (chin/throat underside, unseen/unused).
# v = 1 at the crown, 0 at the far pole -- used directly as the scalp's
# "elevation" coordinate so operator expressions need no vector math.
# ---------------------------------------------------------------------------

def ellipsoid(name, rx, ry, rz, rings, segments, indent="    "):
    """Rings x segments ellipsoid with real triangle-fan poles (not a ring of
    coincident points): a degenerate polar quad ring gives near-zero-area
    faces there, and usdGen's per-face root frame goes singular on them --
    strands scattered on the pole ring come out wildly, visibly wrong (long
    stray streaks). v = 1 at the crown (north pole), 0 at the far pole."""
    def normal_at(x, y, z):
        return normalize((x / (rx * rx), y / (ry * ry), z / (rz * rz)))

    apex_n = (0.0, ry, 0.0)
    apex_s = (0.0, -ry, 0.0)
    points = [apex_n]
    normals = [normal_at(*apex_n)]
    st = [(0.5, 1.0)]

    def add_row(j):
        theta = math.pi * j / rings
        y = ry * math.cos(theta)
        s = math.sin(theta)
        v = 1.0 - j / rings
        row = []
        for i in range(segments):
            phi = 2.0 * math.pi * i / segments
            x = rx * s * math.cos(phi)
            z = rz * s * math.sin(phi)
            row.append(len(points))
            points.append((x, y, z))
            normals.append(normal_at(x, y, z))
            st.append((i / segments, v))
        return row

    rows = [add_row(j) for j in range(1, rings)]   # rings-1 interior rings

    apex_s_index = len(points)
    points.append(apex_s)
    normals.append(normal_at(*apex_s))
    st.append((0.5, 0.0))

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
        tri(0, row0[i2], row0[i])                  # north cap, outward winding

    for j in range(len(rows) - 1):
        a_row, b_row = rows[j], rows[j + 1]
        for i in range(segments):
            i2 = (i + 1) % segments
            quad(a_row[i], a_row[i2], b_row[i2], b_row[i])

    lastRow = rows[-1]
    for i in range(segments):
        i2 = (i + 1) % segments
        tri(apex_s_index, lastRow[i], lastRow[i2])  # south cap, outward winding

    return points, normals, st, counts, indices


def extent_of(points):
    lo = [min(p[i] for p in points) for i in range(3)]
    hi = [max(p[i] for p in points) for i in range(3)]
    return [tuple(lo), tuple(hi)]


def write_mesh_body(points, normals, st, counts, indices, extra_indent="        "):
    p = extra_indent
    out = []
    out.append(p + "uniform token subdivisionScheme = \"none\"")
    # Authored so usdview's FreeCamera can size near/far from the stage bbox
    # without walking every point (Usdviewq/freeCamera.py setClippingPlanes).
    out.append(p + "float3[] extent = " + arr(extent_of(points), per_line=2, indent=p + "    "))
    out.append(p + "int[] faceVertexCounts = " + arr(counts, per_line=16, indent=p + "    "))
    out.append(p + "int[] faceVertexIndices = " + arr(indices, per_line=16, indent=p + "    "))
    out.append(p + "point3f[] points = " + arr(points, per_line=4, indent=p + "    "))
    out.append(p + "point3f[] primvars:rest = " + arr(points, per_line=4, indent=p + "    ") +
               " (\n" + p + "    interpolation = \"vertex\"\n" + p + ")")
    out.append(p + "normal3f[] normals = " + arr(normals, per_line=4, indent=p + "    ") +
               " (\n" + p + "    interpolation = \"vertex\"\n" + p + ")")
    out.append(p + "texCoord2f[] primvars:st = " + arr(st, per_line=6, indent=p + "    ") +
               " (\n" + p + "    interpolation = \"vertex\"\n" + p + ")")
    return "\n".join(out)


RX, RY, RZ = 8.5, 10.0, 9.0     # head ellipsoid radii, cm (~17x20x18 cm head)
RINGS, SEGMENTS = 56, 80
FADE_LOW, FADE_HIGH = 0.30, 0.66  # elevation band the fade expression ramps over
DENSITY = 450.0                  # roots per sq cm (whole-head scatter; groom2 pass)

points, normals, st, counts, indices = ellipsoid("Head", RX, RY, RZ, RINGS, SEGMENTS)

# A big inverted sphere lights the frame with a flat pale-grey backdrop from
# any camera angle (orientation="leftHanded" flips the winding so the camera,
# sitting inside it, sees its lit inner face).
BD_R = 260.0
bd_points, bd_normals, bd_st, bd_counts, bd_indices = ellipsoid(
    "Backdrop", BD_R, BD_R, BD_R, 12, 16)

# Eye / target near the temple's fade boundary (elevation ~0.30-0.35,
# phi=180): close on the crown/fade transition, mimicking the reference
# photo's composition (hair mass upper-right, fade + bare skin lower-left --
# mirrored from the +X side so it reads that way in the rendered frame).
CAM_TEMPLE_EYE = (-19.0, 4.0, 9.5)
CAM_TEMPLE_TARGET = (-7.0, -3.5, 1.0)
CAM_HEAD_EYE = (24.0, 9.0, 30.0)
CAM_HEAD_TARGET = (0.0, -1.0, 0.0)


def add(*vs):
    return tuple(sum(c) for c in zip(*vs))


def scale(v, s):
    return tuple(c * s for c in v)


def light_transform_from_camera(eye, target, up_w, left_w, toward_w, place_at):
    """A DistantLight xformOp:transform (local -Z is the travel direction)
    positioned so its light comes from roughly `up_w` up, `left_w` toward
    camera-left and `toward_w` back toward the camera -- "upper front-left of
    camera" -- computed from the camera's own basis so it tracks CAM_TEMPLE_*
    instead of a hand-picked Euler triple. `place_at` is just where the light
    prim sits in the scene (cosmetic only; a DistantLight's position does not
    affect its illumination, only its orientation does)."""
    f = normalize(tuple(t - e for t, e in zip(target, eye)))       # camera forward (into the scene)
    r = normalize(cross(f, (0.0, 1.0, 0.0)))                       # camera right
    u = cross(r, f)                                                # camera up
    left = scale(r, -1.0)
    back = scale(f, -1.0)                                          # back toward the camera/viewer
    source_dir = normalize(add(scale(back, toward_w), scale(u, up_w), scale(left, left_w)))
    travel_dir = scale(source_dir, -1.0)                           # light travels from source toward the subject
    # Same construction as look_at (local -Z -> travel_dir), translation is
    # place_at rather than an eye position.
    lr = normalize(cross(travel_dir, (0.0, 1.0, 0.0)))
    lu = cross(lr, travel_dir)
    rows = [lr + (0.0,), lu + (0.0,), tuple(-c for c in travel_dir) + (0.0,), tuple(place_at) + (1.0,)]
    return "( " + ", ".join("(" + ", ".join(fmt(c) for c in row) + ")" for row in rows) + " )"


KEY_XFORM = light_transform_from_camera(
    CAM_TEMPLE_EYE, CAM_TEMPLE_TARGET, up_w=0.55, left_w=0.6, toward_w=0.6,
    place_at=(-12.0, 14.0, 16.0))
RIM_XFORM = light_transform_from_camera(
    CAM_TEMPLE_EYE, CAM_TEMPLE_TARGET, up_w=0.35, left_w=-0.5, toward_w=-0.7,
    place_at=(4.0, 10.0, -14.0))

# A lat-long HDRI -- a DomeLight with no inputs:texture:file compiles its
# whole indirect-lighting block out in Storm (ws1-shader), so *some* texture
# is mandatory, not optional polish. Originally found shipped with this
# build's OpenUSD prefix (Hdx's own test asset,
# usd-install/lib/usd/hdx/resources/textures/StinsonBeach.hdr) but copied into
# examples/maps/ and referenced relatively: an absolute D:/ path baked into a
# committed example would break for anyone without that exact usdRig prefix.
DOME_TEXTURE = "./maps/StinsonBeach.hdr"

# Re-exposure (measured on a TempleCam render at the pre-scale intensities
# below, converted sRGB->linear properly -- Storm's render path is linear
# with no tonemap, so a single measured render solves the scale exactly):
# the best clean, unshadowed 24x24px bare-skin window (a max-mean box scan
# rejecting any window touching the pale background) averaged 0.196 linear,
# against target_metahuman.png's own measured 0.454 linear -- about 2.3x
# under. Target mid-range is 0.40-0.45; the true ceiling is a handful (~50
# of 1.2M) of hair/background silhouette anti-aliasing pixels, not real
# material response (traced their coordinates: they trace the diagonal
# hair/skin/background boundary line exactly), whose 99.99th-percentile
# value of 0.44 sets the real headroom. 2.17x lands skin at ~0.43 with that
# headroom essentially exhausted (99.99th pct * 2.17 = 0.95) rather than
# actually clipped; a handful of those literal edge pixels do tip over 1.0.
EXPOSURE_SCALE = 2.17

scene = []
scene.append('#usda 1.0')
scene.append('(')
scene.append('    defaultPrim = "World"')
scene.append('    metersPerUnit = 1')
scene.append('    upAxis = "Y"')
scene.append('    doc = "Head-hair close-up: a dense, dark, combed groom on a head-sized '
             'ellipsoid, fading to bare skin at a scalp-cap boundary -- a '
             'render-comparison scene for a character groom (renders/ue-parity)."')
scene.append(')')
scene.append('')
scene.append('# Numbers are centimetres by convention (as in styled-fur-plane.usda: hair')
scene.append('# widths of 0.004-0.012, lengths of 0.25-0.4 there read as a small swatch;')
scene.append('# here the head itself is ~%s x %s x %s cm and strands are 3-6cm).' % (
    fmt(2 * RX), fmt(2 * RY), fmt(2 * RZ)))
scene.append('#')
scene.append('# FADE MECHANISM')
scene.append('#   Scatter roots the whole Head ellipsoid uniformly (usdGen:density is one')
scene.append('#   groom-wide scalar; binding usdGen:surface to a scalp GeomSubset would')
scene.append('#   restrict the roots, but with a hard edge, and a fade is graded:')
scene.append('#   see the Head mesh comment below). An expression')
scene.append('#   (Expressions/scalpFade, elevation -> [0, 1], elevation being the mesh\'s')
scene.append('#   own "v" -- 1 at the crown, 0 at the far pole) shrinks each strand toward')
scene.append('#   zero as its root drops from %s to %s elevation, connected to BOTH the' % (
    fmt(FADE_HIGH), fmt(FADE_LOW)))
scene.append('#   Length value and the Width base so short hairs thin out too. Length\'s')
scene.append('#   usdGen:cullThreshold then zero-lengthens (hides) whatever the fade has')
scene.append('#   shrunk past a visible length, so most of the head reads as bare skin even')
scene.append('#   though every strand was scattered.')
scene.append('#')
scene.append('# Two cameras: TempleCam frames the crown/fade transition like the')
scene.append('# reference photo, HeadCam is a medium shot of the whole head.')
scene.append('#')
scene.append('# View it:   .\\bin\\launch_usdview.ps1 examples\\head-hair-closeup.usda')
scene.append('# Record it: .\\bin\\render_ue_parity.ps1 -Label current')
scene.append('')
scene.append('def Xform "World"')
scene.append('{')

# Cameras -----------------------------------------------------------------
scene.append('    def Camera "TempleCam"')
scene.append('    {')
scene.append('        uniform token[] xformOpOrder = ["xformOp:transform"]')
scene.append('        matrix4d xformOp:transform = %s' % look_at(CAM_TEMPLE_EYE, CAM_TEMPLE_TARGET))
scene.append('        float2 clippingRange = (0.05, 500)')
scene.append('        float focalLength = 42')
scene.append('        float horizontalAperture = 24')
scene.append('        float verticalAperture = 18')
scene.append('    }')
scene.append('')
scene.append('    def Camera "HeadCam"')
scene.append('    {')
scene.append('        uniform token[] xformOpOrder = ["xformOp:transform"]')
scene.append('        matrix4d xformOp:transform = %s' % look_at(CAM_HEAD_EYE, CAM_HEAD_TARGET))
scene.append('        float2 clippingRange = (0.05, 500)')
scene.append('        float focalLength = 40')
scene.append('        float horizontalAperture = 24')
scene.append('        float verticalAperture = 18')
scene.append('    }')
scene.append('')

# Lights ----------------------------------------------------------------
# HdSt multiplies a DistantLight's intensity by its (tiny, 0.53deg-default)
# solid angle unless inputs:normalize = 1 -- without it, intensity 2-3 comes
# out around 2e-4 in practice and the light is invisible next to usdrecord's
# camera headlight. Key comes from upper front-left of TempleCam (computed
# from the camera basis, not a guessed Euler triple -- see
# light_transform_from_camera above); a much weaker rim from the opposite
# side keeps the far edge of the hair mass from going flat black now that
# the camera headlight is off (render_ue_parity.ps1 -NoCameraLight).
#
# SHADOWS: tried (ShadowAPI + inputs:shadow:enable = 1), to see hair self-
# shadowing the scalp. An initial A/B render appeared to show a rectangular
# clipping artifact on HeadCam, but that did not reproduce on retest at full
# resolution and was independently confirmed (source read, by a teammate) to
# be a genuine no-op in this stack: usdImaging never produces
# HdLightTokens->shadowParams, so HdxShadowParams::enabled stays false and
# HdxTaskController::SetEnableShadows is never called by usdrecord/usdview.
# The earlier artifact was most likely the unrelated HeadCam non-determinism
# this scene hit during a concurrent build relink, not a shadow effect.
# ShadowAPI is left off rather than authored-and-ignored, since it cannot
# currently do anything in this render path.
scene.append('    def DistantLight "Key"')
scene.append('    {')
scene.append('        uniform token[] xformOpOrder = ["xformOp:transform"]')
scene.append('        matrix4d xformOp:transform = %s' % KEY_XFORM)
scene.append('        float inputs:intensity = %s' % fmt(3.8 * EXPOSURE_SCALE))
scene.append('        bool inputs:normalize = 1')
scene.append('        color3f inputs:color = (1, 0.98, 0.94)')
scene.append('    }')
scene.append('')
scene.append('    def DistantLight "Rim"')
scene.append('    {')
scene.append('        uniform token[] xformOpOrder = ["xformOp:transform"]')
scene.append('        matrix4d xformOp:transform = %s' % RIM_XFORM)
scene.append('        float inputs:intensity = %s' % fmt(1.2 * EXPOSURE_SCALE))
scene.append('        bool inputs:normalize = 1')
scene.append('        color3f inputs:color = (0.85, 0.9, 1)')
scene.append('    }')
scene.append('')
scene.append('    # A DomeLight with no inputs:texture:file compiles Storm\'s whole indirect-')
scene.append('    # lighting block out, so the fill needs a real HDRI, not just a colour --')
scene.append('    # this one ships with the OpenUSD prefix itself (Hdx\'s own test asset).')
scene.append('    # Intensity is well under Key\'s (~1:3) so it reads as soft ambient fill.')
scene.append('    def DomeLight "Sky"')
scene.append('    {')
scene.append('        asset inputs:texture:file = @%s@' % DOME_TEXTURE)
scene.append('        float inputs:intensity = %s' % fmt(1.0 * EXPOSURE_SCALE))
scene.append('        color3f inputs:color = (1, 1, 1)')
scene.append('    }')
scene.append('')

# Skin material -----------------------------------------------------------
# No Backdrop here: a 260-unit backdrop sphere in this same layer sits in the
# stage bbox usdview's FreeCamera derives near/far from (Usdviewq/
# freeCamera.py setClippingPlanes) -- far balloons to ~500+ and near gets
# pushed out with it, clipping the hair the moment you zoom in on a 17-unit
# head. head-hair-closeup-render.usda (below) sublayers this file and adds
# the backdrop back for offline renders only, leaving usdview's own bbox
# framing alone.
scene.append('    def Scope "Looks"')
scene.append('    {')
scene.append('        def Material "Skin"')
scene.append('        {')
scene.append('            token outputs:surface.connect = </World/Looks/Skin/Surface.outputs:surface>')
scene.append('')
scene.append('            def Shader "Surface"')
scene.append('            {')
scene.append('                uniform token info:id = "UsdPreviewSurface"')
scene.append('                color3f inputs:diffuseColor = (0.80, 0.62, 0.52)')
scene.append('                float inputs:roughness = 0.5')
scene.append('                token outputs:surface')
scene.append('            }')
scene.append('        }')
scene.append('    }')
scene.append('')

# Head mesh ------------------------------------------------------------
# usdGen:surface can target a Mesh or a face GeomSubset (ADR R15; see
# examples/subset-scatter-plane.usda), and a scalp-cap subset would restrict
# Scatter to it. It is not used here: a subset cuts the roots off at face
# boundaries, and a barbershop fade is graded. Scatter is bound to the whole
# Head mesh, and the elevation fade below (Expressions/scalpFade + Length's
# cullThreshold) does the job: it is a real per-strand fade, not a spatial density
# cut, so every follicle exists everywhere but only survives, visibly, on
# and above the fade band -- which is what a barbershop fade looks like
# anyway (follicle density is uniform; only visible length varies).
scene.append('    def Mesh "Head" (')
scene.append('        prepend apiSchemas = ["UsdGenRestAPI", "MaterialBindingAPI"]')
scene.append('    )')
scene.append('    {')
scene.append(write_mesh_body(points, normals, st, counts, indices))
scene.append('        color3f[] primvars:displayColor = [(0.80, 0.62, 0.52)]')
scene.append('        rel material:binding = </World/Looks/Skin>')
scene.append('    }')
scene.append('')

# Groom -----------------------------------------------------------------
scene.append('    def Xform "Groom"')
scene.append('    {')
scene.append('        def UsdGenDescription "Hair" (')
scene.append('            prepend apiSchemas = ["UsdGenLookAPI"]')
scene.append('        )')
scene.append('        {')
scene.append('            rel usdGen:surface = </World/Head>')
scene.append('            uniform token usdGen:curve:basis = "bspline"')
scene.append('            float usdGen:width:default = 0.005')
scene.append('')
scene.append('            # Dark brown root -> lighter brown tip (schema\'s own displayColor bake;')
scene.append('            # no material is bound here, matching the other examples -- the engine')
scene.append('            # binds a default hair material and this only supplies its colour).')
scene.append('            color3f usdGen:look:rootColor = (0.03, 0.02, 0.015)')
scene.append('            color3f usdGen:look:tipColor = (0.09, 0.06, 0.04)')
scene.append('')
scene.append('            def Scope "Ops"')
scene.append('            {')
scene.append('                # Terminal. Root width ~0.01cm tapering to 30% (~0.003cm) at the')
scene.append('                # tip, then multiplied by the same elevation fade as Length so')
scene.append('                # strands thin out before they are culled away entirely.')
scene.append('                def UsdGenWidth "width"')
scene.append('                {')
scene.append('                    float usdGen:width = 0.01 (')
scene.append('                        customData = { dictionary usdGen = { string evaluation = "primitive" } }')
scene.append('                    )')
scene.append('                    float usdGen:width.connect = </World/Groom/Hair/Expressions/scalpFade.outputs:result>')
scene.append('                    float2[] usdGen:width:knots = [(0, 1), (0.55, 0.7), (1, 0.3)]')
scene.append('                    uniform token usdGen:width:interpolation = "catmullRom"')
scene.append('                    bool usdGen:replace = true')
scene.append('                }')
scene.append('')
scene.append('                # The fade: scales the ~3-5cm top length down toward the scalp cap\'s')
scene.append('                # lower edge, passing through a 2-5mm "stubble" band on the way;')
scene.append('                # usdGen:cullThreshold then zero-lengthens (hides) whatever the fade')
scene.append('                # shrinks past 0.18cm, so only the very bottom reads as true bare skin.')
scene.append('                def UsdGenLength "fade"')
scene.append('                {')
scene.append('                    uniform token usdGen:length:mode = "scale"')
scene.append('                    float usdGen:length:value = 1.0')
scene.append('                    float usdGen:length:value.connect = </World/Groom/Hair/Expressions/scalpFade.outputs:result>')
scene.append('                    float2 usdGen:length:random = (0.8, 1.2)')
scene.append('                    uniform token usdGen:length:method = "scale"')
scene.append('                    uniform token usdGen:rebuild = "keepParam"')
scene.append('                    float usdGen:cullThreshold = 0.18')
scene.append('                }')
scene.append('')
scene.append('                # Chunky clumps (reference photo\'s "soft, chunky clumps ... visible')
scene.append('                # tips along the silhouette"): linearBlend converges each clump\'s CVs')
scene.append('                # toward its centre strand at the same parameter, so TIPS bunch into')
scene.append('                # visible points rather than staying parallel. levels=2 splits every')
scene.append('                # clump into smaller sub-clumps for the layered, non-uniform chunking')
scene.append('                # real hair shows; clump centres are auto-scattered (usdGen:clump:map')
scene.append('                # left unconnected) so no clump map needs baking. A small root pull')
scene.append('                # (profile:knots start > 0) keeps roots from splaying at the scalp so')
scene.append('                # gaps between clumps do not show it through.')
scene.append('                def UsdGenClump "clump"')
scene.append('                {')
scene.append('                    int usdGen:seed = 71')
scene.append('                    float usdGen:clump:amount = 0.8')
scene.append('                    float2[] usdGen:clump:profile:knots = [(0, 0.25), (1, 1)]')
scene.append('                    float usdGen:clump:density = 3.5')
scene.append('                    float usdGen:clump:size = 2.2')
scene.append('                    int usdGen:clump:levels = 2')
scene.append('                    float usdGen:clump:sizeReduction = 0.45')
scene.append('                    float usdGen:clump:tightnessReduction = 0.65')
scene.append('                    uniform token usdGen:clump:method = "linearBlend"')
scene.append('                    float usdGen:clump:stray:rate = 0.05')
scene.append('                    float usdGen:clump:stray:amount = 0.75')
scene.append('                    float usdGen:clump:stray:falloff = 0.55')
scene.append('                    float usdGen:preserveLength = 1')
scene.append('                }')
scene.append('')
scene.append('                # Gentle large-scale coil over the frizzed strands (runs right after')
scene.append('                # frizz): a small radius keeps the wave subtle on these 3-6cm strands')
scene.append('                # while the low frequency (turns per cm: ~one turn over a strand)')
scene.append('                # gives a loose combed wave rather than tight ringlets. Taper stays')
scene.append('                # at its schema default so roots stay planted.')
scene.append('                def UsdGenCurl "curl"')
scene.append('                {')
scene.append('                    float usdGen:radius = 0.08')
scene.append('                    float usdGen:frequency = 0.25')
scene.append('                }')
scene.append('')
scene.append('                # Subtle frizz -- correlated fBm displacement along the still-radial')
scene.append('                # root normal. Correlation 0.55 keeps neighbouring strands waving')
scene.append('                # together instead of fraying independently. Grow leans strands off')
scene.append('                # the pure radial normal (below) so the displacement shows as gentle')
scene.append('                # waviness instead of sliding each strand along itself. A Curl op')
scene.append('                # downstream adds the broader combed wave on top of this fine detail.')
scene.append('                def UsdGenNoise "frizz"')
scene.append('                {')
scene.append('                    int usdGen:seed = 5')
scene.append('                    float usdGen:noise:magnitude = 0.08')
scene.append('                    float2[] usdGen:noise:magnitude:knots = [(0, 0), (0.4, 0.3), (1, 1)]')
scene.append('                    float usdGen:noise:frequency = 1.8')
scene.append('                    float usdGen:noise:correlation = 0.55')
scene.append('                    int usdGen:noise:octaves = 2')
scene.append('                    float usdGen:preserveLength = 1')
scene.append('                }')
scene.append('')
scene.append('                # Strands out of the scalp, swept up/back toward the crown by rotating')
scene.append('                # the growth direction around the root binormal. usdGen:lift is driven')
scene.append('                # per elevation by Expressions/combLift (primitive evaluation): upright')
scene.append('                # ~11deg stubble at/below the fade band rising to the full swept 74deg')
scene.append('                # on top. The 74 literal stays as the $value fallback.')
scene.append('                def UsdGenGrow "grow"')
scene.append('                {')
scene.append('                    int usdGen:seed = 19')
scene.append('                    int usdGen:segments = 12')
scene.append('                    float usdGen:length = 3.8')
scene.append('                    float2 usdGen:lengthRandom = (0.75, 1.25)')
scene.append('                    uniform token usdGen:direction = "surfaceNormal"')
scene.append('                    float usdGen:lift = 74 (')
scene.append('                        customData = { dictionary usdGen = { string evaluation = "primitive" } }')
scene.append('                    )')
scene.append('                    float usdGen:lift.connect = </World/Groom/Hair/Expressions/combLift.outputs:result>')
scene.append('                }')
scene.append('')
scene.append('                # Runs first: roots over the whole Head (see the mesh comment above')
scene.append('                # on why this is not restricted to a scalp subset).')
scene.append('                def UsdGenScatter "scatter"')
scene.append('                {')
scene.append('                    int usdGen:seed = 41')
scene.append('                    float usdGen:density = %s' % fmt(DENSITY))
scene.append('                }')
scene.append('            }')
scene.append('')
scene.append('            def Scope "Expressions"')
scene.append('            {')
scene.append('                # Elevation fade: 1 across the dense crown, ramping to 0 as the')
scene.append('                # strand root nears the scalp cap\'s lower (bald) edge. $v is the')
scene.append('                # root\'s primvars:st.t, which this mesh authored as elevation (1 at')
scene.append('                # the crown, 0 at the far pole), so no vector math is needed.')
scene.append('                def UsdGenExpression "scalpFade"')
scene.append('                {')
scene.append('                    string usdGen:expr:source = """$e = curve($v, %s, 0, 4, %s, 0.5, 4, %s, 1, 4);'
             % (fmt(FADE_LOW), fmt((FADE_LOW + FADE_HIGH) * 0.5), fmt(FADE_HIGH)))
scene.append('$value * $e"""')
scene.append('                    custom float outputs:result')
scene.append('                }')
scene.append('')
scene.append('                # Per-elevation comb lift: upright stubble low, full sweep on top.')
scene.append('                # $v is the root\'s primvars:st.t (1 at the crown, 0 at the far pole);')
scene.append('                # $value is Grow\'s authored 74deg lift, scaled from 0.15 (~11deg)')
scene.append('                # at/below the fade band to 1 (74deg) above it.')
scene.append('                def UsdGenExpression "combLift"')
scene.append('                {')
scene.append('                    string usdGen:expr:source = """$e = curve($v, %s, 0.15, 4, %s, 0.575, 4, %s, 1, 4);'
             % (fmt(FADE_LOW), fmt((FADE_LOW + FADE_HIGH) * 0.5), fmt(FADE_HIGH)))
scene.append('$value * $e"""')
scene.append('                    custom float outputs:result')
scene.append('                }')
scene.append('            }')
scene.append('        }')
scene.append('    }')
scene.append('}')

out_text = "\n".join(scene)

path = os.path.join(EXAMPLES, "head-hair-closeup.usda")
with open(path, "w", newline="\n") as f:
    f.write(out_text)
    f.write("\n")

print("wrote", path)
print("head faces: %d" % len(counts))

# ---------------------------------------------------------------------------
# Render-only wrapper: sublayers the viewable scene above and adds back just
# the pale-grey backdrop sphere, for offline renders (bin/render_ue_parity.ps1
# points at this file, not head-hair-closeup.usda). Kept out of the main file
# so usdview's FreeCamera bbox-derived clipping isn't blown out by a 260-unit
# sphere sitting around a 17-unit head (Usdviewq/freeCamera.py
# setClippingPlanes: near/far come from the stage bbox, so a camera inside
# that sphere gets a far of ~500+ and a near pushed out far enough to clip
# the hair on zoom).
# ---------------------------------------------------------------------------
render_scene = []
render_scene.append('#usda 1.0')
render_scene.append('(')
render_scene.append('    defaultPrim = "World"')
render_scene.append('    subLayers = [')
render_scene.append('        @./head-hair-closeup.usda@')
render_scene.append('    ]')
render_scene.append(')')
render_scene.append('')
render_scene.append('over "World"')
render_scene.append('{')
render_scene.append('    def Mesh "Backdrop" (')
render_scene.append('        prepend apiSchemas = ["MaterialBindingAPI"]')
render_scene.append('    )')
render_scene.append('    {')
render_scene.append('        uniform token orientation = "leftHanded"')
render_scene.append(write_mesh_body(bd_points, bd_normals, bd_st, bd_counts, bd_indices))
render_scene.append('        rel material:binding = </World/Looks/Backdrop>')
render_scene.append('    }')
render_scene.append('')
render_scene.append('    over "Looks"')
render_scene.append('    {')
render_scene.append('        def Material "Backdrop"')
render_scene.append('        {')
render_scene.append('            token outputs:surface.connect = </World/Looks/Backdrop/Surface.outputs:surface>')
render_scene.append('')
render_scene.append('            def Shader "Surface"')
render_scene.append('            {')
render_scene.append('                uniform token info:id = "UsdPreviewSurface"')
render_scene.append('                color3f inputs:diffuseColor = (0, 0, 0)')
render_scene.append('                color3f inputs:emissiveColor = (0.82, 0.82, 0.82)')
render_scene.append('                float inputs:roughness = 1')
render_scene.append('                token outputs:surface')
render_scene.append('            }')
render_scene.append('        }')
render_scene.append('    }')
render_scene.append('}')

render_path = os.path.join(EXAMPLES, "head-hair-closeup-render.usda")
with open(render_path, "w", newline="\n") as f:
    f.write("\n".join(render_scene))
    f.write("\n")

print("wrote", render_path)
