#include "gpu/length.h"
#include "../libs/usdGenMath/usdGenMath/hash.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace usdGen;
using namespace usdGen::gpu;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); return 1; } } while (false)

template <class T>
static bool Upload(DeviceBuffer<T>& d, std::vector<T> const& values, cudaStream_t s) {
    return cudaMemcpyAsync(d.data(), values.data(), values.size()*sizeof(T),
                           cudaMemcpyHostToDevice, s) == cudaSuccess;
}
template <class T>
static bool Download(DeviceBuffer<T> const& d, std::vector<T>& values, cudaStream_t s) {
    if (!values.empty() && cudaMemcpyAsync(values.data(), d.data(),
                                           values.size()*sizeof(T),
                                           cudaMemcpyDeviceToHost, s) != cudaSuccess)
        return false;
    return cudaStreamSynchronize(s) == cudaSuccess;
}
static bool Near(float a, float b) { return std::fabs(a-b) < 3.0e-4f; }
static DeviceView<const float> Const(DeviceBuffer<float> const& d) {
    return {d.data(), d.size()};
}
static DeviceCurveGeometryView Geometry(DeviceBuffer<float3> const& points,
                                        DeviceBuffer<uint32_t> const& offsets,
                                        DeviceBuffer<uint64_t> const& ids) {
    return {{points.data(), points.size()}, {}, {},
            {offsets.data(), offsets.size()}, {ids.data(), ids.size()}, 2, 5};
}

int main() {
    cudaStream_t producer = nullptr, consumer = nullptr;
    CHECK(cudaStreamCreateWithFlags(&producer, cudaStreamNonBlocking) == cudaSuccess);
    CHECK(cudaStreamCreateWithFlags(&consumer, cudaStreamNonBlocking) == cudaSuccess);

    DeviceBuffer<float3> points, output;
    DeviceBuffer<uint32_t> offsets;
    DeviceBuffer<uint64_t> ids;
    DeviceBuffer<uint8_t> keep;
    CHECK(points.reset(5) == cudaSuccess && output.reset(5) == cudaSuccess &&
          offsets.reset(3) == cudaSuccess && ids.reset(2) == cudaSuccess &&
          keep.reset(2) == cudaSuccess);
    const std::vector<float3> source{{0,0,0}, {1,0,0}, {1,1,0},
                                     {0,0,0}, {0,0,3}};
    CHECK(Upload(points, source, producer) &&
          Upload(offsets, {0u,3u,5u}, producer) &&
          Upload(ids, {17u,29u}, producer) && cudaStreamSynchronize(producer) == cudaSuccess);
    const auto geometry = Geometry(points, offsets, ids);
    std::vector<float3> got(5);
    std::vector<uint8_t> gotKeep(2);
    CudaLength op;

    // Scale is root anchored, supports ragged 3/2-CV curves, and does not
    // publish into the borrowed output until Finish on another stream.
    LengthParameters p;
    p.value = ScalarField::Literal(2.0f);
    CHECK(cudaMemsetAsync(output.data(), 0xA5, output.size()*sizeof(float3), producer) == cudaSuccess);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok);
    CHECK(op.pending() && op.Finish(consumer) == StyleStatus::Ok);
    CHECK(Download(output, got, consumer) && Download(keep, gotKeep, consumer));
    CHECK(Near(got[0].x,0) && Near(got[1].x,2) && Near(got[2].x,2) && Near(got[2].y,2));
    CHECK(Near(got[3].z,0) && Near(got[4].z,6) && gotKeep[0] == 1 && gotKeep[1] == 1);

    // Exact random draw and swapped random endpoints are deterministic.
    p.value = ScalarField::Literal(1.0f);
    p.random = Vec2Field::Literal(make_float2(2.0f, 1.0f));
    p.seed = 11;
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    // The endpoints are swapped in the same way as the authored CPU schema.
    CHECK(Near(got[2].y, 1 + UsdGenDraw01(11, 17, usdGen::kSaltLength)));
    CHECK(Near(got[4].z, 3 * (1 + UsdGenDraw01(11, 29, usdGen::kSaltLength))));

    // Set + cutExtend keepParam cuts and collapses trailing CVs; reparam
    // distributes CVs and extends along the last non-degenerate tangent.
    p = LengthParameters{};
    p.mode = LengthMode::Set; p.method = LengthMethod::CutExtend;
    p.rebuild = LengthRebuild::KeepParam; p.value = ScalarField::Literal(.5f);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    CHECK(Near(got[1].x,.5f) && Near(got[2].x,.5f) && Near(got[2].y,0));
    p.rebuild = LengthRebuild::Reparam; p.value = ScalarField::Literal(4.0f);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    CHECK(Near(got[1].x,1) && Near(got[1].y,1) && Near(got[2].y,3) && Near(got[4].z,4));

    // Point fields are accepted and evaluated at every CV; primitive fields
    // broadcast per curve. A zero envelope is an exact no-op and keep=1.
    DeviceBuffer<float> pointBlend, primitiveThreshold;
    CHECK(pointBlend.reset(5) == cudaSuccess && primitiveThreshold.reset(2) == cudaSuccess);
    CHECK(Upload(pointBlend, {0,0,1,1,1}, producer) &&
          Upload(primitiveThreshold, {1.5f,3.1f}, producer) &&
          cudaStreamSynchronize(producer) == cudaSuccess);
    p = LengthParameters{};
    p.value = ScalarField::Literal(2.0f);
    p.blend = ScalarField::Device(Const(pointBlend), expr::Domain::Point);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer) &&
          Download(keep, gotKeep, consumer));
    CHECK(Near(got[0].x,0) && Near(got[1].x,1) && Near(got[2].x,2));
    p = LengthParameters{}; p.mode = LengthMode::Cull;
    p.cullThreshold = ScalarField::Device(Const(primitiveThreshold), expr::Domain::Primitive);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer) &&
          Download(keep, gotKeep, consumer));
    CHECK(std::memcmp(got.data(), source.data(), sizeof(source[0])*source.size()) == 0 &&
          gotKeep[0] == 1 && gotKeep[1] == 0);

    // Disabled and all-zero envelopes preserve both geometry and topology.
    p = LengthParameters{}; p.value = ScalarField::Literal(9);
    p.enabled = BoolField::Literal(false);
    p.cullThreshold = ScalarField::Literal(100);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer) &&
          Download(keep, gotKeep, consumer));
    CHECK(std::memcmp(got.data(), source.data(), sizeof(source[0])*source.size()) == 0 &&
          gotKeep[0] == 1 && gotKeep[1] == 1);

    DeviceBuffer<float> pointValue, zeroProfile;
    CHECK(pointValue.reset(5) == cudaSuccess && zeroProfile.reset(257) == cudaSuccess);
    CHECK(Upload(pointValue, {0,1,2,0,1}, producer) &&
          Upload(zeroProfile, std::vector<float>(257,0), producer));
    p = LengthParameters{}; p.mode = LengthMode::Set;
    p.value = ScalarField::Device(Const(pointValue), expr::Domain::Point);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    CHECK(Near(got[1].x,.5f) && Near(got[2].y,1) && Near(got[4].z,1));
    // Scale + cutExtend must use each CV's expression result, not the root's.
    CHECK(Upload(pointValue, {0,.5f,2,0,1}, producer));
    p.mode = LengthMode::Scale; p.method = LengthMethod::CutExtend;
    p.rebuild = LengthRebuild::Reparam;
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    CHECK(Near(got[1].x,.5f) && Near(got[1].y,0) && Near(got[2].y,3));
    p = LengthParameters{}; p.value = ScalarField::Literal(.1f);
    p.minRemainingLength = ScalarField::Literal(2.5f);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    CHECK(Near(got[2].x,1.25f) && Near(got[2].y,1.25f) && Near(got[4].z,2.5f));
    p = LengthParameters{}; p.mode = LengthMode::Cull;
    p.cullThreshold = ScalarField::Literal(100);
    p.maskProfile = Const(zeroProfile);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::Ok && Download(keep, gotKeep, consumer));
    CHECK(gotKeep == std::vector<uint8_t>({1,1}));

    // Device validation catches malformed offsets and nonfinite input without
    // publishing staging. Host field-count validation queues no work.
    CHECK(Upload(offsets, {0u,3u,9u}, producer) &&
          cudaMemsetAsync(output.data(), 0x5A, output.size()*sizeof(float3), producer) == cudaSuccess);
    CHECK(op.Apply(geometry, {}, LengthParameters{}, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::InvalidArgument);
    CHECK(Download(output, got, consumer));
    std::vector<unsigned char> sentinel(sizeof(float3)*got.size(), 0x5A);
    CHECK(std::memcmp(got.data(), sentinel.data(), sentinel.size()) == 0);
    CHECK(Upload(offsets, {0u,3u,5u}, producer));
    std::vector<float3> bad = source; bad[3].x = NAN;
    CHECK(Upload(points, bad, producer));
    CHECK(op.Apply(geometry, {}, LengthParameters{}, output.view(), keep.view(), producer) == StyleStatus::Ok &&
          op.Finish(consumer) == StyleStatus::NonFiniteInput);
    CHECK(Download(output, got, consumer));
    CHECK(std::memcmp(got.data(), sentinel.data(), sentinel.size()) == 0);
    CHECK(LengthParameters{}.value.domain == expr::Domain::Groom);
    p = LengthParameters{};
    DeviceBuffer<float> shortField; CHECK(shortField.reset(1) == cudaSuccess);
    p.value = ScalarField::Device(Const(shortField), expr::Domain::Primitive);
    CHECK(op.Apply(geometry, {}, p, output.view(), keep.view(), consumer) == StyleStatus::InvalidArgument);

    // Empty geometry (including empty point-domain fields) is a successful no-op.
    DeviceBuffer<uint32_t> emptyOffsets; CHECK(emptyOffsets.reset(1) == cudaSuccess);
    CHECK(Upload(emptyOffsets, {0u}, producer) && cudaStreamSynchronize(producer) == cudaSuccess);
    DeviceCurveGeometryView empty{{}, {}, {}, {emptyOffsets.data(),1}, {}, 0, 0};
    CudaLength emptyOp;
    CHECK(emptyOp.Apply(empty, {}, LengthParameters{}, {}, {}, producer) == StyleStatus::Ok &&
          emptyOp.Finish(consumer) == StyleStatus::Ok && emptyOp.deviceIndex() >= 0);

    CHECK(cudaStreamDestroy(producer) == cudaSuccess && cudaStreamDestroy(consumer) == cudaSuccess);
    std::puts("testUsdGenCudaLength: PASS");
    return 0;
}
