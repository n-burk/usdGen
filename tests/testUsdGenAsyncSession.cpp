// testUsdGenAsyncSession — scheduled session ownership and immutable
// publication snapshots.  The held Capture operator makes owner/work
// separation observable without a timing-dependent sleep.

#include "usdGen/opRegistry.h"
#include "usdGen/session.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdGen;

namespace {

int failures = 0;

void Check(bool ok, std::string const &what)
{
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    } else {
        std::printf("ok:   %s\n", what.c_str());
    }
}

struct HoldCaptureState {
    static std::atomic<bool> hold;
    static std::atomic<bool> entered;
    static std::atomic<bool> release;
};
std::atomic<bool> HoldCaptureState::hold{false};
std::atomic<bool> HoldCaptureState::entered{false};
std::atomic<bool> HoldCaptureState::release{true};

class HoldCaptureOp final : public UsdGenOp {
public:
    TfToken Type() const override { return TfToken("UsdGenAsyncHold"); }
    TfSpan<const TfToken> TopologyParameters() const override { return {}; }
    TfSpan<const TfToken> ValueParameters() const override { return {}; }
    bool Bind(UsdGenParamView const &, UsdGenDiagnostics *) override { return true; }
    UsdGenEpoch CaptureDigest(UsdGenCaptureContext const &) const override {
        return {1, 1};
    }
    bool Capture(UsdGenCaptureContext const &, UsdGenCurveBuffer const &,
                 UsdGenCapture *, UsdGenDiagnostics *diagnostics) override {
        if (HoldCaptureState::hold.load(std::memory_order_acquire)) {
            HoldCaptureState::entered.store(true, std::memory_order_release);
            auto const deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(10);
            while (!HoldCaptureState::release.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            if (!HoldCaptureState::release.load(std::memory_order_acquire)) {
                if (diagnostics) diagnostics->Error("async Capture gate timed out");
                return false;
            }
        }
        return true;
    }
    void Evaluate(UsdGenEvalContext const &, UsdGenCapture const &,
                  UsdGenChunkView *) const override {}
};

bool WaitFor(std::atomic<bool> const &value, int milliseconds = 10000)
{
    auto const deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    while (!value.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return value.load(std::memory_order_acquire);
}

UsdGenGraphDesc MakeDesc(float width)
{
    UsdGenGraphDesc d;
    d.description = SdfPath("/async");
    d.terminal = SdfPath("/async/width");

    UsdGenSurfaceDesc surface;
    surface.path = SdfPath("/async/surface");
    surface.id = 0;
    surface.restPoints = VtVec3fArray{
        GfVec3f(0, 0, 0), GfVec3f(1, 0, 0),
        GfVec3f(0, 1, 0), GfVec3f(1, 1, 0)};
    surface.points = surface.restPoints;
    surface.uv = VtVec2fArray{
        GfVec2f(0, 0), GfVec2f(1, 0), GfVec2f(0, 1), GfVec2f(1, 1)};
    surface.faceVertexCounts = VtIntArray{4};
    surface.faceVertexIndices = VtIntArray{0, 1, 3, 2};
    d.surfaces.push_back(std::move(surface));

    UsdGenNodeDesc scatter;
    scatter.path = SdfPath("/async/scatter");
    scatter.type = TfToken("UsdGenScatter");
    scatter.surfaces = {SdfPath("/async/surface")};
    d.nodes.push_back(std::move(scatter));

    UsdGenNodeDesc grow;
    grow.path = SdfPath("/async/grow");
    grow.type = TfToken("UsdGenGrow");
    grow.inputs = {SdfPath("/async/scatter")};
    grow.params.push_back({TfToken("segments"), VtValue(4), false});
    grow.params.push_back({TfToken("length"), VtValue(1.0), false});
    d.nodes.push_back(std::move(grow));

    UsdGenNodeDesc hold;
    hold.path = SdfPath("/async/hold");
    hold.type = TfToken("UsdGenAsyncHold");
    hold.inputs = {SdfPath("/async/grow")};
    d.nodes.push_back(std::move(hold));

    UsdGenNodeDesc widthNode;
    widthNode.path = SdfPath("/async/width");
    widthNode.type = TfToken("UsdGenWidth");
    widthNode.inputs = {SdfPath("/async/hold")};
    widthNode.params.push_back({TfToken("width"), VtValue(width), false});
    d.nodes.push_back(std::move(widthNode));
    return d;
}

} // namespace

int main()
{
    // Exercise cold registry initialization via actual simultaneous compiles,
    // before explicit initialization/custom startup registration can hide it.
    std::atomic<unsigned> coldSuccess{0};
    std::vector<std::thread> coldStarts;
    auto coldDesc = MakeDesc(0.01f);
    coldDesc.nodes.erase(coldDesc.nodes.begin() + 2); // no custom Hold kernel yet
    coldDesc.nodes.back().inputs = {SdfPath("/async/grow")};
    for (unsigned i = 0; i != 8; ++i) {
        coldStarts.emplace_back([&, i] {
            UsdGenSession cold;
            cold.SetGraphDesc(coldDesc);
            auto snapshot = cold.CommitSnapshot(i, UsdGenCommitReason::NoticeBatchEnd);
            if (snapshot && snapshot->generation && !snapshot->diagnostics.HasErrors())
                coldSuccess.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& thread : coldStarts) thread.join();
    Check(coldSuccess.load() == 8, "simultaneous cold sessions read complete built-in registry");
    usdGenRegisterM1Operators();
    UsdGenOpRegistry::Get().Register(TfToken("UsdGenAsyncHold"), 0,
                                      [] { return std::make_unique<HoldCaptureOp>(); });

    // Seed an actual good publication before exercising supersession and
    // failure retention.  The held operator is disabled for this cook.
    HoldCaptureState::hold.store(false);
    HoldCaptureState::release.store(true);
    // Callback storage must outlive session shutdown, including early-failure
    // exits where accepted work may still be awaiting its terminal callback.
    std::atomic<bool> oldCallback{false}, latestCallback{false};
    std::atomic<bool> callbackDone{false}, callbackRejected{false}, followupAccepted{false};
    std::atomic<bool> failureDone{false}, failureOutcome{false};
    UsdGenSession::SnapshotPtr latestSnapshot;
    UsdGenSession session(4);
    UsdGenGraphDesc baselineDesc = MakeDesc(0.02f);
    session.SetGraphDesc(baselineDesc);
    auto baseline = session.CommitSnapshot(0.0, UsdGenCommitReason::NoticeBatchEnd);
    Check(baseline && baseline->generation && baseline->generation->id == 0,
          "initial scheduled snapshot publishes generation zero");
    if (!baseline || !baseline->generation) {
        std::printf("testUsdGenAsyncSession: FAILED (no baseline)\n");
        return 1;
    }
    Check(baseline && baseline->routing && baseline->graphInfo.NodeCount() == 4,
          "initial snapshot pairs routing and graph metadata");
    Check(baseline && baseline->stats.commits == 1,
          "initial snapshot carries paired commit statistics");
    Check(!session.NeedsCommit(), "accepted initial publication clears dirty summary");

    // A worker is blocked in Capture.  Owner commands must still accept a
    // same-path descriptor replacement and a newer commit request.  The old
    // work is superseded; only the newest descriptor may publish.
    HoldCaptureState::hold.store(true, std::memory_order_release);
    HoldCaptureState::entered.store(false, std::memory_order_release);
    HoldCaptureState::release.store(false, std::memory_order_release);
    UsdGenGraphDesc firstEdit = MakeDesc(0.03f);
    UsdGenGraphDesc latestEdit = MakeDesc(0.07f);
    session.SetGraphDesc(firstEdit);
    Check(true, "first edit owner command accepted");
    bool firstSubmitted = session.CommitAsync(1.0, UsdGenCommitReason::NoticeBatchEnd,
        [&](UsdGenSession::SnapshotPtr, UsdGenExecutionPipeline::Outcome outcome) {
            oldCallback.store(outcome == UsdGenExecutionPipeline::Outcome::Superseded,
                              std::memory_order_release);
        });
    Check(firstSubmitted, "blocked edit commit submitted");
    if (!firstSubmitted) {
        HoldCaptureState::release.store(true, std::memory_order_release);
        return 1;
    }
    Check(WaitFor(HoldCaptureState::entered),
          "worker reached bounded Capture gate");
    if (!HoldCaptureState::entered.load(std::memory_order_acquire)) {
        HoldCaptureState::release.store(true, std::memory_order_release);
        return 1;
    }
    // SetGraphDesc is the short owner operation and must complete while the
    // worker remains held in Capture.
    session.SetGraphDesc(latestEdit);
    Check(true, "latest same-path descriptor accepted while worker is blocked");
    bool latestSubmitted = session.CommitAsync(2.0, UsdGenCommitReason::NoticeBatchEnd,
        [&](UsdGenSession::SnapshotPtr snapshot,
            UsdGenExecutionPipeline::Outcome outcome) {
            latestSnapshot = std::move(snapshot);
            latestCallback.store(
                outcome == UsdGenExecutionPipeline::Outcome::Published,
                std::memory_order_release);
        });
    Check(latestSubmitted, "latest commit submitted before releasing old worker");
    if (!latestSubmitted) {
        HoldCaptureState::release.store(true, std::memory_order_release);
        return 1;
    }
    HoldCaptureState::release.store(true, std::memory_order_release);
    Check(WaitFor(latestCallback), "latest worker publishes after supersession");
    if (!latestCallback.load(std::memory_order_acquire) || !latestSnapshot ||
        !latestSnapshot->generation) {
        return 1;
    }
    Check(oldCallback.load(std::memory_order_acquire),
          "older in-flight cook reports superseded");
    Check(latestSnapshot && latestSnapshot->generation &&
              latestSnapshot->generation->id == baseline->generation->id + 1 &&
              latestSnapshot->generation->frame == 2.0,
          "latest descriptor publishes exactly next generation and frame");
    Check(latestSnapshot && latestSnapshot->graphInfo.Desc().nodes.back().params.front().value ==
              VtValue(0.07f),
          "published snapshot contains newest same-path descriptor");
    bool hasWidthGeometry = false;
    for (auto const &tile : latestSnapshot->generation->tiles) {
        for (float value : tile.widths) {
            hasWidthGeometry = hasWidthGeometry ||
                (std::isfinite(value) && std::fabs(value - 0.07f) < 1e-4f);
            if (hasWidthGeometry) break;
        }
        if (hasWidthGeometry) break;
    }
    Check(hasWidthGeometry,
          "latest publication contains evaluated Width geometry");
    Check(latestSnapshot->stats.commits == 2 && !session.NeedsCommit(),
          "superseded work consumes no public commit count or pending dirt");
    Check(baseline->graphInfo.Desc().nodes.back().params.front().value == VtValue(0.02f) &&
          baseline->generation->id == 0,
          "retained initial snapshot is unchanged by replacement");

    // A malformed replacement returns the last good generation paired with
    // diagnostics; it must not replace the published generation.
    auto badDesc = latestEdit;
    badDesc.nodes.back().type = TfToken("UsdGenMissingAsyncKernel");
    session.SetGraphDesc(badDesc);
    auto rejected = session.CommitSnapshot(3.0, UsdGenCommitReason::NoticeBatchEnd);
    Check(rejected && rejected->generation == latestSnapshot->generation,
          "failed cook retains the last good generation");
    Check(rejected && rejected->diagnostics.HasErrors(),
          "failed cook returns paired diagnostics");
    Check(session.Snapshot() && session.Snapshot()->generation == latestSnapshot->generation,
          "failed cook does not publish replacement generation");
    Check(session.NeedsCommit(), "failed current cook retains pending dirty inputs");
    Check(session.CommitAsync(3.5, UsdGenCommitReason::NoticeBatchEnd,
        [&](UsdGenSession::SnapshotPtr snapshot, UsdGenExecutionPipeline::Outcome outcome) {
            failureOutcome.store(outcome == UsdGenExecutionPipeline::Outcome::Failed &&
                snapshot && snapshot->diagnostics.HasErrors() &&
                snapshot->generation == latestSnapshot->generation, std::memory_order_release);
            failureDone.store(true, std::memory_order_release);
        }), "failed graph can be retried asynchronously");
    Check(WaitFor(failureDone) && failureOutcome.load(std::memory_order_acquire),
          "rejected async cook reports Failed and retains last good generation");

    // Concurrent synchronous compatibility calls use independent await
    // boundaries and both complete without sharing an application lock.
    session.SetGraphDesc(latestEdit);
    std::atomic<bool> syncStarted{false};
    UsdGenSession::SnapshotPtr syncA, syncB;
    std::thread a([&] {
        syncStarted.store(true, std::memory_order_release);
        syncA = session.CommitSnapshot(4.0, UsdGenCommitReason::NoticeBatchEnd);
    });
    std::thread b([&] {
        while (!syncStarted.load(std::memory_order_acquire)) std::this_thread::yield();
        syncB = session.CommitSnapshot(5.0, UsdGenCommitReason::NoticeBatchEnd);
    });
    a.join();
    b.join();
    auto paired = [](UsdGenSession::SnapshotPtr const &snapshot) {
        return snapshot && snapshot->generation &&
            snapshot->stats.commits ==
                static_cast<uint64_t>(snapshot->generation->id + 1);
    };
    Check(paired(syncA) && paired(syncB),
          "concurrent synchronous CommitSnapshot calls return paired snapshots");
    auto beforeDefinitive = session.Snapshot();
    auto definitive = session.CommitSnapshot(5.5, UsdGenCommitReason::NoticeBatchEnd);
    Check(paired(definitive) && beforeDefinitive && beforeDefinitive->generation &&
              definitive->generation &&
              definitive->generation->id == beforeDefinitive->generation->id + 1,
          "a definitive synchronous commit advances exactly one generation");

    // Callback re-entry may enqueue asynchronous work, but a synchronous wait
    // from pipeline execution is rejected rather than self-deadlocking.
    session.CommitAsync(6.0, UsdGenCommitReason::NoticeBatchEnd,
        [&](UsdGenSession::SnapshotPtr,
            UsdGenExecutionPipeline::Outcome outcome) {
            if (outcome == UsdGenExecutionPipeline::Outcome::Published) {
                followupAccepted.store(
                    session.CommitAsync(7.0, UsdGenCommitReason::NoticeBatchEnd),
                    std::memory_order_release);
                try {
                    (void)session.CommitSnapshot(8.0, UsdGenCommitReason::NoticeBatchEnd);
                } catch (std::logic_error const &) {
                    callbackRejected.store(true, std::memory_order_release);
                }
            }
            callbackDone.store(true, std::memory_order_release);
        });
    Check(WaitFor(callbackDone), "commit callback completes");
    Check(followupAccepted.load(std::memory_order_acquire),
          "callback can enqueue asynchronous follow-up");
    Check(callbackRejected.load(std::memory_order_acquire),
          "synchronous CommitSnapshot from callback is rejected");

    // Destruction drains accepted work and gives its callback a terminal
    // outcome; this scope intentionally ends without an explicit Drain().
    std::atomic<unsigned> terminalCallbacks{0};
    UsdGenSession::SnapshotPtr retainedAfterShutdown;
    {
        UsdGenSession closing(2);
        closing.SetGraphDesc(MakeDesc(0.01f));
        retainedAfterShutdown = closing.CommitSnapshot(0.0, UsdGenCommitReason::NoticeBatchEnd);
        for (unsigned i = 0; i != 32; ++i) {
            Check(closing.CommitAsync(i + 1.0, UsdGenCommitReason::NoticeBatchEnd,
                [&](UsdGenSession::SnapshotPtr, UsdGenExecutionPipeline::Outcome) {
                    terminalCallbacks.fetch_add(1, std::memory_order_release);
                }), "closing session accepts asynchronous commit");
        }
    }
    Check(terminalCallbacks.load(std::memory_order_acquire) == 32,
          "session destruction completes every accepted callback exactly once");
    Check(retainedAfterShutdown && retainedAfterShutdown->generation &&
          retainedAfterShutdown->generation->id == 0 && retainedAfterShutdown->routing &&
          retainedAfterShutdown->graphInfo.NodeCount() == 4,
          "immutable geometry and routing survive session destruction");

    std::printf("testUsdGenAsyncSession: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
