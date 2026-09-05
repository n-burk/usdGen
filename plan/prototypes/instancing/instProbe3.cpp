// Probe 3: DOWNSTREAM (post-propagation) synthesis of a NESTED instancer:
// hair-card instancer placed inside the propagated native prototype of a
// natively-instanced scalp.  Verifies the instancer parent chain survives
// Hydra 1.0 emulation.
#include "pxr/pxr.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/usdPrimInfoSchema.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/instancerTopologySchema.h"
#include "pxr/imaging/hd/instancedBySchema.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/purposeSchema.h"
#include "pxr/imaging/hd/extentSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/sceneIndexPrimView.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/unitTestNullRenderDelegate.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/rprim.h"
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

TF_DECLARE_REF_PTRS(_NestedSceneIndex);

class _NestedSceneIndex : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _NestedSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &in,
                                       const SdfPath &niProto,
                                       const SdfPath &niInstancer) {
        return TfCreateRefPtr(new _NestedSceneIndex(in, niProto, niInstancer));
    }
    SdfPath Inst()  const { return _niProto.AppendChild(TfToken("HairCardInst")); }
    SdfPath Proto() const { return Inst().AppendChild(TfToken("Proto")); }

    HdSceneIndexPrim GetPrim(const SdfPath &p) const override
    {
        if (p == Inst())  return { HdInstancerTokens->instancer, _Inst() };
        if (p == Proto()) return { HdPrimTypeTokens->basisCurves, _Proto() };
        return _GetInputSceneIndex()->GetPrim(p);
    }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override
    {
        SdfPathVector r = _GetInputSceneIndex()->GetChildPrimPaths(p);
        if (p == _niProto) r.push_back(Inst());
        if (p == Inst())   r.push_back(Proto());
        return r;
    }
protected:
    _NestedSceneIndex(HdSceneIndexBaseRefPtr const &in, const SdfPath &niProto,
                      const SdfPath &niInstancer)
      : HdSingleInputFilteringSceneIndexBase(in)
      , _niProto(niProto), _niInstancer(niInstancer) {}
    void _PrimsAdded(const HdSceneIndexBase &,
        const HdSceneIndexObserver::AddedPrimEntries &e) override {
        HdSceneIndexObserver::AddedPrimEntries out = e;
        for (const auto &en : e) if (en.primPath == _niProto) {
            out.push_back({Inst(), HdInstancerTokens->instancer});
            out.push_back({Proto(), HdPrimTypeTokens->basisCurves});
        }
        _SendPrimsAdded(out);
    }
    void _PrimsRemoved(const HdSceneIndexBase &,
        const HdSceneIndexObserver::RemovedPrimEntries &e) override
    { _SendPrimsRemoved(e); }
    void _PrimsDirtied(const HdSceneIndexBase &,
        const HdSceneIndexObserver::DirtiedPrimEntries &e) override
    { _SendPrimsDirtied(e); }
private:
    static HdContainerDataSourceHandle _InstancedBy(const SdfPath &inst,
                                                    const SdfPath &root) {
        return HdRetainedContainerDataSource::New(
            HdInstancedBySchema::GetSchemaToken(),
            HdInstancedBySchema::Builder()
              .SetPaths(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>
                        ::New({inst}))
              .SetPrototypeRoots(
                  HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({root}))
              .Build());
    }
    HdContainerDataSourceHandle _Inst() const {
        std::vector<HdDataSourceBaseHandle> idx;
        idx.push_back(HdRetainedTypedSampledDataSource<VtArray<int>>::New(
                          VtArray<int>({0,1,2,3,4})));
        VtVec3fArray t(5); for (int i=0;i<5;++i) t[i]=GfVec3f(0.2f*i,0,0);
        auto pv=[](HdDataSourceBaseHandle v){
            return HdPrimvarSchema::Builder()
              .SetPrimvarValue(HdSampledDataSource::Cast(v))
              .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                  HdPrimvarSchemaTokens->instance)).Build(); };
        return HdOverlayContainerDataSource::New(
            _InstancedBy(_niInstancer, _niProto),
            HdRetainedContainerDataSource::New(
              HdInstancerTopologySchema::GetSchemaToken(),
              HdInstancerTopologySchema::Builder()
                .SetPrototypes(
                  HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
                    {Proto()}))
                .SetInstanceIndices(
                  HdRetainedSmallVectorDataSource::New(idx.size(), idx.data()))
                .Build(),
              HdPrimvarsSchema::GetSchemaToken(),
              HdRetainedContainerDataSource::New(
                HdInstancerTokens->instanceTranslations,
                pv(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(t))),
              HdXformSchema::GetSchemaToken(),
              HdXformSchema::Builder().SetMatrix(
                HdRetainedTypedSampledDataSource<GfMatrix4d>::New(GfMatrix4d(1.0)))
                .Build(),
              HdVisibilitySchema::GetSchemaToken(),
              HdVisibilitySchema::Builder().SetVisibility(
                HdRetainedTypedSampledDataSource<bool>::New(true)).Build()));
    }
    HdContainerDataSourceHandle _Proto() const {
        VtVec3fArray pts={{0,0,0},{0,0.3f,0},{0,0.6f,0},{0,1,0}};
        VtIntArray vc={4}; VtFloatArray w(4,0.01f);
        auto pv=[](HdDataSourceBaseHandle v,const TfToken&i,const TfToken&r){
            return HdPrimvarSchema::Builder()
              .SetPrimvarValue(HdSampledDataSource::Cast(v))
              .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(i))
              .SetRole(r.IsEmpty()?nullptr:HdPrimvarSchema::BuildRoleDataSource(r))
              .Build(); };
        return HdOverlayContainerDataSource::New(
          _InstancedBy(Inst(), Proto()),
          HdRetainedContainerDataSource::New(
            HdBasisCurvesSchema::GetSchemaToken(),
            HdBasisCurvesSchema::Builder().SetTopology(
              HdBasisCurvesTopologySchema::Builder()
                .SetCurveVertexCounts(
                    HdRetainedTypedSampledDataSource<VtIntArray>::New(vc))
                .SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(
                    HdTokens->bezier))
                .SetType(HdRetainedTypedSampledDataSource<TfToken>::New(
                    HdTokens->cubic))
                .SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(
                    HdTokens->nonperiodic)).Build()).Build(),
            HdPrimvarsSchema::GetSchemaToken(),
            HdRetainedContainerDataSource::New(
              HdPrimvarsSchemaTokens->points,
              pv(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(pts),
                 HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->point),
              HdPrimvarsSchemaTokens->widths,
              pv(HdRetainedTypedSampledDataSource<VtFloatArray>::New(w),
                 HdPrimvarSchemaTokens->vertex, TfToken())),
            HdXformSchema::GetSchemaToken(),
            HdXformSchema::Builder().SetMatrix(
              HdRetainedTypedSampledDataSource<GfMatrix4d>::New(GfMatrix4d(1.0)))
              .Build(),
            HdVisibilitySchema::GetSchemaToken(),
            HdVisibilitySchema::Builder().SetVisibility(
              HdRetainedTypedSampledDataSource<bool>::New(true)).Build(),
            HdPurposeSchema::GetSchemaToken(),
            HdPurposeSchema::Builder().SetPurpose(
              HdRetainedTypedSampledDataSource<TfToken>::New(
                HdRenderTagTokens->geometry)).Build(),
            HdExtentSchema::GetSchemaToken(),
            HdExtentSchema::Builder()
              .SetMin(HdRetainedTypedSampledDataSource<GfVec3d>::New({-1,-1,-1}))
              .SetMax(HdRetainedTypedSampledDataSource<GfVec3d>::New({1,1,1}))
              .Build()));
    }
    SdfPath _niProto, _niInstancer;
};

int main(int argc, char **argv)
{
    UsdStageRefPtr stage = UsdStage::Open(argc>1?argv[1]:"probe.usda");
    UsdImagingCreateSceneIndicesInfo info; info.stage = stage;
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;

    // Discover the propagated native prototype + its instancer.
    SdfPath niProto, niInstancer;
    for (const SdfPath &p : HdSceneIndexPrimView(terminal)) {
        HdSceneIndexPrim prim = terminal->GetPrim(p);
        UsdImagingUsdPrimInfoSchema s =
            UsdImagingUsdPrimInfoSchema::GetFromParent(prim.dataSource);
        if (auto d = s.GetIsNiPrototype()) {
            if (d->GetTypedValue(0)) {
                niProto = p;
                if (auto ib = HdInstancedBySchema::GetFromParent(prim.dataSource)){
                    if (auto pd = ib.GetPaths()) {
                        auto v = pd->GetTypedValue(0);
                        if (!v.empty()) niInstancer = v[0];
                    }
                }
            }
        }
    }
    std::cout << "discovered niProto     = " << niProto << "\n";
    std::cout << "discovered niInstancer = " << niInstancer << "\n";
    if (niProto.IsEmpty()) return 1;

    auto si = _NestedSceneIndex::New(terminal, niProto, niInstancer);
    std::cout << "synth instancer = " << si->Inst() << "\n";
    std::cout << "synth proto     = " << si->Proto() << "\n";

    Hd_UnitTestNullRenderDelegate rd;
    std::unique_ptr<HdRenderIndex> ri(HdRenderIndex::New(&rd, {}));
    ri->InsertSceneIndex(si, SdfPath::AbsoluteRootPath());

    std::cout << "hasInstancer(synth) = " << ri->HasInstancer(si->Inst()) << "\n";
    std::cout << "GetRprim(synth proto) = "
              << (ri->GetRprim(si->Proto()) ? "FOUND":"NULL") << "\n";
    if (HdSceneDelegate *sd = ri->GetSceneDelegateForRprim(si->Proto())) {
        SdfPath i1 = sd->GetInstancerId(si->Proto());
        std::cout << "  proto -> instancer   = " << i1 << "\n";
        std::cout << "  instanceIndices      = "
                  << sd->GetInstanceIndices(i1, si->Proto()) << "\n";
        SdfPath i2 = sd->GetInstancerId(i1);
        std::cout << "  instancer -> parent  = " << i2 << "\n";
        std::cout << "  parent indices for child instancer = "
                  << sd->GetInstanceIndices(i2, i1) << "\n";
        SdfPath i3 = sd->GetInstancerId(i2);
        std::cout << "  parent -> grandparent= '" << i3 << "'\n";
    }
    // total instance count Storm would draw = 5 cards x 2 scalp instances
    return 0;
}
