// Probe: the CPU half of the prim-granularity question.
// HdStResourceRegistry::_Commit resolves pending buffer sources with
// WorkParallelForTBBRange (resourceRegistry.cpp:868-871), so the per-prim
// buffer-source Resolve() work parallelises across PRIMS but never within one.
// Measure 100k x 8-CV curves as 1 / 8 / 32 / 128 / 1000 buffer sources.
#include "pxr/pxr.h"
#include "pxr/base/work/loops.h"
#include "pxr/imaging/hd/basisCurvesTopology.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdSt/basisCurvesComputations.h"
#include "pxr/imaging/hdSt/basisCurvesTopology.h"
#include <chrono>
#include <cstdio>
#include <vector>
#include <thread>
PXR_NAMESPACE_USING_DIRECTIVE
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double,std::milli>(b-a).count(); }

int main() {
    const int NCURVES = 100000, NCV = 8;
    VtVec3fArray all(size_t(NCURVES)*NCV);
    for (size_t i = 0; i < all.size(); ++i)
        all[i] = GfVec3f(float(i)*1e-6f, float(i%8)*0.1f, 0.f);

    printf("100000 curves x 8 CV = %zu points (%.2f MB), %u hw threads\n",
           all.size(), all.size()*12.0/1e6, std::thread::hardware_concurrency());
    printf("%-8s %-10s %14s %14s %14s\n", "chunks", "pts/chunk",
           "serial (ms)", "parallel (ms)", "speedup");

    for (int nch : {1, 8, 32, 128, 1000}) {
        const int per = NCURVES / nch;
        VtIntArray counts(per, NCV);
        HdBasisCurvesTopology src(HdTokens->cubic, HdTokens->bspline,
                                  HdTokens->nonperiodic, counts, VtIntArray());
        HdSt_BasisCurvesTopologySharedPtr topo = HdSt_BasisCurvesTopology::New(src);

        std::vector<VtVec3fArray> chunkPts(nch);
        for (int c = 0; c < nch; ++c) {
            chunkPts[c] = VtVec3fArray(size_t(per)*NCV);
            memcpy(chunkPts[c].data(), all.cdata() + size_t(c)*per*NCV,
                   size_t(per)*NCV*sizeof(GfVec3f));
        }
        auto build = [&](int c) {
            return std::make_shared<
                HdSt_BasisCurvesPrimvarInterpolaterComputation<GfVec3f>>(
                topo, chunkPts[c], SdfPath("/p"), HdTokens->points,
                HdInterpolationVertex, GfVec3f(1,0,0), HdTypeFloatVec3);
        };
        double bestSerial = 1e9, bestPar = 1e9;
        for (int rep = 0; rep < 5; ++rep) {
            {   std::vector<HdBufferSourceSharedPtr> srcs(nch);
                for (int c = 0; c < nch; ++c) srcs[c] = build(c);
                auto t0 = Clock::now();
                for (int c = 0; c < nch; ++c) srcs[c]->Resolve();
                auto t1 = Clock::now();
                bestSerial = std::min(bestSerial, ms(t0,t1));
            }
            {   std::vector<HdBufferSourceSharedPtr> srcs(nch);
                for (int c = 0; c < nch; ++c) srcs[c] = build(c);
                auto t0 = Clock::now();
                WorkParallelForN(size_t(nch), [&](size_t b, size_t e) {
                    for (size_t i = b; i < e; ++i) srcs[i]->Resolve(); });
                auto t1 = Clock::now();
                bestPar = std::min(bestPar, ms(t0,t1));
            }
        }
        printf("%-8d %-10d %14.3f %14.3f %13.2fx\n",
               nch, per*NCV, bestSerial, bestPar, bestSerial/bestPar);
    }

    printf("\n--- same split, but the TOPOLOGY index build (DirtyTopology path) ---\n");
    printf("%-8s %-14s %14s %14s %14s\n", "chunks", "patches/chunk",
           "serial (ms)", "parallel (ms)", "speedup");
    for (int nch : {1, 8, 32, 128, 1000}) {
        const int per = NCURVES / nch;
        VtIntArray counts(per, NCV);
        std::vector<HdBasisCurvesTopology> srcs;
        double bestSerial = 1e9, bestPar = 1e9;
        size_t patches = 0;
        for (int rep = 0; rep < 3; ++rep) {
            {   std::vector<HdSt_BasisCurvesTopologySharedPtr> ts(nch);
                for (int c = 0; c < nch; ++c) ts[c] = HdSt_BasisCurvesTopology::New(
                    HdBasisCurvesTopology(HdTokens->cubic, HdTokens->bspline,
                        HdTokens->nonperiodic, counts, VtIntArray()));
                std::vector<HdBufferSourceSharedPtr> ib(nch);
                for (int c = 0; c < nch; ++c) ib[c] = ts[c]->GetIndexBuilderComputation(false);
                auto t0 = Clock::now();
                for (int c = 0; c < nch; ++c) ib[c]->Resolve();
                auto t1 = Clock::now();
                bestSerial = std::min(bestSerial, ms(t0,t1));
                patches = ib[0]->GetNumElements();
            }
            {   std::vector<HdSt_BasisCurvesTopologySharedPtr> ts(nch);
                for (int c = 0; c < nch; ++c) ts[c] = HdSt_BasisCurvesTopology::New(
                    HdBasisCurvesTopology(HdTokens->cubic, HdTokens->bspline,
                        HdTokens->nonperiodic, counts, VtIntArray()));
                std::vector<HdBufferSourceSharedPtr> ib(nch);
                for (int c = 0; c < nch; ++c) ib[c] = ts[c]->GetIndexBuilderComputation(false);
                auto t0 = Clock::now();
                WorkParallelForN(size_t(nch), [&](size_t b, size_t e) {
                    for (size_t i = b; i < e; ++i) ib[i]->Resolve(); });
                auto t1 = Clock::now();
                bestPar = std::min(bestPar, ms(t0,t1));
            }
        }
        printf("%-8d %-14zu %14.3f %14.3f %13.2fx\n",
               nch, patches, bestSerial, bestPar, bestSerial/bestPar);
    }
    return 0;
}
