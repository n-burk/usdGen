// Does g++ -O3 autovectorise a clump/frizz-like strip kernel on this aarch64
// host?  Compare AoS (GfVec3f interleaved, what VDF pools hold) against SoA
// (three planar float arrays) and against a compute-heavy SoA variant.
//
// Build twice: once plain -O3, once -O3 -fno-tree-vectorize, and diff.

#include "kernel.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Clock = std::chrono::steady_clock;
static double MsSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// SoA, all operands streamed from memory (16 flops/elem).
static void SoAStreamed(float *__restrict px, float *__restrict py,
                        float *__restrict pz, const float *__restrict rx,
                        const float *__restrict ry, const float *__restrict rz,
                        const float *__restrict tp, const float *__restrict nz,
                        size_t n, float amount)
{
    probe::StyleSoA(px, py, pz, rx, ry, rz, tp, nz, n, amount);
}

// SoA, t derived from the CV index inside the strip, root broadcast from the
// first CV of each strip: 3 arrays in, 3 arrays out, 16 flops/elem.
static void SoAStrip(float *__restrict px, float *__restrict py,
                     float *__restrict pz, size_t nCurves, float amount)
{
    static const float kT[probe::CVS] = {
        0.f, 1.f / 7, 2.f / 7, 3.f / 7, 4.f / 7, 5.f / 7, 6.f / 7, 1.f};
    for (size_t c = 0; c < nCurves; ++c) {
        const size_t b = c * probe::CVS;
        const float rx = px[b], ry = py[b], rz = pz[b];
#pragma GCC ivdep
        for (int k = 0; k < probe::CVS; ++k) {
            const float t = kT[k];
            const float w = amount * t;
            px[b + k] += ((rx + 0.3f * t) - px[b + k]) * w;
            py[b + k] += ((ry + 1.0f * t) - py[b + k]) * w;
            pz[b + k] += ((rz + 0.0f * t) - pz[b + k]) * w;
        }
    }
}

int main(int argc, char **argv)
{
    size_t nCurves = 100000;
    int iters = 200;
    if (argc > 1) nCurves = strtoull(argv[1], nullptr, 10);
    if (argc > 2) iters = atoi(argv[2]);
    const size_t n = nCurves * probe::CVS;

    std::vector<float> aos(n * 3), px(n), py(n), pz(n), rx(n), ry(n), rz(n),
        tp(n), nz(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = float(i % probe::CVS) / (probe::CVS - 1);
        px[i] = rx[i] = float(i % 512) * 0.01f;
        py[i] = ry[i] = t;
        pz[i] = rz[i] = float(i / 512) * 0.001f;
        tp[i] = t;
        nz[i] = probe::HashNoise(uint32_t(i)) * 0.01f;
        aos[i * 3 + 0] = px[i];
        aos[i * 3 + 1] = py[i];
        aos[i * 3 + 2] = pz[i];
    }

    const double flops = double(n) * 16.0;

    // AoS (interleaved GfVec3f) - what a VDF pool or VtVec3fArray holds.
    {
        auto t0 = Clock::now();
        for (int r = 0; r < iters; ++r) {
            probe::StyleAoS(aos.data(), 0, nCurves, 0.25f, 7u);
        }
        const double ms = MsSince(t0);
        printf("aos_interleaved   total_ms %9.2f  per_pass_ms %7.3f  "
               "GFLOP/s %6.2f  keep %.4f\n",
               ms, ms / iters, flops * iters / (ms * 1e6), aos[7]);
    }

    // SoA, all operands streamed.
    {
        auto t0 = Clock::now();
        for (int r = 0; r < iters; ++r) {
            SoAStreamed(px.data(), py.data(), pz.data(), rx.data(), ry.data(),
                        rz.data(), tp.data(), nz.data(), n, 0.25f);
        }
        const double ms = MsSince(t0);
        const double bytes = double(n) * 4.0 * 11.0;  // 8 read + 3 rmw
        printf("soa_streamed      total_ms %9.2f  per_pass_ms %7.3f  "
               "GFLOP/s %6.2f  GB/s %6.2f  keep %.4f\n",
               ms, ms / iters, flops * iters / (ms * 1e6),
               bytes * iters / (ms * 1e6), px[7]);
    }

    // SoA strip, t from the CV index.
    {
        auto t0 = Clock::now();
        for (int r = 0; r < iters; ++r) {
            SoAStrip(px.data(), py.data(), pz.data(), nCurves, 0.25f);
        }
        const double ms = MsSince(t0);
        const double bytes = double(n) * 4.0 * 6.0;  // 3 read-modify-write
        printf("soa_strip         total_ms %9.2f  per_pass_ms %7.3f  "
               "GFLOP/s %6.2f  GB/s %6.2f  keep %.4f\n",
               ms, ms / iters, flops * iters / (ms * 1e6),
               bytes * iters / (ms * 1e6), px[7]);
    }
    return 0;
}
