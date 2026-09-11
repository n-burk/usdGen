#!/usr/bin/env python3
"""Generate PointInstancer-based fur USD scenes (pure text; no pxr needed).

Usage: fur_gen.py <count> <out.usda>
Scene: scalp sphere (r=1) + N instanced tapered cones (strands) oriented along
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


def cone_strand_body(segs=6, radius=0.02, base_y=-0.05, tip_y=1.0):
    verts = [(radius * math.cos(2.0 * math.pi * i / segs), base_y,
              radius * math.sin(2.0 * math.pi * i / segs)) for i in range(segs)]
    verts.append((0.0, tip_y, 0.0))
    tip = len(verts) - 1
    counts, idx = [], []
    for i in range(segs):
        counts.append(3)
        idx += [tip, i, (i + 1) % segs]
    return ("        int[] faceVertexCounts = " + ints(counts) + "\n"
            "        int[] faceVertexIndices = " + ints(idx) + "\n"
            '        uniform token subdivisionScheme = "none"\n'
            "        point3f[] points = " + vecs(verts) + "\n")


def generate(count, out_path):
    rng = random.Random(14 + count)
    pts, orients, scales, cols = [], [], [], []
    golden = math.pi * (3.0 - math.sqrt(5.0))
    for i in range(count):
        y = 1.0 - 2.0 * (i + 0.5) / count
        r = math.sqrt(max(0.0, 1.0 - y * y))
        th = golden * i
        nx, ny, nz = math.cos(th) * r, y, math.sin(th) * r
        j = 0.18
        dx, dy, dz = nx + rng.gauss(0, j), ny + rng.gauss(0, j), nz + rng.gauss(0, j)
        m = math.sqrt(dx * dx + dy * dy + dz * dz)
        pts.append((nx, ny, nz))
        orients.append(q_from_to(dx / m, dy / m, dz / m))
        length = rng.uniform(0.12, 0.30)
        w = rng.uniform(0.7, 1.4)
        scales.append((w, length, w))
        base = rng.uniform(0.30, 0.66)
        cols.append((base, base * 0.62, base * 0.40))

    out = []
    out.append('#usda 1.0\n(\n    defaultPrim = "World"\n    metersPerUnit = 1\n    upAxis = "Y"\n)\n\n')
    out.append('def Xform "World" (\n    kind = "Assembly"\n)\n{\n')
    out.append('    def Camera "RenderCam"\n    {\n'
               '        uniform token[] xformOpOrder = ["xformOp:lookAt"]\n'
               "        lookAtf xformOp:lookAt = ((4.6, 1.8, 4.6), (0, 0, 0), (0, 1, 0))\n"
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
               "        float inputs:intensity = 800\n"
               "        color3f inputs:color = (0.85, 0.9, 1.0)\n"
               "    }\n\n")
    out.append('    def Scope "Prototypes"\n    {\n'
               '        def Mesh "Strand"\n        {\n' + cone_strand_body() + "        }\n    }\n\n")
    out.append('    def Mesh "Scalp"\n    {\n' + uv_sphere_body() + "    }\n\n")
    out.append('    def PointInstancer "Fur"\n    {\n'
               "        rel prototypes = </World/Prototypes/Strand>\n"
               '        uniform token visibility = "ids"\n'
               "        int[] ids = " + ints([0] * count) + "\n"
               "        point3f[] points = " + vecs(pts) + "\n"
               "        quatf[] orientations = " + quats(orients) + "\n"
               "        vector3f[] scales = " + vecs(scales) + "\n"
               "        float[] primvars:colors = [" + ", ".join(f(v) for c in cols for v in c) + "]" +
               ' (custom data type = "color3f", interpolation = "constant", role = "color")\n'
               "    }\n}\n")

    with open(out_path, "w") as fh:
        fh.write("".join(out))


if __name__ == "__main__":
    generate(int(sys.argv[1]), sys.argv[2])
