#include "usdGen/cudaScatterInput.h"
#include "usdGen/opRegistry.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <string>

using namespace usdGen;
PXR_NAMESPACE_USING_DIRECTIVE
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK failed: %s (%d)\n",#x,__LINE__); return 1; } } while (false)

static UsdGenGraphDesc Desc(double density = 8.0) {
    UsdGenGraphDesc d;
    UsdGenSurfaceDesc s; s.path=SdfPath("/surface");
    s.restPoints={GfVec3f(0,0,0),GfVec3f(1,0,0),GfVec3f(1,1,0),GfVec3f(0,1,0)};
    s.uv={GfVec2f(0,0),GfVec2f(1,0),GfVec2f(1,1),GfVec2f(0,1)};
    s.faceVertexCounts={4}; s.faceVertexIndices={0,1,2,3}; d.surfaces.push_back(s);
    UsdGenNodeDesc n; n.path=SdfPath("/scatter"); n.type=TfToken("UsdGenScatter"); n.seed=41;
    n.surfaces={s.path}; n.params={{TfToken("mode"),VtValue(TfToken("random")),false},
                                    {TfToken("density"),VtValue(density),false}};
    d.nodes.push_back(n); return d;
}

static bool Direct(UsdGenGraphDesc const& d, UsdGenCurveBuffer* out) {
    UsdGenParamView p{&d,&d.nodes.front()}; UsdGenDiagnostics diag; int version=-1;
    auto op=UsdGenOpRegistry::Get().Create(TfToken("UsdGenScatter"),0,&version);
    if (!op || version!=0 || !op->Bind(p,&diag)) return false;
    auto capture=op->CreateCapture(); UsdGenCurveBuffer empty; UsdGenCaptureContext c;
    c.desc=&d; c.params=&p; c.surface=0; c.seed=uint32_t(d.nodes.front().seed); c.diag=&diag;
    if (!op->Capture(c,empty,capture.get(),&diag)) return false;
    *out=capture->Buffer();
    return true;
}

int main() {
    auto d=Desc(); UsdGenCurveBuffer direct; CHECK(Direct(d,&direct));
    std::shared_ptr<const gpu::ScatterGrowRoots> roots; std::string reason;
    CHECK(PrepareCudaScatterInput(d,SdfPath("/scatter"),&roots,&reason)==CudaScatterInputStatus::Ok && roots);
    CHECK(roots->positions.size()==direct.totalCurves && roots->stableIds.size()==direct.totalCurves);
    for(size_t i=0;i<roots->positions.size();++i) {
        CHECK(roots->positions[i].x==direct.px[i]&&roots->positions[i].y==direct.py[i]&&roots->positions[i].z==direct.pz[i]);
        CHECK(roots->stableIds[i]==direct.curveId[i]&&roots->rootPrim[i]==direct.rootPrim[i]);
        CHECK(roots->rootUV[i].x==direct.rootUV[i][0]&&roots->rootUV[i].y==direct.rootUV[i][1]);
        CHECK(roots->rootT[i].x==direct.rootT[i][0]&&roots->rootT[i].y==direct.rootT[i][1]&&roots->rootT[i].z==direct.rootT[i][2]);
        CHECK(roots->rootB[i].x==direct.rootB[i][0]&&roots->rootB[i].y==direct.rootB[i][1]&&roots->rootB[i].z==direct.rootB[i][2]);
        CHECK(roots->rootN[i].x==direct.rootN[i][0]&&roots->rootN[i].y==direct.rootN[i][1]&&roots->rootN[i].z==direct.rootN[i][2]);
    }
    auto preserved=roots; auto malformed=d; malformed.surfaces[0].faceVertexIndices.pop_back();
    CHECK(PrepareCudaScatterInput(malformed,SdfPath("/scatter"),&roots,&reason)==CudaScatterInputStatus::InvalidSurface && roots==preserved);
    auto badDensity=d; badDensity.nodes[0].params[1].value=VtValue(8);
    CHECK(PrepareCudaScatterInput(badDensity,SdfPath("/scatter"),&roots,&reason)==CudaScatterInputStatus::Unsupported && roots==preserved);
    auto badUv=d; badUv.surfaces[0].uv[2]=GfVec2f(std::numeric_limits<float>::quiet_NaN(),0);
    CHECK(PrepareCudaScatterInput(badUv,SdfPath("/scatter"),&roots,&reason)==CudaScatterInputStatus::InvalidSurface && roots==preserved);
    auto duplicateSubset=d; duplicateSubset.surfaces[0].subsetFaces={0,0};
    CHECK(PrepareCudaScatterInput(duplicateSubset,SdfPath("/scatter"),&roots,&reason)==CudaScatterInputStatus::InvalidSurface && roots==preserved);
    auto zero=Desc(0.0); roots.reset();
    CHECK(PrepareCudaScatterInput(zero,SdfPath("/scatter"),&roots,&reason)==CudaScatterInputStatus::Ok && roots && roots->positions.empty());
    auto ignoredMask = d;
    ignoredMask.nodes[0].params.push_back({TfToken("mask:amount"), VtValue(0.0f), false});
    auto emptyPreserved = roots;
    CHECK(PrepareCudaScatterInput(ignoredMask, SdfPath("/scatter"), &roots, &reason) ==
          CudaScatterInputStatus::Unsupported && roots == emptyPreserved);
    auto mapped=d; mapped.nodes[0].maps.push_back(SdfPath("/ignored"));
    CHECK(PrepareCudaScatterInput(mapped,SdfPath("/scatter"),&roots,&reason)==CudaScatterInputStatus::Unsupported);
    std::puts("testUsdGenCudaScatterInput: PASS"); return 0;
}
