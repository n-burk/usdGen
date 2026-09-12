#include "usdGenImaging/cudaBasisCurvesProvider.h"

#if defined(USDGEN_HAS_CUDA_GL_INTEROP) && \
    __has_include("pxr/imaging/hdSt/basisCurvesGpuDataSource.h")

#include "usdGenImaging/cudaGlComputation.h"
#include "usdGen/gpu/curveIndices.h"
#include "usdGen/gpu/generation.h"
#include "usdGen/executionPipeline.h"

#include "pxr/imaging/hd/bufferSpec.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hdSt/computation.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"
#include "pxr/imaging/hdSt/bufferResource.h"
#include "pxr/imaging/hgiGL/buffer.h"
#include "pxr/imaging/hgi/capabilities.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <vector>

#include <cuda_gl_interop.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {
namespace {

bool
_Add(size_t a, size_t b, size_t *result)
{
    if (b > std::numeric_limits<size_t>::max() - a) return false;
    *result = a + b;
    return true;
}

bool
_Mul(size_t a, size_t b, size_t *result)
{
    if (a && b > std::numeric_limits<size_t>::max() / a) return false;
    *result = a * b;
    return true;
}

bool
_Fits(size_t offset, size_t bytes, size_t capacity)
{
    return offset <= capacity && bytes <= capacity - offset;
}

// This computation is intentionally the sole GPU-index producer.  Its full
// CUDA/GL map implementation is kept private to this adapter so HdSt remains
// independent of CUDA and usdGen.  The per-request State owns CUB scratch and
// scalar status/count storage for the life of the queued computation.
class _CurveTopologyComputation final : public HdStComputation {
public:
    _CurveTopologyComputation(
        std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation,
        usdGen::gpu::CurveIndexOptions options,
        usdGen::gpu::CurveIndexRequirements requirements,
        TfToken indicesName, TfToken primitiveName,
        HdBufferArrayRangeSharedPtr countRange, TfToken countName,
        std::optional<usdGen::UsdGenDeviceTileMetadata> tile)
        : _generation(std::move(generation)), _options(options),
          _requirements(requirements), _indicesName(std::move(indicesName)),
          _primitiveName(std::move(primitiveName)), _countRange(std::move(countRange)),
          _countName(std::move(countName)), _tile(std::move(tile)) {}

    int GetNumOutputElements() const override {
        return _requirements.maxRecords <= size_t(std::numeric_limits<int>::max())
            ? int(_requirements.maxRecords) : 0;
    }
    void GetBufferSpecs(HdBufferSpecVector *specs) const override {
        // Keep a zero-capacity topology BAR structurally valid.  Execute then
        // maps only the one-word draw-count BAR and publishes an explicit
        // zero, rather than retaining a previous nonempty indirect count.
        if (!specs) return;
        specs->emplace_back(_indicesName, HdTupleType{
            _requirements.indexArity == 4 ? HdTypeInt32Vec4 :
            _requirements.indexArity == 2 ? HdTypeInt32Vec2 : HdTypeInt32, 1});
        specs->emplace_back(_primitiveName, HdTupleType{HdTypeInt32, 1});
    }
    void Execute(HdBufferArrayRangeSharedPtr const &range, HdResourceRegistry *) override {
        _succeeded = false;
        _error.clear();
        if (usdGen::UsdGenExecutionPipeline::IsExecuting()) {
            _error = "CUDA-GL topology generation requires an external graphics commit thread";
            return;
        }
        auto const topo = std::dynamic_pointer_cast<HdStBufferArrayRange>(range);
        auto const count = std::dynamic_pointer_cast<HdStBufferArrayRange>(_countRange);
        if (!topo || !count || !_generation) { _error = "invalid topology/count BAR"; return; }
        auto const indices = topo->GetResource(_indicesName);
        auto const primitive = topo->GetResource(_primitiveName);
        auto const drawCount = count->GetResource(_countName);
        HdTupleType const expectedIndices{
            _requirements.indexArity == 4 ? HdTypeInt32Vec4 :
            _requirements.indexArity == 2 ? HdTypeInt32Vec2 : HdTypeInt32, 1};
        if (!indices || !primitive || !drawCount ||
            indices->GetTupleType() != expectedIndices ||
            primitive->GetTupleType() != HdTupleType{HdTypeInt32, 1} ||
            drawCount->GetTupleType() != HdTupleType{HdTypeUInt32, 1} ||
            topo->GetNumElements() != _requirements.maxRecords ||
            count->GetNumElements() != 1 ||
            indices->GetStride() != int(_requirements.indexArity * sizeof(int32_t)) ||
            primitive->GetStride() != int(sizeof(int32_t)) ||
            drawCount->GetStride() != int(sizeof(uint32_t)) ||
            indices->GetOffset() < 0 || primitive->GetOffset() < 0 ||
            drawCount->GetOffset() < 0 || topo->GetByteOffset(_indicesName) < 0 ||
            topo->GetByteOffset(_primitiveName) < 0 || count->GetByteOffset(_countName) < 0) {
            _error = "Storm topology tuple, packed stride, or aggregate range is incompatible";
            return;
        }

        int previousDevice = -1;
        cudaError_t status = cudaGetDevice(&previousDevice);
        if (status != cudaSuccess ||
            (status = cudaSetDevice(_generation->Identity().deviceIndex)) != cudaSuccess) {
            _error = "CUDA device selection failed";
            return;
        }
        auto const restoreDevice = [&] {
            return previousDevice != _generation->Identity().deviceIndex
                ? cudaSetDevice(previousDevice) : cudaSuccess;
        };
        int glDevices[16] = {};
        unsigned int glDeviceCount = 0;
        status = cudaGLGetDevices(&glDeviceCount, glDevices, 16, cudaGLDeviceListAll);
        bool const glMatches = status == cudaSuccess && std::any_of(
            glDevices, glDevices + glDeviceCount, [this](int device) {
                return device == _generation->Identity().deviceIndex;
            });
        if (!glMatches) {
            restoreDevice();
            _error = "current GL context is not associated with the generation CUDA device";
            return;
        }

        cudaStream_t stream = nullptr;
        if ((status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking)) != cudaSuccess) {
            restoreDevice(); _error = "CUDA stream creation failed"; return;
        }
        {
            // This scope is the external graphics boundary: workspace remains
            // alive until the lease has fenced the stream and GL owns buffers.
            usdGen::gpu::CudaGeometryLease geometryLease;
            usdGen::gpu::CudaGeometryTileLease tileLease;
            usdGen::gpu::DeviceView<const uint32_t> curveOffsets;
            size_t curveCount = 0, pointCount = 0;
            uint32_t pointBase = 0;
            if (_tile) {
                tileLease = usdGen::gpu::AcquireGeometryTile(_generation, _tile->tile,
                    _generation->Identity().generation, stream);
                if (!tileLease) {
                    cudaStreamDestroy(stream); restoreDevice();
                    _error = "CUDA generation rejected the selected tile consumer stream"; return;
                }
                auto const view = tileLease.View();
                auto const &range = view.range;
                if (view.generation != _generation->Identity().generation ||
                    range.tile != _tile->tile || range.firstCurve != _tile->firstCurve ||
                    range.curveCount != _tile->curveCount || range.firstPoint != _tile->firstPoint ||
                    range.pointCount != _tile->pointCount ||
                    range.firstPoint > uint64_t(UINT32_MAX) ||
                    range.pointCount > uint64_t(UINT32_MAX) - range.firstPoint ||
                    range.curveCount > std::numeric_limits<size_t>::max() ||
                    range.pointCount > std::numeric_limits<size_t>::max() ||
                    view.curveOffsets.size != size_t(range.curveCount) + 1) {
                    tileLease = {}; cudaStreamDestroy(stream); restoreDevice();
                    _error = "selected CUDA tile metadata or offsets changed after preparation"; return;
                }
                curveOffsets = view.curveOffsets;
                curveCount = size_t(range.curveCount);
                pointCount = size_t(range.pointCount);
                pointBase = static_cast<uint32_t>(range.firstPoint);
            } else {
                geometryLease = usdGen::gpu::AcquireGeometry(_generation, stream);
                if (!geometryLease) {
                    cudaStreamDestroy(stream); restoreDevice();
                    _error = "CUDA generation rejected the consumer stream"; return;
                }
                auto const geometry = geometryLease.Geometry();
                curveOffsets = geometry.curveOffsets;
                curveCount = geometry.curveCount;
                pointCount = geometry.pointCount;
            }
            usdGen::gpu::CurveIndexRequirements requirements;
            status = usdGen::gpu::GetCurveIndexRequirements(
                _options, curveCount, pointCount, &requirements, stream);
            if (status != cudaSuccess || requirements.maxRecords != _requirements.maxRecords ||
                requirements.indexArity != _requirements.indexArity ||
                requirements.scanBytes != _requirements.scanBytes ||
                requirements.deviceIndex != _generation->Identity().deviceIndex ||
                _requirements.deviceIndex != _generation->Identity().deviceIndex) {
                geometryLease = {}; tileLease = {}; cudaStreamDestroy(stream); restoreDevice();
                _error = "CUDA topology requirements do not match prepared Storm capacity"; return;
            }

            usdGen::gpu::DeviceBuffer<uint64_t> counts, offsets, recordCount;
            usdGen::gpu::DeviceBuffer<unsigned char> scratch;
            usdGen::gpu::DeviceBuffer<uint32_t> validation;
            if ((status = counts.reset(curveCount + 1)) != cudaSuccess ||
                (status = offsets.reset(curveCount + 1)) != cudaSuccess ||
                (status = scratch.reset(requirements.scanBytes)) != cudaSuccess ||
                (status = recordCount.reset(1)) != cudaSuccess ||
                (status = validation.reset(1)) != cudaSuccess) {
                geometryLease = {}; tileLease = {};
                counts.release(); offsets.release(); scratch.release();
                recordCount.release(); validation.release();
                cudaStreamDestroy(stream); restoreDevice();
                _error = "CUDA topology workspace allocation failed"; return;
            }

            struct Mapping { unsigned int id = 0; cudaGraphicsResource_t resource = nullptr;
                void *pointer = nullptr; size_t bytes = 0; };
            std::array<Mapping, 3> mappings{};
            size_t mappingCount = 0;
            auto addMapping = [&mappings, &mappingCount](HgiGLBuffer *buffer, size_t *index) {
                for (size_t i = 0; i != mappingCount; ++i) {
                    if (mappings[i].id == buffer->GetBufferId()) { *index = i; return true; }
                }
                if (!buffer->GetBufferId() || mappingCount == mappings.size()) return false;
                *index = mappingCount;
                mappings[mappingCount++] = {buffer->GetBufferId(), nullptr, nullptr, 0};
                return true;
            };
            auto field = [](HdStBufferArrayRange const &bar, TfToken const &name,
                            HdStBufferResource const &resource, size_t *offset) {
                return _Add(size_t(bar.GetByteOffset(name)), size_t(resource.GetOffset()), offset);
            };
            size_t indicesOffset = 0, primitiveOffset = 0, countOffset = 0;
            size_t indicesBytes = 0, primitiveBytes = 0;
            HgiGLBuffer *indicesBuffer = indices->GetHandle() ? dynamic_cast<HgiGLBuffer *>(indices->GetHandle().Get()) : nullptr;
            HgiGLBuffer *primitiveBuffer = primitive->GetHandle() ? dynamic_cast<HgiGLBuffer *>(primitive->GetHandle().Get()) : nullptr;
            HgiGLBuffer *countBuffer = drawCount->GetHandle() ? dynamic_cast<HgiGLBuffer *>(drawCount->GetHandle().Get()) : nullptr;
            size_t indicesMapping = 0, primitiveMapping = 0, countMapping = 0;
            bool const empty = requirements.maxRecords == 0;
            bool layoutOK = countBuffer && field(*count, _countName, *drawCount, &countOffset) &&
                countOffset % alignof(uint32_t) == 0 &&
                _Fits(countOffset, sizeof(uint32_t), countBuffer->GetByteSizeOfResource()) &&
                addMapping(countBuffer, &countMapping);
            if (!empty) {
                layoutOK = layoutOK && indicesBuffer && primitiveBuffer &&
                    field(*topo, _indicesName, *indices, &indicesOffset) &&
                    field(*topo, _primitiveName, *primitive, &primitiveOffset) &&
                    _Mul(requirements.maxRecords, size_t(indices->GetStride()), &indicesBytes) &&
                    _Mul(requirements.maxRecords, size_t(primitive->GetStride()), &primitiveBytes) &&
                    indicesOffset % alignof(int32_t) == 0 && primitiveOffset % alignof(int32_t) == 0 &&
                    _Fits(indicesOffset, indicesBytes, indicesBuffer->GetByteSizeOfResource()) &&
                    _Fits(primitiveOffset, primitiveBytes, primitiveBuffer->GetByteSizeOfResource()) &&
                    addMapping(indicesBuffer, &indicesMapping) && addMapping(primitiveBuffer, &primitiveMapping);
            }
            if (!layoutOK) {
                geometryLease = {}; tileLease = {};
                counts.release(); offsets.release(); scratch.release();
                recordCount.release(); validation.release();
                cudaStreamDestroy(stream); restoreDevice();
                _error = "Storm topology field offsets or GL backing capacity are invalid"; return;
            }

            std::array<cudaGraphicsResource_t, 3> resources{};
            size_t resourceCount = 0;
            for (size_t i = 0; i != mappingCount; ++i) {
                Mapping &mapping = mappings[i];
                if (status == cudaSuccess) {
                    status = cudaGraphicsGLRegisterBuffer(
                        &mapping.resource, mapping.id, cudaGraphicsRegisterFlagsNone);
                    if (status == cudaSuccess) resources[resourceCount++] = mapping.resource;
                }
            }
            bool mapped = false;
            if (status == cudaSuccess) {
                status = cudaGraphicsMapResources(unsigned(resourceCount), resources.data(), stream);
                mapped = status == cudaSuccess;
            }
            if (status == cudaSuccess) for (size_t i = 0; i != mappingCount; ++i) {
                Mapping &mapping = mappings[i];
                status = cudaGraphicsResourceGetMappedPointer(&mapping.pointer, &mapping.bytes, mapping.resource);
                if (status != cudaSuccess) break;
            }
            auto mappedFits = [&mappings](size_t mapping, size_t offset, size_t bytes) {
                return mappings[mapping].pointer && _Fits(offset, bytes, mappings[mapping].bytes);
            };
            if (status == cudaSuccess && (!mappedFits(countMapping, countOffset, sizeof(uint32_t)) ||
                (!empty && (!mappedFits(indicesMapping, indicesOffset, indicesBytes) ||
                            !mappedFits(primitiveMapping, primitiveOffset, primitiveBytes))))) {
                status = cudaErrorInvalidValue;
            }
            if (status == cudaSuccess) {
                auto bytePointer = [&mappings](size_t mapping, size_t offset) {
                    return static_cast<unsigned char *>(mappings[mapping].pointer) + offset;
                };
                usdGen::gpu::CurveIndexOutput output{
                    empty ? nullptr : reinterpret_cast<int32_t *>(bytePointer(indicesMapping, indicesOffset)),
                    empty ? 0 : requirements.maxRecords * requirements.indexArity,
                    empty ? nullptr : reinterpret_cast<int32_t *>(bytePointer(primitiveMapping, primitiveOffset)),
                    empty ? 0 : requirements.maxRecords, recordCount.view(), validation.view()};
                if (_tile) {
                    status = usdGen::gpu::BuildCurveIndices(_options,
                        usdGen::gpu::CurveIndexSpan{curveOffsets, curveCount,
                            pointCount, pointBase}, requirements,
                        {counts.view(), offsets.view(), scratch.view()}, output, stream);
                } else {
                    status = usdGen::gpu::BuildCurveIndices(_options, curveCount,
                        pointCount, curveOffsets, requirements,
                        {counts.view(), offsets.view(), scratch.view()}, output, stream);
                }
                if (status == cudaSuccess) status = usdGen::gpu::PackCurveDrawCount(
                    {recordCount.data(), recordCount.size()},
                    {validation.data(), validation.size()}, requirements.indexArity,
                    requirements.maxRecords,
                    {reinterpret_cast<uint32_t *>(bytePointer(countMapping, countOffset)), 1}, stream);
            }

            cudaError_t const workStatus = status;
            cudaError_t unmapStatus = cudaSuccess;
            if (mapped) {
                unmapStatus = cudaGraphicsUnmapResources(
                    unsigned(resourceCount), resources.data(), stream);
            }
            // A failed unmap leaves CUDA/GL ownership unknown.  This is the
            // same unrecoverable boundary as the channel bridge: do not try
            // to unregister or free storage still potentially owned by CUDA.
            if (unmapStatus != cudaSuccess) {
                _error = "cudaGraphicsUnmapResources failed for CUDA topology output";
                TF_FATAL_ERROR("usdGen CUDA-GL ownership hand-off failed: %s", _error.c_str());
                std::terminate();
            }
            cudaError_t const fenceStatus = cudaStreamSynchronize(stream);
            if (fenceStatus != cudaSuccess) {
                _error = "CUDA topology stream fence after GL unmap failed";
                TF_FATAL_ERROR("usdGen CUDA-GL ownership fence failed: %s", _error.c_str());
                std::terminate();
            }
            // Complete fences the external producer before its storage can be
            // reclaimed; workspace is still alive on this selected device.
            geometryLease = {};
            tileLease = {};
            for (size_t i = 0; i != resourceCount; ++i) {
                if (cudaGraphicsUnregisterResource(resources[i]) != cudaSuccess) {
                    _error = "cudaGraphicsUnregisterResource failed for CUDA topology output";
                    TF_FATAL_ERROR("usdGen CUDA-GL resource release failed: %s", _error.c_str());
                    std::terminate();
                }
            }
            if (workStatus != cudaSuccess) {
                _error = "CUDA-GL topology generation failed";
            } else {
                _succeeded = true;
            }
        }
        cudaError_t const destroyStatus = cudaStreamDestroy(stream);
        cudaError_t const restoreStatus = restoreDevice();
        if (_succeeded && (destroyStatus != cudaSuccess || restoreStatus != cudaSuccess)) {
            _succeeded = false;
            _error = "CUDA topology stream/device cleanup failed";
        }
    }
    bool Succeeded() const { return _succeeded; }
    std::string const &Error() const { return _error; }
private:
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> _generation;
    usdGen::gpu::CurveIndexOptions _options;
    usdGen::gpu::CurveIndexRequirements _requirements;
    TfToken _indicesName, _primitiveName;
    HdBufferArrayRangeSharedPtr _countRange;
    TfToken _countName;
    std::optional<usdGen::UsdGenDeviceTileMetadata> _tile;
    bool _succeeded = false;
    std::string _error;
};

bool _Basis(TfToken const &curveType, TfToken const &basis,
    usdGen::gpu::CurveIndexBasis *result) {
    if (curveType == HdTokens->linear) {
        *result = usdGen::gpu::CurveIndexBasis::Linear;
        return true;
    }
    if (curveType != HdTokens->cubic) return false;
    if (basis == HdTokens->bezier) { *result = usdGen::gpu::CurveIndexBasis::Bezier; return true; }
    if (basis == HdTokens->catmullRom) { *result = usdGen::gpu::CurveIndexBasis::CatmullRom; return true; }
    if (basis == HdTokens->centripetalCatmullRom) { *result = usdGen::gpu::CurveIndexBasis::CentripetalCatmullRom; return true; }
    if (basis == HdTokens->bspline) { *result = usdGen::gpu::CurveIndexBasis::BSpline; return true; }
    return false;
}
bool _Wrap(TfToken const &wrap, usdGen::gpu::CurveIndexWrap *result) {
    if (wrap == HdTokens->periodic) { *result = usdGen::gpu::CurveIndexWrap::Periodic; return true; }
    if (wrap == HdTokens->pinned) { *result = usdGen::gpu::CurveIndexWrap::Pinned; return true; }
    if (wrap == HdTokens->segmented) { *result = usdGen::gpu::CurveIndexWrap::Segmented; return true; }
    if (wrap == HdTokens->nonperiodic) { *result = usdGen::gpu::CurveIndexWrap::Nonperiodic; return true; }
    return false;
}
usdGen::gpu::CurveIndexMode _Mode(HdStBasisCurvesGpuTopologyMode m) {
    return m == HdStBasisCurvesGpuTopologyMode::Hull ? usdGen::gpu::CurveIndexMode::Hull :
        m == HdStBasisCurvesGpuTopologyMode::Points ? usdGen::gpu::CurveIndexMode::Points :
        usdGen::gpu::CurveIndexMode::Curves;
}
}

UsdGenCudaBasisCurvesProvider::UsdGenCudaBasisCurvesProvider(CreateInfo info)
    : _info(std::move(info)) {}

HdStBasisCurvesGpuBundleSharedPtr
UsdGenCudaBasisCurvesProvider::Prepare(HdStResourceRegistry *registry,
    HdStBasisCurvesGpuPrepareRequest const &request)
{
    if (!registry || !_info.generation ||
        _info.generation->Identity().backend != usdGen::UsdGenDeviceBackend::Cuda ||
        registry->GetHgi()->GetAPIName() != HgiTokens->OpenGL ||
        !registry->GetHgi()->GetCapabilities()->IsSet(HgiDeviceCapabilitiesBitsMultiDrawIndirect)) return {};

    auto const &geometry = _info.generation->Geometry();
    std::optional<usdGen::UsdGenDeviceTileMetadata> selectedTile;
    if (_info.tileId) {
        auto const found = std::find_if(geometry.tiles.begin(), geometry.tiles.end(),
            [this](usdGen::UsdGenDeviceTileMetadata const &tile) {
                return tile.tile == *_info.tileId;
            });
        if (found == geometry.tiles.end()) return {};
        if (found->firstCurve > geometry.curveCount ||
            found->curveCount > geometry.curveCount - found->firstCurve ||
            found->firstPoint > geometry.pointCount ||
            found->pointCount > geometry.pointCount - found->firstPoint ||
            found->firstPoint > uint64_t(UINT32_MAX) ||
            found->pointCount > uint64_t(UINT32_MAX) - found->firstPoint ||
            found->curveCount > std::numeric_limits<size_t>::max() ||
            found->pointCount > std::numeric_limits<size_t>::max()) return {};
        selectedTile = *found;
    }
    size_t const curveCount = selectedTile ? size_t(selectedTile->curveCount)
                                           : size_t(geometry.curveCount);
    size_t const pointCount = selectedTile ? size_t(selectedTile->pointCount)
                                           : size_t(geometry.pointCount);

    auto result = std::make_shared<HdStBasisCurvesGpuBundle>();
    result->generation = _info.generation->Identity().generation;
    result->curveType = _info.curveType;
    result->curveBasis = _info.curveBasis;
    result->curveWrap = _info.curveWrap;
    result->bounds = _info.conservativeBounds;
    result->basisWidthInterpolation = _info.basisWidthInterpolation;
    result->basisNormalInterpolation = _info.basisNormalInterpolation;

    HdBufferSourceSharedPtrVector vertexSources;
    auto points = std::make_shared<UsdGenCudaGlComputation>(_info.generation,
        usdGen::UsdGenDeviceChannelSemantic::Points, HdTokens->points, _info.tileId);
    auto widths = std::make_shared<UsdGenCudaGlComputation>(_info.generation,
        usdGen::UsdGenDeviceChannelSemantic::Widths, HdTokens->widths, _info.tileId);
    auto hairT = std::make_shared<UsdGenCudaGlComputation>(_info.generation,
        usdGen::UsdGenDeviceChannelSemantic::HairT, TfToken("hairT"), _info.tileId);
    HdBufferSpecVector vertexSpecs;
    points->GetBufferSpecs(&vertexSpecs); widths->GetBufferSpecs(&vertexSpecs); hairT->GetBufferSpecs(&vertexSpecs);
    if (vertexSpecs.empty()) return {};
    result->vertexRange = registry->AllocateNonUniformBufferArrayRange(HdTokens->primvar,
        vertexSpecs, HdBufferArrayUsageHintBitsVertex);
    if (!result->vertexRange) return {};
    registry->AddComputation(result->vertexRange, points, HdStComputeQueueZero);
    registry->AddComputation(result->vertexRange, widths, HdStComputeQueueZero);
    registry->AddComputation(result->vertexRange, hairT, HdStComputeQueueZero);

    std::vector<std::shared_ptr<_CurveTopologyComputation>> topology;
    for (HdStBasisCurvesGpuTopologyMode mode : request.topologyModes) {
        usdGen::gpu::CurveIndexBasis basis;
        usdGen::gpu::CurveIndexWrap wrap;
        if (!_Basis(_info.curveType, _info.curveBasis, &basis) ||
            !_Wrap(_info.curveWrap, &wrap)) return {};
        usdGen::gpu::CurveIndexOptions options{basis, wrap, _Mode(mode)};
        usdGen::gpu::CurveIndexRequirements requirements;
        int previousDevice = -1;
        if (cudaGetDevice(&previousDevice) != cudaSuccess ||
            cudaSetDevice(_info.generation->Identity().deviceIndex) != cudaSuccess) return {};
        cudaError_t const requirementStatus = usdGen::gpu::GetCurveIndexRequirements(options,
            curveCount, pointCount, &requirements);
        cudaError_t const restoreStatus = previousDevice != _info.generation->Identity().deviceIndex
            ? cudaSetDevice(previousDevice) : cudaSuccess;
        if (requirementStatus != cudaSuccess || restoreStatus != cudaSuccess ||
            requirements.deviceIndex != _info.generation->Identity().deviceIndex) return {};
        HdBufferSpecVector countSpecs;
        countSpecs.emplace_back(TfToken("drawCount"), HdTupleType{HdTypeUInt32, 1});
        HdBufferArrayRangeSharedPtr const countRange =
            registry->AllocateNonUniformBufferArrayRange(HdTokens->primvar,
                countSpecs, HdBufferArrayUsageHintBitsStorage);
        if (!countRange) return {};
        countRange->Resize(1);
        auto comp = std::make_shared<_CurveTopologyComputation>(_info.generation, options, requirements,
            HdTokens->indices, HdTokens->primitiveParam, countRange, TfToken("drawCount"),
            selectedTile);
        HdBufferSpecVector specs; comp->GetBufferSpecs(&specs); if (specs.empty()) return {};
        HdStBasisCurvesGpuTopologyRange range; range.mode = mode;
        range.topologyRange = registry->AllocateNonUniformBufferArrayRange(HdTokens->topology, specs,
            HdBufferArrayUsageHintBitsIndex | HdBufferArrayUsageHintBitsStorage);
        if (!range.topologyRange) return {};
        range.drawCountRange = countRange;
        registry->AddComputation(range.topologyRange, comp, HdStComputeQueueZero);
        result->topologyRanges.push_back(range); topology.push_back(comp);
    }
    result->ready = [points, widths, hairT, topology]() {
        bool ready = points->Succeeded() && widths->Succeeded() && hairT->Succeeded();
        if (!points->Succeeded()) TF_WARN("usdGen CUDA BasisCurves points transfer failed: %s", points->Error().c_str());
        if (!widths->Succeeded()) TF_WARN("usdGen CUDA BasisCurves widths transfer failed: %s", widths->Error().c_str());
        if (!hairT->Succeeded()) TF_WARN("usdGen CUDA BasisCurves hairT transfer failed: %s", hairT->Error().c_str());
        for (auto const &computation : topology) {
            if (!computation->Succeeded()) {
                TF_WARN("usdGen CUDA BasisCurves topology transfer failed: %s", computation->Error().c_str());
                ready = false;
            }
        }
        return ready;
    };
    return result;
}
} // namespace usdGenImaging
#endif
