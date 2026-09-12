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
//     terminal HdRetiredAdapterSceneIndexFilter. It captures owning input
//     packets at the caller boundary and schedules per-scene membership,
//     independent per-description cooking, and ordered publication as
//     synthetic Hydra prims <description>/__usdGenRender/tile_NNNN.
//
// GetPrim NEVER commits — it reads the frontend-visible generation (I7). The
// M0 pass-through class is gone: this index owns sessions, synthesizes and
// announces the published prim set and forwards upstream notices.
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
    // Explicit serialized frontend host/test boundary. Reads never call this.
    // Delivers completed notices here; observer/owner callbacks must not wait.
    void Synchronize();
    // External shutdown/test boundary after clients have stopped submitting.
    static void DrainRetired();
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
    void _SystemMessage(
        TfToken const &messageType,
        HdDataSourceBaseHandle const &args) override;

private:
    struct _State;
    struct _Ingress;
    friend struct UsdGenSceneService;

    UsdGenGroomSceneIndex(
        HdSceneIndexBaseRefPtr const &inputScene,
        int renderInstanceId);
    ~UsdGenGroomSceneIndex() override;

    // Hydra capture stays on the caller/notice boundary. Only owning value
    // packets enter the scene owner; no worker assumes upstream affinity.
    void _CaptureAndSubmit(_Ingress ingress);
    // Hydra serializes scene-index frontend calls.  This boundary alone
    // installs visible snapshots and sends notices; owner work only queues
    // immutable packets.  Explicit waits during observer delivery are invalid.
    void _DrainPublications(bool waitForIngress, bool explicitWait = false);

    // StormSurgery test accessors (testHook.h): immutable snapshot reads
    // over the adopted groom's published map, with no owner wait.
    int64_t _TestPublishedGeneration(SdfPath const &groom) const;
    size_t _TestPublishedTileCount(SdfPath const &groom) const;
    // StormSurgery hook reads the private snapshot via the accessors above.
    friend class UsdGenImagingTestHook;

    HdSceneIndexBaseRefPtr _pruned;   // input with extComputationPrimvar
                                      // pruning spliced (06 §3.5); NEVER
                                      // spliced into the chain
    uint64_t _renderInstanceId = 0;

    std::shared_ptr<_State> _state;
    bool _dispatching = false;
    bool _deferredSynchronousFlush = false;

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
