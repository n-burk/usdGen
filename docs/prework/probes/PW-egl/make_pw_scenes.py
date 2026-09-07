#!/usr/bin/env python3
"""PW-2/PW-3 scene generator (scratch variant of plan/prototypes/storm-hair-look/make_scene.py).

Usage:
  make_pw_scenes.py trivial out.usdc
  make_pw_scenes.py <ncurves> out.usdc <A|B>

- variant A: NO hairTangent primvar; material -> usdGenHairPreviewIndata.glslfx
- variant B: hairTangent vertex primvar; material -> usdGenHairPreviewPrimvar.glslfx
Geometry: same strand field as the prototype (seed 7), scaled to ncurves x 8 CV.
"""
import math, random, sys
from pxr import Usd, UsdGeom, UsdShade, UsdLux, Sdf, Gf, Vt

HERE = "/home/burkard/work/usdGen/docs/prework/probes/PW-egl"

def build_hair(ncurves, ncv):
    random.seed(7)
    LEN = 0.55
    pts, widths, hairT, counts, hairId, dispColor, tangents, st = [], [], [], [], [], [], [], []
    for c in range(ncurves):
        while True:
            u = random.uniform(-1, 1); v = random.uniform(-1, 1); w = random.uniform(0.1, 1.0)
            n = Gf.Vec3f(u, w, v)
            if n.GetLength() > 1e-3:
                break
        n = n.GetNormalized()
        root = n * 1.0
        for i in range(ncv):
            t = i / (ncv - 1.0)
            droop = Gf.Vec3f(0.0, -0.9 * t * t, 0.0)
            jitter = Gf.Vec3f(random.uniform(-1, 1), random.uniform(-1, 1),
                              random.uniform(-1, 1)) * 0.02 * t
            p = root + (n * LEN * t) + droop * LEN + jitter
            pts.append(Gf.Vec3f(p))
            widths.append(0.0032 * (1.0 - 0.85 * t))
            hairT.append(t)
            tan = (n * LEN + Gf.Vec3f(0.0, -1.8 * t * LEN, 0.0))
            tangents.append(Gf.Vec3f(tan.GetNormalized()))
        counts.append(ncv)
        hairId.append(random.random())
        shade = 0.75 + 0.5 * random.random()
        dispColor.append(Gf.Vec3f(shade, shade, shade))
        st.append(Gf.Vec2f(0.5 + 0.5 * n[0], 0.5 + 0.5 * n[2]))
    return pts, widths, hairT, counts, hairId, dispColor, tangents, st

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[2]
    if sys.argv[1] == "trivial":
        stage = Usd.Stage.CreateNew(out)
        UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
        UsdGeom.SetStageMetersPerUnit(stage, 1.0)
        world = UsdGeom.Xform.Define(stage, "/World")
        stage.SetDefaultPrim(world.GetPrim())
        sphere = UsdGeom.Sphere.Define(stage, "/World/Scalp")
        sphere.CreateRadiusAttr(0.98)
        sphere.CreateDisplayColorAttr([Gf.Vec3f(0.18, 0.10, 0.07)])
        cam = UsdGeom.Camera.Define(stage, "/World/Cam")
        UsdGeom.Xformable(cam).AddTranslateOp().Set(Gf.Vec3d(0, 0.35, 5.2))
        cam.CreateFocalLengthAttr(50.0)
        cam.CreateHorizontalApertureAttr(24.0)
        cam.CreateVerticalApertureAttr(24.0)
        cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))
        key = UsdLux.DistantLight.Define(stage, "/World/Lights/Key")
        key.CreateIntensityAttr(3.0)
        stage.GetRootLayer().Save()
        print("wrote", out, "(trivial: sphere + camera + key light)")
        return

    ncurves = int(sys.argv[1])
    variant = sys.argv[3]
    glslfx = "./usdGenHairPreviewIndata.glslfx" if variant == "A" else "./usdGenHairPreviewPrimvar.glslfx"
    with_tangent = (variant == "B")

    stage = Usd.Stage.CreateNew(out)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())

    sphere = UsdGeom.Sphere.Define(stage, "/World/Scalp")
    sphere.CreateRadiusAttr(0.98)
    sphere.CreateDisplayColorAttr([Gf.Vec3f(0.18, 0.10, 0.07)])

    (pts, widths, hairT, counts, hairId, dispColor,
     tangents, st) = build_hair(ncurves, 8)

    curves = UsdGeom.BasisCurves.Define(stage, "/World/Hair")
    curves.CreateTypeAttr(UsdGeom.Tokens.cubic)
    curves.CreateBasisAttr(UsdGeom.Tokens.bspline)
    curves.CreateWrapAttr(UsdGeom.Tokens.pinned)
    curves.CreateCurveVertexCountsAttr(Vt.IntArray(counts))
    curves.CreatePointsAttr(Vt.Vec3fArray(pts))
    curves.CreateWidthsAttr(Vt.FloatArray(widths))
    curves.SetWidthsInterpolation(UsdGeom.Tokens.vertex)
    curves.CreateExtentAttr(Vt.Vec3fArray([Gf.Vec3f(-1.7, -1.7, -1.7), Gf.Vec3f(1.7, 1.7, 1.7)]))

    api = UsdGeom.PrimvarsAPI(curves.GetPrim())
    api.CreatePrimvar("hairT", Sdf.ValueTypeNames.FloatArray,
                      UsdGeom.Tokens.vertex).Set(Vt.FloatArray(hairT))
    if with_tangent:
        api.CreatePrimvar("hairTangent", Sdf.ValueTypeNames.Vector3fArray,
                          UsdGeom.Tokens.vertex).Set(Vt.Vec3fArray(tangents))
    api.CreatePrimvar("hairId", Sdf.ValueTypeNames.FloatArray,
                      UsdGeom.Tokens.uniform).Set(Vt.FloatArray(hairId))
    api.CreatePrimvar("displayColor", Sdf.ValueTypeNames.Color3fArray,
                      UsdGeom.Tokens.uniform).Set(Vt.Vec3fArray(dispColor))
    api.CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                      UsdGeom.Tokens.uniform).Set(Vt.Vec2fArray(st))
    api.CreatePrimvar("minScreenSpaceWidths", Sdf.ValueTypeNames.FloatArray,
                      UsdGeom.Tokens.constant).Set(Vt.FloatArray([1.0]))

    mat = UsdShade.Material.Define(stage, "/World/Looks/HairPreview")
    shd = UsdShade.Shader.Define(stage, "/World/Looks/HairPreview/Surface")
    shd.GetPrim().CreateAttribute("info:implementationSource", Sdf.ValueTypeNames.Token,
                                  True).Set("sourceAsset")
    shd.GetPrim().CreateAttribute("info:glslfx:sourceAsset", Sdf.ValueTypeNames.Asset,
                                  True).Set(Sdf.AssetPath(glslfx))
    shd.CreateInput("rootColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.030, 0.014, 0.006))
    shd.CreateInput("tipColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.32, 0.17, 0.07))
    shd.CreateInput("specular1Gain", Sdf.ValueTypeNames.Float).Set(0.45)
    shd.CreateInput("specular2Gain", Sdf.ValueTypeNames.Float).Set(0.30)
    shd.CreateInput("randomValue", Sdf.ValueTypeNames.Float).Set(0.25)
    shd.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    mat.CreateSurfaceOutput().ConnectToSource(shd.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(curves.GetPrim()).Bind(mat)

    key = UsdLux.DistantLight.Define(stage, "/World/Lights/Key")
    key.CreateIntensityAttr(3.0)
    UsdGeom.Xformable(key).AddRotateXYZOp().Set(Gf.Vec3f(-35, 25, 0))
    rim = UsdLux.DistantLight.Define(stage, "/World/Lights/Rim")
    rim.CreateIntensityAttr(4.0)
    rim.CreateColorAttr(Gf.Vec3f(0.7, 0.85, 1.0))
    UsdGeom.Xformable(rim).AddRotateXYZOp().Set(Gf.Vec3f(-10, 200, 0))

    cam = UsdGeom.Camera.Define(stage, "/World/Cam")
    UsdGeom.Xformable(cam).AddTranslateOp().Set(Gf.Vec3d(0, 0.35, 5.2))
    cam.CreateFocalLengthAttr(50.0)
    cam.CreateHorizontalApertureAttr(24.0)
    cam.CreateVerticalApertureAttr(24.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))

    stage.GetRootLayer().Save()
    print(f"wrote {out} curves={ncurves} variant={variant} hairTangent={with_tangent}")

if __name__ == "__main__":
    main()
