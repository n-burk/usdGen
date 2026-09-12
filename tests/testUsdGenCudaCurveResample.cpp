#include "gpu/curveResample.h"

#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace usdGen::gpu;

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "FAIL %d: %s\\n", __LINE__, #expression); \
        return 1; \
    } \
} while (false)

template <class T>
static bool Upload(DeviceBuffer<T>& buffer, std::vector<T> const& values) {
    return buffer.reset(values.size()) == cudaSuccess &&
        (!values.size() || cudaMemcpy(buffer.data(), values.data(), values.size() * sizeof(T),
                                      cudaMemcpyHostToDevice) == cudaSuccess);
}

template <class T>
static std::vector<T> Download(DeviceView<const T> view) {
    std::vector<T> result(view.size);
    if (view.size && cudaMemcpy(result.data(), view.data, view.size * sizeof(T),
                                cudaMemcpyDeviceToHost) != cudaSuccess)
        result.clear();
    return result;
}

template <class T>
static DeviceView<const T> Read(DeviceBuffer<T> const& buffer) {
    return buffer.view();
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    CHECK(cudaSetDevice(0) == cudaSuccess);
    cudaStream_t stream = nullptr;
    CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess);

    DeviceBuffer<float3> points, restPoints;
    DeviceBuffer<float> widths, hairT;
    DeviceBuffer<uint32_t> offsets;
    DeviceBuffer<uint64_t> stableIds;
    DeviceBuffer<int32_t> rootPrim;
    DeviceBuffer<unsigned char> rootUVBytes;
    const std::vector<float3> sourcePoints{{0, 0, 0}, {4, 0, 0},
                                            {10, 0, 0}, {11, 0, 0},
                                            {14, 0, 0}, {15, 0, 0}};
    CHECK(Upload(points, sourcePoints));
    CHECK(Upload(restPoints, {{0, 1, 0}, {4, 1, 0}, {10, 1, 0},
                              {11, 1, 0}, {14, 1, 0}, {15, 1, 0}}));
    CHECK(Upload(widths, {1, 9, 2, 4, 6, 8}));
    CHECK(Upload(hairT, {0, 1, 0, .25f, .75f, 1}));
    CHECK(Upload(offsets, {0, 2, 6}));
    CHECK(Upload(stableIds, {9, 3}));
    CHECK(Upload(rootPrim, {7, 8}));
    std::vector<float2> hostRootUV{{.2f, .3f}, {.7f, .8f}};
    CHECK(rootUVBytes.reset(hostRootUV.size() * sizeof(float2)) == cudaSuccess);
    CHECK(cudaMemcpy(rootUVBytes.data(), hostRootUV.data(), rootUVBytes.size(),
                     cudaMemcpyHostToDevice) == cudaSuccess);

    DeviceCurveGeometryView input{Read(points), Read(restPoints), Read(widths), Read(offsets),
                                  Read(stableIds), 2, 6};
    DeviceView<const float2> rootUV{reinterpret_cast<float2 const*>(rootUVBytes.data()), 2};
    CudaCurveResample resample;

    // Indexed interpolation uses each curve's CV index, not spatial arc length.
    CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, 4, stream) == CurveResampleStatus::Ok);
    CHECK(resample.Finish(stream) == CurveResampleStatus::Ok);
    const auto uniform = resample.view();
    CHECK(uniform.curveCount == 2 && uniform.pointCount == 8);
    CHECK(Download(uniform.curveOffsets) == std::vector<uint32_t>({0, 4, 8}));
    CHECK(Download(uniform.stableIds) == std::vector<uint64_t>({9, 3}));
    CHECK(Download(resample.rootPrim()) == std::vector<int32_t>({7, 8}));
    CHECK(Near(Download(resample.rootUV())[1].x, .7f));
    const auto uniformPoints = Download(uniform.points);
    const auto uniformRest = Download(uniform.restPoints);
    const auto uniformWidths = Download(uniform.widths);
    const auto uniformHairT = Download(resample.hairT());
    const std::vector<float> expectedX{0, 4.f / 3.f, 8.f / 3.f, 4, 10, 11, 14, 15};
    const std::vector<float> expectedWidths{1, 11.f / 3.f, 19.f / 3.f, 9, 2, 4, 6, 8};
    const std::vector<float> expectedHairT{0, 1.f / 3.f, 2.f / 3.f, 1, 0, .25f, .75f, 1};
    for (size_t point = 0; point < expectedX.size(); ++point) {
        CHECK(Near(uniformPoints[point].x, expectedX[point]));
        CHECK(Near(uniformRest[point].y, 1.0f));
        CHECK(Near(uniformWidths[point], expectedWidths[point]));
        CHECK(Near(uniformHairT[point], expectedHairT[point]));
    }

    // target == 0 preserves the source's ragged layout and all supplied channels.
    CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, 0, stream) == CurveResampleStatus::Ok);
    CHECK(resample.Finish(stream) == CurveResampleStatus::Ok);
    CHECK(resample.view().pointCount == 6);
    CHECK(Download(resample.view().curveOffsets) == std::vector<uint32_t>({0, 2, 6}));
    CHECK(Near(Download(resample.view().points)[2].x, 10.0f));

    // Optional channels remain absent rather than becoming zero-filled channels.
    DeviceCurveGeometryView pointsOnly{Read(points), {}, {}, Read(offsets), Read(stableIds), 2, 6};
    CHECK(resample.Apply(pointsOnly, {}, {}, {}, 2, stream) == CurveResampleStatus::Ok);
    CHECK(resample.Finish(stream) == CurveResampleStatus::Ok);
    CHECK(resample.view().restPoints.size == 0 && resample.view().widths.size == 0 &&
          resample.hairT().size == 0 && resample.rootPrim().size == 0 && resample.rootUV().size == 0);

    // Equal spatial positions are valid: indexing, rather than arc length, drives interpolation.
    DeviceBuffer<float3> collapsedPoints;
    DeviceBuffer<uint32_t> collapsedOffsets;
    DeviceBuffer<uint64_t> collapsedIds;
    CHECK(Upload(collapsedPoints, {{3, 0, 0}, {3, 0, 0}}));
    CHECK(Upload(collapsedOffsets, {0, 2}));
    CHECK(Upload(collapsedIds, {42}));
    DeviceCurveGeometryView collapsed{Read(collapsedPoints), {}, {}, Read(collapsedOffsets),
                                      Read(collapsedIds), 1, 2};
    CudaCurveResample collapsedResample;
    CHECK(collapsedResample.Apply(collapsed, {}, {}, {}, 3, stream) == CurveResampleStatus::Ok);
    CHECK(collapsedResample.Finish(stream) == CurveResampleStatus::Ok);
    const auto collapsedOutput = Download(collapsedResample.view().points);
    CHECK(collapsedOutput.size() == 3 && Near(collapsedOutput[0].x, 3.0f) &&
          Near(collapsedOutput[1].x, 3.0f) && Near(collapsedOutput[2].x, 3.0f));

    // A valid empty geometry owns only its required {0} offset.
    DeviceBuffer<uint32_t> emptyOffsets;
    CHECK(Upload(emptyOffsets, {0}));
    DeviceCurveGeometryView empty{{}, {}, {}, Read(emptyOffsets), {}, 0, 0};
    CHECK(resample.Apply(empty, {}, {}, {}, 4, stream) == CurveResampleStatus::Ok);
    CHECK(resample.Finish(stream) == CurveResampleStatus::Ok);
    CHECK(resample.view().curveCount == 0 && resample.view().pointCount == 0);
    CHECK(Download(resample.view().curveOffsets) == std::vector<uint32_t>({0}));

    CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, -1, stream) == CurveResampleStatus::InvalidArgument);
    CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, 1, stream) == CurveResampleStatus::InvalidArgument);
    CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, INT32_MAX, stream) == CurveResampleStatus::InvalidArgument);
    DeviceCurveGeometryView shortOptional = input;
    shortOptional.restPoints = {restPoints.data(), 0};
    CHECK(resample.Apply(shortOptional, Read(hairT), Read(rootPrim), rootUV, 2, stream) == CurveResampleStatus::InvalidArgument);
    shortOptional = input;
    shortOptional.widths = {widths.data(), 0};
    CHECK(resample.Apply(shortOptional, Read(hairT), Read(rootPrim), rootUV, 2, stream) == CurveResampleStatus::InvalidArgument);

    // Failed validation must not replace an already-finished active result.
    CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, 0, stream) == CurveResampleStatus::Ok);
    CHECK(resample.Finish(stream) == CurveResampleStatus::Ok);
    const size_t retainedPointCount = resample.view().pointCount;
    float3 nonFinite{std::numeric_limits<float>::quiet_NaN(), 0, 0};
    CHECK(cudaMemcpy(points.data(), &nonFinite, sizeof(nonFinite), cudaMemcpyHostToDevice) == cudaSuccess);
    CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, 4, stream) == CurveResampleStatus::NonFiniteInput);
    CHECK(resample.view().pointCount == retainedPointCount && Near(Download(resample.view().points)[2].x, 10.0f));
    // Restore the source, then check the minimum-CV topology rule.
    CHECK(cudaMemcpy(points.data(), sourcePoints.data(), sourcePoints.size() * sizeof(float3),
                     cudaMemcpyHostToDevice) == cudaSuccess);
    const std::vector<uint32_t> degenerateOffsets{0, 1, 6};
    CHECK(cudaMemcpy(offsets.data(), degenerateOffsets.data(), degenerateOffsets.size() * sizeof(uint32_t),
                     cudaMemcpyHostToDevice) == cudaSuccess);
    CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, 2, stream) == CurveResampleStatus::InvalidArgument);
    const std::vector<uint32_t> sourceOffsets{0, 2, 6};
    CHECK(cudaMemcpy(offsets.data(), sourceOffsets.data(), sourceOffsets.size() * sizeof(uint32_t),
                     cudaMemcpyHostToDevice) == cudaSuccess);

    int deviceCount = 0;
    CHECK(cudaGetDeviceCount(&deviceCount) == cudaSuccess);
    if (deviceCount > 1) {
        CHECK(cudaSetDevice(1) == cudaSuccess);
        CHECK(resample.Apply(input, Read(hairT), Read(rootPrim), rootUV, 2, stream) == CurveResampleStatus::InvalidArgument);
        CHECK(cudaSetDevice(0) == cudaSuccess);
    }
    CHECK(cudaStreamDestroy(stream) == cudaSuccess);
    std::puts("testUsdGenCudaCurveResample: PASS");
    return 0;
}
