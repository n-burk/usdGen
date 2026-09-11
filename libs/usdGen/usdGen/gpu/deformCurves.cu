#include "deformCurves.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace usdGen { namespace gpu { namespace {

constexpr int kBadOffsets = 1;
constexpr int kNonFinite = 2;
constexpr int kBadValue = 3;

__device__ void SetError(int *error, int code) { atomicCAS(error, 0, code); }
__device__ bool Finite(float value) { return isfinite(value); }

__device__ bool ReadScalar(ScalarField field, size_t curve, size_t point,
                           float *value, int *error) {
    size_t index = 0;
    if (!field.data) {
        if (field.count != 0 || field.domain != expr::Domain::Groom) {
            SetError(error, kBadValue); return false;
        }
        *value = field.literal;
    } else {
        if (field.domain == expr::Domain::Primitive) index = curve;
        else if (field.domain == expr::Domain::Point) index = point;
        else if (field.domain != expr::Domain::Groom) {
            SetError(error, kBadValue); return false;
        }
        if (index >= field.count) { SetError(error, kBadValue); return false; }
        *value = field.data[index];
    }
    if (!Finite(*value)) { SetError(error, kNonFinite); return false; }
    if (*value < 0.0f || *value > 1.0f) {
        SetError(error, kBadValue); return false;
    }
    return true;
}

__device__ bool ReadBool(BoolField field, size_t curve, size_t point,
                         bool *value, int *error) {
    size_t index = 0;
    uint8_t raw = field.literal ? 1u : 0u;
    if (field.data) {
        if (field.domain == expr::Domain::Primitive) index = curve;
        else if (field.domain == expr::Domain::Point) index = point;
        else if (field.domain != expr::Domain::Groom) {
            SetError(error, kBadValue); return false;
        }
        if (index >= field.count) { SetError(error, kBadValue); return false; }
        raw = field.data[index];
    } else if (field.count != 0 || field.domain != expr::Domain::Groom) {
        SetError(error, kBadValue); return false;
    }
    if (raw > 1u) { SetError(error, kBadValue); return false; }
    *value = raw != 0;
    return true;
}

__device__ float Sample257(const float *lut, float t) {
    float coordinate = t * 256.0f;
    int lower = int(floorf(coordinate));
    if (lower >= 256) return lut[256];
    float fraction = coordinate - float(lower);
    return lut[lower] + (lut[lower + 1] - lut[lower]) * fraction;
}

__global__ void ValidateProfile(const float *profile, int *error) {
    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= 257) return;
    float value = profile[i];
    if (!Finite(value)) SetError(error, kNonFinite);
    else if (value < 0.0f || value > 1.0f) SetError(error, kBadValue);
}

__global__ void ValidateShape(const uint32_t *offsets, int curves, int points,
                              const float3 *roots, int *error) {
    if (blockIdx.x == 0 && threadIdx.x == 0 &&
        (offsets[0] != 0 || offsets[curves] != uint32_t(points)))
        SetError(error, kBadOffsets);
    size_t curve = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (curve >= size_t(curves)) return;
    uint32_t begin = offsets[curve], end = offsets[curve + 1];
    float3 root = roots[curve];
    if (begin >= end || end > uint32_t(points) || !Finite(root.x) ||
        !Finite(root.y) || !Finite(root.z)) SetError(error, kBadOffsets);
}

__global__ void ApplyDeformation(
    const float3 *input, const float3 *warped, const uint32_t *offsets,
    const float3 *targets, int curves, float legacyGroom,
    const float *legacyPrimitive, const float *legacyPoint,
    ScalarField blend, ScalarField maskAmount, BoolField enabled,
    BoolField lockRoots, const float *maskProfile, const float *hairT,
    float3 *output, int *error) {
    int curve = int(blockIdx.x) * blockDim.x + threadIdx.x;
    if (curve >= curves) return;
    uint32_t begin = offsets[curve], end = offsets[curve + 1];
    bool groomEnabled = true;
    if (!ReadBool(enabled, size_t(curve), begin, &groomEnabled, error)) return;
    bool lock = true;
    if (!ReadBool(lockRoots, size_t(curve), begin, &lock, error)) return;
    float primitive = 1.0f;
    if (legacyPrimitive) {
        primitive = legacyPrimitive[curve];
        if (!Finite(primitive)) { SetError(error, kNonFinite); return; }
    }
    if (!Finite(legacyGroom)) { SetError(error, kNonFinite); return; }
    float3 correction = make_float3(0, 0, 0);
    if (lock) {
        correction = make_float3(targets[curve].x - warped[begin].x,
                                 targets[curve].y - warped[begin].y,
                                 targets[curve].z - warped[begin].z);
        if (!Finite(correction.x) || !Finite(correction.y) ||
            !Finite(correction.z)) { SetError(error, kNonFinite); return; }
    }
    for (uint32_t point = begin; point < end; ++point) {
        float3 source = input[point], evaluated = warped[point];
        if (!Finite(source.x) || !Finite(source.y) || !Finite(source.z) ||
            !Finite(evaluated.x) || !Finite(evaluated.y) ||
            !Finite(evaluated.z)) { SetError(error, kNonFinite); continue; }
        float blendValue = 1.0f, maskValue = 1.0f;
        if (!ReadScalar(blend, size_t(curve), point, &blendValue, error) ||
            !ReadScalar(maskAmount, size_t(curve), point, &maskValue, error)) continue;
        float pointEnvelope = 1.0f;
        if (legacyPoint) {
            pointEnvelope = legacyPoint[point];
            if (!Finite(pointEnvelope)) { SetError(error, kNonFinite); continue; }
        }
        float t = end - begin <= 1 ? 0.0f :
            float(point - begin) / float(end - begin - 1);
        if (hairT) t = hairT[point];
        if (!Finite(t)) { SetError(error, kNonFinite); continue; }
        if (t < 0.0f || t > 1.0f) { SetError(error, kBadValue); continue; }
        float profile = maskProfile ? Sample257(maskProfile, t) : 1.0f;
        if (!Finite(profile)) { SetError(error, kNonFinite); continue; }
        if (profile < 0.0f || profile > 1.0f) {
            SetError(error, kBadValue); continue;
        }
        float envelope = legacyGroom * primitive * pointEnvelope * blendValue *
                         maskValue * profile;
        if (!Finite(envelope)) { SetError(error, kNonFinite); continue; }
        envelope = fminf(1.0f, fmaxf(0.0f, envelope));
        float3 result = source;
        if (groomEnabled && envelope > 0.0f) {
            float3 destination = lock
                ? make_float3(evaluated.x + correction.x,
                              evaluated.y + correction.y,
                              evaluated.z + correction.z)
                : evaluated;
            result = make_float3(source.x + (destination.x - source.x) * envelope,
                                 source.y + (destination.y - source.y) * envelope,
                                 source.z + (destination.z - source.z) * envelope);
        }
        if (!Finite(result.x) || !Finite(result.y) || !Finite(result.z)) {
            SetError(error, kNonFinite); continue;
        }
        output[point] = result;
    }
}

bool IsDomain(expr::Domain domain) {
    return domain == expr::Domain::Groom || domain == expr::Domain::Primitive ||
           domain == expr::Domain::Point;
}

RbfStatus ValidateScalar(ScalarField field, DeviceCurveGeometryView geometry) {
    if (!IsDomain(field.domain)) return RbfStatus::InvalidArgument;
    size_t expected = field.domain == expr::Domain::Groom ? 1 :
        (field.domain == expr::Domain::Primitive ? geometry.curveCount : geometry.pointCount);
    if (!field.data) {
        if (field.count != 0 || field.domain != expr::Domain::Groom)
            return RbfStatus::InvalidArgument;
        if (!std::isfinite(field.literal)) return RbfStatus::NonFiniteInput;
        return field.literal < 0.0f || field.literal > 1.0f
            ? RbfStatus::InvalidArgument : RbfStatus::Ok;
    }
    return field.count == expected &&
                   field.count <= size_t(std::numeric_limits<int>::max())
               ? RbfStatus::Ok : RbfStatus::InvalidArgument;
}

RbfStatus ValidateBool(BoolField field, DeviceCurveGeometryView geometry,
                       bool allowPrimitive) {
    if (!IsDomain(field.domain) ||
        (allowPrimitive && field.domain == expr::Domain::Point) ||
        (!allowPrimitive && field.domain != expr::Domain::Groom))
        return RbfStatus::InvalidArgument;
    size_t expected = field.domain == expr::Domain::Groom ? 1 :
        (field.domain == expr::Domain::Primitive ? geometry.curveCount : geometry.pointCount);
    if (!field.data)
        return field.count == 0 && field.domain == expr::Domain::Groom
            ? RbfStatus::Ok : RbfStatus::InvalidArgument;
    return field.count == expected &&
                   field.count <= size_t(std::numeric_limits<int>::max())
               ? RbfStatus::Ok : RbfStatus::InvalidArgument;
}

RbfStatus CheckDevice(int *deviceIndex, cudaStream_t stream) {
    int current = -1;
    if (cudaGetDevice(&current) != cudaSuccess) return RbfStatus::CudaError;
    int streamDevice = current;
    if (stream && cudaStreamGetDevice(stream, &streamDevice) != cudaSuccess)
        return RbfStatus::CudaError;
    if (streamDevice != current || (*deviceIndex >= 0 && *deviceIndex != current))
        return RbfStatus::InvalidArgument;
    if (*deviceIndex < 0) *deviceIndex = current;
    return RbfStatus::Ok;
}

} // namespace anonymous

CudaRbfCurveDeformer::~CudaRbfCurveDeformer() {
    // DeviceBuffer::release uses the current CUDA context.  Select the owner
    // before synchronizing/destroying anything, and never free through a
    // foreign or lost context.  A deliberately leaked quarantine is safer
    // than handing an opaque allocation to the wrong context.
    const bool ownsBuffers = warped_.size() || staged_.size() || flags_.size() || ready_;
    int previous = -1;
    const bool selected = !ownsBuffers ||
        (cudaGetDevice(&previous) == cudaSuccess && deviceIndex_ >= 0 &&
         cudaSetDevice(deviceIndex_) == cudaSuccess);
    const bool synchronized = !ownsBuffers ||
        (selected && (!ready_ || cudaEventSynchronize(ready_) == cudaSuccess));
    if (!selected || !synchronized) {
        if (ownsBuffers) {
            warped_.quarantine();
            staged_.quarantine();
            flags_.quarantine();
            ready_ = nullptr;
        }
        if (selected && previous >= 0 && previous != deviceIndex_)
            cudaSetDevice(previous);
        return;
    }
    if (ready_) { cudaEventDestroy(ready_); ready_ = nullptr; }
    warped_.reset(0);
    staged_.reset(0);
    flags_.reset(0);
    if (previous >= 0 && previous != deviceIndex_) cudaSetDevice(previous);
}

RbfStatus CudaRbfCurveDeformer::Deform(
    CudaRbfBinding& rbf, DeviceCurveGeometryView geometry,
    DeviceView<const float3> rootTargets, float groomEnvelope,
    DeviceView<const float> primitiveEnvelope,
    DeviceView<const float> pointEnvelope, DeviceView<float3> output,
    cudaStream_t stream) {
    // Preserve the legacy contract: its restPoints member is the source.
    if (geometry.restPoints.size != geometry.pointCount ||
        (geometry.pointCount && !geometry.restPoints.data))
        return RbfStatus::InvalidArgument;
    DeviceCurveGeometryView legacy = geometry;
    legacy.points = geometry.restPoints;
    DeformParameters parameters;
    return deformImpl(rbf, legacy, rootTargets, parameters, groomEnvelope,
                      primitiveEnvelope, pointEnvelope, output, stream);
}

RbfStatus CudaRbfCurveDeformer::Deform(
    CudaRbfBinding& rbf, DeviceCurveGeometryView geometry,
    DeviceView<const float3> rootTargets, DeformParameters parameters,
    DeviceView<float3> output, cudaStream_t stream) {
    return deformImpl(rbf, geometry, rootTargets, parameters, 1.0f, {}, {},
                      output, stream);
}

RbfStatus CudaRbfCurveDeformer::deformImpl(
    CudaRbfBinding& rbf, DeviceCurveGeometryView geometry,
    DeviceView<const float3> rootTargets, DeformParameters parameters,
    float groomEnvelope, DeviceView<const float> primitiveEnvelope,
    DeviceView<const float> pointEnvelope, DeviceView<float3> output,
    cudaStream_t stream) {
    if (pending_ || poisoned_) return RbfStatus::InvalidArgument;
    RbfStatus status = CheckDevice(&deviceIndex_, stream);
    if (status != RbfStatus::Ok) return status;
    if (geometry.curveCount > size_t(std::numeric_limits<int>::max()) ||
        geometry.pointCount > size_t(std::numeric_limits<int>::max()) ||
        geometry.pointCount > size_t(std::numeric_limits<uint32_t>::max()) ||
        (geometry.curveCount == 0 && geometry.pointCount != 0) ||
        (geometry.curveCount && geometry.pointCount == 0) ||
        (geometry.curveCount && (!geometry.curveOffsets.data ||
         geometry.curveOffsets.size != geometry.curveCount + 1)) ||
        (!geometry.curveCount && geometry.curveOffsets.size != 0 &&
         geometry.curveOffsets.size != 1) ||
        (!geometry.curveCount && geometry.curveOffsets.size == 1 &&
         !geometry.curveOffsets.data) ||
        (geometry.pointCount && (!geometry.points.data ||
         geometry.points.size != geometry.pointCount)) ||
        (!geometry.pointCount && geometry.points.size != 0) ||
        (geometry.curveCount && (!rootTargets.data ||
         rootTargets.size != geometry.curveCount)) ||
        (!geometry.curveCount && rootTargets.size != 0) ||
        output.size != geometry.pointCount ||
        (geometry.pointCount && !output.data) ||
        (!geometry.pointCount && output.size != 0) ||
        (!parameters.maskProfile.data && parameters.maskProfile.size != 0) ||
        (parameters.maskProfile.data && parameters.maskProfile.size != 257) ||
        (!parameters.hairT.data && parameters.hairT.size != 0) ||
        (parameters.hairT.data && parameters.hairT.size != geometry.pointCount) ||
        (!primitiveEnvelope.data && primitiveEnvelope.size != 0) ||
        (primitiveEnvelope.data && primitiveEnvelope.size != geometry.curveCount) ||
        (!pointEnvelope.data && pointEnvelope.size != 0) ||
        (pointEnvelope.data && pointEnvelope.size != geometry.pointCount) ||
        !std::isfinite(groomEnvelope))
        return RbfStatus::InvalidArgument;
    status = ValidateScalar(parameters.blend, geometry);
    if (status != RbfStatus::Ok) return status;
    status = ValidateScalar(parameters.maskAmount, geometry);
    if (status != RbfStatus::Ok) return status;
    status = ValidateBool(parameters.enabled, geometry, false);
    if (status != RbfStatus::Ok) return status;
    status = ValidateBool(parameters.lockRoots, geometry, true);
    if (status != RbfStatus::Ok) return status;
    if (!ready_ && cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming) != cudaSuccess)
        return RbfStatus::CudaError;
    if (flags_.reset(1) != cudaSuccess || warped_.reset(geometry.pointCount) != cudaSuccess ||
        staged_.reset(geometry.pointCount) != cudaSuccess ||
        cudaMemsetAsync(flags_.data(), 0, sizeof(int), stream) != cudaSuccess)
        return RbfStatus::CudaError;
    if (parameters.maskProfile.data) {
        ValidateProfile<<<2, 256, 0, stream>>>(parameters.maskProfile.data,
                                                flags_.data());
        if (cudaGetLastError() != cudaSuccess) return RbfStatus::CudaError;
    }
    if (geometry.curveCount || geometry.curveOffsets.data) {
        const size_t blocks = geometry.curveCount
            ? (geometry.curveCount + 255) / 256 : 1;
        ValidateShape<<<blocks, 256, 0, stream>>>(
            geometry.curveOffsets.data, int(geometry.curveCount),
            int(geometry.pointCount), rootTargets.data, flags_.data());
        if (cudaGetLastError() != cudaSuccess) return RbfStatus::CudaError;
    }
    int shapeError = 0;
    if (cudaMemcpyAsync(&shapeError, flags_.data(), sizeof(shapeError),
                        cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
        cudaStreamSynchronize(stream) != cudaSuccess)
        return RbfStatus::CudaError;
    if (shapeError == kNonFinite) return RbfStatus::NonFiniteInput;
    if (shapeError) return RbfStatus::InvalidArgument;
    if (!geometry.pointCount) return rbf.Finish(stream);
    status = rbf.Evaluate(geometry.points, warped_.view(), stream);
    if (status != RbfStatus::Ok) return status;
    ApplyDeformation<<<(geometry.curveCount + 255) / 256, 256, 0, stream>>>(
        geometry.points.data, warped_.data(), geometry.curveOffsets.data,
        rootTargets.data, int(geometry.curveCount), groomEnvelope,
        primitiveEnvelope.data, pointEnvelope.data,
        parameters.blend, parameters.maskAmount, parameters.enabled,
        parameters.lockRoots, parameters.maskProfile.data, parameters.hairT.data,
        staged_.data(), flags_.data());
    if (cudaGetLastError() != cudaSuccess || cudaEventRecord(ready_, stream) != cudaSuccess)
        return RbfStatus::CudaError;
    output_ = output;
    outputCount_ = geometry.pointCount;
    pending_ = true;
    return RbfStatus::Ok;
}

RbfStatus CudaRbfCurveDeformer::Finish(CudaRbfBinding& rbf, cudaStream_t stream) {
    RbfStatus status = CheckDevice(&deviceIndex_, stream);
    if (status != RbfStatus::Ok) return status;
    if (!pending_) return rbf.Finish(stream);
    int error = 0;
    if (cudaStreamWaitEvent(stream, ready_, 0) != cudaSuccess ||
        cudaMemcpyAsync(&error, flags_.data(), sizeof(error), cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
        cudaStreamSynchronize(stream) != cudaSuccess)
        return RbfStatus::CudaError;
    pending_ = false;
    RbfStatus bindingStatus = rbf.Finish(stream);
    DeviceView<float3> destination = output_;
    output_ = {};
    outputCount_ = 0;
    if (bindingStatus != RbfStatus::Ok) return bindingStatus;
    if (error == kBadOffsets || error == kBadValue) return RbfStatus::InvalidArgument;
    if (error == kNonFinite) { poisoned_ = true; return RbfStatus::NonFiniteInput; }
    if (error != 0) return RbfStatus::CudaError;
    if (cudaMemcpyAsync(destination.data, staged_.data(),
                        staged_.size() * sizeof(float3), cudaMemcpyDeviceToDevice,
                        stream) != cudaSuccess || cudaStreamSynchronize(stream) != cudaSuccess)
        return RbfStatus::CudaError;
    return RbfStatus::Ok;
}

}} // namespace usdGen::gpu
