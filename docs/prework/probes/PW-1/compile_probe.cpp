// PW-1 / E-4 compile probe.
//
// Confirms the EXACT template instantiation the roadmap specifies:
//     nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3>
// against the vendored nanoflann 1.12.1 header
// (thirdparty/nanoflann/nanoflann.hpp, NANOFLANN_VERSION_STRING "1.12.1").
//
// FINDINGS (nanoflann 1.12.1):
//  - "Cloud" must be a dataset-adapter CLASS, not a raw std::vector:
//      L2_Simple_Adaptor::evalMetric() calls  data_source.kdtree_get_pt(b_idx, i)
//      KDTreeBaseClass reads               dataset_.kdtree_get_point_count()
//      KDTreeBaseClass::computeBoundingBox() (line ~1220) calls
//          obj.dataset_.kdtree_get_bbox(bbox)  UNCONDITIONALLY, so even though
//          the docs call kdtree_get_bbox "optional", in 1.12.1 a dataset that
//          lacks it fails to compile. Return false to use the default bbox loop.
//  - Query API used by the bench: index.knnSearch(query_point, k, out_indices, out_distances)
//    (const, thread-safe for concurrent readers per the header's threading note).
//  - Build parallelism is a separate axis (params.n_thread_build); the capture
//    bench partitions QUERIES across a fixed std::thread pool.
//
// Compiles with the repo's Release flags (-O3 -DNDEBUG, C++17); needs only -lpthread.

#include "nanoflann.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

namespace {

// The "Cloud" dataset-adapter class the roadmap's shorthand implies.
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

// The EXACT instantiation under verification.
using KDTree = nanoflann::KDTreeSingleIndexAdaptor<
    nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3>;

}  // namespace

int main() {
  std::vector<std::array<float, 3>> raw;
  raw.reserve(64);
  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> dist(0.0f, 100.0f);
  for (int i = 0; i < 64; ++i) {
    raw.push_back({dist(rng), dist(rng), dist(rng)});
  }
  Cloud guides(raw);

  const nanoflann::KDTreeSingleIndexAdaptorParams params(10);  // leaf_max_size
  KDTree index(3, guides, params);

  // Force instantiation of knnSearch (k=3, the capture query) and radiusSearch.
  const float q[3] = {50.0f, 50.0f, 50.0f};
  std::array<uint32_t, 3> out_indices{};
  std::array<float, 3> out_distances{};
  const auto n = index.knnSearch(q, 3, out_indices.data(), out_distances.data());
  assert(n == 3);
  for (int i = 0; i + 1 < 3; ++i) {
    assert(out_distances[i] <= out_distances[i + 1] + 1e-6f);  // sorted
    assert(out_indices[i] != out_indices[i + 1]);              // distinct
  }

  std::vector<nanoflann::ResultItem<uint32_t, float>> radius_hits;
  index.radiusSearch(q, 40.0f * 40.0f, radius_hits);

  std::printf(
      "PW-1 compile probe: KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<float, Cloud>, Cloud, 3> "
      "instantiated OK; dataset=%zu pts, knnSearch(k=3) returned %zu (sorted+distinct), "
      "radiusSearch returned %zu hits.\n",
      index.size(index), static_cast<size_t>(n), radius_hits.size());
  return 0;
}
