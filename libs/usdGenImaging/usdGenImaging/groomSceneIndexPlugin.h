// usdGen groom imaging plugin — renderer-level scene index
// (06-imaging.md §2, §3; ADR §5.1 #4).
//
// Two classes:
//   * UsdGenGroomSceneIndexPlugin — the HdSceneIndexPlugin registration
//     (AppendSceneIndex rule after stageAdapter, before retiredAdapter;
//     USDGEN_ENABLE kill switch; 06 §6).
//   * UsdGenGroomSceneIndex — the filtering index itself. Chain position:
//     built AFTER the stock HdStageAdapterSceneIndex (so UsdImagingSceneIndex
//     PrimAdapters have run and the usdGen/* containers exist) and BEFORE the
//     terminal HdRetiredAdapterSceneIndexFilter. It observes its (pruned)
//     input with a private HdSceneIndexObserver, routes notices into the
//     per-groom UsdGenImagingSession / UsdGenDirtyRouter (no cook, I7/S17),
//     commits on the 06 §3.9 triggers, and publishes the generation as
//     synthetic Hydra prims <description>/__usdGenRender/tile_NNNN.
//
// GetPrim NEVER commits — it atomic_loads the latest generation (I7). The
// M0 pass-through class is gone: this index owns sessions, synthesizes and
// announces the published prim set (06 §3.4), and forwards only non-usdGen
// prims (06 §3.4.1).
//
// Both classes live in the pxr namespace so the HdSceneIndexPlugin registry
// can reference them; the heavy lifting forwards to the global
// usdGenImaging:: session/router/publisher classes.
//
// Symbol visibility follows the usdGen library convention
// (libs/usdGen/usdGen/export.h): no per-symbol export macro — usdGenImaging
// is built with default visibility.
#ifndef USDGEN_IMAGING_GROOM_SCENE_INDEX_PLUGIN_H
#define USDGEN_IMAGING_GROOM_SCENE_INDEX_PLUGIN_H

#include "pxr/pxr.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/base/tf/registryManager.h"
#include "usdGenImaging/usdGenImagingSession.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace usdGen {
class UsdGenDirtyRouter;
enum class UsdGenCommitReason : uint8_t;
struct UsdGenGeneration;
struct UsdGenTileDirty;
}  // namespace usdGen

PXR_NAMESPACE_OPEN_SCOPE

class HdSceneIndexBase;
using HdSceneIndexBaseRefPtr = TfRefPtr<HdSceneIndexBase>;

/// Filtering half: the scene index spliced into the chain.
class UsdGenGroomSceneIndex final
    : public HdSingleInputFilteringSceneIndexBase
{
public:
    static HdSceneIndexBaseRefPtr New(
        HdSceneIndexBaseRefPtr const &inputScene,
        int renderInstanceId = 0);

    // -- HdSceneIndexInterface ------------------------------------------------
    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override;
    // -- HdSceneIndexObserver (input observations; 06 §3.2) --------------------
    // HdSingleInputFilteringSceneIndexBase installs a private bridge observer
    // on the input; filter subclasses override the underscore hooks below.
    void _PrimsAdded(
        HdSceneIndexBase const &sceneIndex,
        HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(
        HdSceneIndexBase const &sceneIndex,
        HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(
        HdSceneIndexBase const &sceneIndex,
        HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;
    void _PrimsRenamed(
        HdSceneIndexBase const &sceneIndex,
        HdSceneIndexObserver::RenamedPrimEntries const &entries) override;

    // Cross-thread frame channel (06 §3.9 rule b): SystemMessage is NOT
    // delivered through the prim-noticed hooks — we install our own
    // HdSceneIndexBase::Observer (which does override _SystemMessage) on the
    // pruned input; see _FrameObserver in the .cpp.

private:
    struct _Groom;  // per-UsdGenGroom state; defined in the .cpp

    UsdGenGroomSceneIndex(
        HdSceneIndexBaseRefPtr const &inputScene,
        int renderInstanceId);
    ~UsdGenGroomSceneIndex() override;

    // Population (06 §3.1): constructor-time observer-driven discovery, with
    // a bounded traversal as the already-populated fallback.
    void _PopulateFromInput();
    void _ScanInputForGrooms() const;
    void _AdoptGroom(SdfPath const &groomRoot) const;
    void _AdoptPending() const;
    void _ForgetGroomsUnder(SdfPath const &path) const;
    // Description resolution: the UsdGenDescription child under an adopted
    // groom root (SI-6 fixture: /groomA/descA). Caller holds _stateMutex.
    bool _ResolveDescriptionLocked(
        _Groom &groom, HdSceneIndexBaseRefPtr const &input) const;

    // Notice routing + commits (06 §3.2, §3.9).
    void _Forward(
        std::vector<HdSceneIndexObserver::AddedPrimEntry> const &added,
        std::vector<HdSceneIndexObserver::RemovedPrimEntry> const &removed,
        std::vector<HdSceneIndexObserver::DirtiedPrimEntry> const &dirtied) const;
    bool _IsOwned(SdfPath const &path) const;
    bool _OwnsDescription(SdfPath const &maybeDescPath) const;
    void _RouteFrameDirties(
        std::vector<HdSceneIndexObserver::DirtiedPrimEntry> const &entries) const;
    /// Replay of batched surface dirties as an inverted MarkDirty
    /// (06 §3.3 line 825): re-accumulate what the batch just consumed.
    void _ReplaySurfaceDirty() const;
    void _CommitNow(usdGen::UsdGenCommitReason reason, bool republishNeeded) const;

    // Publication (06 §3.4, §3.4.1, §5.1): payload state update.
    // Caller must NOT hold _stateMutex (locks internally for the map swap;
    // notice forward runs after it releases). Attachment fields are
    // immutable after construction (Review 5); membership validated by the
    // caller (ByRoot exact-identity check).
    void _RepublishLocked(
        _Groom &groom,
        ::usdGenImaging::UsdGenImagingSession::CommitPayload const &payload) const;
    // Root-keyed wrapper for the republish callback (lock discipline:
    // resolves the live slot, then calls _Republish unlocked).
    void _RepublishByRoot(
        SdfPath const &groomRoot,
        std::shared_ptr<_Groom> const &expectedGroom,
        ::usdGenImaging::UsdGenImagingSession::CommitPayload const &payload) const;

    // StormSurgery test accessors (testHook.h): lock-free snapshot reads
    // over the adopted groom's published map. Defined in the .cpp so the
    // _Groom layout stays private.
    int64_t _TestPublishedGeneration(SdfPath const &groom) const;
    size_t _TestPublishedTileCount(SdfPath const &groom) const;
    // StormSurgery hook reads the private snapshot via the accessors above.
    friend class UsdGenImagingTestHook;
    static HdDataSourceLocatorSet _DirtiedLocatorsFor(
        usdGen::UsdGenTileDirty const &reportTile,
        usdGen::UsdGenGeneration const &gen, size_t tileIdx,
        bool surfaceXformDirty, bool republishNeeded);

    HdSceneIndexBaseRefPtr _pruned;   // input with extComputationPrimvar
                                      // pruning spliced (06 §3.5); NEVER
                                      // spliced into the chain
    uint64_t _renderInstanceId = 0;

    mutable std::mutex _stateMutex;   // guards everything below
    // Shared ownership: Work snapshots and _RepublishByRoot hold STRONG
    // refs (lifetime by refcount, never a flag); callbacks hold WEAK refs
    // and lock() at invocation (expired ⇒ no-op). No raw g.get()/self
    // captures on the publish path.
    std::vector<std::shared_ptr<_Groom>> _grooms;
    // A ticket reserves a root while its session/callback attachment is
    // constructed outside _stateMutex. Removal erases the ticket so a late
    // candidate cannot reinstall a removed groom.
    uint64_t _nextAdoptionTicket = 0;
    std::unordered_map<SdfPath, uint64_t, SdfPath::Hash> _pendingAdoptions;
    std::atomic_flag _populated;      // one-shot population attempt (06 §3.1)
    std::vector<HdSceneIndexObserver::AddedPrimEntry> _pendingAdd;
    std::vector<SdfPath> _pendingRemove;

};

/// Registration half: the HdSceneIndexPlugin consulted by every renderer's
/// _AppendSceneIndex chain.
class UsdGenGroomSceneIndexPlugin final : public HdSceneIndexPlugin
{
public:
    UsdGenGroomSceneIndexPlugin() = default;

protected:
    // HdSceneIndexPlugin interface: splice UsdGenGroomSceneIndex on top of
    // the input (2-arg overload; no render-instance discrimination needed).
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr &inputScene,
        const HdContainerDataSourceHandle &inputArgs) override;

    // Kill switch (06 §6, ADR §5.1): USDGEN_ENABLE=false → the plugin drops
    // out of the chain without editing plugInfo.
    bool _IsEnabled(
        HdContainerDataSourceHandle const &inputArgs) const override;
};

}  // namespace pxr

#endif  // USDGEN_IMAGING_GROOM_SCENE_INDEX_PLUGIN_H
