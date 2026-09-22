// usdGenTonic imaging — the Tonic scene index (plan/17 D2, plan/18 §2.1-§2.2).
//
// Two classes, mirroring the groom index split:
//   * UsdGenTonicSceneIndexPlugin — the HdSceneIndexPlugin registration
//     (plugInfo: loadWithRenderer "", phase 0, ordered after the groom
//     index; USDGENTONIC_ENABLE kill switch).
//   * UsdGenTonicSceneIndex — the filtering index itself. It publishes
//     synthetic prims under /__usdGenTonic/ from the model the
//     TonicRegistry says is active.
//
// The index owns no model. It attaches to the registry on construction and
// detaches on destruction; Tonic_Publish hands it the active model and it
// stages one prim family per hierarchy level:
//
//   tubes/L<n>      mesh         every tube at level n, one fat mesh
//   centers/L<n>    basisCurves  the center curves (focused level thicker)
//   centerCVs/L<n>  points       the center CVs as dots
//   rings/L<n>      basisCurves  the cross-section rings, closed
//   ringCVs/L<n>    points       the ring CVs as dots
//   guides/L<n>     basisCurves  the K9/K10 guide preview
//   graphNodes / graphEdges / graphRegions   the Graph-mode overlay
//   gizmo / brushRing                        the V1 manipulator overlays
//   material_tube / material_hairPreview / material_overlay
//
// While no model is active and USDGENTONIC_TEST_TUBE=1 the index publishes
// the static test tube at /__usdGenTonic/testTube. It defaults off: an
// empty stage opens clean. Activating a model removes it for good: it is a
// harness convenience, not a fallback, and it must never share the frame
// with real geometry.
//
// GetPrim NEVER cooks — it reads the last staged snapshot. Refresh() stages
// a new one through the per-level publisher and emits leaf-exact dirties for
// exactly what changed, per prim: a one-tube move dirties points and extent
// on that level alone, a level toggling off dirties visibility alone, and a
// focus change dirties widths alone.
//
// Both classes live in the pxr namespace so the HdSceneIndexPlugin registry
// can reference them; the model, registry and publisher live in usdGenTonic.
#ifndef USDGEN_TONIC_SCENE_INDEX_PLUGIN_H
#define USDGEN_TONIC_SCENE_INDEX_PLUGIN_H

#include "usdGenTonic/tonicGizmo.h"
#include "usdGenTonic/tonicRegistry.h"

#include "pxr/pxr.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/dataSourceLocator.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace usdGenTonic {
class TonicModel;
class TonicPublisher;
struct TonicStagedModel;
} // namespace usdGenTonic

PXR_NAMESPACE_OPEN_SCOPE

class UsdGenTonicSceneIndex final
    : public HdSingleInputFilteringSceneIndexBase
    , public usdGenTonic::TonicPublishTarget
{
public:
    // Plan/17 R5: Tonic prims live under /__usdGenTonic/, never under a
    // description. The per-level paths are <root>/<family>/L<n>.
    static SdfPath const &RootPath();
    static SdfPath const &TestTubePath();
    static SdfPath const &TubesScopePath();
    static SdfPath const &CentersScopePath();
    static SdfPath const &CenterCVsScopePath();
    static SdfPath const &RingsScopePath();
    static SdfPath const &RingCVsScopePath();
    static SdfPath const &GuidesScopePath();
    static SdfPath TubesPath(int level);
    static SdfPath CentersPath(int level);
    static SdfPath CenterCVsPath(int level);
    static SdfPath RingsPath(int level);
    static SdfPath RingCVsPath(int level);
    static SdfPath GuidesPath(int level);
    // The V1 overlays (plan/18 §2.4): present only while the model holds
    // a gizmo / brush record, removed the moment it clears one.
    static SdfPath const &GizmoPath();
    static SdfPath const &BrushRingPath();
    static SdfPath const &GraphNodesPath();
    static SdfPath const &GraphEdgesPath();
    static SdfPath const &GraphRegionsPath();
    // The tube glslfx, the hair preview, and the unlit overlay the center
    // curves, rings and CV dots bind (same glslfx, `unlit` parameter set).
    static SdfPath const &TubeMaterialPath();
    // The same glslfx with opacity < 1, which is what puts an x-rayed
    // level in Storm's OIT pass so it stops writing depth over the
    // centers and CV dots inside it (plan/18 §2.4a).
    static SdfPath const &TubeXrayMaterialPath();
    static SdfPath const &HairMaterialPath();
    static SdfPath const &OverlayMaterialPath();

    static HdSceneIndexBaseRefPtr New(
        HdSceneIndexBaseRefPtr const &inputScene);

    // -- HdSceneIndexInterface ------------------------------------------------
    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override;

    // -- usdGenTonic::TonicPublishTarget --------------------------------------
    bool PublishModel(usdGenTonic::TonicModel *model,
                      uint32_t dirtyMask) override;
    bool QueryPublishedLevel(int level, int *outFaceCount, int *outPointCount,
                             int *outTubeCount) const override;

    // Stage the registry's active model and publish it. `dirtyMask` is
    // OR-ed with whatever the model reports pending. Returns false when
    // nothing is published (no active model and no test tube).
    bool Refresh(uint32_t dirtyMask);
    bool Refresh() { return Refresh(~0u); }

    // Levels currently published, ascending. Empty while the test tube (or
    // nothing) is on screen.
    std::vector<int> PublishedLevels() const;
    // Face / point / tube census of one published level; false when the
    // level is not published. This is what the T3 viewport script asserts
    // against, through Tonic_GetPublishedLevelInfo.
    bool PublishedLevelInfo(int level, int *outFaceCount, int *outPointCount,
                            int *outTubeCount) const;
    // Tubes the last publish re-tessellated (the per-tube slice rule).
    int LastRestagedTubeCount() const;
    // True while the static test tube is published.
    bool HasTestTube() const;
    // True while the usdGen cook's amplified tiles are allowed to draw
    // (plan/17 §3.2): "show amplified hair" on and no gesture in flight.
    // False means this index authors visibility = false over every prim
    // under a `__usdGenRender` scope of the input scene.
    bool AmplifiedTilesVisible() const;
    // The tile prims this index is currently overriding, for the T1 test.
    size_t AmplifiedTileCount() const;
    // The committed `<groom>/Guides` prim this index is hiding, or an
    // empty path when it is hiding none (plan/18 §2.4a). It is the
    // previous commit's curves: an ordinary BasisCurves with no
    // displayColor, which Storm draws plain white over the live tubes.
    // The path comes from TonicModel::GetGroomPath and is cleared when
    // the model deactivates.
    SdfPath HiddenGuidesPath() const;
    // The path that a groom path implies. Public so the T1 test states
    // the spelling once; an empty or non-prim groom path answers empty.
    static SdfPath CommittedGuidesPath(std::string const &groomPath);

    // Index-local refinements of TonicDirty, used by the notice builders
    // below. The low bits keep the TonicDirty meanings so a caller may pass
    // either spelling.
    static constexpr uint32_t Dirty_Uniforms = 1u << 8;    // tubeId/colour/sel
    static constexpr uint32_t Dirty_Widths = 1u << 9;
    static constexpr uint32_t Dirty_Xray = 1u << 10;
    static constexpr uint32_t Dirty_Visibility = 1u << 11;
    // The level's mesh swapped between the opaque and the translucent
    // tube material (plan/18 §2.4a x-ray).
    static constexpr uint32_t Dirty_Material = 1u << 12;

    // Exact notice locator sets, pure functions of the dirty bits:
    //   * NoticesFor        — the tubes/L<n> mesh
    //   * CurveNoticesFor   — centers/L<n>, rings/L<n>
    //   * PointNoticesFor   — centerCVs/L<n>, ringCVs/L<n>
    //   * GuideNoticesFor   — guides/L<n>
    //   * GraphNoticesFor   — the Graph-mode overlay
    // Points dirty gives the point/extent leaves; topology adds the
    // topology leaf; uniforms give the per-face/per-curve primvars; widths,
    // x-ray and visibility give one leaf each.
    static HdDataSourceLocatorSet NoticesFor(uint32_t dirty);
    static HdDataSourceLocatorSet CurveNoticesFor(uint32_t dirty);
    static HdDataSourceLocatorSet PointNoticesFor(uint32_t dirty);
    static HdDataSourceLocatorSet GuideNoticesFor(uint32_t dirty,
                                                 bool countChanged);
    static HdDataSourceLocatorSet GraphNoticesFor(uint32_t dirty);
    //   * OverlayNoticesFor — the gizmo / brushRing curves
    static HdDataSourceLocatorSet OverlayNoticesFor(uint32_t dirty,
                                                    bool countChanged);

protected:
    void _PrimsAdded(
        HdSceneIndexBase const &sceneIndex,
        HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(
        HdSceneIndexBase const &sceneIndex,
        HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(
        HdSceneIndexBase const &sceneIndex,
        HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;

private:
    UsdGenTonicSceneIndex(HdSceneIndexBaseRefPtr const &inputScene);
    ~UsdGenTonicSceneIndex() override;

    // One published prim: its type and its retained data source.
    struct _Prim {
        TfToken primType;
        HdContainerDataSourceHandle dataSource;
    };
    // Swap in a new prim table and send the Added/Removed/Dirtied notices
    // that turn the old one into it.
    void _Commit(std::map<SdfPath, _Prim> &&prims,
                 HdSceneIndexObserver::DirtiedPrimEntries const &dirtied);
    void _BuildTestTubePrims(std::map<SdfPath, _Prim> *prims) const;
    void _BuildLevelPrims(usdGenTonic::TonicStagedModel const &staged,
                          std::map<SdfPath, _Prim> *prims) const;
    void _BuildGraphPrims(usdGenTonic::TonicModel &model,
                          std::map<SdfPath, _Prim> *prims) const;
    // What one publish changed about the two overlay prims. They are
    // reported separately because they are separate prims: moving the
    // brush ring must not dirty a leaf of the gizmo.
    struct _OverlayDirty {
        uint32_t gizmo = 0;  // TonicDirty_Gizmo when the record moved
        bool gizmoCountChanged = false;
        uint32_t brush = 0;  // TonicDirty_Brush when the record moved
        bool brushCountChanged = false;
    };
    // The gizmo and brush-ring prims, from the model's overlay records.
    _OverlayDirty _BuildOverlayPrims(usdGenTonic::TonicModel &model,
                                     std::map<SdfPath, _Prim> *prims);
    void _CollectLevelDirties(
        usdGenTonic::TonicStagedModel const &previous,
        usdGenTonic::TonicStagedModel const &current, uint32_t modelDirty,
        HdSceneIndexObserver::DirtiedPrimEntries *out) const;
    // True for any prim under a `__usdGenRender` scope: the amplified tiles
    // the groom scene index publishes for a description (plan/17 §3.2).
    static bool _IsAmplifiedTilePath(SdfPath const &path);
    // Flip the tile override and dirty the visibility of every tile the
    // input has announced so far. Returns true when the state changed.
    bool _SetAmplifiedTilesVisible(bool visible);
    // Swap the hidden-guides path and dirty visibility on both ends.
    bool _SetHiddenGuidesPath(SdfPath const &path);

    std::unique_ptr<usdGenTonic::TonicPublisher> _publisher;
    // The model the staged snapshot belongs to. Slice reuse is keyed on
    // tube id, so switching models has to drop it.
    usdGenTonic::TonicModel *_stagedModel = nullptr;
    // The published snapshot the dirty comparison runs against.
    std::unique_ptr<usdGenTonic::TonicStagedModel> _published;
    // The published prim table. Written by Refresh, read by GetPrim; the
    // mutex keeps Storm's parallel GetPrim race-free (the SI-4 discipline).
    mutable std::mutex _mutex;
    std::map<SdfPath, _Prim> _prims;
    // What the last publish drew for the overlays, so a re-set of the same
    // gizmo dirties nothing and a real move dirties exactly its leaves.
    usdGenTonic::TonicGizmoRecord _publishedGizmo;
    usdGenTonic::TonicBrushRingRecord _publishedBrush;
    int _publishedGizmoCurves = 0;
    int _publishedBrushCurves = 0;
    // Sticky: the test tube never comes back once a model has been active.
    bool _testTubeRetired = false;
    bool _testTubePublished = false;
    int _lastRestagedTubeCount = 0;
    // The amplified-tile override (plan/17 §3.2). Atomic because GetPrim
    // runs on Storm's worker threads while Refresh writes it on the UI
    // thread; `_tilePaths` is the set the input has announced, guarded by
    // _mutex like the prim table.
    std::atomic<bool> _amplifiedTilesVisible{true};
    std::set<SdfPath> _tilePaths;
    // The committed guides prim hidden while a model is active; guarded
    // by _mutex, because GetPrim reads it on Storm's worker threads.
    SdfPath _hiddenGuidesPath;
};

class UsdGenTonicSceneIndexPlugin final : public HdSceneIndexPlugin
{
public:
    UsdGenTonicSceneIndexPlugin() = default;

protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const std::string &renderInstanceId,
        const HdSceneIndexBaseRefPtr &inputScene,
        const HdContainerDataSourceHandle &inputArgs) override;

    bool _IsEnabled(
        HdContainerDataSourceHandle const &inputArgs) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // USDGEN_TONIC_SCENE_INDEX_PLUGIN_H
