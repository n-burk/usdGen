#include "usdGenImaging/deviceCurveGroupPublisher.h"

#if defined(USDGEN_HAS_CUDA_GL_INTEROP) && \
    __has_include("pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h")

#include "usdGenImaging/cudaBasisCurvesProvider.h"
#include "usdGenImaging/usdGenTilePublisher.h"

#include "pxr/base/gf/bbox3d.h"

#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {
namespace {

class _RawControl final : public HdStBasisCurvesGpuGroupDataSource {
public:
    HD_DECLARE_DATASOURCE(_RawControl);
    explicit _RawControl(HdStBasisCurvesGpuGroupCandidateSharedPtr candidate)
        : _candidate(std::move(candidate)) {}
    HdStBasisCurvesGpuGroupCandidateSharedPtr GetCandidate() const override {
        return _candidate;
    }
    // The per-renderer staging filter owns mailbox allocation.  An upstream
    // control must not share a mailbox across render indices.
    HdStBasisCurvesGpuGroupMailboxSharedPtr GetMailbox() const override { return {}; }
private:
    HdStBasisCurvesGpuGroupCandidateSharedPtr const _candidate;
};

bool _Finite(float v) { return std::isfinite(v); }

bool _Finite(GfMatrix4d const &matrix) {
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column)
            if (!std::isfinite(matrix[row][column])) return false;
    return true;
}

bool _ValidBounds(usdGen::UsdGenDeviceTileMetadata const &tile) {
    if (!tile.boundsValid) return false;
    for (int i = 0; i != 3; ++i) {
        if (!_Finite(tile.extentMin[i]) || !_Finite(tile.extentMax[i]) ||
            tile.extentMin[i] > tile.extentMax[i]) return false;
    }
    return true;
}

TfToken _Basis(usdGen::UsdGenDeviceCurveBasis basis) {
    switch (basis) {
    case usdGen::UsdGenDeviceCurveBasis::BSpline: return TfToken("bspline");
    case usdGen::UsdGenDeviceCurveBasis::CatmullRom: return TfToken("catmullRom");
    default: return {};
    }
}

UsdGenDeviceCurveGroupPublisher::Result _Fail(char const *reason) {
    UsdGenDeviceCurveGroupPublisher::Result result;
    result.reason = reason;
    return result;
}

} // anonymous namespace

UsdGenDeviceCurveGroupPublisher::Result
UsdGenDeviceCurveGroupPublisher::Build(
    usdGen::UsdGenGenerationConstPtr const &generation,
    SdfPath const &expectedDescription, uint64_t const ticket,
    bool const ownsSubtree)
{
    if (!generation || !generation->device || !generation->devicePresentation ||
        ticket == 0 || expectedDescription.IsEmpty() ||
        !expectedDescription.IsAbsolutePath() || !expectedDescription.IsPrimPath())
        return _Fail("missing CUDA generation, presentation, description, or ticket");
    if (expectedDescription.IsAbsoluteRootPath())
        return _Fail("CUDA group description must not be the absolute root");
    if (generation->id < 0 ||
        uint64_t(generation->id) != generation->device->Identity().generation)
        return _Fail("public generation id does not match device generation");
    auto const &identity = generation->device->Identity();
    auto const &presentation = *generation->devicePresentation;
    if (identity.backend != usdGen::UsdGenDeviceBackend::Cuda ||
        identity.deviceIndex < 0 || presentation.description != expectedDescription ||
        presentation.renderNamespace !=
            expectedDescription.AppendChild(UsdGenTilePublisher::RenderNamespace()))
        return _Fail("CUDA identity or exact presentation scope is invalid");
    // Host C2 payloads and guides/instancers are mutually exclusive with this
    // device-only ingress.  Do not silently combine representations.
    if (!generation->tiles.empty() || !generation->guides.empty() ||
        !generation->instancers.empty())
        return _Fail("CUDA group ingress rejects host tiles, guides, or instancers");
    auto const &geometry = generation->device->Geometry();
    auto const &topology = geometry.curveTopology;
    if (topology.type != usdGen::UsdGenDeviceCurveType::Cubic ||
        topology.wrap != usdGen::UsdGenDeviceCurveWrap::Pinned ||
        _Basis(topology.basis).IsEmpty())
        return _Fail("CUDA group ingress requires pinned cubic BSpline or CatmullRom");
    if (geometry.tiles.size() > 256 || presentation.refineLevel < 0 ||
        !_Finite(presentation.xformMatrix))
        return _Fail("CUDA tile count or presentation refinement is invalid");

    auto candidate = std::make_shared<HdStBasisCurvesGpuGroupCandidate>();
    candidate->groupPath = presentation.renderNamespace;
    candidate->ticket = ticket;
    candidate->generation = identity.generation;
    candidate->ownsSubtree = ownsSubtree;
    uint64_t nextCurve = 0, nextPoint = 0;
    std::set<uint32_t> tileIds;
    for (usdGen::UsdGenDeviceTileMetadata const &tile : geometry.tiles) {
        if (!_ValidBounds(tile) || !tileIds.insert(tile.tile).second ||
            tile.tile > std::numeric_limits<usdGen::UsdGenTileId>::max() ||
            nextCurve > geometry.curveCount || nextPoint > geometry.pointCount ||
            tile.firstCurve != nextCurve || tile.firstPoint != nextPoint ||
            tile.curveCount > geometry.curveCount - nextCurve ||
            tile.pointCount > geometry.pointCount - nextPoint ||
            ((geometry.curveCount != 0 || geometry.pointCount != 0) &&
             (tile.curveCount == 0 || tile.pointCount == 0)) ||
            tile.firstPoint > uint64_t(std::numeric_limits<uint32_t>::max()) ||
            tile.pointCount > uint64_t(std::numeric_limits<uint32_t>::max()) - tile.firstPoint)
            return _Fail("CUDA tile ranges or bounds are not contiguous and valid");
        nextCurve += tile.curveCount;
        nextPoint += tile.pointCount;
        HdStBasisCurvesGpuGroupMember member;
        member.id = tile.tile;
        member.rprimPath = UsdGenTilePublisher::TilePath(expectedDescription, tile.tile);
        member.presentation.curveType = TfToken("cubic");
        member.presentation.curveBasis = _Basis(topology.basis);
        member.presentation.curveWrap = TfToken("pinned");
        member.presentation.bounds = GfBBox3d(
            GfRange3d(GfVec3d(tile.extentMin[0], tile.extentMin[1], tile.extentMin[2]),
                      GfVec3d(tile.extentMax[0], tile.extentMax[1], tile.extentMax[2])),
            presentation.xformMatrix);
        member.presentation.refineLevel = presentation.refineLevel;
        member.presentation.purpose = presentation.purpose;
        member.presentation.visibility = presentation.visibility;
        member.presentation.materialPath = presentation.materialPath;
        member.presentation.materialPurpose = presentation.materialPurpose;
        member.presentation.primOrigin = presentation.primOrigin;
        member.presentation.dependencySurface = presentation.dependencySurface;
        UsdGenCudaBasisCurvesProvider::CreateInfo info;
        info.generation = generation->device;
        info.curveType = member.presentation.curveType;
        info.curveBasis = member.presentation.curveBasis;
        info.curveWrap = member.presentation.curveWrap;
        info.conservativeBounds = member.presentation.bounds;
        info.tileId = tile.tile;
        member.provider = std::make_shared<UsdGenCudaBasisCurvesProvider>(std::move(info));
        candidate->members.push_back(std::move(member));
    }
    if (nextCurve != geometry.curveCount || nextPoint != geometry.pointCount)
        return _Fail("CUDA tile ranges do not cover geometry exactly");
    // Empty generations deliberately retain an empty valid group: the filter
    // publishes its Ready completion as removal rather than preserving hair.
    if (!HdStValidateBasisCurvesGpuGroupCandidate(*candidate))
        return _Fail("CUDA group presentation or member paths are invalid");
    HdStBasisCurvesGpuGroupCandidateSharedPtr const immutable = candidate;
    UsdGenDeviceCurveGroupPublisher::Result result;
    result.control = _RawControl::New(immutable);
    if (!result.control) result.reason = "failed to allocate CUDA group control";
    return result;
}

} // namespace usdGenImaging

#endif
