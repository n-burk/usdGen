"""Two strands and two dense braids: one description, Ptex-partitioned drivers.

Requires the usdGen/OpenUSD Python environment. Writes the USDA scenes.
The categorical Ptex maps under maps/ are inputs; this script does not bake them.
"""
import math
from pathlib import Path
from pxr import Gf, Sdf, UsdGeom, UsdShade
from make_animated_sphere import build_sphere
from make_motion_examples import curves
from make_center_curve_braid import simulate

HERE=Path(__file__).resolve().parent
T=Sdf.ValueTypeNames


def make(dense, simulated):
    s=build_sphere()
    motion=s.GetPrimAtPath('/World/Motion')
    attr=motion.GetAttribute('xformOp:translate')
    for t in attr.GetTimeSamples(): attr.ClearAtTime(t)
    s.RemovePrim('/World/Motion/Sphere');s.RemovePrim('/World/Motion/Groom')
    surface=UsdGeom.Mesh.Define(s,'/World/Motion/Scalp')
    surface.GetPrim().AddAppliedSchema('UsdGenRestAPI')
    surface.CreatePointsAttr([(-.5,-.14,0),(.5,-.14,0),(.5,.14,0),(-.5,.14,0)])
    surface.CreateFaceVertexCountsAttr([4]);surface.CreateFaceVertexIndicesAttr([0,1,2,3])
    surface.CreateSubdivisionSchemeAttr('none');surface.CreateDoubleSidedAttr(True)
    UsdGeom.PrimvarsAPI(surface).CreatePrimvar('st',T.TexCoord2fArray,'faceVarying').Set([(0,0),(1,0),(1,1),(0,1)])
    # The visible stripe is an actual texture preview of the two IDs within ONE face.
    mat=UsdShade.Material.Define(s,'/World/Looks/Regions')
    shader=UsdShade.Shader.Define(s,'/World/Looks/Regions/Surface');shader.SetShaderId('UsdPreviewSurface')
    shader.CreateInput('roughness',T.Float).Set(.85)
    uv=UsdShade.Shader.Define(s,'/World/Looks/Regions/UV');uv.SetShaderId('UsdPrimvarReader_float2')
    uv.CreateInput('varname',T.Token).Set('st')
    tex=UsdShade.Shader.Define(s,'/World/Looks/Regions/Tex');tex.SetShaderId('UsdUVTexture')
    tex.CreateInput('file',T.Asset).Set('maps/two_regions.png')
    tex.CreateInput('sourceColorSpace',T.Token).Set('sRGB')
    tex.CreateInput('st',T.Float2).ConnectToSource(uv.CreateOutput('result',T.Float2))
    shader.CreateInput('diffuseColor',T.Color3f).ConnectToSource(tex.CreateOutput('rgb',T.Float3))
    mat.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput('surface',T.Token))
    UsdShade.MaterialBindingAPI.Apply(surface.GetPrim()).Bind(mat)
    rest_guides=[];roots=[]
    for center in (-.25,.25):
        if dense:
            for bundle in range(3):
                phase=2*math.pi*bundle/3
                x0=.045*math.sin(phase);y0=.024*math.sin(2*phase)
                rest_guides.append([(center+.045*math.sin(phase+5*math.pi*k/96),
                                     .024*math.sin(2*(phase+5*math.pi*k/96)),-.52*k/96) for k in range(97)])
                offsets=[(0,0)]
                for ring in range(1,7):
                    offsets += [(.003*ring*math.cos(2*math.pi*j/(6*ring)),
                                 .003*ring*math.sin(2*math.pi*j/(6*ring))) for j in range(6*ring)]
                for dx,dy in offsets:
                    roots.append([(center+x0+dx,y0+dy,-.001*k) for k in range(4)])
        else:
            rest_guides.append([(center,0,-.52*k/32) for k in range(33)])
            roots.append([(center,0,-.001*k) for k in range(4)])
    s.DefinePrim('/World/Motion/RestInputs','Scope')
    curves(s,'/World/Motion/RestInputs/Guides',rest_guides)
    curves(s,'/World/Motion/RestInputs/RootSeeds',roots,'hair')
    sim_rest,frames=simulated
    def transform(p,side):
        # Opposite phase/direction and out-of-plane response for clear independence.
        return Gf.Vec3f((-.25 if side==0 else .25)+(p[0] if side==0 else -.65*p[0]),
                       p[1] if side==0 else -p[1],p[2]+.22)
    centers=curves(s,'/World/Motion/Drivers',[[transform(p,side) for p in sim_rest] for side in range(2)])
    for frame,pts in frames.items():
        centers.GetPointsAttr().Set([transform(p,side) for side in range(2) for p in pts],frame)
    # A reversed pair for baking the regression fixture; hidden guide purpose.
    reverse=UsdGeom.Points.Define(s,'/World/RegionSeedsReverse')
    reverse.CreatePointsAttr([(.25,0,.5),(-.25,0,.5)]);reverse.CreatePurposeAttr('guide')
    reverse.CreateWidthsAttr([.003,.003])
    reverse.CreateVisibilityAttr('invisible')
    s.DefinePrim('/World/Motion/Groom','Scope')
    desc=s.DefinePrim('/World/Motion/Groom/Hair','UsdGenDescription');desc.AddAppliedSchema('UsdGenLookAPI')
    desc.CreateRelationship('usdGen:surface').SetTargets([surface.GetPath()])
    UsdShade.MaterialBindingAPI.Apply(desc).Bind(UsdShade.Material(s.GetPrimAtPath('/World/Looks/Fibre')))
    for name,col in [('rootColor',(.72,.48,.08)),('tipColor',(.8,.6,.13))]:
        desc.CreateAttribute('usdGen:look:'+name,T.Color3f).Set(Gf.Vec3f(*col))
    base=str(desc.GetPath());mapprim=s.DefinePrim(base+'/Maps/Regions','UsdGenPtexMap')
    for name,typ,value in [('file',T.Asset,'maps/two_regions.ptx'),('filter',T.Token,'nearest'),
                           ('firstChannel',T.Int,0),('channelCount',T.Int,1),('blur',T.Float,0.)]:
        mapprim.CreateAttribute('usdGen:map:'+name,typ).Set(value)
    ops=s.DefinePrim(base+'/Ops','Scope')
    def op(name,kind,attrs):
        p=s.DefinePrim(str(ops.GetPath())+'/'+name,'UsdGen'+kind)
        for key,typ,value in attrs:p.CreateAttribute('usdGen:'+key,typ).Set(value)
        return p
    source=op('roots','CurveSource',[('useRest',T.Bool,True),('idSource',T.Token,'primvar'),('rebind',T.Token,'onError')])
    source.CreateRelationship('usdGen:curves').SetTargets(['/World/Motion/RestInputs/RootSeeds'])
    grow=op('guideGrow','GuideInterpolate',[('cvCount',T.Int,64),('maxGuides',T.Int,1),
        ('influenceRadius',T.Float,1.),('blendInSkinSpace',T.Float,0.),('useUniqueGuide',T.Bool,True)])
    grow.CreateRelationship('usdGen:guides').SetTargets(['/World/Motion/RestInputs/Guides'])
    grow.CreateRelationship('usdGen:regionMap').SetTargets([mapprim.GetPath()])
    op('width','Width',[('width',T.Float,.0032 if dense else .014),('replace',T.Bool,True)])
    deform=op('regionAnimate','Deform',[('mode',T.Token,'curveWrap'),('lockRoots',T.Bool,False),('guideRegions',T.IntArray,[0,1])])
    deform.CreateRelationship('usdGen:guides').SetTargets([centers.GetPath()])
    deform.CreateRelationship('usdGen:regionMap').SetTargets([mapprim.GetPath()])
    ops.SetChildrenReorder(['regionAnimate','width','guideGrow','roots'])
    s.GetPrimAtPath('/World/Camera').GetAttribute('xformOp:transform').Set(Gf.Matrix4d().SetLookAt(
        Gf.Vec3d(0,-3.4,1.1),Gf.Vec3d(0,0,.25),Gf.Vec3d(0,0,1)).GetInverse())
    s.GetRootLayer().documentation='One description, two animated center curves. Ptex regions 0/1 split a SINGLE quad into two independent growth and curve-wrap bindings. No Scatter.'
    return s


def main():
    (HERE/'maps').mkdir(exist_ok=True)
    simulated=simulate()
    for dense,name in [(False,'two_curves_ptex_regions'),(True,'two_braids_ptex_regions')]:
        out=HERE/(name+'.usda');stage=make(dense,simulated)
        stage.GetRootLayer().Export(str(out));print(out)
    # Same partition as the categorical Ptex file, for an unambiguous visual key.
    from PIL import Image
    im=Image.new('RGB',(64,64));im.putdata([(235,175,45) if x<32 else (45,160,205) for y in range(64) for x in range(64)])
    im.save(HERE/'maps'/'two_regions.png')


if __name__=='__main__':main()
