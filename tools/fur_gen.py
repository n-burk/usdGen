#!/usr/bin/env python3
"""Generate PointInstancer-based fur USD scenes (pure text; no pxr needed).

Usage: fur_gen.py <count> <out.usda>
Scene: scalp sphere (r=1) + N instanced tapered BasisCurves oriented along
perturbed surface normals, per-strand length scale and brown color variation,
plus a 4:3 lookAt render camera and dome/distant lighting.
"""
import math
import random
import sys


def f(v):
    return "%.4f" % v


def ints(vals):
    return "[" + ", ".join(str(v) for v in vals) + "]"


def vecs(vals):
    return "[" + ", ".join("(%s, %s, %s)" % (f(x), f(y), f(z)) for x, y, z in vals) + "]"


def quats(vals):
    return "[" + ", ".join("(%s, %s, %s, %s)" % (f(w), f(x), f(y), f(z)) for w, x, y, z in vals) + "]"


def q_from_to(bx, by, bz):
    """Quaternion (w,x,y,z) rotating +Y onto unit vector b."""
    d = max(-1.0, min(1.0, by))
    if d > 0.999999:
        return (1.0, 0.0, 0.0, 0.0)
    if d < -0.999999:
        return (0.0, 0.0, 1.0, 0.0)  # 180 deg about Z
    # axis = Y x b with Y=(0,1,0) -> (bz, 0, -bx)
    cx, cz = bz, -bx
    s = math.sqrt(2.0 * (1.0 + d))
    w, x, z = 0.5 * s, cx / s, cz / s
    m = math.sqrt(w * w + x * x + z * z)
    return (w / m, x / m, 0.0, z / m)


def uv_sphere_body(rings=24, sectors=48, radius=1.0):
    verts = []
    for r in range(rings + 1):
        phi = math.pi * r / rings
        for s in range(sectors):
            th = 2.0 * math.pi * s / sectors
            verts.append((radius * math.sin(phi) * math.cos(th),
                          radius * math.cos(phi),
                          radius * math.sin(phi) * math.sin(th)))
    counts, idx = [], []
    for r in range(rings):
        for s in range(sectors):
            a = r * sectors + s
            b = r * sectors + (s + 1) % sectors
            counts.append(4)
            idx += [a, b, b + sectors, a + sectors]
    return ("        int[] faceVertexCounts = " + ints(counts) + "\n"
            "        int[] faceVertexIndices = " + ints(idx) + "\n"
            '        uniform token subdivisionScheme = "none"\n'
            "        point3f[] points = " + vecs(verts) + "\n")


def generate(count, out_path):
    if count <= 0:
        raise ValueError("strand count must be positive")
    rng = random.Random(14 + count)
    pts, orients, scales, cols = [], [], [], []
    golden = math.pi * (3.0 - math.sqrt(5.0))
    for i in range(count):
        y = 1.0 - 2.0 * (i + 0.5) / count
        r = math.sqrt(max(0.0, 1.0 - y * y))
        th = golden * i
        nx, ny, nz = math.cos(th) * r, y, math.sin(th) * r
        # Lay the coat along the sphere toward -Y, leaving a normal component
        # so strands clear the scalp. A radial-only groom looks like needles.
        tx, ty, tz = nx * ny, ny * ny - 1.0, nz * ny
        tm = max(1e-6, math.sqrt(tx * tx + ty * ty + tz * tz))
        j = 0.08
        dx = 0.65 * nx + 0.75 * tx / tm + rng.gauss(0, j)
        dy = 0.65 * ny + 0.75 * ty / tm + rng.gauss(0, j)
        dz = 0.65 * nz + 0.75 * tz / tm + rng.gauss(0, j)
        m = math.sqrt(dx * dx + dy * dy + dz * dz)
        pts.append((nx, ny, nz))
        orients.append(q_from_to(dx / m, dy / m, dz / m))
        length = rng.uniform(0.12, 0.30)
        w = rng.uniform(0.7, 1.4)
        scales.append((w, length, w))
        base = rng.uniform(0.11, 0.22)
        cols.append((base, base * 0.62, base * 0.40))

    out = []
    out.append('#usda 1.0\n(\n    defaultPrim = "World"\n    metersPerUnit = 1\n    upAxis = "Y"\n)\n\n')
    out.append('def Xform "World" (\n    kind = "Assembly"\n)\n{\n')
    out.append('    def Camera "RenderCam"\n    {\n'
               '        uniform token[] xformOpOrder = ["xformOp:translate"]\n'
               "        double3 xformOp:translate = (0, 0, 7.2)\n"
               "        float2 clippingRange = (0.1, 60)\n"
               "        float focalLength = 42\n"
               "        uniform token projection = \"perspective\"\n"
               "        float horizontalAperture = 24\n"
               "        float verticalAperture = 18\n"
               "    }\n\n")
    out.append('    def DistantLight "KeyLight"\n    {\n'
               "        float inputs:intensity = 5000\n"
               "        float3 xformOp:rotateXYZ = (35, 0, -30)\n"
               '        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]\n'
               "    }\n\n")
    out.append('    def DomeLight "Ambient"\n    {\n'
               "        float inputs:intensity = 0.3\n"
               "        color3f inputs:color = (0.85, 0.9, 1.0)\n"
               "    }\n\n")
    # The universal outputs:surface is the renderer-agnostic fallback
    # (docs/moonray-fur.md): Storm prefers its glslfx context, while
    # hdMoonray -- and any other Hydra delegate -- falls back to the
    # UsdPreviewSurface, whose diffuseColor reads the per-instance
    # displayColor through a primvar reader. A MoonRay-native hair
    # material (HairMaterial_v3) is deliberately NOT authored: UsdImaging
    # drops nodes whose info:id has no Sdr definition, and MoonRay's own
    # Sdr plugins are not on the translation path, so such a terminal
    # would resolve to no material at all.
    out.append('''    def Material "FurLook"
    {
        token outputs:glslfx:surface.connect = </World/FurLook/Surface.outputs:surface>
        token outputs:surface.connect = </World/FurLook/Preview.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdGenHairPreview"
            color3f inputs:tipColor = (0.48, 0.25, 0.09)
            float inputs:colorRamp = 1.4
            float inputs:selfOcclusion = 0.5
            float inputs:specular1Gain = 0.18
            token outputs:surface
        }
        def Shader "Preview"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/FurLook/DisplayColor.outputs:result>
            float inputs:roughness = 0.6
            token outputs:surface
        }
        def Shader "DisplayColor"
        {
            uniform token info:id = "UsdPrimvarReader_float3"
            token inputs:varname = "displayColor"
            color3f inputs:fallback = (0.16, 0.10, 0.06)
            color3f outputs:result
        }
    }
''')
    out.append('    def Mesh "Scalp"\n    {\n' + uv_sphere_body() +
               '        color3f[] primvars:displayColor = [(0.08, 0.035, 0.012)] (interpolation = "constant")\n' + "    }\n\n")
    out.append('    def PointInstancer "Fur"\n    {\n'
               "        rel prototypes = </World/Fur/Strand>\n"
               "        int[] protoIndices = " + ints([0] * count) + "\n"
               "        int64[] ids = " + ints(range(count)) + "\n"
               "        point3f[] positions = " + vecs(pts) + "\n"
               "        quath[] orientations = " + quats(orients) + "\n"
               "        vector3f[] scales = " + vecs(scales) + "\n"
               "        color3f[] primvars:displayColor = " + vecs(cols) +
               ' (interpolation = "varying")\n'
               "        float[] primvars:hairId = [" + ", ".join(f(rng.random()) for _ in range(count)) +
               '] (interpolation = "varying")\n')
    out.append('''        def BasisCurves "Strand" (prepend apiSchemas = ["MaterialBindingAPI"])
        {
            rel material:binding = </World/FurLook>
            uniform token type = "cubic"
            uniform token basis = "bspline"
            uniform token wrap = "pinned"
            int[] curveVertexCounts = [8]
            point3f[] points = [(0,0,0), (0,0.14,0), (0.01,0.28,0), (0.02,0.43,0), (0.03,0.57,0), (0.05,0.71,0), (0.07,0.86,0), (0.09,1,0)]
            float[] widths = [0.006,0.0057,0.0052,0.0046,0.0038,0.0028,0.0016,0.0003] (interpolation = "vertex")
            float[] primvars:hairT = [0,0.14,0.28,0.43,0.57,0.71,0.86,1] (interpolation = "vertex")
            float primvars:minScreenSpaceWidths = 1 (interpolation = "constant")
        }
    }
}
''')

    with open(out_path, "w") as fh:
        fh.write("".join(out))


if __name__ == "__main__":
    generate(int(sys.argv[1]), sys.argv[2])
