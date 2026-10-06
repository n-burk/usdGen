#!/usr/bin/env python3
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Build an ALab stoat grooming study from the locally consolidated asset.

The ALab source is separately licensed and intentionally stays in out/alab.
This script authors only a small, reproducible USD layer that references it.
The coat is generated at render time by usdGen's Scatter, GuideInterpolate,
Noise and Width operators. Its sparse guides are sampled from the actual
stoat skin and combed along the animal rather than hand-painted in image space.

Example:
    python tools/make_stoat_groom.py --asset out/alab/stoat01.usdc
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("USDGEN_BUILD", ROOT / "build"))
USD_PREFIX = Path(os.environ.get("USD", ROOT.parent / "usdRig" / "usd-install"))
USD_PYTHON = USD_PREFIX / "lib/site-packages"
if USD_PYTHON.exists():
    # Use the OpenUSD Python module matching this build; some environments
    # install an unrelated pxr shim in a normal site-packages .pth file.
    sys.path.insert(0, str(USD_PYTHON))
    os.environ["PATH"] = os.pathsep.join((str(BUILD), str(USD_PREFIX / "bin"),
                                             str(USD_PREFIX / "lib"), os.environ["PATH"]))
    plugin_dirs = [BUILD / "usd" / leaf / "resources" for leaf in (
        "usdGenSchema", "usdGenImaging", "usdGenShaders", "usdGenTools",
        "usdGenPomade", "usdGenPomadeTools")]
    plugin_dirs += [USD_PREFIX / "plugin/usd", USD_PREFIX / "lib/usd"]
    os.environ["PXR_PLUGINPATH_NAME"] = os.pathsep.join(str(p) for p in plugin_dirs if p.exists())

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade, Vt


DEFAULT_ASSET = ROOT / "out/alab/stoat01.usdc"
DEFAULT_OUTPUT = ROOT / "examples/alab/stoat-groom.usda"


def attr(prim, name, type_name, value, variability=Sdf.VariabilityVarying):
    result = prim.CreateAttribute(name, type_name, custom=True, variability=variability)
    result.Set(value)
    return result


def rel(prim, name, target):
    prim.CreateRelationship(name, custom=True).SetTargets([Sdf.Path(target)])


def find_mesh(stage, suffix):
    found = [p for p in stage.Traverse() if p.GetTypeName() == "Mesh" and p.GetName() == suffix]
    if len(found) != 1:
        raise RuntimeError(f"Expected one Mesh named {suffix!r}, found {[str(p.GetPath()) for p in found]}")
    return found[0]


def vec3(value):
    return Gf.Vec3f(float(value[0]), float(value[1]), float(value[2]))


def unit(value):
    norm = math.sqrt(sum(float(v) * float(v) for v in value))
    return tuple(float(v) / norm for v in value) if norm > 1e-10 else (0., 1., 0.)


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def dot(a, b):
    return sum(a[i]*b[i] for i in range(3))


def mesh_faces(mesh, classify):
    points = mesh.GetAttribute("points").Get()
    counts = mesh.GetAttribute("faceVertexCounts").Get()
    indices = mesh.GetAttribute("faceVertexIndices").Get()
    colors = mesh.GetAttribute("primvars:displayColor:indices").Get()
    groups = {}
    offset = 0
    for face_index, count in enumerate(counts):
        vertices = [tuple(points[indices[offset+j]]) for j in range(count)]
        offset += count
        center = tuple(sum(p[axis] for p in vertices) / count for axis in range(3))
        e1 = tuple(vertices[1][i] - vertices[0][i] for i in range(3))
        e2 = tuple(vertices[2][i] - vertices[0][i] for i in range(3))
        normal = unit(cross(e1, e2))
        area = 0.0
        for j in range(1, count-1):
            a = tuple(vertices[j][i] - vertices[0][i] for i in range(3))
            b = tuple(vertices[j+1][i] - vertices[0][i] for i in range(3))
            area += 0.5 * math.sqrt(sum(v*v for v in cross(a, b)))
        color_index = int(colors[face_index]) if colors is not None and len(colors) == len(counts) else 0
        group = classify(center, normal, color_index)
        if group:
            groups.setdefault(group, []).append((face_index, center, normal, area, vertices))
    return groups


def body_region(center, normal, color_index):
    # The supplied body mesh uses its second displayColor index for the inner
    # mouth. Those 760 faces remain bare. Eyes and nose are separate meshes.
    if color_index == 1:
        return None
    y, z = center[1], center[2]
    if y > 25:
        return "CreamFace" if normal[2] > 0.22 and z > 0.4 else "BrownHead"
    return "CreamBody" if normal[2] > 0.20 and z > -4.0 else "BrownBody"


def tail_region(center, normal, color_index):
    return "DarkTip" if center[2] < -29.4 else "Tail"


REGIONS = {
    "BrownBody": dict(density=160, guide_density=1.05, length=0.68, width=0.023,
                      root=(0.37, 0.20, 0.09), tip=(0.66, 0.42, 0.19), flow=(0, -1, -0.42)),
    "CreamBody": dict(density=165, guide_density=1.05, length=0.73, width=0.023,
                      root=(0.68, 0.54, 0.36), tip=(0.96, 0.85, 0.68), flow=(0, -1, -0.30)),
    "BrownHead": dict(density=175, guide_density=1.65, length=0.39, width=0.019,
                      root=(0.38, 0.21, 0.10), tip=(0.66, 0.43, 0.20), flow=(0, -1, -0.22)),
    "CreamFace": dict(density=180, guide_density=1.9, length=0.25, width=0.014,
                      root=(0.72, 0.59, 0.43), tip=(1.0, 0.91, 0.75), flow=(0, -1, 0.18)),
    "Tail": dict(density=185, guide_density=1.5, length=0.38, width=0.023,
                 root=(0.42, 0.27, 0.14), tip=(0.68, 0.48, 0.28), flow=(0, 0.05, -1)),
    "DarkTip": dict(density=190, guide_density=2, length=0.31, width=0.021,
                    root=(0.08, 0.06, 0.045), tip=(0.22, 0.17, 0.12), flow=(0, 0.05, -1)),
}


def make_subset(stage, mesh_path, name, faces):
    subset = UsdGeom.Subset.Define(stage, mesh_path.AppendChild("Groom" + name))
    subset.CreateElementTypeAttr().Set(UsdGeom.Tokens.face)
    subset.CreateIndicesAttr().Set(Vt.IntArray([face[0] for face in faces]))
    subset.CreateFamilyNameAttr().Set("groomRegion")
    if name == "DarkTip":
        dark = UsdShade.Material(stage.GetPrimAtPath("/World/TailTuftDarkLook"))
        UsdShade.MaterialBindingAPI.Apply(subset.GetPrim()).Bind(dark)
    return subset.GetPrim().GetPath()


def sample_guides(faces, config, seed, density_scale):
    rng = random.Random(seed)
    cumulative = []
    total_area = 0.0
    for face in faces:
        total_area += face[3]
        cumulative.append(total_area)
    count = max(6, int(total_area * config["guide_density"] * math.sqrt(density_scale)))
    points = []
    for _ in range(count):
        target = rng.random() * total_area
        lo, hi = 0, len(cumulative)
        while lo < hi:
            mid = (lo+hi)//2
            if cumulative[mid] < target: lo = mid+1
            else: hi = mid
        _, center, normal, _, vertices = faces[min(lo, len(faces)-1)]
        # Triangulate the quad by its first, second and third vertices; the
        # root stays on the ALab polygon. Small regular jitter avoids a grid.
        tri = vertices[:3]
        u, v = rng.random(), rng.random()
        if u + v > 1: u, v = 1-u, 1-v
        root = tuple(tri[0][j] + u*(tri[1][j]-tri[0][j]) + v*(tri[2][j]-tri[0][j]) for j in range(3))
        flow = config["flow"]
        tangent = unit(tuple(flow[j] - dot(flow, normal)*normal[j] for j in range(3)))
        if dot(tangent, tangent) < 0.1:
            tangent = unit(cross(normal, (1, 0, 0)))
        length = config["length"] * rng.uniform(0.87, 1.14)
        for cv in range(8):
            t = cv / 7
            # Immediate outward root clearance, then a combed tangential arc.
            outward = length * (0.12*t + 0.13*math.sin(math.pi*t))
            sweep = length * (0.80*t*t + 0.08*t)
            points.append(Gf.Vec3f(*(root[j] + normal[j]*outward + tangent[j]*sweep for j in range(3))))
    return count, points


def make_guides(stage, name, faces, config, seed, density_scale):
    count, points = sample_guides(faces, config, seed, density_scale)
    guide = UsdGeom.BasisCurves.Define(stage, f"/World/Guides/{name}")
    guide.CreateTypeAttr().Set(UsdGeom.Tokens.cubic)
    guide.CreateBasisAttr().Set(UsdGeom.Tokens.bspline)
    guide.CreateWrapAttr().Set(UsdGeom.Tokens.pinned)
    guide.CreateCurveVertexCountsAttr().Set(Vt.IntArray([8]*count))
    guide.CreatePointsAttr().Set(Vt.Vec3fArray(points))
    guide.CreatePurposeAttr().Set(UsdGeom.Tokens.guide)
    attr(guide.GetPrim(), "primvars:displayColor", Sdf.ValueTypeNames.Color3fArray,
         Vt.Vec3fArray([Gf.Vec3f(0.16, 0.78, 0.77)]))
    guide.GetPrim().ApplyAPI("UsdGenCurveAPI")
    return guide.GetPrim().GetPath(), count


def make_groom(stage, name, surface_path, guide_path, config, density_scale, seed):
    path = f"/World/Groom/{name}"
    desc = stage.DefinePrim(path, "UsdGenDescription")
    desc.ApplyAPI("UsdGenLookAPI")
    rel(desc, "usdGen:surface", surface_path)
    attr(desc, "usdGen:curve:basis", Sdf.ValueTypeNames.Token, "bspline", Sdf.VariabilityUniform)
    attr(desc, "usdGen:width:default", Sdf.ValueTypeNames.Float, config["width"])
    attr(desc, "usdGen:look:rootColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(*config["root"]))
    attr(desc, "usdGen:look:tipColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(*config["tip"]))
    # Storm's physical hair shader can black out a deep coat at its default
    # self-shadow depth. A restrained multiple-scatter fill makes the strand
    # colour legible in the real-time reference, while retaining a true fur
    # optical-depth contribution and the baked ALab surface underneath.
    look_path = f"/World/FurLooks/{name}"
    material = stage.DefinePrim(look_path, "Material")
    shader = stage.DefinePrim(look_path + "/Surface", "Shader")
    attr(shader, "info:id", Sdf.ValueTypeNames.Token, "UsdGenHairStrands", Sdf.VariabilityUniform)
    attr(shader, "inputs:tipColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(*config["tip"]))
    attr(shader, "inputs:selfShadow", Sdf.ValueTypeNames.Float, 0.12)
    attr(shader, "inputs:scatter", Sdf.ValueTypeNames.Float, 0.55)
    attr(shader, "inputs:roughness", Sdf.ValueTypeNames.Float, 0.56)
    attr(shader, "inputs:specular", Sdf.ValueTypeNames.Float, 0.34)
    shader.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True)
    material.CreateAttribute("outputs:glslfx:surface", Sdf.ValueTypeNames.Token, custom=True).AddConnection(
        Sdf.Path(look_path + "/Surface.outputs:surface"))
    UsdShade.MaterialBindingAPI.Apply(desc).Bind(UsdShade.Material(material))
    stage.DefinePrim(path + "/Ops", "Scope")
    # Reverse lexical/namespace order is the operator evaluation order used
    # throughout the existing CPU examples: Scatter -> Interpolate -> Noise -> Width.
    op = stage.DefinePrim(path + "/Ops/width", "UsdGenWidth")
    attr(op, "usdGen:width", Sdf.ValueTypeNames.Float, config["width"])
    attr(op, "usdGen:width:knots", Sdf.ValueTypeNames.Float2Array,
         Vt.Vec2fArray([Gf.Vec2f(0, 1), Gf.Vec2f(0.62, 0.72), Gf.Vec2f(1, 0.08)]))
    attr(op, "usdGen:width:interpolation", Sdf.ValueTypeNames.Token, "catmullRom", Sdf.VariabilityUniform)
    attr(op, "usdGen:replace", Sdf.ValueTypeNames.Bool, True)
    op = stage.DefinePrim(path + "/Ops/noise", "UsdGenNoise")
    attr(op, "usdGen:seed", Sdf.ValueTypeNames.Int, seed+30)
    attr(op, "usdGen:noise:magnitude", Sdf.ValueTypeNames.Float, config["length"]*0.035)
    attr(op, "usdGen:noise:magnitude:knots", Sdf.ValueTypeNames.Float2Array,
         Vt.Vec2fArray([Gf.Vec2f(0, 0), Gf.Vec2f(0.55, 0.35), Gf.Vec2f(1, 1)]))
    attr(op, "usdGen:noise:frequency", Sdf.ValueTypeNames.Float, 2.5)
    attr(op, "usdGen:noise:correlation", Sdf.ValueTypeNames.Float, 0.7)
    attr(op, "usdGen:noise:octaves", Sdf.ValueTypeNames.Int, 2)
    attr(op, "usdGen:preserveLength", Sdf.ValueTypeNames.Float, 1)
    op = stage.DefinePrim(path + "/Ops/interpolate", "UsdGenGuideInterpolate")
    attr(op, "usdGen:seed", Sdf.ValueTypeNames.Int, seed+10)
    rel(op, "usdGen:guides", guide_path)
    attr(op, "usdGen:cvCount", Sdf.ValueTypeNames.Int, 8)
    attr(op, "usdGen:maxGuides", Sdf.ValueTypeNames.Int, 3)
    attr(op, "usdGen:influenceRadius", Sdf.ValueTypeNames.Float, 2.8 if "Body" in name else 1.5)
    attr(op, "usdGen:influenceDecay", Sdf.ValueTypeNames.Float, 1.4)
    attr(op, "usdGen:blendMethod", Sdf.ValueTypeNames.Token, "extrudeAndBlend", Sdf.VariabilityUniform)
    attr(op, "usdGen:blendInSkinSpace", Sdf.ValueTypeNames.Float, 1)
    attr(op, "usdGen:randomizeGuide", Sdf.ValueTypeNames.Float, 0.11)
    op = stage.DefinePrim(path + "/Ops/scatter", "UsdGenScatter")
    attr(op, "usdGen:seed", Sdf.ValueTypeNames.Int, seed)
    attr(op, "usdGen:density", Sdf.ValueTypeNames.Float, config["density"]*density_scale)


def camera(stage, name, eye, target, focal_length):
    cam = UsdGeom.Camera.Define(stage, "/World/" + name)
    matrix = Gf.Matrix4d().SetLookAt(Gf.Vec3d(*eye), Gf.Vec3d(*target), Gf.Vec3d(0, 1, 0)).GetInverse()
    cam.AddTransformOp().Set(matrix)
    cam.CreateFocalLengthAttr().Set(focal_length)
    cam.CreateHorizontalApertureAttr().Set(36.)
    cam.CreateVerticalApertureAttr().Set(28.8)
    cam.CreateClippingRangeAttr().Set(Gf.Vec2f(0.1, 500))


def light(stage, name, rotation, intensity, color):
    prim = stage.DefinePrim("/World/Lighting/" + name, "DistantLight")
    attr(prim, "inputs:intensity", Sdf.ValueTypeNames.Float, intensity)
    attr(prim, "inputs:normalize", Sdf.ValueTypeNames.Bool, True)
    attr(prim, "inputs:color", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(*color))
    attr(prim, "xformOp:rotateXYZ", Sdf.ValueTypeNames.Float3, Gf.Vec3f(*rotation))
    attr(prim, "xformOpOrder", Sdf.ValueTypeNames.TokenArray, Vt.TokenArray(["xformOp:rotateXYZ"]), Sdf.VariabilityUniform)


def studio_backdrop(stage):
    backdrop = UsdGeom.Mesh.Define(stage, "/World/Backdrop")
    backdrop.CreatePointsAttr().Set(Vt.Vec3fArray([
        Gf.Vec3f(-1000, -1000, -115), Gf.Vec3f(1000, -1000, -115),
        Gf.Vec3f(1000, 1000, -115), Gf.Vec3f(-1000, 1000, -115)]))
    backdrop.CreateFaceVertexCountsAttr().Set(Vt.IntArray([4]))
    backdrop.CreateFaceVertexIndicesAttr().Set(Vt.IntArray([0, 1, 2, 3]))
    backdrop.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    backdrop.CreateDoubleSidedAttr().Set(True)
    material = stage.DefinePrim("/World/BackdropLook", "Material")
    shader = stage.DefinePrim("/World/BackdropLook/Surface", "Shader")
    attr(shader, "info:id", Sdf.ValueTypeNames.Token, "UsdPreviewSurface", Sdf.VariabilityUniform)
    attr(shader, "inputs:diffuseColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(0, 0, 0))
    attr(shader, "inputs:emissiveColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(0.12, 0.14, 0.16))
    attr(shader, "inputs:roughness", Sdf.ValueTypeNames.Float, 1.)
    shader.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True)
    material.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True).AddConnection(
        Sdf.Path("/World/BackdropLook/Surface.outputs:surface"))
    UsdShade.MaterialBindingAPI.Apply(backdrop.GetPrim()).Bind(UsdShade.Material(material))


def make_tail_proxy(stage):
    """Smooth scalp envelope fitted to ALab's tuft bounds for this groom study.

    ALab's own tailHair mesh remains in the consolidated character; only its
    visibility is muted in the groom layer. Its long rigid spikes make a poor
    emitter for short dense fibres, so this volume gives the fur a soft outline.
    """
    rings = [
        (-22.5, 1.10, 0.55), (-23.8, 1.15, 0.82), (-25.0, 1.25, 1.20),
        (-26.3, 1.35, 1.60), (-27.7, 1.50, 1.85), (-29.0, 1.65, 1.72),
        (-30.3, 1.75, 1.35), (-31.6, 1.90, 0.78), (-32.8, 2.00, 0.08),
    ]
    sectors = 32
    points = []
    for z, cy, radius in rings:
        for sector in range(sectors):
            angle = 2 * math.pi * sector / sectors
            points.append(Gf.Vec3f(0.2 + radius * math.cos(angle),
                                   cy + 0.83 * radius * math.sin(angle), z))
    counts, indices = [], []
    for ring in range(len(rings)-1):
        for sector in range(sectors):
            a = ring*sectors + sector
            b = ring*sectors + (sector+1) % sectors
            indices.extend((a, a+sectors, b+sectors, b))
            counts.append(4)
    proxy = UsdGeom.Mesh.Define(stage, "/World/TailTuftScalp")
    proxy.CreatePointsAttr().Set(Vt.Vec3fArray(points))
    proxy.CreateFaceVertexCountsAttr().Set(Vt.IntArray(counts))
    proxy.CreateFaceVertexIndicesAttr().Set(Vt.IntArray(indices))
    proxy.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.catmullClark)
    material = stage.DefinePrim("/World/TailTuftScalpLook", "Material")
    shader = stage.DefinePrim("/World/TailTuftScalpLook/Surface", "Shader")
    attr(shader, "info:id", Sdf.ValueTypeNames.Token, "UsdPreviewSurface", Sdf.VariabilityUniform)
    attr(shader, "inputs:diffuseColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(0.36, 0.23, 0.12))
    attr(shader, "inputs:roughness", Sdf.ValueTypeNames.Float, 0.92)
    shader.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True)
    material.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True).AddConnection(
        Sdf.Path("/World/TailTuftScalpLook/Surface.outputs:surface"))
    UsdShade.MaterialBindingAPI.Apply(proxy.GetPrim()).Bind(UsdShade.Material(material))
    dark_material = stage.DefinePrim("/World/TailTuftDarkLook", "Material")
    dark_shader = stage.DefinePrim("/World/TailTuftDarkLook/Surface", "Shader")
    attr(dark_shader, "info:id", Sdf.ValueTypeNames.Token, "UsdPreviewSurface", Sdf.VariabilityUniform)
    attr(dark_shader, "inputs:diffuseColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(0.10, 0.075, 0.055))
    attr(dark_shader, "inputs:roughness", Sdf.ValueTypeNames.Float, 0.94)
    dark_shader.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True)
    dark_material.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True).AddConnection(
        Sdf.Path("/World/TailTuftDarkLook/Surface.outputs:surface"))
    return proxy.GetPrim()


def eye_look(stage, character):
    material = stage.DefinePrim("/World/EyeLook", "Material")
    shader = stage.DefinePrim("/World/EyeLook/Surface", "Shader")
    attr(shader, "info:id", Sdf.ValueTypeNames.Token, "UsdPreviewSurface", Sdf.VariabilityUniform)
    attr(shader, "inputs:diffuseColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(0.012, 0.015, 0.02))
    attr(shader, "inputs:specularColor", Sdf.ValueTypeNames.Color3f, Gf.Vec3f(0.95, 0.95, 0.95))
    attr(shader, "inputs:metallic", Sdf.ValueTypeNames.Float, 0.25)
    attr(shader, "inputs:roughness", Sdf.ValueTypeNames.Float, 0.08)
    shader.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True)
    material.CreateAttribute("outputs:surface", Sdf.ValueTypeNames.Token, custom=True).AddConnection(
        Sdf.Path("/World/EyeLook/Surface.outputs:surface"))
    for prim in character.GetPrim().GetStage().Traverse():
        if not prim.GetPath().HasPrefix(character.GetPath()) or prim.GetTypeName() != "Mesh":
            continue
        if "eyeInnerPart" in prim.GetName():
            UsdShade.MaterialBindingAPI.Apply(prim).Bind(UsdShade.Material(material))
        elif "eyeOuterShell" in prim.GetName():
            UsdGeom.Imageable(prim).GetVisibilityAttr().Set(UsdGeom.Tokens.invisible)


def make_wrapper(source, output, kind):
    wrapper = Usd.Stage.CreateNew(str(output))
    layer = wrapper.GetRootLayer()
    layer.subLayerPaths.append("./" + source.name)
    layer.comment = "Copyright (c) 2026 Nick Burkard; SPDX-License-Identifier: MIT"
    if kind == "bare":
        wrapper.OverridePrim("/World/Groom").SetActive(False)
        UsdGeom.Imageable(find_mesh(wrapper, "tailHair_M_geo")).GetVisibilityAttr().Set(
            UsdGeom.Tokens.inherited)
        UsdGeom.Imageable(wrapper.GetPrimAtPath("/World/TailTuftScalp")).GetVisibilityAttr().Set(
            UsdGeom.Tokens.invisible)
    elif kind == "guides":
        wrapper.OverridePrim("/World/Groom").SetActive(False)
        for name in REGIONS:
            prim = wrapper.OverridePrim("/World/Guides/" + name)
            attr(prim, "purpose", Sdf.ValueTypeNames.Token, "default", Sdf.VariabilityUniform)
            attr(prim, "widths", Sdf.ValueTypeNames.FloatArray, Vt.FloatArray([0.075]))
    else:
        raise ValueError(kind)
    layer.Save()


def make_scene(asset, output, density_scale):
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Usd.Stage.CreateNew(str(output))
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    character = UsdGeom.Xform.Define(stage, "/World/Character")
    character.GetPrim().GetReferences().AddReference(os.path.relpath(asset, output.parent).replace("\\", "/"))
    # The ALab package includes outfit and backpack components. They remain in
    # its consolidated USD, but the coat study exposes the underlying body.
    for child in character.GetPrim().GetChildren():
        if child.GetName().startswith(("outfit", "backpack")):
            UsdGeom.Imageable(child).GetVisibilityAttr().Set(UsdGeom.Tokens.invisible)
    stage.DefinePrim("/World/Lighting", "Scope")
    stage.DefinePrim("/World/Guides", "Scope")
    stage.DefinePrim("/World/Groom", "Xform")
    stage.DefinePrim("/World/FurLooks", "Scope")
    body = find_mesh(stage, "body_M_geo")
    source_tail = find_mesh(stage, "tailHair_M_geo")
    UsdGeom.Imageable(source_tail).GetVisibilityAttr().Set(UsdGeom.Tokens.invisible)
    tail = make_tail_proxy(stage)
    camera(stage, "HeroCam", (78, 45, 72), (0, 16, -10), 60)
    camera(stage, "DetailCam", (21, 35, 41), (0, 27, 0.5), 76)
    camera(stage, "ProfileCam", (83, 31, 15), (0, 14, -9), 52)
    # Four broad directional contributions keep the Storm strand shader's
    # center lit. A cool rim traces the silhouette without crushing the face.
    light(stage, "Key", (-30, -35, -12), 1.7, (1.0, 0.91, 0.79))
    light(stage, "FrontalFill", (-15, 0, 0), 1.25, (0.94, 0.96, 1.0))
    light(stage, "SideFill", (-25, 58, 10), 0.55, (0.91, 0.88, 0.83))
    light(stage, "Rim", (-42, 150, 0), 0.8, (0.77, 0.86, 1.0))
    dome = stage.DefinePrim("/World/Lighting/Sky", "DomeLight")
    hdr = ROOT / "examples/maps/StinsonBeach.hdr"
    attr(dome, "inputs:texture:file", Sdf.ValueTypeNames.Asset,
         Sdf.AssetPath(os.path.relpath(hdr, output.parent).replace("\\", "/")))
    attr(dome, "inputs:intensity", Sdf.ValueTypeNames.Float, 0.32)
    studio_backdrop(stage)
    eye_look(stage, character)
    regions = {}
    for mesh, classify in [(body, body_region), (tail, tail_region)]:
        groups = mesh_faces(mesh, classify)
        for name, faces in groups.items():
            config = REGIONS[name]
            surface = make_subset(stage, mesh.GetPath(), name, faces)
            guides, guide_count = make_guides(stage, name, faces, config, len(regions)*101+11, density_scale)
            make_groom(stage, name, surface, guides, config, density_scale, len(regions)*101+11)
            area = sum(face[3] for face in faces)
            regions[name] = (len(faces), guide_count, area)
    stage.GetRootLayer().comment = "Copyright (c) 2026 Nick Burkard; SPDX-License-Identifier: MIT"
    stage.GetRootLayer().Save()
    for kind in ("bare", "guides"):
        stem = output.stem.replace("-groom", "-" + kind) if "-groom" in output.stem else output.stem + "-" + kind
        make_wrapper(output, output.with_name(stem + output.suffix), kind)
    print(f"Wrote {output}")
    print(f"Source {asset}")
    for name, (faces, guides, area) in regions.items():
        origin = "ALab faces" if name not in ("Tail", "DarkTip") else "proxy faces"
        print(f"  {name}: {faces} {origin}, {guides} guide curves, {area:.1f} surface units squared")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset", type=Path, default=DEFAULT_ASSET)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--density-scale", type=float, default=1.)
    args = parser.parse_args()
    if not args.asset.is_file():
        parser.error(f"Consolidated ALab USD not found: {args.asset}; run tools/prepare_alab.py first")
    if not 0 < args.density_scale <= 2:
        parser.error("--density-scale must be in (0, 2]")
    make_scene(args.asset.resolve(), args.output.resolve(), args.density_scale)


if __name__ == "__main__":
    main()
