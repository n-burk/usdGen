#!/usr/bin/env python3
"""Add a MoonRay-only studio layer over a baked character USD stage."""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
from pathlib import Path

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade


def material(stage, path, color):
    mat = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path.AppendChild("Surface"))
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*color))
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.94)
    mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
    return mat


def quad(stage, path, points, mat):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateSubdivisionSchemeAttr("none")
    mesh.CreateDoubleSidedAttr(True)
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--key-intensity", type=float, default=1.05)
    parser.add_argument("--sky-intensity", type=float, default=0.22)
    parser.add_argument("--camera", default="HeroCam")
    parser.add_argument("--focal-scale", type=float, default=1.17)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Usd.Stage.CreateNew(str(output))
    stage.GetRootLayer().subLayerPaths.append(
        str(Path(__import__("os").path.relpath(source, output.parent)).as_posix()))
    source_stage = Usd.Stage.Open(str(source))
    character = source_stage.GetPrimAtPath("/World/Character")
    bound = UsdGeom.BBoxCache(Usd.TimeCode.Default(), [UsdGeom.Tokens.default_])
    box = bound.ComputeWorldBound(character).ComputeAlignedRange()
    minimum, maximum = box.GetMin(), box.GetMax()
    center = (minimum + maximum) * 0.5
    print("character bounds:", minimum, maximum)

    intensities = {"Key": args.key_intensity, "FrontalFill": 0.0,
                   "SideFill": 0.0, "Rim": 0.0, "Sky": args.sky_intensity}
    for name, value in intensities.items():
        path = Sdf.Path(f"/World/Lighting/{name}")
        stage.OverridePrim(path).GetAttribute("inputs:intensity").Set(value)
    stage.OverridePrim(Sdf.Path("/World/Lighting/Key")).GetAttribute(
        "inputs:angle").Set(8.0)
    # The original USD background is intentionally omitted from the studio.
    stage.OverridePrim(Sdf.Path("/World/Backdrop")).SetActive(False)
    cam_path = Sdf.Path(f"/World/{args.camera}")
    cam = UsdGeom.Camera.Get(source_stage, cam_path)
    matrix = UsdGeom.XformCache().GetLocalToWorldTransform(cam.GetPrim())
    right = Gf.Vec3d(*matrix.GetRow3(0)).GetNormalized()
    back = Gf.Vec3d(*matrix.GetRow3(2)).GetNormalized()
    background_center = center - back * 65.0
    size = 400.0
    up = Gf.Vec3d(*matrix.GetRow3(1)).GetNormalized()
    corners = [background_center - right * size - up * size,
               background_center + right * size - up * size,
               background_center + right * size + up * size,
               background_center - right * size + up * size]
    backdrop = material(stage, Sdf.Path("/World/StudioBackdropLook"),
                        (0.061, 0.067, 0.076))
    quad(stage, Sdf.Path("/World/StudioBackdrop"), corners, backdrop)
    y = minimum[1] - 0.15
    x, z = center[0], center[2]
    floor = material(stage, Sdf.Path("/World/StudioFloorLook"),
                     (0.061, 0.067, 0.076))
    quad(stage, Sdf.Path("/World/StudioFloor"),
         [Gf.Vec3d(x - 120, y, z - 120), Gf.Vec3d(x - 120, y, z + 120),
          Gf.Vec3d(x + 120, y, z + 120), Gf.Vec3d(x + 120, y, z - 120)], floor)
    focal = cam.GetFocalLengthAttr().Get()
    stage.OverridePrim(cam_path).GetAttribute("focalLength").Set(
        float(focal) * args.focal_scale)
    stage.GetRootLayer().Save()
    print(output)


if __name__ == "__main__":
    main()
