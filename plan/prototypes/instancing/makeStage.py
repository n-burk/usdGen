#!/usr/bin/env python3
"""Builds the probe stage:
   /World/Scalp_class  (a class-like def, used as native-instance source)
   /World/HeadA, /World/HeadB   -> instanceable references of /Scalp (native instancing)
   /World/Cards/PI              -> UsdGeomPointInstancer with a card Mesh prototype
"""
import sys
from pxr import Usd, UsdGeom, Sdf, Gf, Vt, UsdShade

out = sys.argv[1]

# ---- the referenced asset layer (scalp) -------------------------------------
scalpLayer = out.replace(".usda", "_scalp.usda")
s2 = Usd.Stage.CreateNew(scalpLayer)
scalp = UsdGeom.Xform.Define(s2, "/Scalp")
m = UsdGeom.Mesh.Define(s2, "/Scalp/ScalpMesh")
m.CreatePointsAttr([(-1,0,-1),(1,0,-1),(1,0,1),(-1,0,1)])
m.CreateFaceVertexCountsAttr([4])
m.CreateFaceVertexIndicesAttr([0,1,2,3])
m.CreateExtentAttr([(-1,0,-1),(1,0,1)])
UsdGeom.PrimvarsAPI(m).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                                     UsdGeom.Tokens.faceVarying).Set(
                                     [(0,0),(1,0),(1,1),(0,1)])
s2.SetDefaultPrim(scalp.GetPrim())
s2.GetRootLayer().Save()

# ---- the main stage ---------------------------------------------------------
st = Usd.Stage.CreateNew(out)
world = UsdGeom.Xform.Define(st, "/World")

import os
rel = os.path.basename(scalpLayer)
for name, tx in (("HeadA", 0.0), ("HeadB", 5.0)):
    x = UsdGeom.Xform.Define(st, "/World/" + name)
    x.AddTranslateOp().Set(Gf.Vec3d(tx, 0, 0))
    x.GetPrim().GetReferences().AddReference(rel)
    x.GetPrim().SetInstanceable(True)

# ---- point instancer with a card mesh prototype ------------------------------
pi = UsdGeom.PointInstancer.Define(st, "/World/Cards/PI")
card = UsdGeom.Mesh.Define(st, "/World/Cards/PI/Prototypes/Card")
card.CreatePointsAttr([(-0.1,0,0),(0.1,0,0),(0.1,1,0),(-0.1,1,0)])
card.CreateFaceVertexCountsAttr([4])
card.CreateFaceVertexIndicesAttr([0,1,2,3])
card.CreateExtentAttr([(-0.1,0,0),(0.1,1,0)])
UsdGeom.Imageable(card).CreatePurposeAttr(UsdGeom.Tokens.default_)
pi.CreatePrototypesRel().SetTargets([card.GetPath()])
pi.CreateProtoIndicesAttr([0,0,0])
pi.CreatePositionsAttr([(0,0,0),(1,0,0),(2,0,0)])
pi.CreateOrientationsAttr([Gf.Quath(1,0,0,0)]*3)
pi.CreateScalesAttr([(1,1,1)]*3)
pi.CreateExtentAttr([(-1,-1,-1),(3,2,1)])

# ---- a standalone card mesh we will later use as a synthesized prototype -----
proto = UsdGeom.Mesh.Define(st, "/World/HairCards/CardProto")
proto.CreatePointsAttr([(-0.05,0,0),(0.05,0,0),(0.05,1,0),(-0.05,1,0)])
proto.CreateFaceVertexCountsAttr([4])
proto.CreateFaceVertexIndicesAttr([0,1,2,3])
proto.CreateExtentAttr([(-0.05,0,0),(0.05,1,0)])

st.SetDefaultPrim(world.GetPrim())
st.GetRootLayer().Save()
print("wrote", out, "and", scalpLayer)
