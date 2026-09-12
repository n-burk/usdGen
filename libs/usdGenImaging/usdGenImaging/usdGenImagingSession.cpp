// usdGen imaging — session store implementation (06-imaging.md §3.7, §3.9).
//
// The wiring contract (06 §3.9): the renderer-level UsdGenGroomSceneIndex
// attaches a session in its constructor, routes observer notices through
// UsdGenDirtyRouter into UsdGenPendingDirty, and commits on triggers
// (a)/(b)/(c). GetPrim paths never commit — they atomic_load the latest
// generation (I7). Superseded commits leave the session dirty (03 §5.6).
#include "usdGenImaging/usdGenImagingSession.h"

#include "usdGen/generationStore.h"
#include "usdGen/session.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/warning.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <limits>
#include <stdexcept>
#include <tbb/flow_graph.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

namespace {
using Pipeline = usdGen::UsdGenExecutionPipeline;
usdGen::UsdGenExecutionRuntime& ImagingRuntime() {
    static usdGen::UsdGenExecutionRuntime runtime(8);
    return runtime;
}

// Public TfRefPtr handles may disappear during a notice callback. Retirement
// is a separate framework graph, never an inline wait/destructor in that
// callback. Its nodes own the retired state until all relayed replies finish.
class Retirements {
    tbb::flow::graph graph;
    tbb::flow::function_node<std::function<void()>> cleanup;
public:
    Retirements() : cleanup(graph, tbb::flow::unlimited,
        [](std::function<void()> action) { action(); return tbb::flow::continue_msg{}; }) {}
    ~Retirements() { graph.wait_for_all(); }
    void Post(std::function<void()> action) { cleanup.try_put(std::move(action)); }
    void Drain() {
        if (Pipeline::IsExecuting()) throw std::logic_error("callback cannot drain retired imaging states");
        graph.wait_for_all();
    }
};
Retirements& RetirementQueue() { static Retirements queue; return queue; }
}

struct UsdGenImagingSession::State {
    std::shared_ptr<usdGen::UsdGenSession> engine;
    std::atomic<bool> accepting{true}, stopped{false}, closeClaimed{false};
    std::atomic<bool> appDriver{false}, needsDesc{false};
    std::atomic<int> attached{0};
    std::atomic<int64_t> generation{-1};
    std::atomic<uint64_t> nextToken{0};
    // Everything below is imaging-command-owner-only.
    double frame;
    usdGen::UsdGenContext context;
    std::shared_ptr<const usdGen::UsdGenGraphDesc> staged;
    std::vector<std::pair<int, std::function<void(CommitPayload const&)>>> callbacks;
    unsigned outstanding = 0;
    bool closing = false;
    std::function<void()> closeDone;
    Pipeline owner;

    State(std::shared_ptr<usdGen::UsdGenSession> e, double f, usdGen::UsdGenContext c)
        : engine(std::move(e)), frame(f), context(c), owner(ImagingRuntime()) {}

    void CompleteClose() {
        if (closing && outstanding == 0 && closeDone) {
            auto done = std::move(closeDone);
            done();
        }
    }
    void Close() {
        if (Pipeline::IsExecuting()) throw std::logic_error("callback cannot shut down imaging synchronously");
        if (stopped.load()) return;
        // Lifecycle admission, not a waiting lock: only one external boundary
        // may drain a graph. Reject competing close callers immediately.
        if (closeClaimed.exchange(true))
            throw std::logic_error("concurrent imaging shutdown is not supported");
        accepting.store(false, std::memory_order_release);
        owner.Await([this](std::function<void()> done) {
            if (!owner.PostCommand([this, done] {
                closing = true;
                closeDone = done;
                CompleteClose();
            })) throw std::runtime_error("imaging close command rejected");
        });
        owner.Shutdown();
        stopped.store(true, std::memory_order_release);
    }

    void Finish(usdGen::UsdGenSession::SnapshotPtr snapshot,
                Pipeline::Outcome outcome, Completion const& done) {
        CommitPayload payload;
        if (!accepting.load()) outcome = Pipeline::Outcome::Superseded;
        if (snapshot) {
            payload.generation = snapshot->generation;
            payload.report = snapshot->report;
            payload.routing = snapshot->routing;
            payload.diagnostics = snapshot->diagnostics;
            payload.stats = snapshot->stats;
        }
        if (outcome == Pipeline::Outcome::Published && payload.generation &&
            payload.generation->id > generation.load()) {
            payload.published = true;
            generation.store(payload.generation->id);
            for (auto const& warning : payload.diagnostics.warnings)
                TF_WARN("usdGen commit: %s", warning.c_str());
            // All mutations enqueue commands; callbacks cannot modify this
            // collection recursively while this publication is being sent.
            for (auto const& entry : callbacks) {
                if (!accepting.load()) break;
                try { entry.second(payload); }
                catch (...) { TF_WARN("usdGen republish callback threw"); }
            }
        } else if (outcome == Pipeline::Outcome::Failed) {
            for (auto const& error : payload.diagnostics.errors)
                TF_WARN("usdGen commit rejected: %s", error.c_str());
        }
        if (done) {
            try { done(payload, outcome); }
            catch (...) { TF_WARN("usdGen imaging completion threw"); }
        }
        --outstanding;
        CompleteClose();
    }

    void Start(CommitRequest request, int callerDevice, Completion done) {
        if (!accepting.load() || !engine) {
            if (done) done({}, Pipeline::Outcome::Superseded);
            return;
        }
        if (request.frame) frame = *request.frame;
        if (request.context) context = *request.context;
        if (request.desc) staged = std::move(request.desc);
        usdGen::UsdGenSession::CommitRequest input;
        input.frame = frame;
        input.context = context;
        input.reason = request.reason;
        input.desc = std::move(staged);
        input.callerDevice = callerDevice;
        ++outstanding;
        try {
            if (engine->CommitAsync(std::move(input),
                [this, done](usdGen::UsdGenSession::SnapshotPtr snapshot, Pipeline::Outcome outcome) {
                    // Close leaves the owner alive until every relay arrives.
                    // A framework dispatch failure is fatal: swallowing it
                    // would strand the outstanding reply and hang retirement.
                    try {
                        if (!owner.PostCommand([this, snapshot, outcome, done] { Finish(snapshot, outcome, done); }))
                            std::terminate();
                    } catch (...) { std::terminate(); }
                })) return;
        } catch (...) {
            Finish({}, Pipeline::Outcome::Failed, done);
            return;
        }
        Finish({}, Pipeline::Outcome::Superseded, done);
    }
};

UsdGenImagingSession::UsdGenImagingSession(UsdGenSessionKey const& key,
    std::shared_ptr<usdGen::UsdGenSession> engine, double frame, usdGen::UsdGenContext context)
    : _key(key), _engine(std::move(engine)), _state(new State(_engine, frame, context)) {
    (void)RetirementQueue();
}
UsdGenImagingSession::~UsdGenImagingSession() {
    State* state = _state.release();
    state->accepting.store(false, std::memory_order_release);
    RetirementQueue().Post([state] { state->Close(); delete state; });
}
void UsdGenImagingSession::Shutdown() { _state->Close(); }
void UsdGenImagingSession::DrainRetired() { RetirementQueue().Drain(); }
void UsdGenImagingSession::SetTime(double frame) {
    auto* state = _state.get();
    state->owner.PostCommand([state, frame] { state->frame = frame; });
}
void UsdGenImagingSession::SetContext(usdGen::UsdGenContext context) {
    auto* state = _state.get();
    state->owner.PostCommand([state, context] { state->context = context; });
}
void UsdGenImagingSession::StageDesc(usdGen::UsdGenGraphDesc const& desc) {
    auto* state = _state.get();
    auto value = std::make_shared<const usdGen::UsdGenGraphDesc>(desc);
    state->owner.PostCommand([state, value] { state->staged = value; });
}
bool UsdGenImagingSession::CommitAsync(CommitRequest request, Completion done) {
    auto* state = _state.get();
    if (!state->accepting.load()) return false;
    const int device = usdGen::UsdGenSession::CaptureCallerDevice();
    return state->owner.PostCommand([state, request=std::move(request), device, done] {
        state->Start(request, device, done);
    }, [done] { if (done) done({}, Pipeline::Outcome::Superseded); });
}
void UsdGenImagingSession::_Commit(CommitRequest request) {
    _state->owner.Await([this, request=std::move(request)](std::function<void()> release) mutable {
        if (!CommitAsync(std::move(request), [release](CommitPayload const&, Pipeline::Outcome) {
            release();
        })) throw std::runtime_error("imaging session rejected commit");
    });
}
void UsdGenImagingSession::Commit(usdGen::UsdGenCommitReason reason) {
    CommitRequest request; request.reason = reason; _Commit(std::move(request));
}
void UsdGenImagingSession::CommitAtTime(double frame, usdGen::UsdGenCommitReason reason) {
    CommitRequest request; request.reason = reason; request.frame = frame; _Commit(std::move(request));
}
void UsdGenImagingSession::StageAndCommit(usdGen::UsdGenGraphDesc const& desc, usdGen::UsdGenCommitReason reason) {
    CommitRequest request; request.reason = reason;
    request.desc = std::make_shared<const usdGen::UsdGenGraphDesc>(desc);
    _Commit(std::move(request));
}
int UsdGenImagingSession::RegisterRepublishCallback(std::function<void(CommitPayload const&)> callback) {
    auto* state = _state.get();
    const auto token = state->nextToken.fetch_add(1);
    if (token > static_cast<uint64_t>(std::numeric_limits<int>::max()) || !state->accepting.load()) return -1;
    if (!state->owner.PostCommand([state, token, callback=std::move(callback)] {
        state->callbacks.emplace_back(static_cast<int>(token), callback);
    })) return -1;
    return static_cast<int>(token);
}
void UsdGenImagingSession::UnregisterRepublishCallback(int token) {
    auto* state = _state.get();
    state->owner.PostCommand([state, token] {
        state->callbacks.erase(std::remove_if(state->callbacks.begin(), state->callbacks.end(),
            [token](auto const& entry) { return entry.first == token; }), state->callbacks.end());
    });
}
int64_t UsdGenImagingSession::Generation() const noexcept { return _state->generation.load(); }
usdGen::UsdGenGenerationConstPtr UsdGenImagingSession::LatestGeneration() const noexcept {
    return _engine ? _engine->Generation() : usdGen::UsdGenGenerationConstPtr();
}
int UsdGenImagingSession::AttachedIndices() const noexcept { return _state->attached.load(); }
void UsdGenImagingSession::NoteAttach() { _state->attached.fetch_add(1); }
void UsdGenImagingSession::NoteDetach() { _state->attached.fetch_sub(1); }
void UsdGenImagingSession::MarkAppDriver() noexcept { _state->appDriver.store(true); }
bool UsdGenImagingSession::HasAppDriver() const noexcept { return _state->appDriver.load(); }
void UsdGenImagingSession::MarkNeedsDesc() noexcept { _state->needsDesc.store(true); }
bool UsdGenImagingSession::NeedsDesc() const noexcept { return _state->needsDesc.load(); }
bool UsdGenImagingSession::ConsumeNeedsDesc() noexcept { return _state->needsDesc.exchange(false); }

// ---------------------------------------------------------------------------

UsdGenSessionStore &UsdGenSessionStore::GetInstance()
{
    // The store can own the last public handles until process teardown.
    // Construct retirement first so its graph outlives the store destructor.
    (void)RetirementQueue();
    static UsdGenSessionStore instance;
    return instance;
}

UsdGenSessionHandle UsdGenSessionStore::Attach(UsdGenSessionKey const &key)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessions.find(key);
    if (it != _sessions.end()) {
        it->second->NoteAttach();
        return it->second;
    }
    auto session = TfCreateRefPtr(new UsdGenImagingSession(
        key, std::make_shared<usdGen::UsdGenSession>()));
    _sessions.emplace(key, session);
    session->NoteAttach();
    return session;
}

void UsdGenSessionStore::Detach(UsdGenSessionKey const &key)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessions.find(key);
    if (it == _sessions.end()) return;
    it->second->NoteDetach();
    if (it->second->AttachedIndices() <= 0) {
        _sessions.erase(it);  // last index gone: store drops its strong ref
    }
}

UsdGenSessionHandle UsdGenSessionStore::Find(UsdGenSessionKey const &key) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessions.find(key);
    if (it != _sessions.end()) return it->second;
    return UsdGenSessionHandle();
}

std::vector<UsdGenImagingSessionRefPtr> UsdGenSessionStore::LiveSessions() const
{
    std::vector<UsdGenImagingSessionRefPtr> out;
    std::lock_guard<std::mutex> lock(_mutex);
    out.reserve(_sessions.size());
    for (auto &entry : _sessions) {
        out.push_back(entry.second);
    }
    return out;
}

namespace {
void CommitBatch(std::vector<UsdGenSessionHandle> const& sessions,
                 usdGen::UsdGenCommitReason reason, std::optional<double> frame = {}) {
    if (Pipeline::IsExecuting()) throw std::logic_error("callback must submit asynchronous imaging requests");
    if (sessions.empty()) return;
    Pipeline boundary(ImagingRuntime());
    struct Batch {
        std::atomic<size_t> remaining;
        std::function<void()> release;
        Batch(size_t count, std::function<void()> done) : remaining(count), release(std::move(done)) {}
        void Complete() { if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) release(); }
    };
    boundary.Await([&](std::function<void()> release) {
        auto batch = std::make_shared<Batch>(sessions.size(), std::move(release));
        // Submit every description before waiting: unrelated sessions execute
        // in parallel rather than serializing the whole store on each reply.
        for (auto const& session : sessions) {
            UsdGenImagingSession::CommitRequest request;
            request.frame = frame;
            request.reason = reason;
            try {
                if (session->CommitAsync(std::move(request),
                    [batch](UsdGenImagingSession::CommitPayload const&, Pipeline::Outcome) {
                        batch->Complete();
                    })) continue;
            } catch (...) {
                // Already accepted siblings must still complete their replies.
                TF_WARN("usdGen imaging batch request could not be dispatched");
            }
            batch->Complete();
        }
    });
}
}

void UsdGenSessionStore::SetTime(double frame)
{
    // The UsdGenImaging_SetTime C ABI (06 §6.1, item 9) forwards here: the
    // app owns time from now on (rule b deactivates, 06 §3.9).
    // Each frame+commit is one scheduled request. The external adapter waits
    // for replies without holding the store guard across execution (S15).
    auto sessions = LiveSessions();
    for (auto const& session : sessions) session->MarkAppDriver();
    CommitBatch(sessions, usdGen::UsdGenCommitReason::SetTime, frame);
}

void UsdGenSessionStore::SetContext(usdGen::UsdGenContext context)
{
    for (auto const &session : LiveSessions()) {
        session->SetContext(context);
    }
}

void UsdGenSessionStore::Commit(usdGen::UsdGenCommitReason reason)
{
    CommitBatch(LiveSessions(), reason);
}

int64_t UsdGenSessionStore::Generation() const noexcept
{
    int64_t gen = -1;
    for (auto const &session : LiveSessions()) {
        gen = std::max(gen, session->Generation());
    }
    return gen;
}

void UsdGenSessionStore::ReloadMaps()
{
    // S13. The M1 engine exposes no session-level map-reload entry: the map
    // textureGeneration rides inside UsdGenGraphDesc::maps (06 §3.7), so the
    // reload is delivered by re-pulling the desc — MarkNeedsDesc routes the
    // next commit through the desc builder (descBuilder reads
    // ArAssetInfo::GetGeneration per map at build time).
    static std::once_flag warned;
    std::call_once(warned, []() {
        TF_WARN("usdGen ReloadMaps: no engine-level map reload hook in M1; "
                "marking sessions desc-dirty so the next commit re-resolves "
                "map textureGenerations (06 §3.7 S13).");
    });
    for (auto const &session : LiveSessions()) {
        session->MarkNeedsDesc();
    }
}

}  // namespace usdGenImaging
