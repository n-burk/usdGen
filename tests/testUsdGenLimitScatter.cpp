#include "usdGen/limitSurface.h"
#include "usdGen/compiler.h"
#include "usdGen/graph.h"
#include "usdGen/opRegistry.h"
#include "usdGen/ops/scatter.h"
#include "usdGen/scheduler.h"
#include <cmath>
#include <cstdio>
using namespace usdGen;
PXR_NAMESPACE_USING_DIRECTIVE
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);return 1;}}while(false)
static UsdGenSurfaceDesc Surface() {
    UsdGenSurfaceDesc s;s.path=SdfPath("/Scalp");s.subdivisionScheme=TfToken("catmullClark");
    for(int y=-1;y<=2;++y)for(int x=-1;x<=2;++x)s.restPoints.push_back(GfVec3f(x,y,x*x+y*y));
    for(int y=0;y<3;++y)for(int x=0;x<3;++x){s.faceVertexCounts.push_back(4);for(int i:{y*4+x,y*4+x+1,(y+1)*4+x+1,(y+1)*4+x})s.faceVertexIndices.push_back(i);}
    s.points=s.restPoints;s.subsetFaces={4};return s;
}
static UsdGenGraphDesc Desc(UsdGenSurfaceDesc const& s,int level) {
    UsdGenGraphDesc d;d.description=SdfPath("/Groom");d.surfaces={s};
    UsdGenNodeDesc n;n.path=SdfPath("/Scatter");n.type=TfToken("UsdGenScatter");n.surfaces={s.path};
    n.params={{TfToken("density"),VtValue(100.0f),false},{TfToken("subdivisionLevel"),VtValue(level),false}};
    d.nodes={n};d.terminal=n.path;return d;
}
static bool Run(UsdGenGraphDesc const& d,UsdGenCurveBuffer* out,int threads=2) {
    UsdGenCompiler compiler;UsdGenGraph graph;if(!compiler.Compile(d,&graph).ok)return false;
    UsdGenScheduler scheduler(threads);UsdGenEvalContext ctx;ctx.desc=&graph.Desc();
    auto run=scheduler.Run(graph,ctx,1);if(run.diagnostics.HasErrors())return false;
    *out=graph.Output();return true;
}
int main() {
    usdGenRegisterM1Operators();auto s=Surface();std::string error;
    UsdGenLimitSurface limit;CHECK(limit.Build(s,3,&error));
    GfVec3f p,du,dv;CHECK(limit.Evaluate(4,.3f,.4f,&p,&du,&dv));
    CHECK(std::abs(p[0]-.3)<1e-5 && std::abs(p[1]-.4)<1e-5);
    // Independent analytic uniform bicubic B-spline, not cage interpolation.
    CHECK(std::abs(p[2]-(.09+.16+2.0/3))<1e-5);
    CHECK(std::abs(du[2]-.6)<1e-5 && std::abs(dv[2]-.8)<1e-5);
    UsdGenCurveBuffer a,b;CHECK(Run(Desc(s,3),&a,1));CHECK(Run(Desc(s,3),&b,4));
    CHECK(a.totalCurves>100 && a.px==b.px && a.py==b.py && a.pz==b.pz && a.curveId==b.curveId);
    for(size_t i=0;i<a.totalCurves;++i) {
        float u=a.rootUV[i][0],v=a.rootUV[i][1];
        CHECK(a.rootPrim[i]==4 && u>=0 && u<=1 && v>=0 && v<=1);
        CHECK(std::abs(a.px[i]-u)<1e-5 && std::abs(a.py[i]-v)<1e-5);
        CHECK(std::abs(a.pz[i]-(u*u+v*v+2.0/3))<2e-5);
        GfVec3f normal(-2*u,-2*v,1);normal.Normalize();
        CHECK((a.rootN[i]-normal).GetLength()<2e-5);
    }
    auto left=s;left.orientation=TfToken("leftHanded");CHECK(Run(Desc(left,3),&b));
    CHECK(a.px==b.px);for(size_t i=0;i<a.totalCurves;++i)CHECK((a.rootN[i]+b.rootN[i]).GetLength()<1e-6);
    auto holes=s;holes.holeIndices={4};CHECK(Run(Desc(holes,3),&b));CHECK(b.totalCurves==0);
    auto tags=s;tags.cornerIndices={5};tags.cornerSharpnesses={4};
    CHECK(UsdGenSubdivisionDigest(s)!=UsdGenSubdivisionDigest(tags));
    UsdGenScatterOp op;auto d0=Desc(s,3),d1=Desc(tags,3);UsdGenCaptureContext c0,c1;c0.desc=&d0;c1.desc=&d1;
    CHECK(op.CaptureDigest(c0)!=op.CaptureDigest(c1));
    // Pose and parent motion are downstream deformation inputs, not rest
    // scatter inputs. Rest/topology/UV/paint edits must still invalidate.
    d1=d0;d1.surfaces[0].points[0]+=GfVec3f(0,0,3);
    d1.surfaces[0].worldMatrix.SetTranslate(GfVec3d(2,0,0));
    d1.surfaces[0].surfaceGeneration=99;
    d1.surfaces[0].samples.push_back({42,d1.surfaces[0].points});
    CHECK(op.CaptureDigest(c0)==op.CaptureDigest(c1));
    d1.surfaces[0].restPoints[0]+=GfVec3f(0,0,1);
    CHECK(op.CaptureDigest(c0)!=op.CaptureDigest(c1));
    d1=d0;d1.surfaces[0].subsetFaces={3};
    CHECK(op.CaptureDigest(c0)!=op.CaptureDigest(c1));
    d1=d0;d1.surfaces[0].uv.push_back(GfVec2f(.2f,.3f));
    CHECK(op.CaptureDigest(c0)!=op.CaptureDigest(c1));
    d1=d0;d1.surfaces[0].densityMultiplier.assign(9,.5f);
    CHECK(op.CaptureDigest(c0)!=op.CaptureDigest(c1));
    CHECK(Run(Desc(s,0),&b));CHECK(b.totalCurves!=a.totalCurves);
    CHECK(!Run(Desc(s,7),&b));
    tags=s;tags.subdivisionScheme=TfToken("none");CHECK(!Run(Desc(tags,3),&b));
    tags=s;tags.creaseIndices={0,1};tags.creaseLengths={2};tags.creaseSharpnesses={3};CHECK(limit.Build(tags,3,&error));
    tags.creaseIndices={999,1};CHECK(!limit.Build(tags,3,&error));
    std::puts("limit scatter: analytic positions/normals, UVs, deterministic IDs, holes, orientation, tags, invalid inputs passed");
    return 0;
}
