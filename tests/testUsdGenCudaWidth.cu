#include "gpu/width.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace usdGen;
using namespace usdGen::gpu;

#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                                      #condition, __FILE__, __LINE__); return 1; } \
} while (false)

static bool Near(float a, float b) { return std::fabs(a - b) < 2.0e-5f; }

template <class T>
static bool Upload(DeviceBuffer<T> &device, std::vector<T> const &host,
                   cudaStream_t stream) {
    return cudaMemcpyAsync(device.data(), host.data(),
                           host.size() * sizeof(T), cudaMemcpyHostToDevice,
                           stream) == cudaSuccess;
}

template <class T>
static bool Download(DeviceBuffer<T> const &device, std::vector<T> &host,
                     cudaStream_t stream) {
    return cudaMemcpyAsync(host.data(), device.data(),
                           host.size() * sizeof(T), cudaMemcpyDeviceToHost,
                           stream) == cudaSuccess &&
           cudaStreamSynchronize(stream) == cudaSuccess;
}

static DeviceCurveGeometryView Geometry(DeviceBuffer<float> const &widths,
                                        DeviceBuffer<uint32_t> const &offsets) {
    return {{}, {}, {widths.data(), widths.size()},
            {offsets.data(), offsets.size()}, {}, 2, 5};
}

static WidthParameters Defaults(DeviceView<const float> profile) {
    WidthParameters p;
    p.widthProfile = profile;
    return p;
}

int main() {
    cudaStream_t producer = nullptr, consumer = nullptr;
    CHECK(cudaStreamCreate(&producer) == cudaSuccess);
    CHECK(cudaStreamCreate(&consumer) == cudaSuccess);

    DeviceBuffer<float> widths, output, profile, maskProfile, pointField;
    DeviceBuffer<uint32_t> offsets;
    CHECK(widths.reset(5) == cudaSuccess && output.reset(5) == cudaSuccess &&
          offsets.reset(3) == cudaSuccess && profile.reset(257) == cudaSuccess &&
          maskProfile.reset(257) == cudaSuccess && pointField.reset(5) == cudaSuccess);
    CHECK(Upload(widths, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}, producer));
    CHECK(Upload(offsets, {0u, 2u, 5u}, producer));
    CHECK(Upload(profile, std::vector<float>(257, 1.0f), producer));
    CHECK(Upload(maskProfile, std::vector<float>(257, 1.0f), producer));
    CHECK(cudaStreamSynchronize(producer) == cudaSuccess);
    DeviceCurveGeometryView geometry = Geometry(widths, offsets);

    // Variable 2/3-CV curves, supplied hairT, target profile arithmetic, and
    // cross-stream Finish. The profile is flat here, so the result is easy to
    // compare with the root/tip and taper equations.
    std::vector<float> hairTHost{0.0f, 1.0f, 0.0f, .5f, 1.0f};
    DeviceBuffer<float> hairT;
    CHECK(hairT.reset(5) == cudaSuccess && Upload(hairT, hairTHost, producer));
    CHECK(cudaStreamSynchronize(producer) == cudaSuccess);
    WidthParameters p = Defaults({profile.data(), profile.size()});
    p.width = ScalarField::Literal(2.0f);
    p.rootScale = ScalarField::Literal(1.0f);
    p.tipScale = ScalarField::Literal(3.0f);
    p.taper = ScalarField::Literal(.5f);
    p.taperStart = ScalarField::Literal(.5f);
    CHECK(CudaWidth{}.pending() == false);
    CudaWidth op;
    CHECK(op.Apply(geometry, {hairT.data(), hairT.size()}, p,
                   {output.data(), output.size()}, producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::Ok);
    std::vector<float> got(5);
    CHECK(Download(output, got, consumer));
    CHECK(Near(got[0], 2.0f) && Near(got[1], 3.0f) &&
          Near(got[2], 2.0f) && Near(got[3], 4.0f) && Near(got[4], 3.0f));

    // Primitive-domain broadcasting and replace=false multiplication.
    DeviceBuffer<float> primitive;
    CHECK(primitive.reset(2) == cudaSuccess &&
          Upload(primitive, {2.0f, 4.0f}, producer));
    p = Defaults({profile.data(), profile.size()});
    p.width = ScalarField::Device({primitive.data(), 2}, expr::Domain::Primitive);
    p.replace = BoolField::Literal(false);
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    CHECK(Near(got[0], 2.0f) && Near(got[1], 4.0f) &&
          Near(got[2], 12.0f) && Near(got[3], 16.0f) && Near(got[4], 20.0f));

    // Point-domain controls, a non-flat width profile, and mask profile.
    std::vector<float> profileHost(257), maskHost(257);
    for (size_t i = 0; i < 257; ++i) {
        profileHost[i] = 1.0f + float(i) / 256.0f;
        maskHost[i] = .5f + .5f * float(i) / 256.0f;
    }
    CHECK(Upload(profile, profileHost, producer) &&
          Upload(maskProfile, maskHost, producer) &&
          Upload(pointField, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}, producer));
    p = Defaults({profile.data(), profile.size()});
    p.maskProfile = {maskProfile.data(), maskProfile.size()};
    p.width = ScalarField::Device({pointField.data(), 5}, expr::Domain::Point);
    p.maskAmount = ScalarField::Literal(1.0f);
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    // At each canonical t, target = pointWidth * widthProfile[t], then the
    // mask profile is the envelope. The first and last values are exact.
    CHECK(Near(got[0], 1.0f) && Near(got[1], 4.0f) &&
          Near(got[2], 3.0f) && Near(got[3], 5.5f) &&
          Near(got[4], 10.0f));

    // Exact-zero enabled/envelope paths copy the upstream width bitwise.
    std::vector<float> sentinel{1.0f, -0.0f, 3.0f, 4.0f, 5.0f};
    CHECK(Upload(widths, sentinel, producer));
    p = Defaults({profile.data(), profile.size()});
    p.blend = ScalarField::Literal(0.0f);
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    CHECK(std::memcmp(got.data(), sentinel.data(), got.size() * sizeof(float)) == 0);
    p.blend = ScalarField::Literal(1.0f);
    p.enabled = BoolField::Literal(false);
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::Ok && Download(output, got, consumer));
    CHECK(std::memcmp(got.data(), sentinel.data(), got.size() * sizeof(float)) == 0);

    // Device diagnostics reject malformed offsets, nonfinite/negative source
    // widths and invalid profile values without publishing staging output.
    CHECK(Upload(widths, std::vector<float>(5, 91.0f), producer) &&
          Upload(output, std::vector<float>(5, 91.0f), producer) &&
          Upload(offsets, {0u, 2u, 9u}, producer));
    CHECK(op.Apply(geometry, {}, Defaults({profile.data(), profile.size()}),
                   {output.data(), 5}, producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::InvalidArgument);
    CHECK(Download(output, got, consumer));
    for (float value : got) CHECK(Near(value, 91.0f));
    CHECK(Upload(offsets, {0u, 2u, 5u}, producer));
    std::vector<float> badWidths{1.0f, NAN, 3.0f, 4.0f, 5.0f};
    CHECK(Upload(widths, badWidths, producer));
    p = Defaults({profile.data(), profile.size()});
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::NonFiniteInput);
    CHECK(Upload(widths, std::vector<float>(5, 91.0f), producer));
    p.width = ScalarField::Literal(-1.0f);
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, producer) ==
          StyleStatus::InvalidValue);
    std::vector<float> badProfile(257, 1.0f);
    badProfile[12] = -1.0f;
    CHECK(Upload(profile, badProfile, producer));
    p = Defaults({profile.data(), profile.size()});
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, producer) == StyleStatus::Ok);
    CHECK(op.Finish(consumer) == StyleStatus::InvalidValue);
    CHECK(Upload(profile, std::vector<float>(257, 1.0f), producer));

    // Host-invalid field counts queue no work; a valid operation can follow
    // immediately on another stream. Device bool values are byte-validated.
    p = Defaults({profile.data(), profile.size()});
    p.width = ScalarField::Device({primitive.data(), 1}, expr::Domain::Primitive);
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, producer) ==
          StyleStatus::InvalidArgument);
    p.width = ScalarField::Literal(2.0f);
    std::vector<unsigned char> badBool{2u, 1u};
    DeviceBuffer<unsigned char> boolBytes;
    CHECK(boolBytes.reset(2) == cudaSuccess && Upload(boolBytes, badBool, producer));
    CHECK(cudaStreamSynchronize(producer) == cudaSuccess);
    p.replace = BoolField::Device(
        {reinterpret_cast<const uint8_t *>(boolBytes.data()), 2},
        expr::Domain::Primitive);
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, consumer) == StyleStatus::Ok);
    CHECK(op.Finish(producer) == StyleStatus::InvalidValue);
    p.replace = BoolField::Literal(true);
    CHECK(op.Apply(geometry, {}, p, {output.data(), 5}, consumer) == StyleStatus::Ok);
    CHECK(op.Finish(producer) == StyleStatus::Ok);

    // Empty geometry is a successful no-op; profile shape remains required.
    DeviceCurveGeometryView empty{{}, {}, {}, {}, {}, 0, 0};
    CudaWidth emptyOp;
    CHECK(emptyOp.Apply(empty, {}, Defaults({profile.data(), profile.size()}),
                        {}, producer) == StyleStatus::Ok);
    CHECK(emptyOp.Finish(consumer) == StyleStatus::Ok);

    cudaStreamDestroy(producer);
    cudaStreamDestroy(consumer);
    return 0;
}
