// Native controller coverage for Commit-boundary GPU-group admission.
//
// The important retained-state case is deliberate: a rejected replacement
// must not invalidate the already accepted provider while the renderer keeps
// that provider as its last-good tile snapshot.  Only CancelAll(), the
// delegate's terminal teardown boundary, invalidates every such provider.
#include "eglctx.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/range3d.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupController.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hgiGL/hgi.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (false)

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

GfBBox3d
_Bounds()
{
    return GfBBox3d(GfRange3d(GfVec3d(-1.0), GfVec3d(1.0)), GfMatrix4d(1.0));
}

HdStBasisCurvesGpuBundleSharedPtr
_Bundle(uint64_t generation)
{
    auto bundle = std::make_shared<HdStBasisCurvesGpuBundle>();
    bundle->generation = generation;
    bundle->curveType = TfToken("linear");
    bundle->curveBasis = TfToken("linear");
    bundle->curveWrap = TfToken("nonperiodic");
    bundle->bounds = _Bounds();
    bundle->vertexRange = std::make_shared<_Range>();
    for (HdStBasisCurvesGpuTopologyMode const mode : {
             HdStBasisCurvesGpuTopologyMode::Curves,
             HdStBasisCurvesGpuTopologyMode::Hull,
             HdStBasisCurvesGpuTopologyMode::Points}) {
        HdStBasisCurvesGpuTopologyRange slot;
        slot.mode = mode;
        slot.topologyRange = std::make_shared<_Range>();
        slot.drawCountRange = std::make_shared<_Range>();
        bundle->topologyRanges.push_back(std::move(slot));
    }
    bundle->ready = [] { return true; };
    return bundle;
}

class _Provider final : public HdStBasisCurvesGpuDataSource {
public:
    HD_DECLARE_DATASOURCE(_Provider);

    explicit _Provider(HdStBasisCurvesGpuBundleSharedPtr bundle)
        : _bundle(std::move(bundle)) {}

    HdStBasisCurvesGpuBundleSharedPtr Prepare(
        HdStResourceRegistry *, HdStBasisCurvesGpuPrepareRequest const &request) override
    {
        ++prepares;
        rprim = request.rprimId;
        modes = request.topologyModes;
        return _bundle;
    }

    int prepares = 0;
    SdfPath rprim;
    HdStBasisCurvesGpuTopologyModeVector modes;

private:
    HdStBasisCurvesGpuBundleSharedPtr const _bundle;
};

HdStBasisCurvesGpuGroupMember
_Member(uint32_t id, HdStBasisCurvesGpuDataSourceHandle const &provider)
{
    HdStBasisCurvesGpuGroupMember member;
    member.id = id;
    member.rprimPath = SdfPath("/Groom/__usdGenRender/tile_" + std::to_string(id));
    member.presentation.curveType = TfToken("linear");
    member.presentation.curveBasis = TfToken("linear");
    member.presentation.curveWrap = TfToken("nonperiodic");
    member.presentation.bounds = _Bounds();
    member.provider = provider;
    return member;
}

HdStBasisCurvesGpuGroupCandidateSharedPtr
_Candidate(uint64_t ticket, uint64_t generation,
           std::vector<HdStBasisCurvesGpuGroupMember> members)
{
    auto candidate = std::make_shared<HdStBasisCurvesGpuGroupCandidate>();
    candidate->groupPath = SdfPath("/Groom/__usdGenRender");
    candidate->ticket = ticket;
    candidate->generation = generation;
    candidate->members = std::move(members);
    return candidate;
}

bool
_PrepareAccepted(HdStBasisCurvesGpuReadyProviderHandle const &provider,
                 HdStResourceRegistry *registry, SdfPath const &path)
{
    HdStBasisCurvesGpuPrepareRequest request;
    request.rprimId = path;
    request.topologyModes = {
        HdStBasisCurvesGpuTopologyMode::Curves,
        HdStBasisCurvesGpuTopologyMode::Hull,
        HdStBasisCurvesGpuTopologyMode::Points};
    HdStBasisCurvesGpuBundleSharedPtr const bundle = provider->Prepare(registry, request);
    return bundle && bundle->IsCompleteFor(request.topologyModes) && bundle->Ready();
}

bool
_Pop(HdStBasisCurvesGpuGroupMailboxSharedPtr const &mailbox,
     HdStBasisCurvesGpuGroupResult *result)
{
    if (!mailbox->TryPop(result)) {
        std::fprintf(stderr, "mailbox unexpectedly had no result\n");
        ++failures;
        return false;
    }
    return true;
}

void
_TestCommitAndRetention(std::shared_ptr<HdStResourceRegistry> const &registry)
{
    HdStBasisCurvesGpuGroupController controller;
    auto firstA = _Provider::New(_Bundle(1));
    auto firstB = _Provider::New(_Bundle(1));
    HdStBasisCurvesGpuGroupMember hidden = _Member(2, firstB);
    hidden.presentation.visibility = TfToken("invisible");
    auto first = _Candidate(1, 1, {_Member(1, firstA), hidden});
    auto firstMailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();

    // Repeated observation of the same control datasource must not prepare or
    // publish it twice.
    controller.Enqueue(first, firstMailbox);
    controller.Enqueue(first, firstMailbox);
    controller.Update(registry, 77);
    CHECK(firstA->prepares == 1 && firstB->prepares == 1);
    CHECK(firstA->rprim == first->members[0].rprimPath);
    CHECK(firstB->rprim == first->members[1].rprimPath);
    HdStBasisCurvesGpuTopologyModeVector const allModes = {
        HdStBasisCurvesGpuTopologyMode::Curves,
        HdStBasisCurvesGpuTopologyMode::Hull,
        HdStBasisCurvesGpuTopologyMode::Points};
    CHECK(firstA->modes == allModes && firstB->modes == allModes);
    HdStBasisCurvesGpuGroupResult firstResult;
    CHECK(!firstMailbox->TryPop(&firstResult));
    registry->Commit();
    if (!_Pop(firstMailbox, &firstResult)) return;
    CHECK(firstResult.status == HdStBasisCurvesGpuGroupResultStatus::Ready);
    CHECK(firstResult.ticket == 1 && firstResult.generation == 1);
    CHECK(firstResult.providers.size() == 2);
    if (firstResult.providers.empty()) return;
    HdStBasisCurvesGpuReadyProviderHandle const oldProvider =
        firstResult.providers.front();
    CHECK(_PrepareAccepted(oldProvider, registry.get(), first->members[0].rprimPath));

    // A later observation of the unchanged control cannot register a second
    // Commit callback or duplicate its terminal acknowledgement.
    controller.Enqueue(first, firstMailbox);
    controller.Update(registry, 77);
    CHECK(firstA->prepares == 1 && firstB->prepares == 1);
    registry->Commit();
    CHECK(!firstMailbox->TryPop(&firstResult));

    // The newer candidate is admitted and prepared, but cannot be accepted.
    // It must not disturb the old provider retained by the staging filter.
    auto throwingBundle = std::make_shared<HdStBasisCurvesGpuBundle>(*_Bundle(2));
    throwingBundle->ready = []() -> bool { throw 17; };
    auto rejecting = _Provider::New(throwingBundle);
    auto replacement = _Candidate(2, 2, {_Member(1, rejecting)});
    auto replacementMailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();
    controller.Enqueue(replacement, replacementMailbox);
    controller.Update(registry, 77);
    CHECK(rejecting->prepares == 1);
    CHECK(_PrepareAccepted(oldProvider, registry.get(), first->members[0].rprimPath));
    CHECK(!replacementMailbox->TryPop(&firstResult));
    registry->Commit();
    HdStBasisCurvesGpuGroupResult rejected;
    if (!_Pop(replacementMailbox, &rejected)) return;
    CHECK(rejected.status == HdStBasisCurvesGpuGroupResultStatus::Rejected);
    CHECK(rejected.providers.empty());
    CHECK(_PrepareAccepted(oldProvider, registry.get(), first->members[0].rprimPath));

    // Scope removal cancels only controller work.  A previously accepted
    // proxy stays valid until the delegate teardown boundary invalidates its
    // registry identity.
    controller.RemovePrefix(SdfPath("/Groom"));
    controller.Update(registry, 77);
    CHECK(_PrepareAccepted(oldProvider, registry.get(), first->members[0].rprimPath));
    controller.CancelAll();
    CHECK(!_PrepareAccepted(oldProvider, registry.get(), first->members[0].rprimPath));
}

void
_TestZeroMemberCommit(std::shared_ptr<HdStResourceRegistry> const &registry)
{
    HdStBasisCurvesGpuGroupController controller;
    auto candidate = _Candidate(5, 5, {});
    auto mailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();
    controller.Enqueue(candidate, mailbox);
    controller.Update(registry, 79);
    HdStBasisCurvesGpuGroupResult result;
    CHECK(!mailbox->TryPop(&result));
    registry->Commit();
    if (!_Pop(mailbox, &result)) return;
    CHECK(result.status == HdStBasisCurvesGpuGroupResultStatus::Ready);
    CHECK(result.providers.empty());
}

void
_TestPreAdmissionCancellation(std::shared_ptr<HdStResourceRegistry> const &registry)
{
    HdStBasisCurvesGpuGroupController controller;
    auto acceptedProducer = _Provider::New(_Bundle(10));
    auto accepted = _Candidate(10, 10, {_Member(10, acceptedProducer)});
    auto acceptedMailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();
    controller.Enqueue(accepted, acceptedMailbox);
    controller.Update(registry, 88);
    registry->Commit();
    HdStBasisCurvesGpuGroupResult acceptedResult;
    if (!_Pop(acceptedMailbox, &acceptedResult) ||
        acceptedResult.status != HdStBasisCurvesGpuGroupResultStatus::Ready ||
        acceptedResult.providers.empty()) {
        ++failures;
        return;
    }
    HdStBasisCurvesGpuReadyProviderHandle const retained =
        acceptedResult.providers.front();
    CHECK(_PrepareAccepted(retained, registry.get(),
                           accepted->members.front().rprimPath));

    // Registry availability and a nonzero serial are admission conditions;
    // neither failed request may replace/cancel the current group or publish
    // a result at a later Commit boundary.
    auto nullRegistryProvider = _Provider::New(_Bundle(11));
    auto nullRegistry = _Candidate(11, 11, {_Member(11, nullRegistryProvider)});
    auto nullMailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();
    controller.Enqueue(nullRegistry, nullMailbox);
    controller.Update({}, 88);
    CHECK(nullMailbox->IsCanceled());
    CHECK(nullRegistryProvider->prepares == 0);

    auto zeroSerialProvider = _Provider::New(_Bundle(11));
    auto zeroSerial = _Candidate(11, 11, {_Member(11, zeroSerialProvider)});
    auto zeroSerialMailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();
    controller.Enqueue(zeroSerial, zeroSerialMailbox);
    controller.Update(registry, 0);
    CHECK(zeroSerialMailbox->IsCanceled());
    CHECK(zeroSerialProvider->prepares == 0);

    // A stale candidate and a same-ticket observation using a different
    // mailbox are never admitted.  They cannot cancel the current mailbox,
    // replace the last-good proxy, or acquire a post-Commit acknowledgement.
    auto staleProvider = _Provider::New(_Bundle(9));
    auto stale = _Candidate(9, 9, {_Member(9, staleProvider)});
    auto staleMailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();
    controller.Enqueue(stale, staleMailbox);
    auto duplicateProvider = _Provider::New(_Bundle(10));
    auto duplicate = _Candidate(10, 10, {_Member(10, duplicateProvider)});
    auto duplicateMailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();
    controller.Enqueue(duplicate, duplicateMailbox);
    controller.Update(registry, 88);
    CHECK(staleMailbox->IsCanceled());
    CHECK(duplicateMailbox->IsCanceled());
    CHECK(staleProvider->prepares == 0 && duplicateProvider->prepares == 0);
    CHECK(!acceptedMailbox->IsCanceled());
    CHECK(_PrepareAccepted(retained, registry.get(),
                           accepted->members.front().rprimPath));

    registry->Commit();
    HdStBasisCurvesGpuGroupResult ignored;
    CHECK(!nullMailbox->TryPop(&ignored));
    CHECK(!zeroSerialMailbox->TryPop(&ignored));
    CHECK(!staleMailbox->TryPop(&ignored));
    CHECK(!duplicateMailbox->TryPop(&ignored));
    CHECK(!acceptedMailbox->TryPop(&ignored));
    CHECK(_PrepareAccepted(retained, registry.get(),
                           accepted->members.front().rprimPath));
}

void
_TestPendingRemoval(std::shared_ptr<HdStResourceRegistry> const &registry)
{
    HdStBasisCurvesGpuGroupController controller;
    auto provider = _Provider::New(_Bundle(3));
    auto candidate = _Candidate(3, 3, {_Member(3, provider)});
    auto mailbox = std::make_shared<HdStBasisCurvesGpuGroupMailbox>();
    controller.Enqueue(candidate, mailbox);
    controller.Update(registry, 78);
    CHECK(provider->prepares == 1);
    controller.RemovePrefix(SdfPath("/Groom/__usdGenRender"));
    controller.Update(registry, 78);
    CHECK(mailbox->IsCanceled());
    registry->Commit();
    HdStBasisCurvesGpuGroupResult result;
    CHECK(!mailbox->TryPop(&result));
}

} // anonymous namespace

int
main()
{
    TfErrorMark errors;
    if (!eglctx::MakeHeadlessGLContext()) return 77;
    GarchGLApiLoad();
    HgiGL hgi;
    auto registry = std::make_shared<HdStResourceRegistry>(&hgi);
    _TestCommitAndRetention(registry);
    _TestPendingRemoval(registry);
    _TestZeroMemberCommit(registry);
    _TestPreAdmissionCancellation(registry);
    CHECK(errors.IsClean());
    return failures == 0 ? 0 : 1;
}
