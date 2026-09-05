// Probe 5: does a CUSTOM prim-level container data source injected right after
// UsdImagingStageSceneIndex survive the rest of the UsdImaging chain (draw
// mode, prototype propagation + flattening, selection, render settings
// flattening) all the way to the terminal scene index?
// This is the pass-through test for an operator/rest container published by a
// UsdImaging prim or API-schema adapter.
#include "pxr/pxr.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include <iostream>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

TF_DECLARE_REF_PTRS(_InjectingSceneIndex);

class _InjectingSceneIndex : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _InjectingSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &in) {
        return TfCreateRefPtr(new _InjectingSceneIndex(in));
    }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(p);
        if (!prim.dataSource) return prim;
        static HdContainerDataSourceHandle const extra =
            HdRetainedContainerDataSource::New(
                TfToken("usdGen"),
                HdRetainedContainerDataSource::New(
                    TfToken("restPoints"),
                    HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
                        VtVec3fArray{GfVec3f(1,2,3)}),
                    TfToken("opType"),
                    HdRetainedTypedSampledDataSource<TfToken>::New(
                        TfToken("clumpStyler"))));
        prim.dataSource = HdOverlayContainerDataSource::New(
            extra, prim.dataSource);
        return prim;
    }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(p);
    }
protected:
    _InjectingSceneIndex(const HdSceneIndexBaseRefPtr &in)
      : HdSingleInputFilteringSceneIndexBase(in) {}
    void _PrimsAdded(const HdSceneIndexBase&,
                     const HdSceneIndexObserver::AddedPrimEntries &e) override
        { _SendPrimsAdded(e); }
    void _PrimsRemoved(const HdSceneIndexBase&,
                       const HdSceneIndexObserver::RemovedPrimEntries &e) override
        { _SendPrimsRemoved(e); }
    void _PrimsDirtied(const HdSceneIndexBase&,
                       const HdSceneIndexObserver::DirtiedPrimEntries &e) override
        { _SendPrimsDirtied(e); }
};

int main(int argc, char **argv)
{
    UsdStageRefPtr stage = UsdStage::Open(argc > 1 ? argv[1] : "scene.usda");
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.overridesSceneIndexCallback =
        [](HdSceneIndexBaseRefPtr const &in) -> HdSceneIndexBaseRefPtr {
            return _InjectingSceneIndex::New(in);
        };
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    sis.stageSceneIndex->SetTime(UsdTimeCode(1.0));
    HdSceneIndexBaseRefPtr t = sis.finalSceneIndex;
    std::function<void(const SdfPath&)> walk = [&](const SdfPath &pp) {
        const char *p = pp.GetText();
        HdSceneIndexPrim prim = t->GetPrim(pp);
        auto c = prim.dataSource
            ? HdContainerDataSource::Cast(prim.dataSource->Get(TfToken("usdGen")))
            : nullptr;
        std::cout << p << " primType='" << prim.primType << "' usdGen container = "
                  << (c ? "PRESENT" : "MISSING");
        if (c) {
            std::cout << " {";
            for (const TfToken &n : c->GetNames()) {
                auto s = HdSampledDataSource::Cast(c->Get(n));
                std::cout << n << "=" << (s ? TfStringify(s->GetValue(0)) : "?")
                          << " ";
            }
            std::cout << "}";
        }
        std::cout << "\n";
        for (const SdfPath &c : t->GetChildPrimPaths(pp)) walk(c);
    };
    walk(SdfPath::AbsoluteRootPath());
    return 0;
}
