import os, shutil
from pxr import Usd, UsdShade, Sdf
here=os.path.dirname(os.path.abspath(__file__))
shutil.copy(os.path.join(here,"hair_scene_mtlx.usda"), os.path.join(here,"hair_scene_mtlx2.usda"))
stage=Usd.Stage.Open(os.path.join(here,"hair_scene_mtlx2.usda"))
gp = UsdShade.Shader.Define(stage, "/World/Looks/HairMx/TangentPV")
gp.CreateIdAttr("ND_geompropvalue_vector3")
gp.CreateInput("geomprop", Sdf.ValueTypeNames.String).Set("hairTangent")
out = gp.CreateOutput("out", Sdf.ValueTypeNames.Float3)
bsdf = UsdShade.Shader.Get(stage, "/World/Looks/HairMx/Bsdf")
bsdf.CreateInput("curve_direction", Sdf.ValueTypeNames.Float3).ConnectToSource(out)
stage.GetRootLayer().Save()
print("ok")
