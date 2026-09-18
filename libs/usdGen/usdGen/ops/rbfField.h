// usdGen — host cubic RBF displacement field (plan/examples/rbf-deformation.md).
//
// The CPU twin of gpu::CudaRbfBinding's formulation, used by UsdGenDeform on
// the CPU lane:
//
//   y       = (x - centre) / scale       centre/scale: the rest samples' extent
//   K[i,j]  = |y_i - y_j|^3              P[i] = [1, y_i]
//   [K P; P^T 0] [a; b] = [d; 0]         d_i = (current_i - rest_i) / scale
//   F(x)    = x + scale * (sum_i a_i |y - y_i|^3 + b . [1, y])
//
// so F reproduces every sample exactly, and any affine motion of the samples
// (translation, rotation, scale) everywhere. Bind factors the system once per
// rest layout; Solve is a pair of triangular solves per pose.
#ifndef USDGEN_OPS_RBF_FIELD_H
#define USDGEN_OPS_RBF_FIELD_H

#include "pxr/base/gf/vec3d.h"

#include <cstddef>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {
namespace rbf {

class CubicField
{
public:
    /// Factors the system for `rest`. False, with the reason, for fewer than
    /// four samples, non-finite input, zero extent, or samples that do not
    /// span 3D (coplanar or collinear: the affine term is then undetermined).
    bool Bind(std::vector<GfVec3d> const &rest, std::string *error);

    /// Coefficients for the samples at `current` (same order as the bound
    /// rest samples).
    bool Solve(std::vector<GfVec3d> const &current, std::string *error);

    /// F(x) - x: the displacement of a rest-space point.
    GfVec3d Displacement(GfVec3d const &x) const;

    bool Bound() const { return _order != 0; }
    size_t SampleCount() const { return _rest.size(); }

private:
    std::vector<GfVec3d> _rest;        // normalised rest samples
    std::vector<double> _restX, _restY, _restZ;   // the same, one array per axis
    GfVec3d _centre{0.0};
    double _scale = 1.0;
    size_t _order = 0;                 // n + 4
    std::vector<double> _lu;           // row-major LU of the augmented matrix
    std::vector<size_t> _pivot;
    std::vector<double> _coefficients; // 3 * order: x, y, z columns
};

/// Up to `budget` well-spread indices into `points`: exact duplicates
/// (within `epsilon`) are dropped, then farthest-point sampling starts at the
/// first point. Deterministic for a given input.
std::vector<size_t> SelectSamples(std::vector<GfVec3d> const &points, size_t budget,
                                  double epsilon);

}  // namespace rbf
}  // namespace usdGen

#endif  // USDGEN_OPS_RBF_FIELD_H
