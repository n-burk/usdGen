// Probe 5: CPU cost of the per-frame notice cascade itself (no GPU involved).
// Measures the wall time of the engine's SetTime/ApplyPendingUpdates block
// and the number of dirty entries a terminal plugin must scan, as the scalp
// count grows.  This is the fixed tax the hair plugin's dirty-scan pays
// every frame before any cooking happens.
#include "pxr/pxr.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/mergingSceneIndex.h"
#include "pxr/imaging/hd/noticeBatchingSceneIndex.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hdsi/sceneGlobalsSceneIndex.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include <chrono>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE
namespace {
class _Count; TF_DECLARE_REF_PTRS(_Count);
class _Count : public HdSingleInputFilteringSceneIndexBase {
public:
    static _CountRefPtr New(const HdSceneIndexBaseRefPtr &i){
        return TfCreateRefPtr(new _Count(i)); }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetPrim(p); }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(p); }
    int calls=0; size_t entries=0; size_t matched=0;
    void Reset(){calls=0;entries=0;matched=0;}
protected:
    _Count(const HdSceneIndexBaseRefPtr &i)
        : HdSingleInputFilteringSceneIndexBase(i) {}
    void _PrimsAdded(const HdSceneIndexBase&,
        const HdSceneIndexObserver::AddedPrimEntries&e) override {_SendPrimsAdded(e);}
    void _PrimsRemoved(const HdSceneIndexBase&,
        const HdSceneIndexObserver::RemovedPrimEntries&e) override {_SendPrimsRemoved(e);}
    void _PrimsDirtied(const HdSceneIndexBase&,
        const HdSceneIndexObserver::DirtiedPrimEntries&e) override {
        ++calls; entries += e.size();
        // realistic dependency scan: is this one of our bound scalps?
        static const HdDataSourceLocator pts(TfToken("primvars"),
                                             TfToken("points"));
        for (const auto &x : e) if (x.dirtyLocators.Intersects(pts)) ++matched;
        _SendPrimsDirtied(e);
    }
};
} // namespace

int main(int argc, char **argv)
{
    printf("%8s %7s %10s %10s %9s %14s\n", "scalps", "batch", "dirtyCalls",
           "entries", "matched", "block_ms/frame");
    for (int n : {1, 10, 100, 1000, 5000}) {
      for (bool batch : {false, true}) {
        UsdStageRefPtr stage = UsdStage::CreateInMemory();
        for (int i = 0; i < n; ++i) {
            UsdGeomMesh m = UsdGeomMesh::Define(
                stage, SdfPath(TfStringPrintf("/W/S%d", i)));
            m.CreateFaceVertexCountsAttr().Set(VtIntArray{4});
            m.CreateFaceVertexIndicesAttr().Set(VtIntArray{0,1,2,3});
            for (int t = 1; t <= 30; ++t) {
                VtVec3fArray p = {GfVec3f(0,0,0),GfVec3f(1,0,0),
                                  GfVec3f(1,1,float(t)),GfVec3f(0,1,0)};
                m.CreatePointsAttr().Set(p, UsdTimeCode(t));
            }
        }
        UsdImagingCreateSceneIndicesInfo info; info.stage = stage;
        UsdImagingSceneIndices si = UsdImagingCreateSceneIndices(info);
        HdMergingSceneIndexRefPtr mg = HdMergingSceneIndex::New();
        mg->AddInputScene(si.finalSceneIndex, SdfPath::AbsoluteRootPath());
        HdNoticeBatchingSceneIndexRefPtr nb =
            HdNoticeBatchingSceneIndex::New(mg);
        HdsiSceneGlobalsSceneIndexRefPtr sg =
            HdsiSceneGlobalsSceneIndex::New(nb);
        _CountRefPtr c = _Count::New(sg);
        // warm: pull points on every prim (registers time deps)
        for (const SdfPath &p : c->GetChildPrimPaths(SdfPath("/W"))) {
            HdSceneIndexPrim pr = c->GetPrim(p);
            if (HdPrimvarsSchema pv =
                    HdPrimvarsSchema::GetFromParent(pr.dataSource))
                if (HdPrimvarSchema s = pv.GetPrimvar(
                        HdPrimvarsSchemaTokens->points))
                    if (HdSampledDataSourceHandle v = s.GetPrimvarValue())
                        v->GetValue(0.0f);
        }
        const int FRAMES = 20;
        c->Reset();
        auto t0 = std::chrono::steady_clock::now();
        for (int f = 1; f <= FRAMES; ++f) {
            if (batch) nb->SetBatchingEnabled(true);
            si.postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(true);
            si.stageSceneIndex->ApplyPendingUpdates();
            si.postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(false);
            si.stageSceneIndex->SetTime(UsdTimeCode(f));
            sg->SetCurrentFrame(f);
            if (batch) nb->SetBatchingEnabled(false);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double,std::milli>(t1-t0).count()
                    / FRAMES;
        printf("%8d %7d %10d %10zu %9zu %14.4f\n", n, int(batch),
               c->calls/FRAMES, c->entries/FRAMES, c->matched/FRAMES, ms);
      }
    }
    return 0;
}
