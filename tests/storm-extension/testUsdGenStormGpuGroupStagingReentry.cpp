// Supplemental native probes for staging dispatch boundaries.  This target is
// intentionally separate from the baseline staging test while recovery work
// is under review.
#include "pxr/pxr.h"

#include "pxr/base/gf/bbox3d.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/range3d.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/systemMessages.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupController.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupStagingSceneIndex.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"

#include <cstdio>
#include <cstdint>
#include <memory>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; \
} } while (false)

class _Range final : public HdStBufferArrayRange {
public:
    _Range() : HdStBufferArrayRange(nullptr) {}
    bool IsValid() const override { return true; } bool IsAssigned() const override { return true; }
    bool IsImmutable() const override { return true; } bool RequiresStaging() const override { return false; }
    bool Resize(int) override { return false; } void CopyData(HdBufferSourceSharedPtr const&) override {}
    VtValue ReadData(TfToken const&) const override { return {}; } int GetElementOffset() const override { return 0; }
    int GetByteOffset(TfToken const&) const override { return 0; } size_t GetNumElements() const override { return 1; }
    size_t GetVersion() const override { return 1; } void IncrementVersion() override {}
    size_t GetMaxNumElements() const override { return 1; } HdBufferArrayUsageHint GetUsageHint() const override { return 0; }
    void SetBufferArray(HdBufferArray*) override {} void DebugDump(std::ostream&) const override {}
    HdStBufferResourceSharedPtr GetResource() const override { return {}; }
    HdStBufferResourceSharedPtr GetResource(TfToken const&) override { return {}; }
    HdStBufferResourceNamedList const& GetResources() const override { static HdStBufferResourceNamedList const e; return e; }
protected: void const* _GetAggregation() const override { return this; }
};
class _Provider final : public HdStBasisCurvesGpuDataSource {
public: HD_DECLARE_DATASOURCE(_Provider);
    HdStBasisCurvesGpuBundleSharedPtr Prepare(HdStResourceRegistry*, HdStBasisCurvesGpuPrepareRequest const&) override { return {}; }
};
class _Control final : public HdStBasisCurvesGpuGroupDataSource {
public:
    HD_DECLARE_DATASOURCE(_Control);
    static Handle Make(HdStBasisCurvesGpuGroupCandidateSharedPtr const& c) {
        return _Control::New(c, std::make_shared<HdStBasisCurvesGpuGroupMailbox>());
    }
    HdStBasisCurvesGpuGroupCandidateSharedPtr GetCandidate() const override { return _candidate; }
    HdStBasisCurvesGpuGroupMailboxSharedPtr GetMailbox() const override { return _mailbox; }
private:
    _Control(HdStBasisCurvesGpuGroupCandidateSharedPtr const& c,
             HdStBasisCurvesGpuGroupMailboxSharedPtr const& m) : _candidate(c), _mailbox(m) {}
    HdStBasisCurvesGpuGroupCandidateSharedPtr const _candidate;
    HdStBasisCurvesGpuGroupMailboxSharedPtr const _mailbox;
};

GfBBox3d _Bounds() { return {GfRange3d(GfVec3d(-1.0), GfVec3d(1.0)), GfMatrix4d(1.0)}; }
HdStBasisCurvesGpuGroupCandidateSharedPtr _Candidate(SdfPath const& group, uint64_t ticket,
    uint64_t generation, SdfPath const& path) {
    auto c = std::make_shared<HdStBasisCurvesGpuGroupCandidate>();
    c->groupPath = group; c->ticket = ticket; c->generation = generation;
    HdStBasisCurvesGpuGroupMember m; m.id = 1; m.rprimPath = path;
    m.presentation.curveType = TfToken("linear"); m.presentation.curveBasis = TfToken("linear");
    m.presentation.curveWrap = TfToken("nonperiodic"); m.presentation.bounds = _Bounds();
    m.provider = _Provider::New(); c->members.push_back(std::move(m)); return c;
}
HdStBasisCurvesGpuGroupResult _Ready(HdStBasisCurvesGpuGroupCandidateSharedPtr const& c,
    HdStBasisCurvesGpuRegistryIdentitySharedPtr const& identity) {
    auto bundle = std::make_shared<HdStBasisCurvesGpuBundle>();
    bundle->generation = c->generation; bundle->curveType = TfToken("linear");
    bundle->curveBasis = TfToken("linear"); bundle->curveWrap = TfToken("nonperiodic");
    bundle->bounds = _Bounds(); bundle->vertexRange = std::make_shared<_Range>(); bundle->ready = [] { return true; };
    for (auto mode : {HdStBasisCurvesGpuTopologyMode::Curves, HdStBasisCurvesGpuTopologyMode::Hull,
                      HdStBasisCurvesGpuTopologyMode::Points}) {
        HdStBasisCurvesGpuTopologyRange r; r.mode = mode; r.topologyRange = std::make_shared<_Range>();
        r.drawCountRange = std::make_shared<_Range>(); bundle->topologyRanges.push_back(std::move(r));
    }
    return HdStMakeBasisCurvesGpuGroupReadyResult(*c, identity, {{1, bundle}});
}
HdStBasisCurvesGpuGroupDataSourceHandle _ControlAt(HdSceneIndexBaseRefPtr const& base, SdfPath const& path) {
    auto const prim = base->GetPrim(path);
    return HdStBasisCurvesGpuGroupDataSource::Cast(prim.dataSource ?
        prim.dataSource->Get(HdStGetBasisCurvesGpuGroupDataSourceToken()) : nullptr);
}
uint64_t _Generation(HdSceneIndexBaseRefPtr const& base, SdfPath const& path) {
    auto const prim = base->GetPrim(path);
    auto value = HdTypedSampledDataSource<uint64_t>::Cast(
        prim.dataSource ? prim.dataSource->Get(TfToken("generation")) : nullptr);
    return value ? value->GetTypedValue(0.0f) : 0;
}
HdStBasisCurvesGpuRegistryIdentitySharedPtr _Identity(std::shared_ptr<void const>* lease) {
    auto owner = std::make_shared<int>(41);
    *lease = std::shared_ptr<HdStResourceRegistry const>(owner,
        reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x1)));
    return std::make_shared<HdStBasisCurvesGpuRegistryIdentity>(
        reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x1)), 41, *lease);
}

class _ReentrantObserver final : public HdSceneIndexObserver {
public:
    HdRetainedSceneIndex* input = nullptr; HdSceneIndexBaseRefPtr base; SdfPath scope;
    bool fired = false, sawTransientRemoval = false, sawStableTile = false;
    SdfPath transient = SdfPath("/Transient");
    void PrimsAdded(HdSceneIndexBase const&, AddedPrimEntries const& entries) override {
        if (fired) return;
        for (auto const& entry : entries) if (entry.primType == TfToken("basisCurves")) {
            fired = true; sawStableTile = base->GetPrim(scope.AppendChild(TfToken("tile"))).primType == TfToken("basisCurves");
            input->AddPrims({{transient, TfToken("scope"), HdRetainedContainerDataSource::New()}});
            input->RemovePrims({transient}); return;
        }
    }
    void PrimsRemoved(HdSceneIndexBase const&, RemovedPrimEntries const& entries) override {
        for (auto const& entry : entries) if (entry.primPath == transient) sawTransientRemoval = true;
    }
    void PrimsDirtied(HdSceneIndexBase const&, DirtiedPrimEntries const&) override {}
    void PrimsRenamed(HdSceneIndexBase const&, RenamedPrimEntries const&) override {}
};
class _ThrowOnceObserver final : public HdSceneIndexObserver {
public:
    bool threw = false;
    void PrimsAdded(HdSceneIndexBase const&, AddedPrimEntries const& entries) override {
        if (!threw && !entries.empty()) { threw = true; throw 7; }
    }
    void PrimsRemoved(HdSceneIndexBase const&, RemovedPrimEntries const&) override {}
    void PrimsDirtied(HdSceneIndexBase const&, DirtiedPrimEntries const&) override {}
    void PrimsRenamed(HdSceneIndexBase const&, RenamedPrimEntries const&) override {}
};
class _ReleaseObserver final : public HdSceneIndexObserver {
public:
    HdSceneIndexBaseRefPtr* owner = nullptr; bool released = false;
    void PrimsAdded(HdSceneIndexBase const&, AddedPrimEntries const& entries) override {
        if (!released && !entries.empty() && owner) { released = true; *owner = HdSceneIndexBaseRefPtr(); }
    }
    void PrimsRemoved(HdSceneIndexBase const&, RemovedPrimEntries const&) override {}
    void PrimsDirtied(HdSceneIndexBase const&, DirtiedPrimEntries const&) override {}
    void PrimsRenamed(HdSceneIndexBase const&, RenamedPrimEntries const&) override {}
};

void _TestAsyncZeroAndReentry() {
    std::shared_ptr<void const> lease; auto identity = _Identity(&lease); auto input = HdRetainedSceneIndex::New();
    auto async = _Candidate(SdfPath("/Async"), 1, 1, SdfPath("/Async/tile"));
    auto reentry = _Candidate(SdfPath("/Reentry"), 1, 2, SdfPath("/Reentry/tile"));
    auto zero = std::make_shared<HdStBasisCurvesGpuGroupCandidate>();
    zero->groupPath = async->groupPath; zero->ticket = 2; zero->generation = 3;
    TfToken const token = HdStGetBasisCurvesGpuGroupDataSourceToken();
    HdDataSourceBaseHandle a = _Control::Make(async), r = _Control::Make(reentry), z = _Control::Make(zero);
    input->AddPrims({{async->groupPath, TfToken("scope"), HdRetainedContainerDataSource::New(1, &token, &a)},
                     {reentry->groupPath, TfToken("scope"), HdRetainedContainerDataSource::New(1, &token, &r)}});
    HdSceneIndexBaseRefPtr base = HdStBasisCurvesGpuGroupStagingSceneIndex::New(input);
    auto* staging = static_cast<HdStBasisCurvesGpuGroupStagingSceneIndex*>(base.operator->());
    _ControlAt(base, async->groupPath)->GetMailbox()->Post(_Ready(async, identity));
    base->SystemMessage(HdSystemMessageTokens->asyncPoll, nullptr);
    CHECK(base->GetPrim(SdfPath("/Async/tile")).primType.IsEmpty());
    base->SystemMessage(HdSystemMessageTokens->asyncAllow, nullptr);
    base->SystemMessage(HdSystemMessageTokens->asyncPoll, nullptr);
    CHECK(base->GetPrim(SdfPath("/Async/tile")).primType == TfToken("basisCurves"));
    HdStBasisCurvesGpuGroupResult zeroResult;
    zeroResult.status = HdStBasisCurvesGpuGroupResultStatus::Ready; zeroResult.groupPath = zero->groupPath;
    zeroResult.ticket = zero->ticket; zeroResult.generation = zero->generation; zeroResult.registryIdentity = identity;
    input->AddPrims({{zero->groupPath, TfToken("scope"),
        HdRetainedContainerDataSource::New(1, &token, &z)}});
    auto zeroMailbox = _ControlAt(base, zero->groupPath)->GetMailbox();
    zeroMailbox->Post(zeroResult); staging->Poll();
    CHECK(base->GetPrim(SdfPath("/Async/tile")).primType.IsEmpty());
    HdStBasisCurvesGpuGroupResult ignored;
    CHECK(!zeroMailbox->TryPop(&ignored));
    _ReentrantObserver observer; observer.input = input.operator->(); observer.base = base; observer.scope = reentry->groupPath;
    base->AddObserver(HdSceneIndexObserverPtr(&observer));
    _ControlAt(base, reentry->groupPath)->GetMailbox()->Post(_Ready(reentry, identity)); staging->Poll();
    CHECK(observer.fired && observer.sawStableTile && observer.sawTransientRemoval);
    CHECK(base->GetPrim(observer.transient).primType.IsEmpty());
    CHECK(base->GetPrim(SdfPath("/Reentry/tile")).primType == TfToken("basisCurves"));
    base->RemoveObserver(HdSceneIndexObserverPtr(&observer));
}

// This regression is expected to expose a dispatch-guard defect until the
// staging implementation resets its guard with exception-safe scope cleanup.
void _TestThrowingObserverRecovery() {
    std::shared_ptr<void const> lease; auto identity = _Identity(&lease); auto input = HdRetainedSceneIndex::New();
    auto first = _Candidate(SdfPath("/Throw"), 1, 1, SdfPath("/Throw/tile"));
    TfToken const token = HdStGetBasisCurvesGpuGroupDataSourceToken(); HdDataSourceBaseHandle data = _Control::Make(first);
    input->AddPrims({{first->groupPath, TfToken("scope"), HdRetainedContainerDataSource::New(1, &token, &data)}});
    HdSceneIndexBaseRefPtr base = HdStBasisCurvesGpuGroupStagingSceneIndex::New(input);
    auto* staging = static_cast<HdStBasisCurvesGpuGroupStagingSceneIndex*>(base.operator->()); _ThrowOnceObserver throwing;
    base->AddObserver(HdSceneIndexObserverPtr(&throwing));
    _ControlAt(base, first->groupPath)->GetMailbox()->Post(_Ready(first, identity));
    bool caught = false; try { staging->Poll(); } catch (int) { caught = true; }
    CHECK(throwing.threw); (void)caught; base->RemoveObserver(HdSceneIndexObserverPtr(&throwing));
    HdContainerDataSourceHandle const firstData = base->GetPrim(SdfPath("/Throw/tile")).dataSource;
    auto replacement = _Candidate(SdfPath("/Throw"), 2, 2, SdfPath("/Throw/tile")); HdDataSourceBaseHandle replacementData = _Control::Make(replacement);
    input->AddPrims({{replacement->groupPath, TfToken("scope"), HdRetainedContainerDataSource::New(1, &token, &replacementData)}});
    _ControlAt(base, replacement->groupPath)->GetMailbox()->Post(_Ready(replacement, identity)); staging->Poll();
    CHECK(base->GetPrim(SdfPath("/Throw/tile")).primType == TfToken("basisCurves"));
    CHECK(base->GetPrim(SdfPath("/Throw/tile")).dataSource != firstData);
    CHECK(_Generation(base, SdfPath("/Throw/tile")) == 2);
}

void _TestObserverMayReleaseLastExternalRef() {
    std::shared_ptr<void const> lease; auto identity = _Identity(&lease); auto input = HdRetainedSceneIndex::New();
    auto candidate = _Candidate(SdfPath("/Release"), 1, 1, SdfPath("/Release/tile"));
    TfToken const token = HdStGetBasisCurvesGpuGroupDataSourceToken(); HdDataSourceBaseHandle data = _Control::Make(candidate);
    input->AddPrims({{candidate->groupPath, TfToken("scope"), HdRetainedContainerDataSource::New(1, &token, &data)}});
    HdSceneIndexBaseRefPtr base = HdStBasisCurvesGpuGroupStagingSceneIndex::New(input);
    auto* staging = static_cast<HdStBasisCurvesGpuGroupStagingSceneIndex*>(base.operator->()); _ReleaseObserver observer;
    observer.owner = &base; base->AddObserver(HdSceneIndexObserverPtr(&observer));
    _ControlAt(base, candidate->groupPath)->GetMailbox()->Post(_Ready(candidate, identity)); staging->Poll();
    CHECK(observer.released && !base);
}
} // namespace
int main() { _TestAsyncZeroAndReentry(); _TestObserverMayReleaseLastExternalRef(); _TestThrowingObserverRecovery(); return failures == 0 ? 0 : 1; }
