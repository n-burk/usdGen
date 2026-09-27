// usdGen — small helpers shared by the CPU operators that read whole
// buffers at capture (Clump, GuideInterpolate).
#ifndef USDGEN_OP_UTIL_H
#define USDGEN_OP_UTIL_H

#include "usdGen/curveBuffer.h"
#include "usdGen/graphDesc.h"
#include "usdGen/op.h"

#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {
namespace opUtil {

/// FNV-1a over capture-class inputs.
struct Digest
{
    uint64_t h = 1469598103934665603ULL;
    void Mix(uint64_t v) { h ^= v; h *= 0x100000001b3ULL; }
    void Mix(double v)
    {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        Mix(bits);
    }
    void Mix(TfToken const &t) { for (char const *p = t.GetText(); *p; ++p) Mix(uint64_t(uint8_t(*p))); }
    void Mix(VtValue const &v) { Mix(uint64_t(v.IsEmpty() ? 0 : v.GetHash())); }
    UsdGenEpoch Epoch(uint64_t salt) const { return {h, h ^ salt}; }
};

/// Curve spans [spans[c], spans[c+1]) of a uniform or ragged buffer.
inline bool CurveSpans(UsdGenCurveBuffer const &buffer, std::vector<uint32_t> *spans,
                       std::string *error)
{
    return detail::_UsdGenCurveSpans(buffer, spans, error);
}

inline GfVec3f Point(UsdGenCurveBuffer const &b, size_t cv)
{
    return GfVec3f(b.px[cv], b.py[cv], b.pz[cv]);
}

/// The rest position of CV `cv` when the buffer carries a complete rest
/// plane, else its current position.
inline GfVec3f RestPoint(UsdGenCurveBuffer const &b, size_t cv)
{
    return b.rest.size() == b.totalCvs ? b.rest[cv] : Point(b, cv);
}

inline GfVec3f Normalized(GfVec3f v, GfVec3f fallback = GfVec3f(0, 1, 0))
{
    float const length = v.GetLength();
    return length > 1e-12f ? v / length : fallback;
}

/// Rotates `v` by the rotation that takes unit `from` onto unit `to`, scaled
/// to `amount` of its angle (0 = identity, 1 = the full minimal rotation).
inline GfVec3f RotateOnto(GfVec3f const &v, GfVec3f const &from, GfVec3f const &to,
                          float amount)
{
    if (amount <= 0.0f) return v;
    GfVec3d const a(from), b(to);
    GfVec3d axis = GfCross(a, b);
    double const s = axis.GetLength();
    double const c = GfDot(a, b);
    double angle = std::atan2(s, c) * double(amount);
    if (s < 1e-9) {
        if (c > 0.0) return v;                 // already aligned
        // Opposite: any axis perpendicular to `from`.
        axis = std::fabs(a[0]) < 0.9 ? GfCross(a, GfVec3d(1, 0, 0)) : GfCross(a, GfVec3d(0, 1, 0));
    }
    axis.Normalize();
    GfVec3d const p(v);
    double const cs = std::cos(angle), sn = std::sin(angle);
    GfVec3d const r = p * cs + GfCross(axis, p) * sn + axis * (GfDot(axis, p) * (1.0 - cs));
    return GfVec3f(r);
}

/// Rest area of the faces a node scatters on: a GeomSubset's faces of its
/// parent mesh, or the whole mesh. 0 when there is no bound surface.
inline double RestSurfaceArea(UsdGenGraphDesc const *desc, UsdGenSurfaceId surface, bool hasSurface)
{
    if (!desc || !hasSurface || surface >= desc->surfaces.size()) return 0.0;
    UsdGenSurfaceDesc const *mesh = &desc->surfaces[surface];
    VtIntArray const subset = mesh->subsetFaces;
    bool const restricted = UsdGenSurfaceRestricted(*mesh);
    if (mesh->faceVertexCounts.empty()) {
        SdfPath const parent = mesh->path.GetParentPath();
        mesh = nullptr;
        for (auto const &candidate : desc->surfaces)
            if (candidate.path == parent) { mesh = &candidate; break; }
        if (!mesh) return 0.0;
    }
    VtVec3fArray const &points = mesh->restPoints.empty() ? mesh->points : mesh->restPoints;
    std::vector<size_t> offsets(mesh->faceVertexCounts.size() + 1, 0);
    for (size_t f = 0; f < mesh->faceVertexCounts.size(); ++f)
        offsets[f + 1] = offsets[f] + size_t(std::max(0, mesh->faceVertexCounts[f]));
    if (offsets.back() != mesh->faceVertexIndices.size()) return 0.0;
    auto faceArea = [&](size_t f) {
        size_t const first = offsets[f], n = offsets[f + 1] - first;
        if (n < 3) return 0.0;
        GfVec3d sum(0.0);
        for (size_t k = 0; k < n; ++k) {
            int const ia = mesh->faceVertexIndices[first + k];
            int const ib = mesh->faceVertexIndices[first + (k + 1) % n];
            if (ia < 0 || ib < 0 || size_t(ia) >= points.size() || size_t(ib) >= points.size())
                return 0.0;
            sum += GfCross(GfVec3d(points[ia]), GfVec3d(points[ib]));
        }
        return 0.5 * sum.GetLength();
    };
    double area = 0.0;
    if (restricted) {
        for (int f : subset)
            if (f >= 0 && size_t(f) < mesh->faceVertexCounts.size()) area += faceArea(size_t(f));
    } else {
        for (size_t f = 0; f < mesh->faceVertexCounts.size(); ++f) area += faceArea(f);
    }
    return area;
}

/// The key a connected region/clump map value groups by: distinct 10-bit
/// levels (an 8-bit painted map) and distinct integer ids both separate.
inline int64_t RegionKey(double value)
{
    return std::isfinite(value) ? static_cast<int64_t>(std::llround(value * 1024.0))
                                : INT64_MIN;
}

}  // namespace opUtil
}  // namespace usdGen

#endif  // USDGEN_OP_UTIL_H
