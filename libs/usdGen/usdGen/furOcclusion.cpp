#include "usdGen/furOcclusion.h"

#include "usdGen/debugCodes.h"
#include "usdGen/digest.h"
#include "usdGen/scheduler.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/trace/trace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace usdGen {
namespace {
constexpr char const* names[] = {"furTauP", "furTauN"};
// Samples per voxel along a curve; the trilinear splat carries the rest, and
// the deposited area is the sub-sample length, so this changes where density
// lands, never how much of it there is.
constexpr float kSamplesPerVoxel = 1.0f;
constexpr int   kMaxSegmentSteps = 512;
// Published depth is gathered from a fixed-point grid, as a host renderer's voxel pages
// are (24-bit hair count at x1000): half the bytes of the eight cells every CV
// touches, and the same quantisation the engine it is matched to uses.
constexpr float kDepthScale = 1000.f;
// The legacy cube resolution, kept as the coarsest voxel the derived grid may
// pick so a small swatch never loses detail to the world-size target.
constexpr int   kLegacyResolution = 48;
// One tile is far too coarse and far too uneven a unit of work for a groom
// whose tiles differ by an order of magnitude in curve count.
constexpr uint32_t kJobCurves = 4096;
constexpr size_t   kJobCvs    = 16384;

bool Reserved(TfToken const& name) {
    for (auto n : names) if (name == n) return true;
    return false;
}

struct Grid {
    int n[3] = {0, 0, 0};
    GfVec3f origin;
    float h = 1.f, invH = 1.f;
    size_t Index(int x, int y, int z) const { return (size_t(z)*n[1]+y)*n[0]+x; }
    size_t Cells() const { return size_t(n[0])*n[1]*n[2]; }
    // The eight cells and trilinear weights around p. Gather and scatter use
    // precisely the same cell-centre convention.
    void Corners(GfVec3f const& p, int base[3], float frac[3]) const {
        for (int k=0;k<3;++k) {
            float q=std::clamp((p[k]-origin[k])*invH-0.5f,0.f,float(n[k]-1));
            base[k]=std::min(int(q),n[k]-2); frac[k]=q-base[k];
        }
    }
    bool Contains(GfVec3f const& p) const {
        for (int k=0;k<3;++k) {
            float const q=(p[k]-origin[k])*invH;
            if (!(q >= 0.f && q <= float(n[k]))) return false;
        }
        return true;
    }
};

// Uniform cubic basis weights. Both bases are evaluated with Hydra's pinned
// convention: the end control points are extrapolated (2*P0 - P1) so the curve
// interpolates the first and last CV, which is the geometry Storm draws.
inline void CubicWeights(bool catmullRom, float u, float w[4]) {
    float const u2 = u*u, u3 = u2*u;
    if (catmullRom) {
        w[0] = 0.5f*(-u3 + 2*u2 - u);
        w[1] = 0.5f*(3*u3 - 5*u2 + 2);
        w[2] = 0.5f*(-3*u3 + 4*u2 + u);
        w[3] = 0.5f*(u3 - u2);
    } else {
        float const v = 1.f-u;
        w[0] = v*v*v*(1.f/6.f);
        w[1] = (3*u3 - 6*u2 + 4)*(1.f/6.f);
        w[2] = (-3*u3 + 3*u2 + 3*u + 1)*(1.f/6.f);
        w[3] = u3*(1.f/6.f);
    }
}

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

/// A contiguous run of curves of one tile: the unit of parallel work for both
/// the splat and the gather.
struct Job {
    uint32_t tile = 0;
    uint32_t firstCurve = 0, curveCount = 0;
    size_t   firstCv = 0, cvCount = 0;
};

// A job's contribution to the density grid, over the sub-box of cells its
// samples touch; jobs are splatted in parallel and summed over disjoint slabs.
struct JobDensity {
    int lo[3] = {0, 0, 0}, dims[3] = {0, 0, 0};
    std::vector<GfVec3f> density;
};

// One world-space occluder triangle. The direction that points into the solid
// is resolved once for the whole mesh set. The corner ids index a per-bake
// vertex array, which is what lets the scalp cap carry smooth normals: a face
// normal would band the shadow along every edge of the scalp mesh.
struct Triangle { GfVec3f a, b, c, normal; uint32_t ia, ib, ic; };

// Triangles with less hair than this over them are dropped, so bare skin is
// untouched and the translucent pass stays small. It has to be small: dropping
// a triangle is a step in the shadow, and transmittance falls steeply from
// zero crossings (a_f is around 0.2 for a dark coat, so 0.05 crossings is
// already 6% of the light), which at a face's size reads as a hard polygonal
// edge across the skin. At 0.002 the step is a quarter of one 8-bit level.
constexpr float kScalpShadowFloor = 0.002f;
// The taps the scalp shader weights the sky with: the normal plus a ring at
// 60 degrees. The cull below has to ask the same question the shader will.
constexpr int   kScalpRing = 4;
constexpr float kScalpRingCos = 0.5f;

/// The consumer's reconstruction, mirrored: a normalised cosine-power-4 blend
/// of the six stored depths. `values` is +X,-X,+Y,-Y,+Z,-Z.
inline float DirectionalDepth(float const values[6], GfVec3f const& dir)
{
    float sum=0, weighted=0;
    for(int k=0;k<3;++k) {
        float w=dir[k]*dir[k]; w*=w;
        weighted+=w*(dir[k]>=0.f?values[k*2]:values[k*2+1]);
        sum+=w;
    }
    return sum>0.f ? weighted/sum : 0.f;
}

/// How much hair a cosine-weighted sky sees from a point with normal `n`. This
/// is what decides whether a patch of skin is under hair at all: the depth
/// toward any one axis is not, because for a point anywhere on a head the
/// horizontal axes run through the hair mass on the far side.
inline float HemisphereDepth(float const values[6], GfVec3f const& n)
{
    GfVec3f up(0,0,1);
    if(std::fabs(n[2])>0.9f) up=GfVec3f(1,0,0);
    GfVec3f t=GfCross(n,up);
    float length=t.GetLength();
    t = length>1e-6f ? t/length : GfVec3f(1,0,0);
    GfVec3f const b=GfCross(n,t);
    float const sinRing=std::sqrt(std::max(0.f,1.f-kScalpRingCos*kScalpRingCos));
    float weighted=DirectionalDepth(values,n), sum=1.f;
    for(int tap=0;tap<kScalpRing;++tap) {
        float const phi=float(tap)*(6.2831853f/float(kScalpRing));
        GfVec3f const dir=n*kScalpRingCos+
            (t*std::cos(phi)+b*std::sin(phi))*sinRing;
        weighted+=kScalpRingCos*DirectionalDepth(values,dir);
        sum+=kScalpRingCos;
    }
    return weighted/sum;
}

/// The scalp-shadow cap. Each occluder triangle is refined k x k, every
/// sub-vertex takes the hair-only depth one voxel out along the surface normal
/// (far enough that the opaque shell two voxels IN cannot reach it), and
/// sub-triangles with no hair over them are dropped.
///
/// One k for the whole mesh, so shared edges are split identically and the cap
/// has no T-junctions: adaptive per face would crack.
template <class Gather>
void BuildScalpShadow(std::vector<Triangle> const& triangles,
                      std::vector<GfVec3f> const& vertexNormals, float inward,
                      Grid const& grid, int maxTriangles, Gather const& gather,
                      UsdGenWorkDispatcher* dispatcher,
                      UsdGenScalpShadowPublication* out)
{
    // The 90th-percentile edge decides the level, so one oversized face cannot
    // refine the whole mesh; the triangle ceiling then bounds it outright.
    int k = 1;
    { TRACE_SCOPE("usdGen scalp: edges");
    std::vector<float> edges;
    edges.reserve(triangles.size());
    for(Triangle const& t:triangles)
        edges.push_back(std::max({(t.b-t.a).GetLength(),(t.c-t.b).GetLength(),
                                  (t.a-t.c).GetLength()}));
    auto at=edges.begin()+std::min(edges.size()-1,size_t(0.9*double(edges.size())));
    std::nth_element(edges.begin(),at,edges.end());
    k=std::max(1,int(std::ceil(*at*grid.invH)));
    int const ceiling=std::max(1,maxTriangles);
    while(k>1 && triangles.size()*size_t(k)*size_t(k) > size_t(ceiling)) --k;
    }

    float const outward=-inward;
    // Sample where the cap is DRAWN, not a voxel out along the normal. The
    // outward probe this used to take was guarding against the opaque shell
    // two voxels in -- but the cap is gathered from the HAIR-ONLY sweep, run
    // before the shell is injected, so there was nothing there to avoid. What
    // it did instead was skip the voxel the roots sit in, which is the densest
    // one, and cost the cap almost exactly one crossing: measured in-render
    // against a strand CV at the same point and direction, the cap read 1.53
    // where the strand read 2.51, i.e. the strand's count after a host renderer's
    // self-shift -- the very shift a skin receiver must not apply.
    float const probe=0.f;
    // A hair's breadth of separation, so the cap wins the depth test against
    // the skin it sits on. glslfx has no polygon offset, and at this scale the
    // margin is still an order of magnitude above the depth buffer's
    // resolution. Kept small because the lift also pushes the cap's silhouette
    // outside the skin's; the shader fades it at grazing angles for the rest.
    float const lift=0.06f*grid.h;
    size_t const perTriangle=size_t(k+1)*size_t(k+2)/2;

    // Triangles are independent work: shared edges are split identically but
    // vertices are never shared across triangles, so the build fans out over
    // contiguous triangle chunks with thread-local outputs, merged in chunk
    // order. The merged arrays match the serial emission order exactly, and
    // no floating-point value crosses a chunk boundary, so the cap, extent,
    // and digest are bit-identical to the serial build.
    struct ChunkOut {
        VtVec3fArray points, normals;
        VtFloatArray tauP, tauN;
        VtIntArray counts, indices;
    };
    size_t const nChunks = triangles.empty() ? 1 : std::max<size_t>(1,
        std::min<size_t>(triangles.size(), size_t(dispatcher ?
            std::max(1, dispatcher->MaxConcurrency()) : 1)));
    std::vector<ChunkOut> chunks(nChunks);
    { TRACE_SCOPE("usdGen scalp: refine");
    ForEach(dispatcher, nChunks, [&](size_t chunk) {
        size_t const first=(chunk*triangles.size())/nChunks;
        size_t const last=((chunk+1)*triangles.size())/nChunks;
        ChunkOut& c=chunks[chunk];
        c.points.reserve((last-first)*perTriangle);
        std::vector<GfVec3f> local(perTriangle), localN(perTriangle);
        std::vector<float> depth(perTriangle*6), shaded(perTriangle);
        std::vector<int> remap(perTriangle);
        for(size_t ti=first;ti<last;++ti) {
            Triangle const& tri=triangles[ti];
            GfVec3f const na=vertexNormals[tri.ia]*outward;
            GfVec3f const nb=vertexNormals[tri.ib]*outward;
            GfVec3f const nc=vertexNormals[tri.ic]*outward;
            GfVec3f const flat=tri.normal*outward;
            GfVec3f const e1=tri.b-tri.a, e2=tri.c-tri.a;
            size_t index=0;
            bool any=false;
            for(int i=0;i<=k;++i) for(int j=0;i+j<=k;++j,++index) {
                float const u=float(i)/float(k), v=float(j)/float(k);
                GfVec3f const on=tri.a+e1*u+e2*v;
                GfVec3f n=na*(1.f-u-v)+nb*u+nc*v;
                float const length=n.GetLength();
                n = length>1e-6f ? n/length : flat;
                local[index]=on; localN[index]=n;
                gather(on+n*(probe+lift),&depth[index*6]);
                shaded[index]=HemisphereDepth(&depth[index*6],n);
                if(shaded[index]>kScalpShadowFloor) any=true;
            }
            if(!any) continue;
            // (i, j) -> the lattice slot, walking i in rows of decreasing length.
            auto slot=[&](int i,int j) {
                int base=0;
                for(int r=0;r<i;++r) base+=k+1-r;
                return base+j;
            };
            std::fill(remap.begin(),remap.end(),-1);
            auto emit=[&](int i,int j)->int {
                int const s=slot(i,j);
                if(remap[size_t(s)]<0) {
                    remap[size_t(s)]=int(c.points.size());
                    c.points.push_back(local[size_t(s)]+localN[size_t(s)]*lift);
                    c.normals.push_back(localN[size_t(s)]);
                    for(int d=0;d<3;++d) {
                        c.tauP.push_back(depth[size_t(s)*6+d*2]);
                        c.tauN.push_back(depth[size_t(s)*6+d*2+1]);
                    }
                }
                return remap[size_t(s)];
            };
            auto face=[&](int i0,int j0,int i1,int j1,int i2,int j2) {
                int const t0=slot(i0,j0), t1=slot(i1,j1), t2=slot(i2,j2);
                bool lit=false;
                for(int s:{t0,t1,t2})
                    if(shaded[size_t(s)]>kScalpShadowFloor) lit=true;
                if(!lit) return;
                c.counts.push_back(3);
                c.indices.push_back(emit(i0,j0));
                c.indices.push_back(emit(i1,j1));
                c.indices.push_back(emit(i2,j2));
            };
            for(int i=0;i<k;++i) for(int j=0;i+j<k;++j) {
                face(i,j, i+1,j, i,j+1);
                if(i+j+2<=k) face(i+1,j, i+1,j+1, i,j+1);
            }
        }
    });
    } /* scalp: refine */
    VtVec3fArray points, normals;
    VtFloatArray tauP, tauN;
    VtIntArray counts, indices;
    { TRACE_SCOPE("usdGen scalp: merge");
        size_t nPoints=0, nTau=0, nCounts=0, nIndices=0;
        for(ChunkOut const& c:chunks) {
            nPoints+=c.points.size(); nTau+=c.tauP.size();
            nCounts+=c.counts.size(); nIndices+=c.indices.size();
        }
        points.resize(nPoints); normals.resize(nPoints);
        tauP.resize(nTau); tauN.resize(nTau);
        counts.resize(nCounts); indices.resize(nIndices);
        size_t oPoints=0, oTau=0, oCounts=0, oIndices=0;
        for(ChunkOut& c:chunks) {
            if(!c.points.empty()) {
                std::memcpy(&points[oPoints],c.points.cdata(),
                            c.points.size()*sizeof(GfVec3f));
                std::memcpy(&normals[oPoints],c.normals.cdata(),
                            c.normals.size()*sizeof(GfVec3f));
            }
            if(!c.tauP.empty()) {
                std::memcpy(&tauP[oTau],c.tauP.cdata(),
                            c.tauP.size()*sizeof(float));
                std::memcpy(&tauN[oTau],c.tauN.cdata(),
                            c.tauN.size()*sizeof(float));
            }
            if(!c.counts.empty())
                std::memcpy(&counts[oCounts],c.counts.cdata(),
                            c.counts.size()*sizeof(int));
            int const base=int(oPoints);
            for(size_t i=0;i<c.indices.size();++i)
                indices[oIndices+i]=c.indices[i]+base;
            oPoints+=c.points.size(); oTau+=c.tauP.size();
            oCounts+=c.counts.size(); oIndices+=c.indices.size();
            c=ChunkOut();
        }
    }
    out->points=points; out->normals=normals;
    out->faceVertexCounts=counts; out->faceVertexIndices=indices;
    out->extraUniform.clear();
    if(!points.empty()) {
        for(int which=0;which<2;++which) {
            UsdGenPlane plane;
            plane.name=TfToken(names[which]);
            plane.interpolation=TfToken("vertex");
            plane.type=TfToken("float");
            plane.arity=3;
            plane.f=which?tauN:tauP;
            out->extraUniform.push_back(std::move(plane));
        }
        { TRACE_SCOPE("usdGen scalp: extent");
        GfVec3f lo=points[0], hi=points[0];
        for(GfVec3f const& p:points) for(int d=0;d<3;++d) {
            lo[d]=std::min(lo[d],p[d]); hi[d]=std::max(hi[d],p[d]);
        }
        out->extentMin=GfVec3d(lo); out->extentMax=GfVec3d(hi);
        }
    }
    // The cap digest is presentation identity (the scene index dirties the
    // prim on it alone): same coverage and feed order as the byte-at-a-time
    // FNV-1a it replaces, hashed with the shared 4-lane word mixer whose
    // contract names cook digests explicitly. Values change; the digest is
    // in-memory equality-only (stability/sensitivity tested, never golden).
    uint64_t digest = UsdGenDigestOffset;
    { TRACE_SCOPE("usdGen scalp: digest");
    digest = UsdGenDigestBytes(points.cdata(),points.size()*sizeof(GfVec3f),digest);
    digest = UsdGenDigestBytes(normals.cdata(),normals.size()*sizeof(GfVec3f),digest);
    digest = UsdGenDigestBytes(indices.cdata(),indices.size()*sizeof(int),digest);
    digest = UsdGenDigestBytes(tauP.cdata(),tauP.size()*sizeof(float),digest);
    digest = UsdGenDigestBytes(tauN.cdata(),tauN.size()*sizeof(float),digest);
    }
    out->digest=digest;
    TF_DEBUG(USDGEN_FUR).Msg(
        "usdGen fur: scalp shadow %d x refinement, %zu points, %zu triangles\n",
        k, points.size(), counts.size());
}
}

bool UsdGenBuildFurOcclusion(std::vector<UsdGenTilePublication>* tiles,
    std::vector<UsdGenTilePublication> const* previous,
    UsdGenFurOcclusionParams const& params, uint64_t* volumeKey,
    UsdGenScalpShadowPublication* scalpShadow)
{
    TRACE_FUNCTION();
    if (!tiles) return false;

    // --- volume identity: everything `previous` cannot show ------------------
    // Same coverage and feed order as the byte-at-a-time FNV-1a this
    // replaces, hashed with the shared 4-lane word mixer whose contract
    // names cook digests explicitly. Values change; the key is in-memory
    // equality-only (stability/sensitivity tested, never golden).
    uint64_t key = UsdGenDigestOffset;
    {
        auto word = [&](void const* p, size_t n) {
            key = UsdGenDigestBytes(p, n, key);
        };
        word(&params.voxelSize, sizeof(params.voxelSize));
        word(&params.maxDimension, sizeof(params.maxDimension));
        word(&params.resolution, sizeof(params.resolution));
        word(&params.opaqueBiasVoxels, sizeof(params.opaqueBiasVoxels));
        word(&params.opaqueMarkVoxels, sizeof(params.opaqueMarkVoxels));
        word(&params.scalpShadow, sizeof(params.scalpShadow));
        word(&params.scalpMaxTriangles, sizeof(params.scalpMaxTriangles));
        for (UsdGenFurOccluder const& occluder : params.occluders) {
            word(&occluder.worldMatrix, sizeof(occluder.worldMatrix));
            word(occluder.points.cdata(), occluder.points.size()*sizeof(GfVec3f));
            word(occluder.faceVertexCounts.cdata(),
                 occluder.faceVertexCounts.size()*sizeof(int));
            word(occluder.faceVertexIndices.cdata(),
                 occluder.faceVertexIndices.size()*sizeof(int));
        }
    }
    bool same = previous && previous->size()==tiles->size() &&
                (!volumeKey || *volumeKey == key);
    if (volumeKey) *volumeKey = key;
    if (same) for(size_t i=0;i<tiles->size();++i) {
        auto const& a=(*tiles)[i]; auto const& b=(*previous)[i];
        if(a.tile!=b.tile || !a.points.IsIdentical(b.points) ||
           !a.widths.IsIdentical(b.widths) || a.curveVertexCounts!=b.curveVertexCounts ||
           a.basis!=b.basis || a.xformMatrix!=b.xformMatrix) { same=false; break; }
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
    UsdGenWorkDispatcher* const dispatcher = params.dispatcher;
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

    // --- grid: anisotropic dims at a target world voxel size -----------------
    // The longest axis alone used to decide a cube, which wastes resolution on
    // a flat groom and under-resolves a head. Here every axis gets the voxels
    // its own extent needs, at a shared edge length.
    int const maxDimension = std::clamp(params.maxDimension, 8, 512);
    int const opaqueBias = std::max(0, params.opaqueBiasVoxels);
    int const opaqueMark = std::max(1, params.opaqueMarkVoxels);
    float const groomLongest = std::max({span[0],span[1],span[2],1e-6f});
    // Voxel edge: keyed to the groom itself, so adding an occluder never
    // coarsens the hair.
    float voxel;
    if (params.resolution > 0) {
        voxel = groomLongest/float(std::clamp(params.resolution,8,maxDimension)-4);
    } else {
        float const target = params.voxelSize > 0.f ? params.voxelSize : 0.3f;
        // Never coarser than the legacy cube, never finer than maxDimension.
        voxel = std::min(target, groomLongest/float(kLegacyResolution-4));
        for (int k=0;k<3;++k) voxel = std::max(voxel, span[k]/float(maxDimension-4));
    }
    if (!(voxel > 0.f) || !std::isfinite(voxel))
        throw std::invalid_argument("fur bounds exceed finite grid range");

    // The opaque shell lives up to (bias + mark + 1) voxels behind a surface.
    // Grow the volume so a scalp sitting on the groom's own boundary -- a
    // ground-plane emitter is the pure case -- still has its shell inside it,
    // but only as far as the occluder actually reaches, so a room-sized floor
    // cannot cost the groom its resolution.
    if (!params.occluders.empty()) {
        float const reach = float(opaqueBias+opaqueMark+1)*voxel;
        GfVec3f occLo(std::numeric_limits<float>::max()), occHi(-occLo[0]);
        for (UsdGenFurOccluder const& occluder : params.occluders) {
            GfMatrix4f const m(occluder.worldMatrix);
            bool const identity = occluder.worldMatrix == GfMatrix4d(1.0);
            for (GfVec3f const& p : occluder.points) {
                GfVec3f const w = identity ? p : m.Transform(p);
                for (int k=0;k<3;++k) {
                    if (!std::isfinite(w[k]))
                        throw std::invalid_argument("non-finite occluder point");
                    occLo[k]=std::min(occLo[k],w[k]); occHi[k]=std::max(occHi[k],w[k]);
                }
            }
        }
        if (occLo[0] <= occHi[0]) for (int k=0;k<3;++k) {
            lo[k]=std::min(lo[k],std::max(occLo[k]-reach,lo[k]-reach));
            hi[k]=std::max(hi[k],std::min(occHi[k]+reach,hi[k]+reach));
        }
        span=hi-lo;
    }

    Grid grid;
    grid.h = voxel;
    if (params.resolution > 0) {
        int const resolution = std::clamp(params.resolution, 8, maxDimension);
        grid.h = std::max({span[0],span[1],span[2],1e-6f})/float(resolution-4);
        grid.n[0]=grid.n[1]=grid.n[2]=resolution;
    } else {
        for (int k=0;k<3;++k) grid.h = std::max(grid.h, span[k]/float(maxDimension-4));
        for (int k=0;k<3;++k)
            grid.n[k] = std::clamp(int(std::ceil(span[k]/grid.h))+4, 8, maxDimension);
    }
    if (!(grid.h > 0.f) || !std::isfinite(grid.h))
        throw std::invalid_argument("fur bounds exceed finite grid range");
    grid.origin = lo-GfVec3f(2*grid.h);
    grid.invH = 1.f/grid.h;
    float const h = grid.h;
    size_t const cells = grid.Cells();

    // --- work decomposition ---------------------------------------------------
    std::vector<Job> jobs;
    for(size_t t=0;t<tileCount;++t) {
        auto const& counts=(*tiles)[t].curveVertexCounts;
        size_t cv=0;
        for(size_t c=0;c<counts.size();) {
            Job job; job.tile=uint32_t(t); job.firstCurve=uint32_t(c); job.firstCv=cv;
            while(c<counts.size() && job.curveCount<kJobCurves && job.cvCount<kJobCvs) {
                job.cvCount+=size_t(counts[c]); ++c; ++job.curveCount;
            }
            cv=job.firstCv+job.cvCount;
            jobs.push_back(job);
        }
    }

    // --- splat projected fibre area, per job into its own sub-box -------------
    std::vector<GfVec3f> density(cells,GfVec3f(0));
    size_t const slabs=std::max<size_t>(1,std::min<size_t>(size_t(grid.n[2]),64));
    {
        TRACE_SCOPE("usdGen occlusion: splat");
        auto const clock0=std::chrono::steady_clock::now();
        std::vector<JobDensity> local(jobs.size());
        ForEach(dispatcher, jobs.size(), [&](size_t index) {
            Job const& job=jobs[index];
            auto const& tile=(*tiles)[job.tile]; auto const& p=positions[job.tile];
            if(!job.cvCount) return;
            GfVec3f jobLo(std::numeric_limits<float>::max()), jobHi(-jobLo[0]);
            for(size_t i=job.firstCv;i<job.firstCv+job.cvCount;++i)
                for(int k=0;k<3;++k) {
                    jobLo[k]=std::min(jobLo[k],p[i][k]); jobHi[k]=std::max(jobHi[k],p[i][k]);
                }
            JobDensity& out=local[index];
            int b0[3], b1[3]; float f[3];
            grid.Corners(jobLo,b0,f);
            grid.Corners(jobHi,b1,f);
            // Two voxels of slack: a pinned end segment and a catmullRom
            // overshoot can leave the control points' own box.
            for(int k=0;k<3;++k) {
                int const first=std::max(0,b0[k]-2);
                int const last=std::min(grid.n[k]-1,b1[k]+3);
                out.lo[k]=first; out.dims[k]=last-first+1;
            }
            out.density.assign(size_t(out.dims[0])*out.dims[1]*out.dims[2],GfVec3f(0));
            size_t const strideY=size_t(out.dims[0]), strideZ=strideY*out.dims[1];
            auto deposit=[&](GfVec3f const& at, GfVec3f const& value) {
                int c[3]; float w[3];
                grid.Corners(at,c,w);
                for(int k=0;k<3;++k)
                    c[k]=std::clamp(c[k],out.lo[k],out.lo[k]+out.dims[k]-2);
                size_t const cell=(size_t(c[2]-out.lo[2])*out.dims[1]+(c[1]-out.lo[1]))
                                  *out.dims[0]+(c[0]-out.lo[0]);
                float const wx[2]={1-w[0],w[0]}, wy[2]={1-w[1],w[1]}, wz[2]={1-w[2],w[2]};
                for(int z=0;z<2;++z) {
                    GfVec3f const vz=value*wz[z];
                    for(int y=0;y<2;++y) {
                        GfVec3f const vy=vz*wy[y];
                        GfVec3f* const row=&out.density[cell+y*strideY+z*strideZ];
                        row[0]+=vy*wx[0]; row[1]+=vy*wx[1];
                    }
                }
            };
            // Conservative cross-section scale under nonuniform transforms.
            float scale=0;
            for(int k=0;k<3;++k) { GfVec3d axis(0);axis[k]=1;
                scale=std::max(scale,float(tile.xformMatrix.TransformDir(axis).GetLength())); }
            bool const catmullRom = tile.basis == "catmullRom";
            bool const constantWidth = tile.widths.size()==1;
            float const oneWidth = tile.widths.empty() ? 0.f :
                (std::isfinite(tile.widths[0]) ? std::max(0.f,tile.widths[0])*scale : 0.f);
            size_t base=job.firstCv;
            for(uint32_t c=job.firstCurve;c<job.firstCurve+job.curveCount;++c) {
                int const count=tile.curveVertexCounts[c];
                if(count<2) { base+=size_t(count>0?count:0); continue; }
                auto point=[&](int i)->GfVec3f {
                    if(i<0) return 2.f*p[base]-p[base+1];
                    if(i>=count) return 2.f*p[base+count-1]-p[base+count-2];
                    return p[base+i];
                };
                auto width=[&](int i)->float {
                    if(tile.widths.empty() || constantWidth) return oneWidth;
                    auto at=[&](int j)->float {
                        float const w=tile.widths[base+size_t(j)];
                        return std::isfinite(w)?std::max(0.f,w):0.f;
                    };
                    float w;
                    if(i<0) w=2.f*at(0)-at(1);
                    else if(i>=count) w=2.f*at(count-1)-at(count-2);
                    else w=at(i);
                    return std::max(0.f,w)*scale;
                };
                // A pinned cubic has one segment per CV interval; splat the
                // curve Storm draws, not the control polygon.
                for(int j=0;j<count-1;++j) {
                    GfVec3f const q[4]={point(j-1),point(j),point(j+1),point(j+2)};
                    float const qw[4]={width(j-1),width(j),width(j+1),width(j+2)};
                    float const chord=(q[2]-q[1]).GetLength();
                    if(!(chord>0.f)) continue;
                    int const steps=std::clamp(
                        int(std::ceil(kSamplesPerVoxel*chord*grid.invH)),1,kMaxSegmentSteps);
                    float w4[4];
                    CubicWeights(catmullRom,0.f,w4);
                    GfVec3f a=q[0]*w4[0]+q[1]*w4[1]+q[2]*w4[2]+q[3]*w4[3];
                    float aw=qw[0]*w4[0]+qw[1]*w4[1]+qw[2]*w4[2]+qw[3]*w4[3];
                    for(int s=0;s<steps;++s) {
                        float const u=float(s+1)/float(steps);
                        CubicWeights(catmullRom,u,w4);
                        GfVec3f const b=q[0]*w4[0]+q[1]*w4[1]+q[2]*w4[2]+q[3]*w4[3];
                        float const bw=qw[0]*w4[0]+qw[1]*w4[1]+qw[2]*w4[2]+qw[3]*w4[3];
                        GfVec3f const d=b-a;
                        float const len=d.GetLength();
                        if(len>1e-12f) {
                            GfVec3f const tangent=d/len;
                            // Projected fibre area over the cell cross-section:
                            // the expected crossings a ray along each axis sees.
                            float const area=std::max(0.f,0.5f*(aw+bw))*len*grid.invH*grid.invH;
                            GfVec3f projection;
                            for(int k=0;k<3;++k)
                                projection[k]=std::sqrt(std::max(0.f,1-tangent[k]*tangent[k]));
                            deposit(a+0.5f*d,projection*area);
                        }
                        a=b; aw=bw;
                    }
                }
                base+=size_t(count);
            }
        });
        if(TfDebug::IsEnabled(USDGEN_FUR)) {
            size_t sub=0; for(auto const& job:local)
                sub+=size_t(job.dims[0])*job.dims[1]*job.dims[2];
            TF_DEBUG(USDGEN_FUR).Msg(
                "usdGen fur: grid %dx%dx%d voxel %.4f, %zu cells, %zu tiles, "
                "%zu jobs, %zu CVs, %zu sub-box cells, %zu occluder mesh(es), "
                "scatter %.1f ms\n",
                grid.n[0],grid.n[1],grid.n[2],double(h),cells,tileCount,jobs.size(),
                pointCount,sub,params.occluders.size(),
                std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-clock0).count());
        }
        // Merge over disjoint z slabs: with the grid at a world voxel size this
        // sum is O(jobs * sub-box) and dominated the splat on one thread.
        ForEach(dispatcher, slabs, [&](size_t slab) {
            int const z0=int((slab*size_t(grid.n[2]))/slabs);
            int const z1=int(((slab+1)*size_t(grid.n[2]))/slabs);
            for(JobDensity const& job:local) {
                if(job.density.empty()) continue;
                int const first=std::max(z0,job.lo[2]);
                int const last=std::min(z1,job.lo[2]+job.dims[2]);
                for(int z=first;z<last;++z) for(int y=0;y<job.dims[1];++y) {
                    size_t const src=(size_t(z-job.lo[2])*job.dims[1]+y)*job.dims[0];
                    size_t const dst=grid.Index(job.lo[0],job.lo[1]+y,z);
                    for(int x=0;x<job.dims[0];++x) density[dst+x]+=job.density[src+x];
                }
            }
        });
    }

    // --- occluder triangles, and which side of them the solid is on ----------
    // Built before the shell is injected, because the scalp-shadow cap needs
    // hair-only depth and the side vote needs hair-only density.
    std::vector<Triangle> triangles;
    std::vector<GfVec3f> vertices, vertexNormals;
    float inward = -1.f;
    if(!params.occluders.empty()) {
        TRACE_SCOPE("usdGen occlusion: occluder triangles");
        for(UsdGenFurOccluder const& occluder:params.occluders) {
            GfMatrix4f const m(occluder.worldMatrix);
            bool const identity = occluder.worldMatrix == GfMatrix4d(1.0);
            uint32_t const base=uint32_t(vertices.size());
            // Both arrays below grow to a size known up front: the points
            // copy is exact, and the face walk emits one triangle per face
            // on a tri mesh (a lower bound otherwise). reserve() changes
            // capacity only, so the built arrays are bit-identical.
            vertices.reserve(vertices.size()+occluder.points.size());
            triangles.reserve(triangles.size()+occluder.faceVertexCounts.size());
            { TRACE_SCOPE("usdGen tris: vertices");
            for(GfVec3f const& p:occluder.points) {
                GfVec3f const w=identity?p:m.Transform(p);
                for(int k=0;k<3;++k) if(!std::isfinite(w[k]))
                    throw std::invalid_argument("non-finite occluder point");
                vertices.push_back(w);
            }
            }
            { TRACE_SCOPE("usdGen tris: faces");
            size_t cursor=0;
            for(int face:occluder.faceVertexCounts) {
                if(face<0) throw std::invalid_argument("negative occluder face vertex count");
                if(cursor+size_t(face)>occluder.faceVertexIndices.size())
                    throw std::invalid_argument("invalid occluder face cardinality");
                if(face<3) { cursor+=size_t(face); continue; }
                auto vertex=[&](size_t at)->uint32_t {
                    int const index=occluder.faceVertexIndices[cursor+at];
                    if(index<0 || size_t(index)>=occluder.points.size())
                        throw std::invalid_argument("occluder face index out of range");
                    return base+uint32_t(index);
                };
                uint32_t const first=vertex(0);
                for(size_t k=1;k+1<size_t(face);++k) {
                    uint32_t const ib=vertex(k), ic=vertex(k+1);
                    Triangle tri{vertices[first],vertices[ib],vertices[ic],
                                 GfVec3f(0),first,ib,ic};
                    GfVec3f const n=GfCross(tri.b-tri.a,tri.c-tri.a);
                    float const length=n.GetLength();
                    if(!(length>0.f)) continue;
                    tri.normal=n/length;
                    triangles.push_back(tri);
                }
                cursor+=size_t(face);
            }
            }
        }
        // Area-weighted vertex normals: the cross product's length is twice the
        // triangle's area, so accumulating it unnormalised is the weighting.
        { TRACE_SCOPE("usdGen tris: normals");
        vertexNormals.assign(vertices.size(),GfVec3f(0));
        for(Triangle const& tri:triangles) {
            GfVec3f const weighted=GfCross(tri.b-tri.a,tri.c-tri.a);
            vertexNormals[tri.ia]+=weighted;
            vertexNormals[tri.ib]+=weighted;
            vertexNormals[tri.ic]+=weighted;
        }
        }
        for(size_t i=0;i<vertexNormals.size();++i) {
            float const length=vertexNormals[i].GetLength();
            if(length>1e-12f) vertexNormals[i]/=length;
        }
        // Which side of the winding the solid is on: the hair grows out of the
        // surface, so the side carrying less fur density is the inside. Voting
        // over the triangles keeps this right for either winding order and for
        // a flat emitter, where a centroid test would be degenerate.
        float vote=0;
        { TRACE_SCOPE("usdGen tris: vote");
            size_t const stride=std::max<size_t>(1,triangles.size()/4096);
            float const probe=3.f*h;
            auto fur=[&](GfVec3f const& at)->float {
                if(!grid.Contains(at)) return 0.f;
                int c[3]; float w[3];
                grid.Corners(at,c,w);
                float sum=0;
                for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
                    float const weight=(x?w[0]:1-w[0])*(y?w[1]:1-w[1])*(z?w[2]:1-w[2]);
                    GfVec3f const d=density[grid.Index(c[0]+x,c[1]+y,c[2]+z)];
                    sum+=weight*(d[0]+d[1]+d[2]);
                }
                return sum;
            };
            for(size_t i=0;i<triangles.size();i+=stride) {
                Triangle const& tri=triangles[i];
                GfVec3f const e1=tri.b-tri.a, e2=tri.c-tri.a;
                // A lattice, not the centroid: one triangle can be larger than
                // the whole groom, and then its centroid says nothing.
                float const extent=std::max({e1.GetLength(),e2.GetLength(),
                                             (tri.c-tri.b).GetLength()});
                int const n=std::clamp(int(std::ceil(extent*grid.invH)),1,4);
                for(int a=0;a<=n;++a) for(int b=0;a+b<=n;++b) {
                    GfVec3f const on=tri.a+e1*(float(a)/float(n))+e2*(float(b)/float(n));
                    vote+=fur(on+tri.normal*probe)-fur(on-tri.normal*probe);
                }
            }
        }
        // vote > 0: more fur on the +normal side, so the solid is at -normal.
        inward = vote >= 0.f ? -1.f : 1.f;
        TF_DEBUG(USDGEN_FUR).Msg(
            "usdGen fur: %zu occluder triangle(s), fur vote %+.3f, solid side %s\n",
            triangles.size(), double(vote), inward < 0.f ? "-normal" : "+normal");
    }

    // --- the six axis sweeps, run once over hair alone and once with the
    // opaque shell ------------------------------------------------------------
    // Prefix integration is O(voxels), independent of hair count and number of
    // lights. Half the receiver's own cell is the correct discretisation of an
    // integral that starts at the CV, and is what a host renderer's HairCount - 1 shift
    // expects to find.
    std::array<std::vector<float>,6> tau;
    auto sweep=[&] {
        ForEach(dispatcher, 6, [&](size_t direction) {
            int const axis=int(direction)/2;
            int const na=grid.n[axis];
            int const nu=grid.n[(axis+1)%3], nv=grid.n[(axis+2)%3];
            auto& out=tau[direction];
            out.assign(cells,0.f);
            for(int v=0;v<nv;++v) for(int u=0;u<nu;++u) {
                float sum=0;
                for(int step=0;step<na;++step) {
                    int c[3]; c[axis]=(direction%2)?step:na-1-step;
                    c[(axis+1)%3]=u;c[(axis+2)%3]=v;
                    size_t idx=grid.Index(c[0],c[1],c[2]);
                    float depth=density[idx][axis];
                    out[idx]=std::min(UsdGenFurTauClamp,sum+0.5f*depth);sum+=depth;
                }
            }
        });
    };
    // Trilinear gather of the six depths at one point.
    auto gather=[&](GfVec3f const& at, float values[6]) {
        int c[3]; float w[3];
        grid.Corners(at,c,w);
        for(int d=0;d<6;++d) values[d]=0.f;
        for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
            float const weight=(x?w[0]:1-w[0])*(y?w[1]:1-w[1])*(z?w[2]:1-w[2]);
            size_t const idx=grid.Index(c[0]+x,c[1]+y,c[2]+z);
            for(int d=0;d<6;++d) values[d]+=weight*tau[d][idx];
        }
    };

    if(params.scalpShadow && scalpShadow && !triangles.empty()) {
        TRACE_SCOPE("usdGen occlusion: scalp shadow");
        sweep();   // hair only: the head shadowing itself is the skin shader's job
        BuildScalpShadow(triangles, vertexNormals, inward, grid,
                         params.scalpMaxTriangles, gather, dispatcher,
                         scalpShadow);
    }

    // --- opaque occluders: a saturated shell just under the surface -----------
    // a host renderer injects the opaque depth buffer with InjectOpaque.BiasCount /
    // MarkCount; with real triangles the same shell is marked along the inward
    // normal, so light is blocked through the scalp while roots resting on it
    // are not shadowed by it.
    if(!triangles.empty()) {
        TRACE_SCOPE("usdGen occlusion: occluders");
        int const bias=opaqueBias, mark=opaqueMark;
        // One crossing of the shell must always reach the published ceiling.
        // Marked layers land at whatever sub-voxel phase the surface happens
        // to have, and max() over trilinear weights then sums to between
        // mark/2 and mark; twice the naive share covers the worst phase, and
        // the ceiling absorbs the rest.
        float const perVoxel=2.f*UsdGenFurTauClamp/float(mark);

        // A shell taken with max(), so it does not depend on how densely the
        // triangles happen to be sampled. Each triangle is rasterized once,
        // into its chunk's own mask; the chunk masks then merge with max(),
        // which over non-negative trilinear weights is order-independent
        // bit-for-bit, so the merged shell matches the old walk exactly.
        // (The slab-bucketed walk this replaces visited every triangle once
        // per overlapping slab and re-iterated the full lattice per visit.)
        size_t const nShellChunks = std::max<size_t>(1, std::min<size_t>(
            triangles.size(), size_t(dispatcher ?
                std::max(1, dispatcher->MaxConcurrency()) : 1)));
        std::vector<std::vector<float>> chunkMasks(nShellChunks);
        ForEach(dispatcher,nShellChunks,[&](size_t chunk) {
            size_t const first=(chunk*triangles.size())/nShellChunks;
            size_t const last=((chunk+1)*triangles.size())/nShellChunks;
            std::vector<float>& mask=chunkMasks[chunk];
            mask.assign(cells,0.f);
            for(size_t ti=first;ti<last;++ti) {
                Triangle const& tri=triangles[ti];
                GfVec3f const e1=tri.b-tri.a, e2=tri.c-tri.a;
                // Barycentric lattice fine enough that adjacent samples'
                // trilinear stencils overlap, so the shell has no pinholes.
                float const extent=std::max({e1.GetLength(),e2.GetLength(),
                                             (tri.c-tri.b).GetLength()});
                int const n=std::clamp(int(std::ceil(1.5f*extent*grid.invH)),1,512);
                GfVec3f const step=tri.normal*(inward*h);
                for(int i=0;i<=n;++i) for(int j=0;i+j<=n;++j) {
                    GfVec3f const on=tri.a+e1*(float(i)/float(n))+e2*(float(j)/float(n));
                    for(int layer=0;layer<mark;++layer) {
                        GfVec3f const at=on+step*(float(bias+layer)+0.5f);
                        if(!grid.Contains(at)) continue;
                        int c[3]; float w[3];
                        grid.Corners(at,c,w);
                        for(int z=0;z<2;++z) {
                            for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
                                float const weight=(x?w[0]:1-w[0])*(y?w[1]:1-w[1])*(z?w[2]:1-w[2]);
                                float& cell=mask[grid.Index(c[0]+x,c[1]+y,c[2]+z)];
                                cell=std::max(cell,weight);
                            }
                        }
                    }
                }
            }
        });
        std::vector<float> mask(cells,0.f);
        ForEach(dispatcher,slabs,[&](size_t slab) {
            size_t const first=(slab*cells)/slabs, last=((slab+1)*cells)/slabs;
            for(size_t i=first;i<last;++i) {
                float m=0.f;
                for(auto const& cm:chunkMasks) m=std::max(m,cm[i]);
                mask[i]=m;
            }
        });
        chunkMasks.clear(); chunkMasks.shrink_to_fit();
        ForEach(dispatcher,slabs,[&](size_t slab) {
            size_t const first=(slab*cells)/slabs, last=((slab+1)*cells)/slabs;
            for(size_t i=first;i<last;++i)
                if(mask[i]>0.f) density[i]+=GfVec3f(mask[i]*perVoxel);
        });
    }

    // --- the sweep the strands read: hair plus the opaque shell --------------
    {
        TRACE_SCOPE("usdGen occlusion: sweep");
        sweep();
    }
    // Interleave the six sweeps: the gather reads all six depths of eight
    // neighbouring cells, which is two cache lines per cell instead of six
    // separate strided arrays.
    std::vector<uint16_t> depths(cells*6);
    {
        TRACE_SCOPE("usdGen occlusion: interleave");
        ForEach(dispatcher, slabs, [&](size_t slab) {
            size_t const first=(slab*cells)/slabs, last=((slab+1)*cells)/slabs;
            for(size_t i=first;i<last;++i)
                for(int d=0;d<6;++d)
                    depths[i*6+d]=uint16_t(tau[d][i]*kDepthScale+0.5f);
        });
        for(auto& plane:tau) std::vector<float>().swap(plane);
    }

    // --- gather: every receiver reads its six depths once --------------------
    {
        TRACE_SCOPE("usdGen occlusion: gather");
        std::vector<std::array<float*,2>> planes(tileCount, {nullptr,nullptr});
        for(size_t t=0;t<tileCount;++t) {
            auto& out=(*tiles)[t].extraUniform;
            out.erase(std::remove_if(out.begin(),out.end(),
                [](auto const& p){return Reserved(p.name);}),out.end());
            for(auto name:names) {
                UsdGenPlane plane;plane.name=TfToken(name);plane.interpolation=TfToken("vertex");
                plane.type=TfToken("float");plane.arity=3;
                // Every element below is written; zero-filling six floats per
                // CV first costs more than the gather itself on a full groom.
                plane.f.resize((*tiles)[t].points.size()*3, [](float* b, float* e) {
                    std::uninitialized_default_construct(b, e);
                });
                out.push_back(std::move(plane));
            }
            std::sort(out.begin(),out.end(),[](auto const& a,auto const& b){return a.name<b.name;});
            for(int which=0;which<2;++which) {
                auto it=std::find_if(out.begin(),out.end(),
                    [&](auto const& p){return p.name==names[which];});
                planes[t][which]=it->f.data();   // detaches this tile's own new plane
            }
        }
        size_t const strideY=size_t(grid.n[0])*6, strideZ=size_t(grid.n[0])*grid.n[1]*6;
        ForEach(dispatcher, jobs.size(), [&](size_t index) {
            Job const& job=jobs[index];
            // names[0] (P) holds the even directions, names[1] (N) the odd.
            float* const p0=planes[job.tile][0];
            float* const p1=planes[job.tile][1];
            auto const& p=positions[job.tile];
            for(size_t i=job.firstCv;i<job.firstCv+job.cvCount;++i) {
                int c[3]; float w[3];
                grid.Corners(p[i],c,w);
                uint16_t const* const cell=&depths[grid.Index(c[0],c[1],c[2])*6];
                float values[6]={0,0,0,0,0,0};
                for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
                    float const weight=(x?w[0]:1-w[0])*(y?w[1]:1-w[1])*(z?w[2]:1-w[2]);
                    uint16_t const* const at=cell+x*6+y*strideY+z*strideZ;
                    for(int d=0;d<6;++d) values[d]+=weight*float(at[d]);
                }
                for(int d=0;d<3;++d) {
                    p0[i*3+d]=values[d*2]*(1.f/kDepthScale);
                    p1[i*3+d]=values[d*2+1]*(1.f/kDepthScale);
                }
            }
        });
    }
    return true;
}
}
