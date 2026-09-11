// usdGenGroom renderer-level scene index plugin.
#include "usdGenImaging/groomSceneIndexPlugin.h"

#include "usdGenImaging/usdGenImagingSession.h"
#include "usdGenImaging/usdGenDirtyRouter.h"
#include "usdGenImaging/usdGenEngineBridge.h"
#include "usdGenImaging/usdGenTilePublisher.h"
#include "usdGenImaging/testHook.h"
#include "usdGenImaging/usdGenGraphDescBuilder.h"
#include "usdGenImaging/usdGenTokens.h"

#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h"
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
    // All staging is sourced from the post-deformation Hydra input.
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
    , _renderInstanceId(static_cast<uint32_t>(renderInstanceId))
    , _lastGlobalFrame(0.0)
    , _haveGlobalFrame(false)
{
    // Explicit host identities occupy the low 32 bits. Unspecified
    // identities are unique for the index lifetime and never reuse a raw
    // scene/stage address (two stages may author the same groom path).
    static std::atomic<uint64_t> nextInstance{uint64_t(1) << 32};
    if (renderInstanceId == 0) _renderInstanceId = nextInstance.fetch_add(1);
    _populated.clear();
    _frameLocators.insert(HdSceneGlobalsSchema::GetCurrentFrameLocator());
    UsdGenImagingTestHook::_RegisterIndex(this);
}
int64_t
UsdGenGroomSceneIndex::_TestPublishedGeneration(SdfPath const &groom) const
{
    // Lock-free past the groom lookup: the published map is atomic_load'ed,
    // exactly like GetPrim. Returns the max `generation` stamp, or -1 when
    // the groom is unknown / unpublished.
    _Groom::PublishedSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (auto const &g : _grooms) {
            if (g && g->groomRoot == groom) {
                snap = std::atomic_load(&g->published);
                break;
            }
        }
    }
    if (!snap || snap->empty()) return -1;
    int64_t best = -1;
    HdDataSourceLocator const genLoc(TfToken("generation"));
    for (auto const &kv : *snap) {
        if (!kv.second) continue;
        HdDataSourceBaseHandle found =
            HdContainerDataSource::Get(kv.second, genLoc);
        HdSampledDataSourceHandle sampled =
            HdSampledDataSource::Cast(found);
        if (!sampled) continue;
        VtValue v = sampled->GetValue(0);
        if (v.IsHolding<int>()) {
            best = std::max(best, int64_t(v.UncheckedGet<int>()));
        }
    }
    return best;
}

size_t
UsdGenGroomSceneIndex::_TestPublishedTileCount(SdfPath const &groom) const
{
    _Groom::PublishedSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (auto const &g : _grooms) {
            if (g && g->groomRoot == groom) {
                snap = std::atomic_load(&g->published);
                break;
            }
        }
    }
    return snap ? snap->size() : 0;
}

UsdGenGroomSceneIndex::~UsdGenGroomSceneIndex()
{
    UsdGenImagingTestHook::_UnregisterIndex(this);
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
    // Read the transported metadata as Hydra data, just like the graph
    // builder; no UsdImaging schema wrapper or stage API is needed here.
    HdTokenDataSourceHandle typeName = HdTokenDataSource::Cast(
        HdContainerDataSource::Get(prim.dataSource,
            HdDataSourceLocator(TfToken("__usdPrimInfo"), TfToken("typeName"))));
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
}  // namespace (groom-root helpers)
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
            if (self->_ResolveDescriptionLocked(*g, input) && g->session) {
                g->session->MarkNeedsDesc();
            }
            return;
        }
    }
    auto groom = std::make_shared<_Groom>();
    groom->groomRoot = groomRoot;
    groom->description = groomRoot;
    groom->key.groomRoot = groomRoot;
    groom->key.renderInstanceId = self->_renderInstanceId;
    if (input) {
        auto data = input->GetPrim(groomRoot).dataSource;
        // The mapped adapter overlays relative property locators at the
        // prim root: usdGen:sessionId is Hydra's `sessionId`.
        auto id = HdStringDataSource::Cast(HdContainerDataSource::Get(
            data, HdDataSourceLocator(TfToken("sessionId"))));
        if (id) groom->key.sessionId = id->GetTypedValue(0.0f);
    }
    // (Review 1) Resolve the description BEFORE attach/first staging:
    // _ScanInputForGrooms stops descending at the groom root, so no later
    // entry repairs this value — staging from the groom root instead of its
    // UsdGenDescription child builds the wrong desc.
    self->_ResolveDescriptionLocked(*groom, input);
    groom->session =
        ::usdGenImaging::UsdGenSessionStore::GetInstance().Attach(groom->key);
    // Lifetime: callback holds WEAK groom + WEAK index refs and lock()s at
    // invocation (expired ⇒ no-op). No raw self/slot captures, no
    // alive-flag (refcount owns lifetime). Lock discipline: the callback
    // resolves the live slot via _RepublishByRoot (locks itself) — never
    // nested under a held _stateMutex.
    if (groom->session) {
        groom->session->MarkNeedsDesc();
        std::weak_ptr<_Groom> weakGroom(groom);
        // HdSceneIndexBase is TF_DECLARE_WEAK_AND_REF_PTRS (sceneIndex.h),
        // so the weak type is HdSceneIndexBasePtr (TfWeakPtr facade).
        HdSceneIndexBasePtr weakSelf = TfCreateWeakPtr(self);
        groom->republishToken = groom->session->RegisterRepublishCallback(
            [weakSelf, weakGroom,
             groomRoot](::usdGenImaging::UsdGenImagingSession::CommitPayload const &payload) {
                // Lifetime: weak groom + weak index, lock() both at
                // invocation (expired => no-op). Lock discipline: resolve
                // the live slot via _RepublishByRoot (locks itself) —
                // never nested under a held _stateMutex. No raw
                // self/slot captures, no alive-flag.
                std::shared_ptr<_Groom> groomLocked = weakGroom.lock();
                HdSceneIndexBaseRefPtr selfLocked =
                    TfCreateRefPtrFromProtectedWeakPtr(weakSelf);
                UsdGenGroomSceneIndex const *index =
                    dynamic_cast<UsdGenGroomSceneIndex const *>(
                        selfLocked ? selfLocked.operator->() : nullptr);
                if (!groomLocked || !index) return;
                // (Review 3) payload travels — never reread live
                // Generation()/LastReport(). Unpublished commits no-op.
                if (!payload.published) return;
                index->_RepublishByRoot(groomRoot, payload);
            });
    }
    self->_grooms.push_back(std::move(groom));
}
// child under the groom root (fixture: /groomA/descA). Resolved eagerly
// when the child is already present and re-resolved on later arrivals, so
// both notice orderings converge. Falls back to the groom root itself.
// Returns true when the resolved description changed (caller marks the
// session descriptor-dirty so an already-compiled session re-stages).
bool
UsdGenGroomSceneIndex::_ResolveDescriptionLocked(
    _Groom &groom, HdSceneIndexBaseRefPtr const &input) const
{
    if (!input) return false;
    SdfPathVector children = input->GetChildPrimPaths(groom.groomRoot);
    for (SdfPath const &c : children) {
        HdSceneIndexPrim prim = input->GetPrim(c);
        TfToken const t = prim.primType.IsEmpty() ? _UsdTypeName(prim)
                                                  : prim.primType;
        if (t == TfToken("UsdGenDescription")) {
            if (groom.description == c) return false;  // already resolved
            groom.description = c;
            return true;
        }
    }
    return false;
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
    bool structural = false;
    for (auto const &e : added) {
        TfToken t = e.primType;
        if (t.IsEmpty() && input) t = _UsdTypeName(input->GetPrim(e.primPath));
        if (t == TfToken("UsdGenGroom")) {
            _AdoptGroom(e.primPath);
            structural = true;
        } else if (t == TfToken("UsdGenDescription")) {
            // A description arriving after its groom: upgrade that groom's
            // resolved description (ordering-proof both ways).
            auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
            std::lock_guard<std::mutex> lock(self->_stateMutex);
            for (auto &g : self->_grooms) {
                if (g && g->groomRoot == e.primPath.GetParentPath()) {
                    // (Review 1) live description change re-stages: an
                    // already-compiled session keeps its old desc unless
                    // marked descriptor-dirty here.
                    if (self->_ResolveDescriptionLocked(*g, input) &&
                        g->session) {
                        g->session->MarkNeedsDesc();
                    }
                }
            }
            structural = true;
        }
    }
    for (SdfPath const &p : removed) _ForgetGroomsUnder(p);
    if (structural) {
        // New topology arrived after attach: re-pull the desc next commit.
        std::lock_guard<std::mutex> lock(
            const_cast<UsdGenGroomSceneIndex *>(this)->_stateMutex);
        for (auto &g : const_cast<UsdGenGroomSceneIndex *>(this)->_grooms) {
            if (g && g->session) g->session->MarkNeedsDesc();
        }
    }
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
            // No alive-flag: lifetime is the shared_ptr refcount. Weak
            // callback refs lock() and expire once the entry below is
            // erased (strong Work/_RepublishByRoot refs keep a removed
            // groom alive until their own publish finishes).
            g = nullptr;
        }
    }
    self->_grooms.erase(
        std::remove(self->_grooms.begin(), self->_grooms.end(), nullptr),
        self->_grooms.end());
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
        // A newly added operator (or a resynced grouping Scope) is not in
        // the compiled router yet. Re-pull the owning hierarchy even when
        // this is not a Groom/Description arrival.
        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            for (auto const &g : _grooms) {
                if (g && g->session && e.primPath.HasPrefix(g->description)) {
                    g->session->MarkNeedsDesc();
                    structural = true;
                }
            }
        }
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
                    if (self->_ResolveDescriptionLocked(*g, input) &&
                        g->session) {
                        g->session->MarkNeedsDesc();
                    }
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
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (auto const &e : entries) {
            for (auto const &g : _grooms) {
                if (g && g->session && e.primPath.HasPrefix(g->description))
                    g->session->MarkNeedsDesc();
            }
        }
    }
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
                // Composed child-order dirties may originate at an Ops
                // Scope rather than at an operator. The aggregate is live;
                // re-pulling it reconstructs hierarchy after such edits.
                if (e.primPath.HasPrefix(g->description)) {
                    g->session->MarkNeedsDesc();
                    routedDirty = true;
                }
                usdGen::UsdGenPendingDirty pending;
                HdSceneIndexObserver::DirtiedPrimEntries single;
                single.push_back(e);
                g->router.Route(single, &pending);
                if (pending.Any()) {
                    // The current engine stores value snapshots, so an
                    // input dirty must refresh them before evaluation.
                    // Incremental descriptor refresh remains separate work.
                    g->session->MarkNeedsDesc();
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
    // Self-deadlock guard: UsdGenImagingSession::Commit fires republish
    // callbacks synchronously, and our callback (_Republish) locks
    // _stateMutex. _stateMutex is non-recursive, so Commit must NEVER run
    // under it. Snapshot (groom*, session, desc) under the lock,
    // then stage + Commit + router rebuild + _Republish unlocked.
    struct Work {
        // STRONG groom ref: lifetime by refcount — a _ForgetGroomsUnder
        // erase between snapshot and use cannot free this storage.
        std::shared_ptr<_Groom> groom;
        ::usdGenImaging::UsdGenSessionHandle session;
        SdfPath description;
        bool needsDesc = false;
    };
    auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
    std::vector<Work> work;
    {
        std::lock_guard<std::mutex> lock(self->_stateMutex);
        work.reserve(self->_grooms.size());
        for (auto &g : self->_grooms) {
            if (!g || !g->session) continue;
            Work w;
            w.groom = g;
            w.session = g->session;
            w.description = g->description;
            // Chunk-C staging decision (06 §3.7/S14): session-owned
            // ATOMIC consume (Review 2). Marks arriving after consumption
            // stay set for the next commit — no lost ReloadMaps/structural
            // marks. First-commit (no generation yet) always stages.
            w.needsDesc =
                w.session->ConsumeNeedsDesc() ||
                !w.session->Engine() ||
                w.session->Engine()->Generation() == nullptr;
            if (char const *dbg = std::getenv("USDGEN_DEBUG_STAGING")) {
                (void)dbg;
                std::printf("[staging] groom=%s needsDesc=%d hydraInput=%d\n",
                            g->groomRoot.GetText(), int(w.needsDesc),
                            int(bool(self->_pruned)));
            }
            work.push_back(std::move(w));
        }
    }
    for (Work &w : work) {
        if (!w.groom) continue;
        if (!w.session) continue;
        w.groom->bridge.GateCommit(w.description, "_CommitNow");
        bool committed = false;
        if (w.needsDesc && self->_pruned) {
            // Pull the post-deformation Hydra input, never the authored
            // stage. Hydra sample times are shutter offsets: zero samples
            // the current frame supplied by the upstream scene index.
            ::usdGenImaging::UsdGenGraphDescBuildOptions opts;
            usdGen::UsdGenGraphDesc desc =
                ::usdGenImaging::BuildGraphDescFromHydra(
                    *self->_pruned, w.description, opts);
            w.session->StageAndCommit(desc, reason);
            committed = true;
            if (std::getenv("USDGEN_DEBUG_STAGING")) {
                std::printf("[staging] staged nodes=%zu terminal=%s\n",
                            desc.nodes.size(), desc.terminal.GetText());
            }
        } else if (w.needsDesc) {
            w.session->MarkNeedsDesc(); // no input: do not lose the request
        }
        // Unlocked: session Commit fires the payload _Republish
        // callback synchronously per attached index; EACH recipient
        // rebuilds its own router from the payload BEFORE its map swap
        // (Review 4 — no initiating-index-only rebuild here, which would
        // leave store-level commits' indices stale and race synchronous
        // observers against old tables).
        if (!committed) w.session->Commit(reason);
        if (std::getenv("USDGEN_DEBUG_STAGING")) {
            usdGen::UsdGenGenerationConstPtr gg =
                w.session->Engine() ? w.session->Engine()->Generation()
                                    : usdGen::UsdGenGenerationConstPtr();
            std::printf("[staging] post-commit gen=%lld tiles=%zu nodes=%d\n",
                        (long long)(gg ? gg->id : -1),
                        gg ? gg->tiles.size() : 0u,
                        w.session->Engine()
                            ? w.session->Engine()->Graph().NodeCount()
                            : -1);
        }
    }
    // Single publication path: session->Commit fires the registered
    // _Republish callback synchronously on publish. The manual call below
    // is DELETED (double-publish: duplicate notices + double hook stamps
    // break the monotone-generation check). republishNeeded is honored by
    // the callback registration itself (adopt always registers).
    (void)republishNeeded;
}
// State-update helper (Review 5): caller must NOT hold _stateMutex.
// Applies the payload report + generation to the groom's published map and
// rebuilds the groom's router from the payload generation's graph BEFORE
// the map swap, so synchronous observers never dirty against stale tables.
// Internal locking: map swap under _stateMutex, notice forward after it
// releases. ByRoot validates membership first (own scope), then calls here
// unlocked — never nested.
void
UsdGenGroomSceneIndex::_RepublishLocked(
    _Groom &groom,
    ::usdGenImaging::UsdGenImagingSession::CommitPayload const &payload) const
{
    // Attachment fields are immutable after construction (Review 5):
    // session/key/description never mutate, so no data race with
    // _ForgetGroomsUnder (which only erases membership + detaches).
    if (!payload.published || !payload.generation) return;
    usdGen::UsdGenGenerationConstPtr const &gen = payload.generation;
    if (gen->device) {
        TF_WARN("usdGen: stock Hydra publication cannot consume CUDA device generations; retaining displayed geometry");
        return;
    }
    usdGen::UsdGenDirtyReport const &report = payload.report;
    auto *self = const_cast<UsdGenGroomSceneIndex *>(this);
    std::vector<HdSceneIndexObserver::RemovedPrimEntry> removed;
    std::vector<HdSceneIndexObserver::AddedPrimEntry> added;
    std::vector<HdSceneIndexObserver::DirtiedPrimEntry> dirtied;
    // Copy-on-write under _stateMutex: clone the current snapshot, apply
    // the report diff, build new tile sources, then atomic_store the fresh
    // map. Readers holding older snapshots keep reading them lock-free.
    // (Review 4) Rebuild THIS recipient's router from the payload
    // generation's graph BEFORE the map swap + notice forwarding.
    if (groom.session && groom.session->Engine()) {
        groom.router.Rebuild(groom.session->Engine()->Graph());
    }
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
// Root-keyed entry for the republish callback (Review 5): validates the
// EXACT shared_ptr identity under the lock (a copied old callback for a
// removed groom never routes into a newly adopted groom at the same path),
// then runs the locked state update + unlocked forward. Never nests
// _stateMutex (non-recursive).
void
UsdGenGroomSceneIndex::_RepublishByRoot(
    SdfPath const &groomRoot,
    ::usdGenImaging::UsdGenImagingSession::CommitPayload const &payload) const
{
    std::shared_ptr<_Groom> slot;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        for (auto const &g : _grooms) {
            // Exact identity: same shared_ptr instance still a member.
            // A stale weak ref that expired is already filtered by the
            // callback's lock(); this covers re-adoption at the same path.
            if (g && g->groomRoot == groomRoot) {
                slot = g;
                break;
            }
        }
    }
    if (!slot) return;  // removed after callback copy ⇒ no-op
    _RepublishLocked(*slot, payload);
    // Unlocked forward is inside _RepublishLocked's tail (computed notices
    // forwarded after the lock releases — see helper).
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
