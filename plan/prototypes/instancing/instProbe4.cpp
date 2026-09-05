// Probe 4: picking round-trip for a DOWNSTREAM-synthesized instancer.
// Shows what HdxPrimOriginInfo::GetFullPath()/ComputeInstancerContext() return
// (a) without a primOrigin data source on the synthesized prims, and
// (b) with one.  No GL needed: HdxPrimOriginInfo only walks data sources.
#include "pxr/pxr.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/instancerTopologySchema.h"
#include "pxr/imaging/hd/instancedBySchema.h"
#include "pxr/imaging/hd/primOriginSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdx/pickTask.h"
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

TF_DECLARE_REF_PTRS(_SI);
class _SI : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _SIRefPtr New(HdSceneIndexBaseRefPtr const &in, bool withOrigin) {
        return TfCreateRefPtr(new _SI(in, withOrigin)); }
    static SdfPath Inst()  { return SdfPath("/World/HairCards/CardInstancer"); }
    static SdfPath Proto() { return SdfPath("/World/HairCards/CardProto"); }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        if (p == Inst()) return { HdInstancerTokens->instancer, _Inst() };
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(p);
        if (p == Proto() && prim.dataSource) {
            prim.dataSource = HdOverlayContainerDataSource::New(
                _InstancedBy(), prim.dataSource);
        }
        return prim;
    }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        SdfPathVector r = _GetInputSceneIndex()->GetChildPrimPaths(p);
        if (p == Inst().GetParentPath()) r.push_back(Inst());
        return r; }
protected:
    _SI(HdSceneIndexBaseRefPtr const &in, bool w)
      : HdSingleInputFilteringSceneIndexBase(in), _withOrigin(w) {}
    void _PrimsAdded(const HdSceneIndexBase&,
        const HdSceneIndexObserver::AddedPrimEntries &e) override
    { _SendPrimsAdded(e); }
    void _PrimsRemoved(const HdSceneIndexBase&,
        const HdSceneIndexObserver::RemovedPrimEntries &e) override
    { _SendPrimsRemoved(e); }
    void _PrimsDirtied(const HdSceneIndexBase&,
        const HdSceneIndexObserver::DirtiedPrimEntries &e) override
    { _SendPrimsDirtied(e); }
private:
    static HdContainerDataSourceHandle _InstancedBy() {
        return HdRetainedContainerDataSource::New(
          HdInstancedBySchema::GetSchemaToken(),
          HdInstancedBySchema::Builder()
            .SetPaths(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
                {Inst()}))
            .SetPrototypeRoots(
                HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
                {Proto()})).Build()); }
    HdContainerDataSourceHandle _Inst() const {
        std::vector<HdDataSourceBaseHandle> idx;
        idx.push_back(HdRetainedTypedSampledDataSource<VtArray<int>>::New(
                          VtArray<int>({0,1,2,3})));
        VtVec3fArray t(4); for (int i=0;i<4;++i) t[i]=GfVec3f(0.25f*i,0,0);
        auto pv=[](HdDataSourceBaseHandle v){
            return HdPrimvarSchema::Builder()
              .SetPrimvarValue(HdSampledDataSource::Cast(v))
              .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                  HdPrimvarSchemaTokens->instance)).Build(); };
        HdContainerDataSourceHandle base = HdRetainedContainerDataSource::New(
          HdInstancerTopologySchema::GetSchemaToken(),
          HdInstancerTopologySchema::Builder()
            .SetPrototypes(
              HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({Proto()}))
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
            HdRetainedTypedSampledDataSource<bool>::New(true)).Build());
        if (!_withOrigin) return base;
        return HdOverlayContainerDataSource::New(
          HdRetainedContainerDataSource::New(
            HdPrimOriginSchema::GetSchemaToken(),
            HdRetainedContainerDataSource::New(
              HdPrimOriginSchemaTokens->scenePath,
              HdRetainedTypedSampledDataSource<HdPrimOriginSchema::OriginPath>
                ::New(HdPrimOriginSchema::OriginPath(
                    SdfPath("/World/Groom/CardInstancer"))))),
          base);
    }
    bool _withOrigin;
};

static void _Report(HdSceneIndexBaseRefPtr si, int instanceIndex,
                    const char *label)
{
    HdxPickHit hit;
    hit.objectId = _SI::Proto();
    hit.instanceIndex = instanceIndex;
    HdxPrimOriginInfo info = HdxPrimOriginInfo::FromPickHit(si, hit);
    std::cout << label << " instanceIndex=" << instanceIndex << "\n";
    std::cout << "   GetFullPath()  = '" << info.GetFullPath() << "'\n";
    HdInstancerContext ctx = info.ComputeInstancerContext();
    std::cout << "   instancerContext (" << ctx.size() << "): ";
    for (auto &c : ctx) std::cout << "(" << c.first << ", " << c.second << ") ";
    std::cout << "\n   #instancerContexts(raw) = "
              << info.instancerContexts.size() << "\n";
    for (auto &c : info.instancerContexts) {
        std::cout << "     instancer=" << c.instancerSceneIndexPath
                  << " instanceId=" << c.instanceId
                  << " instanceScenePath=" << c.instanceSceneIndexPath << "\n";
    }
}

int main(int argc, char **argv)
{
    UsdStageRefPtr stage = UsdStage::Open(argc>1?argv[1]:"probe.usda");
    UsdImagingCreateSceneIndicesInfo info; info.stage = stage;
    HdSceneIndexBaseRefPtr t = UsdImagingCreateSceneIndices(info).finalSceneIndex;

    std::cout << "--- (a) synthesized prims WITHOUT primOrigin ---\n";
    _Report(_SI::New(t, false), 2, "  pick");
    std::cout << "--- (b) synthesized instancer WITH primOrigin ---\n";
    _Report(_SI::New(t, true), 2, "  pick");
    std::cout << "--- (c) stage-authored USD point instancer (control) ---\n";
    {
        HdxPickHit hit;
        hit.objectId = SdfPath(
          "/World/Cards/PI/Prototypes/Card/ForInstancer9c60c467c223c3c0");
        hit.instanceIndex = 1;
        HdxPrimOriginInfo pi = HdxPrimOriginInfo::FromPickHit(t, hit);
        std::cout << "   GetFullPath()  = '" << pi.GetFullPath() << "'\n";
        HdInstancerContext ctx = pi.ComputeInstancerContext();
        std::cout << "   instancerContext (" << ctx.size() << "): ";
        for (auto &c : ctx) std::cout << "(" << c.first << ", " << c.second << ") ";
        std::cout << "\n";
    }
    std::cout << "--- (d) natively instanced scalp (control) ---\n";
    {
        HdxPickHit hit;
        hit.objectId = SdfPath("/UsdNiPropagatedPrototypes/"
          "NoPrimvars___usdUpAxisd827b8678191286/__Prototype_1/UsdNiInstancer/"
          "UsdNiPrototype/ScalpMesh");
        hit.instanceIndex = 1;
        HdxPrimOriginInfo pi = HdxPrimOriginInfo::FromPickHit(t, hit);
        std::cout << "   GetFullPath()  = '" << pi.GetFullPath() << "'\n";
        HdInstancerContext ctx = pi.ComputeInstancerContext();
        std::cout << "   instancerContext (" << ctx.size() << "): ";
        for (auto &c : ctx) std::cout << "(" << c.first << ", " << c.second << ") ";
        std::cout << "\n";
    }
    return 0;
}
