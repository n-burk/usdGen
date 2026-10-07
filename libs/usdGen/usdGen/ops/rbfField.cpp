#include "usdGen/ops/rbfField.h"

#include "pxr/base/gf/vec3i.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_set>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {
namespace rbf {

namespace {

double Cube(double r) { return r * r * r; }

bool Finite(GfVec3d const &p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// The rank test gpu::CudaRbfBinding applies to the polynomial Gram matrix:
// scaled elimination with a relative threshold, which also catches tilted
// coplanar layouts that never produce an exact zero.
bool FullAffineRank(std::vector<GfVec3d> const &y)
{
    double g[16] = {};
    for (GfVec3d const &p : y) {
        double const v[4] = {1.0, p[0], p[1], p[2]};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) g[r * 4 + c] += v[r] * v[c];
    }
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
            double const q = g[r * 4 + c] / g[c * 4 + c];
            for (int k = c; k < 4; ++k) g[r * 4 + k] -= q * g[c * 4 + k];
        }
    }
    return true;
}

}  // namespace

bool CubicField::Bind(std::vector<GfVec3d> const &rest, std::string *error)
{
    auto fail = [&](char const *message) {
        _order = 0;
        _rest.clear();
        _restX.clear();
        _restY.clear();
        _restZ.clear();
        if (error) *error = message;
        return false;
    };
    size_t const n = rest.size();
    if (n < 4) return fail("an RBF needs at least four samples");
    GfVec3d lo(std::numeric_limits<double>::infinity());
    GfVec3d hi(-std::numeric_limits<double>::infinity());
    for (GfVec3d const &p : rest) {
        if (!Finite(p)) return fail("the RBF rest samples contain non-finite values");
        for (int d = 0; d < 3; ++d) {
            lo[d] = std::min(lo[d], p[d]);
            hi[d] = std::max(hi[d], p[d]);
        }
    }
    _centre = (lo + hi) * 0.5;
    _scale = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
    if (!(_scale > 0.0) || !std::isfinite(_scale))
        return fail("the RBF rest samples have zero extent");
    _invScale = 1.0 / _scale;
    _rest.resize(n);
    for (size_t i = 0; i < n; ++i) _rest[i] = (rest[i] - _centre) / _scale;
    if (!FullAffineRank(_rest))
        return fail("the RBF samples do not span 3D (they are coplanar or collinear), "
                    "so the field's affine part is undetermined");

    size_t const m = n + 4;
    _lu.assign(m * m, 0.0);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j)
            _lu[i * m + j] = Cube((_rest[i] - _rest[j]).GetLength());
        double const p[4] = {1.0, _rest[i][0], _rest[i][1], _rest[i][2]};
        for (size_t q = 0; q < 4; ++q) {
            _lu[i * m + n + q] = p[q];
            _lu[(n + q) * m + i] = p[q];
        }
    }
    double maxAbs = 0.0;
    for (double v : _lu) maxAbs = std::max(maxAbs, std::abs(v));

    // Partial-pivoting LU. The augmented matrix is symmetric indefinite with
    // a zero block, so it is not a Cholesky candidate.
    _pivot.resize(m);
    for (size_t c = 0; c < m; ++c) {
        size_t pivot = c;
        for (size_t r = c + 1; r < m; ++r)
            if (std::abs(_lu[r * m + c]) > std::abs(_lu[pivot * m + c])) pivot = r;
        _pivot[c] = pivot;
        if (std::abs(_lu[pivot * m + c]) <= maxAbs * 1e-13)
            return fail("the RBF system is singular: two samples coincide");
        if (pivot != c)
            for (size_t k = 0; k < m; ++k) std::swap(_lu[c * m + k], _lu[pivot * m + k]);
        double const inverse = 1.0 / _lu[c * m + c];
        for (size_t r = c + 1; r < m; ++r) {
            double const factor = _lu[r * m + c] * inverse;
            _lu[r * m + c] = factor;
            if (factor == 0.0) continue;
            for (size_t k = c + 1; k < m; ++k) _lu[r * m + k] -= factor * _lu[c * m + k];
        }
    }
    _order = m;
    _coefficients.assign(3 * m, 0.0);   // unsolved: the identity field
    _restX.resize(n);
    _restY.resize(n);
    _restZ.resize(n);
    for (size_t i = 0; i < n; ++i) {
        _restX[i] = _rest[i][0];
        _restY[i] = _rest[i][1];
        _restZ[i] = _rest[i][2];
    }
    return true;
}

bool CubicField::Solve(std::vector<GfVec3d> const &current, std::string *error)
{
    size_t const n = _rest.size(), m = _order;
    if (!m) {
        if (error) *error = "the RBF field is not bound";
        return false;
    }
    if (current.size() != n) {
        if (error) *error = "the RBF pose has a different sample count than its rest";
        return false;
    }
    // One pass: Finite checks the whole sample, so the old d-outer loop
    // re-checked every sample three times; the first failure and the
    // message are unchanged.
    for (size_t i = 0; i < n; ++i) {
        if (!Finite(current[i])) {
            if (error) *error = "the RBF pose contains non-finite values";
            return false;
        }
    }
    // The three columns are independent (disjoint lanes over shared
    // read-only factors), so they run interleaved: each column keeps its
    // exact op sequence (same operations in the same order), giving the
    // dependent accumulation chain three times the ILP with
    // bitwise-identical coefficients.
    std::vector<double> w(size_t(3) * m);
    double *w0 = w.data(), *w1 = w.data() + m, *w2 = w.data() + size_t(2) * m;
    for (size_t i = 0; i < n; ++i) {
        w0[i] = (current[i][0] - _centre[0]) / _scale - _rest[i][0];
        w1[i] = (current[i][1] - _centre[1]) / _scale - _rest[i][1];
        w2[i] = (current[i][2] - _centre[2]) / _scale - _rest[i][2];
    }
    std::fill(w0 + n, w0 + m, 0.0);
    std::fill(w1 + n, w1 + m, 0.0);
    std::fill(w2 + n, w2 + m, 0.0);
    for (size_t c = 0; c < m; ++c) {
        size_t const p = _pivot[c];
        std::swap(w0[c], w0[p]);
        std::swap(w1[c], w1[p]);
        std::swap(w2[c], w2[p]);
    }
    for (size_t r = 1; r < m; ++r) {           // L (unit diagonal)
        double s0 = w0[r], s1 = w1[r], s2 = w2[r];
        for (size_t k = 0; k < r; ++k) {
            double const l = _lu[r * m + k];
            s0 -= l * w0[k];
            s1 -= l * w1[k];
            s2 -= l * w2[k];
        }
        w0[r] = s0;
        w1[r] = s1;
        w2[r] = s2;
    }
    for (size_t r = m; r-- > 0;) {             // U
        double s0 = w0[r], s1 = w1[r], s2 = w2[r];
        for (size_t k = r + 1; k < m; ++k) {
            double const l = _lu[r * m + k];
            s0 -= l * w0[k];
            s1 -= l * w1[k];
            s2 -= l * w2[k];
        }
        double const d = _lu[r * m + r];
        w0[r] = s0 / d;
        w1[r] = s1 / d;
        w2[r] = s2 / d;
    }
    std::copy(w.begin(), w.end(), _coefficients.begin());
    return true;
}

// CubicField::Displacement lives in rbfField.h (inlined at the per-CV call
// sites); see there.

namespace {

// One fixed-width block of DisplaceBatch: W queries share a single sample
// pass. Every query runs Displacement's operations in Displacement's order
// (the same expression text, so the same FMA contraction applies), which
// is what keeps the block bitwise; the accumulators stay one array per
// query so the vectorizer contracts across lanes instead of splitting mul
// and add. Eight is the measured sweet spot on ARM64 (four leaves FMA
// latency exposed, sixteen spills).
template <size_t W>
void DisplaceBlocked(GfVec3d const &centre, double invScale, double scale,
                     double const *restX, double const *restY, double const *restZ,
                     double const *cx, double const *cy, double const *cz,
                     size_t n, GfVec3d const *qs, GfVec3d *ds)
{
    double p[W][3];
    for (size_t t = 0; t < W; ++t) {
        p[t][0] = (qs[t][0] - centre[0]) * invScale;
        p[t][1] = (qs[t][1] - centre[1]) * invScale;
        p[t][2] = (qs[t][2] - centre[2]) * invScale;
    }
    double o[W][3];
    for (size_t t = 0; t < W; ++t) {
        o[t][0] = cx[n] + cx[n + 1] * p[t][0] + cx[n + 2] * p[t][1] + cx[n + 3] * p[t][2];
        o[t][1] = cy[n] + cy[n + 1] * p[t][0] + cy[n + 2] * p[t][1] + cy[n + 3] * p[t][2];
        o[t][2] = cz[n] + cz[n + 1] * p[t][0] + cz[n + 2] * p[t][1] + cz[n + 3] * p[t][2];
    }
    for (size_t i = 0; i < n; ++i) {
        double const sx = restX[i], sy = restY[i], sz = restZ[i];
        double kk[W];
        for (size_t t = 0; t < W; ++t) {
            double const dx = p[t][0] - sx, dy = p[t][1] - sy, dz = p[t][2] - sz;
            double const rr = std::sqrt(dx * dx + dy * dy + dz * dz);
            kk[t] = rr * rr * rr;
        }
        for (size_t t = 0; t < W; ++t) {
            o[t][0] += cx[i] * kk[t];
            o[t][1] += cy[i] * kk[t];
            o[t][2] += cz[i] * kk[t];
        }
    }
    for (size_t t = 0; t < W; ++t)
        ds[t] = GfVec3d(o[t][0] * scale, o[t][1] * scale, o[t][2] * scale);
}

#if defined(__aarch64__) || defined(_M_ARM64)
// Block pointer bundles for the NEON 4-block template: AoS GfVec3d runs
// (the DisplaceBatch shape) and planar float-in/double-out (the deform
// strands shape, whose queries already sit in planar float planes).
struct AosBlock4 {
    GfVec3d const *qs;
    GfVec3d *ds;
};
struct PlanarFBlock4 {
    float const *qx, *qy, *qz;
    double *dx, *dy, *dz;
};
// Fused 4-query NEON block: the same per-lane operations as DisplaceBlocked
// (same FMA contraction association, correctly-rounded vector sqrt), but
// the kernel evaluation and the accumulation fuse into one sample pass
// with the normalised queries and accumulators held in vector registers.
// The auto-vectorized template above spills the queries to the stack and
// reloads them twice per sample; at 0.62ns/pair it runs 2.2x off the
// 0.28ns vector-sqrt roof, and the fused loop closes most of the
// schedulable gap (0.48ns/pair, on the FP-pipe wall for this op mix).
// Only the query load and the displacement store vary by bundle (vld3/vst3
// for AoS, plain loads/stores for planar); the affine prologue, the sample
// pass, and the FMA association below are one shared text, so both bundles
// evaluate bitwise what the other does.
template <class B>
void DisplaceBlockedNeon4T(GfVec3d const &centre, double invScale, double scale,
                           double const *restX, double const *restY, double const *restZ,
                           double const *cx, double const *cy, double const *cz,
                           size_t n, B blk)
{
    float64x2_t const c0 = vdupq_n_f64(centre[0]);
    float64x2_t const c1 = vdupq_n_f64(centre[1]);
    float64x2_t const c2 = vdupq_n_f64(centre[2]);
    float64x2_t const invS = vdupq_n_f64(invScale);
    // Plain locals, not arrays: the queries and accumulators must stay in
    // vector registers across the sample pass (arrays spill to the stack
    // and reload twice per sample).
    float64x2_t px0, px1, py0, py1, pz0, pz1;
    if constexpr (std::is_same<B, AosBlock4>::value) {
        float64x2x3_t const q01 = vld3q_f64(&blk.qs[0][0]);
        float64x2x3_t const q23 = vld3q_f64(&blk.qs[2][0]);
        px0 = vmulq_f64(vsubq_f64(q01.val[0], c0), invS);
        px1 = vmulq_f64(vsubq_f64(q23.val[0], c0), invS);
        py0 = vmulq_f64(vsubq_f64(q01.val[1], c1), invS);
        py1 = vmulq_f64(vsubq_f64(q23.val[1], c1), invS);
        pz0 = vmulq_f64(vsubq_f64(q01.val[2], c2), invS);
        pz1 = vmulq_f64(vsubq_f64(q23.val[2], c2), invS);
    } else {
        // float32x2 -> float64x2 is exact, so these six are bitwise the
        // vld3 path's six for the same query values.
        px0 = vmulq_f64(vsubq_f64(vcvt_f64_f32(vld1_f32(blk.qx)), c0), invS);
        px1 = vmulq_f64(vsubq_f64(vcvt_f64_f32(vld1_f32(blk.qx + 2)), c0), invS);
        py0 = vmulq_f64(vsubq_f64(vcvt_f64_f32(vld1_f32(blk.qy)), c1), invS);
        py1 = vmulq_f64(vsubq_f64(vcvt_f64_f32(vld1_f32(blk.qy + 2)), c1), invS);
        pz0 = vmulq_f64(vsubq_f64(vcvt_f64_f32(vld1_f32(blk.qz)), c2), invS);
        pz1 = vmulq_f64(vsubq_f64(vcvt_f64_f32(vld1_f32(blk.qz + 2)), c2), invS);
    }
    // One axis per scope: the twelve affine broadcasts must not all be
    // live at once, or the allocator spills the queries it just loaded.
    float64x2_t ox0, ox1, oy0, oy1, oz0, oz1;
    {
        float64x2_t const a0 = vdupq_n_f64(cx[n]);
        float64x2_t const a1 = vdupq_n_f64(cx[n + 1]);
        float64x2_t const a2 = vdupq_n_f64(cx[n + 2]);
        float64x2_t const a3 = vdupq_n_f64(cx[n + 3]);
        ox0 = vfmaq_f64(vfmaq_f64(vfmaq_f64(a0, a1, px0), a2, py0), a3, pz0);
        ox1 = vfmaq_f64(vfmaq_f64(vfmaq_f64(a0, a1, px1), a2, py1), a3, pz1);
    }
    {
        float64x2_t const a0 = vdupq_n_f64(cy[n]);
        float64x2_t const a1 = vdupq_n_f64(cy[n + 1]);
        float64x2_t const a2 = vdupq_n_f64(cy[n + 2]);
        float64x2_t const a3 = vdupq_n_f64(cy[n + 3]);
        oy0 = vfmaq_f64(vfmaq_f64(vfmaq_f64(a0, a1, px0), a2, py0), a3, pz0);
        oy1 = vfmaq_f64(vfmaq_f64(vfmaq_f64(a0, a1, px1), a2, py1), a3, pz1);
    }
    {
        float64x2_t const a0 = vdupq_n_f64(cz[n]);
        float64x2_t const a1 = vdupq_n_f64(cz[n + 1]);
        float64x2_t const a2 = vdupq_n_f64(cz[n + 2]);
        float64x2_t const a3 = vdupq_n_f64(cz[n + 3]);
        oz0 = vfmaq_f64(vfmaq_f64(vfmaq_f64(a0, a1, px0), a2, py0), a3, pz0);
        oz1 = vfmaq_f64(vfmaq_f64(vfmaq_f64(a0, a1, px1), a2, py1), a3, pz1);
    }
    // Unroll ×8: iterations are independent (each query's accumulation
    // keeps sample order, so the bits match), and unrolling amortizes the
    // loop overhead plus the indexed-address setup across more samples:
    // 34.4 instructions per sample at ×8 versus 36.0 rolled (×2: 35.0,
    // ×4: 36.3 — the addressing overhead does not amortize monotonically).
    // No spills at ×8; the remainder epilogue is one predictable dispatch
    // per block. Other compilers see no pragma and keep the rolled loop.
#if defined(__clang__)
#pragma clang loop unroll_count(8)
#elif defined(__GNUC__)
#pragma GCC unroll 8
#endif
    for (size_t i = 0; i < n; ++i) {
        // Broadcast loads keep one live register per sample value (the
        // compiler serves the coefficient lanes from scalar loads into
        // indexed FMA).
        float64x2_t const sx = vld1q_dup_f64(&restX[i]);
        float64x2_t const sy = vld1q_dup_f64(&restY[i]);
        float64x2_t const sz = vld1q_dup_f64(&restZ[i]);
        float64x2_t const qx = vld1q_dup_f64(&cx[i]);
        float64x2_t const qy = vld1q_dup_f64(&cy[i]);
        float64x2_t const qz = vld1q_dup_f64(&cz[i]);
        // Seed with dy*dy: the scalar blocks' codegen evaluates the sum
        // as fma(dz,dz,fma(dx,dx,dy*dy)), and the seed choice is
        // observable (exact-product-plus-rounded-addend commutes only in
        // the product, not across the addend).
        // Both pairs' differences first so each broadcast dies early.
        float64x2_t const dx0 = vsubq_f64(px0, sx);
        float64x2_t const dx1 = vsubq_f64(px1, sx);
        float64x2_t const dy0 = vsubq_f64(py0, sy);
        float64x2_t const dy1 = vsubq_f64(py1, sy);
        float64x2_t const dz0 = vsubq_f64(pz0, sz);
        float64x2_t const dz1 = vsubq_f64(pz1, sz);
        float64x2_t s0 = vfmaq_f64(vmulq_f64(dy0, dy0), dx0, dx0);
        s0 = vfmaq_f64(s0, dz0, dz0);
        float64x2_t s1 = vfmaq_f64(vmulq_f64(dy1, dy1), dx1, dx1);
        s1 = vfmaq_f64(s1, dz1, dz1);
        float64x2_t const rr0 = vsqrtq_f64(s0);
        float64x2_t const rr1 = vsqrtq_f64(s1);
        float64x2_t const kk0 = vmulq_f64(vmulq_f64(rr0, rr0), rr0);
        float64x2_t const kk1 = vmulq_f64(vmulq_f64(rr1, rr1), rr1);
        ox0 = vfmaq_f64(ox0, qx, kk0);
        oy0 = vfmaq_f64(oy0, qy, kk0);
        oz0 = vfmaq_f64(oz0, qz, kk0);
        ox1 = vfmaq_f64(ox1, qx, kk1);
        oy1 = vfmaq_f64(oy1, qy, kk1);
        oz1 = vfmaq_f64(oz1, qz, kk1);
    }
    float64x2_t const sc = vdupq_n_f64(scale);
    if constexpr (std::is_same<B, AosBlock4>::value) {
        float64x2x3_t d01, d23;
        d01.val[0] = vmulq_f64(ox0, sc);
        d01.val[1] = vmulq_f64(oy0, sc);
        d01.val[2] = vmulq_f64(oz0, sc);
        d23.val[0] = vmulq_f64(ox1, sc);
        d23.val[1] = vmulq_f64(oy1, sc);
        d23.val[2] = vmulq_f64(oz1, sc);
        vst3q_f64(&blk.ds[0][0], d01);
        vst3q_f64(&blk.ds[2][0], d23);
    } else {
        vst1q_f64(blk.dx, vmulq_f64(ox0, sc));
        vst1q_f64(blk.dx + 2, vmulq_f64(ox1, sc));
        vst1q_f64(blk.dy, vmulq_f64(oy0, sc));
        vst1q_f64(blk.dy + 2, vmulq_f64(oy1, sc));
        vst1q_f64(blk.dz, vmulq_f64(oz0, sc));
        vst1q_f64(blk.dz + 2, vmulq_f64(oz1, sc));
    }
}
#endif

}  // namespace

namespace {
std::atomic<bool> g_forceScalarDisplace{false};
}  // namespace

void TestForceScalarDisplace(bool force) noexcept
{
    g_forceScalarDisplace.store(force, std::memory_order_relaxed);
}

void CubicField::DisplaceBatch(GfVec3d const *qs, GfVec3d *ds, size_t count) const
{
    if (count == 0) return;
    size_t const n = _rest.size(), m = _order;
    if (!m) {
        for (size_t t = 0; t < count; ++t) ds[t] = GfVec3d(0.0);
        return;
    }
    double const *cx = &_coefficients[0];
    double const *cy = &_coefficients[m];
    double const *cz = &_coefficients[2 * m];
    double const *rx = _restX.data(), *ry = _restY.data(), *rz = _restZ.data();
#if defined(__aarch64__) || defined(_M_ARM64)
    if (!g_forceScalarDisplace.load(std::memory_order_relaxed)) {
        while (count >= 4) {
            DisplaceBlockedNeon4T(_centre, _invScale, _scale, rx, ry, rz,
                                  cx, cy, cz, n, AosBlock4{qs, ds});
            qs += 4;
            ds += 4;
            count -= 4;
        }
        for (size_t t = 0; t < count; ++t) ds[t] = Displacement(qs[t]);
        return;
    }
#endif
    while (count >= 8) {
        DisplaceBlocked<8>(_centre, _invScale, _scale, rx, ry, rz,
                           cx, cy, cz, n, qs, ds);
        qs += 8;
        ds += 8;
        count -= 8;
    }
    if (count >= 4) {
        DisplaceBlocked<4>(_centre, _invScale, _scale, rx, ry, rz,
                           cx, cy, cz, n, qs, ds);
        qs += 4;
        ds += 4;
        count -= 4;
    }
    for (size_t t = 0; t < count; ++t) ds[t] = Displacement(qs[t]);
}

void CubicField::DisplaceBatchPlanar(float const *qx, float const *qy, float const *qz,
                                     double *dx, double *dy, double *dz,
                                     size_t count) const
{
    if (count == 0) return;
    size_t const n = _rest.size(), m = _order;
    if (!m) {
        for (size_t t = 0; t < count; ++t) dx[t] = dy[t] = dz[t] = 0.0;
        return;
    }
    double const *cx = &_coefficients[0];
    double const *cy = &_coefficients[m];
    double const *cz = &_coefficients[2 * m];
    double const *rx = _restX.data(), *ry = _restY.data(), *rz = _restZ.data();
#if defined(__aarch64__) || defined(_M_ARM64)
    if (!g_forceScalarDisplace.load(std::memory_order_relaxed)) {
        while (count >= 4) {
            DisplaceBlockedNeon4T(_centre, _invScale, _scale, rx, ry, rz,
                                  cx, cy, cz, n,
                                  PlanarFBlock4{qx, qy, qz, dx, dy, dz});
            qx += 4;
            qy += 4;
            qz += 4;
            dx += 4;
            dy += 4;
            dz += 4;
            count -= 4;
        }
        for (size_t t = 0; t < count; ++t) {
            GfVec3d const d = Displacement(GfVec3d(qx[t], qy[t], qz[t]));
            dx[t] = d[0];
            dy[t] = d[1];
            dz[t] = d[2];
        }
        return;
    }
#endif
    // Scalar widths transpose through the stack: the block template keeps
    // its AoS spelling (and its FMA association), and the gather/scatter
    // convert exactly.
    while (count >= 8) {
        GfVec3d q[8], d[8];
        for (size_t t = 0; t < 8; ++t) q[t] = GfVec3d(qx[t], qy[t], qz[t]);
        DisplaceBlocked<8>(_centre, _invScale, _scale, rx, ry, rz,
                           cx, cy, cz, n, q, d);
        for (size_t t = 0; t < 8; ++t) {
            dx[t] = d[t][0];
            dy[t] = d[t][1];
            dz[t] = d[t][2];
        }
        qx += 8;
        qy += 8;
        qz += 8;
        dx += 8;
        dy += 8;
        dz += 8;
        count -= 8;
    }
    if (count >= 4) {
        GfVec3d q[4], d[4];
        for (size_t t = 0; t < 4; ++t) q[t] = GfVec3d(qx[t], qy[t], qz[t]);
        DisplaceBlocked<4>(_centre, _invScale, _scale, rx, ry, rz,
                           cx, cy, cz, n, q, d);
        for (size_t t = 0; t < 4; ++t) {
            dx[t] = d[t][0];
            dy[t] = d[t][1];
            dz[t] = d[t][2];
        }
        qx += 4;
        qy += 4;
        qz += 4;
        dx += 4;
        dy += 4;
        dz += 4;
        count -= 4;
    }
    for (size_t t = 0; t < count; ++t) {
        GfVec3d const d = Displacement(GfVec3d(qx[t], qy[t], qz[t]));
        dx[t] = d[0];
        dy[t] = d[1];
        dz[t] = d[2];
    }
}

std::vector<size_t> SelectSamples(std::vector<GfVec3d> const &points, size_t budget,
                                  double epsilon)
{
    // Exact duplicates first: they make the system singular.
    struct KeyHash {
        size_t operator()(GfVec3i const &k) const
        {
            return size_t(uint64_t(uint32_t(k[0])) * 73856093ULL ^
                          uint64_t(uint32_t(k[1])) * 19349663ULL ^
                          uint64_t(uint32_t(k[2])) * 83492791ULL);
        }
    };
    double const cell = epsilon > 0.0 ? epsilon : 1e-9;
    std::unordered_set<GfVec3i, KeyHash> seen;
    std::vector<size_t> unique;
    unique.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        if (!Finite(points[i])) continue;
        GfVec3d const q = points[i] / cell;
        auto clampInt = [](double v) {
            return int(std::max(-2.0e9, std::min(2.0e9, std::floor(v + 0.5))));
        };
        if (seen.insert(GfVec3i(clampInt(q[0]), clampInt(q[1]), clampInt(q[2]))).second)
            unique.push_back(i);
    }
    if (unique.size() <= budget) return unique;

    // Farthest-point sampling from the first point.
    std::vector<size_t> chosen;
    chosen.reserve(budget);
    std::vector<double> distance(unique.size(), std::numeric_limits<double>::infinity());
    size_t next = 0;
    while (chosen.size() < budget) {
        chosen.push_back(unique[next]);
        GfVec3d const &p = points[unique[next]];
        size_t best = 0;
        double bestDistance = -1.0;
        for (size_t k = 0; k < unique.size(); ++k) {
            distance[k] = std::min(distance[k], (points[unique[k]] - p).GetLengthSq());
            if (distance[k] > bestDistance) {
                bestDistance = distance[k];
                best = k;
            }
        }
        if (bestDistance <= 0.0) break;
        next = best;
    }
    std::sort(chosen.begin(), chosen.end());
    return chosen;
}

}  // namespace rbf
}  // namespace usdGen
