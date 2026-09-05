// Probe: two per-prim-per-frame costs that HdStBasisCurves pays on a
// DirtyPoints-only update:
//   (1) the "points fastpath" GetPrim() pull through the whole scene-index
//       chain (hdSt/basisCurves.cpp:946-947), and
//   (2) recomputation of the primvar descriptor cache, which
//       HdSceneIndexAdapterSceneDelegate clears whenever a dirtied locator
//       under `primvars` does NOT end in primvarValue/indexedPrimvarValue/
//       indices (sceneIndexAdapterSceneDelegate.cpp:510-522) -- which is
//       exactly what HdContainerDataSourceEditor::ComputeDirtyLocators emits.
#include "pxr/pxr.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/containerDataSourceEditor.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/extentSchema.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexAdapterSceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/vt/array.h"
#include <chrono>
#include <cstdio>
PXR_NAMESPACE_USING_DIRECTIVE
using Clock = std::chrono::steady_clock;
static double us(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b-a).count(); }

// A do-nothing filter, standing in for one link of the grooming chain.
class _PassThrough : public HdSingleInputFilteringSceneIndexBase {
public:
    static TfRefPtr<_PassThrough> New(HdSceneIndexBaseRefPtr const &in) {
        return TfCreateRefPtr(new _PassThrough(in)); }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetPrim(p); }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(p); }
protected:
    _PassThrough(HdSceneIndexBaseRefPtr const &in)
      : HdSingleInputFilteringSceneIndexBase(in) {}
    void _PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries &e) override { _SendPrimsAdded(e); }
    void _PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries &e) override { _SendPrimsRemoved(e); }
    void _PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries &e) override { _SendPrimsDirtied(e); }
};

static HdContainerDataSourceHandle MakePrimvar(
        const VtValue &v, const TfToken &interp, const TfToken &role) {
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtValue>::New(v))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interp))
        .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
        .Build();
}

int main() {
    const int NCURVES = 3125, NCV = 8;             // one 32-way chunk of 100k
    VtIntArray counts(NCURVES, NCV);
    VtVec3fArray pts(NCURVES*NCV);
    VtVec3fArray col(NCURVES);
    VtFloatArray wid(NCURVES*NCV, 0.0015f);
    VtVec2fArray st(NCURVES);
    VtIntArray clump(NCURVES);

    HdContainerDataSourceHandle topo =
        HdBasisCurvesTopologySchema::Builder()
        .SetCurveVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(counts))
        .SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->bspline))
        .SetType(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->cubic))
        .SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->nonperiodic))
        .Build();

    HdContainerDataSourceHandle primvars = HdRetainedContainerDataSource::New(
        HdTokens->points,       MakePrimvar(VtValue(pts),  HdPrimvarSchemaTokens->vertex,  HdPrimvarSchemaTokens->point),
        HdTokens->widths,       MakePrimvar(VtValue(wid),  HdPrimvarSchemaTokens->vertex,  TfToken()),
        HdTokens->displayColor, MakePrimvar(VtValue(col),  HdPrimvarSchemaTokens->uniform, HdPrimvarSchemaTokens->color),
        TfToken("st"),          MakePrimvar(VtValue(st),   HdPrimvarSchemaTokens->uniform, HdPrimvarSchemaTokens->textureCoordinate),
        TfToken("clumpId"),     MakePrimvar(VtValue(clump),HdPrimvarSchemaTokens->uniform, TfToken()));

    HdContainerDataSourceHandle primDs = HdRetainedContainerDataSource::New(
        HdBasisCurvesSchemaTokens->basisCurves,
            HdBasisCurvesSchema::Builder().SetTopology(topo).Build(),
        HdPrimvarsSchemaTokens->primvars, primvars);

    printf("=== PROBE L: GetPrim() through a chain of K filtering scene indices ===\n");
    printf("    (the DirtyPoints fastpath in hdSt/basisCurves.cpp:946-947 does\n"
           "     GetTerminalSceneIndex()->GetPrim(id) once per prim per frame)\n");
    for (int K : {0, 1, 3, 6, 10}) {
        HdRetainedSceneIndexRefPtr retained = HdRetainedSceneIndex::New();
        retained->AddPrims({{SdfPath("/Hair/chunk_0000"),
                             TfToken("basisCurves"), primDs}});
        HdSceneIndexBaseRefPtr si = retained;
        for (int i = 0; i < K; ++i) si = _PassThrough::New(si);
        // warm
        for (int i = 0; i < 100; ++i) si->GetPrim(SdfPath("/Hair/chunk_0000"));
        const int R = 200000;
        auto t0 = Clock::now();
        for (int i = 0; i < R; ++i) {
            HdSceneIndexPrim p = si->GetPrim(SdfPath("/Hair/chunk_0000"));
            (void)p;
        }
        auto t1 = Clock::now();
        printf("  K=%2d filters: GetPrim = %.3f us/call\n", K, us(t0,t1)/R);
    }

    printf("\n=== PROBE M: primvar-descriptor recompute (what the cache clear costs) ===\n");
    {
        const int R = 200000;
        auto t0 = Clock::now();
        size_t acc = 0;
        for (int i = 0; i < R; ++i) {
            // replicate HdSceneIndexAdapterSceneDelegate::_ComputePrimvarDescriptors
            std::array<HdPrimvarDescriptorVector, HdInterpolationCount> descs;
            if (HdPrimvarsSchema pv = HdPrimvarsSchema::GetFromParent(primDs)) {
                for (const TfToken &name : pv.GetPrimvarNames()) {
                    HdPrimvarSchema p = pv.GetPrimvar(name);
                    if (!p) continue;
                    HdPrimvarDescriptor d = HdPrimvarDescriptorFromSchema(name, p);
                    if (d.interpolation >= HdInterpolationCount) continue;
                    descs[d.interpolation].push_back(d);
                }
            }
            acc += descs[HdInterpolationVertex].size();
        }
        auto t1 = Clock::now();
        printf("  5 primvars: recompute = %.3f us/prim  [acc=%zu]\n", us(t0,t1)/R, acc);
    }

    printf("\n=== PROBE N: does the locator set that ComputeDirtyLocators emits\n"
           "    trip HdSceneIndexAdapterSceneDelegate's descriptor-cache clear? ===\n");
    {
        auto trips = [](const HdDataSourceLocatorSet &s) {
            for (HdDataSourceLocator const &loc : s) {
                if (loc.GetFirstElement() == HdPrimvarsSchemaTokens->primvars &&
                    loc.GetLastElement() != HdPrimvarSchemaTokens->primvarValue &&
                    loc.GetLastElement() != HdPrimvarSchemaTokens->indexedPrimvarValue &&
                    loc.GetLastElement() != HdPrimvarSchemaTokens->indices) {
                    return loc.GetString();
                }
            }
            return std::string("(no)");
        };
        HdDataSourceLocatorSet leaf;
        leaf.insert(HdDataSourceLocator(HdPrimvarsSchemaTokens->primvars,
            HdTokens->points, HdPrimvarSchemaTokens->primvarValue));
        printf("  bare leaf set                       -> clears cache? %s\n",
               trips(leaf).c_str());
        printf("  ComputeDirtyLocators(leaf) set      -> clears cache? %s\n",
               trips(HdContainerDataSourceEditor::ComputeDirtyLocators(leaf)).c_str());
    }
    return 0;
}
