import os, sys
from pxr import Sdf, Usd, UsdGeom, Vt
n = int(sys.argv[1]); out = sys.argv[2]
stage = Usd.Stage.CreateNew(out)
UsdGeom.Xform.Define(stage, "/Char")
UsdGeom.Xform.Define(stage, "/Char/Geo")
m = UsdGeom.Mesh.Define(stage, "/Char/Geo/Skin")
m.CreatePointsAttr(Vt.Vec3fArray([(0,0,0),(1,0,0),(1,1,0),(0,1,0)]))
m.CreateFaceVertexCountsAttr(Vt.IntArray([4]))
m.CreateFaceVertexIndicesAttr(Vt.IntArray([0,1,2,3]))
m.CreateExtentAttr(Vt.Vec3fArray([(0,0,0),(1,1,0)]))
# a broad-but-shallow rig-ish hierarchy so _resetPrimView has real work
for i in range(n):
    UsdGeom.Xform.Define(stage, "/Char/Rig/Grp%03d" % (i // 10))
    UsdGeom.Xform.Define(stage, "/Char/Rig/Grp%03d/Node%04d" % (i // 10, i))
UsdGeom.Xform.Define(stage, "/Groom")
stage.GetRootLayer().Save()
print("wrote", out, "prims:", len(list(stage.Traverse())))
