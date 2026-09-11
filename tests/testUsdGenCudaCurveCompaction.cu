#include "gpu/curveCompaction.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace usdGen::gpu;

#define CHECK(X) do { if (!(X)) { \
    std::fprintf(stderr, "check failed at %d: %s\n", __LINE__, #X); return 1; \
} } while (0)

static float3 P(float x, float y, float z) { return make_float3(x, y, z); }
static bool Near(float3 a, float3 b) {
    return std::fabs(a.x-b.x) < 1.e-5f && std::fabs(a.y-b.y) < 1.e-5f &&
           std::fabs(a.z-b.z) < 1.e-5f;
}

template<class T>
static bool Upload(DeviceBuffer<T>& buffer, std::vector<T> const& values) {
    return buffer.reset(values.size()) == cudaSuccess &&
        (values.empty() || cudaMemcpy(buffer.data(), values.data(),
                                      values.size() * sizeof(T),
                                      cudaMemcpyHostToDevice) == cudaSuccess);
}

template<class T>
static std::vector<T> Read(DeviceView<const T> view, cudaStream_t stream) {
    std::vector<T> result(view.size);
    if (view.size && (cudaMemcpyAsync(result.data(), view.data,
                                      view.size * sizeof(T),
                                      cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
                      cudaStreamSynchronize(stream) != cudaSuccess))
        result.clear();
    return result;
}

int main() {
    cudaStream_t producer = nullptr, consumer = nullptr;
    CHECK(cudaStreamCreate(&producer) == cudaSuccess);
    CHECK(cudaStreamCreate(&consumer) == cudaSuccess);
    {
        const std::vector<float3> points = {
            P(0,0,0), P(1,0,0), P(0,1,0), P(0,0,1), P(1,1,1)};
        const std::vector<float3> rest = {
            P(10,0,0), P(11,0,0), P(10,1,0), P(10,0,1), P(11,1,1)};
        const std::vector<float> widths = {1,2,3,4,5};
        const std::vector<float> hairT = {0,1,0,.5f,1};
        const std::vector<uint32_t> offsets = {0,2,5};
        const std::vector<uint64_t> ids = {30,70};
        const std::vector<int32_t> roots = {4,8};
        const std::vector<float2> rootUV = {{.1f,.2f},{.7f,.8f}};
        DeviceBuffer<float3> dPoints, dRest;
        DeviceBuffer<float> dWidths, dHairT;
        DeviceBuffer<uint32_t> dOffsets;
        DeviceBuffer<uint64_t> dIds;
        DeviceBuffer<int32_t> dRoots;
        DeviceBuffer<unsigned char> dRootUV;
        DeviceBuffer<uint8_t> dKeep;
        CHECK(Upload(dPoints, points) && Upload(dRest, rest) &&
              Upload(dWidths, widths) && Upload(dHairT, hairT) &&
              Upload(dOffsets, offsets) && Upload(dIds, ids) &&
              Upload(dRoots, roots) &&
              dRootUV.reset(rootUV.size() * sizeof(float2)) == cudaSuccess &&
              cudaMemcpy(dRootUV.data(), rootUV.data(),
                         rootUV.size() * sizeof(float2), cudaMemcpyHostToDevice) == cudaSuccess &&
              Upload(dKeep, std::vector<uint8_t>{0,1}));
        DeviceCurveGeometryView input = {
            {dPoints.data(), points.size()}, {dRest.data(), rest.size()},
            {dWidths.data(), widths.size()}, {dOffsets.data(), offsets.size()},
            {dIds.data(), ids.size()}, 2, 5};
        CudaCurveCompaction compact;
        CHECK(compact.Apply(input, {dHairT.data(), hairT.size()},
                            {dRoots.data(), roots.size()},
                            {reinterpret_cast<float2 const*>(dRootUV.data()), 2},
                            {dKeep.data(), 2}, producer) == CurveCompactionStatus::Ok);
        CHECK(compact.pending());
        CHECK(compact.Finish(consumer) == CurveCompactionStatus::Ok);
        CHECK(!compact.pending() && compact.generation() == 1);
        auto view = compact.view();
        CHECK(view.curveCount == 1 && view.pointCount == 3);
        auto outOffsets = Read(view.curveOffsets, consumer);
        auto outPoints = Read(view.points, consumer);
        auto outRest = Read(view.restPoints, consumer);
        auto outWidths = Read(view.widths, consumer);
        auto outHairT = Read(compact.hairT(), consumer);
        auto outIds = Read(view.stableIds, consumer);
        auto outRoots = Read(compact.rootPrim(), consumer);
        auto outUV = Read(compact.rootUV(), consumer);
        CHECK((outOffsets == std::vector<uint32_t>{0,3}));
        CHECK(outPoints.size() == 3 && Near(outPoints[0], points[2]) &&
              Near(outPoints[2], points[4]));
        CHECK(outRest.size() == 3 && Near(outRest[0], rest[2]) &&
              Near(outRest[2], rest[4]));
        CHECK(outWidths == std::vector<float>({3,4,5}));
        CHECK(outHairT == std::vector<float>({0,.5f,1}));
        CHECK((outIds == std::vector<uint64_t>{70}));
        CHECK((outRoots == std::vector<int32_t>{8}));
        CHECK(outUV.size() == 1 && std::fabs(outUV[0].x-.7f) < 1.e-5f);

        // Cross-stream use is explicitly fenced by the owned output channels.
        CHECK(compact.recordUse(producer) == cudaSuccess);
        CHECK(compact.waitOn(consumer) == cudaSuccess);
        CHECK(Read(compact.view().points, consumer).size() == 3);

        // Keep-all retains exact source order and topology.
        CHECK(Upload(dKeep, std::vector<uint8_t>{1,1}));
        CHECK(compact.Apply(input, {dHairT.data(), hairT.size()},
                            {dRoots.data(), roots.size()},
                            {reinterpret_cast<float2 const*>(dRootUV.data()), 2},
                            {dKeep.data(), 2}, producer) == CurveCompactionStatus::Ok);
        CHECK(compact.Finish(consumer) == CurveCompactionStatus::Ok);
        CHECK(compact.view().curveCount == 2 && compact.view().pointCount == 5);
        CHECK(Read(compact.view().curveOffsets, consumer) == offsets);
        CHECK(Read(compact.view().stableIds, consumer) == ids);

        // Keep-none retains a zero sentinel offset and empty optional roots.
        CHECK(Upload(dKeep, std::vector<uint8_t>{0,0}));
        CHECK(compact.Apply(input, {dHairT.data(), hairT.size()},
                            {dRoots.data(), roots.size()},
                            {reinterpret_cast<float2 const*>(dRootUV.data()), 2},
                            {dKeep.data(), 2}, producer) == CurveCompactionStatus::Ok);
        CHECK(compact.Finish(consumer) == CurveCompactionStatus::Ok);
        CHECK(compact.view().curveCount == 0 && compact.view().pointCount == 0 &&
              compact.view().curveOffsets.size == 1 && compact.rootPrim().size == 0 &&
              (Read(compact.view().curveOffsets, consumer) == std::vector<uint32_t>{0}));

        // Invalid input never replaces the completed output.
        CHECK(Upload(dKeep, std::vector<uint8_t>{2,1}));
        CHECK(compact.Apply(input, {dHairT.data(), hairT.size()},
                            {dRoots.data(), roots.size()},
                            {reinterpret_cast<float2 const*>(dRootUV.data()), 2},
                            {dKeep.data(), 2}, producer) == CurveCompactionStatus::InvalidArgument);
        CHECK((compact.view().curveCount == 0 && Read(compact.view().curveOffsets, consumer) ==
              std::vector<uint32_t>{0}));
        CHECK(Upload(dKeep, std::vector<uint8_t>{1,1}));
        DeviceBuffer<uint32_t> dBadOffsets;
        CHECK(Upload(dBadOffsets, std::vector<uint32_t>{1,3,5}));
        // Shape validation is device-side but scalar-only and rejects before
        // any scan or pending publication is started.  The first offset must
        // be zero; this malformed buffer must not replace the empty output.
        DeviceCurveGeometryView malformed = input;
        malformed.curveOffsets = {dBadOffsets.data(), 3};
        CHECK(compact.Apply(malformed, {dHairT.data(), hairT.size()},
                            {dRoots.data(), roots.size()},
                            {reinterpret_cast<float2 const*>(dRootUV.data()), 2},
                            {dKeep.data(), 2}, producer) == CurveCompactionStatus::InvalidArgument);
        CHECK(compact.view().curveCount == 0);
        auto badPoints = points; badPoints[3].x = NAN;
        CHECK(Upload(dPoints, badPoints));
        CHECK(compact.Apply(input, {dHairT.data(), hairT.size()},
                            {dRoots.data(), roots.size()},
                            {reinterpret_cast<float2 const*>(dRootUV.data()), 2},
                            {dKeep.data(), 2}, producer) == CurveCompactionStatus::NonFiniteInput);
        CHECK(compact.view().curveCount == 0);
        CHECK(Upload(dPoints, points));

        auto badWidths = widths;
        badWidths[0] = -1.0f;
        CHECK(Upload(dWidths, badWidths));
        CHECK(compact.Apply(input, {dHairT.data(), hairT.size()},
                            {dRoots.data(), roots.size()},
                            {reinterpret_cast<float2 const*>(dRootUV.data()), 2},
                            {dKeep.data(), 2}, producer) == CurveCompactionStatus::InvalidArgument);
        CHECK(Upload(dWidths, widths));
        auto badRoots = roots;
        badRoots[0] = -1;
        CHECK(Upload(dRoots, badRoots));
        CHECK(compact.Apply(input, {dHairT.data(), hairT.size()},
                            {dRoots.data(), roots.size()},
                            {reinterpret_cast<float2 const*>(dRootUV.data()), 2},
                            {dKeep.data(), 2}, producer) == CurveCompactionStatus::InvalidArgument);
        CHECK(Upload(dRoots, roots));

        // Empty input still has a required one-element offset sentinel.
        DeviceBuffer<uint32_t> emptyOffsets;
        CHECK(Upload(emptyOffsets, std::vector<uint32_t>{0}));
        DeviceCurveGeometryView empty = {{}, {}, {}, {emptyOffsets.data(),1}, {}, 0, 0};
        CHECK(compact.Apply(empty, {}, {}, {}, {}, producer) == CurveCompactionStatus::Ok);
        CHECK(compact.Finish(consumer) == CurveCompactionStatus::Ok);
        CHECK(compact.generation() == 4 && compact.view().curveCount == 0 &&
              compact.view().curveOffsets.size == 1);
    }
    CHECK(cudaStreamDestroy(consumer) == cudaSuccess);
    CHECK(cudaStreamDestroy(producer) == cudaSuccess);
    std::puts("testUsdGenCudaCurveCompaction: PASS");
    return 0;
}
