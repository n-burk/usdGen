// deformRbfHost.h — header-only CPU RBF solver + CPU oracle for the Vulkan
// RBF deform pipeline.
//
// CPU solve (SolveRbf) ports libs/usdGen/usdGen/gpu/rbf.cu exactly:
// extent/center/scale, full affine-rank guard on the polynomial Gram, the
// (n+4)x(n+4) bordered matrix build, in-place Doolittle LU with partial
// pivoting, and the three-column right-hand-side solve. Coefficients are
// emitted column-major m x 3 so the evaluate shader reads coef[k*m + i].
//
// CPU evaluate (RbfEvaluate) ports rbf.cu evalKernel; CPU apply (RbfApply)
// ports gpu/deformCurves.cu ApplyDeformation so the test can build a byte-for-
// byte oracle for the two GPU dispatches. The plan v1 supports the groom and
// mask envelopes only (primitive/point envelopes are the legacy null case,
// i.e. factor 1.0).
//
// Both the pipeline (solve only) and the test (solve + evaluate + apply)
// include this header.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace usdGen::vulkan {

struct float3 { float x, y, z; };


struct RbfStatus { enum class Code { Ok, RankDeficient, SolverError, NonFinite } code; };

struct RbfState {
    int n = 0, m = 0;
    double center[3] = {0, 0, 0};
    double scale = 1.0;
    double coef[3 * 104] = {0};  // m*3, m<=104 (n<=100)
    RbfStatus::Code status = RbfStatus::Code::Ok;
};

inline float float3Component(float3 p, int ax) { return ax == 0 ? p.x : ax == 1 ? p.y : p.z; }

// fullAffineRank — port of rbf.cu:46-56. Scaled Gaussian elimination on a
// 4x4 Gram copy with a relative pivot threshold; false => coplanar/tilted
// sample set that cannot support the affine polynomial null space.
inline bool fullAffineRank(double g[16]) {
    double maxDiag = 0.0;
    for (int i = 0; i < 4; ++i) maxDiag = std::max(maxDiag, std::abs(g[i * 4 + i]));
    if (!(maxDiag > 0.0) || !std::isfinite(maxDiag)) return false;
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(g[r * 4 + c]) > std::abs(g[pivot * 4 + c])) pivot = r;
        if (std::abs(g[pivot * 4 + c]) <= maxDiag * 1e-11) return false;
        if (pivot != c)
            for (int k = c; k < 4; ++k) std::swap(g[c * 4 + k], g[pivot * 4 + k]);
        for (int r = c + 1; r < 4; ++r) {
            double q = g[r * 4 + c] / g[c * 4 + c];
            for (int k = c; k < 4; ++k) g[r * 4 + k] -= q * g[c * 4 + k];
        }
    }
    return true;
}

// SolveRbf — port of rbf.cu (polynomialGram + fullAffineRank + buildMatrix +
// rhsKernel + zeroTail) with an in-place Doolittle LU solve. m = n + 4.
// On success fills out.{n,m,center,scale,coef,status}; on failure sets
// out.status and returns leaving coef undefined.
inline void SolveRbf(float3 const* rest, float3 const* posed, int n, double smoothing,
                     RbfState& out) {
    out.status = RbfStatus::Code::Ok;
    const int m = n + 4;

    // 1. Extent & rank (rest samples).
    double mn[3] = {float3Component(rest[0], 0), float3Component(rest[0], 1),
                    float3Component(rest[0], 2)};
    double mx[3] = {mn[0], mn[1], mn[2]};
    for (int i = 0; i < n; ++i) {
        for (int ax = 0; ax < 3; ++ax) {
            double v = float3Component(rest[i], ax);
            if (!std::isfinite(v)) { out.status = RbfStatus::Code::NonFinite; return; }
            mn[ax] = std::min(mn[ax], v);
            mx[ax] = std::max(mx[ax], v);
        }
    }
    double cx = (mn[0] + mx[0]) * 0.5, cy = (mn[1] + mx[1]) * 0.5, cz = (mn[2] + mx[2]) * 0.5;
    double scale = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]});
    if (!(scale > 0.0) || !std::isfinite(scale)) { out.status = RbfStatus::Code::RankDeficient; return; }
    double invScale = 1.0 / scale;

    // Polynomial Gram G[4][4].
    double G[16] = {0};
    for (int i = 0; i < n; ++i) {
        double v[4] = {1.0, (float3Component(rest[i], 0) - cx) * invScale,
                       (float3Component(rest[i], 1) - cy) * invScale,
                       (float3Component(rest[i], 2) - cz) * invScale};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) G[r * 4 + c] += v[r] * v[c];
    }
    if (!fullAffineRank(G)) { out.status = RbfStatus::Code::RankDeficient; return; }

    // 2. Build bordered A[m][m] (row-major logical).
    std::vector<double> A(m * m, 0.0);
    auto poly = [&](int row, int q) -> double {
        return q == 0 ? 1.0 : q == 1 ? (float3Component(rest[row], 0) - cx) * invScale
                                     : q == 2 ? (float3Component(rest[row], 1) - cy) * invScale
                                              : (float3Component(rest[row], 2) - cz) * invScale;
    };
    for (int r = 0; r < m; ++r) {
        for (int c = 0; c < m; ++c) {
            double value = 0.0;
            if (r < n && c < n) {
                double dx = (float3Component(rest[r], 0) - cx) * invScale -
                            (float3Component(rest[c], 0) - cx) * invScale;
                double dy = (float3Component(rest[r], 1) - cy) * invScale -
                            (float3Component(rest[c], 1) - cy) * invScale;
                double dz = (float3Component(rest[r], 2) - cz) * invScale -
                            (float3Component(rest[c], 2) - cz) * invScale;
                double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                value = dist * dist * dist + (r == c ? smoothing : 0.0);
            } else if (r < n) {
                value = poly(r, c - n);
            } else if (c < n) {
                value = poly(c, r - n);
            }
            if (!std::isfinite(value)) { out.status = RbfStatus::Code::NonFinite; return; }
            A[r * m + c] = value;
        }
    }

    // 3. In-place Doolittle LU with partial pivoting.
    int perm[104];
    for (int i = 0; i < m; ++i) perm[i] = i;
    for (int c = 0; c < m; ++c) {
        int p = c;
        for (int r = c + 1; r < m; ++r)
            if (std::abs(A[r * m + c]) > std::abs(A[p * m + c])) p = r;
        if (!(std::abs(A[p * m + c]) > 1e-300)) { out.status = RbfStatus::Code::RankDeficient; return; }
        if (p != c) {
            for (int k = 0; k < m; ++k) std::swap(A[c * m + k], A[p * m + k]);
            std::swap(perm[c], perm[p]);
        }
        for (int r = c + 1; r < m; ++r) {
            double f = A[r * m + c] / A[c * m + c];
            A[r * m + c] = f;
            for (int k = c + 1; k < m; ++k) {
                A[r * m + k] -= f * A[c * m + k];
                if (!std::isfinite(A[r * m + k])) { out.status = RbfStatus::Code::NonFinite; return; }
            }
        }
    }

    // 4. RHS B[m][3] logical: scaled (posed - rest) for rows < n, zero tail.
    std::vector<double> B(m * 3, 0.0);
    for (int i = 0; i < n; ++i) {
        for (int ax = 0; ax < 3; ++ax) {
            double pv = float3Component(posed[i], ax);
            if (!std::isfinite(pv)) { out.status = RbfStatus::Code::NonFinite; return; }
            B[i * 3 + ax] = (pv - float3Component(rest[i], ax)) * invScale;
        }
    }

    // 5. Solve L X = P b, then U X = that, for all three columns.
    std::vector<double> X(m * 3, 0.0);
    std::vector<double> bvec(m);
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < m; ++i) bvec[i] = B[perm[i] * 3 + k];
        for (int i = 0; i < m; ++i) {  // forward substitution with L
            double s = 0.0;
            for (int j = 0; j < i; ++j) s += A[i * m + j] * bvec[j];
            bvec[i] -= s;
        }
        for (int i = m - 1; i >= 0; --i) {  // back substitution with U
            double s = 0.0;
            for (int j = i + 1; j < m; ++j) s += A[i * m + j] * bvec[j];
            bvec[i] = (bvec[i] - s) / A[i * m + i];
        }
        for (int i = 0; i < m; ++i) X[i * 3 + k] = bvec[i];
    }

    // Column-major output: coef[k*m + i] = X[i][k].
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < m; ++i) {
            double v = X[i * 3 + k];
            if (!std::isfinite(v)) { out.status = RbfStatus::Code::SolverError; return; }
            out.coef[k * m + i] = v;
        }

    out.n = n; out.m = m;
    out.center[0] = cx; out.center[1] = cy; out.center[2] = cz;
    out.scale = scale;
    out.status = RbfStatus::Code::Ok;
}

// RbfEvaluate — port of rbf.cu:93-102 evalKernel, double inner, float out.
inline float3 RbfEvaluate(RbfState const& s, float3 const* rest, float3 p) {
    int const n = s.n, m = s.m;
    double const cx = s.center[0], cy = s.center[1], cz = s.center[2];
    double const invScale = 1.0 / s.scale, scale = s.scale;
    double x = (p.x - cx) * invScale, y = (p.y - cy) * invScale, z = (p.z - cz) * invScale;
    double ox = x + s.coef[n] + s.coef[n + 1] * x + s.coef[n + 2] * y + s.coef[n + 3] * z;
    double oy = y + s.coef[m + n] + s.coef[m + n + 1] * x + s.coef[m + n + 2] * y +
                s.coef[m + n + 3] * z;
    double oz = z + s.coef[2 * m + n] + s.coef[2 * m + n + 1] * x + s.coef[2 * m + n + 2] * y +
                s.coef[2 * m + n + 3] * z;
    for (int j = 0; j < n; ++j) {
        float3 q = rest[j];
        double dx = x - (q.x - cx) * invScale;
        double dy = y - (q.y - cy) * invScale;
        double dz = z - (q.z - cz) * invScale;
        double r = std::sqrt(dx * dx + dy * dy + dz * dz);
        r = r * r * r;
        ox += s.coef[j] * r;
        oy += s.coef[m + j] * r;
        oz += s.coef[2 * m + j] * r;
    }
    return {float(ox * scale + cx), float(oy * scale + cy), float(oz * scale + cz)};
}

// RbfApply — oracle mirroring gpu/deformCurves.cu ApplyDeformation. Domain
// codes: Groom=1, Primitive=2, Point=4. Groom + mask envelopes only (v1).
// Returns false on the first semantic error (oracle is exercised only on
// valid cases, where it returns true and fills `output` with pointCount vec3).
struct DeformApplyParams {
    uint32_t curveCount = 0, pointCount = 0;
    uint32_t const* offsets = nullptr;
    float3 const* targets = nullptr;
    float groomEnvelope = 1.0f;
    // mask (scalar): literal/hasData/domain/data/count
    float maskLiteral = 1.0f;
    bool maskHasData = false;
    uint32_t maskDomain = 1;
    float const* maskData = nullptr;
    uint32_t maskCount = 0;
    // enabled (bool, groom-only)
    uint32_t enabledLiteral = 1;
    bool enabledHasData = false;
    uint32_t enabledDomain = 1;
    uint32_t const* enabledData = nullptr;
    uint32_t enabledCount = 0;
    // lockRoots (bool, groom or primitive)
    uint32_t lockLiteral = 0;
    bool lockHasData = false;
    uint32_t lockDomain = 1;
    uint32_t const* lockData = nullptr;
    uint32_t lockCount = 0;
};

inline bool RbfApply(DeformApplyParams const& p, float3 const* src, float3 const* warped,
                     std::vector<float3>* output) {
    auto finite = [](float v) { return std::isfinite(v); };
    output->resize(p.pointCount);
    for (uint32_t curve = 0; curve < p.curveCount; ++curve) {
        uint32_t begin = p.offsets[curve], end = p.offsets[curve + 1];
        if (begin >= end || end > p.pointCount) return false;

        uint32_t enabledRaw =
            p.enabledHasData ? p.enabledData[curve] : p.enabledLiteral;  // groom-only => index by curve
        if (enabledRaw > 1u) return false;
        bool groomEnabled = enabledRaw != 0u;

        uint32_t lockIdx = curve;  // Groom(1) and Primitive(2) both index by curve
        uint32_t lockRaw = p.lockHasData ? p.lockData[lockIdx] : p.lockLiteral;
        if (lockRaw > 1u) return false;
        bool lock = lockRaw != 0u;

        float3 correction = {0.0f, 0.0f, 0.0f};
        if (lock) {
            correction.x = p.targets[curve].x - warped[begin].x;
            correction.y = p.targets[curve].y - warped[begin].y;
            correction.z = p.targets[curve].z - warped[begin].z;
            if (!finite(correction.x) || !finite(correction.y) || !finite(correction.z))
                return false;
        }

        for (uint32_t point = begin; point < end; ++point) {
            float3 s = src[point], w = warped[point];
            if (!finite(s.x) || !finite(s.y) || !finite(s.z) || !finite(w.x) || !finite(w.y) ||
                !finite(w.z))
                return false;

            uint32_t maskIdx = p.maskDomain == 4 ? point : curve;  // Groom/Prim by curve, Point by point
            float maskValue =
                p.maskHasData ? p.maskData[maskIdx] : p.maskLiteral;
            if (!finite(maskValue)) return false;
            if (maskValue < 0.0f || maskValue > 1.0f) return false;

            float envelope = p.groomEnvelope * maskValue;
            if (!finite(envelope)) return false;
            envelope = std::min(1.0f, std::max(0.0f, envelope));

            float3 result = s;
            if (groomEnabled && envelope > 0.0f) {
                float3 destination = lock ? float3{w.x + correction.x, w.y + correction.y,
                                                    w.z + correction.z}
                                          : w;
                result.x = s.x + (destination.x - s.x) * envelope;
                result.y = s.y + (destination.y - s.y) * envelope;
                result.z = s.z + (destination.z - s.z) * envelope;
            }
            if (!finite(result.x) || !finite(result.y) || !finite(result.z)) return false;
            (*output)[point] = result;
        }
    }
    return true;
}

}  // namespace usdGen::vulkan
