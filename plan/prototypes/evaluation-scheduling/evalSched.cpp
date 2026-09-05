// Probe: notice sequence a renderer-level scene-index plugin observes per
// frame change / per attribute edit, following UsdImagingGLEngine's exact
// order (engine.cpp:485-497, _PreSetTime at 2354-2386).
//
// Chain built here (mirrors the real one):
//
//   UsdImagingCreateSceneIndices(...)            [ends in finalSceneIndex]
//     -> HdMergingSceneIndex                     (renderIndex.cpp:194-198)
//     -> HdNoticeBatchingSceneIndex "post-merging" (renderIndex.cpp:203)
//     -> HdsiSceneGlobalsSceneIndex              (engine.cpp:151-154, phase 0)
//     -> _RecordingSceneIndex   <-- stand-in for the hair plugin
//     -> terminal observer      <-- stand-in for HdSceneIndexAdapterSceneDelegate
//
#include "pxr/pxr.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/plug/registry.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdGeom/xformOp.h"
#include "pxr/usd/usdGeom/scope.h"
#include "pxr/usd/usdGeom/basisCurves.h"

#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/mergingSceneIndex.h"
#include "pxr/imaging/hd/noticeBatchingSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hdsi/sceneGlobalsSceneIndex.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/imaging/hd/containerDataSourceEditor.h"

#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"

#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

PXR_NAMESPACE_USING_DIRECTIVE

// ---------------------------------------------------------------------------
static int g_callIdx = 0;
static bool g_verbose = true;

struct _Rec {
    std::string kind;   // "dirtied" / "added" / "removed"
    int call;
    size_t nEntries;
    std::vector<std::string> lines;
};
static std::vector<_Rec> g_log;

static std::string _Loc(const HdDataSourceLocatorSet &s)
{
    std::string out;
    for (const HdDataSourceLocator &l : s) {
        if (!out.empty()) out += ",";
        out += l.GetString();
    }
    if (out.empty()) out = "<empty>";
    return out;
}

// A filtering scene index standing in for the hair plugin: it logs and
// forwards.  Placed downstream of the scene-globals scene index, exactly
// where HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer would
// place a renderer plugin registered at a phase > 0.
class _RecordingSceneIndex;
TF_DECLARE_REF_PTRS(_RecordingSceneIndex);

class _RecordingSceneIndex : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _RecordingSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &in) {
        return TfCreateRefPtr(new _RecordingSceneIndex(in));
    }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        ++_getPrimCount;
        return _GetInputSceneIndex()->GetPrim(p);
    }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(p);
    }
    mutable std::atomic<int> _getPrimCount{0};

protected:
    _RecordingSceneIndex(const HdSceneIndexBaseRefPtr &in)
        : HdSingleInputFilteringSceneIndexBase(in) {}

    void _PrimsAdded(const HdSceneIndexBase &,
                     const HdSceneIndexObserver::AddedPrimEntries &e) override {
        _Note("added", e.size(), [&](std::vector<std::string> *L){
            for (const auto &x : e) {
                L->push_back(x.primPath.GetString() + "  type=" +
                             x.primType.GetString());
            }
        });
        _SendPrimsAdded(e);
    }
    void _PrimsRemoved(const HdSceneIndexBase &,
                       const HdSceneIndexObserver::RemovedPrimEntries &e) override {
        _Note("removed", e.size(), [&](std::vector<std::string> *L){
            for (const auto &x : e) L->push_back(x.primPath.GetString());
        });
        _SendPrimsRemoved(e);
    }
    void _PrimsDirtied(const HdSceneIndexBase &,
                       const HdSceneIndexObserver::DirtiedPrimEntries &e) override {
        _Note("dirtied", e.size(), [&](std::vector<std::string> *L){
            for (const auto &x : e) {
                L->push_back(x.primPath.GetString() + "  {" +
                             _Loc(x.dirtyLocators) + "}");
            }
        });
        _SendPrimsDirtied(e);
    }
private:
    template <class F>
    void _Note(const char *kind, size_t n, F fill) {
        _Rec r; r.kind = kind; r.call = ++g_callIdx; r.nEntries = n;
        fill(&r.lines);
        g_log.push_back(r);
    }
};

static void _Reset() { g_log.clear(); g_callIdx = 0; }

static void _Dump(const char *label)
{
    printf("\n----- %s -----\n", label);
    if (g_log.empty()) { printf("  (no notices at all)\n"); return; }
    for (const _Rec &r : g_log) {
        printf("  call#%d  Prims%c%s  entries=%zu\n", r.call,
               toupper(r.kind[0]), r.kind.c_str()+1, r.nEntries);
        if (g_verbose) {
            size_t shown = 0;
            for (const std::string &l : r.lines) {
                if (shown++ >= 12) { printf("      ... (%zu more)\n",
                                            r.lines.size()-12); break; }
                printf("      %s\n", l.c_str());
            }
        }
    }
    int nd=0,na=0,nr=0; size_t ed=0;
    for (const _Rec &r : g_log) {
        if (r.kind=="dirtied") { ++nd; ed += r.nEntries; }
        else if (r.kind=="added") ++na; else ++nr;
    }
    printf("  SUMMARY: PrimsDirtied calls=%d (total entries=%zu), "
           "PrimsAdded calls=%d, PrimsRemoved calls=%d\n", nd, ed, na, nr);
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory("evalsched.usda");

    const int N_SCALPS = 3;
    std::vector<UsdGeomMesh> scalps;
    for (int i = 0; i < N_SCALPS; ++i) {
        SdfPath p(TfStringPrintf("/World/Scalp%d", i));
        UsdGeomMesh m = UsdGeomMesh::Define(stage, p);
        VtIntArray fvc = {4};
        VtIntArray fvi = {0,1,2,3};
        m.CreateFaceVertexCountsAttr().Set(fvc);
        m.CreateFaceVertexIndicesAttr().Set(fvi);
        // time-varying points at 1,2,3
        for (int t = 1; t <= 3; ++t) {
            VtVec3fArray pts = {
                GfVec3f(0,0,0), GfVec3f(1,0,0),
                GfVec3f(1,1,float(t)*0.1f), GfVec3f(0,1,0) };
            m.CreatePointsAttr().Set(pts, UsdTimeCode(t));
        }
        // a NON-time-varying primvar we will edit interactively
        m.GetPrim().CreateAttribute(TfToken("primvars:density"),
                                    SdfValueTypeNames->Float)
            .Set(1.0f);
        scalps.push_back(m);
    }
    // A "hair" prim: a BasisCurves standing in for a generator's output prim.
    UsdGeomBasisCurves hair =
        UsdGeomBasisCurves::Define(stage, SdfPath("/World/Hair"));
    VtIntArray vc = {4};
    hair.CreateCurveVertexCountsAttr().Set(vc);
    VtVec3fArray hp = {GfVec3f(0,0,0),GfVec3f(0,0,1),
                       GfVec3f(0,0,2),GfVec3f(0,0,3)};
    hair.CreatePointsAttr().Set(hp);
    hair.GetPrim().CreateAttribute(TfToken("usdGen:clumpScale"),
                                   SdfValueTypeNames->Float).Set(0.5f);

    // --- build the chain ---------------------------------------------------
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    UsdImagingSceneIndices si = UsdImagingCreateSceneIndices(info);

    HdMergingSceneIndexRefPtr merging = HdMergingSceneIndex::New();
    merging->AddInputScene(si.finalSceneIndex, SdfPath::AbsoluteRootPath());

    HdNoticeBatchingSceneIndexRefPtr postMergeBatch =
        HdNoticeBatchingSceneIndex::New(merging);
    postMergeBatch->SetDisplayName("Post-Merging Notice Batching Scene Index");

    HdsiSceneGlobalsSceneIndexRefPtr sgsi =
        HdsiSceneGlobalsSceneIndex::New(postMergeBatch);

    _RecordingSceneIndexRefPtr rec = _RecordingSceneIndex::New(sgsi);

    // terminal observer (stand-in for HdSceneIndexAdapterSceneDelegate)
    struct _Term : public HdSceneIndexObserver {
        int d=0,a=0,r=0;
        void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries&e) override {++a;}
        void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries&e) override {++r;}
        void PrimsDirtied(const HdSceneIndexBase&, const DirtiedPrimEntries&e) override {++d;}
        void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries&e) override {}
    } term;
    rec->AddObserver(HdSceneIndexObserverPtr(&term));

    printf("=== population (SetStage already happened inside "
           "UsdImagingCreateSceneIndices) ===\n");
    printf("prims under /World: ");
    for (const SdfPath &p : rec->GetChildPrimPaths(SdfPath("/World")))
        printf("%s ", p.GetName().c_str());
    printf("\n");

    // Helper reproducing engine.cpp:485-497 exactly.
    auto engineFrame = [&](double frame, bool wrapPostMergeBatch) {
        if (wrapPostMergeBatch) postMergeBatch->SetBatchingEnabled(true);
        // _PreSetTime (engine.cpp:2354-2386)
        {
            // _ScopedHydraNoticeBatch on postInstancingNoticeBatchingSceneIndex
            si.postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(true);
            si.stageSceneIndex->ApplyPendingUpdates();
            si.postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(false);
        }
        // SetTime  (engine.cpp:488)  -- NOT wrapped in a notice batch
        si.stageSceneIndex->SetTime(UsdTimeCode(frame));
        // _SetSceneGlobalsCurrentFrame (engine.cpp:496 -> 2062-2073)
        sgsi->SetCurrentFrame(frame);
        // _PostSetTime: empty (engine.cpp:2389-2393)
        if (wrapPostMergeBatch) postMergeBatch->SetBatchingEnabled(false);
    };

    // Warm pull: walk every prim and pull points/xform, exactly as a render
    // delegate's first Sync would.  This is what registers time dependencies
    // (UsdImagingStageSceneIndex::_StageGlobals::FlagAsTimeVarying is called
    // from data-source pulls, dataSourcePrim.cpp:43/236/327/486).
    auto warmPull = [&](const HdSceneIndexBaseRefPtr &sceneIndex) {
        std::vector<SdfPath> stack{SdfPath::AbsoluteRootPath()};
        size_t pulled = 0;
        while (!stack.empty()) {
            SdfPath p = stack.back(); stack.pop_back();
            HdSceneIndexPrim prim = sceneIndex->GetPrim(p);
            if (prim.dataSource) {
                if (HdPrimvarsSchema pv =
                        HdPrimvarsSchema::GetFromParent(prim.dataSource)) {
                    for (const TfToken &n : pv.GetPrimvarNames()) {
                        if (HdPrimvarSchema s = pv.GetPrimvar(n)) {
                            if (HdSampledDataSourceHandle v =
                                    s.GetPrimvarValue()) {
                                v->GetValue(0.0f); ++pulled;
                            }
                        }
                    }
                }
                if (HdXformSchema x =
                        HdXformSchema::GetFromParent(prim.dataSource)) {
                    if (HdMatrixDataSourceHandle m = x.GetMatrix()) {
                        m->GetValue(0.0f); ++pulled;
                    }
                }
            }
            for (const SdfPath &c : sceneIndex->GetChildPrimPaths(p))
                stack.push_back(c);
        }
        printf("  warmPull: %zu data-source values pulled\n", pulled);
    };

    printf("\n=== WARM PULL (registers time dependencies) ===\n");
    warmPull(rec);

    // ---- S1: first frame set (1.0) ---------------------------------------
    _Reset(); engineFrame(1.0, false);
    _Dump("S1  frame 1.0, engine order, no post-merge batching");

    // ---- S2: frame 1 -> 2 -------------------------------------------------
    _Reset(); engineFrame(2.0, false);
    _Dump("S2  frame 1->2, engine order, no post-merge batching");

    // ---- S3: frame 2 -> 3, with post-merge batching around the whole block
    _Reset(); engineFrame(3.0, true);
    _Dump("S3  frame 2->3, engine order, WITH MergingSceneIndexNoticeBatch"
          " begin/end around the whole block");

    // ---- S4: redraw at the same frame (no change) -------------------------
    _Reset(); engineFrame(3.0, false);
    _Dump("S4  same frame again (3.0) -- idempotence check");

    // ---- S5: one interactive parameter edit + frame unchanged -------------
    _Reset();
    hair.GetPrim().GetAttribute(TfToken("usdGen:clumpScale")).Set(0.9f);
    engineFrame(3.0, false);
    _Dump("S5  edit usdGen:clumpScale on /World/Hair, then engine order "
          "(same frame)");

    // ---- S6: three separate edits then ONE engine pass --------------------
    _Reset();
    for (int i = 0; i < N_SCALPS; ++i) {
        scalps[i].GetPrim().GetAttribute(TfToken("primvars:density"))
            .Set(2.0f + i);
    }
    engineFrame(3.0, false);
    _Dump("S6  three attribute edits coalesced into one ApplyPendingUpdates");

    // ---- S7: edit + frame change in the same engine pass ------------------
    _Reset();
    hair.GetPrim().GetAttribute(TfToken("usdGen:clumpScale")).Set(0.3f);
    engineFrame(4.0, false);
    _Dump("S7  edit + frame change in the SAME engine pass "
          "(does scene-globals arrive last?)");

    // ---- S8: same as S7 but with post-merge batching ----------------------
    _Reset();
    hair.GetPrim().GetAttribute(TfToken("usdGen:clumpScale")).Set(0.4f);
    engineFrame(5.0, true);
    _Dump("S8  edit + frame change WITH post-merge batching");

    // ---- S9: structural edit (define a new prim) --------------------------
    _Reset();
    UsdGeomMesh::Define(stage, SdfPath("/World/Scalp3"));
    engineFrame(6.0, false);
    _Dump("S9  new prim defined (resync) + frame change");

    // ---- S10: does the scene-globals dirty always come last? --------------
    printf("\n----- S10 scene-globals ordering check -----\n");
    printf("  HdSceneGlobalsSchema default prim path = %s\n",
           HdSceneGlobalsSchema::GetDefaultPrimPath().GetText());
    printf("  currentFrame locator = %s\n",
           HdSceneGlobalsSchema::GetCurrentFrameLocator().GetString().c_str());

    // ---- S9b: warm again after the resync, then frame change --------------
    warmPull(rec);
    _Reset(); engineFrame(6.5, false);
    _Dump("S9b post-warm frame change (time-varying deps now registered)");

    _Reset(); engineFrame(6.75, true);
    _Dump("S9c post-warm frame change WITH post-merge batching");

    // ---- S9d: edit + frame change post-warm --------------------------------
    _Reset();
    for (int i = 0; i < N_SCALPS; ++i)
        scalps[i].GetPrim().GetAttribute(TfToken("primvars:density"))
            .Set(9.0f + i);
    engineFrame(6.9, false);
    _Dump("S9d post-warm: 3 edits + frame change, NO post-merge batching");

    _Reset();
    for (int i = 0; i < N_SCALPS; ++i)
        scalps[i].GetPrim().GetAttribute(TfToken("primvars:density"))
            .Set(19.0f + i);
    engineFrame(6.95, true);
    _Dump("S9e post-warm: 3 edits + frame change, WITH post-merge batching");

    // ---- S10b: is the new time visible from inside the dirty handler? ------
    printf("\n----- S10b read-through-from-inside-the-notice-handler -----\n");
    {
        struct _Reader : public HdSceneIndexObserver {
            HdSceneIndexBaseRefPtr si;
            std::vector<std::string> seen;
            void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries&) override {}
            void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries&) override {}
            void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries&) override {}
            void PrimsDirtied(const HdSceneIndexBase&,
                              const DirtiedPrimEntries &e) override {
                HdSceneIndexPrim p = si->GetPrim(SdfPath("/World/Scalp0"));
                std::string s = "  in-handler(" +
                    std::to_string(e.size()) + " entries, first=" +
                    (e.empty() ? std::string("-") : e[0].primPath.GetString()) +
                    ") points[2].z = ";
                if (HdPrimvarsSchema pv =
                        HdPrimvarsSchema::GetFromParent(p.dataSource)) {
                    if (HdPrimvarSchema pts = pv.GetPrimvar(
                            HdPrimvarsSchemaTokens->points)) {
                        if (HdSampledDataSourceHandle v = pts.GetPrimvarValue()) {
                            VtValue val = v->GetValue(0.0f);
                            if (val.IsHolding<VtVec3fArray>()) {
                                s += std::to_string(
                                    val.UncheckedGet<VtVec3fArray>()[2][2]);
                            } else s += "<not vec3f[]>";
                        }
                    }
                }
                seen.push_back(s);
            }
        } reader;
        reader.si = rec;
        rec->AddObserver(HdSceneIndexObserverPtr(&reader));
        _Reset();
        engineFrame(2.0, false);   // points[2].z should read 0.2 at frame 2
        for (const std::string &s : reader.seen) printf("%s\n", s.c_str());
        printf("  (authored points[2].z is 0.1*frame; frame set to 2.0)\n");
        rec->RemoveObserver(HdSceneIndexObserverPtr(&reader));
    }

    // ---- S11: concurrent GetPrim while another thread edits+dirties -------
    // (Demonstrates the shape of the race a synchronous cook must survive.)
    printf("\n----- S11 concurrent GetPrim from N threads -----\n");
    {
        std::atomic<bool> stop{false};
        std::atomic<long> reads{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 4; ++i) {
            ts.emplace_back([&]{
                while (!stop.load()) {
                    HdSceneIndexPrim p = rec->GetPrim(SdfPath("/World/Scalp0"));
                    if (p.dataSource) reads.fetch_add(1);
                }
            });
        }
        for (int f = 7; f < 17; ++f) engineFrame(double(f), false);
        stop.store(true);
        for (auto &t : ts) t.join();
        printf("  %ld concurrent GetPrim calls survived 10 engine frames "
               "(no crash, but note: notices were delivered on the main "
               "thread only).\n", reads.load());
    }

    printf("\nDONE\n");
    return 0;
}
