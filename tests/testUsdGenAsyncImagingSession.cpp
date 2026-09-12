// testUsdGenAsyncImagingSession -- imaging-owner request pairing, callback
// re-entry, retirement, and terminal shutdown behavior.

#include "usdGen/opRegistry.h"
#include "usdGenImaging/usdGenImagingSession.h"
#include "usdGenImaging/groomSceneIndexPlugin.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"
#include "pxr/pxr.h"
#include "pxr/imaging/hd/retainedDataSource.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdGen;
using namespace usdGenImaging;

namespace {

int failures = 0;

void Check(bool value, std::string const &what)
{
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    } else {
        std::printf("ok:   %s\n", what.c_str());
    }
}

template <class Predicate>
bool WaitFor(Predicate predicate, int milliseconds = 10000)
{
    auto const deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return predicate();
}

UsdGenGraphDesc MakeDesc(float width)
{
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/asyncImaging");
    desc.terminal = SdfPath("/asyncImaging/width");

    UsdGenSurfaceDesc surface;
    surface.path = SdfPath("/asyncImaging/surface");
    surface.id = 0;
    surface.restPoints = VtVec3fArray{
        GfVec3f(0, 0, 0), GfVec3f(1, 0, 0),
        GfVec3f(0, 1, 0), GfVec3f(1, 1, 0)};
    surface.points = surface.restPoints;
    surface.uv = VtVec2fArray{
        GfVec2f(0, 0), GfVec2f(1, 0),
        GfVec2f(0, 1), GfVec2f(1, 1)};
    surface.faceVertexCounts = VtIntArray{4};
    surface.faceVertexIndices = VtIntArray{0, 1, 3, 2};
    desc.surfaces.push_back(std::move(surface));

    UsdGenNodeDesc scatter;
    scatter.path = SdfPath("/asyncImaging/scatter");
    scatter.type = TfToken("UsdGenScatter");
    scatter.surfaces = {SdfPath("/asyncImaging/surface")};
    desc.nodes.push_back(std::move(scatter));

    UsdGenNodeDesc grow;
    grow.path = SdfPath("/asyncImaging/grow");
    grow.type = TfToken("UsdGenGrow");
    grow.inputs = {SdfPath("/asyncImaging/scatter")};
    grow.params.push_back({TfToken("segments"), VtValue(4), false});
    grow.params.push_back({TfToken("length"), VtValue(1.0), false});
    desc.nodes.push_back(std::move(grow));

    UsdGenNodeDesc widthNode;
    widthNode.path = SdfPath("/asyncImaging/width");
    widthNode.type = TfToken("UsdGenWidth");
    widthNode.inputs = {SdfPath("/asyncImaging/grow")};
    widthNode.params.push_back({TfToken("width"), VtValue(width), false});
    desc.nodes.push_back(std::move(widthNode));
    return desc;
}

UsdGenSessionHandle NewSession()
{
    UsdGenSessionKey key;
    key.groomRoot = SdfPath("/asyncImaging");
    key.renderInstanceId = 991;
    return TfCreateRefPtr(new UsdGenImagingSession(
        key, std::make_shared<UsdGenSession>(2)));
}

UsdGenImagingSession::CommitRequest Request(
    std::shared_ptr<const UsdGenGraphDesc> desc, double frame)
{
    UsdGenImagingSession::CommitRequest request;
    request.reason = UsdGenCommitReason::NoticeBatchEnd;
    request.frame = frame;
    request.desc = std::move(desc);
    request.context = UsdGenContext::Interactive;
    return request;
}

bool HasWidth(UsdGenImagingSession::CommitPayload const &payload,
             double frame, float width)
{
    if (!payload.published || !payload.generation ||
        payload.generation->frame != frame)
        return false;
    bool found = false;
    for (auto const &tile : payload.generation->tiles) {
        for (float value : tile.widths) {
            if (!std::isfinite(value) || std::fabs(value - width) > 1e-4f)
                return false;
            found = true;
        }
    }
    return found;
}

std::atomic<unsigned> batchEntered{0};
std::atomic<bool> batchRelease{false}, batchTimedOut{false};
class BatchHoldOp final : public UsdGenOp {
public:
    TfToken Type() const override { return TfToken("UsdGenImagingBatchHold"); }
    TfSpan<const TfToken> TopologyParameters() const override { return {}; }
    TfSpan<const TfToken> ValueParameters() const override { return {}; }
    bool Bind(UsdGenParamView const&, UsdGenDiagnostics*) override { return true; }
    UsdGenEpoch CaptureDigest(UsdGenCaptureContext const&) const override { return {1, 1}; }
    bool Capture(UsdGenCaptureContext const&, UsdGenCurveBuffer const&,
                 UsdGenCapture*, UsdGenDiagnostics* diagnostics) override {
        batchEntered.fetch_add(1, std::memory_order_release);
        if (WaitFor([] { return batchRelease.load(std::memory_order_acquire); })) return true;
        batchTimedOut.store(true);
        diagnostics->Error("store batch capture gate timed out");
        return false;
    }
    void Evaluate(UsdGenEvalContext const&, UsdGenCapture const&, UsdGenChunkView*) const override {}
};

void TestPairedConcurrentRequests()
{
    auto session = NewSession();
    auto descA = std::make_shared<const UsdGenGraphDesc>(MakeDesc(.031f));
    auto descB = std::make_shared<const UsdGenGraphDesc>(MakeDesc(.047f));
    std::atomic<unsigned> callbacks{0};
    std::atomic<unsigned> publications{0};
    std::atomic<unsigned> mismatches{0};
    std::atomic<unsigned> accepted{0};

    auto submit = [&](std::shared_ptr<const UsdGenGraphDesc> desc,
                      double frame, float width) {
        auto request = Request(std::move(desc), frame);
        if (session->CommitAsync(std::move(request),
                [&, frame, width](UsdGenImagingSession::CommitPayload const &payload,
                                  UsdGenExecutionPipeline::Outcome outcome) {
                    if (outcome == UsdGenExecutionPipeline::Outcome::Published) {
                        publications.fetch_add(1, std::memory_order_relaxed);
                        if (!HasWidth(payload, frame, width))
                            mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                    callbacks.fetch_add(1, std::memory_order_release);
                }))
            accepted.fetch_add(1, std::memory_order_relaxed);
    };

    usdGenRegisterM1Operators();
    std::thread a(submit, descA, 10.0, .031f);
    std::thread b(submit, descB, 11.0, .047f);
    a.join();
    b.join();

    Check(WaitFor([&] { return callbacks.load(std::memory_order_acquire) ==
                               accepted.load(std::memory_order_acquire); }),
          "concurrent imaging requests reach one terminal callback each");
    Check(accepted.load() == 2, "both concurrent imaging requests are accepted");
    Check(publications.load() > 0 && publications.load() <= accepted.load() && mismatches.load() == 0,
          "published imaging payload keeps descriptor geometry paired with frame");
    session->Shutdown();
    session.Reset();
    UsdGenImagingSession::DrainRetired();
}

void TestCallbackUnregisterAndReentry()
{
    auto session = NewSession();
    auto desc = std::make_shared<const UsdGenGraphDesc>(MakeDesc(.055f));
    auto token = std::make_shared<std::atomic<int>>(-1);
    std::atomic<unsigned> callbackCount{0};
    std::atomic<bool> followupAccepted{false};
    std::atomic<bool> followupDone{false};
    std::atomic<bool> synchronousRejected{false};

    // The callback intentionally owns a handle while it is registered.  It
    // unregisters before queuing the follow-up, so the follow-up cannot call
    // this callback after the owner processes those commands in order.
    UsdGenSessionHandle callbackOwner = session;
    int callbackToken = session->RegisterRepublishCallback(
        [callbackOwner, token, desc, &callbackCount, &followupAccepted,
         &followupDone, &synchronousRejected](
            UsdGenImagingSession::CommitPayload const &payload) mutable {
            if (!payload.published) return;
            unsigned const call = callbackCount.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (call != 1) return;
            callbackOwner->UnregisterRepublishCallback(token->load());
            UsdGenImagingSession::CommitRequest followup =
                Request(desc, 2.0);
            followupAccepted.store(callbackOwner->CommitAsync(
                std::move(followup),
                [&followupDone](UsdGenImagingSession::CommitPayload const &,
                                UsdGenExecutionPipeline::Outcome) {
                    followupDone.store(true, std::memory_order_release);
                }), std::memory_order_release);
            try {
                callbackOwner->Shutdown();
            } catch (std::logic_error const &) {
                synchronousRejected.store(true, std::memory_order_release);
            }
        });
    token->store(callbackToken, std::memory_order_release);
    Check(callbackToken >= 0, "republish callback registration returns a token");

    Check(session->CommitAsync(Request(desc, 1.0)),
          "initial imaging request for callback test is accepted");
    Check(WaitFor([&] { return followupDone.load(std::memory_order_acquire); }),
          "callback-enqueued follow-up reaches a terminal completion");
    Check(callbackCount.load(std::memory_order_acquire) == 1,
          "owner-ordered unregister prevents callback on follow-up publication");
    Check(followupAccepted.load(std::memory_order_acquire),
          "republish callback can enqueue asynchronous follow-up");
    Check(synchronousRejected.load(std::memory_order_acquire),
          "synchronous shutdown from callback is rejected");

    session->Shutdown();
    session.Reset();
    callbackOwner.Reset();
    UsdGenImagingSession::DrainRetired();
}

void TestLastHandleReleaseInCompletion()
{
    UsdGenSessionHandle session = NewSession();
    auto weakSession = TfCreateWeakPtr(session.operator->());
    UsdGenSessionHandle completionOwner = session;
    session.Reset();
    std::atomic<bool> completed{false};
    std::atomic<bool> published{false};
    std::shared_ptr<const UsdGenGeneration> retainedGeneration;
    std::shared_ptr<const UsdGenGraphRoutingSnapshot> retainedRouting;

    // Keep a separate caller handle while moving the final handle into the
    // completion; argument evaluation order must not make the call through
    // an already-moved TfRefPtr.
    UsdGenSessionHandle submitter = completionOwner;
    bool accepted = submitter->CommitAsync(
        Request(std::make_shared<const UsdGenGraphDesc>(MakeDesc(.065f)), 3.0),
        [completionOwner = std::move(completionOwner), &completed, &published,
         &retainedGeneration, &retainedRouting](
            UsdGenImagingSession::CommitPayload const &payload,
            UsdGenExecutionPipeline::Outcome outcome) mutable {
            retainedGeneration = payload.generation;
            retainedRouting = payload.routing;
            published.store(outcome == UsdGenExecutionPipeline::Outcome::Published &&
                                payload.published,
                            std::memory_order_release);
            // This is deliberately the last TfRefPtr release.  The imaging
            // state must retire asynchronously instead of deleting itself on
            // the owner callback stack.
            completionOwner.Reset();
            completed.store(true, std::memory_order_release);
        });
    submitter.Reset();
    Check(accepted, "last-handle completion request is accepted");
    Check(WaitFor([&] { return completed.load(std::memory_order_acquire); }),
          "last-handle completion runs");
    Check(published.load(std::memory_order_acquire) && retainedGeneration &&
              retainedRouting,
          "completion retains immutable geometry and routing payloads");
    Check(WaitFor([&] { return !weakSession; }),
          "all callback-owned public handles are released before retirement drain");
    UsdGenImagingSession::DrainRetired();
}

void TestShutdownCompletesAcceptedQueue()
{
    auto session = NewSession();
    auto desc = std::make_shared<const UsdGenGraphDesc>(MakeDesc(.075f));
    std::atomic<bool> baselineDone{false};
    std::shared_ptr<const UsdGenGeneration> retainedGeneration;
    std::shared_ptr<const UsdGenGraphRoutingSnapshot> retainedRouting;
    Check(session->CommitAsync(Request(desc, 4.0),
        [&](UsdGenImagingSession::CommitPayload const &payload,
            UsdGenExecutionPipeline::Outcome outcome) {
            if (outcome == UsdGenExecutionPipeline::Outcome::Published) {
                retainedGeneration = payload.generation;
                retainedRouting = payload.routing;
            }
            baselineDone.store(true, std::memory_order_release);
        }), "shutdown test baseline is accepted");
    Check(WaitFor([&] { return baselineDone.load(std::memory_order_acquire); }),
          "shutdown test baseline completes");

    constexpr unsigned requestCount = 48;
    std::vector<std::atomic<unsigned>> perRequest(requestCount);
    for (auto &count : perRequest) count.store(0);
    unsigned accepted = 0;
    for (unsigned i = 0; i != requestCount; ++i) {
        if (session->CommitAsync(
                Request(desc, 5.0 + static_cast<double>(i)),
                [&, i](UsdGenImagingSession::CommitPayload const &,
                        UsdGenExecutionPipeline::Outcome) {
                    perRequest[i].fetch_add(1, std::memory_order_relaxed);
                }))
            ++accepted;
    }
    session->Shutdown();
    unsigned terminal = 0;
    bool duplicate = false;
    for (auto const &count : perRequest) {
        unsigned const value = count.load(std::memory_order_acquire);
        terminal += value;
        duplicate = duplicate || value > 1;
    }
    Check(accepted == requestCount, "shutdown queue accepts all bounded requests");
    Check(terminal == accepted && !duplicate,
          "shutdown gives every accepted request exactly one terminal callback");
    Check(retainedGeneration && retainedRouting,
          "shutdown preserves retained immutable geometry and routing");
    session.Reset();
    UsdGenImagingSession::DrainRetired();
}

class AdoptionInput final : public HdSceneIndexBase {
public:
    SdfPath const root{"/adoptionRace"};
    std::atomic<bool> present{true}, entered{false}, release{false}, timedOut{false};
    mutable std::atomic<unsigned> reads{0};
    HdSceneIndexPrim GetPrim(SdfPath const& path) const override {
        if (path != root) return {};
        const bool exists = present.load();
        // First read discovers the root; second reads private candidate data
        // after the adoption ticket has been reserved.
        if (reads.fetch_add(1) == 1) {
            auto* self = const_cast<AdoptionInput*>(this);
            self->entered.store(true, std::memory_order_release);
            if (!WaitFor([&] { return release.load(std::memory_order_acquire); }))
                self->timedOut.store(true);
        }
        return exists ? HdSceneIndexPrim{TfToken("UsdGenGroom"), HdRetainedContainerDataSource::New()}
                      : HdSceneIndexPrim{};
    }
    SdfPathVector GetChildPrimPaths(SdfPath const& path) const override {
        return path == SdfPath::AbsoluteRootPath() && present.load()
            ? SdfPathVector{root} : SdfPathVector{};
    }
    void Remove() { present.store(false); _SendPrimsRemoved({{root}}); }
    void Add() { present.store(true); _SendPrimsAdded({{root, TfToken("UsdGenGroom")}}); }
};

void TestCancelledAdoptionCannotReplaceNewAttachment()
{
    auto input = TfCreateRefPtr(new AdoptionInput);
    auto index = UsdGenGroomSceneIndex::New(input, 7321);
    auto& store = UsdGenSessionStore::GetInstance();
    UsdGenSessionKey key{"", input->root, 7321};
    std::thread discover([&] { index->GetChildPrimPaths(SdfPath::AbsoluteRootPath()); });
    const bool entered = WaitFor([&] { return input->entered.load(std::memory_order_acquire); });
    Check(entered, "candidate adoption reaches bounded input gate");
    if (!entered) {
        input->release.store(true);
        discover.join();
        return;
    }
    input->Remove();
    Check(!input->timedOut.load(), "removal is accepted while candidate construction is held");
    input->Add();
    auto replacement = store.Find(key);
    Check(replacement && replacement->AttachedIndices() == 1,
          "same-path replacement installs a fresh adoption ticket");
    input->release.store(true, std::memory_order_release);
    discover.join();
    Check(replacement && store.Find(key) == replacement && replacement->AttachedIndices() == 1,
          "late cancelled candidate cannot replace or leak an attachment");
    input->Remove();
    Check(!store.Find(key), "replacement detach removes the final store attachment");
    index.Reset();
    replacement.Reset();
    UsdGenImagingSession::DrainRetired();
}

void TestStoreDispatchesIndependentSessionsTogether()
{
    auto& store = UsdGenSessionStore::GetInstance();
    UsdGenSessionKey keyA{"", SdfPath("/asyncImaging"), 8301};
    UsdGenSessionKey keyB{"", SdfPath("/asyncImaging"), 8302};
    auto a = store.Attach(keyA), b = store.Attach(keyB);
    auto desc = MakeDesc(.03f);
    UsdGenNodeDesc hold;
    hold.path = SdfPath("/asyncImaging/hold");
    hold.type = TfToken("UsdGenImagingBatchHold");
    hold.inputs = {desc.terminal};
    desc.nodes.push_back(hold);
    desc.terminal = hold.path;
    a->StageDesc(desc);
    b->StageDesc(desc);
    std::exception_ptr failure;
    std::thread caller([&] {
        try { store.SetTime(42.0); }
        catch (...) { failure = std::current_exception(); }
    });
    const bool overlap = WaitFor([] { return batchEntered.load(std::memory_order_acquire) == 2; });
    Check(overlap && !batchTimedOut.load(),
          "store frame update dispatches both descriptions before either capture is released");
    batchRelease.store(true, std::memory_order_release);
    caller.join();
    Check(!failure && a->LatestGeneration() && b->LatestGeneration() &&
          a->LatestGeneration()->frame == 42.0 && b->LatestGeneration()->frame == 42.0,
          "parallel store batch waits for both exact-frame publications");
    store.Detach(keyA);
    store.Detach(keyB);
    a.Reset();
    b.Reset();
    UsdGenImagingSession::DrainRetired();
}

} // namespace

int main()
{
    UsdGenOpRegistry::Get().Register(TfToken("UsdGenImagingBatchHold"), 0,
        [] { return std::make_unique<BatchHoldOp>(); });
    TestPairedConcurrentRequests();
    TestCallbackUnregisterAndReentry();
    TestLastHandleReleaseInCompletion();
    TestShutdownCompletesAcceptedQueue();
    TestCancelledAdoptionCannotReplaceNewAttachment();
    TestStoreDispatchesIndependentSessionsTogether();
    std::printf("testUsdGenAsyncImagingSession: %s\n",
                failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
