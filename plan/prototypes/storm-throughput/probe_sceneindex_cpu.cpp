// Probe: per-frame CPU cost of the Hydra-2 invalidation path that a grooming
// scene index would drive: ComputeDirtyLocators per prim, and
// HdDependencyForwardingSceneIndex fan-out for N curve prims that all depend on
// one deforming mesh. No GL context required.
#include "pxr/pxr.h"
#include "pxr/imaging/hd/containerDataSourceEditor.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/dependenciesSchema.h"
#include "pxr/imaging/hd/dependencySchema.h"
#include "pxr/imaging/hd/dependencyForwardingSceneIndex.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/base/tf/token.h"

#include <chrono>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Counting terminal observer: stands in for the render index's observer.
class _CountingObserver : public HdSceneIndexObserver {
public:
    size_t dirtiedEntries = 0;
    size_t dirtiedNotices = 0;
    void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries&) override {}
    void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries&) override {}
    void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries&) override {}
    void PrimsDirtied(const HdSceneIndexBase&, const DirtiedPrimEntries &e) override {
        dirtiedNotices++; dirtiedEntries += e.size();
    }
};

int main() {
    // ---------------------------------------------------------------- part 1
    printf("=== PROBE I: HdContainerDataSourceEditor::ComputeDirtyLocators cost ===\n");
    HdDataSourceLocatorSet pointsLeaf;
    pointsLeaf.insert(HdDataSourceLocator(HdPrimvarsSchemaTokens->primvars,
                                          HdTokens->points,
                                          HdPrimvarSchemaTokens->primvarValue));
    {
        HdDataSourceLocatorSet out =
            HdContainerDataSourceEditor::ComputeDirtyLocators(pointsLeaf);
        size_t n = 0; for (auto const &l : out) { (void)l; ++n; }
        printf("  1 leaf 'primvars/points/primvarValue' -> %zu locators:\n", n);
        for (auto const &l : out) printf("     %s\n", l.GetString().c_str());
    }
    for (int N : {1000, 10000, 100000}) {
        auto t0 = Clock::now();
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        entries.reserve(N);
        for (int i = 0; i < N; ++i) {
            entries.emplace_back(
                SdfPath(TfStringPrintf("/Hair/chunk_%d", i)),
                HdContainerDataSourceEditor::ComputeDirtyLocators(pointsLeaf));
        }
        auto t1 = Clock::now();
        printf("  build %6d DirtiedPrimEntries (path + ComputeDirtyLocators) = %.3f ms"
               " (%.2f us/prim)\n", N, ms(t0,t1), ms(t0,t1)*1000.0/N);
    }
    // Compare against reusing one precomputed locator set.
    {
        const HdDataSourceLocatorSet cached =
            HdContainerDataSourceEditor::ComputeDirtyLocators(pointsLeaf);
        for (int N : {100000}) {
            auto t0 = Clock::now();
            HdSceneIndexObserver::DirtiedPrimEntries entries;
            entries.reserve(N);
            for (int i = 0; i < N; ++i)
                entries.emplace_back(SdfPath(TfStringPrintf("/Hair/chunk_%d", i)), cached);
            auto t1 = Clock::now();
            printf("  build %6d entries reusing a CACHED locator set = %.3f ms"
                   " (%.2f us/prim)\n", N, ms(t0,t1), ms(t0,t1)*1000.0/N);
        }
    }
    // SdfPath construction alone, to attribute the cost above.
    {
        int N = 100000;
        auto t0 = Clock::now();
        std::vector<SdfPath> paths; paths.reserve(N);
        for (int i = 0; i < N; ++i) paths.emplace_back(TfStringPrintf("/Hair/chunk_%d", i));
        auto t1 = Clock::now();
        printf("  (reference) %6d SdfPath-from-string constructions = %.3f ms\n", N, ms(t0,t1));
    }

    // ---------------------------------------------------------------- part 2
    printf("\n=== PROBE J: HdDependencyForwardingSceneIndex fan-out, N curve prims -> 1 mesh ===\n");
    static const HdDataSourceLocator meshPointsLoc(
        HdPrimvarsSchemaTokens->primvars, HdTokens->points,
        HdPrimvarSchemaTokens->primvarValue);

    for (int N : {32, 1000, 10000, 100000}) {
        HdRetainedSceneIndexRefPtr retained = HdRetainedSceneIndex::New();

        // One deforming mesh.
        HdRetainedSceneIndex::AddedPrimEntries adds;
        adds.push_back({SdfPath("/Body"), TfToken("mesh"),
                        HdRetainedContainerDataSource::New()});

        // N curve prims, each declaring a dependency on /Body's points.
        HdContainerDataSourceHandle dep =
            HdDependencySchema::Builder()
                .SetDependedOnPrimPath(
                    HdRetainedTypedSampledDataSource<SdfPath>::New(SdfPath("/Body")))
                .SetDependedOnDataSourceLocator(
                    HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
                        meshPointsLoc))
                .SetAffectedDataSourceLocator(
                    HdRetainedTypedSampledDataSource<HdDataSourceLocator>::New(
                        meshPointsLoc))
                .Build();
        HdContainerDataSourceHandle deps =
            HdRetainedContainerDataSource::New(TfToken("bodyPoints"), dep);

        for (int i = 0; i < N; ++i) {
            adds.push_back({SdfPath(TfStringPrintf("/Hair/chunk_%d", i)),
                            TfToken("basisCurves"),
                            HdRetainedContainerDataSource::New(
                                HdDependenciesSchemaTokens->__dependencies, deps)});
        }
        retained->AddPrims(adds);

        HdDependencyForwardingSceneIndexRefPtr fwd =
            HdDependencyForwardingSceneIndex::New(retained);
        _CountingObserver obs;
        fwd->AddObserver(HdSceneIndexObserverPtr(&obs));

        // Dependencies are populated lazily on GetPrim; a real render index
        // pulls every prim once at population time.
        auto tp0 = Clock::now();
        for (int i = 0; i < N; ++i)
            fwd->GetPrim(SdfPath(TfStringPrintf("/Hair/chunk_%d", i)));
        auto tp1 = Clock::now();

        // Now dirty the mesh points once (one deforming-surface frame).
        HdSceneIndexObserver::DirtiedPrimEntries meshDirty;
        meshDirty.emplace_back(SdfPath("/Body"),
                               HdDataSourceLocatorSet{meshPointsLoc});

        double best = 1e9;
        for (int rep = 0; rep < 5; ++rep) {
            obs.dirtiedEntries = 0;
            auto t0 = Clock::now();
            retained->DirtyPrims(meshDirty);
            auto t1 = Clock::now();
            best = std::min(best, ms(t0,t1));
        }
        printf("  N=%6d  lazy dep population (GetPrim x N) = %7.3f ms |"
               "  1 mesh-points dirty -> forwarded entries=%zu in %6.3f ms"
               "  (%.2f us/affected prim)\n",
               N, ms(tp0,tp1), obs.dirtiedEntries, best, best*1000.0/N);

        fwd->RemoveObserver(HdSceneIndexObserverPtr(&obs));
    }

    // ---------------------------------------------------------------- part 3
    printf("\n=== PROBE K: raw notice cost with NO dependency scene index (direct dirty of N prims) ===\n");
    for (int N : {32, 1000, 10000, 100000}) {
        HdRetainedSceneIndexRefPtr retained = HdRetainedSceneIndex::New();
        HdRetainedSceneIndex::AddedPrimEntries adds;
        for (int i = 0; i < N; ++i)
            adds.push_back({SdfPath(TfStringPrintf("/Hair/chunk_%d", i)),
                            TfToken("basisCurves"),
                            HdRetainedContainerDataSource::New()});
        retained->AddPrims(adds);
        _CountingObserver obs;
        retained->AddObserver(HdSceneIndexObserverPtr(&obs));

        const HdDataSourceLocatorSet cached =
            HdContainerDataSourceEditor::ComputeDirtyLocators(
                HdDataSourceLocatorSet{HdDataSourceLocator(
                    HdPrimvarsSchemaTokens->primvars, HdTokens->points,
                    HdPrimvarSchemaTokens->primvarValue)});
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        entries.reserve(N);
        for (int i = 0; i < N; ++i)
            entries.emplace_back(SdfPath(TfStringPrintf("/Hair/chunk_%d", i)), cached);

        double best = 1e9;
        for (int rep = 0; rep < 5; ++rep) {
            auto t0 = Clock::now();
            retained->DirtyPrims(entries);
            auto t1 = Clock::now();
            best = std::min(best, ms(t0,t1));
        }
        printf("  N=%6d  DirtyPrims(N entries) through 1 observer = %6.3f ms"
               " (%.2f us/prim)\n", N, best, best*1000.0/N);
        retained->RemoveObserver(HdSceneIndexObserverPtr(&obs));
    }
    return 0;
}
