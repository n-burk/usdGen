// Native retained-scene-index coverage for GPU group staging.  Results are
// constructed with the real ready-provider factory; no controller, GL, or
// producer Prepare callback is bypassed by the staging assertions.
#include "pxr/pxr.h"

#include "pxr/base/gf/bbox3d.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/range3d.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupController.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupMemberDataSource.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupStagingSceneIndex.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
    void CopyData(HdBufferSourceSharedPtr const&) override {}
    VtValue ReadData(TfToken const&) const override { return {}; }
    int GetElementOffset() const override { return 0; }
    int GetByteOffset(TfToken const&) const override { return 0; }
    size_t GetNumElements() const override { return 1; }
    size_t GetVersion() const override { return 1; }
    void IncrementVersion() override {}
    size_t GetMaxNumElements() const override { return 1; }
    HdBufferArrayUsageHint GetUsageHint() const override { return 0; }
    void SetBufferArray(HdBufferArray*) override {}
    void DebugDump(std::ostream&) const override {}
    HdStBufferResourceSharedPtr GetResource() const override { return {}; }
    HdStBufferResourceSharedPtr GetResource(TfToken const&) override { return {}; }
    HdStBufferResourceNamedList const& GetResources() const override {
        static HdStBufferResourceNamedList const empty; return empty;
    }
protected:
    void const* _GetAggregation() const override { return this; }
};

class _Provider final : public HdStBasisCurvesGpuDataSource {
public:
    HD_DECLARE_DATASOURCE(_Provider);
    HdStBasisCurvesGpuBundleSharedPtr Prepare(
        HdStResourceRegistry *, HdStBasisCurvesGpuPrepareRequest const&) override { return {}; }
};

class _Control final : public HdStBasisCurvesGpuGroupDataSource {
public:
    HD_DECLARE_DATASOURCE(_Control);
    static Handle Make(HdStBasisCurvesGpuGroupCandidateSharedPtr const& candidate) {
        return _Control::New(candidate, std::make_shared<HdStBasisCurvesGpuGroupMailbox>());
    }
    HdStBasisCurvesGpuGroupCandidateSharedPtr GetCandidate() const override { return _candidate; }
    HdStBasisCurvesGpuGroupMailboxSharedPtr GetMailbox() const override { return _mailbox; }
private:
    _Control(HdStBasisCurvesGpuGroupCandidateSharedPtr const& candidate,
             HdStBasisCurvesGpuGroupMailboxSharedPtr const& mailbox)
        : _candidate(candidate), _mailbox(mailbox) {}
    HdStBasisCurvesGpuGroupCandidateSharedPtr const _candidate;
    HdStBasisCurvesGpuGroupMailboxSharedPtr const _mailbox;
};

GfBBox3d _Bounds() {
    return GfBBox3d(GfRange3d(GfVec3d(-1.0), GfVec3d(1.0)), GfMatrix4d(1.0));
}

HdStBasisCurvesGpuGroupCandidateSharedPtr _Candidate(
    SdfPath const& group, uint64_t ticket, uint64_t generation, uint32_t id,
    SdfPath const& memberPath) {
    auto candidate = std::make_shared<HdStBasisCurvesGpuGroupCandidate>();
    candidate->groupPath = group; candidate->ticket = ticket; candidate->generation = generation;
    HdStBasisCurvesGpuGroupMember member;
    member.id = id; member.rprimPath = memberPath;
    member.presentation.curveType = TfToken("linear");
    member.presentation.curveBasis = TfToken("linear");
    member.presentation.curveWrap = TfToken("nonperiodic");
    member.presentation.bounds = _Bounds();
    member.provider = _Provider::New();
    candidate->members.push_back(std::move(member));
    return candidate;
}

HdStBasisCurvesGpuGroupResult _Ready(
    HdStBasisCurvesGpuGroupCandidateSharedPtr const& candidate,
    HdStBasisCurvesGpuRegistryIdentitySharedPtr const& identity) {
    auto bundle = std::make_shared<HdStBasisCurvesGpuBundle>();
    bundle->generation = candidate->generation;
    bundle->curveType = TfToken("linear"); bundle->curveBasis = TfToken("linear");
    bundle->curveWrap = TfToken("nonperiodic"); bundle->bounds = _Bounds();
    bundle->vertexRange = std::make_shared<_Range>();
    for (auto mode : {HdStBasisCurvesGpuTopologyMode::Curves,
                      HdStBasisCurvesGpuTopologyMode::Hull,
                      HdStBasisCurvesGpuTopologyMode::Points}) {
        HdStBasisCurvesGpuTopologyRange range;
        range.mode = mode; range.topologyRange = std::make_shared<_Range>();
        range.drawCountRange = std::make_shared<_Range>();
        bundle->topologyRanges.push_back(std::move(range));
    }
    bundle->ready = [] { return true; };
    return HdStMakeBasisCurvesGpuGroupReadyResult(
        *candidate, identity, {{candidate->members.front().id, bundle}});
}

HdStBasisCurvesGpuGroupDataSourceHandle _ControlAt(
    HdSceneIndexBaseRefPtr const& index, SdfPath const& path) {
    HdSceneIndexPrim const prim = index->GetPrim(path);
    return HdStBasisCurvesGpuGroupDataSource::Cast(
        prim.dataSource ? prim.dataSource->Get(HdStGetBasisCurvesGpuGroupDataSourceToken()) : nullptr);
}

void _CheckRetainedMember(HdSceneIndexBaseRefPtr const& index,
                          SdfPath const& path,
                          HdContainerDataSourceHandle const& expectedData,
                          uint64_t expectedGeneration) {
    HdSceneIndexPrim const prim = index->GetPrim(path);
    CHECK(prim.primType == TfToken("basisCurves"));
    CHECK(prim.dataSource == expectedData);
    auto generation = HdTypedSampledDataSource<uint64_t>::Cast(
        prim.dataSource ? prim.dataSource->Get(TfToken("generation")) : nullptr);
    CHECK(generation);
    if (generation) CHECK(generation->GetTypedValue(0.0f) == expectedGeneration);
}

class _ReentrantObserver final : public HdSceneIndexObserver {
public:
    HdStBasisCurvesGpuGroupStagingSceneIndex* index = nullptr;
    HdStBasisCurvesGpuGroupMailboxSharedPtr mailbox;
    HdStBasisCurvesGpuGroupResult result;
    bool fired = false;
    bool sawStableFirst = false;
    SdfPathVector added;
    SdfPathVector removed;
    void PrimsAdded(HdSceneIndexBase const&, AddedPrimEntries const& entries) override {
        for (auto const& entry : entries) added.push_back(entry.primPath);
        if (index && index->GetPrim(SdfPath("/G0/tile")).primType == TfToken("basisCurves"))
            sawStableFirst = true;
        if (!fired && index && mailbox) { fired = true; mailbox->Post(result); index->Poll(); }
    }
    void PrimsRemoved(HdSceneIndexBase const&, RemovedPrimEntries const& entries) override {
        for (auto const& entry : entries) removed.push_back(entry.primPath);
    }
    void PrimsDirtied(HdSceneIndexBase const&, DirtiedPrimEntries const&) override {}
    void PrimsRenamed(HdSceneIndexBase const&, RenamedPrimEntries const&) override {}
};

void _TestPublicationRetentionAndHierarchy() {
    auto owner = std::make_shared<int>(1);
    auto registryOwner = std::shared_ptr<HdStResourceRegistry const>(
        owner, reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x1)));
    auto identity = std::make_shared<HdStBasisCurvesGpuRegistryIdentity>(
        reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x1)), 1,
        std::weak_ptr<void const>(registryOwner));
    auto input = HdRetainedSceneIndex::New();
    auto first = _Candidate(SdfPath("/G0"), 1, 10, 1, SdfPath("/G0/tile"));
    auto second = _Candidate(SdfPath("/G1"), 1, 11, 2, SdfPath("/G1/tiles/tile"));
    auto third = _Candidate(SdfPath("/G2"), 1, 12, 3, SdfPath("/G2/tile"));
    HdDataSourceBaseHandle firstData = _Control::Make(first);
    HdDataSourceBaseHandle secondData = _Control::Make(second);
    HdDataSourceBaseHandle thirdData = _Control::Make(third);
    TfToken const token = HdStGetBasisCurvesGpuGroupDataSourceToken();
    input->AddPrims({
        {SdfPath("/G0"), TfToken("scope"), HdRetainedContainerDataSource::New(1, &token, &firstData)},
        {SdfPath("/G1"), TfToken("scope"), HdRetainedContainerDataSource::New(1, &token, &secondData)},
        {SdfPath("/G2"), TfToken("scope"), HdRetainedContainerDataSource::New(1, &token, &thirdData)}});
    HdSceneIndexBaseRefPtr index = HdStBasisCurvesGpuGroupStagingSceneIndex::New(input);
    auto* staging = static_cast<HdStBasisCurvesGpuGroupStagingSceneIndex*>(index.operator->());
    auto firstMailbox = _ControlAt(index, SdfPath("/G0"))->GetMailbox();
    auto secondMailbox = _ControlAt(index, SdfPath("/G1"))->GetMailbox();
    auto thirdMailbox = _ControlAt(index, SdfPath("/G2"))->GetMailbox();
    _ReentrantObserver observer;
    observer.index = staging;
    observer.mailbox = thirdMailbox; observer.result = _Ready(third, identity);
    index->AddObserver(HdSceneIndexObserverPtr(&observer));
    firstMailbox->Post(_Ready(first, identity));
    secondMailbox->Post(_Ready(second, identity));
    staging->Poll();
    CHECK(observer.fired);
    CHECK(observer.sawStableFirst);
    HdContainerDataSourceHandle const firstMember = index->GetPrim(SdfPath("/G0/tile")).dataSource;
    _CheckRetainedMember(index, SdfPath("/G0/tile"), firstMember, 10);
    CHECK(index->GetPrim(SdfPath("/G1/tiles/tile")).primType == TfToken("basisCurves"));
    CHECK(index->GetPrim(SdfPath("/G2/tile")).primType == TfToken("basisCurves"));
    CHECK(index->GetPrim(SdfPath("/G1/tiles")).primType == TfToken("scope"));
    SdfPathVector const g1Children = index->GetChildPrimPaths(SdfPath("/G1"));
    CHECK(std::find(g1Children.begin(), g1Children.end(), SdfPath("/G1/tiles")) != g1Children.end());
    CHECK(std::find(observer.added.begin(), observer.added.end(), SdfPath("/G1/tiles")) !=
          observer.added.end());

    // Same ticket/candidate retains the already-consumed mailbox.  A duplicate
    // ready post cannot publish a second packet or replace the retained value.
    HdDataSourceBaseHandle sameData = _Control::Make(first);
    input->AddPrims({{SdfPath("/G0"), TfToken("scope"),
                      HdRetainedContainerDataSource::New(1, &token, &sameData)}});
    CHECK(_ControlAt(index, SdfPath("/G0"))->GetMailbox() == firstMailbox);
    size_t const noticesBeforeDuplicate = observer.added.size();
    firstMailbox->Post(_Ready(first, identity)); staging->Poll();
    CHECK(observer.added.size() == noticesBeforeDuplicate);
    _CheckRetainedMember(index, SdfPath("/G0/tile"), firstMember, 10);

    // A matching rejected terminal result leaves the entire old datasource,
    // including its typed generation, immutable and visible.
    auto replacement = _Candidate(SdfPath("/G0"), 2, 10, 1, SdfPath("/G0/tile"));
    HdDataSourceBaseHandle replacementData = _Control::Make(replacement);
    input->AddPrims({{SdfPath("/G0"), TfToken("scope"),
                      HdRetainedContainerDataSource::New(1, &token, &replacementData)}});
    auto replacementMailbox = _ControlAt(index, SdfPath("/G0"))->GetMailbox();
    CHECK(replacementMailbox != firstMailbox);
    HdStBasisCurvesGpuGroupResult rejected;
    rejected.status = HdStBasisCurvesGpuGroupResultStatus::Rejected;
    rejected.groupPath = replacement->groupPath; rejected.ticket = replacement->ticket;
    rejected.generation = replacement->generation;
    replacementMailbox->Post(rejected); staging->Poll();
    _CheckRetainedMember(index, SdfPath("/G0/tile"), firstMember, 10);

    // Public result fields are mutable.  Exercise ticket, rprim, and registry
    // mismatches independently while keeping the candidate generation equal.
    auto crossCandidate = _Candidate(SdfPath("/G0"), 3, 10, 1, SdfPath("/G0/tile"));
    HdDataSourceBaseHandle crossData = _Control::Make(crossCandidate);
    input->AddPrims({{SdfPath("/G0"), TfToken("scope"),
                      HdRetainedContainerDataSource::New(1, &token, &crossData)}});
    auto crossMailbox = _ControlAt(index, SdfPath("/G0"))->GetMailbox();
    HdStBasisCurvesGpuGroupResult swapped = _Ready(first, identity);
    swapped.groupPath = crossCandidate->groupPath;
    swapped.ticket = crossCandidate->ticket;
    swapped.generation = crossCandidate->generation;
    crossMailbox->Post(swapped); staging->Poll();
    _CheckRetainedMember(index, SdfPath("/G0/tile"), firstMember, 10);

    auto pathCandidate = _Candidate(SdfPath("/G0"), 4, 10, 1, SdfPath("/G0/tile"));
    HdDataSourceBaseHandle pathData = _Control::Make(pathCandidate);
    input->AddPrims({{SdfPath("/G0"), TfToken("scope"),
                      HdRetainedContainerDataSource::New(1, &token, &pathData)}});
    auto wrongPathCandidate = _Candidate(SdfPath("/Foreign"), 4, 10, 1,
                                         SdfPath("/Foreign/tile"));
    HdStBasisCurvesGpuGroupResult wrongPath = _Ready(wrongPathCandidate, identity);
    wrongPath.groupPath = pathCandidate->groupPath;
    wrongPath.ticket = pathCandidate->ticket;
    wrongPath.generation = pathCandidate->generation;
    _ControlAt(index, SdfPath("/G0"))->GetMailbox()->Post(wrongPath); staging->Poll();
    _CheckRetainedMember(index, SdfPath("/G0/tile"), firstMember, 10);

    auto secondOwner = std::make_shared<int>(2);
    auto secondRegistryOwner = std::shared_ptr<HdStResourceRegistry const>(
        secondOwner, reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x2)));
    auto secondIdentity = std::make_shared<HdStBasisCurvesGpuRegistryIdentity>(
        reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x2)), 2,
        std::weak_ptr<void const>(secondRegistryOwner));
    auto registryCandidate = _Candidate(SdfPath("/G0"), 5, 10, 1, SdfPath("/G0/tile"));
    HdDataSourceBaseHandle registryData = _Control::Make(registryCandidate);
    input->AddPrims({{SdfPath("/G0"), TfToken("scope"),
                      HdRetainedContainerDataSource::New(1, &token, &registryData)}});
    HdStBasisCurvesGpuGroupResult wrongRegistry = _Ready(registryCandidate, secondIdentity);
    wrongRegistry.registryIdentity = identity;
    _ControlAt(index, SdfPath("/G0"))->GetMailbox()->Post(wrongRegistry); staging->Poll();
    _CheckRetainedMember(index, SdfPath("/G0/tile"), firstMember, 10);

    // Removing only a pending member cancels that pending mailbox; it must not
    // turn an accepted synthetic member into a removal.
    auto pending = _Candidate(SdfPath("/G0"), 6, 10, 9, SdfPath("/G0/pending_tile"));
    HdDataSourceBaseHandle pendingData = _Control::Make(pending);
    input->AddPrims({{SdfPath("/G0"), TfToken("scope"),
                      HdRetainedContainerDataSource::New(1, &token, &pendingData)}});
    auto pendingMailbox = _ControlAt(index, SdfPath("/G0"))->GetMailbox();
    input->RemovePrims({SdfPath("/G0/pending_tile")});
    pendingMailbox->Post(_Ready(pending, identity)); staging->Poll();
    _CheckRetainedMember(index, SdfPath("/G0/tile"), firstMember, 10);
    size_t const removalsBeforeRetainedPath = observer.removed.size();
    input->RemovePrims({SdfPath("/G0/tile")});
    _CheckRetainedMember(index, SdfPath("/G0/tile"), firstMember, 10);
    CHECK(observer.removed.size() == removalsBeforeRetainedPath);

    input->RemovePrims({SdfPath("/G0")});
    CHECK(index->GetPrim(SdfPath("/G0/tile")).primType.IsEmpty());
    CHECK(index->GetPrim(SdfPath("/G1/tiles/tile")).primType == TfToken("basisCurves"));
    input->RemovePrims({SdfPath("/G1")});
    CHECK(index->GetPrim(SdfPath("/G1/tiles")).primType.IsEmpty());
    CHECK(std::find(observer.removed.begin(), observer.removed.end(), SdfPath("/G1/tiles")) !=
          observer.removed.end());
    index->RemoveObserver(HdSceneIndexObserverPtr(&observer));
}

} // anonymous namespace

int main() {
    _TestPublicationRetentionAndHierarchy();
    return failures == 0 ? 0 : 1;
}
