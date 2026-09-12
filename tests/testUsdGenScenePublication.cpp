// Integration coverage for per-scene publication ownership.  The target is
// registered by the integration owner.
#include "usdGenImaging/groomSceneIndexPlugin.h"
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
                # The legacy CPU Width implementation uses uniform chunks.
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

    first.groom->SystemMessage(HdSystemMessageTokens->asyncAllow, nullptr);
    Check(width.Set(0.16f, UsdTimeCode(3.0)), "async width sample authors successfully");
    first.indices.stageSceneIndex->SetTime(UsdTimeCode(3.0));
    first.indices.stageSceneIndex->ApplyPendingUpdates();
    Check(std::fabs(FirstWidth(first, tile) - 0.12f) < 1e-6f,
          "async edit retains previously delivered geometry before polling");
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

    std::printf("testUsdGenScenePublication: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
