#include "usdGen/limitSurface.h"
#include "usdGen/digest.h"
#include "pxr/imaging/pxOsd/meshTopology.h"
#include "pxr/imaging/pxOsd/refinerFactory.h"
#include <opensubdiv/far/patchTableFactory.h>
#include <opensubdiv/far/patchMap.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <algorithm>
#include <cmath>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE
namespace usdGen {
namespace {
PxOsdMeshTopology Topology(UsdGenSurfaceDesc const& s) {
    PxOsdSubdivTags tags(s.interpolateBoundary, s.faceVaryingLinearInterpolation,
        s.creaseMethod, s.triangleSubdivisionRule, s.creaseIndices, s.creaseLengths,
        s.creaseSharpnesses, s.cornerIndices, s.cornerSharpnesses);
    // Evaluate in authored corner order. Left-handedness changes the normal,
    // not Ptex's corner coordinate convention.
    return PxOsdMeshTopology(s.subdivisionScheme, TfToken("rightHanded"),
        s.faceVertexCounts, s.faceVertexIndices, s.holeIndices, tags);
}
struct Vertex {
    GfVec3f p{0};
    void Clear() { p=GfVec3f(0); }
    void AddWithWeight(Vertex const& v, float w) { p+=v.p*w; }
};
}
struct UsdGenLimitSurface::State {
    std::unique_ptr<OpenSubdiv::Far::PatchTable> patches;
    std::unique_ptr<OpenSubdiv::Far::PatchMap> map;
    std::vector<Vertex> values;
    VtIntArray holes;
};
UsdGenLimitSurface::UsdGenLimitSurface() = default;
UsdGenLimitSurface::~UsdGenLimitSurface() = default;
namespace {
void FeedSubdivText(uint64_t &h, TfToken const& tok) {
    std::string const& str = tok.GetString();
    h = UsdGenDigestBytes(str.data(), str.size(), h);
    UsdGenDigestMixWord(h, uint64_t(str.size()));
}
template <class Array>
void FeedSubdivArray(uint64_t &h, Array const& values) {
    h = UsdGenDigestBytes(values.cdata(),
        values.size() * sizeof(*values.cdata()), h);
    UsdGenDigestMixWord(h, uint64_t(values.size()));
}
// Tags, holes, creases, corners: the subdivision inputs OUTSIDE the two
// face arrays. Shared by both entry points so the coverage cannot drift.
void FeedSubdivTags(uint64_t &h, UsdGenSurfaceDesc const& s) {
    FeedSubdivText(h, s.subdivisionScheme);
    FeedSubdivText(h, s.orientation);
    FeedSubdivText(h, s.interpolateBoundary);
    FeedSubdivText(h, s.faceVaryingLinearInterpolation);
    FeedSubdivText(h, s.triangleSubdivisionRule);
    FeedSubdivText(h, s.creaseMethod);
    FeedSubdivArray(h, s.holeIndices);
    FeedSubdivArray(h, s.creaseIndices);
    FeedSubdivArray(h, s.creaseLengths);
    FeedSubdivArray(h, s.creaseSharpnesses);
    FeedSubdivArray(h, s.cornerIndices);
    FeedSubdivArray(h, s.cornerSharpnesses);
}
void FeedSubdivTopology(uint64_t &h, UsdGenSurfaceDesc const& s) {
    FeedSubdivArray(h, s.faceVertexCounts);
    FeedSubdivArray(h, s.faceVertexIndices);
}
} // namespace
// Same logical coverage as Topology(s).ComputeHash() plus s.orientation,
// hashed with the 4-lane word-wise FNV instead of SpookyHash (which
// re-streamed ~20MB of topology arrays at ~half the per-byte throughput).
// Tokens feed as string bytes rather than TfToken rep bytes, so the digest
// is deterministic across processes (values stay internal keys, never
// persisted). Sizes mix between fields so no two distinct field sequences
// concatenate ambiguously.
uint64_t UsdGenSubdivisionDigest(UsdGenSurfaceDesc const& s) {
    uint64_t h = UsdGenDigestOffset;
    FeedSubdivTags(h, s);
    FeedSubdivTopology(h, s);
    return h;
}
uint64_t UsdGenSubdivisionTagsDigest(UsdGenSurfaceDesc const& s) {
    uint64_t h = UsdGenDigestOffset;
    FeedSubdivTags(h, s);
    return h;
}
bool UsdGenLimitSurface::Build(UsdGenSurfaceDesc const& s, int level, std::string* error) {
    _state.reset();
    auto fail=[&](char const* e){if(error)*error=e;return false;};
    if(level<1 || level>6) return fail("subdivisionLevel must be in [1,6]");
    if(s.subdivisionScheme!=TfToken("catmullClark"))
        return fail("limit scatter requires subdivisionScheme=catmullClark");
    if(s.faceVertexCounts.empty() || s.restPoints.empty()) return fail("empty limit surface");
    for(int n:s.faceVertexCounts) if(n!=4)
        return fail("limit scatter currently requires quad faces; quadrangulate before painting Ptex");
    if(s.faceVertexIndices.size()!=4*s.faceVertexCounts.size()) return fail("inconsistent limit topology");
    for(int i:s.faceVertexIndices) if(i<0 || size_t(i)>=s.restPoints.size()) return fail("invalid limit vertex index");
    for(auto const& p:s.restPoints) for(int j=0;j<3;++j)
        if(!std::isfinite(p[j])) return fail("non-finite limit point");
    for(int f:s.holeIndices) if(f<0 || size_t(f)>=s.faceVertexCounts.size()) return fail("invalid hole face");
    size_t nc=0,ne=0;
    for(int n:s.creaseLengths) {if(n<2)return fail("invalid crease length");nc+=n;ne+=n-1;}
    if(nc!=s.creaseIndices.size() ||
       (s.creaseSharpnesses.size()!=s.creaseLengths.size() && s.creaseSharpnesses.size()!=ne))
        return fail("inconsistent crease tags");
    if(s.cornerIndices.size()!=s.cornerSharpnesses.size()) return fail("inconsistent corner tags");
    for(auto const* ids:{&s.creaseIndices,&s.cornerIndices})
        for(int i:*ids) if(i<0 || size_t(i)>=s.restPoints.size()) return fail("invalid sharpness vertex");
    for(auto const* weights:{&s.creaseSharpnesses,&s.cornerSharpnesses})
        for(float w:*weights) if(!std::isfinite(w)||w<0) return fail("invalid sharpness");
    auto refiner=PxOsdRefinerFactory::Create(Topology(s));
    if(!refiner) return fail("OpenSubdiv could not create the topology refiner");
    OpenSubdiv::Far::PatchTableFactory::Options opts(level);
    opts.SetEndCapType(OpenSubdiv::Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS);
    refiner->RefineAdaptive(opts.GetRefineAdaptiveOptions());
    auto state=std::make_unique<State>();
    state->patches.reset(OpenSubdiv::Far::PatchTableFactory::Create(*refiner,opts));
    if(!state->patches) return fail("OpenSubdiv could not create limit patches");
    int total=refiner->GetNumVerticesTotal();
    state->values.resize(total+state->patches->GetNumLocalPoints());
    int offset=refiner->GetLevel(0).GetNumVertices();
    for(int i=0;i<offset;++i) state->values[i].p=s.restPoints[i];
    OpenSubdiv::Far::PrimvarRefiner pv(*refiner);
    int previous=0;
    for(int l=1;l<=refiner->GetMaxLevel();++l) {
        auto* src=state->values.data()+previous;auto* dst=state->values.data()+offset;
        pv.Interpolate(l,src,dst);
        previous=offset;offset+=refiner->GetLevel(l).GetNumVertices();
    }
    state->patches->ComputeLocalPointValues(state->values.data(),state->values.data()+total);
    state->map=std::make_unique<OpenSubdiv::Far::PatchMap>(*state->patches);
    state->holes=s.holeIndices;
    _state=std::move(state);
    return true;
}
bool UsdGenLimitSurface::IsHole(int face) const {
    return _state && std::find(_state->holes.begin(),_state->holes.end(),face)!=_state->holes.end();
}
bool UsdGenLimitSurface::Evaluate(int face,float u,float v,GfVec3f* p,GfVec3f* du,GfVec3f* dv) const {
    if(!_state || !p || !du || !dv || !std::isfinite(u)||!std::isfinite(v)||u<0||v<0||u>1||v>1||IsHole(face)) return false;
    auto const* h=_state->map->FindPatch(face,u,v);
    if(!h) return false;
    float w[20],wu[20],wv[20];
    _state->patches->EvaluateBasis(*h,u,v,w,wu,wv);
    auto ids=_state->patches->GetPatchVertices(*h);
    *p=*du=*dv=GfVec3f(0);
    for(int i=0;i<ids.size();++i){auto const& q=_state->values[ids[i]].p;*p+=q*w[i];*du+=q*wu[i];*dv+=q*wv[i];}
    for(auto const* value:{p,du,dv})for(int j=0;j<3;++j)if(!std::isfinite((*value)[j]))return false;
    return true;
}
}
