import os, shutil
from pxr import Usd, UsdShade, UsdGeom, Sdf, Gf
here=os.path.dirname(os.path.abspath(__file__))
shutil.copy(os.path.join(here,"hair_scene.usda"), os.path.join(here,"hair_scene_preview.usda"))
stage=Usd.Stage.Open(os.path.join(here,"hair_scene_preview.usda"))
curves=stage.GetPrimAtPath("/World/Hair")

mat = UsdShade.Material.Define(stage, "/World/Looks/HairPreviewSurface")
ps  = UsdShade.Shader.Define(stage, "/World/Looks/HairPreviewSurface/PS")
ps.CreateIdAttr("UsdPreviewSurface")
ps.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.35)
ps.CreateInput("metallic",  Sdf.ValueTypeNames.Float).Set(0.0)
ps.CreateInput("specular",  Sdf.ValueTypeNames.Float).Set(0.6)

pv = UsdShade.Shader.Define(stage, "/World/Looks/HairPreviewSurface/StReader")
pv.CreateIdAttr("UsdPrimvarReader_float2")
pv.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("st")
pvOut = pv.CreateOutput("result", Sdf.ValueTypeNames.Float2)

tex = UsdShade.Shader.Define(stage, "/World/Looks/HairPreviewSurface/Tex")
tex.CreateIdAttr("UsdUVTexture")
tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(Sdf.AssetPath("./scalp_map.png"))
tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(pvOut)
tex.CreateInput("fallback", Sdf.ValueTypeNames.Float4).Set(Gf.Vec4f(1,0,1,1))
texOut = tex.CreateOutput("rgb", Sdf.ValueTypeNames.Float3)

ps.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(texOut)
psOut = ps.CreateOutput("surface", Sdf.ValueTypeNames.Token)
mat.CreateSurfaceOutput().ConnectToSource(psOut)
UsdShade.MaterialBindingAPI(curves).Bind(mat)
stage.GetRootLayer().Save()
print("ok")
