#include "gpu/deformCurves.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace usdGen::gpu;
#define CHECK(X) do { if (!(X)) { \
    std::fprintf(stderr, "check failed at %d: %s\n", __LINE__, #X); return 1; \
} } while (0)

static float3 P(float x, float y, float z) { return make_float3(x, y, z); }
static bool Near(float3 a, float3 b) {
    return std::fabs(a.x-b.x) < 4.e-4f && std::fabs(a.y-b.y) < 4.e-4f &&
           std::fabs(a.z-b.z) < 4.e-4f;
}
template<class T>
static bool Upload(DeviceBuffer<T>& buffer, std::vector<T> const& values) {
    return buffer.reset(values.size()) == cudaSuccess &&
        cudaMemcpy(buffer.data(), values.data(), values.size()*sizeof(T),
                   cudaMemcpyHostToDevice) == cudaSuccess;
}
template<class T>
static std::vector<T> Read(DeviceBuffer<T> const& buffer) {
    std::vector<T> values(buffer.size());
    if (cudaMemcpy(values.data(), buffer.data(), values.size()*sizeof(T),
                   cudaMemcpyDeviceToHost) != cudaSuccess) values.clear();
    return values;
}

int main() {
    cudaStream_t producer = nullptr, consumer = nullptr;
    const cudaError_t initialized = cudaStreamCreate(&producer);
    if (initialized != cudaSuccess) {
        std::fprintf(stderr, "CUDA initialization: %s\n", cudaGetErrorString(initialized));
        return 1;
    }
    CHECK(cudaStreamCreate(&consumer) == cudaSuccess);
    {
        const std::vector<float3> samples = {
            P(0,0,0), P(1,0,0), P(0,1,0), P(0,0,1), P(1,1,1)};
        const std::vector<float3> rest = {
            P(.2,.1,.1), P(.3,.1,.4), P(.6,.2,.1), P(.7,.4,.3), P(.5,.6,.5)};
        const std::vector<uint32_t> offsets = {0,2,5}; // variable 2/3 CVs
        DeviceBuffer<float3> drivers, posed, points, targets, output, warp;
        DeviceBuffer<uint32_t> deviceOffsets;
        DeviceBuffer<float> primitive, pointMask;
        CHECK(Upload(drivers, samples) && Upload(points, rest) &&
              Upload(deviceOffsets, offsets));
        CHECK(output.reset(5) == cudaSuccess && warp.reset(5) == cudaSuccess);
        DeviceCurveGeometryView geometry = {
            {points.data(),5}, {points.data(),5}, {}, {deviceOffsets.data(),3}, {}, 2,5};
        CudaRbfBinding binding;
        CudaRbfCurveDeformer deform;
        CHECK(binding.Bind({drivers.data(),5}, 0, producer) == RbfStatus::Ok);

        // Affine motion is exactly representable; roots follow explicit targets.
        auto animated = samples;
        for (auto& p : animated) p = P(-p.y+2, p.x-3, p.z+4);
        CHECK(Upload(posed, animated));
        CHECK(Upload(targets, std::vector<float3>{
            P(-rest[0].y+2,rest[0].x-3,rest[0].z+4),
            P(-rest[2].y+2,rest[2].x-3,rest[2].z+4)}));
        CHECK(binding.Solve({posed.data(),5}, producer) == RbfStatus::Ok);
        CHECK(deform.Deform(binding, geometry, {targets.data(),2}, 1, {}, {},
                            output.view(), producer) == RbfStatus::Ok);
        CHECK(deform.Finish(binding, consumer) == RbfStatus::Ok);
        auto actual = Read(output);
        CHECK(actual.size() == 5);
        for (unsigned i=0; i<5; ++i)
            CHECK(Near(actual[i], P(-rest[i].y+2,rest[i].x-3,rest[i].z+4)));

        // Reuse the rest binding for nonlinear motion. Check the whole curve's
        // root correction, not just whether some point changed.
        animated = samples;
        for (auto& p : animated) p.z += p.x*p.y;
        CHECK(Upload(posed, animated));
        CHECK(binding.Solve({posed.data(),5}, producer) == RbfStatus::Ok);
        CHECK(binding.Evaluate({points.data(),5}, warp.view(), producer) == RbfStatus::Ok);
        CHECK(binding.Finish(consumer) == RbfStatus::Ok);
        const auto warped = Read(warp);
        CHECK(warped.size() == 5 && !Near(warped[3], rest[3]));
        CHECK(Upload(targets, std::vector<float3>{rest[0],rest[2]}));
        CHECK(deform.Deform(binding, geometry, {targets.data(),2}, 1, {}, {},
                            output.view(), producer) == RbfStatus::Ok);
        CHECK(deform.Finish(binding, consumer) == RbfStatus::Ok);
        actual = Read(output);
        CHECK(actual.size() == 5);
        std::vector<float3> locked(5);
        for (unsigned i=0; i<5; ++i) {
            const unsigned root = i < 2 ? 0 : 2;
            locked[i] = P(warped[i].x + rest[root].x - warped[root].x,
                          warped[i].y + rest[root].y - warped[root].y,
                          warped[i].z + rest[root].z - warped[root].z);
            CHECK(Near(actual[i], locked[i]));
        }
        CHECK(Near(actual[0],rest[0]) && Near(actual[2],rest[2]));
        CHECK(!Near(actual[3],rest[3]));

        // Every envelope domain composes; zero, half and full remain numeric
        // oracles with variable CV counts and a different completion stream.
        const std::vector<float> primWeights = {0.5f,1.f};
        const std::vector<float> pointWeights = {1.f,0.f,1.f,0.5f,1.f};
        CHECK(Upload(primitive, primWeights) && Upload(pointMask, pointWeights));
        for (float groom : {0.f,0.5f,1.f}) {
            CHECK(deform.Deform(binding, geometry, {targets.data(),2}, groom,
                {primitive.data(),2}, {pointMask.data(),5}, output.view(), producer) == RbfStatus::Ok);
            CHECK(deform.Finish(binding, consumer) == RbfStatus::Ok);
            actual = Read(output);
            CHECK(actual.size() == 5);
            for (unsigned i=0; i<5; ++i) {
                const float e = groom * primWeights[i<2 ? 0 : 1] * pointWeights[i];
                CHECK(Near(actual[i], P(rest[i].x+(locked[i].x-rest[i].x)*e,
                    rest[i].y+(locked[i].y-rest[i].y)*e,
                    rest[i].z+(locked[i].z-rest[i].z)*e)));
            }
        }
        const auto beforeInvalid = actual;
        for (auto invalid : {std::vector<uint32_t>{0,2,4},
                             std::vector<uint32_t>{0,2,6},
                             std::vector<uint32_t>{0,0,5},
                             std::vector<uint32_t>{1,2,5}}) {
            CHECK(Upload(deviceOffsets, invalid));
            geometry.curveOffsets = {deviceOffsets.data(),3};
            CHECK(deform.Deform(binding, geometry, {targets.data(),2}, 1, {}, {},
                                output.view(), producer) == RbfStatus::InvalidArgument);
            actual = Read(output);
            CHECK(actual.size() == beforeInvalid.size());
            for (unsigned i=0; i<actual.size(); ++i) CHECK(Near(actual[i],beforeInvalid[i]));
        }
        CHECK(Upload(deviceOffsets, offsets));
        geometry.curveOffsets = {deviceOffsets.data(),3};
        CHECK(deform.Deform(binding, geometry, {targets.data(),2}, NAN, {}, {},
                            output.view(), producer) == RbfStatus::InvalidArgument);
        CHECK(deform.Deform(binding, geometry, {targets.data(),2}, 1, {nullptr,2}, {},
                            output.view(), producer) == RbfStatus::InvalidArgument);
        CHECK(deform.Deform(binding, geometry, {targets.data(),2}, 1, {}, {pointMask.data(),4},
                            output.view(), producer) == RbfStatus::InvalidArgument);

        // Nonfinite device fields invalidate the generation at Finish.
        CHECK(Upload(primitive, std::vector<float>{NAN,1.f}));
        CHECK(deform.Deform(binding, geometry, {targets.data(),2}, 1,
            {primitive.data(),2}, {}, output.view(), producer) == RbfStatus::Ok);
        CHECK(deform.Finish(binding, consumer) == RbfStatus::NonFiniteInput);
    }
    CHECK(cudaStreamDestroy(consumer) == cudaSuccess);
    CHECK(cudaStreamDestroy(producer) == cudaSuccess);
    std::puts("testUsdGenCudaDeform: PASS");
    return 0;
}
