// Caller-boundary Hydra capture -> per-scene owner -> independent session work.
// One immutable scene snapshot and ordered notices; no application mutex.
#include "usdGenImaging/groomSceneIndexPlugin.h"
#include "usdGenImaging/usdGenDirtyRouter.h"
#include "usdGenImaging/usdGenTilePublisher.h"
#include "usdGenImaging/usdGenGraphDescBuilder.h"
#include "usdGenImaging/usdGenEnable.h"
#include "usdGenImaging/testHook.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h"
#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/refPtr.h"
#include "pxr/imaging/hd/systemMessages.h"
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_vector.h>
#include <tbb/flow_graph.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(USDGEN_ENABLE, true, "Enable usdGen scene index plugins.");
TF_REGISTRY_FUNCTION(TfType) {
    HdSceneIndexPluginRegistry::Define<UsdGenGroomSceneIndexPlugin>();
}
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),
        TfToken("UsdGenGroomSceneIndexPlugin"), nullptr, 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}
HdSceneIndexBaseRefPtr UsdGenGroomSceneIndexPlugin::_AppendSceneIndex(
    HdSceneIndexBaseRefPtr const& input, HdContainerDataSourceHandle const&) {
    return UsdGenGroomSceneIndex::New(input);
}
bool UsdGenGroomSceneIndexPlugin::_IsEnabled(HdContainerDataSourceHandle const&) const {
    return TfGetEnvSetting(USDGEN_ENABLE);
}

namespace {
using Pipeline = usdGen::UsdGenExecutionPipeline;
using Session = ::usdGenImaging::UsdGenImagingSession;
using Handle = ::usdGenImaging::UsdGenSessionHandle;
using Key = ::usdGenImaging::UsdGenSessionKey;
using Desc = usdGen::UsdGenGraphDesc;
using CaptureCache = ::usdGenImaging::UsdGenGraphDescCaptureCache;
using Added = HdSceneIndexObserver::AddedPrimEntries;
using Removed = HdSceneIndexObserver::RemovedPrimEntries;
using Dirtied = HdSceneIndexObserver::DirtiedPrimEntries;
using TileMap = std::map<SdfPath, HdContainerDataSourceHandle>;
SdfPath RenderPath(SdfPath const& description) {
    return description.AppendChild(TfToken("__usdGenRender"));
}
TfToken TypeName(HdSceneIndexPrim const& prim) {
    if (!prim.primType.IsEmpty()) return prim.primType;
    auto type = HdTokenDataSource::Cast(HdContainerDataSource::Get(prim.dataSource,
        HdDataSourceLocator(TfToken("__usdPrimInfo"), TfToken("typeName"))));
    return type ? type->GetTypedValue(0) : TfToken();
}
bool IsGroom(TfToken const& type) {
    return type == TfToken("UsdGenGroom") || type == TfToken("UsdGenDescription");
}

// Records actual builder reads, including missing targets and GeomSubset
// parent meshes. No live data-source handle enters the owner catalog.
class RecordingInput final : public HdSceneIndexBase {
public:
    explicit RecordingInput(HdSceneIndexBaseRefPtr input) : input(std::move(input)) {}
    HdSceneIndexPrim GetPrim(SdfPath const& path) const override {
        paths.insert(path);
        return input->GetPrim(path);
    }
    SdfPathVector GetChildPrimPaths(SdfPath const& path) const override {
        paths.insert(path);
        return input->GetChildPrimPaths(path);
    }
    std::shared_ptr<const SdfPathVector> Dependencies(Desc const& desc) const {
        // Some adapter aggregates transport referenced values directly,
        // without a separate GetPrim through this recording facade.
        for (auto const& expression : desc.expressions) paths.insert(expression.path);
        for (auto const& node : desc.nodes) {
            for (auto const& binding : node.expressionBindings) paths.insert(binding.expression.GetPrimPath());
            for (auto const& path : node.references) paths.insert(path.GetPrimPath());
        }
        paths.erase(SdfPath());
        return std::make_shared<const SdfPathVector>(paths.begin(), paths.end());
    }
private:
    HdSceneIndexBaseRefPtr input;
    mutable std::set<SdfPath> paths;
};
}

struct UsdGenGroomSceneIndex::_Ingress {
    struct Input {
        SdfPath root, description;
        Key key;
        std::shared_ptr<const Desc> desc;
        std::shared_ptr<const SdfPathVector> dependencies;
        std::shared_ptr<const CaptureCache> cache;
        bool authoredRender = false;
    };
    uint64_t sequence = 0;
    int device = -2;
    double frame = 0;
    bool initial = false, failed = false;
    bool fullPopulation = true;
    SdfPathVector captureRoots;
    Added added;
    Removed removed;
    Dirtied dirtied;
    std::vector<Input> inputs;
};

// Constructed after the store: scenes close before store/retirement teardown.
// Every scene has its OWN pipeline on the shared runtime.
struct UsdGenSceneService {
    struct RetirementRecord {
        std::unique_ptr<tbb::flow::graph> completion{new tbb::flow::graph};
        std::atomic<bool> pending{true};
        RetirementRecord() { completion->reserve_wait(); }
        ~RetirementRecord() { if (completion) Done(); }
        void Done() { if (pending.exchange(false)) completion->release_wait(); }
        void Wait() { completion->wait_for_all(); }
        // Called only with a strong live State after all its work is gone.
        // A public static handle can then outlive framework static teardown.
        void Disarm() { Done(); completion.reset(); }
    };
    struct Entry {
        std::weak_ptr<UsdGenGroomSceneIndex::_State> state;
        std::shared_ptr<RetirementRecord> retirement;
    };
    usdGen::UsdGenExecutionRuntime runtime{8};
    tbb::flow::graph retirement;
    tbb::flow::function_node<std::function<void()>> cleanup;
    tbb::concurrent_vector<Entry> states;
    UsdGenSceneService() : cleanup(retirement, tbb::flow::unlimited,
        [](std::function<void()> action) { action(); return tbb::flow::continue_msg{}; }) {}
    ~UsdGenSceneService();
    void Retire(std::function<void()> action) {
        if (!cleanup.try_put(std::move(action))) std::terminate();
    }
    void DrainRetired();
};
namespace {
UsdGenSceneService& SceneService() {
    (void)::usdGenImaging::UsdGenSessionStore::GetInstance();
    static UsdGenSceneService service;
    return service;
}
}

struct UsdGenGroomSceneIndex::_State : std::enable_shared_from_this<_State> {
    struct Groom {
        uint64_t id = 0, captured = 0;
        bool alive = true, authoredRender = false;
        SdfPath root, description;
        Key key;
        Handle session;
        int callback = -1, device = -2;
        double frame = 0;
        std::shared_ptr<const Desc> desc;
        std::shared_ptr<const SdfPathVector> dependencies;
        std::shared_ptr<const CaptureCache> cache;
        std::shared_ptr<const usdGenImaging::UsdGenDirtyRouter> router;
        std::shared_ptr<const TileMap> tiles = std::make_shared<const TileMap>();
        int64_t generation = -1;
    };
    struct View {
        SdfPath root, description;
        std::shared_ptr<const TileMap> tiles;
        int64_t generation;
        std::shared_ptr<const SdfPathVector> dependencies;
        std::shared_ptr<const CaptureCache> cache;
        double frame = 0;
    };
    struct Snapshot {
        std::vector<View> members;
        uint64_t capturedThrough = 0;
        bool captureTrusted = true;
    };
    // catalog is owner-written/capture-read. visible is only installed by
    // the serialized Hydra frontend immediately before its matching notice.
    std::shared_ptr<const Snapshot> catalog = std::make_shared<const Snapshot>();
    std::shared_ptr<const Snapshot> visible = std::make_shared<const Snapshot>();
    struct PublicationPacket {
        std::shared_ptr<const Snapshot> snapshot;
        Added added;
        Removed removed;
        Dirtied dirtied;
    };
    tbb::concurrent_queue<PublicationPacket> publications;
    std::atomic<uint64_t> readyPublications{0};
    uint64_t deliveredPublications = 0; // serialized frontend only
    HdSceneIndexBasePtr recipient; // initialized before admission; weak
    std::atomic<uint64_t> issued{0};
    std::atomic<bool> closing{false};
    std::atomic<bool> quiesced{false};
    std::atomic<bool> asyncAllowed{false};
    // Owner-only state below.
    std::map<SdfPath, std::shared_ptr<Groom>> members;
    std::map<SdfPath, uint64_t> events, tombstones;
    uint64_t nextId = 0, completedPrefix = 0;
    std::set<uint64_t> completed;
    std::map<uint64_t, size_t> holds;
    struct Waiter { uint64_t watermark; std::function<void()> done; };
    std::vector<Waiter> waiters;
    bool closeProcessed = false;
    bool captureTrusted = true;
    uint64_t captureTrustSequence = 0;
    std::vector<TfWeakPtr<Session>> usedSessions;
    std::unique_ptr<Pipeline> owner; // removed at process shutdown, even if public index survives

    explicit _State(usdGen::UsdGenExecutionRuntime& runtime) : owner(new Pipeline(runtime)) {}
    ~_State() { if (owner) owner->Shutdown(); }
    auto SnapshotValue() const { return std::atomic_load(&catalog); }
    auto VisibleSnapshot() const { return std::atomic_load(&visible); }
    std::shared_ptr<const Snapshot> PublishSnapshot() {
        auto next = std::make_shared<Snapshot>();
        next->capturedThrough = completedPrefix;
        next->captureTrusted = captureTrusted;
        for (auto const& item : members) {
            auto const& g = *item.second;
            next->members.push_back({g.root, g.description, g.tiles, g.generation,
                                     g.dependencies, g.cache, g.frame});
        }
        auto result = std::shared_ptr<const Snapshot>(std::move(next));
        std::atomic_store(&catalog, result);
        return result;
    }
    void QueuePublication(Added added = {}, Removed removed = {}, Dirtied dirtied = {}) {
        // The queue is single-owner producer and serialized-Hydra consumer.
        // A packet is retained even when empty so the visible catalog cannot
        // leap ahead of a delayed structural notice.
        publications.push({PublishSnapshot(), std::move(added), std::move(removed),
                           std::move(dirtied)});
        readyPublications.fetch_add(1, std::memory_order_release);
    }
    void Post(std::function<void()> command) {
        // Required lifetime relays cannot be silently discarded.
        try { if (!owner->PostCommand(std::move(command))) std::terminate(); }
        catch (...) { std::terminate(); }
    }
    void CheckWaiters() {
        auto it = waiters.begin();
        while (it != waiters.end()) {
            bool ready = (!closing.load() || closeProcessed) && completedPrefix >= it->watermark &&
                (holds.empty() || holds.begin()->first > it->watermark);
            if (!ready) { ++it; continue; }
            auto done = std::move(it->done);
            it = waiters.erase(it);
            done();
        }
    }
    void Hold(uint64_t seq) { ++holds[seq]; }
    void Release(uint64_t seq) {
        auto it = holds.find(seq);
        if (it == holds.end() || it->second == 0) std::terminate();
        if (--it->second == 0) holds.erase(it);
        CheckWaiters();
    }
    void CompleteIngress(uint64_t seq) {
        completed.insert(seq);
        while (completed.erase(completedPrefix + 1)) ++completedPrefix;
        // Every capture at/below this prefix has arrived. Its suppression
        // history is no longer needed by any delayed ingress.
        auto prune = [this](auto& history) {
            for (auto it = history.begin(); it != history.end();)
                if (it->second <= completedPrefix) it = history.erase(it);
                else ++it;
        };
        prune(events);
        prune(tombstones);
        // Catalog reuse is safe only after every earlier capture has been
        // applied. Publication callbacks do not advance this source watermark.
        QueuePublication();
        CheckWaiters();
    }
    void Synchronize() {
        if (quiesced.load()) return;
        auto self = shared_from_this();
        const uint64_t watermark = issued.load(std::memory_order_acquire);
        owner->Await([self, watermark](std::function<void()> done) {
            self->Post([self, watermark, done] {
                self->waiters.push_back({watermark, done});
                self->CheckWaiters();
            });
        });
    }
    bool Current(std::shared_ptr<Groom> const& g) const {
        auto it = members.find(g->root);
        return g->alive && it != members.end() && it->second == g;
    }
    bool NewerEvent(SdfPath const& path, uint64_t seq) const {
        for (auto const& e : events)
            if (e.second > seq && (path.HasPrefix(e.first) || e.first.HasPrefix(path))) return true;
        return false;
    }
    bool RemovedSince(SdfPath const& root, uint64_t seq) const {
        for (auto const& e : tombstones)
            if (root.HasPrefix(e.first) && e.second >= seq) return true;
        return false;
    }
    void Notify(Added const& added, Removed const& removed, Dirtied const& dirtied) {
        if (closing.load()) return;
        // Owner callbacks never enter Hydra observers.  The immutable packet
        // is delivered by the serialized frontend on a synchronous ingress or
        // negotiated asyncPoll boundary.
        QueuePublication(added, removed, dirtied);
    }
    void Detach(Handle session, Key key, uint64_t seq) {
        if (!session) return;
        Hold(seq);
        auto self = shared_from_this();
        bool accepted = false;
        try {
            accepted = ::usdGenImaging::UsdGenSessionStore::GetInstance().DetachAsync(key, session,
                [self, session, seq] { self->Post([self, session, seq] { self->Release(seq); }); });
        } catch (...) { std::terminate(); }
        if (!accepted) Release(seq);
    }
    void Remove(std::shared_ptr<Groom> const& g, uint64_t seq) {
        g->alive = false;
        if (g->session) {
            if (g->callback >= 0) {
                Hold(seq);
                auto self = shared_from_this();
                try {
                    if (!g->session->UnregisterRepublishCallbackAsync(g->callback,
                        [self, seq] { self->Post([self, seq] { self->Release(seq); }); }))
                        Release(seq);
                } catch (...) { std::terminate(); }
            }
            Detach(g->session, g->key, seq);
        }
    }
    void Close() {
        if (closing.exchange(true)) return;
        auto self = shared_from_this();
        Post([self] {
            try {
                const uint64_t seq = self->issued.load();
                for (auto const& item : self->members) self->Remove(item.second, seq);
                self->members.clear();
                self->PublishSnapshot();
                self->closeProcessed = true;
                self->CheckWaiters();
            } catch (...) { std::terminate(); }
        });
    }
    void Publish(std::shared_ptr<Groom> const& g, Session::CommitPayload const& payload) {
        if (closing.load() || !Current(g) || !payload.published || !payload.generation ||
            payload.generation->id <= g->generation) return;
        auto const& generation = *payload.generation;
        if (generation.device) {
            TF_WARN("usdGen: stock Hydra publication requires unfinished device interop; retaining displayed geometry");
            return;
        }
        auto fresh = std::make_shared<TileMap>();
        const auto render = RenderPath(g->description);
        for (auto const& tile : generation.tiles) {
            if (!tile.primPath.HasPrefix(render)) return; // old description namespace
            fresh->emplace(tile.primPath,
                ::usdGenImaging::UsdGenTilePublisher::BuildTileDataSource(tile, generation.id));
        }
        Added added;
        Removed removed;
        Dirtied dirtied;
        for (auto const& old : *g->tiles)
            if (!fresh->count(old.first)) removed.emplace_back(old.first);
        for (auto const& tile : *fresh) {
            if (!g->tiles->count(tile.first)) added.emplace_back(tile.first, TfToken("basisCurves"));
            else {
                // Missed intermediate publications make their report an
                // invalid diff against THIS scene's displayed baseline.
                HdDataSourceLocatorSet locators;
                if (g->generation + 1 != generation.id) locators.insert(HdDataSourceLocator());
                else for (auto const& report : payload.report.tiles) {
                    if (report.primPath != tile.first) continue;
                    for (auto const& loc : ::usdGenImaging::UsdGenTilePublisher::NoticesFor(report).all())
                        locators.insert(loc);
                }
                if (!locators.IsEmpty()) dirtied.emplace_back(tile.first, locators);
            }
        }
        auto router = std::make_shared<usdGenImaging::UsdGenDirtyRouter>();
        if (payload.routing) router->Rebuild(*payload.routing);
        g->router = std::move(router);
        g->generation = generation.id;
        g->tiles = std::move(fresh);
        Notify(added, removed, dirtied);
    }
    void Cook(std::shared_ptr<Groom> const& g, uint64_t seq) {
        if (!g->session || !g->desc || closing.load() || !Current(g)) return;
        Session::CommitRequest request;
        request.reason = usdGen::UsdGenCommitReason::NoticeBatchEnd;
        request.desc = g->desc;
        request.callerDevice = g->device;
        if (!g->session->HasAppDriver()) request.frame = g->frame;
        g->session->ConsumeNeedsDesc();
        auto self = shared_from_this();
        Hold(seq);
        bool accepted = false;
        try {
            accepted = g->session->CommitAsync(std::move(request),
                [self, g, seq](Session::CommitPayload const& payload, Pipeline::Outcome) {
                    self->Post([self, g, seq, payload] {
                        try { self->Publish(g, payload); }
                        catch (...) { TF_WARN("usdGen scene publication failed"); }
                        self->Release(seq);
                    });
                });
        } catch (...) { TF_WARN("usdGen scene cook request failed"); }
        if (!accepted) Release(seq);
    }
    void Attach(std::shared_ptr<Groom> const& g, uint64_t seq) {
        auto self = shared_from_this();
        Hold(seq);
        bool accepted = false;
        try {
            accepted = ::usdGenImaging::UsdGenSessionStore::GetInstance().AttachAsync(g->key,
                [self, g, seq](Handle session) {
                    self->Post([self, g, seq, session] {
                        try {
                            if (!session) { self->Release(seq); return; }
                            self->usedSessions.push_back(TfCreateWeakPtr(session.operator->()));
                            if (self->closing.load() || !self->Current(g)) {
                                self->Detach(session, g->key, seq);
                                self->Release(seq);
                                return;
                            }
                            g->session = session;
                            std::weak_ptr<_State> weak(self);
                            std::weak_ptr<Groom> groom(g);
                            g->callback = session->RegisterRepublishCallback(
                                [weak, groom](Session::CommitPayload const& payload) {
                                    auto state = weak.lock();
                                    auto member = groom.lock();
                                    if (!state || !member || state->closing.load()) return;
                                    state->Post([state, member, payload] { state->Publish(member, payload); });
                                });
                            self->Cook(g, seq);
                            self->Release(seq);
                        } catch (...) { std::terminate(); }
                    });
                });
        } catch (...) { TF_WARN("usdGen scene attachment request failed"); }
        if (!accepted) Release(seq);
    }
    void Apply(_Ingress const& packet) {
        const uint64_t seq = packet.sequence;
        if (packet.failed && seq >= captureTrustSequence) {
            captureTrusted = false;
            captureTrustSequence = seq;
        }
        if (closing.load() || packet.failed) { CompleteIngress(seq); return; }
        if (packet.fullPopulation && seq >= captureTrustSequence) {
            captureTrusted = true;
            captureTrustSequence = seq;
        }
        Added forwardAdded;
        Removed forwardRemoved;
        Dirtied forwardDirtied;
        for (auto const& e : packet.removed) {
            if (NewerEvent(e.primPath, seq)) continue;
            events[e.primPath] = std::max(events[e.primPath], seq);
            tombstones[e.primPath] = std::max(tombstones[e.primPath], seq);
            for (auto it = members.begin(); it != members.end();) {
                if (it->first.HasPrefix(e.primPath)) {
                    Remove(it->second, seq);
                    it = members.erase(it);
                } else ++it;
            }
            forwardRemoved.push_back(e);
        }
        for (auto const& e : packet.added) {
            if (NewerEvent(e.primPath, seq)) continue;
            events[e.primPath] = std::max(events[e.primPath], seq);
            forwardAdded.push_back(e);
        }
        for (auto const& e : packet.dirtied) {
            if (NewerEvent(e.primPath, seq)) continue;
            events[e.primPath] = std::max(events[e.primPath], seq);
            forwardDirtied.push_back(e);
        }
        std::vector<std::shared_ptr<Groom>> startAttach, startCook;
        // A Hydra Added notice can resync an existing groom to a non-groom
        // type without an explicit Removed. Retire roots absent from this
        // authoritative capture, but never overwrite a newer ingress.
        for (auto it = members.begin(); it != members.end();) {
            bool found = std::any_of(packet.inputs.begin(), packet.inputs.end(),
                [&](auto const& input) { return input.root == it->first; });
            bool captured = packet.fullPopulation ||
                std::find(packet.captureRoots.begin(), packet.captureRoots.end(), it->first) !=
                    packet.captureRoots.end();
            if (captured && !found && it->second->captured <= seq && !NewerEvent(it->first, seq)) {
                if (!it->second->authoredRender)
                    forwardRemoved.emplace_back(RenderPath(it->second->description));
                Remove(it->second, seq);
                it = members.erase(it);
            } else ++it;
        }
        for (auto const& input : packet.inputs) {
            if (RemovedSince(input.root, seq) || NewerEvent(input.root, seq)) continue;
            auto it = members.find(input.root);
            if (it != members.end() && it->second->captured > seq) continue;
            if (it != members.end() && (it->second->description != input.description ||
                                       !(it->second->key == input.key))) {
                if (!it->second->authoredRender)
                    forwardRemoved.emplace_back(RenderPath(it->second->description));
                Remove(it->second, seq);
                members.erase(it);
                it = members.end();
            }
            std::shared_ptr<Groom> groom;
            if (it == members.end()) {
                groom = std::make_shared<Groom>();
                groom->id = ++nextId;
                groom->root = input.root;
                groom->description = input.description;
                groom->key = input.key;
                groom->authoredRender = input.authoredRender;
                members.emplace(input.root, groom);
                if (!input.authoredRender)
                    forwardAdded.emplace_back(RenderPath(input.description), TfToken("scope"));
                startAttach.push_back(groom);
            } else {
                groom = it->second;
                if (groom->session) startCook.push_back(groom);
            }
            groom->captured = seq;
            groom->desc = input.desc;
            groom->dependencies = input.dependencies;
            groom->cache = input.cache;
            groom->device = packet.device;
            groom->frame = packet.frame;
        }
        Notify(forwardAdded, forwardRemoved, forwardDirtied);
        for (auto const& groom : startAttach) Attach(groom, seq);
        for (auto const& groom : startCook) Cook(groom, seq);
        CompleteIngress(seq);
    }
};

UsdGenSceneService::~UsdGenSceneService() {
    std::vector<std::shared_ptr<UsdGenGroomSceneIndex::_State>> live;
    std::vector<std::shared_ptr<RetirementRecord>> liveRecords;
    for (auto const& entry : states) {
        if (auto state = entry.state.lock()) {
            live.push_back(std::move(state));
            liveRecords.push_back(entry.retirement);
        }
        else entry.retirement->Wait(); // includes final-ref -> enqueue gap
    }
    for (auto const& state : live) state->Close();
    for (auto const& state : live) state->Synchronize();
    ::usdGenImaging::UsdGenSessionStore::GetInstance().Drain();
    // Process shutdown only: finish source callback frames before destroying
    // the retirement service captured by a scene state's final deleter.
    std::vector<Handle> sessions;
    for (auto const& state : live) state->owner->InvokeOwner([&] {
        for (auto const& weak : state->usedSessions)
            if (auto session = TfCreateRefPtrFromProtectedWeakPtr(weak))
                sessions.push_back(std::move(session));
    });
    for (auto const& session : sessions) session->Shutdown();
    for (auto const& session : sessions) if (session->Engine()) session->Engine()->Drain();
    for (size_t i = 0; i < live.size(); ++i) {
        auto const& state = live[i];
        state->owner->Shutdown();
        state->owner.reset();
        liveRecords[i]->Disarm();
        state->quiesced.store(true, std::memory_order_release);
    }
    live.clear();
    retirement.wait_for_all();
    sessions.clear();
    Session::DrainRetired();
}
void UsdGenSceneService::DrainRetired() {
    if (Pipeline::IsExecuting()) throw std::logic_error("scene callback cannot drain retirement");
    std::vector<std::shared_ptr<UsdGenGroomSceneIndex::_State>> retired;
    for (auto const& entry : states) {
        if (auto state = entry.state.lock()) {
            if (state->closing.load()) retired.push_back(std::move(state));
        } else entry.retirement->Wait();
    }
    for (auto const& state : retired) state->Synchronize();
    retired.clear();
    retirement.wait_for_all();
}

UsdGenGroomSceneIndex::UsdGenGroomSceneIndex(HdSceneIndexBaseRefPtr const& input, int id)
    : HdSingleInputFilteringSceneIndexBase(input),
      _pruned(HdSiExtComputationPrimvarPruningSceneIndex::New(input)),
      _renderInstanceId(static_cast<uint32_t>(id)) {
    static std::atomic<uint64_t> next{uint64_t(1) << 32};
    if (id == 0) _renderInstanceId = next.fetch_add(1);
    auto& service = SceneService();
    auto record = std::make_shared<UsdGenSceneService::RetirementRecord>();
    _state = std::shared_ptr<_State>(new _State(service.runtime), [&service, record](_State* state) {
        if (state->quiesced.load(std::memory_order_acquire)) {
            // A static public handle may outlive the process service. All
            // pipelines/subscriptions have already been removed externally.
            delete state;
        } else service.Retire([state, record] { delete state; record->Done(); });
    });
    service.states.push_back({_state, record});
}
HdSceneIndexBaseRefPtr UsdGenGroomSceneIndex::New(HdSceneIndexBaseRefPtr const& input, int id) {
    auto index = TfCreateRefPtr(new UsdGenGroomSceneIndex(input, id));
    index->_state->recipient = TfCreateWeakPtr(index.operator->());
    UsdGenImagingTestHook::_RegisterIndex(index.operator->());
    _Ingress initial;
    initial.initial = true;
    index->_CaptureAndSubmit(std::move(initial));
    return index;
}
UsdGenGroomSceneIndex::~UsdGenGroomSceneIndex() {
    _state->Close();
    if (!_state->quiesced.load()) UsdGenImagingTestHook::_UnregisterIndex(this);
}
void UsdGenGroomSceneIndex::_DrainPublications(bool waitForIngress, bool explicitWait) {
    // HdSceneIndex frontend entry points are serialized by Hydra.  Query
    // methods remain concurrent immutable snapshot reads; do not add an
    // application mutex here.
    if (_dispatching) {
        if (explicitWait)
            throw std::logic_error("Synchronize cannot re-enter from a scene-index notice");
        if (waitForIngress) _deferredSynchronousFlush = true;
        return;
    }
    auto live = TfCreateRefPtrFromProtectedWeakPtr(_state->recipient);
    auto *const index = dynamic_cast<UsdGenGroomSceneIndex *>(
        live ? live.operator->() : nullptr);
    if (!index) return;

    _dispatching = true;
    try {
        do {
            if (waitForIngress) _state->Synchronize();
            _deferredSynchronousFlush = false;
            // Take a finite ready watermark. Work produced while callbacks
            // run belongs to a later poll, not an ever-growing render turn.
            const auto ready = _state->readyPublications.load(std::memory_order_acquire);
            _State::PublicationPacket packet;
            while (_state->deliveredPublications < ready &&
                   _state->publications.try_pop(packet)) {
                ++_state->deliveredPublications;
                // Observers may query during _Send: they must see precisely
                // this packet's immutable post-state, never a later owner
                // catalog snapshot.
                std::atomic_store(&_state->visible, packet.snapshot);
                try { if (!packet.removed.empty()) index->_SendPrimsRemoved(packet.removed); }
                catch (...) { TF_WARN("usdGen removal observer threw"); }
                try { if (!packet.added.empty()) index->_SendPrimsAdded(packet.added); }
                catch (...) { TF_WARN("usdGen addition observer threw"); }
                try { if (!packet.dirtied.empty()) index->_SendPrimsDirtied(packet.dirtied); }
                catch (...) { TF_WARN("usdGen dirty observer threw"); }
            }
            // An observer can synchronously cause another input ingress.  It
            // cannot wait recursively; its causal flush belongs to this outer
            // frontend turn and remains FIFO after the current packet.
            waitForIngress = _deferredSynchronousFlush;
        } while (waitForIngress);
    } catch (...) {
        _dispatching = false;
        throw;
    }
    _dispatching = false;
}
void UsdGenGroomSceneIndex::Synchronize() {
    // An observer invoked by the drain may release the caller's final public
    // reference.  Keep this frontend object alive until this method returns.
    auto live = TfCreateRefPtrFromProtectedWeakPtr(_state->recipient);
    if (!live) return;
    _DrainPublications(true, true);
}
void UsdGenGroomSceneIndex::DrainRetired() { SceneService().DrainRetired(); }

void UsdGenGroomSceneIndex::_CaptureAndSubmit(_Ingress packet) {
    // A synchronous default-mode drain below can re-enter arbitrary client
    // code. Pin the public index for the entire caller-boundary operation.
    auto live = TfCreateRefPtrFromProtectedWeakPtr(_state->recipient);
    if (!live) return;
    auto state = _state;
    if (state->closing.load()) return;
    packet.sequence = state->issued.fetch_add(1, std::memory_order_acq_rel) + 1;
    packet.device = usdGen::UsdGenSession::CaptureCallerDevice();
    try {
        // Use only an owner-published complete catalog. An older in-flight
        // capture may establish new external dependencies, so fall back to
        // full discovery until its ingress has been applied.
        auto catalog = state->SnapshotValue();
        std::vector<SdfPath> stack;
        if (!packet.initial && packet.added.empty() && packet.removed.empty() &&
            catalog->captureTrusted && catalog->capturedThrough == packet.sequence - 1) {
            packet.fullPopulation = false;
            for (auto const& member : catalog->members) {
                bool affected = !member.dependencies;
                for (auto const& dirty : packet.dirtied) {
                    auto const& path = dirty.primPath;
                    if (path.HasPrefix(member.root) || member.root.HasPrefix(path)) affected = true;
                    if (member.dependencies) for (auto const& dependency : *member.dependencies)
                        // A dirty on an ancestor can change a resolved input.
                        // A sibling child of a queried metadata container
                        // cannot change that container's own sampled values.
                        if (dependency.HasPrefix(path)) affected = true;
                    if (affected) break;
                }
                if (affected) stack.push_back(member.root);
            }
            packet.captureRoots = stack;
        } else stack.push_back(SdfPath::AbsoluteRootPath());

        auto input = _GetInputSceneIndex();
        if (input && !stack.empty()) {
            auto frame = HdSceneGlobalsSchema::GetFromParent(
                input->GetPrim(SdfPath::AbsoluteRootPath()).dataSource).GetCurrentFrame();
            if (frame) {
                double value = frame->GetTypedValue(0);
                if (std::isfinite(value)) packet.frame = value;
            }
            while (!stack.empty()) {
                const auto path = stack.back(); stack.pop_back();
                const auto prim = input->GetPrim(path);
                if (path != SdfPath::AbsoluteRootPath() && IsGroom(TypeName(prim))) {
                    auto known = std::find_if(catalog->members.begin(), catalog->members.end(),
                        [&](auto const& member) { return member.root == path; });
                    _Ingress::Input captured;
                    captured.root = captured.description = path;
                    captured.key.groomRoot = path;
                    captured.key.renderInstanceId = _renderInstanceId;
                    auto id = HdStringDataSource::Cast(HdContainerDataSource::Get(
                        prim.dataSource, HdDataSourceLocator(TfToken("sessionId"))));
                    if (id) captured.key.sessionId = id->GetTypedValue(0);
                    if (!packet.fullPopulation && known != catalog->members.end()) {
                        // Dirty-only notices cannot change child prim types.
                        // Structural resyncs always take the discovery path.
                        captured.description = known->description;
                    } else if (TypeName(prim) == TfToken("UsdGenGroom")) {
                        for (auto const& child : input->GetChildPrimPaths(path))
                            if (TypeName(input->GetPrim(child)) == TfToken("UsdGenDescription")) {
                                captured.description = child; break;
                            }
                    }
                    auto authored = input->GetPrim(RenderPath(captured.description));
                    captured.authoredRender = authored.dataSource || !authored.primType.IsEmpty();
                    RecordingInput recorder(_pruned);
                    ::usdGenImaging::UsdGenGraphDescBuildOptions options;
                    // Hydra samples are relative to the current stage frame:
                    // keep offset zero, and gate reuse with the absolute
                    // packet frame separately. Passing packet.frame as the
                    // offset here would sample at twice the current frame.
                    if (!packet.fullPopulation && known != catalog->members.end() &&
                        known->frame == packet.frame) {
                        options.reuseNodes = true;
                        options.previousCache = known->cache;
                        for (auto const& dirty : packet.dirtied)
                            options.dirtyPrimPaths.push_back(dirty.primPath);
                    }
                    auto result = ::usdGenImaging::CaptureGraphDescFromHydra(
                        recorder, captured.description, options);
                    captured.desc = std::make_shared<const Desc>(std::move(result.desc));
                    captured.cache = std::move(result.cache);
                    captured.dependencies = recorder.Dependencies(*captured.desc);
                    packet.inputs.push_back(std::move(captured));
                    continue; // never adopt nested roots under a groom
                }
                auto children = input->GetChildPrimPaths(path);
                stack.insert(stack.end(), children.begin(), children.end());
            }
        }
    } catch (...) {
        packet.failed = true;
        packet.inputs.clear();
        state->Post([state, packet=std::move(packet)] { state->Apply(packet); });
        throw;
    }
    state->Post([state, packet=std::move(packet)] {
        try { state->Apply(packet); }
        catch (...) {
            // Required ownership/hold updates are not transactionally
            // recoverable after an allocation/framework failure yet.
            // Never disguise partial mutation as a completed ingress.
            std::terminate();
        }
    });
    // Without asyncAllow, the upstream observer call is the only legal
    // frontend delivery point.  Wait through causal attach/cook/publication
    // work, then send from this caller rather than the owner worker.
    if (!state->asyncAllowed.load(std::memory_order_acquire))
        _DrainPublications(true);
}
void UsdGenGroomSceneIndex::_PrimsAdded(HdSceneIndexBase const&, Added const& entries) {
    _Ingress packet; packet.added = entries; _CaptureAndSubmit(std::move(packet));
}
void UsdGenGroomSceneIndex::_PrimsRemoved(HdSceneIndexBase const&, Removed const& entries) {
    _Ingress packet; packet.removed = entries; _CaptureAndSubmit(std::move(packet));
}
void UsdGenGroomSceneIndex::_PrimsDirtied(HdSceneIndexBase const&, Dirtied const& entries) {
    _Ingress packet; packet.dirtied = entries; _CaptureAndSubmit(std::move(packet));
}
void UsdGenGroomSceneIndex::_PrimsRenamed(HdSceneIndexBase const& sender,
    HdSceneIndexObserver::RenamedPrimEntries const& entries) {
    _Ingress packet;
    HdSceneIndexObserver::ConvertPrimsRenamedToRemovedAndAdded(sender, entries,
        &packet.removed, &packet.added);
    _CaptureAndSubmit(std::move(packet));
}

void UsdGenGroomSceneIndex::_SystemMessage(
    TfToken const& messageType, HdDataSourceBaseHandle const&) {
    if (messageType == HdSystemMessageTokens->asyncAllow) {
        _state->asyncAllowed.store(true, std::memory_order_release);
        return;
    }
    if (messageType == HdSystemMessageTokens->asyncPoll &&
        _state->asyncAllowed.load(std::memory_order_acquire)) {
        // Poll is a render-thread flush only: it must not await owner work or
        // start cooking.  The engine's temporary observer sees these notices.
        auto live = TfCreateRefPtrFromProtectedWeakPtr(_state->recipient);
        if (!live) return;
        _DrainPublications(false);
    }
}

HdSceneIndexPrim UsdGenGroomSceneIndex::GetPrim(SdfPath const& path) const {
    auto snapshot = _state->VisibleSnapshot();
    auto input = _GetInputSceneIndex();
    auto authored = input ? input->GetPrim(path) : HdSceneIndexPrim();
    // Authored namespace collisions win, including authored tile paths.
    if (authored.dataSource || !authored.primType.IsEmpty()) {
        if (authored.primType.IsEmpty() && IsGroom(TypeName(authored)))
            authored.primType = TypeName(authored);
        return authored;
    }
    for (auto const& g : snapshot->members) {
        if (path == g.root) return {TfToken("UsdGenGroom"), {}};
        if (path == RenderPath(g.description)) return {TfToken("scope"), HdRetainedContainerDataSource::New()};
        auto tile = g.tiles->find(path);
        if (tile != g.tiles->end()) return {TfToken("basisCurves"), tile->second};
    }
    return {};
}
SdfPathVector UsdGenGroomSceneIndex::GetChildPrimPaths(SdfPath const& path) const {
    auto snapshot = _state->VisibleSnapshot();
    auto input = _GetInputSceneIndex();
    auto result = input ? input->GetChildPrimPaths(path) : SdfPathVector();
    auto append = [&](SdfPath const& child) {
        if (std::find(result.begin(), result.end(), child) == result.end()) result.push_back(child);
    };
    for (auto const& g : snapshot->members) {
        if (path == g.description) append(RenderPath(g.description));
        if (path == RenderPath(g.description))
            for (auto const& tile : *g.tiles) append(tile.first);
    }
    return result;
}
int64_t UsdGenGroomSceneIndex::_TestPublishedGeneration(SdfPath const& root) const {
    auto snapshot = _state->VisibleSnapshot();
    for (auto const& g : snapshot->members) if (g.root == root) return g.generation;
    return -1;
}
size_t UsdGenGroomSceneIndex::_TestPublishedTileCount(SdfPath const& root) const {
    auto snapshot = _state->VisibleSnapshot();
    for (auto const& g : snapshot->members) if (g.root == root) return g.tiles->size();
    return 0;
}

PXR_NAMESPACE_CLOSE_SCOPE
