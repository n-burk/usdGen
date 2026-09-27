"""Run with the usdGen/OpenUSD Python environment to regenerate the example."""
import math
import sys
from pathlib import Path
from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade, UsdLux
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'felt'))
from studio_lighting import add_studio_lighting


def build_sphere(outside=False, studio=False):
    s = Usd.Stage.CreateInMemory()
    s.SetDefaultPrim(s.DefinePrim('/World', 'Xform'))
    s.SetStartTimeCode(1); s.SetEndTimeCode(100)
    s.SetTimeCodesPerSecond(24); s.SetFramesPerSecond(24)
    UsdGeom.SetStageUpAxis(s, 'Z'); UsdGeom.SetStageMetersPerUnit(s, 1)
    motion = UsdGeom.Xform.Define(s, '/World/Motion').AddTranslateOp()
    motion.Set(Gf.Vec3d(0, 0, .5))
    points, faces, lookup = [], [], {}
    n = 8
    for axis in range(3):
        others = [a for a in range(3) if a != axis]
        for sign in (-1, 1):
            for j in range(n):
                for i in range(n):
                    face = []
                    for u,v in ((i,j),(i+1,j),(i+1,j+1),(i,j+1)):
                        key=[0,0,0];key[axis]=sign*n
                        key[others[0]]=2*u-n;key[others[1]]=2*v-n
                        key=tuple(key)
                        if key not in lookup:
                            lookup[key]=len(points)
                            points.append(Gf.Vec3f(*key).GetNormalized()*.24)
                        face.append(lookup[key])
                    if Gf.Dot(Gf.Cross(points[face[1]]-points[face[0]],
                                      points[face[2]]-points[face[0]]),points[face[0]]) < 0:
                        face.reverse()
                    faces.append(face)
    mesh=UsdGeom.Mesh.Define(s,'/World/Motion/Sphere')
    mesh.GetPrim().AddAppliedSchema('UsdGenRestAPI')
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr([4]*len(faces))
    mesh.CreateFaceVertexIndicesAttr([v for f in faces for v in f])
    mesh.CreateSubdivisionSchemeAttr('catmullClark')
    mesh.CreateInterpolateBoundaryAttr('edgeAndCorner')
    mesh.CreateOrientationAttr('rightHanded')
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar('rest',Sdf.ValueTypeNames.Point3fArray,'vertex').Set(points)
    for frame in range(1,101):
        t=(frame-1)/99
        stretch=math.exp(.48*math.sin(4*math.pi*t))
        wide=1/math.sqrt(stretch)
        mesh.GetPointsAttr().Set([Gf.Vec3f(p[0]*wide,p[1]*wide,p[2]*stretch) for p in points],frame)
        motion.Set(Gf.Vec3d(.55*math.sin(2*math.pi*t),0,.5),frame)

    def material(name,color,roughness):
        mat=UsdShade.Material.Define(s,'/World/Looks/'+name)
        shader=UsdShade.Shader.Define(s,str(mat.GetPath())+'/Surface')
        shader.SetShaderId('UsdPreviewSurface')
        shader.CreateInput('diffuseColor',Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*color))
        shader.CreateInput('roughness',Sdf.ValueTypeNames.Float).Set(roughness)
        mat.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput('surface',Sdf.ValueTypeNames.Token))
        return mat
    cloth=material('Cloth',(.52,.31,.045),.9)
    fibre=material('Fibre',(.72,.48,.08),.9)
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(cloth)
    # An untyped groom container selects the established CPU reference lane,
    # required by OpenSubdiv limit Scatter. No baked/authored curve groom.
    groom = '/World/Groom' if outside else '/World/Motion/Groom'
    s.DefinePrim(groom,'Scope')
    desc=s.DefinePrim(groom+'/Hair','UsdGenDescription')
    desc.AddAppliedSchema('UsdGenLookAPI')
    desc.CreateRelationship('usdGen:surface').SetTargets([mesh.GetPath()])
    UsdShade.MaterialBindingAPI.Apply(desc).Bind(fibre)
    desc.CreateAttribute('usdGen:look:rootColor',Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(.72,.48,.08))
    desc.CreateAttribute('usdGen:look:tipColor',Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(.8,.6,.13))
    ops=s.DefinePrim(str(desc.GetPath())+'/Ops','Scope')
    def op(name,kind,attrs):
        p=s.DefinePrim(str(ops.GetPath())+'/'+name,'UsdGen'+kind)
        for key,typ,value in attrs:
            p.CreateAttribute('usdGen:'+key,typ).Set(value)
        return p
    T=Sdf.ValueTypeNames
    op('scatter','Scatter',[('density',T.Float,4500.),('seed',T.Int,41),('subdivisionLevel',T.Int,3)])
    op('grow','Grow',[('length',T.Float,.025),('segments',T.Int,8),('lift',T.Float,20.),
                     ('azimuthRandom',T.Float,1.),('lengthRandom',T.Float2,Gf.Vec2f(.75,1.25))])
    op('curl','Curl',[('radius',T.Float,.002),('frequency',T.Float,55.),('phaseRandom',T.Float,1.)])
    op('width','Width',[('width',T.Float,.0011),('replace',T.Bool,True),
                       ('width:knots',T.Float2Array,[(0,1),(.7,.7),(1,.08)])])
    op('surfaceAnimate','Deform',[('rbfSamples',T.Int,64),('lockRoots',T.Bool,False),('mask',T.Float,1.)])
    ops.SetChildrenReorder(['surfaceAnimate','width','curl','grow','scatter'])

    cam=UsdGeom.Camera.Define(s,'/World/Camera')
    cam.CreateFocalLengthAttr(55);cam.CreateHorizontalApertureAttr(36);cam.CreateVerticalApertureAttr(20.25)
    cam.CreateClippingRangeAttr(Gf.Vec2f(.01,100))
    cam.AddTransformOp().Set(Gf.Matrix4d().SetLookAt(Gf.Vec3d(0,-3.5,1.15),Gf.Vec3d(0,0,.5),Gf.Vec3d(0,0,1)).GetInverse())
    if studio:
        add_studio_lighting(s, '/World/Studio', (0, 0, .5),
                            '../felt/textures/studio_contrast.exr')
    else:
        # Braid generators reuse this base scene and retain their lighting.
        key=UsdLux.DistantLight.Define(s,'/World/Key')
        key.CreateIntensityAttr(1);key.CreateNormalizeAttr(True);key.CreateAngleAttr(15)
        UsdGeom.Xformable(key).AddTransformOp().Set(Gf.Matrix4d().SetLookAt(Gf.Vec3d(-1,-2,3),Gf.Vec3d(0),Gf.Vec3d(0,0,1)).GetInverse())
        fill=UsdLux.DomeLight.Define(s,'/World/Fill');fill.CreateIntensityAttr(.6)
        fill.CreateTextureFileAttr('../felt/textures/studio.exr')
    return s


def main():
    out = Path(__file__).parent / 'felt_sphere_animated.usda'
    s = build_sphere(studio=True)
    s.GetRootLayer().Export(str(out))
    print(out)


if __name__=='__main__':main()
