// PW-1 / E-4 — nanoflann 1.12.1 kNN capture-cost benchmark.
//
// Workload (roadmap PW-1 row / gate E-4):
//   - KD-tree dataset: 4,000 guide roots (rest positions), uniform in [0,100)^3.
//   - Queries: k=3 nearest guides for each rest root; N = 100,000 and 1,000,000.
//   - Parallelism: fixed pool of 1/4/8/20 std::thread workers; the query range
//     is partitioned into contiguous per-thread slices (one knnSearch call per
//     rest root; no shared mutable state — nanoflann queries are const and
//     thread-safe for concurrent readers, header threading note).
//   - 7 iterations per configuration; report median + p95 (nearest-rank).
//   - Each iteration rebuilds the tree so the measurement covers the whole
//     "capture" step (tree build over the 4k guides + all N kNN queries);
//     build and query phases are also reported separately.
//
// Flags: the repo's Release flags (-O3 -DNDEBUG, C++17), same as the usdGen
// build (build/CMakeCache.txt: CMAKE_CXX_FLAGS_RELEASE).

#include "nanoflann.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms_of(std::chrono::nanoseconds ns) {
  return std::chrono::duration_cast<std::chrono::microseconds>(ns).count() / 1000.0;
}

// Dataset-adapter class (see compile_probe.cpp for the 1.12.1 contract).
class Cloud {
 public:
  explicit Cloud(const std::vector<std::array<float, 3>>& p) : pts_(p) {}
  size_t kdtree_get_point_count() const { return pts_.size(); }
  float  kdtree_get_pt(size_t index, int dimension) const { return pts_[index][dimension]; }
  template <class BBOX>
  bool kdtree_get_bbox(BBOX& /*bb*/) const { return false; }
 private:
  const std::vector<std::array<float, 3>>& pts_;
};

// The EXACT instantiation the roadmap specifies.
using KDTree = nanoflann::KDTreeSingleIndexAdaptor<
    nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3>;

std::vector<std::array<float, 3>> make_points(size_t n, unsigned seed) {
  std::vector<std::array<float, 3>> pts(n);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> d(0.0f, 100.0f);
  for (auto& p : pts) p = {d(rng), d(rng), d(rng)};
  return pts;
}

// One full iteration: build the tree over the guides, then run all N k=3
// queries across a fixed pool of `threads` std::thread workers.
// Returns {build_ms, query_ms, total_ms}.
std::array<double, 3> run_iteration(const std::vector<std::array<float, 3>>& guides,
                                    const std::vector<std::array<float, 3>>& rest,
                                    int threads, std::vector<uint32_t>* sink) {
  const size_t n = rest.size();
  sink->resize(n * 3);

  auto t0 = Clock::now();
  Cloud guide_cloud(guides);
  const nanoflann::KDTreeSingleIndexAdaptorParams params(10);
  KDTree index(3, guide_cloud, params);
  auto t1 = Clock::now();

  auto worker = [&](size_t lo, size_t hi) {
    uint32_t idx[3];
    float dist[3];
    for (size_t i = lo; i < hi; ++i) {
      const float q[3] = {rest[i][0], rest[i][1], rest[i][2]};
      size_t got = index.knnSearch(q, 3, idx, dist);
      // record result indices so the work cannot be eliminated; also lets a
      // spot-check compare against a single-threaded reference.
      for (size_t k = 0; k < got; ++k) (*sink)[i * 3 + k] = idx[k];
    }
  };

  if (threads <= 1) {
    worker(0, n);
  } else {
    std::vector<std::thread> pool;
    pool.reserve(threads);
    const size_t chunk = (n + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
      size_t lo = static_cast<size_t>(t) * chunk;
      if (lo >= n) break;
      size_t hi = std::min(n, lo + chunk);
      pool.emplace_back(worker, lo, hi);
    }
    for (auto& th : pool) th.join();
  }
  auto t2 = Clock::now();
  return {ms_of(t1 - t0), ms_of(t2 - t1), ms_of(t2 - t0)};
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  size_t rank = static_cast<size_t>(std::ceil(p / 100.0 * v.size()));
  if (rank < 1) rank = 1;
  if (rank > v.size()) rank = v.size();
  return v[rank - 1];
}

struct ConfigResult {
  std::string label;
  size_t n_rest;
  int threads;
  double build_med;
  double query_med;
  double total_med;
  double total_p95;
  double per_1k;
};
std::vector<ConfigResult> g_results;

void run_config(size_t n_rest, int threads, int iters,
                const std::vector<std::array<float, 3>>& guides) {
  auto rest = make_points(n_rest, 777);
  std::vector<uint32_t> sink;

  // warm-up (page faults, allocator, TLB) — not reported
  run_iteration(guides, rest, threads, &sink);

  std::vector<double> totals, builds, queries;
  totals.reserve(iters);
  builds.reserve(iters);
  queries.reserve(iters);
  for (int it = 0; it < iters; ++it) {
    auto r = run_iteration(guides, rest, threads, &sink);
    builds.push_back(r[0]);
    queries.push_back(r[1]);
    totals.push_back(r[2]);
  }

  double total_med = percentile(totals, 50.0);
  double total_p95 = percentile(totals, 95.0);
  double query_med = percentile(queries, 50.0);
  double build_med = percentile(builds, 50.0);
  double per_1k = total_med / (n_rest / 1000.0);

  std::string label = "N=" + std::to_string(n_rest) + " T=" + std::to_string(threads);
  std::printf("%-16s n=%-10zu threads=%-3d build_med=%7.3f ms  query_med=%8.3f ms  "
              "total_med=%8.3f ms  total_p95=%8.3f ms  per1k=%7.4f ms\n",
              label.c_str(), n_rest, threads, build_med, query_med, total_med, total_p95, per_1k);

  g_results.push_back({label, n_rest, threads, build_med, query_med, total_med,
                       total_p95, per_1k});
}

double find_total(const std::string& label) {
  for (auto& r : g_results)
    if (r.label == label) return r.total_med;
  return -1.0;
}

}  // namespace

int main(int argc, char** argv) {
  int iters = 7;
  if (argc > 1) iters = std::atoi(argv[1]);

  const size_t kGuides = 4000;
  auto guides = make_points(kGuides, 9001);

  std::printf("PW-1 / E-4 nanoflann %s kNN capture bench\n",
              NANOFLANN_VERSION_STRING);
  std::printf("guides=%zu (KD-tree dataset), k=3, uniform [0,100)^3, %d measured iterations "
              "per config (+1 warm-up), fixed std::thread pool, contiguous query partition\n",
              kGuides, iters);
  std::printf("std::thread::hardware_concurrency = %zu\n\n",
              std::thread::hardware_concurrency());
  std::printf("%-16s %-12s %-8s\n", "config", "n_rest", "threads");

  for (size_t n_rest : {size_t(100000), size_t(1000000)}) {
    for (int threads : {1, 4, 8, 20}) {
      run_config(n_rest, threads, iters, guides);
    }
  }

  std::printf("\nlinearity ratio (total median, 1M / 100k at same thread count):\n");
  for (int threads : {1, 4, 8, 20}) {
    double a = find_total("N=100000 T=" + std::to_string(threads));
    double b = find_total("N=1000000 T=" + std::to_string(threads));
    std::printf("  T=%-2d: %.2f x   (100k med %.2f ms -> 1M med %.2f ms)\n",
                threads, a > 0 ? b / a : 0.0, a, b);
  }

  double t100k_8 = find_total("N=100000 T=8");
  double t1M_8 = find_total("N=1000000 T=8");
  std::printf("\nACCEPTANCE:\n");
  std::printf("  100k @ 8 threads total median = %.2f ms   gate: <= 25 ms -> %s\n",
              t100k_8, t100k_8 <= 25.0 ? "PASS" : "FAIL");
  std::printf("  1M   @ 8 threads total median = %.2f ms   stop: > 300 ms triggers SC-6 -> %s\n",
              t1M_8, t1M_8 > 300.0 ? "TRIGGERED" : "NOT triggered");
  return 0;
}
