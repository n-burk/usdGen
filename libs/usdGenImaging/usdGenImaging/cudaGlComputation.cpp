#include "usdGenImaging/cudaGlComputation.h"

#ifdef USDGEN_HAS_CUDA_GL_INTEROP

#include "usdGen/gpu/generation.h"
#include "usdGen/executionPipeline.h"

#include "pxr/imaging/hd/bufferSpec.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"
#include "pxr/imaging/hdSt/bufferResource.h"
#include "pxr/imaging/hgiGL/buffer.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/garch/glApi.h"

#include <cuda_gl_interop.h>

#include <limits>
#include <exception>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {
namespace {

bool
_Mul(size_t a, size_t b, size_t *result)
{
    if (a && b > std::numeric_limits<size_t>::max() / a) {
        return false;
    }
    *result = a * b;
    return true;
}

bool
_Add(size_t a, size_t b, size_t *result)
{
    if (b > std::numeric_limits<size_t>::max() - a) {
        return false;
    }
    *result = a + b;
    return true;
}

bool
_FitsInt(size_t value)
{
    return value <= static_cast<size_t>(std::numeric_limits<int>::max());
}

} // namespace

UsdGenCudaGlComputation::UsdGenCudaGlComputation(
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation,
    usdGen::UsdGenDeviceChannelSemantic semantic,
    TfToken destinationName)
    : _generation(std::move(generation))
    , _semantic(semantic)
    , _destinationName(std::move(destinationName))
{
}

void
UsdGenCudaGlComputation::_Fail(char const *where, int cudaError) noexcept
{
    std::ostringstream stream;
    stream << where << ": " << cudaGetErrorName(static_cast<cudaError_t>(cudaError))
           << " (" << cudaGetErrorString(static_cast<cudaError_t>(cudaError)) << ')';
    _Fail(stream.str());
}

void
UsdGenCudaGlComputation::_Fail(std::string message) noexcept
{
    _succeeded = false;
    _error = std::move(message);
}

bool
UsdGenCudaGlComputation::_Describe(
    HdTupleType *tuple, size_t *count, size_t *elementBytes) const noexcept
{
    if (_destinationName.IsEmpty() || !_generation ||
        _generation->Identity().backend != usdGen::UsdGenDeviceBackend::Cuda ||
        _generation->Identity().deviceIndex < 0) {
        return false;
    }

    using S = usdGen::UsdGenDeviceChannelSemantic;
    switch (_semantic) {
    case S::Points:
    case S::RestPoints:
        *tuple = HdTupleType{HdTypeFloatVec3, 1};
        *count = _generation->Geometry().pointCount;
        *elementBytes = sizeof(float) * 3;
        return true;
    case S::Widths:
    case S::HairT:
        *tuple = HdTupleType{HdTypeFloat, 1};
        *count = _generation->Geometry().pointCount;
        *elementBytes = sizeof(float);
        return true;
    case S::CurveOffsets:
        *tuple = HdTupleType{HdTypeUInt32, 1};
        if (_generation->Geometry().curveCount ==
            std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        *count = _generation->Geometry().curveCount + 1;
        *elementBytes = sizeof(uint32_t);
        return true;
    case S::StableIds:
        // HdTypeUInt32Vec2 preserves each uint64 id as its exact two words.
        *tuple = HdTupleType{HdTypeUInt32Vec2, 1};
        *count = _generation->Geometry().curveCount;
        *elementBytes = sizeof(uint64_t);
        return true;
    case S::RootPrim:
        *tuple = HdTupleType{HdTypeInt32, 1};
        *count = _generation->Geometry().curveCount;
        *elementBytes = sizeof(int32_t);
        return true;
    case S::RootUV:
        *tuple = HdTupleType{HdTypeFloatVec2, 1};
        *count = _generation->Geometry().curveCount;
        *elementBytes = sizeof(float) * 2;
        return true;
    default:
        return false;
    }
}

int
UsdGenCudaGlComputation::GetNumOutputElements() const
{
    HdTupleType tuple;
    size_t count = 0, elementBytes = 0;
    return _Describe(&tuple, &count, &elementBytes) && _FitsInt(count)
        ? static_cast<int>(count) : 0;
}

void
UsdGenCudaGlComputation::GetBufferSpecs(HdBufferSpecVector *specs) const
{
    if (!specs) {
        return;
    }
    HdTupleType tuple;
    size_t count = 0, elementBytes = 0;
    if (_Describe(&tuple, &count, &elementBytes) && _FitsInt(count)) {
        specs->emplace_back(_destinationName, tuple);
    }
}

void
UsdGenCudaGlComputation::Execute(
    HdBufferArrayRangeSharedPtr const &range,
    HdResourceRegistry *)
{
    _succeeded = false;
    _error.clear();
    if (usdGen::UsdGenExecutionPipeline::IsExecuting()) {
        _Fail("CUDA-GL transfer requires an external graphics commit thread");
        return;
    }

    HdTupleType expectedTuple;
    size_t count = 0, elementBytes = 0;
    if (!_Describe(&expectedTuple, &count, &elementBytes) || !_FitsInt(count)) {
        _Fail("invalid CUDA generation or unsupported channel");
        return;
    }
    using S = usdGen::UsdGenDeviceChannelSemantic;

    auto const destinationRange =
        std::dynamic_pointer_cast<HdStBufferArrayRange>(range);
    if (!destinationRange) {
        _Fail("destination is not an HdStBufferArrayRange");
        return;
    }
    HdStBufferResourceSharedPtr const destination =
        destinationRange->GetResource(_destinationName);
    if (!destination || destination->GetTupleType() != expectedTuple ||
        destinationRange->GetNumElements() != count) {
        _Fail("destination tuple or element count does not match CUDA channel");
        return;
    }
    usdGen::UsdGenDeviceValueType expectedValueType =
        usdGen::UsdGenDeviceValueType::Float32;
    uint32_t expectedArity = 1;
    using VT = usdGen::UsdGenDeviceValueType;
    switch (_semantic) {
    case S::Points:
    case S::RestPoints: expectedValueType = VT::Float32x3; expectedArity = 3; break;
    case S::RootUV: expectedValueType = VT::Float32x2; expectedArity = 2; break;
    case S::CurveOffsets: expectedValueType = VT::UInt32; break;
    case S::StableIds: expectedValueType = VT::UInt64; break;
    case S::RootPrim: expectedValueType = VT::Int32; break;
    case S::Widths:
    case S::HairT: break;
    default: _Fail("unsupported CUDA channel semantic"); return;
    }
    bool metadataMatches = false;
    for (usdGen::UsdGenDeviceChannelMetadata const &channel :
         _generation->Channels()) {
        if (channel.semantic == _semantic && channel.elementCount == count &&
            channel.type == expectedValueType && channel.arity == expectedArity &&
            channel.strideBytes == elementBytes) {
            metadataMatches = true;
            break;
        }
    }
    if (!metadataMatches) {
        _Fail("CUDA channel metadata does not match its required Storm layout");
        return;
    }
    if (!destination->GetHandle()) {
        _Fail("destination has no allocated Hgi buffer");
        return;
    }
    HgiGLBuffer *const glBuffer = dynamic_cast<HgiGLBuffer *>(
        destination->GetHandle().Get());
    if (!glBuffer || !glBuffer->GetBufferId()) {
        _Fail("destination is not an HgiGLBuffer");
        return;
    }
    if (destination->GetOffset() < 0 || destination->GetStride() <= 0 ||
        destinationRange->GetByteOffset(_destinationName) < 0) {
        _Fail("destination has a negative Storm byte offset or stride");
        return;
    }

    size_t destinationOffset = 0;
    if (!_Add(static_cast<size_t>(destinationRange->GetByteOffset(_destinationName)),
              static_cast<size_t>(destination->GetOffset()), &destinationOffset) ||
        static_cast<size_t>(destination->GetStride()) < elementBytes) {
        _Fail("destination Storm layout overflows or has an incompatible stride");
        return;
    }
    size_t lastOffset = destinationOffset;
    if (count > 0) {
        size_t span = 0;
        if (!_Mul(count - 1, static_cast<size_t>(destination->GetStride()), &span) ||
            !_Add(destinationOffset, span, &lastOffset) ||
            !_Add(lastOffset, elementBytes, &lastOffset) ||
            lastOffset > glBuffer->GetByteSizeOfResource()) {
            _Fail("destination Storm range exceeds its GL buffer");
            return;
        }
    }

    int glDevices[16] = {};
    unsigned int glDeviceCount = 0;
    cudaError_t status = cudaGLGetDevices(
        &glDeviceCount, glDevices, 16, cudaGLDeviceListAll);
    if (status != cudaSuccess) {
        _Fail("cudaGLGetDevices", status);
        return;
    }
    bool const glMatchesGeneration = [&] {
        for (unsigned int i = 0; i != glDeviceCount; ++i) {
            if (glDevices[i] == _generation->Identity().deviceIndex) return true;
        }
        return false;
    }();
    if (!glMatchesGeneration) {
        _Fail("current GL context is not associated with the generation CUDA device");
        return;
    }
    int previousDevice = -1;
    if ((status = cudaGetDevice(&previousDevice)) != cudaSuccess) {
        _Fail("cudaGetDevice", status);
        return;
    }
    if ((status = cudaSetDevice(_generation->Identity().deviceIndex)) != cudaSuccess) {
        _Fail("cudaSetDevice", status);
        return;
    }
    auto const restoreDevice = [&] {
        return previousDevice != _generation->Identity().deviceIndex
            ? cudaSetDevice(previousDevice) : cudaSuccess;
    };

    cudaStream_t stream = nullptr;
    if ((status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking)) != cudaSuccess) {
        restoreDevice();
        _Fail("cudaStreamCreateWithFlags", status);
        return;
    }

    // AcquireGeometry owns producer ordering and keeps its CUDA source alive.
    // It must be destroyed before this stream is destroyed (it fences stream).
    usdGen::gpu::CudaGeometryLease lease =
        usdGen::gpu::AcquireGeometry(_generation, stream);
    if (!lease) {
        cudaStreamDestroy(stream);
        restoreDevice();
        _Fail("CUDA generation rejected the consumer stream");
        return;
    }

    void const *source = nullptr;
    size_t sourceCount = 0;
    auto const geometry = lease.Geometry();
    switch (_semantic) {
    case S::Points: source = geometry.points.data; sourceCount = geometry.points.size; break;
    case S::RestPoints: source = geometry.restPoints.data; sourceCount = geometry.restPoints.size; break;
    case S::Widths: source = geometry.widths.data; sourceCount = geometry.widths.size; break;
    case S::HairT: { auto v = lease.HairT(); source = v.data; sourceCount = v.size; break; }
    case S::CurveOffsets: source = geometry.curveOffsets.data; sourceCount = geometry.curveOffsets.size; break;
    case S::StableIds: source = geometry.stableIds.data; sourceCount = geometry.stableIds.size; break;
    case S::RootPrim: { auto v = lease.RootPrim(); source = v.data; sourceCount = v.size; break; }
    case S::RootUV: { auto v = lease.RootUV(); source = v.data; sourceCount = v.size; break; }
    default: break;
    }
    if (sourceCount != count || (count && !source)) {
        lease = {};
        cudaStreamDestroy(stream);
        restoreDevice();
        _Fail("CUDA channel shape does not match immutable generation metadata");
        return;
    }

    cudaGraphicsResource_t graphics = nullptr;
    bool mapped = false;
    status = cudaGraphicsGLRegisterBuffer(
        &graphics, glBuffer->GetBufferId(), cudaGraphicsRegisterFlagsNone);
    if (status == cudaSuccess) {
        status = cudaGraphicsMapResources(1, &graphics, stream);
        mapped = status == cudaSuccess;
    }
    void *mappedDestination = nullptr;
    size_t mappedBytes = 0;
    if (status == cudaSuccess) {
        status = cudaGraphicsResourceGetMappedPointer(
            &mappedDestination, &mappedBytes, graphics);
    }
    if (status == cudaSuccess && mappedBytes < glBuffer->GetByteSizeOfResource()) {
        _Fail("mapped GL buffer is smaller than Storm's HgiGLBuffer allocation");
        status = cudaErrorInvalidValue;
    }
    if (status == cudaSuccess && count) {
        status = cudaMemcpy2DAsync(
            static_cast<unsigned char *>(mappedDestination) + destinationOffset,
            static_cast<size_t>(destination->GetStride()), source, elementBytes,
            elementBytes, count, cudaMemcpyDeviceToDevice, stream);
    }

    // Unmap is the CUDA-to-GL ownership hand-off.  Do not unregister if it
    // fails: CUDA's ownership is then unknown, so freeing the registration is
    // unsafe.  The process/context teardown is the recovery boundary.
    cudaError_t const workStatus = status;
    cudaError_t unmapStatus = cudaSuccess;
    if (mapped) {
        unmapStatus = cudaGraphicsUnmapResources(1, &graphics, stream);
    }
    if (unmapStatus != cudaSuccess) {
        _Fail("cudaGraphicsUnmapResources", unmapStatus);
        TF_FATAL_ERROR("usdGen CUDA-GL ownership hand-off failed: %s",
                       _error.c_str());
        std::terminate();
    }
    // cudaGraphicsUnmapResources is ordered on stream.  Check that stream
    // explicitly: CudaGeometryLease::Complete also fences it, but deliberately
    // cannot return its status to this computation.
    if ((status = cudaStreamSynchronize(stream)) != cudaSuccess) {
        _Fail("cudaStreamSynchronize after CUDA-GL unmap", status);
        TF_FATAL_ERROR("usdGen CUDA-GL ownership fence failed: %s",
                       _error.c_str());
        std::terminate();
    }
    // Completion fences the consumer stream, including the asynchronous
    // unmap, before the GL resource registration is released.
    lease = {};
    if (graphics) {
        cudaError_t const unregisterStatus = cudaGraphicsUnregisterResource(graphics);
        if (unregisterStatus != cudaSuccess) {
            _Fail("cudaGraphicsUnregisterResource", unregisterStatus);
            TF_FATAL_ERROR("usdGen CUDA-GL resource release failed: %s",
                           _error.c_str());
            std::terminate();
        }
    }

    // The source lease was completed while stream was valid; its owner
    // synchronized this stream before permitting storage reclamation.
    cudaError_t const destroyStatus = cudaStreamDestroy(stream);
    cudaError_t const restoreStatus = restoreDevice();
    if (workStatus != cudaSuccess) {
        _Fail("CUDA-to-GL copy", workStatus);
        return;
    }
    if (destroyStatus != cudaSuccess || restoreStatus != cudaSuccess) {
        _Fail("CUDA-GL stream/device cleanup", destroyStatus != cudaSuccess ? destroyStatus : restoreStatus);
        return;
    }
    _succeeded = true;
}

} // namespace usdGenImaging

#endif // USDGEN_HAS_CUDA_GL_INTEROP
