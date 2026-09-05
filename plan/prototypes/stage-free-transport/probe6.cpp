// Probe 6: can a Ts spline be transported through Hydra as a whole curve
// (rather than a value sampled at scene time), so that a root-to-tip ramp can
// be authored as `float usdGen:clumpRamp.spline = {...}` and evaluated at a
// normalized u by a downstream, stage-free scene index?
#include "pxr/pxr.h"
#include "pxr/base/ts/spline.h"
#include "pxr/base/ts/knot.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include <iostream>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

using HdTsSplineDataSource = HdTypedSampledDataSource<TsSpline>;

TF_DECLARE_REF_PTRS(_SplineInject);

class _SplineInject : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _SplineInjectRefPtr New(const HdSceneIndexBaseRefPtr &in,
                                   const UsdStageRefPtr &stage) {
        return TfCreateRefPtr(new _SplineInject(in, stage));
    }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(p);
        if (!prim.dataSource) return prim;
        UsdPrim up = _stage->GetPrimAtPath(p);
        if (!up) return prim;
        UsdAttribute a = up.GetAttribute(TfToken("usdGen:clumpRadius"));
        if (!a || !a.HasSpline()) return prim;
        // Publish the whole spline, unsampled, NOT flagged time varying.
        prim.dataSource = HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(
                TfToken("usdGen"),
                HdRetainedContainerDataSource::New(
                    TfToken("clumpRamp"),
                    HdRetainedTypedSampledDataSource<TsSpline>::New(
                        a.GetSpline()))),
            prim.dataSource);
        return prim;
    }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(p);
    }
protected:
    _SplineInject(const HdSceneIndexBaseRefPtr &in, const UsdStageRefPtr &s)
      : HdSingleInputFilteringSceneIndexBase(in), _stage(s) {}
    void _PrimsAdded(const HdSceneIndexBase&,
        const HdSceneIndexObserver::AddedPrimEntries &e) override { _SendPrimsAdded(e); }
    void _PrimsRemoved(const HdSceneIndexBase&,
        const HdSceneIndexObserver::RemovedPrimEntries &e) override { _SendPrimsRemoved(e); }
    void _PrimsDirtied(const HdSceneIndexBase&,
        const HdSceneIndexObserver::DirtiedPrimEntries &e) override { _SendPrimsDirtied(e); }
    UsdStageRefPtr _stage;
};

int main(int argc, char **argv)
{
    UsdStageRefPtr stage = UsdStage::Open(argc > 1 ? argv[1] : "scene.usda");
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.overridesSceneIndexCallback =
        [&](HdSceneIndexBaseRefPtr const &in) -> HdSceneIndexBaseRefPtr {
            return _SplineInject::New(in, stage);
        };
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    sis.stageSceneIndex->SetTime(UsdTimeCode(1.0));
    HdSceneIndexBaseRefPtr t = sis.finalSceneIndex;

    HdSceneIndexPrim prim = t->GetPrim(SdfPath("/World/Clump"));
    auto c = HdContainerDataSource::Cast(
        prim.dataSource->Get(TfToken("usdGen")));
    std::cout << "usdGen container: " << (c ? "PRESENT" : "MISSING") << "\n";
    if (!c) return 1;
    HdDataSourceBaseHandle ds = c->Get(TfToken("clumpRamp"));
    std::cout << "clumpRamp data source: " << (ds ? "PRESENT" : "MISSING")
              << "\n";
    auto sds = HdTsSplineDataSource::Cast(ds);
    std::cout << "cast to HdTypedSampledDataSource<TsSpline>: "
              << (sds ? "OK" : "FAILED") << "\n";
    if (auto samp = HdSampledDataSource::Cast(ds)) {
        VtValue v = samp->GetValue(0.0);
        std::cout << "VtValue type = " << v.GetTypeName()
                  << "  isHoldingTsSpline = " << v.IsHolding<TsSpline>() << "\n";
    }
    if (sds) {
        TsSpline s = sds->GetTypedValue(0.0);
        std::cout << "knot count = " << s.GetKnots().size()
                  << "  valueType = " << s.GetValueType().GetTypeName() << "\n";
        // Evaluate the spline as a root-to-tip RAMP: parameter is u, not time.
        for (double u : {0.0, 1.0, 6.0, 12.0, 18.0, 24.0, 30.0}) {
            double val = 0.0;
            const bool ok = s.Eval(u, &val);
            std::cout << "  Eval(u=" << u << ") ok=" << ok
                      << " value=" << val << "\n";
        }
    }
    return 0;
}
