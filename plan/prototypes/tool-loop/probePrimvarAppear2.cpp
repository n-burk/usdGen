// probePrimvarAppear2.cpp -- with a scene index whose prim data source flips
// under the delegate WITHOUT any PrimsAdded, which dirty notice makes a NEW
// primvar visible to HdSceneIndexAdapterSceneDelegate (i.e. to Storm)?
#include "pxr/pxr.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexAdapterSceneDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/unitTestNullRenderDelegate.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/gf/vec3f.h"
#include <chrono>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

static HdContainerDataSourceHandle
_Primvar(const VtValue &v, const TfToken &interp, const TfToken &role)
{
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtValue>::New(v))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interp))
        .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
        .Build();
}

class _TestSceneIndex;
using _TestSceneIndexPtr = TfRefPtr<_TestSceneIndex>;

class _TestSceneIndex : public HdSceneIndexBase
{
public:
    static _TestSceneIndexPtr New() {
        return TfCreateRefPtr(new _TestSceneIndex);
    }
    bool hasPaint = false;
    SdfPath path{"/Mesh"};

    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        if (p != path) return {TfToken(), nullptr};
        VtVec3fArray pts(4, GfVec3f(0.f));
        std::vector<TfToken> names = { HdPrimvarsSchemaTokens->points };
        std::vector<HdDataSourceBaseHandle> vals = {
            _Primvar(VtValue(pts), HdPrimvarSchemaTokens->vertex,
                     HdPrimvarSchemaTokens->point) };
        if (hasPaint) {
            VtFloatArray paint(4, 0.5f);
            names.push_back(TfToken("usdGen_density"));
            vals.push_back(_Primvar(VtValue(paint),
                                    HdPrimvarSchemaTokens->vertex, TfToken()));
        }
        HdContainerDataSourceHandle pv =
            HdRetainedContainerDataSource::New(names.size(), names.data(),
                                               vals.data());
        TfToken top[] = { HdPrimvarsSchemaTokens->primvars };
        HdDataSourceBaseHandle tv[] = { pv };
        return { HdPrimTypeTokens->mesh,
                 HdRetainedContainerDataSource::New(1, top, tv) };
    }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        if (p == SdfPath::AbsoluteRootPath()) return { path };
        return {};
    }
    void SendAdded() {
        _SendPrimsAdded({{path, HdPrimTypeTokens->mesh}});
    }
    void SendDirty(const HdDataSourceLocatorSet &locs) {
        _SendPrimsDirtied({{path, locs}});
    }
};

int main()
{
    Hd_UnitTestNullRenderDelegate rd;
    const SdfPath path("/Mesh");

    struct Case { const char *label; HdDataSourceLocatorSet locs; };
    std::vector<Case> cases = {
        {"dirty {} (universal locator)",
         HdDataSourceLocatorSet{HdDataSourceLocator()}},
        {"dirty primvars",
         HdDataSourceLocatorSet{HdPrimvarsSchema::GetDefaultLocator()}},
        {"dirty primvars/usdGen_density",
         HdDataSourceLocatorSet{HdDataSourceLocator(
             HdPrimvarsSchemaTokens->primvars, TfToken("usdGen_density"))}},
        {"dirty primvars/usdGen_density/primvarValue",
         HdDataSourceLocatorSet{HdDataSourceLocator(
             HdPrimvarsSchemaTokens->primvars, TfToken("usdGen_density"),
             HdPrimvarSchemaTokens->primvarValue)}},
        {"dirty primvars/points/primvarValue (per-move points push)",
         HdDataSourceLocatorSet{HdDataSourceLocator(
             HdPrimvarsSchemaTokens->primvars, HdPrimvarsSchemaTokens->points,
             HdPrimvarSchemaTokens->primvarValue)}},
    };

    for (auto &c : cases) {
        _TestSceneIndexPtr si = _TestSceneIndex::New();
        std::unique_ptr<HdRenderIndex> ri(HdRenderIndex::New(&rd, {}));
        HdSceneIndexAdapterSceneDelegate del(si, ri.get(),
                                             SdfPath::AbsoluteRootPath());
        si->SendAdded();
        const size_t before =
            del.GetPrimvarDescriptors(path, HdInterpolationVertex).size();
        si->hasPaint = true;                 // the primvar now exists upstream
        si->SendDirty(c.locs);
        auto t0 = std::chrono::steady_clock::now();
        const size_t after =
            del.GetPrimvarDescriptors(path, HdInterpolationVertex).size();
        auto t1 = std::chrono::steady_clock::now();
        printf("%-56s before=%zu after=%zu  %-8s (%.2f us)\n", c.label,
               before, after, after == 2 ? "VISIBLE" : "INVISIBLE",
               std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    // And the re-PrimsAdded route, for comparison.
    {
        _TestSceneIndexPtr si = _TestSceneIndex::New();
        std::unique_ptr<HdRenderIndex> ri(HdRenderIndex::New(&rd, {}));
        HdSceneIndexAdapterSceneDelegate del(si, ri.get(),
                                             SdfPath::AbsoluteRootPath());
        si->SendAdded();
        const size_t before =
            del.GetPrimvarDescriptors(path, HdInterpolationVertex).size();
        si->hasPaint = true;
        auto t0 = std::chrono::steady_clock::now();
        si->SendAdded();                     // the usdRig re-announce
        const size_t after =
            del.GetPrimvarDescriptors(path, HdInterpolationVertex).size();
        auto t1 = std::chrono::steady_clock::now();
        printf("%-56s before=%zu after=%zu  %-8s (%.2f us)\n",
               "re-PrimsAdded with the same prim type", before, after,
               after == 2 ? "VISIBLE" : "INVISIBLE",
               std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    printf("DONE\n");
    return 0;
}
