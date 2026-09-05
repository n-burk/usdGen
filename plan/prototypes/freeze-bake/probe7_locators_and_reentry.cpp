// Gap G part (b), mechanics:
//  1. What dirty locators does an edit to a FROZEN curves prim produce?  (The
//     reverse-dependency map in the hair SI must key on these.)
//  2. Can a filtering scene index inserted after UsdImagingStageSceneIndex
//     read the frozen prim's points/rest/skinprim straight out of its INPUT
//     scene index (i.e. does a frozen prim re-enter the graph without going
//     back to the stage)?
//  3. Does a relationship (usdGen:source) on an operator prim survive to the
//     scene index at all?
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/scope.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/tokens.h"
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

class LocatorLogger : public HdSceneIndexObserver
{
public:
    void PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries &e) override
        { for (const auto &x : e)
              std::printf("  ADDED   %s (%s)\n", x.primPath.GetText(),
                          x.primType.GetText()); }
    void PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries &e) override
        { for (const auto &x : e)
              std::printf("  REMOVED %s\n", x.primPath.GetText()); }
    void PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries &e) override
        { for (const auto &x : e) {
              std::printf("  DIRTIED %s :", x.primPath.GetText());
              for (const HdDataSourceLocator &l : x.dirtyLocators)
                  std::printf(" [%s]", l.GetString().c_str());
              std::printf("\n"); } }
    void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries&) override {}
};

// A stand-in for the usdGen hair scene index: it wants to read a frozen prim
// that another prim points at.
class ReadFrozenSceneIndex : public HdSingleInputFilteringSceneIndexBase
{
public:
    static HdSceneIndexBaseRefPtr New(const HdSceneIndexBaseRefPtr &in)
        { return TfCreateRefPtr(new ReadFrozenSceneIndex(in)); }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override
        { return _GetInputSceneIndex()->GetPrim(p); }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override
        { return _GetInputSceneIndex()->GetChildPrimPaths(p); }
    size_t ReadFrozen(const SdfPath &frozen) const
    {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(frozen);
        HdPrimvarsSchema pvs = HdPrimvarsSchema::GetFromParent(prim.dataSource);
        if (!pvs) return 0;
        size_t total = 0;
        for (const TfToken &n : {TfToken("points"), TfToken("rest"),
                                 TfToken("skinprim"), TfToken("skinprimuv"),
                                 TfToken("usdGen:curveId")}) {
            HdPrimvarSchema pv = pvs.GetPrimvar(n);
            size_t sz = 0;
            std::string interp;
            if (pv) {
                if (auto d = pv.GetPrimvarValue()) {
                    VtValue v = d->GetValue(0);
                    if (v.IsArrayValued()) sz = v.GetArraySize();
                }
                if (auto d = pv.GetInterpolation())
                    interp = d->GetTypedValue(0).GetString();
            }
            std::printf("    input-SI primvar %-16s size=%-8zu interp=%s\n",
                        n.GetText(), sz, interp.c_str());
            total += sz;
        }
        return total;
    }
protected:
    ReadFrozenSceneIndex(const HdSceneIndexBaseRefPtr &in)
        : HdSingleInputFilteringSceneIndexBase(in) {}
    void _PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries &e) override
        { _SendPrimsAdded(e); }
    void _PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries &e) override
        { _SendPrimsRemoved(e); }
    void _PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries &e) override
        { _SendPrimsDirtied(e); }
};

int main()
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomScope::Define(stage, SdfPath("/Groom"));

    const SdfPath frozen("/Groom/Frozen");
    UsdGeomBasisCurves c = UsdGeomBasisCurves::Define(stage, frozen);
    c.CreateCurveVertexCountsAttr().Set(VtIntArray{4, 4});
    VtVec3fArray pts(8, GfVec3f(0, 0, 0));
    c.CreatePointsAttr().Set(pts);
    c.CreateTypeAttr().Set(UsdGeomTokens->cubic);
    c.CreateBasisAttr().Set(UsdGeomTokens->bspline);
    c.CreateWrapAttr().Set(UsdGeomTokens->pinned);
    UsdGeomPrimvarsAPI pv(c.GetPrim());
    pv.CreatePrimvar(TfToken("rest"), SdfValueTypeNames->Point3fArray,
                     UsdGeomTokens->vertex).Set(pts);
    pv.CreatePrimvar(TfToken("skinprim"), SdfValueTypeNames->IntArray,
                     UsdGeomTokens->uniform).Set(VtIntArray{0, 1});
    pv.CreatePrimvar(TfToken("skinprimuv"), SdfValueTypeNames->TexCoord2fArray,
                     UsdGeomTokens->uniform)
        .Set(VtVec2fArray{GfVec2f(0.1f, 0.2f), GfVec2f(0.3f, 0.4f)});
    pv.CreatePrimvar(TfToken("usdGen:curveId"), SdfValueTypeNames->IntArray,
                     UsdGeomTokens->uniform).Set(VtIntArray{7, 9});

    // The operator prim: an untyped Scope with a relationship pointing at the
    // frozen prim (stand-in for usdGen:source).
    UsdPrim op = UsdGeomScope::Define(stage, SdfPath("/Groom/Clump")).GetPrim();
    op.CreateRelationship(TfToken("usdGen:source")).SetTargets({frozen});

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.addDrawModeSceneIndex = false;
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr stageSi = sis.finalSceneIndex;
    auto filt = TfDynamic_cast<TfRefPtr<ReadFrozenSceneIndex>>(
        ReadFrozenSceneIndex::New(stageSi));

    LocatorLogger log;
    filt->AddObserver(HdSceneIndexObserverPtr(&log));
    sis.stageSceneIndex->SetTime(UsdTimeCode(0.0));
    sis.stageSceneIndex->ApplyPendingUpdates();

    std::printf("== 2. frozen prim read from the filtering SI's INPUT ==\n");
    std::printf("  total elements: %zu\n", filt->ReadFrozen(frozen));

    std::printf("\n== 3. does the operator prim appear at all? ==\n");
    HdSceneIndexPrim opPrim = stageSi->GetPrim(SdfPath("/Groom/Clump"));
    std::printf("  /Groom/Clump primType='%s' names:", opPrim.primType.GetText());
    if (opPrim.dataSource)
        for (const TfToken &n : opPrim.dataSource->GetNames())
            std::printf(" %s", n.GetText());
    std::printf("\n");

    std::printf("\n== 1a. dirty locators for a POINTS edit on the frozen prim ==\n");
    { VtVec3fArray p(8, GfVec3f(1, 0, 0));
      c.GetPointsAttr().Set(p);
      sis.stageSceneIndex->ApplyPendingUpdates(); }

    std::printf("== 1b. dirty locators for a custom PRIMVAR edit (curveId) ==\n");
    { pv.GetPrimvar(TfToken("usdGen:curveId")).Set(VtIntArray{11, 13});
      sis.stageSceneIndex->ApplyPendingUpdates(); }

    std::printf("== 1c. dirty locators for a REST edit ==\n");
    { VtVec3fArray p(8, GfVec3f(0, 2, 0));
      pv.GetPrimvar(TfToken("rest")).Set(p);
      sis.stageSceneIndex->ApplyPendingUpdates(); }

    std::printf("== 1d. relationship retarget on the operator prim ==\n");
    { op.GetRelationship(TfToken("usdGen:source"))
        .SetTargets({SdfPath("/Groom/Other")});
      sis.stageSceneIndex->ApplyPendingUpdates(); }

    std::printf("== 1e. curveVertexCounts (topology) edit ==\n");
    { c.GetCurveVertexCountsAttr().Set(VtIntArray{8});
      sis.stageSceneIndex->ApplyPendingUpdates(); }

    std::printf("== 1f. add a NEW primvar to the frozen prim ==\n");
    { pv.CreatePrimvar(TfToken("usdGen:clumpId"), SdfValueTypeNames->IntArray,
                       UsdGeomTokens->uniform).Set(VtIntArray{1, 2});
      sis.stageSceneIndex->ApplyPendingUpdates(); }

    std::printf("== 1g. relationship on a TYPED (adapter-backed) prim ==\n");
    { c.GetPrim().CreateRelationship(TfToken("usdGen:boundSurface"))
          .SetTargets({SdfPath("/Groom/SkinA")});
      sis.stageSceneIndex->ApplyPendingUpdates();
      std::printf("   -- retarget --\n");
      c.GetPrim().GetRelationship(TfToken("usdGen:boundSurface"))
          .SetTargets({SdfPath("/Groom/SkinB")});
      sis.stageSceneIndex->ApplyPendingUpdates(); }

    std::printf("== 1h. custom (non-primvar) ATTRIBUTE on a typed prim ==\n");
    { c.GetPrim().CreateAttribute(TfToken("usdGen:frozenEpoch"),
                                  SdfValueTypeNames->String)
          .Set(std::string("sha1:aaa"));
      sis.stageSceneIndex->ApplyPendingUpdates();
      std::printf("   -- change value --\n");
      c.GetPrim().GetAttribute(TfToken("usdGen:frozenEpoch"))
          .Set(std::string("sha1:bbb"));
      sis.stageSceneIndex->ApplyPendingUpdates(); }

    std::printf("== 1i. does /Groom/Frozen expose usdGen:boundSurface? ==\n");
    { HdSceneIndexPrim fp = stageSi->GetPrim(frozen);
      std::printf("   names:");
      if (fp.dataSource)
          for (const TfToken &n : fp.dataSource->GetNames())
              std::printf(" %s", n.GetText());
      std::printf("\n"); }

    std::printf("== 1j. frozenEpoch as a CONSTANT string primvar ==\n");
    { pv.CreatePrimvar(TfToken("usdGen:frozenEpoch"),
                       SdfValueTypeNames->String, UsdGeomTokens->constant)
          .Set(std::string("sha1:aaa"));
      sis.stageSceneIndex->ApplyPendingUpdates();
      std::printf("   -- change value --\n");
      pv.GetPrimvar(TfToken("usdGen:frozenEpoch")).Set(std::string("sha1:bbb"));
      sis.stageSceneIndex->ApplyPendingUpdates();
      HdSceneIndexPrim fp = stageSi->GetPrim(frozen);
      HdPrimvarsSchema pvs = HdPrimvarsSchema::GetFromParent(fp.dataSource);
      HdPrimvarSchema e = pvs.GetPrimvar(TfToken("usdGen:frozenEpoch"));
      std::string val, interp;
      if (e) { if (auto d = e.GetPrimvarValue())
                   val = d->GetValue(0).Get<std::string>();
               if (auto d = e.GetInterpolation())
                   interp = d->GetTypedValue(0).GetString(); }
      std::printf("   readback: '%s' interp=%s\n", val.c_str(), interp.c_str()); }

    filt->RemoveObserver(HdSceneIndexObserverPtr(&log));
    return 0;
}
