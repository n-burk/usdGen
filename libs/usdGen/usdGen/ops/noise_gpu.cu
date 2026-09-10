// usdGen — CUDA path for UsdGenNoiseOp::Capture (02 §2.7.1, 04 §2.9).
//
// One kernel pins the same fBm field the CPU loop in noise.cpp computes:
//   base[c]   = rootRest[c]*corr + (1-corr)*h_c      (precomputed on the host)
//   perCurve  = 2*fbm3(base[c]) - 1
//   field[i]  = 2*fbm3(base[c] + (0, 0, t[i]*freq)) - 1
// The device math mirrors thirdparty/seexpr/SeExpr2/Noise.cpp exactly:
// hashReduceChar<3>, NOISE_TABLES<3>::g (exact double copy; double grad ×
// double weight with float accumulation rounding as on CPU), the quintic
// s-curve, and the FBM<3,1,false,float> operation order
// (compiled with --fmad=false).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cuda_runtime.h>

#include "noise_tables_gen.cuh"  // __constant__ double G[514][3]

namespace usdgen_noise {

// Quintic s-curve (SeExpr2::s_curve: double s_curve(double), result assigned
// back to float alphas) — polynomial in double, unsuffixed constants.
__device__ __host__ float SCurve(float x)
{
    const double t = x;
    return (float)(t * t * t * (t * (t * 6 - 15) + 10));
}

// Perlin-style lattice hash (SeExpr2::hashReduceChar<3>), 3D scalar form.
__device__ unsigned char HashReduceChar(int x, int y, int z)
{
    uint32_t seed = 0;
    static const uint32_t M = 1664525;
    static const uint32_t C = 1013904223;
    const int index[3] = {x, y, z};
    for (int k = 0; k < 3; k++) seed = seed * M + (uint32_t)index[k] + C;
    seed ^= (seed >> 11);
    seed ^= (seed << 7) & 0x9d2c5680UL;
    seed ^= (seed << 15) & 0xefc60000UL;
    seed ^= (seed >> 18);
    return (unsigned char)((((seed & 0xff0000) >> 4) + (seed & 0xff)) & 0xff);
}

// 3D Perlin noise (SeExpr2::noiseHelper<3, float, false>), float path.
__device__ float Noise3(float x, float y, float z)
{
    const float X[3] = {x, y, z};
    int index[3];
    float weights[2][3];
    for (int k = 0; k < 3; k++) {
        const float f = floorf(X[k]);
        index[k] = (int)f;
        weights[0][k] = X[k] - f;
        weights[1][k] = weights[0][k] - 1;  // dist to cell with index one above
    }
    float vals[8];
    for (int dummy = 0; dummy < 8; dummy++) {
        int latticeIndex[3];
        int offset[3];
        for (int k = 0; k < 3; k++) {
            offset[k] = ((dummy & (1 << k)) != 0);
            latticeIndex[k] = index[k] + offset[k];
        }
        const int lookup = HashReduceChar(latticeIndex[0], latticeIndex[1], latticeIndex[2]);
        float val = 0;
        for (int k = 0; k < 3; k++) {
            // CPU: `double grad = g[lookup][k]; val += grad * weight;` with
            // float val — double product added in double, one float rounding
            // per accumulation step.
            val = (float)((double)val + (double)G[lookup][k] * (double)weights[offset[k]][k]);
        }
        vals[dummy] = val;
    }
    float alphas[3];
    for (int k = 0; k < 3; k++) alphas[k] = SCurve(weights[0][k]);
    // Multilinear (trilinear) interpolation.
    for (int newd = 2; newd >= 0; newd--) {
        const int newnum = 1 << newd;
        const int k = (3 - newd - 1);
        const float alpha = alphas[k];
        const float beta = 1.0f - alphas[k];
        for (int dummy = 0; dummy < newnum; dummy++) {
            const int index = dummy * (1 << (3 - newd));
            const int otherIndex = index + (1 << k);
            vals[index] = beta * vals[index] + alpha * vals[otherIndex];
        }
    }
    return vals[0];
}

// FBM (SeExpr2::FBM<3, 1, false, float>), non-turbulent.
__device__ float Fbm3(float x, float y, float z, int octaves, float lacunarity, float gain)
{
    float P[3];
    P[0] = x;
    P[1] = y;
    P[2] = z;
    float out = 0;
    float scale = 1;
    int octave = 0;
    while (1) {
        out += Noise3(P[0], P[1], P[2]) * scale;
        if (++octave >= octaves) break;
        scale *= gain;
        P[0] = P[0] * lacunarity + 1234.0f;
        P[1] = P[1] * lacunarity + 1234.0f;
        P[2] = P[2] * lacunarity + 1234.0f;
    }
    return out;
}

}  // namespace usdgen_noise

// One thread per curve. base[c*3+k] is the rest-pinned sample position,
// cvT[c*nCV+i] the CV t-value (hairT or the i/(cvCount-1) ramp).
__global__ void FbmKernel(
    const float * __restrict__ base,
    const float * __restrict__ cvT,
    float freq,
    int octaves,
    float lac,
    float gain,
    int nCurves,
    int nCV,
    float * __restrict__ field,
    float * __restrict__ perCurve)
{
    const int c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= nCurves) return;
    const float bx = base[c * 3 + 0];
    const float by = base[c * 3 + 1];
    const float bz = base[c * 3 + 2];
    perCurve[c] = 2.0f * usdgen_noise::Fbm3(bx, by, bz, octaves, lac, gain) - 1.0f;
    for (int i = 0; i < nCV; i++) {
        const float t = cvT[c * nCV + i];
        field[c * nCV + i] = 2.0f * usdgen_noise::Fbm3(bx, by, bz + t * freq, octaves, lac, gain)
                             - 1.0f;
    }
}

namespace usdGen {

// GPU path for UsdGenNoiseOp::Capture. Returns false (CPU fallback) if the
// device, any transfer, or the launch fails.
//
// Persistent plumbing (CUDA1b): stream, device scratch, and pinned staging
// are function-local statics created once and grown on demand. Capture runs
// sequentially on the scheduler commit thread (topo order), so no locking
// is needed. Per call: memcpy into pinned staging, two async H2D copies,
// the kernel, two async D2H copies, stream sync, memcpy out.
bool UsdGenNoiseCaptureGPU(const float *base,
                           const float *cvT,
                           float frequency,
                           int octaves,
                           float lacunarity,
                           float gain,
                           int nCurves,
                           int nCV,
                           float *field,
                           float *perCurve)
{
    if (nCurves <= 0 || nCV <= 0) return false;
    const size_t n = (size_t)nCurves * (size_t)nCV;
    const size_t nBase = (size_t)nCurves * 3u;

    static cudaStream_t stream = nullptr;
    static float *dBase = nullptr;   // nCurves*3 floats
    static float *dT = nullptr;      // n floats
    static float *dField = nullptr;  // n floats
    static float *dPcv = nullptr;    // nCurves floats
    static float *hIn = nullptr;     // pinned: base | cvT
    static float *hOut = nullptr;    // pinned: field | perCurve
    static size_t capBase = 0, capT = 0, capField = 0, capPcv = 0;
    static size_t capIn = 0, capOut = 0;

    if (stream == nullptr) {
        int dev = -1;
        if (cudaGetDevice(&dev) != cudaSuccess || dev < 0) return false;
        if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
            return false;
    }

    auto grow = [](float **slot, size_t &cap, size_t need, bool pinned) {
        if (need <= cap) return true;
        if (pinned) cudaFreeHost(*slot); else cudaFree(*slot);
        cudaError_t e =
            pinned ? cudaMallocHost(slot, need * sizeof(float))
                   : cudaMalloc(slot, need * sizeof(float));
        if (e != cudaSuccess) {
            *slot = nullptr;
            cap = 0;
            (void)cudaGetLastError();
            return false;
        }
        cap = need;
        return true;
    };
    bool ok = grow(&dBase, capBase, nBase, false) &&
              grow(&dT, capT, n, false) &&
              grow(&dField, capField, n, false) &&
              grow(&dPcv, capPcv, (size_t)nCurves, false) &&
              grow(&hIn, capIn, nBase + n, true) &&
              grow(&hOut, capOut, n + (size_t)nCurves, true);
    if (ok) {
        std::memcpy(hIn, base, nBase * sizeof(float));
        std::memcpy(hIn + nBase, cvT, n * sizeof(float));
        ok =
            cudaMemcpyAsync(dBase, hIn, nBase * sizeof(float), cudaMemcpyHostToDevice, stream) ==
                cudaSuccess &&
            cudaMemcpyAsync(dT, hIn + nBase, n * sizeof(float), cudaMemcpyHostToDevice, stream) ==
                cudaSuccess;
    }
    if (ok) {
        const int block = 128;
        const int grid = (nCurves + block - 1) / block;
        FbmKernel<<<grid, block, 0, stream>>>(dBase, dT, frequency, octaves, lacunarity, gain,
                                              nCurves, nCV, dField, dPcv);
        ok = cudaGetLastError() == cudaSuccess;
    }
    if (ok) {
        ok = cudaMemcpyAsync(hOut, dField, n * sizeof(float), cudaMemcpyDeviceToHost, stream) ==
                 cudaSuccess &&
             cudaMemcpyAsync(hOut + n, dPcv, (size_t)nCurves * sizeof(float),
                             cudaMemcpyDeviceToHost, stream) == cudaSuccess;
    }
    if (ok) ok = cudaStreamSynchronize(stream) == cudaSuccess;
    if (ok) {
        std::memcpy(field, hOut, n * sizeof(float));
        std::memcpy(perCurve, hOut + n, (size_t)nCurves * sizeof(float));
    }
    if (!ok) (void)cudaGetLastError();  // clear the sticky error for the CPU fallback
    return ok;
}

}  // namespace usdGen
