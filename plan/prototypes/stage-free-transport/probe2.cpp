// Probe 2: invalidation / time transport for stage-free operator parameters.
//  - what notices reach a downstream scene index for edits to schema attrs of
//    a codeless prim with no adapter, vs. primvars: attrs
//  - what SetTime dirties (per-frame cost of a Ts-spline parameter)
//  - what ArNotice::ResolverChanged does to asset-path-dependent properties
//  - relationship primvars are Get-able but not enumerated
//  - SetStage(nullptr) / SetStage(stage) lifecycle
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
        for (auto &x : e) std::cout << "    ~REN " << x.oldPrimPath << "\n";
        n += e.size();
    }
    size_t n = 0;
    void Reset() { n = 0; }
};

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
    // Force population by pulling the root subtree.
    std::function<void(const SdfPath&)> pull = [&](const SdfPath &p) {
        terminal->GetPrim(p);
        for (const SdfPath &c : terminal->GetChildPrimPaths(p)) pull(c);
    };
    pull(SdfPath::AbsoluteRootPath());

    UsdPrim clump = stage->GetPrimAtPath(SdfPath("/World/Clump"));

    auto step = [&](const char *label, std::function<void()> f) {
        std::cout << "\n--- " << label << " ---\n";
        rec.Reset();
        f();
        sis.stageSceneIndex->ApplyPendingUpdates();
        if (rec.n == 0) std::cout << "    (no notices)\n";
    };

    step("A: author /World/Clump.usdGen:clumpRadius = 0.42 (schema attr, no adapter)",
         [&]{ clump.GetAttribute(TfToken("usdGen:clumpRadius")).Set(0.42f); });

    step("B: author /World/Clump.usdGen:seed = 99 (schema attr, no adapter)",
         [&]{ clump.GetAttribute(TfToken("usdGen:seed")).Set(99); });

    step("C: author /World/Clump.primvars:usdGen:rampKnots (primvar attr)",
         [&]{ VtVec2fArray v = { GfVec2f(0,0), GfVec2f(1,1) };
              clump.GetAttribute(TfToken("primvars:usdGen:rampKnots")).Set(v); });

    step("D: SetTime(24)", [&]{ sis.stageSceneIndex->SetTime(UsdTimeCode(24.0)); });
    step("E: SetTime(24) again (same time)",
         [&]{ sis.stageSceneIndex->SetTime(UsdTimeCode(24.0)); });
    step("F: SetTime(2)", [&]{ sis.stageSceneIndex->SetTime(UsdTimeCode(2.0)); });

    step("G: author /World/Scalp.points at time 2",
         [&]{ VtVec3fArray v = { GfVec3f(0,0,1), GfVec3f(1,0,1),
                                 GfVec3f(1,1,1), GfVec3f(0,1,1) };
              stage->GetPrimAtPath(SdfPath("/World/Scalp"))
                   .GetAttribute(TfToken("points")).Set(v, UsdTimeCode(2.0)); });

    step("H: send ArNotice::ResolverChanged (simulates a painted-map re-resolve)",
         [&]{ ArNotice::ResolverChanged().Send(); });

    step("I: add a new operator prim /World/Frizz at runtime",
         [&]{ stage->DefinePrim(SdfPath("/World/Frizz"),
                                TfToken("UsdGenProbeOperator")); });

    step("J: change /World/Clump type name to UsdGenProbeOperator",
         [&]{ stage->GetPrimAtPath(SdfPath("/World/Clump"))
                   .SetTypeName(TfToken("UsdGenProbeOperator")); });

    std::cout << "\n--- K: relationship primvar visibility ---\n";
    {
        HdSceneIndexPrim p = terminal->GetPrim(SdfPath("/World/Clump"));
        auto pv = HdContainerDataSource::Cast(
            p.dataSource->Get(TfToken("primvars")));
        std::cout << "    primvars GetNames():";
        for (const TfToken &n : pv->GetNames()) std::cout << " " << n;
        std::cout << "\n";
        auto rel = pv->Get(TfToken("usdGen:surface"));
        std::cout << "    primvars->Get('usdGen:surface') = "
                  << (rel ? "NON-NULL" : "null") << "\n";
        if (auto c = HdContainerDataSource::Cast(rel)) {
            for (const TfToken &n : c->GetNames()) {
                auto s = HdSampledDataSource::Cast(c->Get(n));
                std::cout << "      " << n << " = "
                          << (s ? TfStringify(s->GetValue(0)) : "?") << "\n";
            }
        }
    }

    step("L: SetStage(nullptr)", [&]{ sis.stageSceneIndex->SetStage(nullptr); });
    step("M: SetStage(stage) again",
         [&]{ sis.stageSceneIndex->SetStage(stage);
              sis.stageSceneIndex->SetTime(UsdTimeCode(1.0)); });
    std::cout << "    after re-set, GetPrim(/World/Clump) type='"
              << terminal->GetPrim(SdfPath("/World/Clump")).primType << "'"
              << " dataSource=" << (terminal->GetPrim(SdfPath("/World/Clump"))
                                    .dataSource ? "yes" : "no") << "\n";

    terminal->RemoveObserver(HdSceneIndexObserverPtr(&rec));
    std::cout << "\nDONE\n";
    return 0;
}
