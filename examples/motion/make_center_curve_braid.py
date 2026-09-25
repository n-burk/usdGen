"""A live braid wrapped around ONE baked particle-chain simulation (100 frames)."""
import math
from pathlib import Path
from pxr import Gf, Sdf, UsdGeom
from make_motion_examples import braid, curves


def simulate():
    # Verlet integration + length constraints. The top three particles are pinned;
    # gravity, damping and changing wind produce a hanging, swinging centerline.
    rest=[Gf.Vec3d(0,0,-.22-.02*i) for i in range(31)]
    p=list(rest); previous=list(rest)
    frames={1:list(p)}
    dt=1/(24*8)
    for frame in range(2,101):
        for substep in range(8):
            time=((frame-2)*8+substep+1)*dt
            before=list(p)
            wind=Gf.Vec3d(3.2*math.sin(2.8*time)+1.1*math.sin(6.1*time),
                         1.4*math.sin(1.9*time),-9.81)
            for i in range(3,len(p)):
                p[i]=p[i]+(p[i]-previous[i])*.996+wind*(dt*dt)
            previous=before
            for iteration in range(80):
                # Alternating sweep avoids a persistent directional solve bias.
                edges=range(len(p)-1) if iteration%2==0 else range(len(p)-2,-1,-1)
                for i in edges:
                    d=p[i+1]-p[i];length=d.GetLength()
                    w0=0 if i<3 else 1;w1=0 if i+1<3 else 1
                    if length>1e-12 and w0+w1:
                        correction=d*((length-.02)/length/(w0+w1))
                        p[i]+=correction*w0;p[i+1]-=correction*w1
                p[:3]=rest[:3]
            assert all(math.isfinite(float(v)) for point in p for v in point)
        frames[frame]=list(p)
    return rest,frames


def main():
    s=braid(False)
    # Hang from the bottom of the sphere. Transform the static growth inputs
    # themselves, keeping the entire rest/groom/simulation domain consistent.
    for path in ('/World/Motion/RestInputs/Guides','/World/Motion/RestInputs/RootSeeds'):
        prim=s.GetPrimAtPath(path)
        for name in ('points','primvars:rest'):
            attr=prim.GetAttribute(name)
            attr.Set([Gf.Vec3f(p[0],-p[1],-p[2]) for p in attr.Get()])
    s.RemovePrim('/World/Motion/Drivers')
    rest,frames=simulate()
    center=curves(s,'/World/Motion/SimulatedCenter',[rest])
    for frame,points in frames.items():
        center.GetPointsAttr().Set([Gf.Vec3f(p) for p in points],frame)
    center.GetPrim().SetDocumentation('The only animated curve: 31 particles, 3 pinned, Verlet/PBD, gravity and wind; baked at 24 fps. No braid strands are simulated or baked.')
    op=s.GetPrimAtPath('/World/Motion/Groom/Hair/Ops/guideAnimate')
    op.CreateAttribute('usdGen:mode',Sdf.ValueTypeNames.Token).Set('curveWrap')
    op.GetAttribute('usdGen:lockRoots').Set(False)
    op.RemoveProperty('usdGen:rbfSamples')
    op.GetRelationship('usdGen:guides').SetTargets([center.GetPath()])
    op.SetDocumentation('Wrap the rest-grown braid around one simulated centerline using rotation-minimizing frames. No RBF or hidden driver cage.')
    cam=s.GetPrimAtPath('/World/Camera')
    cam.GetAttribute('xformOp:transform').Set(Gf.Matrix4d().SetLookAt(
        Gf.Vec3d(.15,-3.6,1.05),Gf.Vec3d(.06,0,.18),Gf.Vec3d(0,0,1)).GetInverse())
    s.GetRootLayer().documentation='100-frame braid driven by a single simulated center curve. Static root seeds -> rest guide interpolation -> Width -> UsdGenDeform mode=curveWrap. The centerline simulation alone is baked; braid geometry remains live.'
    out=Path(__file__).with_name('braid_simulated_center_curve.usda')
    s.GetRootLayer().Export(str(out))
    error=max(abs((p[i+1]-p[i]).GetLength()-.02) for p in frames.values() for i in range(len(p)-1))
    print(out)
    print('Maximum simulation segment length error:',error)


if __name__=='__main__': main()
