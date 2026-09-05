// Probe: attribute the cost of HdSt_BasisCurvesPrimvarInterpolaterComputation's
// "VtArray<T> primvars(numVertsExpected);" -- an allocation + value-init that is
// thrown away in the common exact-size case (basisCurvesComputations.h:196-202).
#include "pxr/base/vt/array.h"
#include "pxr/base/gf/vec3f.h"
#include <chrono>
#include <cstdio>
PXR_NAMESPACE_USING_DIRECTIVE
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double,std::milli>(b-a).count(); }
int main() {
    for (size_t n : {size_t(50000), size_t(200000), size_t(800000)}) {
        double best = 1e9, bestcopy = 1e9;
        VtArray<GfVec3f> src(n);
        for (int r = 0; r < 20; ++r) {
            auto t0 = Clock::now();
            VtArray<GfVec3f> a(n);           // the thrown-away allocation
            auto t1 = Clock::now();
            volatile float f = a[n-1][0]; (void)f;
            best = std::min(best, ms(t0,t1));
            auto t2 = Clock::now();
            VtArray<GfVec3f> b(n);
            memcpy(b.data(), src.cdata(), n*sizeof(GfVec3f));
            auto t3 = Clock::now();
            volatile float g = b[n-1][0]; (void)g;
            bestcopy = std::min(bestcopy, ms(t2,t3));
        }
        printf("n=%7zu (%6.2f MB): VtArray(n) alloc+init = %.3f ms | "
               "alloc+memcpy = %.3f ms\n",
               n, n*12.0/(1024*1024), best, bestcopy);
    }
    printf("\nstaging-buffer threshold (hdSt/stagingBuffer.cpp:73): 512 KiB\n");
    printf("  512 KiB / 12 B per GfVec3f = %.0f points = %.0f curves at 8 CV\n",
           512.0*1024/12, 512.0*1024/12/8);
    return 0;
}
