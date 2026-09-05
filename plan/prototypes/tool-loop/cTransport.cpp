// cTransport.cpp -- emulates the usdGen imaging library's C surface, the same
// shape as usdRig's registry.h C entry points, extended with a live-override
// setter and a curves reader.  No USD types in the ABI: raw float*.
#include <cstring>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct Store {
    std::unordered_map<std::string, std::vector<float>> points;
    long long generation = 0;
};
Store& G() { static Store s; return s; }
}

extern "C" {

// Allocate/replace the stored array for a prim (simulates the first publish).
int UsdGenImaging_Resize(const char* path, int nPoints)
{
    G().points[path].assign(size_t(nPoints) * 3, 0.0f);
    ++G().generation;
    return 0;
}

// Full-array live override: the caller hands a float* to 3*nPoints floats.
// We memcpy into our own storage exactly as a snapshot publish would.
int UsdGenImaging_SetLiveOverride(const char* path, const float* xyz, int nPoints)
{
    auto& v = G().points[path];
    v.resize(size_t(nPoints) * 3);
    std::memcpy(v.data(), xyz, sizeof(float) * size_t(nPoints) * 3);
    ++G().generation;
    return 0;
}

// Sparse live override: only the CVs the brush touched.
int UsdGenImaging_SetLiveOverrideIndexed(const char* path, const int* idx,
                                         const float* xyz, int nIdx)
{
    auto it = G().points.find(path);
    if (it == G().points.end()) return 1;
    auto& v = it->second;
    for (int i = 0; i < nIdx; ++i) {
        const size_t d = size_t(idx[i]) * 3;
        if (d + 2 >= v.size()) return 2;
        v[d]     = xyz[i * 3];
        v[d + 1] = xyz[i * 3 + 1];
        v[d + 2] = xyz[i * 3 + 2];
    }
    ++G().generation;
    return 0;
}

// Zero-copy read: hand out a pointer into our storage.  Valid until the next
// publish that reallocates.  This is what a Python tool would wrap with
// numpy.ctypeslib.as_array / (c_float*n).from_address.
int UsdGenImaging_ReadCurvesPtr(const char* path, const float** outXyz, int* outN)
{
    auto it = G().points.find(path);
    if (it == G().points.end()) return 1;
    *outXyz = it->second.data();
    *outN = int(it->second.size() / 3);
    return 0;
}

// Copying read into a caller-owned buffer.
int UsdGenImaging_ReadCurvesCopy(const char* path, float* dst, int nPoints)
{
    auto it = G().points.find(path);
    if (it == G().points.end()) return 1;
    const size_t n = std::min(it->second.size(), size_t(nPoints) * 3);
    std::memcpy(dst, it->second.data(), sizeof(float) * n);
    return 0;
}

long long UsdGenImaging_GetGeneration() { return G().generation; }

// A no-op call with the same signature, to measure pure ctypes call overhead.
int UsdGenImaging_Noop(const char* path, const float* xyz, int nPoints)
{
    (void)path; (void)xyz; return nPoints;
}
}

// ---------------------------------------------------------------------------
// CPU CV picking, done in C++ so the Qt main thread pays only the call.
// vp is a row-major 4x4 (USD row-vector convention: clip = p * VP).
#include <cmath>
#include <limits>
extern "C" {

int UsdGenImaging_PickCV(const char* path, const float* vp,
                         int width, int height, float cx, float cy,
                         float radiusPx, int* outIdx, float* outDistPx)
{
    auto it = G().points.find(path);
    if (it == G().points.end()) return 1;
    const std::vector<float>& v = it->second;
    const size_t n = v.size() / 3;
    float best = radiusPx * radiusPx;
    int bestI = -1;
    for (size_t i = 0; i < n; ++i) {
        const float x = v[i*3], y = v[i*3+1], z = v[i*3+2];
        const float w = x*vp[3] + y*vp[7] + z*vp[11] + vp[15];
        if (w <= 0.0f) continue;
        const float inv = 1.0f / w;
        const float px = (x*vp[0] + y*vp[4] + z*vp[8]  + vp[12]) * inv;
        const float py = (x*vp[1] + y*vp[5] + z*vp[9]  + vp[13]) * inv;
        const float sx = (px * 0.5f + 0.5f) * float(width);
        const float sy = (1.0f - (py * 0.5f + 0.5f)) * float(height);
        const float dx = sx - cx, dy = sy - cy;
        const float d2 = dx*dx + dy*dy;
        if (d2 < best) { best = d2; bestI = int(i); }
    }
    *outIdx = bestI;
    *outDistPx = bestI < 0 ? -1.0f : std::sqrt(best);
    return 0;
}

// Indices of every CV within radiusPx of (cx,cy); writes at most maxOut.
int UsdGenImaging_FootprintCV(const char* path, const float* vp,
                              int width, int height, float cx, float cy,
                              float radiusPx, int* outIdx, int maxOut,
                              int* outCount)
{
    auto it = G().points.find(path);
    if (it == G().points.end()) return 1;
    const std::vector<float>& v = it->second;
    const size_t n = v.size() / 3;
    const float r2 = radiusPx * radiusPx;
    int k = 0;
    for (size_t i = 0; i < n && k < maxOut; ++i) {
        const float x = v[i*3], y = v[i*3+1], z = v[i*3+2];
        const float w = x*vp[3] + y*vp[7] + z*vp[11] + vp[15];
        if (w <= 0.0f) continue;
        const float inv = 1.0f / w;
        const float px = (x*vp[0] + y*vp[4] + z*vp[8]  + vp[12]) * inv;
        const float py = (x*vp[1] + y*vp[5] + z*vp[9]  + vp[13]) * inv;
        const float sx = (px * 0.5f + 0.5f) * float(width);
        const float sy = (1.0f - (py * 0.5f + 0.5f)) * float(height);
        const float dx = sx - cx, dy = sy - cy;
        if (dx*dx + dy*dy < r2) outIdx[k++] = int(i);
    }
    *outCount = k;
    return 0;
}
}
