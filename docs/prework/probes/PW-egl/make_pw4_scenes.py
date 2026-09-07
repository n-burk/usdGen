#!/usr/bin/env python3
"""PW-4 (L-1): build four 4000-curve scenes that isolate the Storm material
terminal used.

  pw4_glslfx_only.usda      material: outputs:surface -> glslfx shader
  pw4_mtlx_only.usda        material: outputs:surface -> chiang hair network
  pw4_glslfx_ctx_only.usda  material: outputs:glslfx:surface -> glslfx shader
  pw4_both.usda             material: outputs:surface -> chiang hair network
                            AND outputs:glslfx:surface -> glslfx shader  <-- test

If Storm resolves the render-context terminal first (materialRenderContexts =
{glslfx, mtlx}, hdSt/renderDelegate.cpp:695-707), the "both" render must match
the glslfx-only render and differ from the mtlx-only render.
"""
import os, sys
from pxr import Usd, UsdGeom, UsdShade, UsdLux, Sdf, Gf, Vt, Tf

HERE = os.path.dirname(os.path.abspath(__file__))
NCURVES, NCV, LEN = 4000, 8, 0.55


def build_geometry(stage):
    import random
    random.seed(7)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())

    sphere = UsdGeom.Sphere.Define(stage, "/World/Scalp")
    sphere.CreateRadiusAttr(0.98)
    sphere.CreateDisplayColorAttr([Gf.Vec3f(0.18, 0.10, 0.07)])

    pts, widths, hairT, counts, hairId, dispColor, tangents, st = [], [], [], [], [], [], [], []
    for c in range(NCURVES):
        while True:
            u = random.uniform(-1, 1); v = random.uniform(-1, 1); w = random.uniform(0.1, 1.0)
            n = Gf.Vec3f(u, w, v)
            if n.GetLength() > 1e-3:
                break
        n = n.GetNormalized()
        root = n * 1.0
        for i in range(NCV):
            t = i / (NCV - 1.0)
            droop = Gf.Vec3f(0.0, -0.9 * t * t, 0.0)
            jitter = Gf.Vec3f(random.uniform(-1, 1), random.uniform(-1, 1),
                              random.uniform(-1, 1)) * 0.02 * t
            p = root + (n * LEN * t) + droop * LEN + jitter
            pts.append(Gf.Vec3f(p))
            widths.append(0.0032 * (1.0 - 0.85 * t))
            hairT.append(t)
            tan = (n * LEN + Gf.Vec3f(0.0, -1.8 * t * LEN, 0.0))
            tangents.append(Gf.Vec3f(tan.GetNormalized()))
        counts.append(NCV)
        hairId.append(random.random())
        shade = 0.75 + 0.5 * random.random()
        dispColor.append(Gf.Vec3f(shade, shade, shade))
        st.append(Gf.Vec2f(0.5 + 0.5 * n[0], 0.5 + 0.5 * n[2]))

    curves = UsdGeom.BasisCurves.Define(stage, "/World/Hair")
    curves.CreateTypeAttr(UsdGeom.Tokens.cubic)
    curves.CreateBasisAttr(UsdGeom.Tokens.bspline)
    curves.CreateWrapAttr(UsdGeom.Tokens.pinned)
    curves.CreateCurveVertexCountsAttr(Vt.IntArray(counts))
    curves.CreatePointsAttr(Vt.Vec3fArray(pts))
    curves.CreateWidthsAttr(Vt.FloatArray(widths))
    curves.SetWidthsInterpolation(UsdGeom.Tokens.vertex)
    api = UsdGeom.PrimvarsAPI(curves.GetPrim())
    api.CreatePrimvar("hairT", Sdf.ValueTypeNames.FloatArray,
                      UsdGeom.Tokens.vertex).Set(Vt.FloatArray(hairT))
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
    return curves


def add_glslfx_shaders(stage, matName):
    shd = UsdShade.Shader.Define(stage, f"/World/Looks/{matName}/GlslfxSurface")
    shd.GetPrim().CreateAttribute("info:implementationSource", Sdf.ValueTypeNames.Token,
                                  True).Set("sourceAsset")
    shd.GetPrim().CreateAttribute("info:glslfx:sourceAsset", Sdf.ValueTypeNames.Asset,
                                  True).Set(Sdf.AssetPath("./usdGenHairPreviewPrimvar.glslfx"))
    shd.CreateInput("rootColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.030, 0.014, 0.006))
    shd.CreateInput("tipColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.32, 0.17, 0.07))
    shd.CreateInput("specular1Gain", Sdf.ValueTypeNames.Float).Set(0.45)
    shd.CreateInput("specular2Gain", Sdf.ValueTypeNames.Float).Set(0.30)
    shd.CreateInput("randomValue", Sdf.ValueTypeNames.Float).Set(0.25)
    shd.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    return shd


def add_mtlx_network(stage, matName):
    absorb = UsdShade.Shader.Define(stage, f"/World/Looks/{matName}/Absorb")
    absorb.GetPrim().CreateAttribute("info:id", Sdf.ValueTypeNames.Token, True
                                     ).Set("ND_deon_hair_absorption_from_melanin")
    absorb.CreateInput("melanin_concentration", Sdf.ValueTypeNames.Float).Set(0.03)
    absorb.CreateInput("melanin_redness", Sdf.ValueTypeNames.Float).Set(0.6)
    absorb.CreateOutput("absorption", Sdf.ValueTypeNames.Float3)

    rough = UsdShade.Shader.Define(stage, f"/World/Looks/{matName}/Rough")
    rough.GetPrim().CreateAttribute("info:id", Sdf.ValueTypeNames.Token, True
                                    ).Set("ND_chiang_hair_roughness")
    rough.CreateInput("azimuthal", Sdf.ValueTypeNames.Float).Set(0.22)
    rough.CreateInput("longitudinal", Sdf.ValueTypeNames.Float).Set(0.1)
    rough.CreateOutput("roughness_R", Sdf.ValueTypeNames.Float2)
    rough.CreateOutput("roughness_TRT", Sdf.ValueTypeNames.Float2)
    rough.CreateOutput("roughness_TT", Sdf.ValueTypeNames.Float2)

    bsdf = UsdShade.Shader.Define(stage, f"/World/Looks/{matName}/Bsdf")
    bsdf.GetPrim().CreateAttribute("info:id", Sdf.ValueTypeNames.Token, True
                                   ).Set("ND_chiang_hair_bsdf")
    bsdf.CreateInput("absorption_coefficient", Sdf.ValueTypeNames.Float3
                     ).ConnectToSource(absorb.ConnectableAPI(), "absorption")
    bsdf.CreateInput("curve_direction", Sdf.ValueTypeNames.Float3)
    bsdf.CreateInput("cuticle_angle", Sdf.ValueTypeNames.Float).Set(0.5)
    bsdf.CreateInput("ior", Sdf.ValueTypeNames.Float).Set(1.55)
    bsdf.CreateInput("roughness_R", Sdf.ValueTypeNames.Float2
                     ).ConnectToSource(rough.ConnectableAPI(), "roughness_R")
    bsdf.CreateInput("roughness_TRT", Sdf.ValueTypeNames.Float2
                     ).ConnectToSource(rough.ConnectableAPI(), "roughness_TRT")
    bsdf.CreateInput("roughness_TT", Sdf.ValueTypeNames.Float2
                     ).ConnectToSource(rough.ConnectableAPI(), "roughness_TT")
    bsdf.CreateOutput("out", Sdf.ValueTypeNames.Token)

    tang = UsdShade.Shader.Define(stage, f"/World/Looks/{matName}/TangentPV")
    tang.GetPrim().CreateAttribute("info:id", Sdf.ValueTypeNames.Token, True
                                   ).Set("ND_geompropvalue_vector3")
    tang.CreateInput("geomprop", Sdf.ValueTypeNames.String).Set("hairTangent")
    tang.CreateOutput("out", Sdf.ValueTypeNames.Float3)
    bsdf.GetInput("curve_direction").ConnectToSource(tang.ConnectableAPI(), "out")

    surf = UsdShade.Shader.Define(stage, f"/World/Looks/{matName}/MxSurface")
    surf.GetPrim().CreateAttribute("info:id", Sdf.ValueTypeNames.Token, True
                                   ).Set("ND_surface")
    surf.CreateInput("bsdf", Sdf.ValueTypeNames.Token).ConnectToSource(
        bsdf.ConnectableAPI(), "out")
    surf.CreateInput("opacity", Sdf.ValueTypeNames.Float).Set(1.0)
    surf.CreateOutput("out", Sdf.ValueTypeNames.Token)
    return surf


def add_common(stage):
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


def build(out, terminals):
    """terminals: list of (renderContext, net):
       ('universal','glslfx') -> outputs:surface = glslfx shader
       ('universal','mtlx')   -> outputs:surface = chiang network
       ('glslfx','glslfx')    -> outputs:glslfx:surface = glslfx shader
       ('mtlx','mtlx')        -> outputs:mtlx:surface = chiang network
    """
    stage = Usd.Stage.CreateNew(out)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    curves = build_geometry(stage)
    mat = UsdShade.Material.Define(stage, "/World/Looks/M")
    for ctx, net in terminals:
        rctx = None if ctx == "universal" else ctx
        outp = mat.CreateSurfaceOutput() if rctx is None else mat.CreateSurfaceOutput(rctx)
        if net == "glslfx":
            src = add_glslfx_shaders(stage, "M")
            outp.ConnectToSource(src.ConnectableAPI(), "surface")
        else:
            src = add_mtlx_network(stage, "M")
            outp.ConnectToSource(src.ConnectableAPI(), "out")
    UsdShade.MaterialBindingAPI.Apply(curves.GetPrim()).Bind(mat)
    add_common(stage)
    stage.GetRootLayer().Save()
    print("wrote", out, terminals)


KINDS = {
    "glslfx_only": [("universal", "glslfx")],
    "mtlx_only": [("universal", "mtlx")],
    "glslfx_ctx_only": [("glslfx", "glslfx")],
    "both": [("universal", "mtlx"), ("glslfx", "glslfx")],
}

if __name__ == "__main__":
    kinds = sys.argv[1:] or list(KINDS)
    for k in kinds:
        build(os.path.join(HERE, f"pw4_{k}.usda"), KINDS[k])
