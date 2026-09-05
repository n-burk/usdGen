// Probe: CPU-side cost structure of HdStBasisCurves primvar/topology handling.
// Builds against the OpenUSD 26.08 install + two private hdSt headers copied
// into priv_include/. No GL context required.
#include "pxr/pxr.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/basisCurvesTopology.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/imaging/hdSt/basisCurvesComputations.h"
#include "pxr/imaging/hdSt/basisCurvesTopology.h"

#include <chrono>
#include <cstdio>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static VtVec3fArray MakePoints(size_t n, float seed) {
    VtVec3fArray p(n);
    for (size_t i = 0; i < n; ++i) {
        p[i] = GfVec3f(float(i) * 0.001f + seed, float(i % 8) * 0.1f, seed);
    }
    return p;
}

struct Result {
    size_t outSize;
    bool   isFallback;   // first element == fallback (1,0,0)
    bool   sharesInput;  // output data ptr == input data ptr (no CPU copy)
    double resolveMs;
};

static Result RunInterp(const HdBasisCurvesTopology &srcTopo,
                        const VtVec3fArray &pts,
                        HdInterpolation interp)
{
    HdSt_BasisCurvesTopologySharedPtr topo = HdSt_BasisCurvesTopology::New(srcTopo);
    auto comp = std::make_shared<
        HdSt_BasisCurvesPrimvarInterpolaterComputation<GfVec3f>>(
            topo, pts, SdfPath("/probe"), HdTokens->points, interp,
            GfVec3f(1,0,0), HdTypeFloatVec3);
    auto t0 = Clock::now();
    comp->Resolve();
    auto t1 = Clock::now();
    Result r;
    r.resolveMs = ms(t0, t1);
    r.outSize = comp->GetNumElements();
    const GfVec3f *out = static_cast<const GfVec3f*>(comp->GetData());
    r.isFallback = (r.outSize > 0 && out[0] == GfVec3f(1,0,0));
    r.sharesInput = (out == pts.cdata());
    return r;
}

int main() {
    printf("=== PROBE A: does HdSt_BasisCurvesPrimvarInterpolaterComputation copy points? ===\n");
    const int NCURVES = 100000;
    const int NCV = 8;              // 8 CVs per curve, bspline
    VtIntArray counts(NCURVES, NCV);
    const size_t exact = size_t(NCURVES) * NCV;

    HdBasisCurvesTopology topoExact(HdTokens->cubic, HdTokens->bspline,
                                    HdTokens->nonperiodic, counts, VtIntArray());
    printf("topology: %d curves x %d CV; CalculateNeededNumberOfControlPoints = %zu"
           " (sum(counts) = %zu)\n", NCURVES, NCV,
           topoExact.CalculateNeededNumberOfControlPoints(), exact);
    printf("          CalculateNeededNumberOfVaryingControlPoints = %zu\n",
           topoExact.CalculateNeededNumberOfVaryingControlPoints());

    VtVec3fArray ptsExact = MakePoints(exact, 0.f);
    // run 3x to warm allocator
    Result rExact{};
    for (int i = 0; i < 3; ++i) rExact = RunInterp(topoExact, ptsExact, HdInterpolationVertex);
    printf("[exact size] out=%zu fallback=%d sharesInputBuffer=%d resolve=%.2f ms\n",
           rExact.outSize, (int)rExact.isFallback, (int)rExact.sharesInput, rExact.resolveMs);

    printf("\n=== PROBE B: points array LONGER than sum(curveVertexCounts), NO curveIndices ===\n");
    VtVec3fArray ptsPadded = MakePoints(exact + exact/4, 0.f);  // 25%% headroom
    {
        TfErrorMark m;
        Result r = RunInterp(topoExact, ptsPadded, HdInterpolationVertex);
        printf("[padded, no indices] authored=%zu expected=%zu -> out=%zu fallback=%d"
               " sharesInput=%d resolve=%.2f ms\n",
               ptsPadded.size(), exact, r.outSize, (int)r.isFallback,
               (int)r.sharesInput, r.resolveMs);
        m.Clear();
    }

    printf("\n=== PROBE C: points LONGER, WITH curveIndices covering only the live CVs ===\n");
    {
        VtIntArray indices(exact);
        for (size_t i = 0; i < exact; ++i) indices[i] = int(i);
        HdBasisCurvesTopology topoIdx(HdTokens->cubic, HdTokens->bspline,
                                      HdTokens->nonperiodic, counts, indices);
        printf("  with indices: CalculateNeededNumberOfControlPoints = %zu"
               " (1+max(indices))\n", topoIdx.CalculateNeededNumberOfControlPoints());
        TfErrorMark m;
        Result r = RunInterp(topoIdx, ptsPadded, HdInterpolationVertex);
        printf("[padded, with indices] authored=%zu -> out=%zu fallback=%d"
               " sharesInput=%d resolve=%.2f ms\n",
               ptsPadded.size(), r.outSize, (int)r.isFallback,
               (int)r.sharesInput, r.resolveMs);
        m.Clear();
    }

    printf("\n=== PROBE D: fewer live curves via curveVertexCounts, same fat points array ===\n");
    // Interactive density scrub emulation: keep the points array allocated at
    // max size, shrink only the counts. (no indices)
    for (int live : {100000, 50000, 25000}) {
        VtIntArray c(live, NCV);
        HdBasisCurvesTopology t(HdTokens->cubic, HdTokens->bspline,
                                HdTokens->nonperiodic, c, VtIntArray());
        TfErrorMark m;
        Result r = RunInterp(t, ptsPadded, HdInterpolationVertex);
        printf("[live=%6d] expected=%zu authored=%zu -> out=%zu fallback=%d resolve=%.2f ms\n",
               live, t.CalculateNeededNumberOfControlPoints(), ptsPadded.size(),
               r.outSize, (int)r.isFallback, r.resolveMs);
        m.Clear();
    }

    printf("\n=== PROBE E: varying primvar expansion cost (per-curve -> per-vertex) ===\n");
    {
        size_t nVarying = topoExact.CalculateNeededNumberOfVaryingControlPoints();
        VtVec3fArray varyingVals = MakePoints(nVarying, 1.f);
        Result r{};
        for (int i = 0; i < 3; ++i)
            r = RunInterp(topoExact, varyingVals, HdInterpolationVarying);
        printf("[varying] authored=%zu -> out=%zu fallback=%d sharesInput=%d expand=%.2f ms\n",
               nVarying, r.outSize, (int)r.isFallback, (int)r.sharesInput, r.resolveMs);
    }

    printf("\n=== PROBE F: index-buffer rebuild cost (topology change) ===\n");
    for (int nc : {1000, 10000, 100000}) {
        VtIntArray c(nc, NCV);
        HdBasisCurvesTopology src(HdTokens->cubic, HdTokens->bspline,
                                  HdTokens->nonperiodic, c, VtIntArray());
        HdSt_BasisCurvesTopologySharedPtr topo = HdSt_BasisCurvesTopology::New(src);
        auto t0 = Clock::now();
        HdBufferSourceSharedPtr ib = topo->GetIndexBuilderComputation(false);
        ib->Resolve();
        auto t1 = Clock::now();
        printf("[cubic bspline %6d curves x %d CV] indices=%zu elems (int32x4)"
               " -> %.2f MB, build=%.2f ms\n",
               nc, NCV, ib->GetNumElements(),
               double(ib->GetNumElements()) * 16.0 / (1024*1024), ms(t0, t1));
    }
    // linear comparison
    for (int nc : {100000}) {
        VtIntArray c(nc, NCV);
        HdBasisCurvesTopology src(HdTokens->linear, TfToken(),
                                  HdTokens->nonperiodic, c, VtIntArray());
        HdSt_BasisCurvesTopologySharedPtr topo = HdSt_BasisCurvesTopology::New(src);
        auto t0 = Clock::now();
        HdBufferSourceSharedPtr ib = topo->GetIndexBuilderComputation(false);
        ib->Resolve();
        auto t1 = Clock::now();
        printf("[linear      %6d curves x %d CV] indices=%zu elems (int32x2)"
               " -> %.2f MB, build=%.2f ms\n",
               nc, NCV, ib->GetNumElements(),
               double(ib->GetNumElements()) * 8.0 / (1024*1024), ms(t0, t1));
    }

    printf("\n=== PROBE G: topology hash cost (ComputeHash on every DirtyTopology) ===\n");
    for (int nc : {1000, 100000}) {
        VtIntArray c(nc, NCV);
        HdBasisCurvesTopology src(HdTokens->cubic, HdTokens->bspline,
                                  HdTokens->nonperiodic, c, VtIntArray());
        auto t0 = Clock::now();
        volatile HdTopology::ID h = 0;
        for (int i = 0; i < 10; ++i) h = src.ComputeHash();
        auto t1 = Clock::now();
        (void)h;
        printf("[%6d curves] ComputeHash = %.4f ms/call\n", nc, ms(t0, t1) / 10.0);
    }

    printf("\n=== PROBE H: pure memcpy reference for a points upload ===\n");
    {
        VtVec3fArray dst(exact);
        auto t0 = Clock::now();
        memcpy(dst.data(), ptsExact.cdata(), exact * sizeof(GfVec3f));
        auto t1 = Clock::now();
        printf("memcpy %zu GfVec3f (%.2f MB) = %.2f ms\n", exact,
               double(exact * 12) / (1024*1024), ms(t0, t1));
    }
    return 0;
}
