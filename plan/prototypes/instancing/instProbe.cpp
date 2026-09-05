// Probe: what a downstream (post-UsdImaging) scene index sees for native
// instancing + point instancing, and whether such a scene index can *synthesize*
// a well-formed HdInstancer whose prototype is a stage-authored mesh.
//
// Build: see CMakeLists.txt.  Runs fully headless (no GL).

#include "pxr/pxr.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"

#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/usdPrimInfoSchema.h"

#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/instancerTopologySchema.h"
#include "pxr/imaging/hd/instancedBySchema.h"
#include "pxr/imaging/hd/instanceSchema.h"
#include "pxr/imaging/hd/instanceIndicesSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/purposeSchema.h"
#include "pxr/imaging/hd/primOriginSchema.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/unitTestNullRenderDelegate.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/dirtyBitsTranslator.h"
#include "pxr/imaging/hd/sceneIndexPrimView.h"

#include <iostream>
#include <sstream>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

// ---------------------------------------------------------------------------
// small generic dumper
// ---------------------------------------------------------------------------
static std::string _Val(const HdSampledDataSourceHandle &ds)
{
    if (!ds) return "<null-sampled>";
    VtValue v = ds->GetValue(0.0f);
    std::ostringstream os; os << v;
    std::string s = os.str();
    if (s.size() > 220) s = s.substr(0,220) + "...";
    return s;
}

static void _Dump(const HdDataSourceBaseHandle &ds, const std::string &indent,
                  int depth)
{
    if (!ds) { std::cout << indent << "<null>\n"; return; }
    if (auto c = HdContainerDataSource::Cast(ds)) {
        for (const TfToken &n : c->GetNames()) {
            std::cout << indent << n << ":";
            HdDataSourceBaseHandle child = c->Get(n);
            if (HdContainerDataSource::Cast(child) ||
                HdVectorDataSource::Cast(child)) {
                std::cout << "\n";
                if (depth > 0) _Dump(child, indent + "  ", depth-1);
                else std::cout << indent << "  ...\n";
            } else if (auto s = HdSampledDataSource::Cast(child)) {
                std::cout << " " << _Val(s) << "\n";
            } else {
                std::cout << " <other>\n";
            }
        }
    } else if (auto v = HdVectorDataSource::Cast(ds)) {
        for (size_t i = 0; i < v->GetNumElements(); ++i) {
            std::cout << indent << "[" << i << "]:";
            HdDataSourceBaseHandle e = v->GetElement(i);
            if (auto s = HdSampledDataSource::Cast(e)) {
                std::cout << " " << _Val(s) << "\n";
            } else { std::cout << "\n"; if (depth>0) _Dump(e, indent+"  ", depth-1); }
        }
    } else if (auto s = HdSampledDataSource::Cast(ds)) {
        std::cout << indent << _Val(s) << "\n";
    }
}

static void _DumpLocator(const HdContainerDataSourceHandle &prim,
                         const TfToken &name, int depth = 3)
{
    if (!prim) return;
    HdDataSourceBaseHandle ds = prim->Get(name);
    if (!ds) return;
    std::cout << "    " << name << ":\n";
    _Dump(ds, "      ", depth);
}

static void _DumpTree(const HdSceneIndexBaseRefPtr &si, const char *label)
{
    std::cout << "\n================ " << label << " ================\n";
    static const TfTokenVector interesting = {
        HdInstancerTopologySchema::GetSchemaToken(),
        HdInstancedBySchema::GetSchemaToken(),
        HdInstanceSchema::GetSchemaToken(),
        UsdImagingUsdPrimInfoSchemaTokens->__usdPrimInfo,
        HdXformSchema::GetSchemaToken(),
        HdPrimOriginSchema::GetSchemaToken(),
    };
    for (const SdfPath &p : HdSceneIndexPrimView(si)) {
        HdSceneIndexPrim prim = si->GetPrim(p);
        std::string names;
        if (prim.dataSource) {
            for (const TfToken &n : prim.dataSource->GetNames()) {
                names += n.GetString() + " ";
            }
        }
        std::cout << p << "   type='" << prim.primType << "'\n";
        if (!names.empty()) std::cout << "    [ds] " << names << "\n";
        for (const TfToken &t : interesting) {
            _DumpLocator(prim.dataSource, t);
        }
        // primvars: only names + interpolation
        if (prim.dataSource) {
            if (auto pvs = HdPrimvarsSchema::GetFromParent(prim.dataSource)) {
                std::string pv;
                for (const TfToken &n : pvs.GetPrimvarNames()) {
                    HdPrimvarSchema s = pvs.GetPrimvar(n);
                    std::string interp = "?";
                    if (auto i = s.GetInterpolation()) {
                        interp = i->GetTypedValue(0).GetString();
                    }
                    pv += n.GetString() + "(" + interp + ") ";
                }
                if (!pv.empty()) std::cout << "    [primvars] " << pv << "\n";
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The synthesizing scene index: pretends to be the "usdGen" card-instancing SI.
// It (a) inserts an instancer prim, (b) overlays instancedBy on an existing
// stage-authored mesh, (c) leaves the mesh where it is.
// ---------------------------------------------------------------------------
TF_DECLARE_REF_PTRS(_CardInstancingSceneIndex);

class _CardInstancingSceneIndex : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _CardInstancingSceneIndexRefPtr New(
        HdSceneIndexBaseRefPtr const &input,
        SdfPath const &instancerPath,
        SdfPath const &protoPath)
    {
        return TfCreateRefPtr(
            new _CardInstancingSceneIndex(input, instancerPath, protoPath));
    }

    HdSceneIndexPrim GetPrim(const SdfPath &p) const override
    {
        if (p == _instancerPath) {
            return { HdInstancerTokens->instancer, _BuildInstancer() };
        }
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(p);
        if (p == _protoPath && prim.dataSource) {
            prim.dataSource = HdOverlayContainerDataSource::New(
                _BuildInstancedBy(), prim.dataSource);
        }
        return prim;
    }

    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override
    {
        SdfPathVector r = _GetInputSceneIndex()->GetChildPrimPaths(p);
        if (p == _instancerPath.GetParentPath()) {
            r.push_back(_instancerPath);
        }
        return r;
    }

protected:
    _CardInstancingSceneIndex(HdSceneIndexBaseRefPtr const &input,
                              SdfPath const &instancerPath,
                              SdfPath const &protoPath)
      : HdSingleInputFilteringSceneIndexBase(input)
      , _instancerPath(instancerPath), _protoPath(protoPath) {}

    void _PrimsAdded(const HdSceneIndexBase &,
                     const HdSceneIndexObserver::AddedPrimEntries &e) override
    { _SendPrimsAdded(e); }
    void _PrimsRemoved(const HdSceneIndexBase &,
                       const HdSceneIndexObserver::RemovedPrimEntries &e) override
    { _SendPrimsRemoved(e); }
    void _PrimsDirtied(const HdSceneIndexBase &,
                       const HdSceneIndexObserver::DirtiedPrimEntries &e) override
    { _SendPrimsDirtied(e); }

private:
    HdContainerDataSourceHandle _BuildInstancedBy() const
    {
        return HdRetainedContainerDataSource::New(
            HdInstancedBySchema::GetSchemaToken(),
            HdInstancedBySchema::Builder()
                .SetPaths(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>
                          ::New({_instancerPath}))
                .SetPrototypeRoots(
                    HdRetainedTypedSampledDataSource<VtArray<SdfPath>>
                          ::New({_protoPath}))
                .Build());
    }

    HdContainerDataSourceHandle _BuildInstancer() const
    {
        // topology
        std::vector<HdDataSourceBaseHandle> idx;
        idx.push_back(HdRetainedTypedSampledDataSource<VtArray<int>>::New(
                          VtArray<int>({0,1,2,3})));
        HdContainerDataSourceHandle topo =
            HdInstancerTopologySchema::Builder()
              .SetPrototypes(
                  HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
                      {_protoPath}))
              .SetInstanceIndices(
                  HdRetainedSmallVectorDataSource::New(idx.size(), idx.data()))
              .SetMask(HdRetainedTypedSampledDataSource<VtArray<bool>>::New(
                      VtArray<bool>({true,true,false,true})))
              .Build();

        // instance-rate primvars
        VtVec3fArray t = {{0,0,0},{0.3f,0,0},{0.6f,0,0},{0.9f,0,0}};
        VtQuathArray  r(4, GfQuath(1,0,0,0));
        VtVec3fArray  s(4, GfVec3f(1,1,1));
        VtVec3fArray  col = {{1,0,0},{0,1,0},{0,0,1},{1,1,0}};

        auto mkPv = [](HdDataSourceBaseHandle v, const TfToken &role) {
            return HdPrimvarSchema::Builder()
                .SetPrimvarValue(HdSampledDataSource::Cast(v))
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                     HdPrimvarSchemaTokens->instance))
                .SetRole(role.IsEmpty() ? nullptr
                     : HdPrimvarSchema::BuildRoleDataSource(role))
                .Build();
        };

        HdContainerDataSourceHandle primvars =
            HdRetainedContainerDataSource::New(
                HdInstancerTokens->instanceTranslations,
                mkPv(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(t),
                     TfToken()),
                HdInstancerTokens->instanceRotations,
                mkPv(HdRetainedTypedSampledDataSource<VtQuathArray>::New(r),
                     TfToken()),
                HdInstancerTokens->instanceScales,
                mkPv(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(s),
                     TfToken()),
                TfToken("displayColor"),
                mkPv(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(col),
                     HdPrimvarSchemaTokens->color));

        return HdRetainedContainerDataSource::New(
            HdInstancerTopologySchema::GetSchemaToken(), topo,
            HdPrimvarsSchema::GetSchemaToken(), primvars,
            HdXformSchema::GetSchemaToken(),
            HdXformSchema::Builder()
              .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(
                   GfMatrix4d(1.0)))
              .Build(),
            HdVisibilitySchema::GetSchemaToken(),
            HdVisibilitySchema::Builder()
              .SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(true))
              .Build(),
            HdPurposeSchema::GetSchemaToken(),
            HdPurposeSchema::Builder()
              .SetPurpose(HdRetainedTypedSampledDataSource<TfToken>::New(
                   HdRenderTagTokens->geometry))
              .Build());
    }

    SdfPath _instancerPath, _protoPath;
};

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    const std::string stagePath = argc > 1 ? argv[1] : "probe.usda";
    UsdStageRefPtr stage = UsdStage::Open(stagePath);
    if (!stage) { std::cerr << "cannot open " << stagePath << "\n"; return 1; }

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.addDrawModeSceneIndex = true;
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;

    _DumpTree(terminal, "A. UsdImaging terminal scene index (what a downstream SI sees)");

    // -- append the synthesizing SI ------------------------------------------
    const SdfPath instancerPath("/World/HairCards/CardInstancer");
    const SdfPath protoPath("/World/HairCards/CardProto");
    auto cardSi = _CardInstancingSceneIndex::New(terminal, instancerPath, protoPath);

    std::cout << "\n================ B. synthesized instancer verification ================\n";
    {
        HdSceneIndexPrim ip = cardSi->GetPrim(instancerPath);
        std::cout << "instancer primType = '" << ip.primType << "'\n";
        HdInstancerTopologySchema topo =
            HdInstancerTopologySchema::GetFromParent(ip.dataSource);
        std::cout << "  topo.IsDefined = " << bool(topo) << "\n";
        auto protoDs = topo.GetPrototypes();
        std::cout << "  prototypes = " << (protoDs ? _Val(protoDs) : "<null>") << "\n";
        HdIntArrayVectorSchema iiv = topo.GetInstanceIndices();
        std::cout << "  instanceIndices numElements = " << iiv.GetNumElements() << "\n";
        for (size_t i=0;i<iiv.GetNumElements();++i) {
            std::cout << "    [" << i << "] = " << _Val(iiv.GetElement(i)) << "\n";
        }
        std::cout << "  mask = " << (topo.GetMask()? _Val(topo.GetMask()) : "<null>") << "\n";
        std::cout << "  instanceLocations = "
                  << (topo.GetInstanceLocations()? _Val(topo.GetInstanceLocations())
                                                 : "<null (correct for explicit)>") << "\n";
        VtArray<int> forProto = topo.ComputeInstanceIndicesForProto(protoPath);
        std::cout << "  ComputeInstanceIndicesForProto(" << protoPath << ") = "
                  << forProto << "\n";
        VtArray<int> forWrong =
            topo.ComputeInstanceIndicesForProto(SdfPath("/World/Nope"));
        std::cout << "  ComputeInstanceIndicesForProto(/World/Nope) = "
                  << forWrong << " (size " << forWrong.size() << ")\n";

        HdSceneIndexPrim pp = cardSi->GetPrim(protoPath);
        HdInstancedBySchema ib = HdInstancedBySchema::GetFromParent(pp.dataSource);
        std::cout << "  proto primType = '" << pp.primType << "'\n";
        std::cout << "  proto.instancedBy.paths = "
                  << (ib.GetPaths()? _Val(ib.GetPaths()) : "<null>") << "\n";
        std::cout << "  proto.instancedBy.prototypeRoots = "
                  << (ib.GetPrototypeRoots()? _Val(ib.GetPrototypeRoots()) : "<null>")
                  << "\n";
        // did the overlay preserve upstream data?
        std::cout << "  proto still has 'mesh' ds: "
                  << bool(pp.dataSource->Get(TfToken("mesh"))) << "\n";
        // Are the children of /World/HairCards correct?
        std::cout << "  children of /World/HairCards:";
        for (const SdfPath &c :
                 cardSi->GetChildPrimPaths(SdfPath("/World/HairCards"))) {
            std::cout << " " << c;
        }
        std::cout << "\n";
    }

    // -- push through emulation into a render index --------------------------
    std::cout << "\n================ C. emulation round-trip (null render delegate) ================\n";
    {
        Hd_UnitTestNullRenderDelegate rd;
        HdDriverVector drivers;
        std::unique_ptr<HdRenderIndex> ri(
            HdRenderIndex::New(&rd, drivers));
        ri->InsertSceneIndex(cardSi, SdfPath::AbsoluteRootPath());
        // force sync of the scene index -> render index
        HdSceneIndexBaseRefPtr t2 = ri->GetTerminalSceneIndex();
        std::cout << "renderIndex terminal SI = " << (t2 ? "ok" : "null") << "\n";

        HdInstancer *inst = ri->GetInstancer(instancerPath);
        std::cout << "GetInstancer(" << instancerPath << ") = "
                  << (inst ? "FOUND" : "NULL") << "\n";
        HdRprim const *rp = ri->GetRprim(protoPath);
        std::cout << "GetRprim(" << protoPath << ") = "
                  << (rp ? "FOUND" : "NULL") << "\n";
        if (rp) {
            std::cout << "  rprim.GetInstancerId() [pre-Sync] = '"
                      << rp->GetInstancerId() << "'\n";
        }
        // Ask the emulated scene delegate directly (this is exactly what
        // HdRprim::_UpdateInstancer does during Sync).
        for (const SdfPath &q : { protoPath,
                 SdfPath("/World/Cards/PI/Prototypes/Card/ForInstancer9c60c467c223c3c0") }) {
            SdfPath dlg, inst;
            const bool ok = ri->GetSceneDelegateAndInstancerIds(q, &dlg, &inst);
            std::cout << "  GetSceneDelegateAndInstancerIds(" << q << ") ok=" << ok
                      << " delegate=" << dlg << " instancer='" << inst << "'\n";
            if (HdSceneDelegate *sd = ri->GetSceneDelegateForRprim(q)) {
                std::cout << "    sd->GetInstancerId       = '"
                          << sd->GetInstancerId(q) << "'\n";
                std::cout << "    sd->GetInstanceIndices   = "
                          << sd->GetInstanceIndices(sd->GetInstancerId(q), q) << "\n";
                SdfPathVector protos =
                    sd->GetInstancerPrototypes(sd->GetInstancerId(q));
                std::cout << "    sd->GetInstancerPrototypes = [";
                for (auto &pp2 : protos) std::cout << pp2 << " ";
                std::cout << "]\n";
                std::cout << "    sd->GetTransform(instancer) = "
                          << sd->GetTransform(sd->GetInstancerId(q)) << "\n";
                HdPrimvarDescriptorVector pvs = sd->GetPrimvarDescriptors(
                    sd->GetInstancerId(q), HdInterpolationInstance);
                std::cout << "    instance-rate primvar descriptors: ";
                for (auto &d : pvs) std::cout << d.name << " ";
                std::cout << "\n";
                for (auto &d : pvs) {
                    VtValue v = sd->Get(sd->GetInstancerId(q), d.name);
                    std::ostringstream o; o << v;
                    std::string vs = o.str();
                    if (vs.size() > 120) vs = vs.substr(0,120) + "...";
                    std::cout << "      " << d.name << " = " << vs << "\n";
                }
            }
        }
        // Also report the PI + native-instancing prims the render index got.
        SdfPathVector all = ri->GetRprimIds();
        std::cout << "rprim ids (" << all.size() << "):\n";
        for (auto &p : all) std::cout << "   " << p << "\n";
        // instancers: probe every path the SI reports as type 'instancer'
        std::cout << "instancers present in render index:\n";
        for (const SdfPath &p : HdSceneIndexPrimView(cardSi)) {
            if (cardSi->GetPrim(p).primType == HdInstancerTokens->instancer) {
                std::cout << "   " << p << "  hasInstancer="
                          << ri->HasInstancer(p) << "\n";
            }
        }
    }
    return 0;
}
