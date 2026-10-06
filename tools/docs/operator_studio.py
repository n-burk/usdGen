#!/usr/bin/env python3
"""Add a subdued studio to baked guide-example stages for MoonRay capture."""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade, Vt


def style(input_stage: str, output_stage: str) -> None:
    stage = Usd.Stage.Open(input_stage)
    if not stage:
        raise RuntimeError(f"Cannot open {input_stage}")
    key = UsdLux.DistantLight.Get(stage, "/World/Key")
    sky = UsdLux.DomeLight.Get(stage, "/World/Sky")
    if not key or not sky:
        raise RuntimeError("Guide example needs /World/Key and /World/Sky")
    key.GetIntensityAttr().Set(1.25)
    sky.GetIntensityAttr().Set(0.35)

    ground = UsdGeom.Mesh.Define(stage, "/World/StudioGround")
    ground.CreatePointsAttr(Vt.Vec3fArray([
        Gf.Vec3f(-7.0, -0.10, -7.0),
        Gf.Vec3f(7.0, -0.10, -7.0),
        Gf.Vec3f(7.0, -0.10, 7.0),
        Gf.Vec3f(-7.0, -0.10, 7.0),
    ]))
    ground.CreateFaceVertexCountsAttr(Vt.IntArray([4]))
    ground.CreateFaceVertexIndicesAttr(Vt.IntArray([0, 1, 2, 3]))
    ground.CreateDoubleSidedAttr(True)
    ground.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)

    material = UsdShade.Material.Define(stage, "/World/StudioGroundMaterial")
    surface = UsdShade.Shader.Define(stage, "/World/StudioGroundMaterial/Surface")
    surface.CreateIdAttr("UsdPreviewSurface")
    surface.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.095, 0.105, 0.115))
    surface.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.9)
    material.CreateSurfaceOutput().ConnectToSource(surface.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(ground.GetPrim()).Bind(material)
    if not stage.Export(output_stage):
        raise RuntimeError(f"Cannot export {output_stage}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_stage")
    parser.add_argument("output_stage")
    args = parser.parse_args()
    style(args.input_stage, args.output_stage)
