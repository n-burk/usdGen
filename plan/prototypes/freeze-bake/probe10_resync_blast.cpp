// Gap G (d): how wide is the resync blast radius of one freeze?
// 200 sibling curve prims under /Groom; then (1) add a new sibling,
// (2) edit a sibling's points, (3) add a child under an EXISTING sibling,
// (4) re-author the PARENT scope's typeName.  Count Removed/Added/Dirtied.
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/scope.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include <chrono>
#include <cstdio>
PXR_NAMESPACE_USING_DIRECTIVE
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b)
{ return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Rec : public HdSceneIndexObserver {
    int a = 0, r = 0, d = 0;
    void Reset() { a = r = d = 0; }
    void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries &e) override
        { a += int(e.size()); }
    void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries &e) override
        { r += int(e.size()); }
    void PrimsDirtied(const HdSceneIndexBase&, const DirtiedPrimEntries &e) override
        { d += int(e.size()); }
    void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries&) override {}
};

static void MakeCurves(const UsdStagePtr &s, const SdfPath &p, int n)
{
    UsdGeomBasisCurves c = UsdGeomBasisCurves::Define(s, p);
    VtVec3fArray pts(size_t(n) * 8, GfVec3f(0, 0, 0));
    c.CreateCurveVertexCountsAttr().Set(VtIntArray(n, 8));
    c.CreatePointsAttr().Set(pts);
    c.CreateWidthsAttr().Set(VtFloatArray(size_t(n) * 8, 0.002f));
    c.SetWidthsInterpolation(UsdGeomTokens->vertex);
}

int main(int argc, char **argv)
{
    const int siblings = (argc > 1) ? atoi(argv[1]) : 200;
    const int curves = (argc > 2) ? atoi(argv[2]) : 500;
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomScope::Define(stage, SdfPath("/Groom"));
    for (int i = 0; i < siblings; ++i)
        MakeCurves(stage, SdfPath(TfStringPrintf("/Groom/D%03d", i)), curves);

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage; info.addDrawModeSceneIndex = false;
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    Rec rec;
    sis.stageSceneIndex->AddObserver(HdSceneIndexObserverPtr(&rec));
    sis.finalSceneIndex->AddObserver(HdSceneIndexObserverPtr(&rec));
    sis.stageSceneIndex->SetTime(UsdTimeCode(0.0));
    Clock::time_point t0 = Clock::now();
    sis.stageSceneIndex->ApplyPendingUpdates();
    (void)sis.finalSceneIndex->GetChildPrimPaths(SdfPath("/Groom"));
    std::printf("initial populate: %d siblings x %d curves -> added=%d in %.2f ms\n",
                siblings, curves, rec.a, ms(t0, Clock::now()));

    stage->SetEditTarget(UsdEditTarget(stage->GetSessionLayer()));

    struct C { const char *name; std::function<void()> act; };
    std::vector<C> cases = {
      {"add a NEW sibling freeze under /Groom",
       [&]{ MakeCurves(stage, SdfPath("/Groom/Frozen"), curves); }},
      {"edit points on one sibling",
       [&]{ UsdGeomBasisCurves(stage->GetPrimAtPath(SdfPath("/Groom/D000")))
              .GetPointsAttr().Set(VtVec3fArray(size_t(curves)*8, GfVec3f(1,0,0))); }},
      {"add a CHILD under an existing sibling",
       [&]{ MakeCurves(stage, SdfPath("/Groom/D001/Sub"), curves); }},
      {"re-author the PARENT scope typeName",
       [&]{ stage->GetPrimAtPath(SdfPath("/Groom")).SetTypeName(TfToken("Xform")); }},
      {"deactivate one sibling",
       [&]{ stage->GetPrimAtPath(SdfPath("/Groom/D002")).SetActive(false); }},
      {"RemovePrim on a ROOT-layer sibling (session ET)",
       [&]{ stage->RemovePrim(SdfPath("/Groom/D003"));
            std::printf("   [still on stage: %d] ",
                        bool(stage->GetPrimAtPath(SdfPath("/Groom/D003")))); }},
      {"RemovePrim on the SESSION-layer freeze",
       [&]{ stage->RemovePrim(SdfPath("/Groom/Frozen"));
            std::printf("   [still on stage: %d] ",
                        bool(stage->GetPrimAtPath(SdfPath("/Groom/Frozen")))); }},
    };
    for (const C &c : cases) {
        rec.Reset();
        Clock::time_point a0 = Clock::now();
        c.act();
        Clock::time_point a1 = Clock::now();
        sis.stageSceneIndex->ApplyPendingUpdates();
        Clock::time_point a2 = Clock::now();
        std::printf("%-42s added=%-4d removed=%-4d dirtied=%-4d "
                    "author=%.2f ms apply=%.2f ms\n",
                    c.name, rec.a, rec.r, rec.d, ms(a0, a1), ms(a1, a2));
    }
    sis.finalSceneIndex->RemoveObserver(HdSceneIndexObserverPtr(&rec));
    return 0;
}
