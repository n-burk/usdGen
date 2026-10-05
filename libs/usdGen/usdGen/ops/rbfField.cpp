#include "usdGen/ops/rbfField.h"

#include "pxr/base/gf/vec3i.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>

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
    std::vector<double> rhs(m);
    for (int d = 0; d < 3; ++d) {
        for (size_t i = 0; i < n; ++i) {
            if (!Finite(current[i])) {
                if (error) *error = "the RBF pose contains non-finite values";
                return false;
            }
            rhs[i] = (current[i][d] - _centre[d]) / _scale - _rest[i][d];
        }
        std::fill(rhs.begin() + n, rhs.end(), 0.0);
        for (size_t c = 0; c < m; ++c) std::swap(rhs[c], rhs[_pivot[c]]);
        for (size_t r = 1; r < m; ++r) {           // L (unit diagonal)
            double s = rhs[r];
            for (size_t k = 0; k < r; ++k) s -= _lu[r * m + k] * rhs[k];
            rhs[r] = s;
        }
        for (size_t r = m; r-- > 0;) {             // U
            double s = rhs[r];
            for (size_t k = r + 1; k < m; ++k) s -= _lu[r * m + k] * rhs[k];
            rhs[r] = s / _lu[r * m + r];
        }
        std::copy(rhs.begin(), rhs.end(), _coefficients.begin() + d * m);
    }
    return true;
}

// CubicField::Displacement lives in rbfField.h (inlined at the per-CV call
// sites); see there.

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
