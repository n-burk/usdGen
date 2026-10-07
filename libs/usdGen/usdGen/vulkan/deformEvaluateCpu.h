// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

// deformEvaluateCpu.h — host mirror of deformEvaluate.comp's per-CV math.
//
// Every query runs the shader's operations in the shader's order (the same
// expression text, so the same FMA contraction applies): plain operators
// where the shader spells plain operators, std::fma/std::sqrt where it
// spells fma()/sqrt(). Queries are independent, so any range partition and
// any block alignment evaluates bitwise what the direct shader did; the
// non-finite flag is a single OR bit. Non-finite CVs and outputs are left
// unwritten, exactly like the shader's early returns.
//
// The W-wide blocking mirrors rbfField.cpp's DisplaceBlocked: W queries
// share one sample pass so the fp64 sqrt/FMA pipelines stay fed (a
// one-CV-at-a-time port exposes the full sqrt latency and runs ~15x
// slower). Twelve is the measured sweet spot: eight leaves vector-sqrt
// throughput exposed, sixteen spills, thirty-two falls off a cliff. The
// tail runs the same scalar expression text as the block.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace usdGen::vulkan {

struct DeformEvalCpuParams {
    double const* samples = nullptr; // 3*n host-centered doubles (sample - center)
    double const* coef = nullptr;    // 3*m column-major doubles
    int n = 0;                       // samples
    int m = 0;                       // n + 4
    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
    double invScale = 1.0;
    double scale = 1.0;
};

namespace deformCpuDetail {

// Exponent-bit finiteness: all-set exponent bits iff infinite or NaN (sNaN
// included), exactly `!(isinf || isnan)` on each lane with no FP work, so
// the per-CV checks run on the integer pipe (which the sample loop's FP
// mix leaves idle) instead of fabs/fcmp/fccmp plus short-circuit
// branches. The verdicts, the skip pattern, and the bad flag are
// unchanged, so every output bit matches.
inline bool FiniteF(float v) noexcept
{
    uint32_t u;
    std::memcpy(&u, &v, sizeof u);
    return (u & 0x7F800000u) != 0x7F800000u;
}
inline bool FiniteD(double v) noexcept
{
    uint64_t u;
    std::memcpy(&u, &v, sizeof u);
    return (u & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

// One W-wide block over qs[0..W) -> ds[0..W): shader text, lane-arrayed so
// the vectorizer contracts across lanes. `bad` accumulates the non-finite
// flag. W must divide nothing in particular; the caller handles the tail.
template <size_t W>
inline void EvalBlock(DeformEvalCpuParams const& p, float const* qs, float* ds,
                      bool& bad) noexcept
{
    double qx[W], qy[W], qz[W];
    bool skip[W];
    for (size_t t = 0; t < W; ++t) {
        float const px = qs[t * 3], py = qs[t * 3 + 1], pz = qs[t * 3 + 2];
        bool const ok = FiniteF(px) && FiniteF(py) && FiniteF(pz);
        skip[t] = !ok;
        bad = bad || !ok;
        qx[t] = px;
        qy[t] = py;
        qz[t] = pz;
    }
    // Normalizer: (double(p) - c) * invScale, verbatim.
    double x[W], y[W], z[W];
    for (size_t t = 0; t < W; ++t) {
        x[t] = (qx[t] - p.cx) * p.invScale;
        y[t] = (qy[t] - p.cy) * p.invScale;
        z[t] = (qz[t] - p.cz) * p.invScale;
    }
    // Polynomial part, verbatim (last 4 coefficients of each column).
    double ox[W], oy[W], oz[W];
    double const* cx = p.coef;
    double const* cy = p.coef + size_t(p.m);
    double const* cz = p.coef + size_t(2) * size_t(p.m);
    size_t const n = size_t(p.n);
    for (size_t t = 0; t < W; ++t) {
        ox[t] = x[t] + cx[n] + cx[n + 1] * x[t] + cx[n + 2] * y[t] + cx[n + 3] * z[t];
        oy[t] = y[t] + cy[n] + cy[n + 1] * x[t] + cy[n + 2] * y[t] + cy[n + 3] * z[t];
        oz[t] = z[t] + cz[n] + cz[n + 1] * x[t] + cz[n + 2] * y[t] + cz[n + 3] * z[t];
    }
    for (size_t j = 0; j < n; ++j) {
        double const sx = p.samples[j * 3], sy = p.samples[j * 3 + 1], sz = p.samples[j * 3 + 2];
        double kk[W];
        for (size_t t = 0; t < W; ++t) {
            double const dx = std::fma(-sx, p.invScale, x[t]);
            double const dy = std::fma(-sy, p.invScale, y[t]);
            double const dz = std::fma(-sz, p.invScale, z[t]);
            double const rr = std::sqrt(std::fma(dz, dz, std::fma(dx, dx, dy * dy)));
            kk[t] = rr * rr * rr;
        }
        for (size_t t = 0; t < W; ++t) {
            ox[t] += cx[j] * kk[t];
            oy[t] += cy[j] * kk[t];
            oz[t] += cz[j] * kk[t];
        }
    }
    for (size_t t = 0; t < W; ++t) {
        if (skip[t]) continue;
        double const oxw = ox[t] * p.scale + p.cx;
        double const oyw = oy[t] * p.scale + p.cy;
        double const ozw = oz[t] * p.scale + p.cz;
        if (!FiniteD(oxw) || !FiniteD(oyw) || !FiniteD(ozw)) {
            bad = true;
            continue;
        }
        ds[t * 3] = float(oxw);
        ds[t * 3 + 1] = float(oyw);
        ds[t * 3 + 2] = float(ozw);
    }
}

// Scalar tail query: the same expression text as EvalBlock's lanes.
inline void EvalOne(DeformEvalCpuParams const& p, float const* q, float* d,
                    bool& bad) noexcept
{
    float const px = q[0], py = q[1], pz = q[2];
    if (!FiniteF(px) || !FiniteF(py) || !FiniteF(pz)) {
        bad = true;
        return;
    }
    double const x = (double(px) - p.cx) * p.invScale;
    double const y = (double(py) - p.cy) * p.invScale;
    double const z = (double(pz) - p.cz) * p.invScale;
    double const* cx = p.coef;
    double const* cy = p.coef + size_t(p.m);
    double const* cz = p.coef + size_t(2) * size_t(p.m);
    size_t const n = size_t(p.n);
    double ox = x + cx[n] + cx[n + 1] * x + cx[n + 2] * y + cx[n + 3] * z;
    double oy = y + cy[n] + cy[n + 1] * x + cy[n + 2] * y + cy[n + 3] * z;
    double oz = z + cz[n] + cz[n + 1] * x + cz[n + 2] * y + cz[n + 3] * z;
    for (size_t j = 0; j < n; ++j) {
        double const dx = std::fma(-p.samples[j * 3], p.invScale, x);
        double const dy = std::fma(-p.samples[j * 3 + 1], p.invScale, y);
        double const dz = std::fma(-p.samples[j * 3 + 2], p.invScale, z);
        double r = std::sqrt(std::fma(dz, dz, std::fma(dx, dx, dy * dy)));
        r = r * r * r;
        ox += cx[j] * r;
        oy += cy[j] * r;
        oz += cz[j] * r;
    }
    double const oxw = ox * p.scale + p.cx;
    double const oyw = oy * p.scale + p.cy;
    double const ozw = oz * p.scale + p.cz;
    if (!FiniteD(oxw) || !FiniteD(oyw) || !FiniteD(ozw)) {
        bad = true;
        return;
    }
    d[0] = float(oxw);
    d[1] = float(oyw);
    d[2] = float(ozw);
}

} // namespace deformCpuDetail

// Evaluates `count` CVs from qs (3 floats each) to ds (3 floats each).
// Bitwise deformEvaluate.comp over the same inputs. ORs 1 into *flag when
// any query or output is non-finite (leaving that output unwritten).
// Single-threaded; callers partition ranges across threads freely.
inline bool DeformEvaluateCpu(DeformEvalCpuParams const& p, float const* qs, float* ds,
                              size_t count, uint32_t* flag) noexcept
{
    if (count == 0) return true;
    if (!qs || !ds || !p.samples || !p.coef || !flag || p.n < 1 || p.m != p.n + 4)
        return false;
    constexpr size_t W = 12;
    bool bad = false;
    size_t t = 0, blocks = count / W;
    for (size_t b = 0; b < blocks; ++b, t += W)
        deformCpuDetail::EvalBlock<W>(p, qs + t * 3, ds + t * 3, bad);
    for (; t < count; ++t) deformCpuDetail::EvalOne(p, qs + t * 3, ds + t * 3, bad);
    if (bad) *flag |= 1u;
    return true;
}

} // namespace usdGen::vulkan
