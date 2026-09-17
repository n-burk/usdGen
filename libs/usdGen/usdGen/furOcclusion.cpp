#include "usdGen/furOcclusion.h"

#include "usdGen/scheduler.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/trace/trace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

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
    // The eight cells and trilinear weights around p. Gather and scatter use
    // precisely the same cell-centre convention.
    void Corners(GfVec3f const& p, int base[3], float frac[3]) const {
        auto q = (p-origin)/h-GfVec3f(0.5f);
        for (int k=0;k<3;++k) {
            q[k]=std::clamp(q[k],0.f,float(n-1));
            base[k]=std::min(int(q[k]),n-2); frac[k]=q[k]-base[k];
        }
    }
};

// Runs body(i) for i in [0, count) on the dispatcher's arena, or serially.
template <class F>
void ForEach(UsdGenWorkDispatcher* dispatcher, size_t count, F const& body) {
    if (!dispatcher || count < 2) {
        for (size_t i = 0; i < count; ++i) body(i);
        return;
    }
    struct Payload { F const* body; } payload{&body};
    dispatcher->ParallelFor(count, [](size_t i, void* p) {
        (*static_cast<Payload*>(p)->body)(i);
    }, &payload);
}

// A tile's contribution to the density grid, over the sub-box of cells its
// samples touch; tiles are splatted in parallel and summed serially.
struct TileDensity {
    int lo[3] = {0, 0, 0}, dims[3] = {0, 0, 0};
    std::vector<GfVec3f> density;
};
}

bool UsdGenBuildFurOcclusion(std::vector<UsdGenTilePublication>* tiles,
    std::vector<UsdGenTilePublication> const* previous, int resolution,
    UsdGenWorkDispatcher* dispatcher)
{
    TRACE_FUNCTION();
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
    size_t const tileCount = tiles->size();

    // --- world-space positions and bounds, per tile ---------------------------
    std::vector<std::vector<GfVec3f>> positions(tileCount);
    std::vector<GfVec3f> tileLo(tileCount, GfVec3f(std::numeric_limits<float>::max()));
    std::vector<GfVec3f> tileHi(tileCount, GfVec3f(-std::numeric_limits<float>::max()));
    std::vector<char const*> invalid(tileCount, nullptr);
    {
        TRACE_SCOPE("usdGen occlusion: positions");
        ForEach(dispatcher, tileCount, [&](size_t t) {
            auto const& tile=(*tiles)[t];
            size_t count=0;
            for(int c:tile.curveVertexCounts) {
                if(c<0) { invalid[t]="negative fur curve vertex count"; return; }
                count+=size_t(c);
            }
            if(count!=tile.points.size() || (!tile.widths.empty() &&
                tile.widths.size()!=1 && tile.widths.size()!=count)) {
                invalid[t]="invalid fur points/widths cardinality";
                return;
            }
            GfMatrix4f const m(tile.xformMatrix);
            bool const identity = tile.xformMatrix == GfMatrix4d(1.0);
            auto& p=positions[t]; p.resize(count);
            GfVec3f lo=tileLo[t], hi=tileHi[t];
            for(size_t i=0;i<count;++i) {
                GfVec3f const w = identity ? tile.points[i] : m.Transform(tile.points[i]);
                for(int k=0;k<3;++k) {
                    if(!std::isfinite(w[k])) { invalid[t]="non-finite fur point"; return; }
                    lo[k]=std::min(lo[k],w[k]); hi[k]=std::max(hi[k],w[k]);
                }
                p[i]=w;
            }
            tileLo[t]=lo; tileHi[t]=hi;
        });
    }
    GfVec3f lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    size_t pointCount=0;
    for(size_t t=0;t<tileCount;++t) {
        if(invalid[t]) throw std::invalid_argument(invalid[t]);
        pointCount+=positions[t].size();
        if(positions[t].empty()) continue;
        for(int k=0;k<3;++k) { lo[k]=std::min(lo[k],tileLo[t][k]); hi[k]=std::max(hi[k],tileHi[t][k]); }
    }
    if(!pointCount) return true;
    auto span=hi-lo;
    for(int k=0;k<3;++k) if(!std::isfinite(span[k]))
        throw std::invalid_argument("fur bounds exceed finite grid range");
    float h=std::max({span[0],span[1],span[2],1e-6f})/float(resolution-4);
    Grid grid{resolution,lo-GfVec3f(2*h),h};
    size_t cells=size_t(resolution)*resolution*resolution;

    // --- splat projected fibre area, per tile into its own sub-box -------------
    std::vector<GfVec3f> density(cells,GfVec3f(0));
    {
        TRACE_SCOPE("usdGen occlusion: splat");
        std::vector<TileDensity> local(tileCount);
        ForEach(dispatcher, tileCount, [&](size_t t) {
            auto const& tile=(*tiles)[t]; auto const& p=positions[t];
            if(p.empty()) return;
            TileDensity& out=local[t];
            int b0[3], b1[3]; float f[3];
            grid.Corners(tileLo[t],b0,f);
            grid.Corners(tileHi[t],b1,f);
            for(int k=0;k<3;++k) { out.lo[k]=b0[k]; out.dims[k]=b1[k]+2-b0[k]; }
            out.density.assign(size_t(out.dims[0])*out.dims[1]*out.dims[2],GfVec3f(0));
            auto localIndex=[&](int x,int y,int z) {
                return (size_t(z-out.lo[2])*out.dims[1]+(y-out.lo[1]))*out.dims[0]+(x-out.lo[0]);
            };
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
                        int c[3]; float w[3];
                        grid.Corners(p[a]+u*d,c,w);
                        for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
                            float const weight=(x?w[0]:1-w[0])*(y?w[1]:1-w[1])*(z?w[2]:1-w[2]);
                            out.density[localIndex(c[0]+x,c[1]+y,c[2]+z)]+=projection*(weight*area);
                        }
                    }
                }
                base+=count;
            }
        });
        for(TileDensity const& tile:local) {
            size_t i=0;
            for(int z=0;z<tile.dims[2];++z) for(int y=0;y<tile.dims[1];++y)
                for(int x=0;x<tile.dims[0];++x,++i)
                    density[grid.Index(tile.lo[0]+x,tile.lo[1]+y,tile.lo[2]+z)]+=tile.density[i];
        }
    }

    // --- one sweep per axis/sign ---------------------------------------------
    // Prefix integration is O(voxels), independent of hair count and number
    // of lights. Exclude half the receiver cell.
    std::array<std::vector<float>,6> tau;
    {
        TRACE_SCOPE("usdGen occlusion: sweep");
        ForEach(dispatcher, 6, [&](size_t direction) {
            int const axis=int(direction)/2;
            auto& out=tau[direction];
            out.assign(cells,0.f);
            for(int v=0;v<resolution;++v) for(int u=0;u<resolution;++u) {
                float sum=0;
                for(int step=0;step<resolution;++step) {
                    int c[3]; c[axis]=(direction%2)?step:resolution-1-step;
                    c[(axis+1)%3]=u;c[(axis+2)%3]=v;
                    size_t idx=grid.Index(c[0],c[1],c[2]);
                    float depth=density[idx][axis];
                    out[idx]=std::min(32.f,sum+0.5f*depth);sum+=depth;
                }
            }
        });
    }

    // --- gather: every receiver reads its six depths once --------------------
    {
        TRACE_SCOPE("usdGen occlusion: gather");
        for(auto& tile:*tiles) {
            auto& out=tile.extraUniform;
            out.erase(std::remove_if(out.begin(),out.end(),[](auto const& p){return Reserved(p.name);}),out.end());
            for(auto name:names) {
                UsdGenPlane plane;plane.name=TfToken(name);plane.interpolation=TfToken("vertex");
                plane.type=TfToken("float");plane.arity=3;plane.f.resize(tile.points.size()*3);
                out.push_back(std::move(plane));
            }
            std::sort(out.begin(),out.end(),[](auto const& a,auto const& b){return a.name<b.name;});
        }
        ForEach(dispatcher, tileCount, [&](size_t t) {
            auto& out=(*tiles)[t].extraUniform;
            auto plane=[&](char const* name) -> float* {
                auto it=std::find_if(out.begin(),out.end(),[&](auto const& p){return p.name==name;});
                return it->f.data();   // detaches this tile's own new plane
            };
            // names[0] (P) holds the even directions, names[1] (N) the odd.
            float* const planes[2]={plane(names[0]),plane(names[1])};
            auto const& p=positions[t];
            for(size_t i=0;i<p.size();++i) {
                int c[3]; float w[3];
                grid.Corners(p[i],c,w);
                float values[6]={0,0,0,0,0,0};
                for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
                    float const weight=(x?w[0]:1-w[0])*(y?w[1]:1-w[1])*(z?w[2]:1-w[2]);
                    size_t const idx=grid.Index(c[0]+x,c[1]+y,c[2]+z);
                    for(int d=0;d<6;++d) values[d]+=weight*tau[d][idx];
                }
                for(int d=0;d<6;++d) planes[d%2][i*3+d/2]=values[d];
            }
        });
    }
    return true;
}
}
