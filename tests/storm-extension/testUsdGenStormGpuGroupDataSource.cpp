// Focused native coverage for the renderer-neutral group transaction SDK.
// No GL context is needed: sentinel registry addresses exercise the API state
// machine only, not actual Hgi/resource-registry lifetime.  The test verifies
// that the queue/proxy boundary cannot retain a fallible producer callback.
#include "pxr/pxr.h"

#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"

#include <cstdio>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int failures = 0;
#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; \
} } while (false)

class _Range final : public HdStBufferArrayRange {
public:
    _Range() : HdStBufferArrayRange(nullptr) {}
    bool IsValid() const override { return true; }
    bool IsAssigned() const override { return true; }
    bool IsImmutable() const override { return true; }
    bool RequiresStaging() const override { return false; }
    bool Resize(int) override { return false; }
    void CopyData(HdBufferSourceSharedPtr const &) override {}
    VtValue ReadData(TfToken const &) const override { return {}; }
    int GetElementOffset() const override { return 0; }
    int GetByteOffset(TfToken const &) const override { return 0; }
    size_t GetNumElements() const override { return 1; }
    size_t GetVersion() const override { return 1; }
    void IncrementVersion() override {}
    size_t GetMaxNumElements() const override { return 1; }
    HdBufferArrayUsageHint GetUsageHint() const override { return 0; }
    void SetBufferArray(HdBufferArray *) override {}
    void DebugDump(std::ostream &) const override {}
    HdStBufferResourceSharedPtr GetResource() const override { return {}; }
    HdStBufferResourceSharedPtr GetResource(TfToken const &) override { return {}; }
    HdStBufferResourceNamedList const &GetResources() const override {
        static HdStBufferResourceNamedList const empty;
        return empty;
    }
protected:
    void const *_GetAggregation() const override { return this; }
};

class _Provider final : public HdStBasisCurvesGpuDataSource {
public:
    HD_DECLARE_DATASOURCE(_Provider);
    HdStBasisCurvesGpuBundleSharedPtr Prepare(
        HdStResourceRegistry *, HdStBasisCurvesGpuPrepareRequest const &) override {
        return {};
    }
};

GfBBox3d
_Bounds(double tx = 0.0)
{
    GfMatrix4d matrix(1.0);
    matrix.SetTranslate(GfVec3d(tx, 0.0, 0.0));
    return GfBBox3d(GfRange3d(GfVec3d(-1.0), GfVec3d(1.0)), matrix);
}

std::shared_ptr<HdStBasisCurvesGpuBundle>
_Bundle(uint64_t generation, GfBBox3d const &bounds, bool ready = true)
{
    auto result = std::make_shared<HdStBasisCurvesGpuBundle>();
    result->generation = generation;
    result->curveType = TfToken("linear");
    result->curveBasis = TfToken("linear");
    result->curveWrap = TfToken("nonperiodic");
    result->bounds = bounds;
    result->vertexRange = std::make_shared<_Range>();
    for (HdStBasisCurvesGpuTopologyMode mode : {
             HdStBasisCurvesGpuTopologyMode::Curves,
             HdStBasisCurvesGpuTopologyMode::Hull,
             HdStBasisCurvesGpuTopologyMode::Points}) {
        HdStBasisCurvesGpuTopologyRange topology;
        topology.mode = mode;
        topology.topologyRange = std::make_shared<_Range>();
        topology.drawCountRange = std::make_shared<_Range>();
        result->topologyRanges.push_back(std::move(topology));
    }
    result->ready = [ready] { return ready; };
    return result;
}

HdStBasisCurvesGpuGroupMember
_Member(uint32_t id, GfBBox3d const &bounds)
{
    HdStBasisCurvesGpuGroupMember result;
    result.id = id;
    result.rprimPath = SdfPath("/Groom/__usdGenRender/tile_" + std::to_string(id));
    result.presentation.curveType = TfToken("linear");
    result.presentation.curveBasis = TfToken("linear");
    result.presentation.curveWrap = TfToken("nonperiodic");
    result.presentation.bounds = bounds;
    result.provider = _Provider::New();
    return result;
}

HdStBasisCurvesGpuGroupCandidate
_Candidate()
{
    HdStBasisCurvesGpuGroupCandidate candidate;
    candidate.groupPath = SdfPath("/Groom/__usdGenRender");
    candidate.ticket = 9;
    candidate.generation = 7;
    candidate.members.push_back(_Member(3, _Bounds()));
    return candidate;
}

void
_TestCandidateAndResult()
{
    HdStBasisCurvesGpuGroupCandidate const candidate = _Candidate();
    CHECK(HdStValidateBasisCurvesGpuGroupCandidate(candidate));

    HdStBasisCurvesGpuGroupCandidate empty = candidate;
    empty.members.clear();
    CHECK(HdStValidateBasisCurvesGpuGroupCandidate(empty));

    HdStBasisCurvesGpuGroupCandidate duplicate = candidate;
    duplicate.members.push_back(duplicate.members.front());
    CHECK(!HdStValidateBasisCurvesGpuGroupCandidate(duplicate));

    HdStBasisCurvesGpuGroupCandidate missingPresentationValue = candidate;
    missingPresentationValue.members.front().presentation.curveBasis = TfToken();
    CHECK(!HdStValidateBasisCurvesGpuGroupCandidate(missingPresentationValue));

    HdStBasisCurvesGpuGroupCandidate defaultMaterialPurpose = candidate;
    defaultMaterialPurpose.members.front().presentation.materialPath =
        SdfPath("/Looks/Accepted");
    CHECK(HdStValidateBasisCurvesGpuGroupCandidate(defaultMaterialPurpose));
    HdStBasisCurvesGpuGroupCandidate relativeMaterial = defaultMaterialPurpose;
    relativeMaterial.members.front().presentation.materialPath =
        SdfPath("Looks/Rejected");
    CHECK(!HdStValidateBasisCurvesGpuGroupCandidate(relativeMaterial));
    HdStBasisCurvesGpuGroupCandidate relativeOrigin = candidate;
    relativeOrigin.members.front().presentation.primOrigin = SdfPath("origin");
    CHECK(!HdStValidateBasisCurvesGpuGroupCandidate(relativeOrigin));
    HdStBasisCurvesGpuGroupCandidate relativeDependency = candidate;
    relativeDependency.members.front().presentation.dependencySurface =
        SdfPath("surface");
    CHECK(!HdStValidateBasisCurvesGpuGroupCandidate(relativeDependency));

    auto owner = std::make_shared<int>(41);
    auto registryOwner = std::shared_ptr<HdStResourceRegistry const>(
        owner, reinterpret_cast<HdStResourceRegistry *>(uintptr_t(0x1)));
    auto identity = std::make_shared<HdStBasisCurvesGpuRegistryIdentity>(
        reinterpret_cast<HdStResourceRegistry *>(uintptr_t(0x1)), 41,
        std::weak_ptr<void const>(registryOwner));
    std::vector<HdStBasisCurvesGpuGroupPreparedMember> prepared{
        {3, _Bundle(7, _Bounds())}};
    auto result = HdStMakeBasisCurvesGpuGroupReadyResult(
        candidate, identity, prepared);
    CHECK(result.status == HdStBasisCurvesGpuGroupResultStatus::Ready);
    CHECK(result.providers.size() == 1);

    auto wrongMatrix = prepared;
    wrongMatrix.front().bundle = _Bundle(7, _Bounds(2.0));
    CHECK(HdStMakeBasisCurvesGpuGroupReadyResult(candidate, identity, wrongMatrix)
              .status == HdStBasisCurvesGpuGroupResultStatus::Rejected);

    HdStBasisCurvesGpuGroupCandidate mismatchedPresentation = candidate;
    mismatchedPresentation.members.front().presentation.bounds = _Bounds(3.0);
    CHECK(HdStValidateBasisCurvesGpuGroupCandidate(mismatchedPresentation));
    CHECK(HdStMakeBasisCurvesGpuGroupReadyResult(
              mismatchedPresentation, identity, prepared)
              .status == HdStBasisCurvesGpuGroupResultStatus::Rejected);

    auto wrongGeneration = prepared;
    wrongGeneration.front().bundle = _Bundle(8, _Bounds());
    CHECK(HdStMakeBasisCurvesGpuGroupReadyResult(candidate, identity, wrongGeneration)
              .status == HdStBasisCurvesGpuGroupResultStatus::Rejected);

    CHECK(HdStMakeBasisCurvesGpuGroupReadyResult(
              candidate, identity, {{3, _Bundle(7, _Bounds(), false)}})
              .status == HdStBasisCurvesGpuGroupResultStatus::Rejected);
    auto throwing = _Bundle(7, _Bounds());
    throwing->ready = []() -> bool { throw 17; };
    CHECK(HdStMakeBasisCurvesGpuGroupReadyResult(candidate, identity, {{3, throwing}})
              .status == HdStBasisCurvesGpuGroupResultStatus::Rejected);
    auto noVertex = _Bundle(7, _Bounds());
    noVertex->vertexRange.reset();
    CHECK(HdStMakeBasisCurvesGpuGroupReadyResult(candidate, identity, {{3, noVertex}})
              .status == HdStBasisCurvesGpuGroupResultStatus::Rejected);
}

void
_TestRegistryScopedCachedReady()
{
    HdStResourceRegistry * const first =
        reinterpret_cast<HdStResourceRegistry *>(uintptr_t(0x1));
    HdStResourceRegistry * const second =
        reinterpret_cast<HdStResourceRegistry *>(uintptr_t(0x2));
    auto owner = std::make_shared<int>(99);
    auto registryOwner = std::shared_ptr<HdStResourceRegistry const>(owner, first);
    auto identity = std::make_shared<HdStBasisCurvesGpuRegistryIdentity>(
        first, 99, std::weak_ptr<void const>(registryOwner));
    HdStBasisCurvesGpuGroupCandidate candidate = _Candidate();
    candidate.ticket = 12;
    auto producerBundle = _Bundle(7, _Bounds());
    int readyCalls = 0;
    producerBundle->ready = [&readyCalls]() -> bool {
        if (++readyCalls == 1) {
            return true;
        }
        throw 17;
    };
    auto acceptedResult = HdStMakeBasisCurvesGpuGroupReadyResult(
        candidate, identity, {{3, producerBundle}});
    CHECK(acceptedResult.status == HdStBasisCurvesGpuGroupResultStatus::Ready);
    CHECK(readyCalls == 1);
    auto proxy = acceptedResult.providers.front();
    HdStBasisCurvesGpuPrepareRequest request;
    request.rprimId = candidate.members.front().rprimPath;
    request.topologyModes = {HdStBasisCurvesGpuTopologyMode::Curves};
    auto accepted = proxy->Prepare(first, request);
    CHECK(accepted && accepted->Ready());
    CHECK(readyCalls == 1);
    CHECK(!proxy->Prepare(second, request));
    request.rprimId = SdfPath("/Groom/__usdGenRender/tile_wrong");
    CHECK(!proxy->Prepare(first, request));
    request.rprimId = candidate.members.front().rprimPath;
    registryOwner.reset();
    owner.reset();
    CHECK(!proxy->Prepare(first, request));
}

void
_TestMailboxCancellation()
{
    HdStBasisCurvesGpuGroupMailbox mailbox;
    HdStBasisCurvesGpuGroupResult result;
    result.groupPath = SdfPath("/Groom/__usdGenRender");
    result.ticket = 3;
    result.generation = 4;
    result.status = HdStBasisCurvesGpuGroupResultStatus::Ready;
    mailbox.Post(result);
    HdStBasisCurvesGpuGroupResult popped;
    CHECK(mailbox.TryPop(&popped));
    CHECK(popped.ticket == 3 && popped.generation == 4);
    mailbox.Post(result);
    mailbox.Cancel();
    CHECK(mailbox.IsCanceled());
    CHECK(!mailbox.TryPop(&popped));
    mailbox.Post(result);
    CHECK(!mailbox.TryPop(&popped));
}

} // anonymous namespace

int
main()
{
    _TestCandidateAndResult();
    _TestRegistryScopedCachedReady();
    _TestMailboxCancellation();
    return failures == 0 ? 0 : 1;
}
