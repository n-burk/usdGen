#ifdef USDGEN_ENABLE_CUDA
#include "cudaSurfaceInput.h"

#include <cmath>
#include <limits>
#include <new>

namespace usdGen {
namespace {
void Diag(std::vector<std::string>* d, std::string const& s) { if (d) d->push_back(s); }
bool Finite(VtVec3fArray const& points) {
    for (auto const& p : points)
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    return true;
}
bool Identity(GfMatrix4d const& m) {
    GfMatrix4d const identity(1.0);
    for (int r = 0; r != 4; ++r) for (int c = 0; c != 4; ++c)
        if (m[r][c] != identity[r][c]) return false;
    return true;
}
bool SameKey(CudaSurfaceBindingKey const& a, CudaSurfaceBindingKey const& b) {
    return a.path == b.path && a.restPoints == b.restPoints &&
        a.faceVertexCounts == b.faceVertexCounts &&
        a.faceVertexIndices == b.faceVertexIndices &&
        a.sampleBudget == b.sampleBudget && a.algorithmVersion == b.algorithmVersion;
}
} // namespace

CudaSurfacePreparationStatus PrepareCudaSurface(
    UsdGenSurfaceDesc const& source, uint32_t sampleBudget, int algorithmVersion,
    CudaSurfacePrepared* output, std::vector<std::string>* diagnostics) {
    if (!output) { Diag(diagnostics, "null CUDA surface output"); return CudaSurfacePreparationStatus::InvalidArgument; }
    auto fail = [&](CudaSurfacePreparationStatus status, char const* message) {
        Diag(diagnostics, message); return status;
    };
    if (sampleBudget < 4) return fail(CudaSurfacePreparationStatus::InvalidArgument, "surface sampleBudget must be at least 4");
    if (source.restFromCurrentPoints)
        return fail(CudaSurfacePreparationStatus::UnsupportedFeature, "RBF requires a Default-time rest surface; current-frame fallback is not a binding");
    if (!source.subsetFaces.empty()) return fail(CudaSurfacePreparationStatus::UnsupportedFeature, "surface subsets are unsupported");
    if (!Identity(source.worldMatrix)) return fail(CudaSurfacePreparationStatus::UnsupportedFeature, "non-identity surface transforms are unsupported");
    if (source.restPoints.empty() || source.points.empty() || source.restPoints.size() != source.points.size())
        return fail(CudaSurfacePreparationStatus::InvalidTopology, "surface rest/current vertex counts do not match");
    if (source.restPoints.size() > std::numeric_limits<uint32_t>::max() ||
        source.faceVertexIndices.size() > std::numeric_limits<uint32_t>::max())
        return fail(CudaSurfacePreparationStatus::InvalidTopology, "surface exceeds uint32 index limits");
    if (!Finite(source.restPoints) || !Finite(source.points))
        return fail(CudaSurfacePreparationStatus::NonFiniteInput, "surface positions must be finite");
    size_t indexCount = 0;
    for (int count : source.faceVertexCounts) {
        if (count != 3 && count != 4) return fail(CudaSurfacePreparationStatus::InvalidTopology, "surface faces must be triangles or quads");
        if (indexCount > std::numeric_limits<size_t>::max() - size_t(count))
            return fail(CudaSurfacePreparationStatus::InvalidTopology, "surface face index count overflow");
        indexCount += size_t(count);
    }
    if (indexCount != source.faceVertexIndices.size())
        return fail(CudaSurfacePreparationStatus::InvalidTopology, "surface face counts do not match indices");
    for (int index : source.faceVertexIndices)
        if (index < 0 || size_t(index) >= source.points.size())
            return fail(CudaSurfacePreparationStatus::InvalidTopology, "surface face index is out of range");

    try {
        CudaSurfacePrepared candidate;
        candidate.restPoints.reserve(source.restPoints.size());
        candidate.currentPoints.reserve(source.points.size());
        candidate.faceOffsets.reserve(source.faceVertexCounts.size() + 1);
        candidate.faceVertexIndices.reserve(source.faceVertexIndices.size());
        for (auto const& p : source.restPoints) candidate.restPoints.push_back({p[0], p[1], p[2]});
        for (auto const& p : source.points) candidate.currentPoints.push_back({p[0], p[1], p[2]});
        candidate.faceOffsets.push_back(0);
        uint64_t offset = 0;
        for (int count : source.faceVertexCounts) {
            for (int i = 0; i < count; ++i)
                candidate.faceVertexIndices.push_back(static_cast<uint32_t>(source.faceVertexIndices[offset + i]));
            offset += static_cast<uint64_t>(count);
            candidate.faceOffsets.push_back(static_cast<uint32_t>(offset));
        }
        candidate.key = {source.path, source.restPoints, source.faceVertexCounts,
                         source.faceVertexIndices, sampleBudget, algorithmVersion};
        candidate.sampleBudget = sampleBudget;
        candidate.algorithmVersion = algorithmVersion;
        *output = std::move(candidate);
        return CudaSurfacePreparationStatus::Ok;
    } catch (std::bad_alloc const&) {
        return fail(CudaSurfacePreparationStatus::AllocationFailure, "surface preparation allocation failed");
    }
}

bool RestBindingMatches(CudaSurfacePrepared const& a, CudaSurfacePrepared const& b) {
    return SameKey(a.key, b.key);
}
} // namespace usdGen
#endif // USDGEN_ENABLE_CUDA
