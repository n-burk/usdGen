// Shared "styler" kernel used by every engine variant so the arithmetic is
// identical across VDF / TBB / SoA measurements.
//
// Clump/frizz-like: per curve of CVS control vertices, lerp each CV toward a
// target derived from the curve root plus a cheap hash-noise offset.
#ifndef USDGEN_PROBE_KERNEL_H
#define USDGEN_PROBE_KERNEL_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace probe {

static constexpr int CVS = 8;

inline float HashNoise(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    // [-1, 1)
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// AoS kernel over an interleaved float[3] buffer, curves [begin, end).
// 'seed' distinguishes stylers in the chain.
inline void StyleAoS(float *p, size_t begin, size_t end,
                     float amount, uint32_t seed)
{
    for (size_t c = begin; c < end; ++c) {
        float *cv = p + c * CVS * 3;
        const float rx = cv[0], ry = cv[1], rz = cv[2];
        for (int k = 0; k < CVS; ++k) {
            const float t = (float)k * (1.0f / (CVS - 1));
            const uint32_t h = (uint32_t)c * 9781u + (uint32_t)k * 6271u + seed;
            const float n = HashNoise(h) * 0.01f;
            const float tx = rx + 0.3f * t + n;
            const float ty = ry + 1.0f * t;
            const float tz = rz + n;
            const float w = amount * t;
            cv[k * 3 + 0] += (tx - cv[k * 3 + 0]) * w;
            cv[k * 3 + 1] += (ty - cv[k * 3 + 1]) * w;
            cv[k * 3 + 2] += (tz - cv[k * 3 + 2]) * w;
        }
    }
}

// SoA kernel over three separate float arrays. Same math, layout changed so
// the compiler can vectorise across CVs of consecutive curves.
inline void StyleSoA(float *__restrict px, float *__restrict py,
                     float *__restrict pz,
                     const float *__restrict rx, const float *__restrict ry,
                     const float *__restrict rz,
                     const float *__restrict tparam,
                     const float *__restrict noise,
                     size_t n, float amount)
{
    for (size_t i = 0; i < n; ++i) {
        const float t = tparam[i];
        const float nn = noise[i];
        const float w = amount * t;
        px[i] += ((rx[i] + 0.3f * t + nn) - px[i]) * w;
        py[i] += ((ry[i] + 1.0f * t) - py[i]) * w;
        pz[i] += ((rz[i] + nn) - pz[i]) * w;
    }
}


// Per-CV kernel over the half-open element range [b, e) of a flat CV buffer.
// Reads NO neighbouring element: safe when VDF splits a pool into 500-element
// invocations that do not align with curve boundaries.
inline void StyleCVRange(float *p, size_t b, size_t e, float amount,
                         uint32_t seed)
{
    for (size_t i = b; i < e; ++i) {
        const size_t c = i / CVS;
        const int k = int(i % CVS);
        const float t = (float)k * (1.0f / (CVS - 1));
        const float rx = (float)(c % 512) * 0.01f;
        const float ry = 0.0f;
        const float rz = (float)(c / 512) * 0.01f;
        const uint32_t h = (uint32_t)c * 9781u + (uint32_t)k * 6271u + seed;
        const float n = HashNoise(h) * 0.01f;
        const float w = amount * t;
        float *cv = p + i * 3;
        cv[0] += ((rx + 0.3f * t + n) - cv[0]) * w;
        cv[1] += ((ry + 1.0f * t) - cv[1]) * w;
        cv[2] += ((rz + n) - cv[2]) * w;
    }
}

// One curve = one data-flow element.
struct HairStrip {
    float cv[CVS][3];
    bool operator==(const HairStrip &o) const {
        return memcmp(cv, o.cv, sizeof(cv)) == 0;
    }
    bool operator!=(const HairStrip &o) const { return !(*this == o); }
};

inline void StyleStripRange(HairStrip *s, size_t b, size_t e, float amount,
                            uint32_t seed)
{
    for (size_t c = b; c < e; ++c) {
        HairStrip &h = s[c];
        const float rx = h.cv[0][0], ry = h.cv[0][1], rz = h.cv[0][2];
        for (int k = 0; k < CVS; ++k) {
            const float t = (float)k * (1.0f / (CVS - 1));
            const uint32_t hh = (uint32_t)c * 9781u + (uint32_t)k * 6271u + seed;
            const float n = HashNoise(hh) * 0.01f;
            const float w = amount * t;
            h.cv[k][0] += ((rx + 0.3f * t + n) - h.cv[k][0]) * w;
            h.cv[k][1] += ((ry + 1.0f * t) - h.cv[k][1]) * w;
            h.cv[k][2] += ((rz + n) - h.cv[k][2]) * w;
        }
    }
}

}  // namespace probe

#endif
