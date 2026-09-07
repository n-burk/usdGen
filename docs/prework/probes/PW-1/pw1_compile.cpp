// PW-1 compile probe: confirm the EXACT nanoflann 1.12.1 instantiation
//   nanoflann::KDTreeSingleIndexAdaptor<nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3>
// compiles and runs (k=3 NN query) against the vendored header at
// thirdparty/nanoflann/nanoflann.hpp.
//
// NOTE (1.12.1): the template is <Distance, DatasetAdaptor, int32_t DIM = -1,
// typename index_t = uint32_t> — the roadmap spec omits the 4th parameter
// (defaults to uint32_t). The constructor is
//   (dimensionality, dataset, params)
// and query() writes into out_indices/out_distances arrays.

#include <cstdio>
#include <vector>

#include "nanoflann.hpp"

// Plain-struct dataset adapter, exactly the shape the usdGen engine will use
// for a 3-component rest-pose root array.
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

// The EXACT template instantiation from the PW-1 spec (3 args; index_t
// defaults to uint32_t in nanoflann 1.12.1).
using Index = nanoflann::KDTreeSingleIndexAdaptor<
    nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3>;

static_assert(sizeof(Index::IndexType) == 4, "index_t defaults to uint32_t");

int main()
{
    constexpr std::size_t kGuides = 64;
    Cloud cloud;
    for (std::size_t i = 0; i < kGuides; ++i)
    {
        const float x = static_cast<float>(i) * 0.5f;
        cloud.pts.push_back(x);
        cloud.pts.push_back(x * 0.25f);
        cloud.pts.push_back((i % 8) * 1.5f);
    }

    nanoflann::KDTreeSingleIndexAdaptorParams params( /*leaf_max_size=*/10 );
    Index const index(/*dimensionality=*/3, cloud, params);

    const float qpt[3] = { 12.3f, 5.0f, 3.0f };
    constexpr int kK = 3;
    std::vector<Index::IndexType> ids(kK);
    std::vector<float> dists2(kK);
    index.knnSearch(qpt, kK, ids.data(), dists2.data());

    for (int i = 0; i < kK; ++i)
        std::printf("k=%d  id=%lu  dist2=%.6f\n", i,
                    static_cast<unsigned long>(ids[i]), dists2[i]);

    std::printf("COMPILE-AND-RUN OK: exact 3-arg instantiation "
                "KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<float, Cloud>, "
                "Cloud, 3> (nanoflann %s)\n", NANOFLANN_VERSION_STRING);
    return 0;
}
