import math, random, os, sys
from pxr import Usd, UsdGeom, UsdShade, UsdLux, Sdf, Gf, Vt

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "hair_scene.usda")
random.seed(7)

stage = Usd.Stage.CreateNew(out) if not os.path.exists(out) else Usd.Stage.CreateNew(out+".tmp")
if os.path.exists(out+".tmp"):
    os.remove(out+".tmp"); stage = Usd.Stage.CreateNew(out) if not os.path.exists(out) else Usd.Stage.Open(out)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
UsdGeom.SetStageMetersPerUnit(stage, 1.0)
world = UsdGeom.Xform.Define(stage, "/World")
stage.SetDefaultPrim(world.GetPrim())

# ---- scalp (a simple sphere cap made of a mesh) so we can see hair against geo
sphere = UsdGeom.Sphere.Define(stage, "/World/Scalp")
sphere.CreateRadiusAttr(0.98)
sphere.CreateDisplayColorAttr([Gf.Vec3f(0.18, 0.10, 0.07)])

# ---- hair
NCURVES   = 4000
NCV       = 8          # control points per curve
LEN       = 0.55
pts, widths, hairT, counts, hairId, dispColor, tangents, st = [], [], [], [], [], [], [], []
for c in range(NCURVES):
    # random point on the upper hemisphere
    while True:
        u = random.uniform(-1,1); v = random.uniform(-1,1); w = random.uniform(0.1,1.0)
        n = Gf.Vec3f(u, w, v)
        if n.GetLength() > 1e-3: break
    n = n.GetNormalized()
    root = n * 1.0
    # bend the strand: start along the normal, curl toward -Y
    for i in range(NCV):
        t = i/(NCV-1.0)
        droop = Gf.Vec3f(0.0, -0.9*t*t, 0.0)
        jitter = Gf.Vec3f(random.uniform(-1,1), random.uniform(-1,1), random.uniform(-1,1))*0.02*t
        p = root + (n*LEN*t) + droop*LEN + jitter
        pts.append(Gf.Vec3f(p))
        widths.append(0.0032*(1.0-0.85*t))
        hairT.append(t)
        # object-space tangent (finite difference approximation)
        tan = (n*LEN + Gf.Vec3f(0.0, -1.8*t*LEN, 0.0))
        tangents.append(Gf.Vec3f(tan.GetNormalized()))
    counts.append(NCV)
    hairId.append(random.random())
    shade = 0.75 + 0.5*random.random()
    dispColor.append(Gf.Vec3f(shade, shade, shade))
    st.append(Gf.Vec2f(0.5+0.5*n[0], 0.5+0.5*n[2]))

curves = UsdGeom.BasisCurves.Define(stage, "/World/Hair")
curves.CreateTypeAttr(UsdGeom.Tokens.cubic)
curves.CreateBasisAttr(UsdGeom.Tokens.bspline)
curves.CreateWrapAttr(UsdGeom.Tokens.pinned)
curves.CreateCurveVertexCountsAttr(Vt.IntArray(counts))
curves.CreatePointsAttr(Vt.Vec3fArray(pts))
curves.CreateWidthsAttr(Vt.FloatArray(widths))
curves.SetWidthsInterpolation(UsdGeom.Tokens.vertex)
curves.CreateExtentAttr(UsdGeom.Boundable.ComputeExtentFromPlugins(curves, Usd.TimeCode.Default()))

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

# per-prim refine level (Hydra reads displayStyle/refineLevel)
# refineLevel is a Hydra displayStyle concept; usdImaging reads it from
# UsdGeomGprim "refinementLevel" custom metadata only when the plugin declares it.
# We instead rely on the engine complexity setting (see driver: SetComplexity).


# ---- material: our glslfx
mat = UsdShade.Material.Define(stage, "/World/Looks/HairPreview")
shd = UsdShade.Shader.Define(stage, "/World/Looks/HairPreview/Surface")
shd.GetPrim().CreateAttribute("info:implementationSource", Sdf.ValueTypeNames.Token,
                              True).Set("sourceAsset")
shd.GetPrim().CreateAttribute("info:glslfx:sourceAsset", Sdf.ValueTypeNames.Asset,
                              True).Set(Sdf.AssetPath("./usdGenHairPreview.glslfx"))
shd.CreateInput("rootColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.030,0.014,0.006))
shd.CreateInput("tipColor",  Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.32,0.17,0.07))
shd.CreateInput("specular1Gain", Sdf.ValueTypeNames.Float).Set(0.45)
shd.CreateInput("specular2Gain", Sdf.ValueTypeNames.Float).Set(0.30)
shd.CreateInput("randomValue", Sdf.ValueTypeNames.Float).Set(0.25)
shd.CreateOutput("surface", Sdf.ValueTypeNames.Token)
mat.CreateSurfaceOutput().ConnectToSource(shd.ConnectableAPI(), "surface")
UsdShade.MaterialBindingAPI.Apply(curves.GetPrim()).Bind(mat)

# ---- lights
key = UsdLux.DistantLight.Define(stage, "/World/Lights/Key")
key.CreateIntensityAttr(3.0)
UsdGeom.Xformable(key).AddRotateXYZOp().Set(Gf.Vec3f(-35, 25, 0))
rim = UsdLux.DistantLight.Define(stage, "/World/Lights/Rim")
rim.CreateIntensityAttr(4.0)
rim.CreateColorAttr(Gf.Vec3f(0.7,0.85,1.0))
UsdGeom.Xformable(rim).AddRotateXYZOp().Set(Gf.Vec3f(-10, 200, 0))

cam = UsdGeom.Camera.Define(stage, "/World/Cam")
UsdGeom.Xformable(cam).AddTranslateOp().Set(Gf.Vec3d(0, 0.35, 5.2))
cam.CreateFocalLengthAttr(50.0)
cam.CreateHorizontalApertureAttr(24.0)
cam.CreateVerticalApertureAttr(24.0)
cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 100.0))

stage.GetRootLayer().Save()
print("wrote", out, "curves:", NCURVES, "cvs:", len(pts))
