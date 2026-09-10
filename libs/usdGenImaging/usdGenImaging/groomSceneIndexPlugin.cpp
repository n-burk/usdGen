// usdGenGroom renderer-level scene index plugin.
#include "usdGenImaging/groomSceneIndexPlugin.h"

#include "usdGenImaging/usdGenImagingSession.h"
#include "usdGenImaging/usdGenDirtyRouter.h"
#include "usdGenImaging/usdGenEngineBridge.h"
#include "usdGenImaging/usdGenTilePublisher.h"
#include "usdGenImaging/usdGenGraphDescBuilder.h"
#include "usdGenImaging/usdGenTokens.h"

#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h"
#include "pxr/usdImaging/usdImaging/usdPrimInfoSchema.h"
#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/refPtr.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/token.h"

#include "usdGenImaging/usdGenEnable.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// Kill switch (ADR §5.1): the registry consults _IsEnabled per append, so a
// disabled plugin returns its input unchanged and drops out of the chain
// without editing plugInfo. Shared with the metadata plugin via usdGenEnable.h
// (single TF_DEFINE_ENV_SETTING lives in this translation unit).
TF_DEFINE_ENV_SETTING(
    USDGEN_ENABLE, true,
    "Enable the usdGen scene index plugins (false disables both the groom "
    "resolution plugin and the metadata-only plugin).");

TF_REGISTRY_FUNCTION(TfType) {
    HdSceneIndexPluginRegistry::Define<UsdGenGroomSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),
        TfToken("UsdGenGroomSceneIndexPlugin"),
        /*inputArgs      =*/ nullptr,
        /*insertionPhase =*/ 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

HdSceneIndexBaseRefPtr
UsdGenGroomSceneIndexPlugin::_AppendSceneIndex(
    const HdSceneIndexBaseRefPtr &inputScene,
    const HdContainerDataSourceHandle &inputArgs)
{
    TF_UNUSED(inputArgs);
    // M0: pass-through node so the chain position is observable. M1 wraps the
    // input in the real UsdGenGroomSceneIndex (evaluator + tile publisher).
    return UsdGenGroomSceneIndex::New(inputScene);
}

bool
UsdGenGroomSceneIndexPlugin::_IsEnabled(
    const HdContainerDataSourceHandle &inputArgs) const
{
    TF_UNUSED(inputArgs);
    return TfGetEnvSetting(USDGEN_ENABLE);
}

// ---------------------------------------------------------------------------
// M1 chunk A: UsdGenGroomSceneIndex filtering half — construction,
// population scaffolding, and read paths (06 §3.1, §3.4, §3.5).
// Chunks B/C (notice routing, commit, publish) follow in this file.
//
// Namespace note: the session key/handle/store live in the GLOBAL
// ::usdGenImaging namespace (usdGenImagingSession.h), while the router,
// bridge and SdfPathHash live in pxr::usdGenImaging. This file sits inside
// PXR_NAMESPACE, so session types are spelled ::usdGenImaging:: and router
// types usdGenImaging::.
// ---------------------------------------------------------------------------

// Per-UsdGenGroom state: session attachment plus the router/bridge pair that
// chunks B/C drive. Published tile sources land here (chunk C).
struct UsdGenGroomSceneIndex::_Groom {
    using PublishedMap = std::unordered_map<SdfPath, HdContainerDataSourceHandle,
                                            usdGenImaging::SdfPathHash>;
    using PublishedSnapshot = std::shared_ptr<PublishedMap const>;
    SdfPath groomRoot;
    SdfPath description;
    ::usdGenImaging::UsdGenSessionKey key;
    ::usdGenImaging::UsdGenSessionHandle session;
    usdGenImaging::UsdGenDirtyRouter router;
    usdGenImaging::UsdGenEngineBridge bridge;
    int republishToken = -1;
    // P0 race fix: readers snapshot-and-release (atomic_load, no mutex on
    // the read path); _Republish swaps a fresh map under _stateMutex.
    // C++17 free-function atomic shared_ptr idiom (R20, as the generation
    // handoff in generationStore.cpp).
    PublishedSnapshot published;
};

UsdGenGroomSceneIndex::UsdGenGroomSceneIndex(
    HdSceneIndexBaseRefPtr const &inputScene,
    int renderInstanceId)
    : HdSingleInputFilteringSceneIndexBase(inputScene)
    , _pruned(HdSiExtComputationPrimvarPruningSceneIndex::New(inputScene))
    , _renderInstanceId(renderInstanceId)
    , _lastGlobalFrame(0.0)
    , _haveGlobalFrame(false)
{
    _populated.clear();
    _frameLocators.insert(HdSceneGlobalsSchema::GetCurrentFrameLocator());
}

UsdGenGroomSceneIndex::~UsdGenGroomSceneIndex()
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    for (auto &g : _grooms) {
        if (!g) continue;
        if (g->republishToken >= 0 && g->session) {
            g->session->UnregisterRepublishCallback(g->republishToken);
            g->republishToken = -1;
        }
        if (g->session) {
            ::usdGenImaging::UsdGenSessionStore::GetInstance().Detach(g->key);
        }
    }
}

HdSceneIndexBaseRefPtr
UsdGenGroomSceneIndex::New(
    HdSceneIndexBaseRefPtr const &inputScene,
    int renderInstanceId)
{
    return TfCreateRefPtr(new UsdGenGroomSceneIndex(inputScene, renderInstanceId));
}

void
UsdGenGroomSceneIndex::_PopulateFromInput()
{
    if (_populated.test_and_set()) return;
    _ScanInputForGrooms();
    _AdoptPending();
}
namespace {
// USD type name from __usdPrimInfo (carried regardless of adapter subprim
// type). Observed: terminal index serves primType == "" with
// __usdPrimInfo/typeName == "UsdGenGroom" intact (probeGroomType).
TfToken
_UsdTypeName(HdSceneIndexPrim const &prim)
{
    if (!prim.dataSource) return TfToken();
    UsdImagingUsdPrimInfoSchema info =
        UsdImagingUsdPrimInfoSchema::GetFromParent(prim.dataSource);
    if (!info) return TfToken();
    HdTokenDataSourceHandle typeName = info.GetTypeName();
    if (!typeName) return TfToken();
    VtValue v = typeName->GetValue(0);
    if (!v.IsHolding<TfToken>()) return TfToken();
    return v.UncheckedGet<TfToken>();
}
// Root-set test (06 §3.6): the groom root set is UsdGenGroom OR
// UsdGenDescription. Our adapters return the empty subprim type (06 §2.1),
// so usdGen prims MEASURED-arrive with primType == "" (06 §2.7 probe2) —
// string-matching the Hydra type misses them on BOTH paths. Fall back to
// __usdPrimInfo/typeName, which carries the USD type name regardless.
bool
_IsGroomRootPrim(HdSceneIndexPrim const &prim)
{
    if (prim.primType == TfToken("UsdGenGroom") ||
        prim.primType == TfToken("UsdGenDescription")) {
        return true;
    }
    TfToken const t = _UsdTypeName(prim);
    return t == TfToken("UsdGenGroom") || t == TfToken("UsdGenDescription");
}
}  // namespace
void
UsdGenGroomSceneIndex::_ScanInputForGrooms() const
{
    HdSceneIndexBaseRefPtr input = _GetInputSceneIndex();
    if (!input) return;
    std::vector<SdfPath> stack{SdfPath::AbsoluteRootPath()};
    while (!stack.empty()) {
        SdfPath path = stack.back();
        stack.pop_back();
        if (path != SdfPath::AbsoluteRootPath()) {
            HdSceneIndexPrim prim = input->GetPrim(path);
            if (_IsGroomRootPrim(prim)) {
                _AdoptGroom(path);
                continue;  // never descend below a groom root (06 §3.6)
            }
        }
        SdfPathVector children = input->GetChildPrimPaths(path);
        for (SdfPath const &c : children) stack.push_back(c);
    }
}

void
UsdGenGroomSceneIndex::_AdoptGroom(SdfPath const &groomRoot) const
{
    auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
    HdSceneIndexBaseRefPtr input = self->_GetInputSceneIndex();
    std::lock_guard<std::mutex> lock(self->_stateMutex);
    for (auto &g : self->_grooms) {
        if (g && g->groomRoot == groomRoot) {
            self->_ResolveDescriptionLocked(*g, input);
            return;
        }
    }
    auto groom = std::make_unique<_Groom>();
    groom->groomRoot = groomRoot;
    groom->description = groomRoot;
    groom->key.groomRoot = groomRoot;
    groom->key.sessionId = std::string();
    groom->key.stage =
        ::usdGenImaging::UsdGenSessionStore::FindGroomStage(groomRoot);
    groom->session =
        ::usdGenImaging::UsdGenSessionStore::GetInstance().Attach(groom->key);
    self->_ResolveDescriptionLocked(*groom, input);
    self->_grooms.push_back(std::move(groom));
}
// Description resolution: the session description is the UsdGenDescription
// child under the groom root (fixture: /groomA/descA). Resolved eagerly
// when the child is already present and re-resolved on later arrivals, so
// both notice orderings converge. Falls back to the groom root itself.
void
UsdGenGroomSceneIndex::_ResolveDescriptionLocked(
    _Groom &groom, HdSceneIndexBaseRefPtr const &input) const
{
    if (!input) return;
    if (groom.description != groom.groomRoot) return;  // already resolved
    SdfPathVector children = input->GetChildPrimPaths(groom.groomRoot);
    for (SdfPath const &c : children) {
        HdSceneIndexPrim prim = input->GetPrim(c);
        TfToken const t = prim.primType.IsEmpty() ? _UsdTypeName(prim)
                                                  : prim.primType;
        if (t == TfToken("UsdGenDescription")) {
            groom.description = c;
            return;
        }
    }
}

void
UsdGenGroomSceneIndex::_AdoptPending() const
{
    std::vector<HdSceneIndexObserver::AddedPrimEntry> added;
    std::vector<SdfPath> removed;
    {
        auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
        std::lock_guard<std::mutex> lock(self->_stateMutex);
        added.swap(self->_pendingAdd);
        removed.swap(self->_pendingRemove);
    }
    HdSceneIndexBaseRefPtr input = _GetInputSceneIndex();
    for (auto const &e : added) {
        TfToken t = e.primType;
        if (t.IsEmpty() && input) t = _UsdTypeName(input->GetPrim(e.primPath));
        if (t == TfToken("UsdGenGroom")) {
            _AdoptGroom(e.primPath);
        } else if (t == TfToken("UsdGenDescription")) {
            // A description arriving after its groom: upgrade that groom's
            // resolved description (ordering-proof both ways).
            auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
            std::lock_guard<std::mutex> lock(self->_stateMutex);
            for (auto &g : self->_grooms) {
                if (g && g->groomRoot == e.primPath.GetParentPath()) {
                    self->_ResolveDescriptionLocked(*g, input);
                }
            }
        }
    }
    for (SdfPath const &p : removed) _ForgetGroomsUnder(p);
}
void
UsdGenGroomSceneIndex::_ForgetGroomsUnder(SdfPath const &path) const
{
    auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
    std::lock_guard<std::mutex> lock(self->_stateMutex);
    for (auto &g : self->_grooms) {
        if (!g) continue;
        if (g->groomRoot == path || g->groomRoot.HasPrefix(path)) {
            if (g->republishToken >= 0 && g->session) {
                g->session->UnregisterRepublishCallback(g->republishToken);
                g->republishToken = -1;
            }
            if (g->session) {
                ::usdGenImaging::UsdGenSessionStore::GetInstance().Detach(
                    g->key);
                g->session = ::usdGenImaging::UsdGenSessionHandle();
            }
            g.reset();
        }
    }
}

bool
UsdGenGroomSceneIndex::_IsOwned(SdfPath const &path) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    for (auto const &g : _grooms) {
        if (!g || !g->session) continue;
        SdfPath const render = g->description.AppendChild(
            usdGenImaging::UsdGenRenderNamespaceToken());
        if (path == render || path.HasPrefix(render)) return true;
    }
    return false;
}

bool
UsdGenGroomSceneIndex::_OwnsDescription(SdfPath const &maybeDescPath) const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    for (auto const &g : _grooms) {
        if (g && g->description == maybeDescPath) return true;
    }
    return false;
}

HdSceneIndexPrim
UsdGenGroomSceneIndex::GetPrim(SdfPath const &primPath) const
{
    // I7/S17: snapshot loads only — never commits, never cooks, never
    // locks the engine.
    const_cast<UsdGenGroomSceneIndex *>(this)->_PopulateFromInput();
    {
        // Snapshot-and-release: atomic_load the per-groom published map;
        // all reads below run lock-free against the snapshot.
        std::vector<_Groom::PublishedSnapshot> snaps;
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            snaps.reserve(_grooms.size());
            for (auto const &g : _grooms) {
                if (g) snaps.push_back(std::atomic_load(&g->published));
            }
        }
        for (auto const &snap : snaps) {
            if (!snap) continue;
            auto it = snap->find(primPath);
            if (it != snap->end() && it->second) {
                return HdSceneIndexPrim{TfToken("basisCurves"), it->second};
            }
        }
    }
    HdSceneIndexBaseRefPtr input = _GetInputSceneIndex();
    HdSceneIndexPrim out = input ? input->GetPrim(primPath) : HdSceneIndexPrim();
    // Adopted groom roots: the terminal index serves primType == "" (our
    // adapters return the empty subprim type, 06 §2.1). SI-6 pins the
    // UsdGenGroom type on the root; the USD type name rides in
    // __usdPrimInfo/typeName (observed), so report it. Pass-through
    // otherwise — never invent types for non-groom prims.
    if (!out.primType.IsEmpty()) return out;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (auto const &g : _grooms) {
            if (g && g->groomRoot == primPath) {
                out.primType = TfToken("UsdGenGroom");
                return out;
            }
        }
    }
    return out;
}

SdfPathVector
UsdGenGroomSceneIndex::GetChildPrimPaths(SdfPath const &path) const
{
    const_cast<UsdGenGroomSceneIndex *>(this)->_PopulateFromInput();
    HdSceneIndexBaseRefPtr input = _GetInputSceneIndex();
    SdfPathVector result =
        input ? input->GetChildPrimPaths(path) : SdfPathVector();
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (auto const &g : _grooms) {
            if (!g) continue;
            if (path == g->description) {
                // Authored prims win: serve __usdGenRender only where the
                // input has no prim at that path (06 §3.4).
                SdfPath const render = g->description.AppendChild(
                    usdGenImaging::UsdGenRenderNamespaceToken());
                if (std::find(result.begin(), result.end(), render) ==
                    result.end()) {
                    result.push_back(render);
                }
            } else if (path == g->description.AppendChild(
                           usdGenImaging::UsdGenRenderNamespaceToken())) {
                _Groom::PublishedSnapshot snap =
                    std::atomic_load(&g->published);
                if (!snap) continue;
                for (auto const &kv : *snap) {
                    if (std::find(result.begin(), result.end(), kv.first) ==
                        result.end()) {
                        result.push_back(kv.first);
                    }
                }
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Chunk B: notice routing + commit triggers (06 §3.2, §3.3, §3.9).
// Shape per handler: route into UsdGenPendingDirty (table lookups, never a
// cook) -> forward input entries downstream UNCHANGED, immediately -> commit
// on trigger (b)/(c) and emit our own notices after the forward.
// ---------------------------------------------------------------------------
namespace {
bool
_IsFrameDirtyEntry(HdSceneIndexObserver::DirtiedPrimEntry const &e)
{
    if (e.primPath != SdfPath::AbsoluteRootPath()) return false;
    return e.dirtyLocators.Intersects(
        HdSceneGlobalsSchema::GetCurrentFrameLocator());
}
}  // namespace
void
UsdGenGroomSceneIndex::_PrimsAdded(
    HdSceneIndexBase const &,
    HdSceneIndexObserver::AddedPrimEntries const &entries)
{
    _PopulateFromInput();
    bool structural = false;
    HdSceneIndexBaseRefPtr input = _GetInputSceneIndex();
    for (auto const &e : entries) {
        TfToken t = e.primType;
        if (t.IsEmpty() && input) t = _UsdTypeName(input->GetPrim(e.primPath));
        if (t == TfToken("UsdGenGroom")) {
            _AdoptGroom(e.primPath);
            structural = true;
        } else if (t == TfToken("UsdGenDescription") ||
                   _OwnsDescription(e.primPath)) {
            // Description arrival: resolve/upgrade the parent groom's
            // description; structural — the render-ns split appears.
            auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
            std::lock_guard<std::mutex> lock(self->_stateMutex);
            for (auto &g : self->_grooms) {
                if (g && g->groomRoot == e.primPath.GetParentPath()) {
                    self->_ResolveDescriptionLocked(*g, input);
                }
            }
            structural = true;
        }
    }
    _SendPrimsAdded(entries);
    if (structural) {
        _CommitNow(usdGen::UsdGenCommitReason::NoticeBatchEnd,
                   /*republishNeeded=*/true);
    }
}
void
UsdGenGroomSceneIndex::_PrimsRemoved(
    HdSceneIndexBase const &,
    HdSceneIndexObserver::RemovedPrimEntries const &entries)
{
    _PopulateFromInput();
    for (auto const &e : entries) _ForgetGroomsUnder(e.primPath);
    _SendPrimsRemoved(entries);
    if (!entries.empty()) {
        _CommitNow(usdGen::UsdGenCommitReason::NoticeBatchEnd,
                   /*republishNeeded=*/true);
    }
}
void
UsdGenGroomSceneIndex::_PrimsDirtied(
    HdSceneIndexBase const &,
    HdSceneIndexObserver::DirtiedPrimEntries const &entries)
{
    _PopulateFromInput();
    bool frameDirty = false;
    bool routedDirty = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (auto const &e : entries) {
            // NOTE: _stateMutex is already held here — do NOT call _IsOwned
            // (it locks). Inline the render-namespace check instead.
            bool owned = false;
            for (auto const &g : _grooms) {
                if (!g || !g->session) continue;
                SdfPath const render = g->description.AppendChild(
                    usdGenImaging::UsdGenRenderNamespaceToken());
                if (e.primPath == render ||
                    e.primPath.HasPrefix(render)) {
                    owned = true;
                    break;
                }
            }
            if (owned) continue;
            if (_IsFrameDirtyEntry(e)) {
                frameDirty = true;
                continue;
            }
            for (auto const &g : _grooms) {
                if (!g || !g->session || !g->session->Engine()) continue;
                usdGen::UsdGenPendingDirty pending;
                HdSceneIndexObserver::DirtiedPrimEntries single;
                single.push_back(e);
                g->router.Route(single, &pending);
                if (pending.Any()) {
                    g->session->Engine()->AccumulateDirty(std::move(pending));
                    routedDirty = true;
                }
            }
        }
    }
    _SendPrimsDirtied(entries);
    if (!frameDirty && !routedDirty) return;
    // Trigger (b) only when no app driver owns time on ANY live session;
    bool anyAppDriver = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (auto const &g : _grooms) {
            if (g && g->session && g->session->HasAppDriver()) {
                anyAppDriver = true;
                break;
            }
        }
    }
    _CommitNow(frameDirty && !anyAppDriver
                   ? usdGen::UsdGenCommitReason::SceneFrameDirty
                   : usdGen::UsdGenCommitReason::NoticeBatchEnd,
               /*republishNeeded=*/true);
}
void
UsdGenGroomSceneIndex::_PrimsRenamed(
    HdSceneIndexBase const &sender,
    HdSceneIndexObserver::RenamedPrimEntries const &entries)
{
    HdSceneIndexObserver::RemovedPrimEntries removed;
    HdSceneIndexObserver::AddedPrimEntries added;
    HdSceneIndexObserver::ConvertPrimsRenamedToRemovedAndAdded(
        sender, entries, &removed, &added);
    if (!removed.empty()) _PrimsRemoved(sender, removed);
    if (!added.empty()) _PrimsAdded(sender, added);
}
void
UsdGenGroomSceneIndex::_Forward(
    std::vector<HdSceneIndexObserver::AddedPrimEntry> const &added,
    std::vector<HdSceneIndexObserver::RemovedPrimEntry> const &removed,
    std::vector<HdSceneIndexObserver::DirtiedPrimEntry> const &dirtied) const
{
    auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
    if (!added.empty()) {
        HdSceneIndexObserver::AddedPrimEntries e;
        for (auto const &a : added) e.push_back(a);
        self->_SendPrimsAdded(e);
    }
    if (!removed.empty()) {
        HdSceneIndexObserver::RemovedPrimEntries e;
        for (auto const &r : removed) e.push_back(r);
        self->_SendPrimsRemoved(e);
    }
    if (!dirtied.empty()) {
        HdSceneIndexObserver::DirtiedPrimEntries e;
        for (auto const &d : dirtied) e.push_back(d);
        self->_SendPrimsDirtied(e);
    }
}
void
UsdGenGroomSceneIndex::_RouteFrameDirties(
    std::vector<HdSceneIndexObserver::DirtiedPrimEntry> const &entries) const
{
    auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
    HdSceneIndexObserver::DirtiedPrimEntries e;
    for (auto const &d : entries) e.push_back(d);
    self->_PrimsDirtied(*_GetInputSceneIndex(), e);
}
void
UsdGenGroomSceneIndex::_ReplaySurfaceDirty() const
{
    std::lock_guard<std::mutex> lock(_stateMutex);
    for (auto const &g : _grooms) {
        if (!g || !g->session || !g->session->Engine()) continue;
        usdGen::UsdGenPendingDirty pending;
        pending.surfaceTopology = true;
        g->session->Engine()->AccumulateDirty(std::move(pending));
    }
}
void
UsdGenGroomSceneIndex::_CommitNow(
    usdGen::UsdGenCommitReason reason, bool republishNeeded) const
{
    auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
    {
        std::lock_guard<std::mutex> lock(self->_stateMutex);
        for (auto &g : self->_grooms) {
            if (!g || !g->session) continue;
            g->bridge.GateCommit(g->description, "_CommitNow");
            g->session->Commit(reason);
            if (g->session->Engine()) {
                g->router.Rebuild(g->session->Engine()->Graph());
            }
        }
    }
    if (republishNeeded) {
        for (auto &g : self->_grooms) {
            if (g && g->session) self->_Republish(*g, reason);
        }
    }
}
void
UsdGenGroomSceneIndex::_Republish(
    UsdGenGroomSceneIndex::_Groom &groom, usdGen::UsdGenCommitReason) const
{
    if (!groom.session || !groom.session->Engine()) return;
    usdGen::UsdGenGenerationConstPtr gen =
        groom.session->Engine()->Generation();
    usdGen::UsdGenDirtyReport const &report =
        groom.session->Engine()->LastReport();
    auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
    std::vector<HdSceneIndexObserver::RemovedPrimEntry> removed;
    std::vector<HdSceneIndexObserver::AddedPrimEntry> added;
    std::vector<HdSceneIndexObserver::DirtiedPrimEntry> dirtied;
    // Copy-on-write under _stateMutex: clone the current snapshot, apply
    // the report diff, build new tile sources, then atomic_store the fresh
    // map. Readers holding older snapshots keep reading them lock-free.
    _Groom::PublishedSnapshot next;
    {
        std::lock_guard<std::mutex> lock(self->_stateMutex);
        _Groom::PublishedMap fresh;
        if (_Groom::PublishedSnapshot cur =
                std::atomic_load(&groom.published)) {
            fresh = *cur;
        }
        for (usdGen::UsdGenTileDirty const &td : report.tiles) {
            if (td.removed) {
                removed.emplace_back(td.primPath);
                fresh.erase(td.primPath);
            } else if (td.added) {
                added.emplace_back(td.primPath, TfToken("basisCurves"));
            }
        }
        if (gen) {
            int64_t const snapId = gen->id;
            for (usdGen::UsdGenTilePublication const &tile : gen->tiles) {
                // Every tile in the snapshot is (re)built stamped with the
                // snapshot id — including carried-over payloads, whose data
                // IS this snapshot's data. Readers comparing stamps across
                // tiles therefore always agree within one swap.
                fresh[tile.primPath] =
                    ::usdGenImaging::UsdGenTilePublisher::BuildTileDataSource(
                        tile, snapId);
                bool announced = false;
                for (auto const &a : added) {
                    if (a.primPath == tile.primPath) {
                        announced = true;
                        break;
                    }
                }
                bool member = false;
                if (_Groom::PublishedSnapshot cur =
                        std::atomic_load(&groom.published)) {
                    member = cur->find(tile.primPath) != cur->end();
                }
                if (!announced && !member) {
                    added.emplace_back(tile.primPath, TfToken("basisCurves"));
                }
            }
        }
        next = std::make_shared<_Groom::PublishedMap const>(std::move(fresh));
        std::atomic_store(&groom.published, next);
    }
    usdGen::UsdGenGeneration emptyGen;
    usdGen::UsdGenGeneration const &genRef = gen ? *gen : emptyGen;
    for (usdGen::UsdGenTileDirty const &td : report.tiles) {
        if (td.removed || td.added) continue;
        if (td.pointsDirty || td.widthsDirty || td.xformDirty ||
            !td.newPrimvars.empty() || !td.dirtyPrimvars.empty()) {
            dirtied.emplace_back(
                td.primPath, _DirtiedLocatorsFor(td, genRef, 0, false, false));
        }
    }
    self->_Forward(added, removed, dirtied);
}
HdDataSourceLocatorSet
UsdGenGroomSceneIndex::_DirtiedLocatorsFor(
    usdGen::UsdGenTileDirty const &reportTile,
    usdGen::UsdGenGeneration const &, size_t,
    bool, bool)
{
    ::usdGenImaging::UsdGenTilePublisher::TileNotices n =
        ::usdGenImaging::UsdGenTilePublisher::NoticesFor(reportTile);
    HdDataSourceLocatorSet out;
    for (HdDataSourceLocator const &l : n.all()) out.insert(l);
    return out;
}
PXR_NAMESPACE_CLOSE_SCOPE
