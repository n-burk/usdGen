#include "width.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace usdGen::gpu {
namespace {

constexpr size_t kProfileSize = 257;
constexpr int kBadOffsets = 1;
constexpr int kNonFinite = 2;
constexpr int kBadValue = 3;

__device__ void SetError(int *error, int code) {
    atomicCAS(error, 0, code);
}

__device__ int LoadError(int const *error) {
    return atomicAdd(const_cast<int *>(error), 0);
}

__device__ bool Finite(float value) { return isfinite(value); }

__device__ bool ReadScalar(ScalarField field, size_t curve, size_t point,
                           float *value, int *error) {
    size_t index = 0;
    if (!field.data) {
        if (field.count != 0 || field.domain != expr::Domain::Groom) {
            SetError(error, kBadValue);
            return false;
        }
        *value = field.literal;
    } else {
        if (field.domain == expr::Domain::Primitive) index = curve;
        else if (field.domain == expr::Domain::Point) index = point;
        else if (field.domain != expr::Domain::Groom) {
            SetError(error, kBadValue);
            return false;
        }
        if (index >= field.count) {
            SetError(error, kBadValue);
            return false;
        }
        *value = field.data[index];
    }
    if (!Finite(*value)) {
        SetError(error, kNonFinite);
        return false;
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
            SetError(error, kBadValue);
            return false;
        }
        if (index >= field.count) {
            SetError(error, kBadValue);
            return false;
        }
        raw = field.data[index];
    } else if (field.count != 0 || field.domain != expr::Domain::Groom) {
        SetError(error, kBadValue);
        return false;
    }
    if (raw > 1u) {
        SetError(error, kBadValue);
        return false;
    }
    *value = raw != 0;
    return true;
}

__device__ bool ReadHairT(DeviceView<const float> hairT, size_t point,
                          float fallback, float *t, int *error) {
    *t = hairT.data ? hairT.data[point] : fallback;
    if (!Finite(*t)) {
        SetError(error, kNonFinite);
        return false;
    }
    if (*t < 0.0f || *t > 1.0f) {
        SetError(error, kBadValue);
        return false;
    }
    return true;
}

__device__ float Sample257(const float *lut, float t) {
    float coordinate = t * 256.0f;
    int lower = int(floorf(coordinate));
    if (lower >= 256) return lut[256];
    float fraction = coordinate - float(lower);
    return lut[lower] + (lut[lower + 1] - lut[lower]) * fraction;
}

__global__ void ValidateOffsets(const uint32_t *offsets, size_t curves,
                                size_t points, int *error) {
    if (blockIdx.x == 0 && threadIdx.x == 0 &&
        (offsets[0] != 0 || offsets[curves] != points))
        SetError(error, kBadOffsets);
    size_t curve = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (curve >= curves) return;
    uint32_t begin = offsets[curve];
    uint32_t end = offsets[curve + 1];
    if (begin >= end || end > points) SetError(error, kBadOffsets);
}

__global__ void ValidateProfile(const float *widthProfile,
                                const float *maskProfile, int *error) {
    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= kProfileSize) return;
    float widthValue = widthProfile[i];
    if (!Finite(widthValue)) SetError(error, kNonFinite);
    else if (widthValue < 0.0f) SetError(error, kBadValue);
    if (maskProfile) {
        float maskValue = maskProfile[i];
        if (!Finite(maskValue)) SetError(error, kNonFinite);
        else if (maskValue < 0.0f || maskValue > 1.0f)
            SetError(error, kBadValue);
    }
}

__global__ void WidthKernel(DeviceCurveGeometryView geometry,
                            DeviceView<const float> hairT,
                            WidthParameters parameters, float *output,
                            int *error) {
    size_t curve = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (curve >= geometry.curveCount || LoadError(error) != 0) return;
    uint32_t begin = geometry.curveOffsets.data[curve];
    uint32_t end = geometry.curveOffsets.data[curve + 1];
    float denominator = float(end - begin - 1u);
    for (uint32_t point = begin; point < end; ++point) {
        float fallback = denominator > 0.0f
            ? float(point - begin) / denominator : 0.0f;
        float t = 0.0f, base = 0.0f, width = 0.0f;
        float rootScale = 0.0f, tipScale = 0.0f;
        float taper = 0.0f, taperStart = 0.0f, blend = 0.0f;
        float maskAmount = 0.0f;
        bool enabled = true, replace = true;
        float input = geometry.widths.data[point];
        if (!Finite(input)) {
            SetError(error, kNonFinite);
            continue;
        }
        if (input < 0.0f) {
            SetError(error, kBadValue);
            continue;
        }
        if (!ReadHairT(hairT, point, fallback, &t, error) ||
            !ReadScalar(parameters.base, curve, point, &base, error) ||
            !ReadScalar(parameters.width, curve, point, &width, error) ||
            !ReadScalar(parameters.rootScale, curve, point, &rootScale, error) ||
            !ReadScalar(parameters.tipScale, curve, point, &tipScale, error) ||
            !ReadScalar(parameters.taper, curve, point, &taper, error) ||
            !ReadScalar(parameters.taperStart, curve, point, &taperStart, error) ||
            !ReadScalar(parameters.blend, curve, point, &blend, error) ||
            !ReadScalar(parameters.maskAmount, curve, point, &maskAmount, error) ||
            !ReadBool(parameters.enabled, curve, point, &enabled, error) ||
            !ReadBool(parameters.replace, curve, point, &replace, error))
            continue;
        if (base < 0.0f || width < 0.0f || rootScale < 0.0f ||
            tipScale < 0.0f || taper < 0.0f || taper > 1.0f ||
            taperStart < 0.0f || taperStart > 1.0f || blend < 0.0f ||
            blend > 1.0f || maskAmount < 0.0f || maskAmount > 1.0f) {
            SetError(error, kBadValue);
            continue;
        }
        float maskRamp = parameters.maskProfile.data
            ? Sample257(parameters.maskProfile.data, t) : 1.0f;
        if (!Finite(maskRamp)) {
            SetError(error, kNonFinite);
            continue;
        }
        float envelope = blend * maskAmount * maskRamp;
        if (!Finite(envelope) || envelope < 0.0f || envelope > 1.0f) {
            SetError(error, kBadValue);
            continue;
        }
        // Preserve the source bit pattern without doing any arithmetic at an
        // exact zero envelope, including when enabled is false.
        if (!enabled || envelope == 0.0f) {
            output[point] = input;
            continue;
        }
        float profile = Sample257(parameters.widthProfile.data, t);
        float taperTerm = 1.0f;
        if (taper > 0.0f && t > taperStart) {
            float span = 1.0f - taperStart;
            taperTerm = 1.0f - taper * (t - taperStart) /
                        (span > 0.0f ? span : 1.0f);
        }
        float target = base * width * profile *
            (rootScale + (tipScale - rootScale) * t) * taperTerm;
        float result = replace ? input + (target - input) * envelope
                               : input * (1.0f + (target - 1.0f) * envelope);
        if (!Finite(target) || !Finite(result) || target < 0.0f ||
            result < 0.0f) {
            SetError(error, !Finite(target) || !Finite(result)
                              ? kNonFinite : kBadValue);
            continue;
        }
        output[point] = result;
    }
}

bool IsDomain(expr::Domain domain) {
    return domain == expr::Domain::Groom ||
           domain == expr::Domain::Primitive ||
           domain == expr::Domain::Point;
}

StyleStatus DecodeError(int code) {
    if (code == kBadOffsets) return StyleStatus::InvalidArgument;
    if (code == kNonFinite) return StyleStatus::NonFiniteInput;
    if (code == kBadValue) return StyleStatus::InvalidValue;
    return StyleStatus::CudaError;
}

} // namespace

CudaWidth::~CudaWidth() {
    int previous = -1;
    cudaGetDevice(&previous);
    const bool selected = deviceIndex_ >= 0 &&
        cudaSetDevice(deviceIndex_) == cudaSuccess;
    if (ready_) {
        cudaEventSynchronize(ready_);
        cudaEventDestroy(ready_);
        ready_ = nullptr;
    }
    staging_ = DeviceBuffer<float>{};
    error_ = DeviceBuffer<int>{};
    if (selected && previous >= 0 && previous != deviceIndex_)
        cudaSetDevice(previous);
}

StyleStatus CudaWidth::validateField(ScalarField field,
                                     DeviceCurveGeometryView geometry) const {
    if (!IsDomain(field.domain)) return StyleStatus::InvalidArgument;
    if (!field.data) {
        if (field.count == 0 &&
            ((field.domain == expr::Domain::Primitive && geometry.curveCount == 0) ||
             (field.domain == expr::Domain::Point && geometry.pointCount == 0)))
            return StyleStatus::Ok;
        if (field.domain != expr::Domain::Groom || field.count != 0)
            return StyleStatus::InvalidArgument;
        return std::isfinite(field.literal) ? StyleStatus::Ok
                                            : StyleStatus::NonFiniteInput;
    }
    size_t expected = field.domain == expr::Domain::Groom
        ? 1 : (field.domain == expr::Domain::Primitive
            ? geometry.curveCount : geometry.pointCount);
    if (field.count != expected ||
        field.count > size_t(std::numeric_limits<int>::max()))
        return StyleStatus::InvalidArgument;
    return StyleStatus::Ok;
}

StyleStatus CudaWidth::validateBoolField(BoolField field,
                                         DeviceCurveGeometryView geometry,
                                         bool groomOnly) const {
    if (!IsDomain(field.domain)) return StyleStatus::InvalidArgument;
    if (groomOnly && field.domain != expr::Domain::Groom)
        return StyleStatus::InvalidArgument;
    if (!field.data) {
        if (field.count == 0 &&
            ((field.domain == expr::Domain::Primitive && geometry.curveCount == 0) ||
             (field.domain == expr::Domain::Point && geometry.pointCount == 0)))
            return StyleStatus::Ok;
        if (field.domain != expr::Domain::Groom || field.count != 0)
            return StyleStatus::InvalidArgument;
        return StyleStatus::Ok;
    }
    size_t expected = field.domain == expr::Domain::Groom
        ? 1 : (field.domain == expr::Domain::Primitive
            ? geometry.curveCount : geometry.pointCount);
    return field.count == expected &&
               field.count <= size_t(std::numeric_limits<int>::max())
        ? StyleStatus::Ok : StyleStatus::InvalidArgument;
}

StyleStatus CudaWidth::begin(DeviceCurveGeometryView geometry,
                             DeviceView<float> output, cudaStream_t stream) {
    if (!ready_ && cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming) !=
                     cudaSuccess)
        return StyleStatus::CudaError;
    if (error_.reset(1) != cudaSuccess ||
        staging_.reset(geometry.pointCount) != cudaSuccess ||
        cudaMemsetAsync(error_.data(), 0, sizeof(int), stream) != cudaSuccess)
        return StyleStatus::CudaError;
    pointCount_ = geometry.pointCount;
    output_ = output;
    return StyleStatus::Ok;
}

StyleStatus CudaWidth::Apply(DeviceCurveGeometryView geometry,
                             DeviceView<const float> hairT,
                             WidthParameters parameters,
                             DeviceView<float> output,
                             cudaStream_t stream) {
    if (pending_) return StyleStatus::InvalidArgument;
    int current = -1;
    if (cudaGetDevice(&current) != cudaSuccess) return StyleStatus::CudaError;
    int streamDevice = current;
    if (stream && cudaStreamGetDevice(stream, &streamDevice) != cudaSuccess)
        return StyleStatus::CudaError;
    if (streamDevice != current ||
        (deviceIndex_ >= 0 && deviceIndex_ != current))
        return StyleStatus::InvalidArgument;
    if (deviceIndex_ < 0) deviceIndex_ = current;

    if (geometry.curveCount > size_t(std::numeric_limits<int>::max()) ||
        geometry.pointCount > size_t(std::numeric_limits<int>::max()) ||
        geometry.pointCount > size_t(std::numeric_limits<uint32_t>::max()) ||
        (geometry.curveCount == 0 && geometry.pointCount != 0) ||
        geometry.curveCount == std::numeric_limits<size_t>::max() ||
        output.size != geometry.pointCount ||
        (geometry.pointCount && !output.data) ||
        (geometry.pointCount && (!geometry.widths.data ||
                                 geometry.widths.size != geometry.pointCount)) ||
        (!hairT.data && hairT.size != 0) ||
        (hairT.data && hairT.size != geometry.pointCount) ||
        (geometry.curveCount && (!geometry.curveOffsets.data ||
                                 geometry.curveOffsets.size !=
                                     geometry.curveCount + 1)))
        return StyleStatus::InvalidArgument;
    if (!geometry.pointCount && geometry.widths.size != 0)
        return StyleStatus::InvalidArgument;
    if (parameters.widthProfile.size != kProfileSize ||
        !parameters.widthProfile.data ||
        (parameters.maskProfile.data &&
         parameters.maskProfile.size != kProfileSize) ||
        (!parameters.maskProfile.data && parameters.maskProfile.size != 0))
        return StyleStatus::InvalidArgument;

    auto validateControl = [&](ScalarField field, float minimum,
                               float maximum) {
        StyleStatus result = validateField(field, geometry);
        if (result != StyleStatus::Ok || field.data) return result;
        return field.literal < minimum || field.literal > maximum
            ? StyleStatus::InvalidValue : StyleStatus::Ok;
    };
    StyleStatus status = validateControl(parameters.base, 0.0f,
                                         std::numeric_limits<float>::max());
    if (status != StyleStatus::Ok) return status;
    status = validateControl(parameters.width, 0.0f,
                             std::numeric_limits<float>::max());
    if (status != StyleStatus::Ok) return status;
    status = validateControl(parameters.rootScale, 0.0f,
                             std::numeric_limits<float>::max());
    if (status != StyleStatus::Ok) return status;
    status = validateControl(parameters.tipScale, 0.0f,
                             std::numeric_limits<float>::max());
    if (status != StyleStatus::Ok) return status;
    status = validateControl(parameters.taper, 0.0f, 1.0f);
    if (status != StyleStatus::Ok) return status;
    status = validateControl(parameters.taperStart, 0.0f, 1.0f);
    if (status != StyleStatus::Ok) return status;
    status = validateControl(parameters.blend, 0.0f, 1.0f);
    if (status != StyleStatus::Ok) return status;
    status = validateControl(parameters.maskAmount, 0.0f, 1.0f);
    if (status != StyleStatus::Ok) return status;
    status = validateBoolField(parameters.enabled, geometry, true);
    if (status != StyleStatus::Ok) return status;
    status = validateBoolField(parameters.replace, geometry, false);
    if (status != StyleStatus::Ok) return status;

    status = begin(geometry, output, stream);
    if (status != StyleStatus::Ok) return status;
    if (geometry.curveCount) {
        ValidateOffsets<<<(geometry.curveCount + 255) / 256, 256, 0, stream>>>(
            geometry.curveOffsets.data, geometry.curveCount,
            geometry.pointCount, error_.data());
        if (cudaGetLastError() != cudaSuccess) return StyleStatus::CudaError;
        ValidateProfile<<<2, 256, 0, stream>>>(parameters.widthProfile.data,
                                                parameters.maskProfile.data,
                                                error_.data());
        if (cudaGetLastError() != cudaSuccess) return StyleStatus::CudaError;
        WidthKernel<<<(geometry.curveCount + 255) / 256, 256, 0, stream>>>(
            geometry, hairT, parameters, staging_.data(), error_.data());
        if (cudaGetLastError() != cudaSuccess) return StyleStatus::CudaError;
    } else {
        ValidateProfile<<<2, 256, 0, stream>>>(parameters.widthProfile.data,
                                                parameters.maskProfile.data,
                                                error_.data());
        if (cudaGetLastError() != cudaSuccess) return StyleStatus::CudaError;
    }
    if (cudaEventRecord(ready_, stream) != cudaSuccess)
        return StyleStatus::CudaError;
    pending_ = true;
    return StyleStatus::Ok;
}

StyleStatus CudaWidth::finishPublication(cudaStream_t stream) {
    if (pointCount_ == 0) {
        if (cudaStreamSynchronize(stream) != cudaSuccess)
            return StyleStatus::CudaError;
    } else if (cudaMemcpyAsync(output_.data, staging_.data(),
                               pointCount_ * sizeof(float),
                               cudaMemcpyDeviceToDevice, stream) != cudaSuccess ||
               cudaStreamSynchronize(stream) != cudaSuccess) {
        return StyleStatus::CudaError;
    }
    pending_ = false;
    pointCount_ = 0;
    output_ = {};
    return StyleStatus::Ok;
}

StyleStatus CudaWidth::Finish(cudaStream_t stream) {
    if (!pending_) return StyleStatus::InvalidArgument;
    int current = -1;
    if (cudaGetDevice(&current) != cudaSuccess || current != deviceIndex_)
        return StyleStatus::InvalidArgument;
    int streamDevice = current;
    if (stream && cudaStreamGetDevice(stream, &streamDevice) != cudaSuccess)
        return StyleStatus::CudaError;
    if (streamDevice != deviceIndex_) return StyleStatus::InvalidArgument;
    int code = 0;
    if (cudaStreamWaitEvent(stream, ready_, 0) != cudaSuccess ||
        cudaMemcpyAsync(&code, error_.data(), sizeof(code),
                        cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
        cudaStreamSynchronize(stream) != cudaSuccess)
        return StyleStatus::CudaError;
    if (code != 0) {
        pending_ = false;
        pointCount_ = 0;
        output_ = {};
        return DecodeError(code);
    }
    return finishPublication(stream);
}

} // namespace usdGen::gpu
