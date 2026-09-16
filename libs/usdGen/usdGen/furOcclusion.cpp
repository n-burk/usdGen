#include "usdGen/furOcclusion.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace usdGen {
namespace {
constexpr char const* names[] = {"furTauP", "furTauN"};
bool Reserved(TfToken const& name) {
    for (auto n : names) if (name == n) return true;
    return false;
}
struct Grid {
    int n;
    GfVec3f origin;
    float h;
    size_t Index(int x, int y, int z) const { return (size_t(z)*n+y)*n+x; }
    // Trilinear gather/scatter use precisely the same cell-center convention.
    template<class F> void Weights(GfVec3f const& p, F&& f) const {
        auto q = (p-origin)/h-GfVec3f(0.5f);
        int b[3]; float a[3];
        for (int k=0;k<3;++k) {
            q[k]=std::clamp(q[k],0.f,float(n-1));
            b[k]=std::min(int(q[k]),n-2); a[k]=q[k]-b[k];
        }
        for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x)
            f(Index(b[0]+x,b[1]+y,b[2]+z),
                (x?a[0]:1-a[0])*(y?a[1]:1-a[1])*(z?a[2]:1-a[2]));
    }
};
}

bool UsdGenBuildFurOcclusion(std::vector<UsdGenTilePublication>* tiles,
    std::vector<UsdGenTilePublication> const* previous, int resolution)
{
    if (!tiles) return false;
    bool same = previous && previous->size()==tiles->size();
    if (same) for(size_t i=0;i<tiles->size();++i) {
        auto const& a=(*tiles)[i]; auto const& b=(*previous)[i];
        if(a.tile!=b.tile || !a.points.IsIdentical(b.points) ||
           !a.widths.IsIdentical(b.widths) || a.curveVertexCounts!=b.curveVertexCounts ||
           a.xformMatrix!=b.xformMatrix) { same=false; break; }
        for(auto name:names) {
            auto it=std::find_if(b.extraUniform.begin(),b.extraUniform.end(),
                [&](auto const& plane){return plane.name==name;});
            if(it==b.extraUniform.end() || it->arity!=3 || it->f.size()!=b.points.size()*3)
                same=false;
        }
    }
    if (same) {
        for(size_t i=0;i<tiles->size();++i) {
            auto& dst=(*tiles)[i].extraUniform;
            dst.erase(std::remove_if(dst.begin(),dst.end(),
                [](auto const& p){return Reserved(p.name);}),dst.end());
            for(auto const& p:(*previous)[i].extraUniform)
                if(Reserved(p.name)) dst.push_back(p);
            std::sort(dst.begin(),dst.end(),[](auto const& a,auto const& b){return a.name<b.name;});
        }
        return false;
    }
    resolution=std::clamp(resolution,8,128);
    std::vector<std::vector<GfVec3f>> positions(tiles->size());
    GfVec3f lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    size_t pointCount=0;
    for(size_t t=0;t<tiles->size();++t) {
        auto const& tile=(*tiles)[t];
        size_t count=0;
        for(int c:tile.curveVertexCounts) {
            if(c<0) throw std::invalid_argument("negative fur curve vertex count");
            count+=size_t(c);
        }
        if(count!=tile.points.size() || (!tile.widths.empty() &&
            tile.widths.size()!=1 && tile.widths.size()!=count))
            throw std::invalid_argument("invalid fur points/widths cardinality");
        auto& p=positions[t]; p.reserve(count); pointCount+=count;
        for(auto const& v:tile.points) {
            GfVec3f w(tile.xformMatrix.Transform(GfVec3d(v)));
            for(int k=0;k<3;++k) {
                if(!std::isfinite(w[k])) throw std::invalid_argument("non-finite fur point");
                lo[k]=std::min(lo[k],w[k]); hi[k]=std::max(hi[k],w[k]);
            }
            p.push_back(w);
        }
    }
    if(!pointCount) return true;
    auto span=hi-lo;
    for(int k=0;k<3;++k) if(!std::isfinite(span[k]))
        throw std::invalid_argument("fur bounds exceed finite grid range");
    float h=std::max({span[0],span[1],span[2],1e-6f})/float(resolution-4);
    Grid grid{resolution,lo-GfVec3f(2*h),h};
    size_t cells=size_t(resolution)*resolution*resolution;
    std::vector<GfVec3f> density(cells,GfVec3f(0));
    for(size_t t=0;t<tiles->size();++t) {
        auto const& tile=(*tiles)[t]; auto const& p=positions[t];
        // Conservative cross-section scale under nonuniform transforms.
        float scale=0;
        for(int k=0;k<3;++k) { GfVec3d axis(0);axis[k]=1;
            scale=std::max(scale,float(tile.xformMatrix.TransformDir(axis).GetLength())); }
        auto width=[&](size_t i) { float w=tile.widths.empty()?0.f:
            tile.widths[tile.widths.size()==1?0:i];
            return std::isfinite(w)?std::max(0.f,w)*scale:0.f; };
        size_t base=0;
        for(int count:tile.curveVertexCounts) {
            for(int j=1;j<count;++j) {
                size_t a=base+j-1,b=a+1; auto d=p[b]-p[a];
                float len=d.GetLength(); if(len<1e-12f) continue;
                auto tangent=d/len;
                int steps=std::max(1,int(std::ceil(2*len/h)));
                GfVec3f projection;
                for(int k=0;k<3;++k) projection[k]=std::sqrt(std::max(0.f,1-tangent[k]*tangent[k]));
                for(int s=0;s<steps;++s) {
                    float u=(s+0.5f)/steps;
                    float area=((1-u)*width(a)+u*width(b))*len/(steps*h*h);
                    grid.Weights(p[a]+u*d,[&](size_t cell,float w) {density[cell]+=projection*(w*area);});
                }
            }
            base+=count;
        }
    }
    // One sweep per axis/sign. Prefix integration is O(voxels), independent
    // of hair count and number of lights. Exclude half the receiver cell.
    for(auto& tile:*tiles) {
        auto& out=tile.extraUniform;
        out.erase(std::remove_if(out.begin(),out.end(),[](auto const& p){return Reserved(p.name);}),out.end());
        for(auto name:names) {
            UsdGenPlane plane;plane.name=TfToken(name);plane.interpolation=TfToken("vertex");
            plane.type=TfToken("float");plane.arity=3;plane.f.resize(tile.points.size()*3);
            out.push_back(std::move(plane));
        }
    }
    std::vector<float> tau(cells);
    for(int direction=0;direction<6;++direction) {
        int axis=direction/2;
        for(int v=0;v<resolution;++v) for(int u=0;u<resolution;++u) {
            float sum=0;
            for(int step=0;step<resolution;++step) {
                int c[3]; c[axis]=(direction%2)?step:resolution-1-step;
                c[(axis+1)%3]=u;c[(axis+2)%3]=v;
                size_t idx=grid.Index(c[0],c[1],c[2]);
                float depth=density[idx][axis];
                tau[idx]=std::min(32.f,sum+0.5f*depth);sum+=depth;
            }
        }
        for(size_t t=0;t<tiles->size();++t) {
            auto& out=(*tiles)[t].extraUniform;
            auto& plane=*std::find_if(out.begin(),out.end(),[&](auto const& p){return p.name==names[direction%2];});
            for(size_t i=0;i<positions[t].size();++i) {
                float value=0;
                grid.Weights(positions[t][i],[&](size_t cell,float w){value+=w*tau[cell];});
                plane.f[i*3+axis]=value;
            }
        }
    }
    for(auto& tile:*tiles) std::sort(tile.extraUniform.begin(),tile.extraUniform.end(),
        [](auto const& a,auto const& b){return a.name<b.name;});
    return true;
}
}
