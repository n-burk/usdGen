// Probe 2: insert a "hair/card generator" scene index UPSTREAM of UsdImaging's
// pi/ni prototype propagation (via UsdImagingCreateSceneIndicesInfo::
// overridesSceneIndexCallback, sceneIndices.cpp:222-225) and observe what the
// propagation machinery does for us:
//   (a) curves added under a NATIVE prototype  (/__Prototype_1)
//   (b) curves added under a POINT-INSTANCER prototype
//   (c) an INSTANCER prim we synthesize ourselves, upstream of propagation
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
#include <iostream>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

static std::string _Val(const HdDataSourceBaseHandle &d)
{
    auto s = HdSampledDataSource::Cast(d);
    if (!s) return "<n/a>";
    std::ostringstream os; os << s->GetValue(0.0f);
    std::string r = os.str();
    if (r.size() > 200) r = r.substr(0,200)+"...";
    return r;
}

TF_DECLARE_REF_PTRS(_GenSceneIndex);

class _GenSceneIndex : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _GenSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &in) {
        return TfCreateRefPtr(new _GenSceneIndex(in));
    }

    // parent path -> child name we add
    static const SdfPath &NiProtoParent() {
        static SdfPath p("/__Prototype_1"); return p; }
    static const SdfPath &PiProtoParent() {
        static SdfPath p("/World/Cards/PI/Prototypes/Card"); return p; }
    static const SdfPath &CardInstancerParent() {
        static SdfPath p("/World/HairCards"); return p; }

    HdSceneIndexPrim GetPrim(const SdfPath &p) const override
    {
        if (p == NiProtoParent().AppendChild(TfToken("Hair")) ||
            p == PiProtoParent().AppendChild(TfToken("Hair"))) {
            return { HdPrimTypeTokens->basisCurves, _Curves() };
        }
        if (p == CardInstancerParent().AppendChild(TfToken("CardInstancer"))) {
            return { HdInstancerTokens->instancer,
                     _Instancer(SdfPath("/World/HairCards/CardProto")) };
        }
        // (d) self-contained instancer: prototype is a namespace child
        if (p == SdfPath("/World/SelfCards/Inst")) {
            return { HdInstancerTokens->instancer,
                     _Instancer(SdfPath("/World/SelfCards/Inst/Proto")) };
        }
        if (p == SdfPath("/World/SelfCards") ||
            p == SdfPath("/World/SelfCards/Inst/Proto")) {
            return { p == SdfPath("/World/SelfCards")
                        ? TfToken() : HdPrimTypeTokens->basisCurves,
                     _Curves() };
        }
        // (f) instancer at a SUBPRIM (property) path, as an adapter subprim
        if (p == SdfPath("/World/HairCards.groomInstancer")) {
            return { HdInstancerTokens->instancer,
                     _Instancer(SdfPath("/World/HairCards/SubProto")) };
        }
        if (p == SdfPath("/World/HairCards/SubProto")) {
            return { HdPrimTypeTokens->basisCurves, _Curves() };
        }
        // (e) instancer INSIDE the native prototype
        if (p == SdfPath("/__Prototype_1/NiCardInst")) {
            return { HdInstancerTokens->instancer,
                     _Instancer(SdfPath("/__Prototype_1/NiCardInst/Proto")) };
        }
        if (p == SdfPath("/__Prototype_1/NiCardInst/Proto")) {
            return { HdPrimTypeTokens->basisCurves, _Curves() };
        }
        return _GetInputSceneIndex()->GetPrim(p);
    }

    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override
    {
        SdfPathVector r = _GetInputSceneIndex()->GetChildPrimPaths(p);
        if (p == NiProtoParent() || p == PiProtoParent()) {
            r.push_back(p.AppendChild(TfToken("Hair")));
        }
        if (p == CardInstancerParent()) {
            r.push_back(p.AppendChild(TfToken("CardInstancer")));
        }
        if (p == CardInstancerParent()) {
            r.push_back(SdfPath("/World/HairCards.groomInstancer"));
            r.push_back(SdfPath("/World/HairCards/SubProto"));
        }
        if (p == SdfPath("/World")) r.push_back(SdfPath("/World/SelfCards"));
        if (p == SdfPath("/World/SelfCards"))
            r.push_back(SdfPath("/World/SelfCards/Inst"));
        if (p == SdfPath("/World/SelfCards/Inst"))
            r.push_back(SdfPath("/World/SelfCards/Inst/Proto"));
        if (p == NiProtoParent())
            r.push_back(SdfPath("/__Prototype_1/NiCardInst"));
        if (p == SdfPath("/__Prototype_1/NiCardInst"))
            r.push_back(SdfPath("/__Prototype_1/NiCardInst/Proto"));
        return r;
    }
protected:
    _GenSceneIndex(HdSceneIndexBaseRefPtr const &in)
      : HdSingleInputFilteringSceneIndexBase(in) {}
    void _PrimsAdded(const HdSceneIndexBase &,
        const HdSceneIndexObserver::AddedPrimEntries &e) override {
        HdSceneIndexObserver::AddedPrimEntries out = e;
        for (const auto &en : e) {
            if (en.primPath == NiProtoParent() || en.primPath == PiProtoParent()) {
                out.push_back({en.primPath.AppendChild(TfToken("Hair")),
                               HdPrimTypeTokens->basisCurves});
            }
            if (en.primPath == CardInstancerParent()) {
                out.push_back({en.primPath.AppendChild(TfToken("CardInstancer")),
                               HdInstancerTokens->instancer});
            }
            if (en.primPath == CardInstancerParent()) {
                out.push_back({SdfPath("/World/HairCards.groomInstancer"),
                               HdInstancerTokens->instancer});
                out.push_back({SdfPath("/World/HairCards/SubProto"),
                               HdPrimTypeTokens->basisCurves});
            }
            if (en.primPath == SdfPath("/World")) {
                out.push_back({SdfPath("/World/SelfCards"), TfToken()});
                out.push_back({SdfPath("/World/SelfCards/Inst"),
                               HdInstancerTokens->instancer});
                out.push_back({SdfPath("/World/SelfCards/Inst/Proto"),
                               HdPrimTypeTokens->basisCurves});
            }
            if (en.primPath == NiProtoParent()) {
                out.push_back({SdfPath("/__Prototype_1/NiCardInst"),
                               HdInstancerTokens->instancer});
                out.push_back({SdfPath("/__Prototype_1/NiCardInst/Proto"),
                               HdPrimTypeTokens->basisCurves});
            }
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
    static HdContainerDataSourceHandle _Curves()
    {
        VtVec3fArray pts = {{0,0,0},{0,0.3f,0},{0,0.6f,0},{0,1,0}};
        VtIntArray vc = {4};
        VtFloatArray w(4, 0.01f);
        auto pv = [](HdDataSourceBaseHandle v, const TfToken &interp,
                     const TfToken &role) {
            return HdPrimvarSchema::Builder()
              .SetPrimvarValue(HdSampledDataSource::Cast(v))
              .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interp))
              .SetRole(role.IsEmpty()?nullptr:HdPrimvarSchema::BuildRoleDataSource(role))
              .Build();
        };
        return HdRetainedContainerDataSource::New(
          HdBasisCurvesSchema::GetSchemaToken(),
          HdBasisCurvesSchema::Builder()
            .SetTopology(HdBasisCurvesTopologySchema::Builder()
               .SetCurveVertexCounts(
                   HdRetainedTypedSampledDataSource<VtIntArray>::New(vc))
               .SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(
                   HdTokens->bezier))
               .SetType(HdRetainedTypedSampledDataSource<TfToken>::New(
                   HdTokens->cubic))
               .SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(
                   HdTokens->nonperiodic))
               .Build())
            .Build(),
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
            .Build());
    }

    static HdContainerDataSourceHandle _Instancer(const SdfPath &proto)
    {
        std::vector<HdDataSourceBaseHandle> idx;
        idx.push_back(HdRetainedTypedSampledDataSource<VtArray<int>>::New(
                          VtArray<int>({0,1,2})));
        VtVec3fArray t = {{0,0,0},{0.3f,0,0},{0.6f,0,0}};
        auto pv = [](HdDataSourceBaseHandle v) {
            return HdPrimvarSchema::Builder()
              .SetPrimvarValue(HdSampledDataSource::Cast(v))
              .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                  HdPrimvarSchemaTokens->instance)).Build(); };
        return HdRetainedContainerDataSource::New(
          HdInstancerTopologySchema::GetSchemaToken(),
          HdInstancerTopologySchema::Builder()
            .SetPrototypes(
               HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
                 {proto}))
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
    }
};

int main(int argc, char **argv)
{
    const std::string stagePath = argc > 1 ? argv[1] : "probe.usda";
    UsdStageRefPtr stage = UsdStage::Open(stagePath);
    if (!stage) { std::cerr << "cannot open\n"; return 1; }

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.overridesSceneIndexCallback =
        [](HdSceneIndexBaseRefPtr const &in) -> HdSceneIndexBaseRefPtr {
            return _GenSceneIndex::New(in); };
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;

    std::cout << "==== terminal tree after upstream-inserted generator ====\n";
    for (const SdfPath &p : HdSceneIndexPrimView(terminal)) {
        HdSceneIndexPrim prim = terminal->GetPrim(p);
        std::cout << p << "  type='" << prim.primType << "'";
        if (prim.dataSource) {
            if (auto ib = HdInstancedBySchema::GetFromParent(prim.dataSource)) {
                std::cout << "\n     instancedBy.paths="
                          << (ib.GetPaths()? _Val(ib.GetPaths()):"<null>")
                          << " roots="
                          << (ib.GetPrototypeRoots()?
                              _Val(ib.GetPrototypeRoots()):"<null>");
            }
            if (auto it = HdInstancerTopologySchema::GetFromParent(prim.dataSource)) {
                std::cout << "\n     topo.prototypes="
                          << (it.GetPrototypes()? _Val(it.GetPrototypes()):"<null>")
                          << " locations="
                          << (it.GetInstanceLocations()?
                              _Val(it.GetInstanceLocations()):"<null>");
                HdIntArrayVectorSchema iv = it.GetInstanceIndices();
                std::cout << " indices=[";
                for (size_t i=0;i<iv.GetNumElements();++i)
                    std::cout << _Val(iv.GetElement(i)) << " ";
                std::cout << "]";
            }
            if (auto pi = UsdImagingUsdPrimInfoSchema::GetFromParent(
                    prim.dataSource)) {
                if (auto d = pi.GetNiPrototypePath())
                    std::cout << "\n     niPrototypePath=" << _Val(d);
                if (auto d = pi.GetIsNiPrototype())
                    std::cout << "\n     isNiPrototype=" << _Val(d);
            }
            if (auto x = HdXformSchema::GetFromParent(prim.dataSource)) {
                if (auto m = x.GetMatrix())
                    std::cout << "\n     xform=" << _Val(m);
            }
        }
        std::cout << "\n";
    }
    return 0;
}
