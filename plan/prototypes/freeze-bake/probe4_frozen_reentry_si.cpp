// Gap G parts (b) and (d):
//   (b) does an authored *frozen* UsdGeomBasisCurves prim carrying the
//       proposed root-binding / rest / id primvars survive into the Hydra 2.0
//       terminal scene index unchanged?  Dump the data sources.
//   (d) what does one freeze cost the scene index?  Measure the time from the
//       stage edit to terminal GetPrim availability + first full data pull,
//       for a resync (new prim), a points-only edit, and a removal.
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/changeBlock.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec2f.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// ---------------------------------------------------------------- observer
class Recorder : public HdSceneIndexObserver
{
public:
    int added = 0, removed = 0, dirtied = 0;
    Clock::time_point firstNotice;
    bool sawNotice = false;
    void Reset() { added = removed = dirtied = 0; sawNotice = false; }
    void _Stamp() { if (!sawNotice) { firstNotice = Clock::now(); sawNotice = true; } }
    void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries &e) override
        { _Stamp(); added += int(e.size()); }
    void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries &e) override
        { _Stamp(); removed += int(e.size()); }
    void PrimsDirtied(const HdSceneIndexBase&, const DirtiedPrimEntries &e) override
        { _Stamp(); dirtied += int(e.size()); }
    void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries &e) override
        { _Stamp(); }
};

// ------------------------------------------------------------- authoring
struct Arrays {
    VtVec3fArray points, rest;
    VtIntArray counts, ids, skinprim;
    VtFloatArray widths;
    VtVec2fArray skinuv;
};

static Arrays MakeArrays(int n, int cvs)
{
    Arrays a;
    a.points.reserve(n * cvs);
    for (int i = 0; i < n; ++i) {
        const float x = (i % 100) * 0.01f, z = (i / 100) * 0.01f;
        for (int c = 0; c < cvs; ++c)
            a.points.push_back(GfVec3f(x, c * 0.02f, z));
        a.counts.push_back(cvs);
        a.ids.push_back(i);
        a.skinprim.push_back(i % 977);
        a.skinuv.push_back(GfVec2f((i % 31) / 31.0f, (i % 17) / 17.0f));
    }
    a.rest = a.points;
    a.widths.assign(n * cvs, 0.002f);
    return a;
}

static void AuthorFrozen(const UsdStagePtr &stage, const SdfPath &path,
                         const Arrays &a, bool timeSamples)
{
    UsdGeomBasisCurves c = UsdGeomBasisCurves::Define(stage, path);
    c.CreateCurveVertexCountsAttr().Set(a.counts);
    c.CreatePointsAttr().Set(a.points);
    c.CreateWidthsAttr().Set(a.widths);
    c.SetWidthsInterpolation(UsdGeomTokens->vertex);
    c.CreateTypeAttr().Set(UsdGeomTokens->cubic);
    c.CreateBasisAttr().Set(UsdGeomTokens->bspline);
    c.CreateWrapAttr().Set(UsdGeomTokens->pinned);

    UsdGeomPrimvarsAPI pv(c.GetPrim());
    pv.CreatePrimvar(TfToken("rest"), SdfValueTypeNames->Point3fArray,
                     UsdGeomTokens->vertex).Set(a.rest);
    pv.CreatePrimvar(TfToken("skinprim"), SdfValueTypeNames->IntArray,
                     UsdGeomTokens->uniform).Set(a.skinprim);
    pv.CreatePrimvar(TfToken("skinprimuv"), SdfValueTypeNames->TexCoord2fArray,
                     UsdGeomTokens->uniform).Set(a.skinuv);
    pv.CreatePrimvar(TfToken("usdGen:curveId"), SdfValueTypeNames->IntArray,
                     UsdGeomTokens->uniform).Set(a.ids);
    pv.CreatePrimvar(TfToken("usdGen:rootFrame"), SdfValueTypeNames->Matrix4dArray,
                     UsdGeomTokens->uniform);
    // Not a primvar: the operator-side pointer + staleness digest.
    c.GetPrim().SetCustomDataByKey(TfToken("usdGen"),
        VtValue(VtDictionary{{"frozenEpoch", VtValue(std::string("sha1:cafe"))},
                             {"frozenFrom", VtValue(std::string("/Groom/Gen1"))}}));
    if (timeSamples) {
        for (int t = 0; t < 24; ++t) {
            VtVec3fArray p = a.points;
            for (auto &q : p) q[1] += 0.001f * t;
            c.GetPointsAttr().Set(p, UsdTimeCode(double(t)));
        }
    }
}

// ------------------------------------------------------------------ dump
static void DumpPrimvars(const HdSceneIndexBaseRefPtr &si, const SdfPath &p)
{
    HdSceneIndexPrim prim = si->GetPrim(p);
    std::printf("terminal prim %s type=%s\n", p.GetText(),
                prim.primType.GetText());
    if (!prim.dataSource) { std::printf("  (no dataSource)\n"); return; }
    std::printf("  top-level names:");
    for (const TfToken &n : prim.dataSource->GetNames())
        std::printf(" %s", n.GetText());
    std::printf("\n");

    HdPrimvarsSchema pvs = HdPrimvarsSchema::GetFromParent(prim.dataSource);
    if (!pvs) { std::printf("  (no primvars schema)\n"); return; }
    std::printf("  %-26s %-12s %-12s %-10s %s\n",
                "primvar", "interp", "role", "type", "size");
    for (const TfToken &name : pvs.GetPrimvarNames()) {
        HdPrimvarSchema pv = pvs.GetPrimvar(name);
        std::string interp, role, type = "-";
        size_t sz = 0;
        if (auto d = pv.GetInterpolation()) interp = d->GetTypedValue(0).GetString();
        if (auto d = pv.GetRole()) role = d->GetTypedValue(0).GetString();
        if (auto d = pv.GetPrimvarValue()) {
            VtValue v = d->GetValue(0);
            type = v.GetTypeName();
            if (v.IsArrayValued()) sz = v.GetArraySize();
        }
        std::printf("  %-26s %-12s %-12s %-10s %zu\n", name.GetText(),
                    interp.c_str(), role.empty() ? "-" : role.c_str(),
                    type.c_str(), sz);
    }
    HdBasisCurvesSchema bc = HdBasisCurvesSchema::GetFromParent(prim.dataSource);
    if (bc) {
        HdBasisCurvesTopologySchema topo = bc.GetTopology();
        std::string basis, type, wrap;
        if (auto d = topo.GetBasis()) basis = d->GetTypedValue(0).GetString();
        if (auto d = topo.GetType()) type = d->GetTypedValue(0).GetString();
        if (auto d = topo.GetWrap()) wrap = d->GetTypedValue(0).GetString();
        size_t nc = 0;
        if (auto d = topo.GetCurveVertexCounts())
            nc = d->GetTypedValue(0).size();
        std::printf("  topology: type=%s basis=%s wrap=%s curves=%zu\n",
                    type.c_str(), basis.c_str(), wrap.c_str(), nc);
    }
}

static double PullEverything(const HdSceneIndexBaseRefPtr &si, const SdfPath &p)
{
    // Simulates what a render delegate pulls on a resync: topology + every
    // primvar value at time 0.
    Clock::time_point t0 = Clock::now();
    HdSceneIndexPrim prim = si->GetPrim(p);
    size_t bytes = 0;
    HdPrimvarsSchema pvs = HdPrimvarsSchema::GetFromParent(prim.dataSource);
    if (pvs) {
        for (const TfToken &name : pvs.GetPrimvarNames()) {
            if (auto d = pvs.GetPrimvar(name).GetPrimvarValue()) {
                VtValue v = d->GetValue(0);
                if (v.IsArrayValued()) bytes += v.GetArraySize();
            }
        }
    }
    HdBasisCurvesSchema bc = HdBasisCurvesSchema::GetFromParent(prim.dataSource);
    if (bc)
        if (auto d = bc.GetTopology().GetCurveVertexCounts())
            bytes += d->GetTypedValue(0).size();
    double dt = ms(t0, Clock::now());
    (void)bytes;
    return dt;
}

int main(int argc, char **argv)
{
    const int n = (argc > 1) ? atoi(argv[1]) : 10000;
    const int cvs = (argc > 2) ? atoi(argv[2]) : 8;
    std::printf("=== frozen curves: %d curves x %d CVs ===\n", n, cvs);

    Arrays a = MakeArrays(n, cvs);

    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomXform::Define(stage, SdfPath("/Groom"));

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.addDrawModeSceneIndex = false;
    UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;

    Recorder rec;
    terminal->AddObserver(HdSceneIndexObserverPtr(&rec));
    sis.stageSceneIndex->SetTime(UsdTimeCode(0.0));
    sis.stageSceneIndex->ApplyPendingUpdates();

    // ---- FREEZE #1: author into the session layer, then repopulate --------
    stage->SetEditTarget(UsdEditTarget(stage->GetSessionLayer()));
    const SdfPath frozen("/Groom/Frozen");
    rec.Reset();
    Clock::time_point tAuthor0 = Clock::now();
    AuthorFrozen(stage, frozen, a, /*timeSamples=*/false);
    Clock::time_point tAuthor1 = Clock::now();
    sis.stageSceneIndex->ApplyPendingUpdates();
    Clock::time_point tApply = Clock::now();
    double tGet = 0.0;
    { Clock::time_point g0 = Clock::now();
      HdSceneIndexPrim pr = terminal->GetPrim(frozen);
      tGet = ms(g0, Clock::now());
      if (pr.primType != HdPrimTypeTokens->basisCurves)
          std::printf("!! terminal primType is '%s', expected basisCurves\n",
                      pr.primType.GetText()); }
    double tPull = PullEverything(terminal, frozen);
    std::printf("\nFREEZE (author -> terminal):\n"
                "  author to stage        %8.2f ms\n"
                "  ApplyPendingUpdates    %8.2f ms  (added=%d dirtied=%d removed=%d)\n"
                "  GetPrim (terminal)     %8.3f ms\n"
                "  full data pull         %8.2f ms\n"
                "  TOTAL edit->pulled     %8.2f ms\n",
                ms(tAuthor0, tAuthor1), ms(tAuthor1, tApply),
                rec.added, rec.dirtied, rec.removed, tGet, tPull,
                ms(tAuthor0, tApply) + tGet + tPull);

    DumpPrimvars(terminal, frozen);

    // ---- second pull (cached?) -------------------------------------------
    std::printf("  second full pull       %8.2f ms\n",
                PullEverything(terminal, frozen));

    // ---- DEFORM: points-only edit (the DeformWithSurface case) -----------
    {
        VtVec3fArray p = a.points;
        for (auto &q : p) q[0] += 0.01f;
        rec.Reset();
        Clock::time_point d0 = Clock::now();
        UsdGeomBasisCurves(stage->GetPrimAtPath(frozen)).GetPointsAttr().Set(p);
        Clock::time_point d1 = Clock::now();
        sis.stageSceneIndex->ApplyPendingUpdates();
        Clock::time_point d2 = Clock::now();
        double pull = PullEverything(terminal, frozen);
        std::printf("\nDEFORM (points-only edit):\n"
                    "  author points          %8.2f ms\n"
                    "  ApplyPendingUpdates    %8.2f ms  (added=%d dirtied=%d)\n"
                    "  full re-pull           %8.2f ms\n",
                    ms(d0, d1), ms(d1, d2), rec.added, rec.dirtied, pull);
    }

    // ---- UNDO: remove the frozen prim ------------------------------------
    {
        rec.Reset();
        Clock::time_point u0 = Clock::now();
        stage->RemovePrim(frozen);
        Clock::time_point u1 = Clock::now();
        sis.stageSceneIndex->ApplyPendingUpdates();
        Clock::time_point u2 = Clock::now();
        HdSceneIndexPrim pr = terminal->GetPrim(frozen);
        std::printf("\nUNDO (RemovePrim):\n"
                    "  stage RemovePrim       %8.2f ms\n"
                    "  ApplyPendingUpdates    %8.2f ms  (removed=%d added=%d dirtied=%d)\n"
                    "  terminal primType now  '%s'\n",
                    ms(u0, u1), ms(u1, u2), rec.removed, rec.added,
                    rec.dirtied, pr.primType.GetText());
    }

    // ---- REDO: deactivate/reactivate instead ------------------------------
    {
        AuthorFrozen(stage, frozen, a, false);
        sis.stageSceneIndex->ApplyPendingUpdates();
        rec.Reset();
        Clock::time_point v0 = Clock::now();
        stage->GetPrimAtPath(frozen).SetActive(false);
        sis.stageSceneIndex->ApplyPendingUpdates();
        Clock::time_point v1 = Clock::now();
        HdSceneIndexPrim off = terminal->GetPrim(frozen);
        int removedOff = rec.removed;
        rec.Reset();
        stage->GetPrimAtPath(frozen).SetActive(true);
        sis.stageSceneIndex->ApplyPendingUpdates();
        Clock::time_point v2 = Clock::now();
        HdSceneIndexPrim on = terminal->GetPrim(frozen);
        double pull = PullEverything(terminal, frozen);
        std::printf("\nUNDO-BY-DEACTIVATE:\n"
                    "  SetActive(false)+apply %8.2f ms  (removed=%d) type now '%s'\n"
                    "  SetActive(true)+apply  %8.2f ms  (added=%d) type now '%s'\n"
                    "  full re-pull           %8.2f ms\n",
                    ms(v0, v1), removedOff, off.primType.GetText(),
                    ms(v1, v2), rec.added, on.primType.GetText(), pull);
    }

    terminal->RemoveObserver(HdSceneIndexObserverPtr(&rec));
    return 0;
}
