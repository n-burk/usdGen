// Integration coverage for per-scene publication ownership.  The target is
// registered by the integration owner.
#include "usdGenImaging/groomSceneIndexPlugin.h"
#include "usdGenImaging/testHook.h"
#include "usdGen/opRegistry.h"

#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/systemMessages.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int failures = 0;

void Check(bool value, char const *message)
{
    if (!value) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    } else {
        std::printf("ok:   %s\n", message);
    }
}

char const *kFixture = R"USDA(#usda 1.0
def Mesh "Scalp" (prepend apiSchemas = ["UsdGenRestAPI"])
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1,0,-1), (1,0,-1), (1,0,1), (-1,0,1)]
    point3f[] primvars:rest = [(-1,0,-1), (1,0,-1), (1,0,1), (-1,0,1)] (interpolation = "vertex")
    texCoord2f[] primvars:st = [(0,0), (1,0), (1,1), (0,1)] (interpolation = "vertex")
}
# Legacy CPU reference fixture: an untyped container has no CUDA backend
# schema fallback. Production UsdGenGroom remains CUDA-only.
def Scope "Groom"
{
    def UsdGenDescription "hair"
    {
        def Scope "Ops"
        {
            def UsdGenWidth "width"
            {
                float usdGen:width = 0.02
                bool usdGen:replace = true
                uniform token usdGen:width:interpolation = "linear"
                float2[] usdGen:width:knots = [(0, 1), (1, 1)]
            }
            def UsdGenCurveSource "source"
            {
                rel usdGen:surface = </Scalp>
                rel usdGen:curves = </Groom/hair/curves>
                # Keep a uniform publication fixture; ragged source handling
                # is covered separately by the engine/CUDA source tests.
                int usdGen:resampleTo = 4
                uniform token usdGen:rebind = "never"
            }
        }
        def BasisCurves "curves" (
            prepend apiSchemas = ["UsdGenCurveAPI"]
        )
        {
            uniform token type = "cubic"
            uniform token basis = "bspline"
            uniform token wrap = "pinned"
            int[] curveVertexCounts = [4]
            point3f[] points = [(0,0,0), (0,1,0), (0,2,0), (0,3,0)]
            uniform token primvars:usdGen:role = "hair"
            uint64[] primvars:usdGen:curveId = [1] (interpolation = "uniform")
            # Explicit never-rebind input: these bindings are authored C3
            # data, rather than relying on the old scalp-as-curves surrogate.
            int[] primvars:skinprim = [0] (interpolation = "uniform")
            texCoord2f[] primvars:skinprimuv = [(0.5,0.5)] (interpolation = "uniform")
            matrix4d[] primvars:usdGen:rootFrame = [((1,0,0,0),(0,0,1,0),(0,-1,0,0),(0,0,0,1))] (interpolation = "uniform")
        }
    }
}
)USDA";

struct Scene {
    UsdStageRefPtr stage;
    UsdImagingSceneIndices indices;
    HdSceneIndexBaseRefPtr groom;
    UsdGenGroomSceneIndex *owner = nullptr;
};

Scene MakeScene(char const *name)
{
    Scene out;
    out.stage = UsdStage::CreateInMemory(name);
    out.stage->GetRootLayer()->ImportFromString(kFixture);
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = out.stage;
    out.indices = UsdImagingCreateSceneIndices(info);
    out.groom = UsdGenGroomSceneIndex::New(out.indices.finalSceneIndex);
    out.owner = dynamic_cast<UsdGenGroomSceneIndex *>(out.groom.operator->());
    return out;
}

SdfPath FirstTile(Scene const &scene)
{
    SdfPath const render("/Groom/hair/__usdGenRender");
    SdfPathVector children = scene.groom->GetChildPrimPaths(render);
    return children.empty() ? SdfPath() : children.front();
}

float FirstWidth(Scene const &scene, SdfPath const &tile)
{
    HdSceneIndexPrim prim = scene.groom->GetPrim(tile);
    HdDataSourceBaseHandle value = HdContainerDataSource::Get(
        prim.dataSource, HdDataSourceLocator(TfToken("primvars"),
        TfToken("widths"), TfToken("primvarValue")));
    HdSampledDataSourceHandle sampled = HdSampledDataSource::Cast(value);
    if (!sampled) return -1.0f;
    VtValue result = sampled->GetValue(0.0f);
    if (!result.IsHolding<VtFloatArray>() || result.UncheckedGet<VtFloatArray>().empty())
        return -1.0f;
    return result.UncheckedGet<VtFloatArray>()[0];
}

bool HasChild(HdSceneIndexBase const &sceneIndex, SdfPath const &parent,
              SdfPath const &child)
{
    for (SdfPath const &candidate : sceneIndex.GetChildPrimPaths(parent))
        if (candidate == child) return true;
    return false;
}

// Scope and Xform have no guaranteed Hydra prim type token.  Check that the
// pass-through result is present, remains parented, and agrees with the
// authoritative source scene index instead of guessing a schema token.
bool MatchesAuthoredSourcePrim(Scene const &scene, SdfPath const &path)
{
    HdSceneIndexPrim const source = scene.indices.finalSceneIndex->GetPrim(path);
    HdSceneIndexPrim const published = scene.groom->GetPrim(path);
    SdfPath const parent = path.GetParentPath();
    return source.dataSource && published.dataSource &&
        source.primType == published.primType &&
        HasChild(*scene.indices.finalSceneIndex, parent, path) &&
        HasChild(*scene.groom, parent, path);
}

class TileHoldObserver final : public HdSceneIndexObserver {
public:
    explicit TileHoldObserver(SdfPath prefix) : _prefix(std::move(prefix)) {}
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> timedOut{false};

    void PrimsAdded(HdSceneIndexBase const &, AddedPrimEntries const &entries) override {
        for (auto const &entry : entries) {
            if (entry.primPath.HasPrefix(_prefix)) Hold();
        }
    }
    void PrimsRemoved(HdSceneIndexBase const &, RemovedPrimEntries const &) override {}
    void PrimsDirtied(HdSceneIndexBase const &, DirtiedPrimEntries const &entries) override {
        for (auto const &entry : entries) {
            if (entry.primPath.HasPrefix(_prefix)) Hold();
        }
    }
    void PrimsRenamed(HdSceneIndexBase const &, RenamedPrimEntries const &) override {}

private:
    void Hold() {
        entered.store(true, std::memory_order_release);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (!release.load(std::memory_order_acquire))
            timedOut.store(true, std::memory_order_release);
    }
    SdfPath _prefix;
};

class SourceNoticeObserver final : public HdSceneIndexObserver {
public:
    explicit SourceNoticeObserver(SdfPath path) : _path(std::move(path)) {}
    std::atomic<bool> added{false};
    std::atomic<bool> removed{false};
    std::atomic<bool> dirtied{false};
    void PrimsAdded(HdSceneIndexBase const &, AddedPrimEntries const &entries) override {
        for (auto const &entry : entries)
            if (entry.primPath == _path) added.store(true, std::memory_order_release);
    }
    void PrimsRemoved(HdSceneIndexBase const &, RemovedPrimEntries const &entries) override {
        for (auto const &entry : entries)
            if (entry.primPath == _path) removed.store(true, std::memory_order_release);
    }
    void PrimsDirtied(HdSceneIndexBase const &, DirtiedPrimEntries const &entries) override {
        for (auto const &entry : entries)
            if (entry.primPath == _path) dirtied.store(true, std::memory_order_release);
    }
    void PrimsRenamed(HdSceneIndexBase const &, RenamedPrimEntries const &) override {}
private:
    SdfPath _path;
};

class SourceRecoveryObserver final : public HdSceneIndexObserver {
public:
    SourceRecoveryObserver(SdfPath child, SdfPath tile, SdfPath render,
                           SdfPath removalRoot = SdfPath())
        : _child(std::move(child)), _tile(std::move(tile)), _render(std::move(render)),
          _removalRoot(std::move(removalRoot)) {}
    std::atomic<bool> childReadded{false};
    std::atomic<bool> sawMatchingRemoval{false};
    std::atomic<bool> tileEmptyDuringRemoval{true};
    std::atomic<bool> renderEmptyDuringRemoval{true};

    void PrimsAdded(HdSceneIndexBase const &, AddedPrimEntries const &entries) override {
        for (auto const &entry : entries)
            if (entry.primPath == _child)
                childReadded.store(true, std::memory_order_release);
    }
    void PrimsRemoved(HdSceneIndexBase const &sender,
                      RemovedPrimEntries const &entries) override {
        for (auto const &entry : entries) {
            if (entry.primPath != _removalRoot) continue;
            sawMatchingRemoval.store(true, std::memory_order_release);
            if (!sender.GetPrim(_tile).primType.IsEmpty())
                tileEmptyDuringRemoval.store(false, std::memory_order_release);
            if (!sender.GetChildPrimPaths(_render).empty())
                renderEmptyDuringRemoval.store(false, std::memory_order_release);
        }
    }
    void PrimsDirtied(HdSceneIndexBase const &, DirtiedPrimEntries const &) override {}
    void PrimsRenamed(HdSceneIndexBase const &, RenamedPrimEntries const &) override {}

private:
    SdfPath _child, _tile, _render, _removalRoot;
};

class SyntheticReaddObserver final : public HdSceneIndexObserver {
public:
    SyntheticReaddObserver(SdfPath render, SdfPath tile)
        : _render(std::move(render)), _tile(std::move(tile)) {}
    std::atomic<bool> renderAdded{false};
    std::atomic<bool> tileAdded{false};

    void PrimsAdded(HdSceneIndexBase const &, AddedPrimEntries const &entries) override {
        for (auto const &entry : entries) {
            if (entry.primPath == _render)
                renderAdded.store(true, std::memory_order_release);
            if (entry.primPath == _tile)
                tileAdded.store(true, std::memory_order_release);
        }
    }
    void PrimsRemoved(HdSceneIndexBase const &, RemovedPrimEntries const &) override {}
    void PrimsDirtied(HdSceneIndexBase const &, DirtiedPrimEntries const &) override {}
    void PrimsRenamed(HdSceneIndexBase const &, RenamedPrimEntries const &) override {}

private:
    SdfPath _render, _tile;
};

class AnyNoticeObserver final : public HdSceneIndexObserver {
public:
    std::atomic<unsigned> notices{0};
    void PrimsAdded(HdSceneIndexBase const &, AddedPrimEntries const &) override {
        notices.fetch_add(1, std::memory_order_release);
    }
    void PrimsRemoved(HdSceneIndexBase const &, RemovedPrimEntries const &) override {
        notices.fetch_add(1, std::memory_order_release);
    }
    void PrimsDirtied(HdSceneIndexBase const &, DirtiedPrimEntries const &) override {
        notices.fetch_add(1, std::memory_order_release);
    }
    void PrimsRenamed(HdSceneIndexBase const &, RenamedPrimEntries const &) override {}
};

bool WaitFor(std::atomic<bool> const &value)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire)) return true;
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire);
}
} // namespace

int main()
{
    usdGen::usdGenRegisterM1Operators();
    Scene first = MakeScene("scene-publication-first");
    Check(first.owner != nullptr, "first concrete groom scene owner is available");
    if (!first.owner) return 1;
    first.owner->Synchronize();
    SdfPath tile = FirstTile(first);
    Check(!tile.IsEmpty(), "initial CPU curve source publishes a synthetic tile");
    float const before = tile.IsEmpty() ? -1.0f : FirstWidth(first, tile);
    std::printf("initial width: %.9g\n", before);
    Check(std::fabs(before - 0.02f) < 1e-6f,
          "initial synthetic tile exposes exact width 0.02");

    TileHoldObserver hold(SdfPath("/Groom/hair/__usdGenRender"));
    first.groom->AddObserver(TfCreateWeakPtr(&hold));
    UsdAttribute width = first.stage->GetAttributeAtPath(
        SdfPath("/Groom/hair/Ops/width.usdGen:width"));
    Check(bool(width) && width.Set(0.08f), "authored width edit succeeds");
    // The frontend owns delivery, not a background scene command. Transfer
    // frontend ownership to this thread while its observer is held; only
    // read-only queries run concurrently on the first scene.
    std::thread delivery([&] { first.indices.stageSceneIndex->ApplyPendingUpdates(); });

    Check(WaitFor(hold.entered), "synthetic tile notice is held at publication boundary");
    std::atomic<bool> readDone{false}, readExactWidth{false};
    std::thread reader([&] {
        float const after = tile.IsEmpty() ? -1.0f : FirstWidth(first, tile);
        std::printf("edited width: %.9g\n", after);
        readExactWidth.store(std::fabs(after - 0.08f) < 1e-6f,
                             std::memory_order_release);
        readDone.store(true, std::memory_order_release);
    });
    Check(WaitFor(readDone),
          "GetPrim completes while the synthetic tile notice remains held");
    Check(readExactWidth.load(std::memory_order_acquire),
          "held-notice GetPrim reads exact new width 0.08");

    std::atomic<bool> secondDone{false}, secondPublished{false};
    std::thread independent([&] {
        Scene second = MakeScene("scene-publication-second");
        if (second.owner) {
            second.owner->Synchronize();
            secondPublished.store(!FirstTile(second).IsEmpty(),
                                  std::memory_order_release);
        }
        secondDone.store(true, std::memory_order_release);
    });
    Check(WaitFor(secondDone),
          "second independent scene synchronizes while first notice is held");
    Check(secondPublished.load(std::memory_order_acquire),
          "second independent scene publishes a tile while first notice is held");

    hold.release.store(true, std::memory_order_release);
    reader.join();
    independent.join();
    delivery.join();
    first.owner->Synchronize();
    first.groom->RemoveObserver(TfCreateWeakPtr(&hold));
    Check(!hold.timedOut.load(std::memory_order_acquire),
          "held synthetic tile notice was released before its bounded timeout");

    // Time-varying mapped parameters must be sampled at the current stage
    // time on each capture; cache reuse must not freeze the initial width.
    Check(width.Set(0.04f, UsdTimeCode(1.0)) &&
              width.Set(0.12f, UsdTimeCode(2.0)),
          "animated width time samples author successfully");
    first.indices.stageSceneIndex->SetTime(UsdTimeCode(1.0));
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    first.owner->Synchronize();
    float const frameOne = tile.IsEmpty() ? -1.0f : FirstWidth(first, tile);
    Check(std::fabs(frameOne - 0.04f) < 1e-6f,
          "frame 1 synthetic tile samples animated width");

    first.indices.stageSceneIndex->SetTime(UsdTimeCode(2.0));
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    first.owner->Synchronize();
    float const frameTwo = tile.IsEmpty() ? -1.0f : FirstWidth(first, tile);
    Check(std::fabs(frameTwo - 0.12f) < 1e-6f,
          "frame 2 synthetic tile samples animated width");

    // Saturate only the groom owner ingress, then deliver a source dirty.
    // The rejected ingress allocates no watermark and emits no source notice;
    // recovery later emits the source dirty as well as refreshing the tile.
    unsigned heldCredits = 0;
    while (heldCredits < 4096 && UsdGenImagingTestHook::holdOneGroomOwnerCredit())
        ++heldCredits;
    Check(heldCredits > 0 && heldCredits < 4096,
          "test hook deterministically fills bounded groom owner admission");
    SourceNoticeObserver dirtyNotice(SdfPath("/Groom/hair/Ops/width"));
    first.groom->AddObserver(TfCreateWeakPtr(&dirtyNotice));
    uint64_t const issuedBeforePressure = UsdGenImagingTestHook::issuedIngresses();
    Check(width.Set(0.14f, UsdTimeCode(2.0)),
          "pressure-recovery width edit authors at the current frame");
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    Check(!dirtyNotice.dirtied.load(std::memory_order_acquire),
          "rejected source dirty emits no premature observer notice");
    Check(UsdGenImagingTestHook::issuedIngresses() == issuedBeforePressure,
          "rejected ingress allocates no sequence watermark");
    Check(std::fabs(FirstWidth(first, tile) - 0.12f) < 1e-6f,
          "rejected ingress retains the prior synthetic snapshot without waiting");
    UsdGenImagingTestHook::releaseGroomOwnerCredits();
    first.owner->Synchronize();
    Check(std::fabs(FirstWidth(first, tile) - 0.14f) < 1e-6f,
          "released admission forces full synthetic discovery of deferred edit");
    Check(dirtyNotice.dirtied.load(std::memory_order_acquire),
          "recovery emits the deferred source dirty notice");
    first.groom->RemoveObserver(TfCreateWeakPtr(&dirtyNotice));

    heldCredits = 0;
    while (heldCredits < 4096 && UsdGenImagingTestHook::holdOneGroomOwnerCredit()) ++heldCredits;
    SourceNoticeObserver outsideAdded(SdfPath("/Outside"));
    first.groom->AddObserver(TfCreateWeakPtr(&outsideAdded));
    Check(bool(first.stage->DefinePrim(SdfPath("/Outside"), TfToken("Scope"))),
          "outside source addition authors under pressure");
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    Check(!outsideAdded.added.load(std::memory_order_acquire),
          "rejected source addition emits no premature observer notice");
    UsdGenImagingTestHook::releaseGroomOwnerCredits();
    first.owner->Synchronize();
    Check(MatchesAuthoredSourcePrim(first, SdfPath("/Outside")),
          "namespace recovery restores deferred outside source addition");
    Check(outsideAdded.added.load(std::memory_order_acquire),
          "recovery emits the deferred source addition notice");
    first.groom->RemoveObserver(TfCreateWeakPtr(&outsideAdded));

    heldCredits = 0;
    while (heldCredits < 4096 && UsdGenImagingTestHook::holdOneGroomOwnerCredit()) ++heldCredits;
    SourceNoticeObserver outsideRemoved(SdfPath("/Outside"));
    first.groom->AddObserver(TfCreateWeakPtr(&outsideRemoved));
    first.stage->RemovePrim(SdfPath("/Outside"));
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    Check(!outsideRemoved.removed.load(std::memory_order_acquire),
          "rejected source removal emits no premature observer notice");
    UsdGenImagingTestHook::releaseGroomOwnerCredits();
    first.owner->Synchronize();
    Check(first.groom->GetPrim(SdfPath("/Outside")).primType.IsEmpty(),
          "namespace recovery removes deferred outside source prim");
    Check(outsideRemoved.removed.load(std::memory_order_acquire),
          "recovery emits the deferred source removal notice");
    first.groom->RemoveObserver(TfCreateWeakPtr(&outsideRemoved));

    heldCredits = 0;
    while (heldCredits < 4096 && UsdGenImagingTestHook::holdOneGroomOwnerCredit()) ++heldCredits;
    first.stage->DefinePrim(SdfPath("/Outside"), TfToken("Mesh"));
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    UsdGenImagingTestHook::releaseGroomOwnerCredits();
    first.owner->Synchronize();
    Check(first.groom->GetPrim(SdfPath("/Outside")).primType == TfToken("mesh"),
          "namespace recovery restores deferred outside type change");

    // A second frontend ingress must consume the deferred full-capture latch
    // itself: callers need not issue a separate explicit Synchronize to make
    // the first rejected source edit visible.
    heldCredits = 0;
    while (heldCredits < 4096 && UsdGenImagingTestHook::holdOneGroomOwnerCredit()) ++heldCredits;
    SourceNoticeObserver lostNotice(SdfPath("/LostBeforeNext"));
    first.groom->AddObserver(TfCreateWeakPtr(&lostNotice));
    Check(bool(first.stage->DefinePrim(SdfPath("/LostBeforeNext"), TfToken("Scope"))),
          "first deferred source edit authors under pressure");
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    Check(!lostNotice.added.load(std::memory_order_acquire),
          "lost source addition emits no premature observer notice");
    UsdGenImagingTestHook::releaseGroomOwnerCredits();
    Check(bool(first.stage->DefinePrim(SdfPath("/AcceptedAfterLoss"), TfToken("Scope"))),
          "next source edit authors after pressure release");
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    Check(MatchesAuthoredSourcePrim(first, SdfPath("/LostBeforeNext")) &&
              MatchesAuthoredSourcePrim(first, SdfPath("/AcceptedAfterLoss")),
          "next admitted frontend ingress recovers the earlier rejected source edit");
    Check(lostNotice.added.load(std::memory_order_acquire),
          "next admitted ingress emits the recovered lost source addition");
    first.groom->RemoveObserver(TfCreateWeakPtr(&lostNotice));

    // Recovery of an ancestor type change must recreate retained descendants,
    // rather than treating the child as a value-only dirty prim.
    SdfPath const typeRoot("/TypeRecovery"), typeChild("/TypeRecovery/child");
    Check(bool(first.stage->DefinePrim(typeRoot, TfToken("Scope"))) &&
              bool(first.stage->DefinePrim(typeChild, TfToken("Scope"))),
          "type-recovery source hierarchy authors successfully");
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    first.owner->Synchronize();
    SourceRecoveryObserver sourceObserver(typeChild, tile,
                                          SdfPath("/Groom/hair/__usdGenRender"));
    first.groom->AddObserver(TfCreateWeakPtr(&sourceObserver));
    heldCredits = 0;
    while (heldCredits < 4096 && UsdGenImagingTestHook::holdOneGroomOwnerCredit()) ++heldCredits;
    Check(bool(first.stage->DefinePrim(typeRoot, TfToken("Mesh"))),
          "ancestor type change authors under pressure while retaining child");
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    UsdGenImagingTestHook::releaseGroomOwnerCredits();
    first.owner->Synchronize();
    Check(sourceObserver.childReadded.load(std::memory_order_acquire) &&
              MatchesAuthoredSourcePrim(first, typeChild),
          "recovery re-adds a child retained below a changed source ancestor");
    first.groom->RemoveObserver(TfCreateWeakPtr(&sourceObserver));

    first.groom->SystemMessage(HdSystemMessageTokens->asyncAllow, nullptr);
    Check(width.Set(0.16f, UsdTimeCode(3.0)), "async width sample authors successfully");
    first.indices.stageSceneIndex->SetTime(UsdTimeCode(3.0));
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    Check(std::fabs(FirstWidth(first, tile) - 0.14f) < 1e-6f,
          "async edit retains the recovered width 0.14 before polling");
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool delivered = false;
    do {
        first.groom->SystemMessage(HdSystemMessageTokens->asyncPoll, nullptr);
        delivered = std::fabs(FirstWidth(first, tile) - 0.16f) < 1e-6f;
        if (delivered) break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    Check(delivered, "asyncPoll alone installs the exact newly cooked width 0.16");

    // Exercise shutdown with accepted work still in flight. Lazy static
    // operator tables used to be destroyed before the retirement service
    // drained this cook (caught by ASan in Graph::RoutingSnapshot).
    Check(width.Set(0.20f, UsdTimeCode(4.0)), "exit-time width sample authors successfully");
    first.indices.stageSceneIndex->SetTime(UsdTimeCode(4.0));
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    first.owner->Synchronize();
    Check(std::fabs(FirstWidth(first, tile) - 0.20f) < 1e-6f,
          "exit-time width update publishes before removal recovery");

    // A changed groom ancestor removes the render subtree from the source
    // namespace, but it retains the Description that owns virtual tiles.
    // Recovery must therefore re-add the synthetic scope and existing tile,
    // not merely dirty the retained authored hierarchy.
    SdfPath const renderPath("/Groom/hair/__usdGenRender");
    SyntheticReaddObserver syntheticReadd(renderPath, tile);
    first.groom->AddObserver(TfCreateWeakPtr(&syntheticReadd));
    heldCredits = 0;
    while (heldCredits < 4096 && UsdGenImagingTestHook::holdOneGroomOwnerCredit()) ++heldCredits;
    Check(bool(first.stage->DefinePrim(SdfPath("/Groom"), TfToken("Mesh"))),
          "groom ancestor type change authors under pressure while retaining Description");
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    Check(!syntheticReadd.renderAdded.load(std::memory_order_acquire) &&
              !syntheticReadd.tileAdded.load(std::memory_order_acquire),
          "rejected groom type change emits no premature synthetic re-add");
    UsdGenImagingTestHook::releaseGroomOwnerCredits();
    first.owner->Synchronize();
    Check(syntheticReadd.renderAdded.load(std::memory_order_acquire) &&
              syntheticReadd.tileAdded.load(std::memory_order_acquire) &&
              first.groom->GetPrim(renderPath).primType == TfToken("scope") &&
              first.groom->GetPrim(tile).primType == TfToken("basisCurves") &&
              std::fabs(FirstWidth(first, tile) - 0.20f) < 1e-6f,
          "groom ancestor recovery re-adds synthetic scope and tile with preserved values");
    first.groom->RemoveObserver(TfCreateWeakPtr(&syntheticReadd));

    // The final source-removal recovery also owns the synthetic namespace:
    // observers must never see a removed ancestor while stale tiles remain
    // queryable beneath its virtual render scope.
    SourceRecoveryObserver removalObserver(SdfPath(), tile,
                                           SdfPath("/Groom/hair/__usdGenRender"),
                                           SdfPath("/Groom"));
    first.groom->AddObserver(TfCreateWeakPtr(&removalObserver));
    heldCredits = 0;
    while (heldCredits < 4096 && UsdGenImagingTestHook::holdOneGroomOwnerCredit()) ++heldCredits;
    first.stage->RemovePrim(SdfPath("/Groom"));
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    UsdGenImagingTestHook::releaseGroomOwnerCredits();
    first.owner->Synchronize();
    Check(removalObserver.sawMatchingRemoval.load(std::memory_order_acquire) &&
              removalObserver.tileEmptyDuringRemoval.load(std::memory_order_acquire) &&
              removalObserver.renderEmptyDuringRemoval.load(std::memory_order_acquire) &&
              first.groom->GetPrim(tile).primType.IsEmpty() &&
              first.groom->GetChildPrimPaths(SdfPath("/Groom/hair/__usdGenRender")).empty(),
          "ancestor removal recovery clears synthetic tile visibility before removal notice returns");
    first.groom->RemoveObserver(TfCreateWeakPtr(&removalObserver));

    // The latest-publication slot must release superseded immutable snapshots
    // and their tile maps while a consumer declines to poll.  This uses the
    // CPU fixture so no GPU work is needed to exercise generation ownership.
    Scene coalesced = MakeScene("scene-publication-coalesced");
    Check(coalesced.owner != nullptr, "coalesced fixture creates a groom owner");
    if (coalesced.owner) {
        coalesced.owner->Synchronize();
        SdfPath const coalescedTile = FirstTile(coalesced);
        float const visibleBefore = FirstWidth(coalesced, coalescedTile);
        AnyNoticeObserver coalescedNotices;
        coalesced.groom->AddObserver(TfCreateWeakPtr(&coalescedNotices));
        UsdAttribute coalescedWidth = coalesced.stage->GetAttributeAtPath(
            SdfPath("/Groom/hair/Ops/width.usdGen:width"));
        coalesced.groom->SystemMessage(HdSystemMessageTokens->asyncAllow, nullptr);
        Check(coalescedWidth.Set(0.31f, UsdTimeCode(5.0)),
              "first coalesced width generation authors successfully");
        coalesced.indices.stageSceneIndex->SetTime(UsdTimeCode(5.0));
        coalesced.indices.stageSceneIndex->ApplyPendingUpdates();
        UsdGenImagingTestHook::drainGroomOwnersWithoutFrontend(*coalesced.groom);
        auto supersededSnapshot =
            UsdGenImagingTestHook::pendingGroomPublicationSnapshotWeak(*coalesced.groom);
        auto supersededTiles = UsdGenImagingTestHook::pendingGroomPublicationTileMapWeak(
            *coalesced.groom, SdfPath("/Groom/hair"));
        Check(!supersededSnapshot.expired() && !supersededTiles.expired(),
              "first unpolled generation remains retained while pending");
        Check(coalescedWidth.Set(0.32f, UsdTimeCode(6.0)),
              "replacement coalesced width generation authors successfully");
        coalesced.indices.stageSceneIndex->SetTime(UsdTimeCode(6.0));
        coalesced.indices.stageSceneIndex->ApplyPendingUpdates();
        UsdGenImagingTestHook::drainGroomOwnersWithoutFrontend(*coalesced.groom);
        int64_t const generationBeforeBurst =
            UsdGenImagingTestHook::pendingGroomPublishedGeneration(
                *coalesced.groom, SdfPath("/Groom/hair"));
        constexpr unsigned kGenerationBurst = 4098;
        bool allBurstEditsAccepted = true;
        bool allGenerationBatchesBounded = true;
        for (unsigned i = 0; i != kGenerationBurst; ++i) {
            float const value = 1.0f + static_cast<float>(i);
            allBurstEditsAccepted =
                coalescedWidth.Set(value, UsdTimeCode(6.0)) && allBurstEditsAccepted;
            coalesced.indices.stageSceneIndex->SetTime(UsdTimeCode(6.0));
            coalesced.indices.stageSceneIndex->ApplyPendingUpdates();
            UsdGenImagingTestHook::drainGroomOwnersWithoutFrontend(*coalesced.groom);
            if ((i & 63u) == 63u)
                allGenerationBatchesBounded =
                    UsdGenImagingTestHook::pendingGroomPublicationCount(*coalesced.groom) <= 1 &&
                    allGenerationBatchesBounded;
        }
        uint64_t const capturesBeforeCoalescedPoll =
            UsdGenImagingTestHook::groomCaptureCount(*coalesced.groom);
        uint64_t const cooksBeforeCoalescedPoll =
            UsdGenImagingTestHook::groomCookCount(*coalesced.groom);
        int64_t const generationAfterBurst =
            UsdGenImagingTestHook::pendingGroomPublishedGeneration(
                *coalesced.groom, SdfPath("/Groom/hair"));
        Check(supersededSnapshot.expired() && supersededTiles.expired() &&
                  UsdGenImagingTestHook::pendingGroomPublicationCount(*coalesced.groom) <= 1 &&
                  allBurstEditsAccepted &&
                  allGenerationBatchesBounded &&
                  generationBeforeBurst >= 0 && generationAfterBurst >= generationBeforeBurst &&
                  generationAfterBurst >= generationBeforeBurst + kGenerationBurst &&
                  coalescedNotices.notices.load(std::memory_order_acquire) == 0 &&
                  std::fabs(FirstWidth(coalesced, coalescedTile) - visibleBefore) < 1e-6f,
              "4098 superseded CPU generations release old snapshots while visible geometry stays unchanged");
        coalesced.groom->SystemMessage(HdSystemMessageTokens->asyncPoll, nullptr);
        Check(std::fabs(FirstWidth(coalesced, coalescedTile) -
                             static_cast<float>(kGenerationBurst)) < 1e-6f &&
                  coalescedNotices.notices.load(std::memory_order_acquire) > 0 &&
                  UsdGenImagingTestHook::groomCaptureCount(*coalesced.groom) ==
                      capturesBeforeCoalescedPoll &&
                  UsdGenImagingTestHook::groomCookCount(*coalesced.groom) ==
                      cooksBeforeCoalescedPoll,
              "one poll installs the final width without capture or cook work");
        coalesced.groom->RemoveObserver(TfCreateWeakPtr(&coalescedNotices));
    }

    // Keep the original process-teardown coverage independent of the removal
    // assertions above: submit accepted async work and deliberately leave it
    // outstanding as this fixture unwinds.  In particular, do not synchronize
    // or poll this scene after ApplyPendingUpdates.
    Scene teardown = MakeScene("scene-publication-teardown");
    Check(teardown.owner != nullptr, "teardown fixture creates a groom owner");
    if (teardown.owner) {
        teardown.owner->Synchronize();
        teardown.groom->SystemMessage(HdSystemMessageTokens->asyncAllow, nullptr);
        UsdAttribute teardownWidth = teardown.stage->GetAttributeAtPath(
            SdfPath("/Groom/hair/Ops/width.usdGen:width"));
        Check(teardownWidth.Set(0.20f, UsdTimeCode(4.0)),
              "teardown fixture authors accepted async width work");
        teardown.indices.stageSceneIndex->SetTime(UsdTimeCode(4.0));
        teardown.indices.stageSceneIndex->ApplyPendingUpdates();
    }

    std::printf("testUsdGenScenePublication: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
