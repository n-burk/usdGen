#include "gpu/topology.h"
#include "gpu/deviceBuffer.h"

#include <cuda_runtime.h>

#include <cstdint>
#include <cstdio>

using namespace usdGen::gpu;

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    cudaStream_t stream = nullptr;
    check(cudaStreamCreate(&stream) == cudaSuccess, "create stream");
    if (failures) return failures;

    DeviceBuffer<uint32_t> offsetsA, offsetsB;
    DeviceBuffer<uint64_t> idsA, idsB;
    check(offsetsA.reset(3) == cudaSuccess && offsetsB.reset(3) == cudaSuccess &&
          idsA.reset(2) == cudaSuccess && idsB.reset(2) == cudaSuccess, "allocate topology");
    uint32_t offsets[]{0, 2, 5};
    uint64_t ids[]{17, 42};
    check(cudaMemcpy(offsetsA.data(), offsets, sizeof(offsets), cudaMemcpyHostToDevice) == cudaSuccess &&
          cudaMemcpy(offsetsB.data(), offsets, sizeof(offsets), cudaMemcpyHostToDevice) == cudaSuccess &&
          cudaMemcpy(idsA.data(), ids, sizeof(ids), cudaMemcpyHostToDevice) == cudaSuccess &&
          cudaMemcpy(idsB.data(), ids, sizeof(ids), cudaMemcpyHostToDevice) == cudaSuccess, "upload topology");
    DeviceCurveGeometryView a;
    a.curveCount = 2; a.pointCount = 5;
    a.curveOffsets = {offsetsA.data(), offsetsA.size()};
    a.stableIds = {idsA.data(), idsA.size()};
    DeviceCurveGeometryView b = a;
    b.curveOffsets = {offsetsB.data(), offsetsB.size()};
    b.stableIds = {idsB.data(), idsB.size()};
    bool equal = false;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaSuccess && equal, "equal topology");

    DeviceBuffer<float3> pointsA, pointsB;
    check(pointsA.reset(5) == cudaSuccess && pointsB.reset(5) == cudaSuccess,
          "allocate ignored point channels");
    a.points = {pointsA.data(), pointsA.size()};
    b.points = {pointsB.data(), pointsB.size()};
    // Layout identity intentionally ignores point positions.
    equal = false;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaSuccess && equal,
          "point values do not affect topology");

    ids[1] = 99;
    check(cudaMemcpy(idsB.data() + 1, ids + 1, sizeof(uint64_t), cudaMemcpyHostToDevice) == cudaSuccess,
          "change id");
    equal = true;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaSuccess && !equal, "different id");

    uint64_t permuted[]{42,17};
    check(cudaMemcpy(idsB.data(), permuted, sizeof(permuted), cudaMemcpyHostToDevice) == cudaSuccess,
          "permute same stable-id set");
    equal = true;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaSuccess && !equal,
          "stable-id order is part of topology");

    ids[1] = 42;
    check(cudaMemcpy(idsB.data(), ids, sizeof(ids), cudaMemcpyHostToDevice) == cudaSuccess,
          "restore id");
    uint32_t redistributed[]{0, 1, 5};
    check(cudaMemcpy(offsetsB.data(), redistributed, sizeof(redistributed), cudaMemcpyHostToDevice) == cudaSuccess,
          "change offset layout");
    equal = true;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaSuccess && !equal,
          "different offset layout");
    check(cudaMemcpy(offsetsB.data(), offsets, sizeof(offsets), cudaMemcpyHostToDevice) == cudaSuccess,
          "restore offset layout");

    b.curveCount = 1;
    equal = false;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaErrorInvalidValue && !equal,
          "unequal malformed shape is validated");
    b.curveCount = 2;

    DeviceBuffer<uint32_t> bad;
    check(bad.reset(3) == cudaSuccess, "allocate bad offsets");
    uint32_t malformed[]{0, 3, 2};
    check(cudaMemcpy(bad.data(), malformed, sizeof(malformed), cudaMemcpyHostToDevice) == cudaSuccess,
          "upload malformed offsets");
    b.curveOffsets = {bad.data(), bad.size()};
    equal = true;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaErrorInvalidValue && equal,
          "malformed topology preserves result");

    b = a;
    b.curveOffsets = {offsetsB.data(), 2};
    equal = false;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaErrorInvalidValue && !equal,
          "offset shape validation preserves result");
    b = a;
    b.stableIds = {nullptr, 2};
    equal = true;
    check(CompareCurveTopology(a, b, stream, &equal) == cudaErrorInvalidValue && equal,
          "null topology pointer validation preserves result");

    DeviceBuffer<uint32_t> emptyOffsetsA, emptyOffsetsB;
    DeviceBuffer<uint64_t> emptyIdsA, emptyIdsB;
    check(emptyOffsetsA.reset(1) == cudaSuccess && emptyOffsetsB.reset(1) == cudaSuccess &&
          emptyIdsA.reset(0) == cudaSuccess && emptyIdsB.reset(0) == cudaSuccess, "allocate empty topology");
    uint32_t zero = 0;
    check(cudaMemcpy(emptyOffsetsA.data(), &zero, sizeof(zero), cudaMemcpyHostToDevice) == cudaSuccess &&
          cudaMemcpy(emptyOffsetsB.data(), &zero, sizeof(zero), cudaMemcpyHostToDevice) == cudaSuccess,
          "upload empty offsets");
    DeviceCurveGeometryView emptyA, emptyB;
    emptyA.curveOffsets = {emptyOffsetsA.data(), emptyOffsetsA.size()};
    emptyA.stableIds = {emptyIdsA.data(), emptyIdsA.size()};
    emptyB.curveOffsets = {emptyOffsetsB.data(), emptyOffsetsB.size()};
    emptyB.stableIds = {emptyIdsB.data(), emptyIdsB.size()};
    equal = false;
    check(CompareCurveTopology(emptyA, emptyB, stream, &equal) == cudaSuccess && equal,
          "empty topology");
    equal = true;
    check(CompareCurveTopology(a, emptyB, stream, &equal) == cudaSuccess && !equal,
          "valid different curve counts");
    cudaStreamDestroy(stream);
    return failures;
}
