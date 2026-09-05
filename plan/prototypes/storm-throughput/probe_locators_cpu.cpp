// Probe: isolate ComputeDirtyLocators from SdfPath construction, and measure a
// realistic per-frame invalidation build with pre-cached prim paths.
#include "pxr/pxr.h"
#include "pxr/imaging/hd/containerDataSourceEditor.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/tokens.h"
#include <chrono>
#include <cstdio>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b-a).count(); }

int main() {
    const HdDataSourceLocator ptsLeaf(HdPrimvarsSchemaTokens->primvars,
        HdTokens->points, HdPrimvarSchemaTokens->primvarValue);
    HdDataSourceLocatorSet leafSet; leafSet.insert(ptsLeaf);

    printf("=== ComputeDirtyLocators in isolation ===\n");
    for (int N : {1000, 10000, 100000}) {
        auto t0 = Clock::now();
        size_t acc = 0;
        for (int i = 0; i < N; ++i) {
            HdDataSourceLocatorSet s =
                HdContainerDataSourceEditor::ComputeDirtyLocators(leafSet);
            acc += s.IsEmpty() ? 0 : 1;
        }
        auto t1 = Clock::now();
        printf("  %6d calls = %7.3f ms (%.3f us/call)  [acc=%zu]\n",
               N, ms(t0,t1), ms(t0,t1)*1000.0/N, acc);
    }

    printf("\n=== per-frame entry vector build, prim paths PRE-CACHED ===\n");
    for (int N : {32, 256, 1000, 10000, 100000}) {
        std::vector<SdfPath> paths; paths.reserve(N);
        for (int i = 0; i < N; ++i)
            paths.emplace_back(SdfPath("/Hair").AppendChild(
                TfToken(TfStringPrintf("chunk_%d", i))));
        const HdDataSourceLocatorSet cached =
            HdContainerDataSourceEditor::ComputeDirtyLocators(leafSet);
        double best = 1e9;
        for (int rep = 0; rep < 5; ++rep) {
            auto t0 = Clock::now();
            HdSceneIndexObserver::DirtiedPrimEntries entries;
            entries.reserve(N);
            for (int i = 0; i < N; ++i) entries.emplace_back(paths[i], cached);
            auto t1 = Clock::now();
            best = std::min(best, ms(t0,t1));
        }
        printf("  N=%6d  cached-locator-set build = %7.3f ms (%.3f us/prim)\n",
               N, best, best*1000.0/N);
        // and with a freshly computed set each prim
        best = 1e9;
        for (int rep = 0; rep < 5; ++rep) {
            auto t0 = Clock::now();
            HdSceneIndexObserver::DirtiedPrimEntries entries;
            entries.reserve(N);
            for (int i = 0; i < N; ++i)
                entries.emplace_back(paths[i],
                    HdContainerDataSourceEditor::ComputeDirtyLocators(leafSet));
            auto t1 = Clock::now();
            best = std::min(best, ms(t0,t1));
        }
        printf("  N=%6d  recomputed-per-prim build  = %7.3f ms (%.3f us/prim)\n",
               N, best, best*1000.0/N);
    }

    printf("\n=== HdDataSourceLocatorSet::Intersects cost (the dependency inner loop) ===\n");
    {
        const HdDataSourceLocatorSet cached =
            HdContainerDataSourceEditor::ComputeDirtyLocators(leafSet);
        int N = 1000000; volatile int acc = 0;
        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) acc += cached.Intersects(ptsLeaf) ? 1 : 0;
        auto t1 = Clock::now();
        printf("  %d Intersects(locator) = %.3f ms (%.4f us/call)\n",
               N, ms(t0,t1), ms(t0,t1)*1000.0/N);
    }
    return 0;
}
