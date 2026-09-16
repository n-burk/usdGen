#include "usdGen/furOcclusion.h"
#include "usdGen/generationStore.h"
#include "usdGenImaging/usdGenTilePublisher.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdio>
#include <stdexcept>

using namespace usdGen;
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void Require(bool b,char const* msg) { if(!b) throw std::runtime_error(msg); }
UsdGenPlane const& Plane(UsdGenTilePublication const& tile,char const* name) {
    for(auto const& p:tile.extraUniform) if(p.name==name) return p;
    throw std::runtime_error("missing optical depth plane");
}
UsdGenTilePublication Make(int id,float x,int strands) {
    UsdGenTilePublication t;t.tile=static_cast<UsdGenTileId>(id);t.xformMatrix.SetIdentity();
    t.primPath=SdfPath("/fur/tile_"+std::to_string(id));
    for(int i=0;i<strands;++i) {
        float z=float(i%20)/100-0.1f;
        float dx=float(i/20)/100;
        t.points.push_back(GfVec3f(x+dx,0,z));
        t.points.push_back(GfVec3f(x+dx,1,z));
        t.curveVertexCounts.push_back(2);
    }
    t.widths=VtFloatArray{0.01f};return t;
}
}
int main() try {
    std::vector<UsdGenTilePublication> tiles{Make(0,0,1),Make(1,0.2f,400)};
    Require(UsdGenBuildFurOcclusion(&tiles),"initial bake");
    auto const px=Plane(tiles[0],"furTauP").f[0];
    auto const nx=Plane(tiles[0],"furTauN").f[0];
    Require(px>nx+0.2f,"neighboring tile shadows receiver from +X");
    Require(Plane(tiles[0],"furTauP").f[1]<0.001f,"parallel fibers have zero projected area");
    for(auto const& tile:tiles) for(auto const& plane:tile.extraUniform) {
        Require(plane.f.size()==tile.points.size()*3,"vertex cardinality");
        for(float x:plane.f) Require(std::isfinite(x)&&x>=0&&x<=32,"finite bounded depth");
    }
    auto prior=tiles;
    Require(!UsdGenBuildFurOcclusion(&tiles,&prior),"no-op uses cached bake");
    Require(Plane(tiles[0],"furTauP").f.IsIdentical(Plane(prior[0],"furTauP").f),"COW cache sharing");
    tiles[1].widths=VtFloatArray{0.02f};
    Require(UsdGenBuildFurOcclusion(&tiles,&prior),"width edit invalidates all receivers");
    Require(Plane(tiles[0],"furTauP").f[0]>px*1.5f,"thicker neighbors increase depth");
    auto moved=prior;
    moved[1].xformMatrix.SetTranslate(GfVec3d(-1,0,0));
    Require(UsdGenBuildFurOcclusion(&moved,&prior),"transform edit invalidates bake");
    Require(Plane(moved[0],"furTauN").f[0]>Plane(moved[0],"furTauP").f[0],
        "moving occluder to -X reverses shadow direction");
    UsdGenGeneration a,b;a.tiles=prior;b.tiles=tiles;
    auto report=UsdGenGenerationStore().Diff(a,b);
    Require(!report.tiles[0].pointsDirty,"shadow update does not dirty receiver points");
    Require(std::find(report.tiles[0].dirtyPrimvars.begin(),report.tiles[0].dirtyPrimvars.end(),TfToken("furTauP"))!=report.tiles[0].dirtyPrimvars.end(),"neighbor update dirties receiver optical depth");
    auto ds=usdGenImaging::UsdGenTilePublisher::BuildTileDataSource(tiles[0]);
    auto pv=HdPrimvarsSchema::GetFromParent(ds).GetPrimvar(TfToken("furTauP"));
    Require(pv.IsDefined()&&pv.GetInterpolation()->GetTypedValue(0)==TfToken("vertex"),"Hydra vertex transport");
    Require(pv.GetPrimvarValue()->GetValue(0).IsHolding<VtVec3fArray>(),"packed vec3 GPU transport");
    tiles.resize(1);
    UsdGenBuildFurOcclusion(&tiles,&prior);
    Require(Plane(tiles[0],"furTauP").f[0]<px,"removed occluder clears shadow");
    tiles[0].points[0][0]=std::numeric_limits<float>::quiet_NaN();
    bool rejected=false;try { UsdGenBuildFurOcclusion(&tiles); } catch(std::invalid_argument const&) {rejected=true;}
    Require(rejected,"reject nonfinite geometry");
    std::vector<UsdGenTilePublication> bench{Make(0,0,50000)};
    auto start=std::chrono::steady_clock::now();
    UsdGenBuildFurOcclusion(&bench);
    auto end=std::chrono::steady_clock::now();
    std::printf("PASS fur optical depth; 50k strands / 100k CV: %.2f ms bake\n",
        std::chrono::duration<double,std::milli>(end-start).count());
    return 0;
} catch(std::exception const& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
