// Scalar-only admission coverage for the device generation -> GPU group
// control mapper.  This intentionally never calls provider Prepare.
#include "usdGenImaging/deviceCurveGroupPublisher.h"
#include "../cudaGlFixture.h"

#include "usdGen/gpu/generation.h"

#include <cstdio>
#include <memory>
#include <limits>
#include <string>
#include <vector>

using namespace usdGen;
using namespace usdGen::gpu;
using namespace usdGenImaging;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (false)

namespace {

class _Owner final : public UsdGenDeviceOwner {
public:
    bool ProducerReady() const noexcept override { return true; }
    std::unique_ptr<UsdGenDeviceConsumer> AcquireConsumer(
        UsdGenDeviceStream) const noexcept override { return {}; }
};

UsdGenDeviceTileMetadata Tile(uint32_t id, uint64_t firstCurve, uint64_t curves,
                              uint64_t firstPoint, uint64_t points)
{
    UsdGenDeviceTileMetadata t;
    t.tile = id; t.firstCurve = firstCurve; t.curveCount = curves;
    t.firstPoint = firstPoint; t.pointCount = points; t.boundsValid = true;
    t.extentMin = {-1.f, -1.f, -.1f}; t.extentMax = {1.f, 1.f, .1f};
    return t;
}

UsdGenGenerationConstPtr MakeGeneration(
    std::shared_ptr<const UsdGenDeviceGeneration> device, int64_t id = 1)
{
    auto generation = std::make_shared<UsdGenGeneration>();
    generation->id = id; generation->device = std::move(device);
    UsdGenDevicePresentationMetadata p;
    p.description = SdfPath("/Groom/Device");
    p.renderNamespace = SdfPath("/Groom/Device/__usdGenRender");
    p.xformMatrix.SetTranslate(GfVec3d(2, 3, 4));
    p.purpose = TfToken("render"); p.visibility = TfToken("inherited");
    p.materialPath = SdfPath("/Looks/Hair"); p.materialPurpose = TfToken("allPurpose");
    p.refineLevel = 3; p.primOrigin = p.description; p.dependencySurface = SdfPath("/Scalp");
    generation->devicePresentation = p;
    return generation;
}

std::shared_ptr<const UsdGenDeviceGeneration> WithTiles(
    std::shared_ptr<const UsdGenDeviceGeneration> const &base,
    std::vector<UsdGenDeviceTileMetadata> tiles)
{
    std::string reason;
    return WithTileMetadata(base, std::move(tiles), &reason,
        {UsdGenDeviceCurveType::Cubic, UsdGenDeviceCurveBasis::BSpline,
         UsdGenDeviceCurveWrap::Pinned});
}

std::shared_ptr<const UsdGenDeviceGeneration> EmptyDevice()
{
    UsdGenDeviceGeneration::CreateInfo info;
    info.identity = {UsdGenDeviceBackend::Cuda, 0, 1};
    info.geometry.curveTopology = {UsdGenDeviceCurveType::Cubic,
        UsdGenDeviceCurveBasis::BSpline, UsdGenDeviceCurveWrap::Pinned};
    info.owner = std::make_shared<_Owner>();
    std::string reason;
    return UsdGenDeviceGeneration::Create(std::move(info), &reason);
}

std::shared_ptr<const UsdGenDeviceGeneration> FakeDevice(
    std::vector<UsdGenDeviceTileMetadata> tiles, uint64_t curves, uint64_t points,
    UsdGenDeviceCurveBasis basis = UsdGenDeviceCurveBasis::BSpline)
{
    UsdGenDeviceGeneration::CreateInfo info;
    info.identity = {UsdGenDeviceBackend::Cuda, 0, 1};
    info.geometry.curveCount = curves; info.geometry.pointCount = points;
    info.geometry.tiles = std::move(tiles);
    info.geometry.curveTopology = {UsdGenDeviceCurveType::Cubic,
        basis, UsdGenDeviceCurveWrap::Pinned};
    info.owner = std::make_shared<_Owner>();
    return UsdGenDeviceGeneration::Create(std::move(info));
}

} // namespace

int main()
{
    auto base = usdGenTest::MakeCudaGlFixture(.2f, 1);
    if (!base) return 77;
    auto const &shape = base->Geometry();
    auto one = WithTiles(base, {Tile(7, 0, shape.curveCount, 0, shape.pointCount)});
    CHECK(one);
    auto generation = MakeGeneration(one);
    auto result = UsdGenDeviceCurveGroupPublisher::Build(
        generation, SdfPath("/Groom/Device"), 41);
    CHECK(result && result.reason.empty());
    CHECK(!result.control->GetMailbox());
    auto candidate = result.control->GetCandidate();
    CHECK(candidate && candidate->ticket == 41 && candidate->generation == 1 &&
          candidate->groupPath == SdfPath("/Groom/Device/__usdGenRender") &&
          !candidate->ownsSubtree && candidate->members.size() == 1);
    auto owning = UsdGenDeviceCurveGroupPublisher::Build(
        generation, SdfPath("/Groom/Device"), 42, /* ownsSubtree = */ true);
    CHECK(owning && owning.control->GetCandidate() &&
          owning.control->GetCandidate()->ownsSubtree);
    auto const &member = candidate->members.front();
    CHECK(member.id == 7 && member.rprimPath == SdfPath("/Groom/Device/__usdGenRender/tile_0007") &&
          member.provider && member.presentation.bounds.GetMatrix() ==
              generation->devicePresentation->xformMatrix &&
          member.presentation.materialPath == SdfPath("/Looks/Hair") &&
          member.presentation.refineLevel == 3);
    // Mapper does not mutate caller state or synthesize host C2 payload.
    CHECK(generation->tiles.empty() && generation->guides.empty() && generation->instancers.empty() &&
          generation->device == one);

    // These synthetic devices exercise mapper-only metadata admission.  Keep
    // their C3 point spans realistic even though they never enter Prepare.
    std::vector<UsdGenDeviceTileMetadata> many;
    for (uint32_t i = 0; i != 32; ++i)
        many.push_back(Tile(i, i, 1, uint64_t(i) * 4, 4));
    auto thirtyTwo = FakeDevice(many, 32, 128); CHECK(thirtyTwo);
    auto thirtyTwoResult = UsdGenDeviceCurveGroupPublisher::Build(
        MakeGeneration(thirtyTwo), SdfPath("/Groom/Device"), 42);
    CHECK(thirtyTwoResult && thirtyTwoResult.control->GetCandidate() &&
          thirtyTwoResult.control->GetCandidate()->members.size() == 32);
    std::vector<UsdGenDeviceTileMetadata> overflow = many;
    for (uint32_t i = 32; i != 257; ++i)
        overflow.push_back(Tile(i, i, 1, uint64_t(i) * 4, 4));
    auto tooMany = FakeDevice(overflow, 257, 1028); CHECK(tooMany);
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(MakeGeneration(tooMany), SdfPath("/Groom/Device"), 43));
    std::vector<UsdGenDeviceTileMetadata> atLimit;
    for (uint32_t i = 0; i != 256; ++i)
        atLimit.push_back(Tile(i, i, 1, uint64_t(i) * 4, 4));
    auto twoFiveSix = FakeDevice(atLimit, 256, 1024); CHECK(twoFiveSix);
    CHECK(UsdGenDeviceCurveGroupPublisher::Build(MakeGeneration(twoFiveSix), SdfPath("/Groom/Device"), 43));
    auto catmull = FakeDevice({Tile(1, 0, 1, 0, 4)}, 1, 4, UsdGenDeviceCurveBasis::CatmullRom);
    CHECK(catmull && UsdGenDeviceCurveGroupPublisher::Build(MakeGeneration(catmull), SdfPath("/Groom/Device"), 43));
    auto overflowId = FakeDevice({Tile(65536, 0, 1, 0, 4)}, 1, 4); CHECK(overflowId);
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(MakeGeneration(overflowId), SdfPath("/Groom/Device"), 43));
    // Create validates per-tile in-bounds ranges but deliberately does not
    // require a complete partition.  These reach the mapper admission gate.
    auto gap = FakeDevice({Tile(0, 0, 1, 0, 4), Tile(1, 2, 1, 4, 4)}, 3, 12);
    CHECK(gap && !UsdGenDeviceCurveGroupPublisher::Build(MakeGeneration(gap), SdfPath("/Groom/Device"), 43));
    auto overlap = FakeDevice({Tile(0, 0, 1, 0, 4), Tile(1, 0, 1, 4, 4)}, 2, 8);
    CHECK(overlap && !UsdGenDeviceCurveGroupPublisher::Build(MakeGeneration(overlap), SdfPath("/Groom/Device"), 43));
    auto badBounds = Tile(0, 0, 1, 0, 4);
    badBounds.extentMin[0] = std::numeric_limits<float>::quiet_NaN();
    // Individual non-finite bounds are rejected by Create before mapper
    // admission, unlike gaps/overlaps above.
    auto nanBounds = FakeDevice({badBounds}, 1, 4);
    CHECK(!nanBounds);

    auto empty = EmptyDevice(); CHECK(empty);
    auto emptyResult = UsdGenDeviceCurveGroupPublisher::Build(
        MakeGeneration(empty), SdfPath("/Groom/Device"), 44);
    CHECK(emptyResult && emptyResult.control->GetCandidate() &&
          emptyResult.control->GetCandidate()->members.empty());

    auto badId = MakeGeneration(one, -1);
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(badId, SdfPath("/Groom/Device"), 45));
    auto mismatchedId = MakeGeneration(one, 2);
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(mismatchedId, SdfPath("/Groom/Device"), 45));
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(generation, SdfPath("/Groom/Device"), 0));
    auto badScopeMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    badScopeMutable->devicePresentation->description = SdfPath("/Other");
    UsdGenGenerationConstPtr badScope = badScopeMutable;
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(badScope, SdfPath("/Groom/Device"), 46));
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(
        generation, SdfPath("/Groom/Other"), 46));
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(
        generation, SdfPath::AbsoluteRootPath(), 46));
    auto badNamespaceMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    badNamespaceMutable->devicePresentation->renderNamespace = SdfPath("/Other/__usdGenRender");
    UsdGenGenerationConstPtr badNamespace = badNamespaceMutable;
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(badNamespace, SdfPath("/Groom/Device"), 46));
    auto badPathMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    badPathMutable->devicePresentation->materialPath = SdfPath("relativeMaterial");
    UsdGenGenerationConstPtr badPath = badPathMutable;
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(badPath, SdfPath("/Groom/Device"), 46));
    auto badOriginMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    badOriginMutable->devicePresentation->primOrigin = SdfPath("relativeOrigin");
    UsdGenGenerationConstPtr badOrigin = badOriginMutable;
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(badOrigin, SdfPath("/Groom/Device"), 46));
    auto badDependencyMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    badDependencyMutable->devicePresentation->dependencySurface = SdfPath("relativeSurface");
    UsdGenGenerationConstPtr badDependency = badDependencyMutable;
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(badDependency, SdfPath("/Groom/Device"), 46));
    auto nanMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    nanMutable->devicePresentation->xformMatrix[0][0] = std::numeric_limits<double>::quiet_NaN();
    UsdGenGenerationConstPtr nan = nanMutable;
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(nan, SdfPath("/Groom/Device"), 46));
    auto hostPayloadMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    hostPayloadMutable->tiles.emplace_back();
    UsdGenGenerationConstPtr hostPayload = hostPayloadMutable;
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(hostPayload, SdfPath("/Groom/Device"), 47));
    auto guidesMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    guidesMutable->guides.emplace_back();
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(guidesMutable, SdfPath("/Groom/Device"), 47));
    auto instancersMutable = std::make_shared<UsdGenGeneration>(*MakeGeneration(one));
    instancersMutable->instancers.emplace_back();
    CHECK(!UsdGenDeviceCurveGroupPublisher::Build(instancersMutable, SdfPath("/Groom/Device"), 47));
    std::puts("device curve group publisher: PASS");
    return 0;
}
