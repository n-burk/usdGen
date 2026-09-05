// Probe 3: cook counts per interactive event for the three scheduling models.
//
//   M1  EAGER      cook synchronously inside every relevant _PrimsDirtied
//                  (what HdGpGenerativeProceduralResolvingSceneIndex does,
//                   generativeProceduralResolvingSceneIndex.cpp:553-669)
//   M2  LAZY       cook inside GetPrim when marked dirty
//                  (forbidden by usdRig docs/spec.md:1584)
//   M3  DEFERRED   mark dirty in _PrimsDirtied; cook exactly once at a
//                  commit point, publish an immutable snapshot, then dirty
//                  the generated prims.
//                  3a commit = the scene-globals currentFrame notice
//                  3b commit = an explicit app-called Commit() (usdRig model)
//
#include "pxr/pxr.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/mergingSceneIndex.h"
#include "pxr/imaging/hd/noticeBatchingSceneIndex.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hdsi/sceneGlobalsSceneIndex.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <sstream>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// The "hair snapshot": what a cook produces.  Immutable once published.
struct _Snapshot {
    double frame = -1;
    float  density = -1;
    size_t generation = 0;
};
using _SnapshotPtr = std::shared_ptr<const _Snapshot>;

enum class _Model { Eager, Lazy, DeferredOnFrame, DeferredOnCommit };

class _HairSceneIndex;
TF_DECLARE_REF_PTRS(_HairSceneIndex);

class _HairSceneIndex : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _HairSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &in,
                                     _Model m) {
        return TfCreateRefPtr(new _HairSceneIndex(in, m));
    }

    mutable std::atomic<int> cooks{0};
    mutable std::atomic<int> getPrimCalls{0};
    mutable std::atomic<int> dirtyCallbacks{0};
    mutable std::atomic<int> emittedDirties{0};
    mutable std::mutex tidMutex;
    mutable std::set<std::string> cookThreads;

    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        getPrimCalls.fetch_add(1);
        if (_model == _Model::Lazy && p == _hairPath) {
            // FORBIDDEN MODEL: cook during the pull.  Storm calls this from
            // N rprim-sync worker threads (renderIndex.cpp:1849-1853).
            if (_dirty.load()) {
                std::lock_guard<std::mutex> lock(_cookMutex);
                if (_dirty.load()) {
                    const_cast<_HairSceneIndex*>(this)->_Cook();
                    _dirty.store(false);
                }
            }
        }
        return _GetInputSceneIndex()->GetPrim(p);
    }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(p);
    }

    // The explicit commit point an application can call (usdRig's
    // RigExecImaging_SetTime, registry.cpp:1198-1201).
    void Commit() {
        if (_dirty.exchange(false)) {
            _Cook();
            _EmitGeneratedDirty();
        }
    }

    _SnapshotPtr Read() const { return std::atomic_load(&_published); }

protected:
    _HairSceneIndex(const HdSceneIndexBaseRefPtr &in, _Model m)
        : HdSingleInputFilteringSceneIndexBase(in), _model(m) {}

    void _PrimsAdded(const HdSceneIndexBase&,
        const HdSceneIndexObserver::AddedPrimEntries&e) override {
        _SendPrimsAdded(e);
    }
    void _PrimsRemoved(const HdSceneIndexBase&,
        const HdSceneIndexObserver::RemovedPrimEntries&e) override {
        _SendPrimsRemoved(e);
    }
    void _PrimsDirtied(const HdSceneIndexBase&,
        const HdSceneIndexObserver::DirtiedPrimEntries&e) override {
        dirtyCallbacks.fetch_add(1);
        bool touchesDeps = false, isFrame = false;
        for (const auto &x : e) {
            if (x.primPath == HdSceneGlobalsSchema::GetDefaultPrimPath() &&
                x.dirtyLocators.Intersects(
                    HdSceneGlobalsSchema::GetCurrentFrameLocator())) {
                isFrame = true;
            } else if (x.primPath.HasPrefix(SdfPath("/World"))) {
                touchesDeps = true;
            }
        }
        if (touchesDeps || isFrame) _dirty.store(true);

        _SendPrimsDirtied(e);   // always forward first

        switch (_model) {
        case _Model::Eager:
            if (_dirty.exchange(false)) { _Cook(); _EmitGeneratedDirty(); }
            break;
        case _Model::DeferredOnFrame:
            if (isFrame && _dirty.exchange(false)) {
                _Cook(); _EmitGeneratedDirty();
            }
            break;
        case _Model::Lazy:
        case _Model::DeferredOnCommit:
            break;      // nothing here
        }
    }

private:
    void _Cook() {
        cooks.fetch_add(1);
        { std::ostringstream o; o << std::this_thread::get_id();
          std::lock_guard<std::mutex> l(tidMutex); cookThreads.insert(o.str()); }
        // Read the inputs exactly as a real generator would.
        auto s = std::make_shared<_Snapshot>();
        s->generation = cooks.load();
        HdSceneIndexPrim scalp =
            _GetInputSceneIndex()->GetPrim(SdfPath("/World/Scalp0"));
        if (HdPrimvarsSchema pv =
                HdPrimvarsSchema::GetFromParent(scalp.dataSource)) {
            if (HdPrimvarSchema d = pv.GetPrimvar(TfToken("density"))) {
                if (HdSampledDataSourceHandle v = d.GetPrimvarValue()) {
                    VtValue val = v->GetValue(0.0f);
                    if (val.IsHolding<float>()) s->density = val.Get<float>();
                }
            }
        }
        HdSceneIndexPrim g = _GetInputSceneIndex()->GetPrim(
            HdSceneGlobalsSchema::GetDefaultPrimPath());
        if (HdSceneGlobalsSchema sg =
                HdSceneGlobalsSchema::GetFromParent(g.dataSource)) {
            if (HdDoubleDataSourceHandle f = sg.GetCurrentFrame()) {
                s->frame = f->GetTypedValue(0.0f);
            }
        }
        // Atomic publish, exactly usdRig snapshotStore.h:369-371.
        std::atomic_store(&_published, _SnapshotPtr(std::move(s)));
    }
    void _EmitGeneratedDirty() {
        emittedDirties.fetch_add(1);
        _SendPrimsDirtied({{_hairPath,
            HdDataSourceLocator(TfToken("primvars"))}});
    }

    const _Model _model;
    const SdfPath _hairPath{"/World/Hair"};
    mutable std::atomic<bool> _dirty{false};
    mutable std::mutex _cookMutex;
    _SnapshotPtr _published;
};

struct _Harness {
    UsdStageRefPtr stage;
    UsdImagingSceneIndices si;
    HdNoticeBatchingSceneIndexRefPtr postMergeBatch;
    HdsiSceneGlobalsSceneIndexRefPtr sgsi;
    _HairSceneIndexRefPtr hair;
    std::vector<UsdGeomMesh> scalps;

    _Harness(_Model m) {
        stage = UsdStage::CreateInMemory();
        for (int i = 0; i < 3; ++i) {
            UsdGeomMesh mesh = UsdGeomMesh::Define(
                stage, SdfPath(TfStringPrintf("/World/Scalp%d", i)));
            mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{4});
            mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0,1,2,3});
            for (int t = 1; t <= 20; ++t) {
                VtVec3fArray pts = {GfVec3f(0,0,0),GfVec3f(1,0,0),
                                    GfVec3f(1,1,float(t)),GfVec3f(0,1,0)};
                mesh.CreatePointsAttr().Set(pts, UsdTimeCode(t));
            }
            mesh.GetPrim().CreateAttribute(TfToken("primvars:density"),
                SdfValueTypeNames->Float).Set(1.0f);
            scalps.push_back(mesh);
        }
        UsdImagingCreateSceneIndicesInfo info; info.stage = stage;
        si = UsdImagingCreateSceneIndices(info);
        HdMergingSceneIndexRefPtr merging = HdMergingSceneIndex::New();
        merging->AddInputScene(si.finalSceneIndex, SdfPath::AbsoluteRootPath());
        postMergeBatch = HdNoticeBatchingSceneIndex::New(merging);
        sgsi = HdsiSceneGlobalsSceneIndex::New(postMergeBatch);
        hair = _HairSceneIndex::New(sgsi, m);
        // warm pull registers time dependencies
        Warm();
    }
    void Warm() {
        std::vector<SdfPath> stack{SdfPath::AbsoluteRootPath()};
        while (!stack.empty()) {
            SdfPath p = stack.back(); stack.pop_back();
            HdSceneIndexPrim prim = hair->GetPrim(p);
            if (HdPrimvarsSchema pv =
                    HdPrimvarsSchema::GetFromParent(prim.dataSource)) {
                for (const TfToken &n : pv.GetPrimvarNames())
                    if (HdPrimvarSchema s = pv.GetPrimvar(n))
                        if (HdSampledDataSourceHandle v = s.GetPrimvarValue())
                            v->GetValue(0.0f);
            }
            for (const SdfPath &c : hair->GetChildPrimPaths(p))
                stack.push_back(c);
        }
    }
    void EngineFrame(double f, bool batch) {
        if (batch) postMergeBatch->SetBatchingEnabled(true);
        si.postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(true);
        si.stageSceneIndex->ApplyPendingUpdates();
        si.postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(false);
        si.stageSceneIndex->SetTime(UsdTimeCode(f));
        sgsi->SetCurrentFrame(f);
        if (batch) postMergeBatch->SetBatchingEnabled(false);
    }
    // simulate Storm's parallel Rprim sync: N threads pulling GetPrim
    void ParallelSync(int nThreads, int iters) {
        std::vector<std::thread> ts;
        for (int i = 0; i < nThreads; ++i)
            ts.emplace_back([&]{
                for (int k = 0; k < iters; ++k)
                    hair->GetPrim(SdfPath("/World/Hair"));
            });
        for (auto &t : ts) t.join();
    }
};

const char *_Name(_Model m) {
    switch (m) {
    case _Model::Eager: return "M1 EAGER (cook in every notice, hdGp model)";
    case _Model::Lazy:  return "M2 LAZY  (cook in GetPrim, spec-forbidden)";
    case _Model::DeferredOnFrame:
        return "M3a DEFERRED (cook on the sceneGlobals currentFrame notice)";
    case _Model::DeferredOnCommit:
        return "M3b DEFERRED (cook on an explicit app Commit(), usdRig model)";
    }
    return "?";
}

void _Run(_Model m, bool batch)
{
    _Harness h(m);
    // 10 frames, each preceded by one parameter edit -- the worst realistic
    // per-frame interactive case (scrub while a slider is being dragged).
    for (int f = 1; f <= 10; ++f) {
        h.scalps[0].GetPrim().GetAttribute(TfToken("primvars:density"))
            .Set(float(f));
        h.EngineFrame(double(f), batch);
        if (m == _Model::DeferredOnCommit) h.hair->Commit();
        h.ParallelSync(8, 50);      // stands for Storm's parallel Rprim sync
    }
    _SnapshotPtr s = h.hair->Read();
    printf("  %-58s batch=%d  cooks=%3d  dirtyCallbacks=%3d  "
           "emittedDirties=%3d  getPrim=%6d  final(frame=%.0f,density=%.0f)\n",
           _Name(m), int(batch), h.hair->cooks.load(),
           h.hair->dirtyCallbacks.load(), h.hair->emittedDirties.load(),
           h.hair->getPrimCalls.load(),
           s ? s->frame : -1.0, s ? double(s->density) : -1.0);
}

} // namespace

int main()
{
    printf("=== 10 interactive events (1 edit + 1 frame change each), "
           "8-thread pull after each ===\n");
    for (_Model m : {_Model::Eager, _Model::Lazy,
                     _Model::DeferredOnFrame, _Model::DeferredOnCommit}) {
        _Run(m, false);
        _Run(m, true);
    }
    printf("\nIdeal cook count for 10 events = 10.\n");

    // --- which threads run the cook? ---
    printf("\n=== threads that executed a cook (main thread id = %s) ===\n",
           []{ std::ostringstream o; o<<std::this_thread::get_id();
               static std::string s = o.str(); return s.c_str(); }());
    for (_Model m : {_Model::Eager, _Model::Lazy, _Model::DeferredOnCommit}) {
        _Harness h(m);
        for (int f = 1; f <= 5; ++f) {
            h.scalps[0].GetPrim().GetAttribute(TfToken("primvars:density"))
                .Set(float(f));
            h.EngineFrame(double(f), false);
            if (m == _Model::DeferredOnCommit) h.hair->Commit();
            h.ParallelSync(8, 50);
        }
        printf("  %-58s distinct cook threads = %zu %s\n", _Name(m),
               h.hair->cookThreads.size(),
               h.hair->cookThreads.size() > 1 ? "  <-- RENDER THREADS" : "");
    }

    // --- snapshot tearing test: readers vs publisher ---
    printf("\n=== atomic-snapshot consistency under concurrent readers ===\n");
    {
        _Harness h(_Model::DeferredOnCommit);
        std::atomic<bool> stop{false};
        std::atomic<long> reads{0}, torn{0}, nulls{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i) ts.emplace_back([&]{
            while (!stop.load()) {
                _SnapshotPtr s = h.hair->Read();
                if (!s) { nulls.fetch_add(1); continue; }
                reads.fetch_add(1);
                // invariant established by the cook: density == frame
                if (double(s->density) != s->frame) torn.fetch_add(1);
            }
        });
        for (int f = 1; f <= 20; ++f) {
            h.scalps[0].GetPrim().GetAttribute(TfToken("primvars:density"))
                .Set(float(f));
            h.EngineFrame(double(f), false);
            h.hair->Commit();
        }
        stop.store(true);
        for (auto &t : ts) t.join();
        printf("  reads=%ld  torn=%ld  (null-before-first-publish=%ld)\n",
               reads.load(), torn.load(), nulls.load());
        printf("  %s\n", torn.load()==0
               ? "PASS: readers never saw a mixed generation"
               : "FAIL: snapshot tearing observed");
    }
    return 0;
}
