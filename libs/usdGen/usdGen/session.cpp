#include "usdGen/session.h"
#include "usdGen/sessionCooker.h"

#ifdef USDGEN_ENABLE_CUDA
#include <cuda_runtime_api.h>
#endif
#include <stdexcept>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE
namespace usdGen {
namespace {
UsdGenExecutionRuntime& DefaultRuntime() {
    static UsdGenExecutionRuntime runtime(8);
    return runtime;
}

void Merge(UsdGenPendingDirty& into, UsdGenPendingDirty const& pending) {
    if (pending.structural) {
        into = UsdGenPendingDirty{};
        into.structural = true;
        return;
    }
    into.surfaceTopology |= pending.surfaceTopology;
    for (auto const& entry : pending.nodeBits) into.nodeBits[entry.first] |= entry.second;
    for (auto const& entry : pending.surfaceBits) into.surfaceBits[entry.first] |= entry.second;
}

UsdGenSessionGraphSnapshot GraphInfo(UsdGenSessionCooker const& cooker) {
    UsdGenSessionGraphSnapshot info;
    info.nodeCount = cooker.NodeCount();
    info.desc = cooker.GraphDesc();
    info.cudaPlan = cooker.CudaPlan();
    for (auto const& node : info.desc.nodes)
        info.nodeIds[node.path] = cooker.NodeIdForPath(node.path);
    return info;
}

struct CommitReply {
    UsdGenSession::Completion completion;
    bool completed = false; // command owner only
    void Finish(UsdGenSession::SnapshotPtr snapshot, UsdGenExecutionPipeline::Outcome outcome) {
        if (completed) return;
        completed = true;
        if (completion) completion(std::move(snapshot), outcome);
    }
};
}

struct UsdGenSession::State {
    // Command-owner state. The cooker below is accessed ONLY by the work node.
    std::shared_ptr<const UsdGenGraphDesc> desc = std::make_shared<const UsdGenGraphDesc>();
    UsdGenContext context = UsdGenContext::Interactive;
    UsdGenPendingDirty pending;
    std::atomic<bool> dirty{false};
    bool devicePublication = false, densityDrag = false, invalidateValues = false;
    std::shared_ptr<const UsdGenDeviceGeneration> staged;
    UsdGenGenerationConstPtr stagedBase;
    uint64_t activeEdit = 0, nextEdit = 0;
    uint64_t publishedWorkerEpoch = 0, activeWorkEpoch = 0;
    std::unique_ptr<UsdGenSessionCooker> cooker;
    std::unique_ptr<UsdGenExecutionRuntime> privateRuntime;
    // Last member: shutdown drains/cancels before any captured owner state dies.
    UsdGenExecutionPipeline pipeline;

    explicit State(int limit)
        : cooker(new UsdGenSessionCooker(limit)),
          privateRuntime(limit > 0 ? new UsdGenExecutionRuntime(limit) : nullptr),
          pipeline(privateRuntime ? *privateRuntime : DefaultRuntime()) {}

    void Invalidate() {
        staged.reset();
        stagedBase.reset();
        dirty.store(true, std::memory_order_release);
        // Called on this pipeline's owner: invalidation precedes any queued
        // completion, even while the work node is occupied by an old cook.
        pipeline.CancelPending();
    }
    void SetDesc(std::shared_ptr<const UsdGenGraphDesc> value) {
        desc = std::move(value);
        pending = UsdGenPendingDirty{};
        pending.structural = true;
        Invalidate();
    }
    void SetContext(UsdGenContext value) {
        if (context == value) return;
        context = value;
        Invalidate();
    }
    void SetDevicePublication(bool value) {
        if (devicePublication == value) return;
        devicePublication = value;
        Invalidate();
    }
};

UsdGenSession::UsdGenSession(int limit) : _state(new State(limit)) {}
UsdGenSession::~UsdGenSession() {
    // External submitters must already have stopped. Keep _snapshot alive while
    // closing invokes terminal callbacks for accepted work and commands.
    if (UsdGenExecutionPipeline::IsExecuting()) std::terminate();
    _state->pipeline.Shutdown();
    _state.reset();
}
UsdGenSession::SnapshotPtr UsdGenSession::Snapshot() const noexcept {
    return std::atomic_load(&_snapshot);
}
int UsdGenSession::CaptureCallerDevice() noexcept {
    int device = -2;
#ifdef USDGEN_ENABLE_CUDA
    if (cudaGetDevice(&device) != cudaSuccess) device = -2;
#endif
    return device;
}
bool UsdGenSession::NeedsCommit() const noexcept {
    return _state->dirty.load(std::memory_order_acquire);
}
void UsdGenSession::PostGraphDesc(UsdGenGraphDesc desc) {
    auto value = std::make_shared<const UsdGenGraphDesc>(std::move(desc));
    _state->pipeline.PostCommand([this, value] { _state->SetDesc(value); });
}
void UsdGenSession::PostContext(UsdGenContext context) {
    _state->pipeline.PostCommand([this, context] { _state->SetContext(context); });
}
void UsdGenSession::PostDevicePublicationEnabled(bool value) {
    _state->pipeline.PostCommand([this, value] { _state->SetDevicePublication(value); });
}
void UsdGenSession::PostDirty(UsdGenPendingDirty pending) {
    if (!pending.Any()) return;
    _state->pipeline.PostCommand([this, pending=std::move(pending)] {
        Merge(_state->pending, pending);
        _state->Invalidate();
    });
}
void UsdGenSession::SetGraphDesc(UsdGenGraphDesc const& desc) {
    auto value = std::make_shared<const UsdGenGraphDesc>(desc);
    _state->pipeline.InvokeOwner([this, value] { _state->SetDesc(value); });
}
void UsdGenSession::SetContext(UsdGenContext context) {
    _state->pipeline.InvokeOwner([this, context] { _state->SetContext(context); });
}
void UsdGenSession::SetDevicePublicationEnabled(bool value) {
    _state->pipeline.InvokeOwner([this, value] { _state->SetDevicePublication(value); });
}
void UsdGenSession::AccumulateDirty(UsdGenPendingDirty&& pending) {
    if (!pending.Any()) return;
    _state->pipeline.InvokeOwner([this, pending=std::move(pending)] {
        Merge(_state->pending, pending);
        _state->Invalidate();
    });
}

bool UsdGenSession::BeginDeviceEdit(UsdGenGenerationConstPtr const& expected,
                                    uint64_t* token, std::string* reason) {
    bool ok = false;
    _state->pipeline.InvokeOwner([&] {
        auto& state = *_state;
        if (!token || state.activeEdit || state.nextEdit == UINT64_MAX ||
            !state.devicePublication || !expected || !expected->device ||
            expected != Generation() || state.dirty.load() || state.pending.Any() ||
            state.activeWorkEpoch) {
            if (reason) *reason = "device stroke is already reserved or its base is not clean/current";
            return;
        }
        state.activeEdit = ++state.nextEdit;
        *token = state.activeEdit;
        ok = true;
    });
    return ok;
}

bool UsdGenSession::StageDeviceRevision(UsdGenGenerationConstPtr const& expected,
    std::shared_ptr<const UsdGenDeviceGeneration> revision, uint64_t token, std::string* reason) {
    bool ok = false;
    _state->pipeline.InvokeOwner([&] {
        auto& state = *_state;
        if (!token || token != state.activeEdit || !state.devicePublication ||
            !expected || !expected->device || expected != Generation() ||
            state.pending.Any() || state.activeWorkEpoch || (state.dirty.load() && !state.staged)) {
            if (reason) *reason = "device edit base is stale or the graph has pending changes";
            return;
        }
        if (!revision || !revision->Owner()->ProducerReady() ||
            revision->Identity().backend != expected->device->Identity().backend ||
            revision->Identity().deviceIndex != expected->device->Identity().deviceIndex ||
            revision->Identity().generation != static_cast<uint64_t>(expected->id + 1) ||
            revision->Geometry().topologyVersion != expected->device->Geometry().topologyVersion ||
            revision->Geometry().curveCount != expected->device->Geometry().curveCount ||
            revision->Geometry().pointCount != expected->device->Geometry().pointCount ||
            revision->Geometry().alreadyDeformed != expected->device->Geometry().alreadyDeformed) {
            if (reason) *reason = "device edit revision has incompatible identity or topology";
            return;
        }
        state.staged = std::move(revision);
        state.stagedBase = expected;
        state.dirty.store(true, std::memory_order_release);
        ok = true;
    });
    return ok;
}

bool UsdGenSession::EndDeviceEdit(uint64_t token,
    UsdGenGenerationConstPtr const& expected, bool discard) {
    bool ok = false;
    _state->pipeline.InvokeOwner([&] {
        auto& state = *_state;
        if (!token || token != state.activeEdit) return;
        ok = expected && expected == Generation() && !state.pending.Any() &&
            !state.activeWorkEpoch && (!state.dirty.load() || state.staged);
        // A matching reservation must be released even when a cook has made
        // its base stale. A wrong token returned above cannot release it.
        state.activeEdit = 0;
        if (discard && state.staged) {
            state.staged.reset();
            state.stagedBase.reset();
            state.dirty.store(state.pending.Any(), std::memory_order_release);
        }
    });
    return ok;
}

bool UsdGenSession::CommitAsync(CommitRequest request, Completion completion) {
    // CUDA's current device is thread-local. Capture it at the caller boundary,
    // never infer it from whichever framework worker picks up this request.
    const int callerDevice = request.callerDevice ? *request.callerDevice : CaptureCallerDevice();
    auto reply = std::make_shared<CommitReply>();
    reply->completion = std::move(completion);
    return _state->pipeline.PostCommand([this, request=std::move(request), callerDevice, reply] {
        auto& state = *_state;
        try {
            // These optional mutations and the baseline capture are one owner
            // command.  Do not route them through PostGraphDesc/PostContext:
            // another command could otherwise split this request in two.
            if (request.desc) state.SetDesc(request.desc);
            if (request.context) state.SetContext(*request.context);
            const double frame = request.frame;
            const UsdGenCommitReason reason = request.reason;
            auto baseline = Snapshot();
            auto previous = baseline ? baseline->generation : nullptr;
            if (state.staged && reason == UsdGenCommitReason::LiveOverride &&
                state.devicePublication && !state.pending.Any() && !state.activeWorkEpoch &&
                previous && previous->frame == frame && state.stagedBase == previous) {
                auto next = std::make_shared<UsdGenGeneration>();
                next->id = previous->id + 1;
                next->frame = frame;
                next->device = std::move(state.staged);
                auto snapshot = std::make_shared<UsdGenSessionSnapshot>(*baseline);
                snapshot->generation = std::move(next);
                snapshot->report = UsdGenDirtyReport{};
                snapshot->diagnostics = UsdGenDiagnostics{};
                ++snapshot->stats.commits;
                std::atomic_store(&_snapshot, SnapshotPtr(snapshot));
                state.stagedBase.reset();
                state.dirty.store(false, std::memory_order_release);
                reply->Finish(snapshot, UsdGenExecutionPipeline::Outcome::Published);
                return;
            }
            state.staged.reset();
            state.stagedBase.reset();
            state.dirty.store(true, std::memory_order_release);
            auto work = [this, desc=state.desc, context=state.context,
                         device=state.devicePublication, pending=state.pending,
                         invalidate=state.invalidateValues, publishedEpoch=state.publishedWorkerEpoch,
                         baseline, previous, frame, reason, callerDevice]
                        (UsdGenExecutionPipeline::Cancellation const& cancellation) {
                auto& cooker = *_state->cooker;
                auto generation = cooker.Cook(desc, context, device, pending, frame, reason,
                    previous, baseline ? baseline->stats : UsdGenStats{}, invalidate,
                    publishedEpoch, cancellation.epoch, callerDevice);
                auto snapshot = std::make_shared<UsdGenSessionSnapshot>();
                snapshot->generation = generation;
                snapshot->report = cooker.Report();
                snapshot->diagnostics = cooker.Diagnostics();
                snapshot->stats = cooker.Stats();
                snapshot->routing = cooker.Routing();
                snapshot->graphInfo = GraphInfo(cooker);
                snapshot->nodeStats = cooker.NodeStats();
                snapshot->cudaBindings = cooker.CudaBindingStats();
                const bool failed = snapshot->diagnostics.HasErrors() || !generation || generation == previous;
                if (failed) {
                    snapshot->generation = previous;
                    snapshot->report = UsdGenDirtyReport{};
                    snapshot->stats.commits = baseline ? baseline->stats.commits : 0;
                    // Performance/resource counters describe the retained
                    // generation, not a partially executed rejected graph.
                    snapshot->nodeStats = baseline ? baseline->nodeStats
                        : std::unordered_map<UsdGenNodeId, UsdGenNodeStats>{};
                    snapshot->cudaBindings = baseline ? baseline->cudaBindings
                        : std::vector<UsdGenCudaBindingStats>{};
                } else if (!generation->device) {
                    snapshot->cudaBindings.clear();
                }
                if (failed && !snapshot->diagnostics.HasErrors())
                    snapshot->diagnostics.Error("execution returned no new generation");
                return UsdGenExecutionPipeline::Publish([this, snapshot, failed, epoch=cancellation.epoch] {
                    // The pipeline executes this only for its current ticket.
                    std::atomic_store(&_snapshot, SnapshotPtr(snapshot));
                    if (failed) throw std::runtime_error("session cook failed; previous generation retained");
                    _state->pending = UsdGenPendingDirty{};
                    _state->invalidateValues = false;
                    _state->publishedWorkerEpoch = epoch;
                    _state->dirty.store(false, std::memory_order_release);
                });
            };
            state.activeWorkEpoch = state.pipeline.Submit(std::move(work),
                [this, reply](uint64_t epoch, UsdGenExecutionPipeline::Outcome outcome, std::exception_ptr error) {
                    if (_state->activeWorkEpoch == epoch) _state->activeWorkEpoch = 0;
                    if (outcome == UsdGenExecutionPipeline::Outcome::Failed && error) {
                        auto current = Snapshot();
                        if (!current || !current->diagnostics.HasErrors()) {
                            auto failed = current ? std::make_shared<UsdGenSessionSnapshot>(*current)
                                                  : std::make_shared<UsdGenSessionSnapshot>();
                            failed->report = UsdGenDirtyReport{};
                            try { std::rethrow_exception(error); }
                            catch (std::exception const& e) { failed->diagnostics.Error(e.what()); }
                            catch (...) { failed->diagnostics.Error("unknown session execution failure"); }
                            std::atomic_store(&_snapshot, SnapshotPtr(failed));
                        }
                    }
                    reply->Finish(Snapshot(), outcome);
                });
            if (!state.activeWorkEpoch)
                reply->Finish(Snapshot(), UsdGenExecutionPipeline::Outcome::Superseded);
        } catch (...) {
            reply->Finish(Snapshot(), UsdGenExecutionPipeline::Outcome::Failed);
        }
    }, [this, reply] { reply->Finish(Snapshot(), UsdGenExecutionPipeline::Outcome::Superseded); });
}

bool UsdGenSession::CommitAsync(double frame, UsdGenCommitReason reason,
                                Completion completion) {
    CommitRequest request;
    request.frame = frame;
    request.reason = reason;
    return CommitAsync(std::move(request), std::move(completion));
}

UsdGenSession::SnapshotPtr UsdGenSession::CommitSnapshot(CommitRequest request) {
    SnapshotPtr result;
    _state->pipeline.Await([&](std::function<void()> release) {
        if (!CommitAsync(std::move(request), [&, release](SnapshotPtr snapshot, UsdGenExecutionPipeline::Outcome) {
                result = std::move(snapshot);
                release();
            })) throw std::runtime_error("session rejected commit command");
    });
    return result;
}

UsdGenSession::SnapshotPtr UsdGenSession::CommitSnapshot(
    double frame, UsdGenCommitReason reason) {
    CommitRequest request;
    request.frame = frame;
    request.reason = reason;
    return CommitSnapshot(std::move(request));
}
std::vector<UsdGenCudaBindingStats> UsdGenSession::CudaBindingStats() const {
    auto snapshot = Snapshot();
    return snapshot ? snapshot->cudaBindings : std::vector<UsdGenCudaBindingStats>{};
}
std::optional<UsdGenNodeStats> UsdGenSession::NodeStats(UsdGenNodeId id) const {
    auto snapshot = Snapshot();
    if (!snapshot) return {};
    auto found = snapshot->nodeStats.find(id);
    return found == snapshot->nodeStats.end() ? std::optional<UsdGenNodeStats>{} : found->second;
}
void UsdGenSession::InvalidateAllValues() {
    _state->pipeline.InvokeOwner([this] {
        _state->invalidateValues = true;
        _state->Invalidate();
    });
}
void UsdGenSession::BeginDensityDrag() {
    _state->pipeline.InvokeOwner([this] { _state->densityDrag = true; });
}
void UsdGenSession::EndDensityDrag() {
    _state->pipeline.InvokeOwner([this] {
        _state->densityDrag = false;
        _state->Invalidate();
    });
}
} // namespace usdGen
