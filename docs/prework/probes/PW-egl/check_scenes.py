from pxr import Usd, UsdGeom
for f in ('scene_100k_A.usdc','scene_100k_B.usdc','scene_200k_B.usdc'):
    s = Usd.Stage.Open(f)
    c = UsdGeom.BasisCurves(s.GetPrimAtPath('/World/Hair'))
    names = [a.GetName() for a in c.GetPrim().GetAttributes()]
    print(f, 'hasHairTangent=', 'hairTangent' in names, 'nCVs=', len(c.GetPointsAttr().Get()))
