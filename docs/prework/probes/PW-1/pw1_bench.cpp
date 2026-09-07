// PW-1 benchmark: nanoflann 1.12.1 KD-tree capture cost for usdGen guide->rest
// kNN interpolation.
//
// Spec (plan/11-roadmap.md §1.1 PW-1, gate E-4):
//   - exact instantiation: KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<float, Cloud>, Cloud, 3>
//   - 4,000 guide roots (the KD-tree dataset)
//   - k = 3 queries for 100,000 and 1,000,000 rest roots
//   - 1 / 4 / 8 / 20 threads (fixed std::thread pool; queries partitioned)
//   - median + p95 over >= 5 iterations per configuration
//
// Acceptance:
//   - <= 25 ms at 100k queries on 8 threads
//   - cost linear in root count
//   - > 300 ms at 1M triggers SC-6
//
// Build once (single shared index, read-only concurrent queries — nanoflann's
// query path is const and vAcc_ is built at construction).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "nanoflann.hpp"

// Plain-struct dataset adapter: 3-component root array (float).
struct Cloud
{
    std::vector<float> pts;  // 3 floats per point

    std::size_t kdtree_get_point_count() const { return pts.size() / 3; }
    float kdtree_get_pt(std::size_t row_id, int col) const
    { return pts[row_id * 3 + col]; }

    // Optional bounding-box hook; return false -> nanoflann computes it.
    template <class BBOX>
    bool kdtree_get_bbox(BBOX& /*bb*/) const { return false; }
};

using Index = nanoflann::KDTreeSingleIndexAdaptor<
    nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3>;

namespace
{

// Deterministic pseudo-random cloud (so re-runs are comparable).
std::vector<float> MakePoints(std::size_t n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 100.0f);
    std::vector<float> pts;
    pts.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i)
    {
        pts.push_back(dist(rng));
        pts.push_back(dist(rng));
        pts.push_back(dist(rng));
    }
    return pts;
}

using TimeMs = double;

TimeMs NowMs()
{
    return std::chrono::duration_cast<
               std::chrono::duration<double, std::milli>>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

// Run all `queries` k=3 NN lookups, partitioned across a pool of `numThreads`
// std::thread workers. Returns wall-clock ms for the whole batch.
// Also accumulates a checksum of hit-ids so we can sanity-check that the
// threaded results match a reference (kNN is deterministic per query).
TimeMs RunQueries(const Index& index, const Cloud& queries, int numThreads,
                  unsigned long long* checksumOut)
{
    const std::size_t n = queries.kdtree_get_point_count();
    *checksumOut = 0;
    std::atomic<unsigned long long> totalChecksum{0};

    const TimeMs t0 = NowMs();

    if (numThreads <= 1)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            Index::IndexType ids[3];
            float dists2[3];
            index.knnSearch(&queries.pts[i * 3], 3, ids, dists2);
            totalChecksum.fetch_add(static_cast<unsigned long long>(ids[0])
                                        ^ (static_cast<unsigned long long>(ids[1]) << 10),
                                    std::memory_order_relaxed);
        }
        *checksumOut = totalChecksum.load();
        return NowMs() - t0;
    }

    std::vector<std::thread> workers;
    workers.reserve(numThreads);
    for (int t = 0; t < numThreads; ++t)
    {
        workers.emplace_back([&, t]() {
            // Contiguous partition: thread t owns [lo, hi).
            const std::size_t base = n / numThreads;
            const std::size_t extra = n % numThreads;
            const std::size_t lo = t * base + std::min<std::size_t>(t, extra);
            const std::size_t hi = lo + base + (t >= extra ? 0 : 1);
            for (std::size_t i = lo; i < hi; ++i)
            {
                Index::IndexType ids[3];
                float dists2[3];
                index.knnSearch(&queries.pts[i * 3], 3, ids, dists2);
                totalChecksum.fetch_add(
                    static_cast<unsigned long long>(ids[0]) ^
                        (static_cast<unsigned long long>(ids[1]) << 10),
                    std::memory_order_relaxed);
            }
        });
    }
    for (auto& w : workers) w.join();

    *checksumOut = totalChecksum.load();
    return NowMs() - t0;
}

}  // namespace

int main()
{
    constexpr std::size_t kGuides = 4000;
    constexpr std::size_t kQuerySets[] = { 100000, 1000000 };
    constexpr int kThreads[] = { 1, 4, 8, 20 };
    constexpr int kIters = 7;

    std::printf("nanoflann %s | host hardware_concurrency=%zu\n",
                NANOFLANN_VERSION_STRING,
                std::thread::hardware_concurrency());

    // ---- build the guide index once -------------------------------------
    Cloud guideCloud;
    guideCloud.pts = MakePoints(kGuides, /*seed=*/0xC0FFEE);

    nanoflann::KDTreeSingleIndexAdaptorParams params( /*leaf_max_size=*/10 );
    const TimeMs tBuild0 = NowMs();
    Index index(/*dimensionality=*/3, guideCloud, params);
    const TimeMs buildMs = NowMs() - tBuild0;
    std::printf("build: %lu guide roots in %.3f ms\n",
                (unsigned long)guideCloud.kdtree_get_point_count(), buildMs);

    // ---- query each rest-root set ----------------------------------------
    for (std::size_t qs = 0; qs < 2; ++qs)
    {
        const std::size_t nq = kQuerySets[qs];
        Cloud queryCloud;
        queryCloud.pts = MakePoints(nq, /*seed=*/0x1234 + qs);

        for (int nt : kThreads)
        {
            std::vector<TimeMs> samples;
            samples.reserve(kIters);
            unsigned long long refChecksum = 0;

            for (int it = 0; it < kIters; ++it)
            {
                unsigned long long cs = 0;
                const TimeMs dt = RunQueries(index, queryCloud, nt, &cs);
                samples.push_back(dt);
                if (it == 0) refChecksum = cs;
                else if (cs != refChecksum)
                {
                    std::printf(
                        "  [warn] checksum drift at iters=%d threads=%d\n",
                        it, nt);
                }
            }

            auto sorted = samples;
            std::sort(sorted.begin(), sorted.end());
            const TimeMs median = sorted[sorted.size() / 2];
            const int p95idx =
                std::min<int>(static_cast<int>(sorted.size()) - 1,
                              static_cast<int>(0.95 * sorted.size()));
            const TimeMs p95 = sorted[p95idx];

            std::printf(
                "queries=%-9zu threads=%2d  median=%8.3f ms  p95=%8.3f ms  "
                "samples={",
                nq, nt, median, p95);
            for (std::size_t i = 0; i < samples.size(); ++i)
                std::printf("%s%.3f", i ? "," : "", samples[i]);
            std::printf("}  checksum=%llu\n",
                        static_cast<unsigned long long>(refChecksum));
        }
    }

    return 0;
}
