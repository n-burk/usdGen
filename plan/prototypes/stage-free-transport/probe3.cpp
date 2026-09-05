// Probe 3: lazy time-varying / asset-path-dependent registration.
// Same as probe2 but performs a DEEP pull (evaluating every sampled data
// source) before testing SetTime and ArNotice::ResolverChanged.
#include "pxr/pxr.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/ar/notice.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

class Recorder : public HdSceneIndexObserver
{
public:
    void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries &e) override {
        for (auto &x : e) std::cout << "    +ADD " << x.primPath
            << " type='" << x.primType << "'\n";
        n += e.size();
    }
    void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries &e) override {
        for (auto &x : e) std::cout << "    -REM " << x.primPath << "\n";
        n += e.size();
    }
    void PrimsDirtied(const HdSceneIndexBase&, const DirtiedPrimEntries &e) override {
        for (auto &x : e) {
            std::cout << "    *DIRTY " << x.primPath << " {";
            for (const HdDataSourceLocator &l : x.dirtyLocators)
                std::cout << "'" << l.GetString() << "' ";
            std::cout << "}\n";
        }
        n += e.size();
    }
    void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries &e) override {
        n += e.size();
    }
    size_t n = 0;
    void Reset() { n = 0; }
};

static void DeepPull(const HdDataSourceBaseHandle &ds, int depth)
{
    if (!ds || depth <= 0) return;
    if (auto c = HdContainerDataSource::Cast(ds)) {
        for (const TfToken &n : c->GetNames()) DeepPull(c->Get(n), depth - 1);
        return;
    }
    if (auto v = HdVectorDataSource::Cast(ds)) {
        for (size_t i = 0; i < v->GetNumElements(); ++i)
            DeepPull(v->GetElement(i), depth - 1);
        return;
    }
    if (auto s = HdSampledDataSource::Cast(ds)) { s->GetValue(0.0); }
}

int main(int argc, char **argv)
{
    const std::string sceneFile = (argc > 1) ? argv[1] : "scene.usda";
    UsdStageRefPtr stage = UsdStage::Open(sceneFile);
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;
    Recorder rec;
    terminal->AddObserver(HdSceneIndexObserverPtr(&rec));
    sis.stageSceneIndex->SetTime(UsdTimeCode(1.0));

    std::function<void(const SdfPath&)> pull = [&](const SdfPath &p) {
        DeepPull(terminal->GetPrim(p).dataSource, 8);
        for (const SdfPath &c : terminal->GetChildPrimPaths(p)) pull(c);
    };

    auto step = [&](const char *label, std::function<void()> f) {
        std::cout << "\n--- " << label << " ---\n";
        rec.Reset();
        f();
        sis.stageSceneIndex->ApplyPendingUpdates();
        if (rec.n == 0) std::cout << "    (no notices)\n";
    };

    std::cout << "--- deep pull of whole scene at t=1 ---\n";
    rec.Reset();
    pull(SdfPath::AbsoluteRootPath());
    std::cout << "    notices during pull: " << rec.n << "\n";

    step("D: SetTime(24) after deep pull", [&]{
        sis.stageSceneIndex->SetTime(UsdTimeCode(24.0)); });
    step("E: SetTime(2)", [&]{
        sis.stageSceneIndex->SetTime(UsdTimeCode(2.0)); });
    step("H: ArNotice::ResolverChanged().Send()",
         [&]{ ArNotice::ResolverChanged().Send(); });
    step("H2: stage->Reload()", [&]{ stage->Reload(); });

    std::cout << "\nDONE\n";
    return 0;
}
