// Native retained-scene-index coverage for GPU group staging.  Results are
// constructed with the real ready-provider factory; no controller, GL, or
// producer Prepare callback is bypassed by the staging assertions.
#include "pxr/pxr.h"

#include "pxr/base/gf/bbox3d.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/range3d.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
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

// The retained scene index only publishes notifications; the scene-index
// contract permits a backing datasource to change before a Dirtied notice.
// This small fixture exercises disappearance of the exact control child.
class _MutableControlContainer final : public HdContainerDataSource {
public:
    HD_DECLARE_DATASOURCE(_MutableControlContainer);

    TfTokenVector GetNames() override {
        TfTokenVector names;
        names.reserve(_entries.size());
        for (auto const& entry : _entries) names.push_back(entry.first);
        return names;
    }
    HdDataSourceBaseHandle Get(TfToken const& name) override {
        for (auto const& entry : _entries)
            if (entry.first == name) return entry.second;
        return {};
    }
    void Set(TfToken const& name, HdDataSourceBaseHandle const& value) {
        for (auto& entry : _entries) {
            if (entry.first == name) { entry.second = value; return; }
        }
        _entries.emplace_back(name, value);
    }

private:
    explicit _MutableControlContainer(
        std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> entries)
        : _entries(std::move(entries)) {}
    std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> _entries;
};

GfBBox3d _Bounds() {
    return GfBBox3d(GfRange3d(GfVec3d(-1.0), GfVec3d(1.0)), GfMatrix4d(1.0));
}

HdStBasisCurvesGpuGroupCandidateSharedPtr _Candidate(
    SdfPath const& group, uint64_t ticket, uint64_t generation, uint32_t id,
    SdfPath const& memberPath, bool ownsSubtree = false) {
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
    candidate->ownsSubtree = ownsSubtree;
    return candidate;
}

HdStBasisCurvesGpuGroupResult _Ready(
    HdStBasisCurvesGpuGroupCandidateSharedPtr const& candidate,
    HdStBasisCurvesGpuRegistryIdentitySharedPtr const& identity) {
    std::vector<HdStBasisCurvesGpuGroupPreparedMember> prepared;
    for (auto const& member : candidate->members) {
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
        prepared.push_back({member.id, std::move(bundle)});
    }
    return HdStMakeBasisCurvesGpuGroupReadyResult(*candidate, identity, prepared);
}

HdStBasisCurvesGpuGroupResult _EmptyReady(
    HdStBasisCurvesGpuGroupCandidateSharedPtr const& candidate,
    HdStBasisCurvesGpuRegistryIdentitySharedPtr const& identity) {
    return HdStMakeBasisCurvesGpuGroupReadyResult(*candidate, identity, {});
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

class _VisibilityObserver final : public HdSceneIndexObserver {
public:
    HdStBasisCurvesGpuGroupStagingSceneIndex* index = nullptr;
    SdfPath scope;
    SdfPathVector added;
    SdfPathVector removed;
    SdfPathVector dirtied;
    bool coherent = true;

    void _CheckSnapshot() {
        if (!index || index->GetPrim(scope).primType.IsEmpty()) { coherent = false; return; }
        for (SdfPath const& child : index->GetChildPrimPaths(scope))
            if (index->GetPrim(child).primType.IsEmpty()) coherent = false;
    }
    void PrimsAdded(HdSceneIndexBase const&, AddedPrimEntries const& entries) override {
        for (auto const& entry : entries) added.push_back(entry.primPath);
        _CheckSnapshot();
    }
    void PrimsRemoved(HdSceneIndexBase const&, RemovedPrimEntries const& entries) override {
        for (auto const& entry : entries) removed.push_back(entry.primPath);
        _CheckSnapshot();
    }
    void PrimsDirtied(HdSceneIndexBase const&, DirtiedPrimEntries const& entries) override {
        for (auto const& entry : entries) dirtied.push_back(entry.primPath);
        _CheckSnapshot();
    }
    void PrimsRenamed(HdSceneIndexBase const&, RenamedPrimEntries const&) override {}
};

bool _Contains(SdfPathVector const& paths, SdfPath const& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

void _TestControlDisappearance(
    HdStBasisCurvesGpuRegistryIdentitySharedPtr const& identity) {
    TfToken const token = HdStGetBasisCurvesGpuGroupDataSourceToken();
    for (bool const empty : {false, true}) {
        for (bool const dirtied : {false, true}) {
            std::string const suffix = std::string(empty ? "empty" : "nonempty") +
                (dirtied ? "_dirtied" : "_added");
            SdfPath const scope("/ControlGone/" + suffix);
            auto makeCandidate = [&](uint64_t ticket) {
                auto mutableCandidate =
                    std::make_shared<HdStBasisCurvesGpuGroupCandidate>();
                mutableCandidate->groupPath = scope;
                mutableCandidate->ticket = ticket;
                mutableCandidate->generation = 70 + uint64_t(empty) * 2 + uint64_t(dirtied);
                if (!empty) {
                    *mutableCandidate = *_Candidate(scope, mutableCandidate->ticket,
                                                    mutableCandidate->generation, 1,
                                                    scope.AppendChild(TfToken("tile")));
                }
                return HdStBasisCurvesGpuGroupCandidateSharedPtr(mutableCandidate);
            };
            auto const candidate = makeCandidate(100 + uint64_t(empty) * 2 + uint64_t(dirtied));
            HdStBasisCurvesGpuGroupResult const ready =
                empty ? _EmptyReady(candidate, identity) : _Ready(candidate, identity);
            CHECK(ready.status == HdStBasisCurvesGpuGroupResultStatus::Ready);
            HdDataSourceBaseHandle const control = _Control::Make(candidate);
            auto input = HdRetainedSceneIndex::New();
            HdContainerDataSourceHandle source;
            _MutableControlContainer::Handle mutableSource;
            if (dirtied) {
                std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> entries{{token, control}};
                mutableSource = _MutableControlContainer::New(std::move(entries));
                source = mutableSource;
            } else {
                source = HdRetainedContainerDataSource::New(1, &token, &control);
            }
            input->AddPrims({{scope, TfToken("scope"), source}});
            HdSceneIndexBaseRefPtr index = HdStBasisCurvesGpuGroupStagingSceneIndex::New(input);
            auto* staging = static_cast<HdStBasisCurvesGpuGroupStagingSceneIndex*>(
                index.operator->());
            auto mailbox = _ControlAt(index, scope)->GetMailbox();
            mailbox->Post(ready);
            staging->Poll();
            CHECK(_ControlAt(index, scope));
            if (!empty)
                CHECK(index->GetPrim(scope.AppendChild(TfToken("tile"))).primType ==
                      TfToken("basisCurves"));

            // A newer candidate is still pending when the control vanishes.
            // Its unconsumed mailbox, rather than the terminal accepted one,
            // must be canceled and must not restore the retained subtree.
            auto const replacement = makeCandidate(candidate->ticket + 10);
            HdStBasisCurvesGpuGroupResult const replacementReady =
                empty ? _EmptyReady(replacement, identity) : _Ready(replacement, identity);
            CHECK(replacementReady.status == HdStBasisCurvesGpuGroupResultStatus::Ready);
            HdDataSourceBaseHandle const replacementControl = _Control::Make(replacement);
            if (dirtied) {
                mutableSource->Set(token, replacementControl);
                input->DirtyPrims({{scope, HdDataSourceLocatorSet(HdDataSourceLocator())}});
            } else {
                input->AddPrims({{scope, TfToken("scope"),
                                  HdRetainedContainerDataSource::New(
                                      1, &token, &replacementControl)}});
            }
            auto pendingMailbox = _ControlAt(index, scope)->GetMailbox();
            CHECK(pendingMailbox != mailbox);

            if (dirtied) {
                mutableSource->Set(token, {});
                input->DirtyPrims({{scope, HdDataSourceLocatorSet(HdDataSourceLocator())}});
            } else {
                input->AddPrims({{scope, TfToken("scope"),
                                  HdRetainedContainerDataSource::New()}});
            }
            // Empty groups have no synthetic member to inspect: the control
            // itself is the required acceptance/retention oracle.
            CHECK(!_ControlAt(index, scope));
            CHECK(mailbox->IsCanceled());
            CHECK(pendingMailbox->IsCanceled());
            pendingMailbox->Post(replacementReady);
            staging->Poll();
            CHECK(!_ControlAt(index, scope));
            if (!empty)
                CHECK(index->GetPrim(scope.AppendChild(TfToken("tile"))).primType.IsEmpty());
        }
    }
}

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

void _TestOwnedSubtreeTransitions(
    HdStBasisCurvesGpuRegistryIdentitySharedPtr const& identity) {
    TfToken const token = HdStGetBasisCurvesGpuGroupDataSourceToken();
    SdfPath const scope("/Owned");
    SdfPath const cpuTile("/Owned/cpuTile");
    SdfPath const cpuParent("/Owned/transform");
    SdfPath const cpuLeaf("/Owned/transform/cpuLeaf");
    auto input = HdRetainedSceneIndex::New();
    auto first = _Candidate(scope, 1, 90, 1, cpuTile, true);
    auto firstMutable = std::make_shared<HdStBasisCurvesGpuGroupCandidate>(*first);
    HdStBasisCurvesGpuGroupMember nested;
    nested.id = 2;
    nested.rprimPath = cpuParent.AppendChild(TfToken("gpuNested"));
    nested.presentation = firstMutable->members.front().presentation;
    nested.provider = _Provider::New();
    firstMutable->members.push_back(std::move(nested));
    first = HdStBasisCurvesGpuGroupCandidateSharedPtr(firstMutable);
    HdDataSourceBaseHandle firstControl = _Control::Make(first);
    std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> controlEntries{
        {token, firstControl}};
    auto source = _MutableControlContainer::New(std::move(controlEntries));
    HdContainerDataSourceHandle const cpuTileData = HdRetainedContainerDataSource::New();
    input->AddPrims({
        {scope, TfToken("scope"), source},
        {cpuTile, TfToken("basisCurves"), cpuTileData},
        {cpuParent, TfToken("xform"), HdRetainedContainerDataSource::New()},
        {cpuLeaf, TfToken("basisCurves"), HdRetainedContainerDataSource::New()}});
    HdSceneIndexBaseRefPtr index = HdStBasisCurvesGpuGroupStagingSceneIndex::New(input);
    auto* staging = static_cast<HdStBasisCurvesGpuGroupStagingSceneIndex*>(index.operator->());
    auto firstMailbox = _ControlAt(index, scope)->GetMailbox();
    _VisibilityObserver observer;
    observer.index = staging;
    observer.scope = scope;
    index->AddObserver(HdSceneIndexObserverPtr(&observer));

    // Pending ownership has no visibility effect: CPU remains the exact
    // last-good subtree until the complete group result is accepted.
    CHECK(index->GetPrim(cpuTile).primType == TfToken("basisCurves"));
    CHECK(index->GetPrim(cpuTile).dataSource == cpuTileData);
    firstMailbox->Post(_Ready(first, identity));
    staging->Poll();
    CHECK(index->GetPrim(cpuTile).primType == TfToken("basisCurves"));
    CHECK(index->GetPrim(cpuTile).dataSource != cpuTileData);
    CHECK(_Contains(observer.removed, cpuTile));
    CHECK(_Contains(observer.removed, cpuParent));
    CHECK(_Contains(observer.added, cpuTile));
    CHECK(_Contains(observer.added, cpuParent));
    // An authored parent colliding with a synthetic GPU parent is represented
    // as a synthetic scope, not leaked authored data.
    CHECK(index->GetPrim(cpuParent).primType == TfToken("scope"));
    CHECK(index->GetPrim(cpuParent.AppendChild(TfToken("gpuNested"))).primType ==
          TfToken("basisCurves"));

    // Hidden upstream churn is neither forwarded into visibility nor allowed
    // to cancel the accepted owner.
    SdfPath const hiddenNew("/Owned/transform/newCpuLeaf");
    size_t const noticesBeforeHiddenChurn =
        observer.added.size() + observer.removed.size() + observer.dirtied.size();
    input->AddPrims({{hiddenNew, TfToken("basisCurves"),
                      HdRetainedContainerDataSource::New()}});
    input->RemovePrims({cpuTile});
    CHECK(index->GetPrim(cpuTile).primType ==
          TfToken("basisCurves"));
    CHECK(index->GetPrim(hiddenNew).primType.IsEmpty());
    CHECK(observer.added.size() + observer.removed.size() + observer.dirtied.size() ==
          noticesBeforeHiddenChurn);

    // A foreign nested control is likewise masked rather than admitted.
    SdfPath const foreignScope = scope.AppendChild(TfToken("foreign"));
    auto foreign = _Candidate(foreignScope, 1, 1, 1,
                              foreignScope.AppendChild(TfToken("tile")));
    HdDataSourceBaseHandle foreignControl = _Control::Make(foreign);
    input->AddPrims({{foreignScope, TfToken("scope"),
                      HdRetainedContainerDataSource::New(1, &token, &foreignControl)}});
    CHECK(index->GetPrim(foreignScope).primType.IsEmpty());

    // A rejected owning replacement retains the complete accepted GPU group.
    auto replacement = _Candidate(scope, 2, 91, 2,
        scope.AppendChild(TfToken("replacement")), true);
    HdDataSourceBaseHandle replacementControl = _Control::Make(replacement);
    source->Set(token, replacementControl);
    input->DirtyPrims({{scope, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    auto replacementMailbox = _ControlAt(index, scope)->GetMailbox();
    CHECK(replacementMailbox != firstMailbox);
    HdStBasisCurvesGpuGroupResult rejected;
    rejected.groupPath = scope; rejected.ticket = replacement->ticket;
    rejected.generation = replacement->generation;
    rejected.status = HdStBasisCurvesGpuGroupResultStatus::Rejected;
    replacementMailbox->Post(rejected); staging->Poll();
    CHECK(index->GetPrim(cpuTile).primType ==
          TfToken("basisCurves"));
    CHECK(index->GetPrim(hiddenNew).primType.IsEmpty());

    // A ready empty owner masks every CPU descendant as well.
    auto empty = std::make_shared<HdStBasisCurvesGpuGroupCandidate>();
    empty->groupPath = scope; empty->ticket = 3; empty->generation = 92;
    empty->ownsSubtree = true;
    HdStBasisCurvesGpuGroupCandidateSharedPtr emptyCandidate(empty);
    HdDataSourceBaseHandle emptyControl = _Control::Make(emptyCandidate);
    source->Set(token, emptyControl);
    input->DirtyPrims({{scope, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    auto emptyMailbox = _ControlAt(index, scope)->GetMailbox();
    emptyMailbox->Post(_EmptyReady(emptyCandidate, identity)); staging->Poll();
    CHECK(index->GetPrim(cpuTile).primType.IsEmpty());
    CHECK(index->GetPrim(cpuParent).primType.IsEmpty());
    CHECK(index->GetChildPrimPaths(scope).empty());

    // Clearing exactly the control is the GPU-to-CPU handoff: it cancels the
    // pending/accepted state and exposes the current (including churned) CPU
    // subtree atomically on the next scene-index packet.
    source->Set(token, {});
    input->DirtyPrims({{scope, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    CHECK(!_ControlAt(index, scope));
    CHECK(index->GetPrim(cpuParent).primType == TfToken("xform"));
    CHECK(index->GetPrim(hiddenNew).primType == TfToken("basisCurves"));
    SdfPathVector const children = index->GetChildPrimPaths(scope);
    CHECK(std::find(children.begin(), children.end(), cpuParent) != children.end());
    CHECK(_Contains(observer.added, cpuParent));
    CHECK(_Contains(observer.added, hiddenNew));
    CHECK(observer.coherent);
    index->RemoveObserver(HdSceneIndexObserverPtr(&observer));
}

} // anonymous namespace

int main() {
    _TestPublicationRetentionAndHierarchy();
    auto owner = std::make_shared<int>(3);
    auto registryOwner = std::shared_ptr<HdStResourceRegistry const>(
        owner, reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x3)));
    auto identity = std::make_shared<HdStBasisCurvesGpuRegistryIdentity>(
        reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x3)), 3,
        std::weak_ptr<void const>(registryOwner));
    _TestControlDisappearance(identity);
    _TestOwnedSubtreeTransitions(identity);
    return failures == 0 ? 0 : 1;
}
