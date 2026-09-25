"""Regenerate the four motion demos with the usdGen/OpenUSD Python environment."""
import math
from pathlib import Path
from pxr import Gf, Sdf, UsdGeom, UsdShade
from make_animated_sphere import build_sphere

HERE = Path(__file__).resolve().parent
T = Sdf.ValueTypeNames


def curves(stage, path, strands, role='guide'):
    c = UsdGeom.BasisCurves.Define(stage, path)
    c.GetPrim().AddAppliedSchema('UsdGenCurveAPI')
    c.CreateTypeAttr('cubic'); c.CreateBasisAttr('catmullRom'); c.CreateWrapAttr('pinned')
    c.CreateCurveVertexCountsAttr([len(p) for p in strands])
    pts = [Gf.Vec3f(*p) for strand in strands for p in strand]
    c.CreatePointsAttr(pts)
    UsdGeom.PrimvarsAPI(c).CreatePrimvar('rest', T.Point3fArray, 'vertex').Set(pts)
    UsdGeom.PrimvarsAPI(c).CreatePrimvar('usdGen:role',T.Token,'constant').Set(role)
    UsdGeom.PrimvarsAPI(c).CreatePrimvar('usdGen:curveId',T.UInt64Array,'uniform').Set(list(range(len(strands))))
    c.CreateWidthsAttr([.003]*len(pts)); c.SetWidthsInterpolation('vertex')
    c.CreatePurposeAttr('guide')
    return c


def braid(combined):
    s = build_sphere()
    motion = s.GetPrimAtPath('/World/Motion')
    sphere = s.GetPrimAtPath('/World/Motion/Sphere')
    if not combined:
        for prim, name in ((motion, 'xformOp:translate'), (sphere, 'points')):
            attr = prim.GetAttribute(name)
            for t in attr.GetTimeSamples(): attr.ClearAtTime(t)
    s.RemovePrim('/World/Motion/Groom')
    guides, seeds = [], []
    for bundle in range(3):
        phase = 2*math.pi*bundle/3
        x0, y0 = .045*math.sin(phase), .024*math.sin(2*phase)
        z0 = math.sqrt(.24**2-x0*x0-y0*y0)
        strand = []
        for k in range(97):
            u = k/96
            a = phase + 5*math.pi*u
            strand.append((.045*math.sin(a), .024*math.sin(2*a), z0+.52*u))
        guides.append(strand)
        # A fixed concentric root layout, not a surface scatter. GuideInterpolate
        # expands these short seed curves into the three woven fibre bundles.
        offsets = [(0.,0.)]
        for ring in range(1,7):
            for j in range(6*ring):
                a=2*math.pi*j/(6*ring)
                offsets.append((.003*ring*math.cos(a), .003*ring*math.sin(a)))
        for dx,dy in offsets:
            x,y=x0+dx,y0+dy
            z=math.sqrt(.24**2-x*x-y*y)
            seeds.append([(x,y,z+.001*k) for k in range(4)])
    # Static Default-time growth inputs share the surface's local domain.
    # Animated driver curves and the posed sphere are only read downstream.
    s.DefinePrim('/World/Motion/RestInputs','Scope')
    curves(s,'/World/Motion/RestInputs/Guides',guides)
    source=curves(s,'/World/Motion/RestInputs/RootSeeds',seeds,'hair')
    # Seed roots bind once against the Default-time surface on initial capture.
    drivers=curves(s,'/World/Motion/Drivers',guides)
    for frame in range(1,101):
        t=(frame-1)/99
        dx=.23*math.sin(2*math.pi*t)
        dy=.085*math.sin(4*math.pi*t)
        pts=[]
        for strand in guides:
            for k,p in enumerate(strand):
                u=k/(len(strand)-1)
                pts.append(Gf.Vec3f(p[0]+dx*u*u, p[1]+dy*u*u, p[2]))
        drivers.GetPointsAttr().Set(pts,frame)
    s.DefinePrim('/World/Motion/Groom','Scope')
    desc=s.DefinePrim('/World/Motion/Groom/Hair','UsdGenDescription')
    desc.AddAppliedSchema('UsdGenLookAPI')
    desc.CreateRelationship('usdGen:surface').SetTargets([sphere.GetPath()])
    UsdShade.MaterialBindingAPI.Apply(desc).Bind(UsdShade.Material(s.GetPrimAtPath('/World/Looks/Fibre')))
    desc.CreateAttribute('usdGen:look:rootColor',T.Color3f).Set(Gf.Vec3f(.72,.48,.08))
    desc.CreateAttribute('usdGen:look:tipColor',T.Color3f).Set(Gf.Vec3f(.8,.6,.13))
    ops=s.DefinePrim(str(desc.GetPath())+'/Ops','Scope')
    def op(name,kind,attrs,rel=None):
        p=s.DefinePrim(str(ops.GetPath())+'/'+name,'UsdGen'+kind)
        for key,typ,value in attrs: p.CreateAttribute('usdGen:'+key,typ).Set(value)
        if rel: p.CreateRelationship('usdGen:'+rel[0]).SetTargets([rel[1]])
        return p
    op('roots','CurveSource',[('useRest',T.Bool,True),('idSource',T.Token,'primvar'),
        ('rebind',T.Token,'onError')],('curves',source.GetPath()))
    op('guideGrow','GuideInterpolate',[('cvCount',T.Int,64),('maxGuides',T.Int,1),
        ('influenceRadius',T.Float,.05),('blendInSkinSpace',T.Float,0.),
        ('useUniqueGuide',T.Bool,True)],('guides',Sdf.Path('/World/Motion/RestInputs/Guides')))
    op('width','Width',[('width',T.Float,.0032),('replace',T.Bool,True),
        ('width:knots',T.Float2Array,[(0,1),(.85,1),(1,.35)])])
    op('guideAnimate','Deform',[('rbfSamples',T.Int,128),('lockRoots',T.Bool,True)],
        ('guides',drivers.GetPath()))
    order=['guideAnimate','width','guideGrow','roots']
    if combined:
        op('surfaceAnimate','Deform',[('rbfSamples',T.Int,64),('lockRoots',T.Bool,False)])
        order.insert(0,'surfaceAnimate')
    ops.SetChildrenReorder(order)
    cam=UsdGeom.Camera(s.GetPrimAtPath('/World/Camera'))
    cam.GetPrim().GetAttribute('xformOp:transform').Set(Gf.Matrix4d().SetLookAt(
        Gf.Vec3d(0,-5.1,1.85),Gf.Vec3d(0,0,.95),Gf.Vec3d(0,0,1)).GetInverse())
    s.GetRootLayer().documentation = (
        '100 frames. Fixed root seeds -> rest guide growth -> Width -> guide RBF'
        + (' -> surface RBF. Drivers contain local residual bend only; the surface and parent supply squash/stretch and translation once.' if combined else
           '. Animated input guide curves bend the braid; no Scatter operator or baked groom.')
    )
    return s


def main():
    scenes = [('felt_sphere_animated',build_sphere()),
              ('felt_sphere_groom_outside_xform',build_sphere(outside=True)),
              ('braid_animated_guides',braid(False)),
              ('braid_animated_guides_and_surface',braid(True))]
    for name, stage in scenes:
        path=HERE/(name+'.usda')
        stage.GetRootLayer().Export(str(path))
        print(path)


if __name__=='__main__': main()
