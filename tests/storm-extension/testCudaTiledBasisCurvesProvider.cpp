// Native Storm per-tile CUDA BasisCurves provider oracle.
#include "usdGenImaging/cudaBasisCurvesProvider.h"
#include "eglctx.h"
#include "usdGen/session.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdSt/basisCurvesGpuDataSource.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"
#include "pxr/imaging/hdSt/bufferResource.h"
#include "pxr/imaging/hdSt/renderDelegate.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgiGL/buffer.h"
#include "pxr/imaging/hgiGL/hgi.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using usdGenImaging::UsdGenCudaBasisCurvesProvider;

#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (false)

namespace {

struct Fields {
    HgiGLBuffer *pointsBuffer = nullptr;
    HgiGLBuffer *widthsBuffer = nullptr;
    HgiGLBuffer *hairTBuffer = nullptr;
    HgiGLBuffer *indicesBuffer = nullptr;
    HgiGLBuffer *primitiveBuffer = nullptr;
    HgiGLBuffer *countBuffer = nullptr;
    size_t pointsOffset = 0;
    size_t widthsOffset = 0;
    size_t hairTOffset = 0;
    size_t indicesOffset = 0;
    size_t primitiveOffset = 0;
    size_t countOffset = 0;
    size_t pointsStride = 0;
    size_t widthsStride = 0;
    size_t hairTStride = 0;
    size_t indicesStride = 0;
    size_t primitiveStride = 0;
    size_t countStride = 0;
};

size_t Offset(HdStBufferArrayRange const &range, TfToken const &name,
              HdStBufferResource const &resource)
{
    return size_t(range.GetByteOffset(name)) + size_t(resource.GetOffset());
}

bool GetBuffer(HdStBufferArrayRange &range, TfToken const &name,
               HgiGLBuffer **buffer, size_t *offset, size_t *stride)
{
    auto resource = range.GetResource(name);
    if (!resource || !resource->GetHandle() || resource->GetOffset() < 0 ||
        resource->GetStride() <= 0 || range.GetByteOffset(name) < 0) return false;
    auto *gl = dynamic_cast<HgiGLBuffer *>(resource->GetHandle().Get());
    if (!gl) return false;
    *buffer = gl;
    *offset = Offset(range, name, *resource);
    *stride = size_t(resource->GetStride());
    return true;
}

bool GetFields(HdStBasisCurvesGpuBundle const &bundle, Fields *out)
{
    auto vertex = std::dynamic_pointer_cast<HdStBufferArrayRange>(bundle.vertexRange);
    auto const *slot = bundle.FindTopologyRange(HdStBasisCurvesGpuTopologyMode::Curves);
    auto topology = slot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(slot->topologyRange) : nullptr;
    auto count = slot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(slot->drawCountRange) : nullptr;
    if (!vertex || !topology || !count ||
        !GetBuffer(*vertex, HdTokens->points, &out->pointsBuffer, &out->pointsOffset,
                   &out->pointsStride) ||
        !GetBuffer(*vertex, HdTokens->widths, &out->widthsBuffer, &out->widthsOffset,
                   &out->widthsStride) ||
        !GetBuffer(*vertex, TfToken("hairT"), &out->hairTBuffer, &out->hairTOffset,
                   &out->hairTStride) ||
        !GetBuffer(*topology, HdTokens->indices, &out->indicesBuffer, &out->indicesOffset,
                   &out->indicesStride) ||
        !GetBuffer(*topology, HdTokens->primitiveParam, &out->primitiveBuffer,
                   &out->primitiveOffset, &out->primitiveStride) ||
        !GetBuffer(*count, TfToken("drawCount"), &out->countBuffer, &out->countOffset,
                   &out->countStride)) return false;
    // drawCount is scalar and always tightly packed by the provider contract.
    return true;
}

template <class T>
T Read(HgiGLBuffer *buffer, size_t offset)
{
    T result{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer->GetBufferId());
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(offset),
                       GLsizeiptr(sizeof(T)), &result);
    return result;
}

bool Disjoint(size_t a, size_t aBytes, size_t b, size_t bBytes)
{
    return a + aBytes <= b || b + bBytes <= a;
}

bool SamePoint(std::array<float, 3> const &actual, float x, float y)
{
    return std::fabs(actual[0] - x) < 1e-5f &&
        std::fabs(actual[1] - y) < 1e-5f && std::fabs(actual[2]) < 1e-6f;
}

std::shared_ptr<const usdGen::UsdGenDeviceGeneration> MakeTiledGeneration()
{
    constexpr size_t kCurves = 1600;
    usdGen::UsdGenGraphDesc desc;
    desc.description = SdfPath("/Tiled/Description");
    desc.executionBackend = usdGen::UsdGenExecutionBackend::Cuda;
    desc.defaultWidth = .1f;
    desc.tileTarget = 64;

    usdGen::UsdGenSurfaceDesc scalp;
    scalp.path = SdfPath("/Tiled/Scalp");
    scalp.worldMatrix.SetIdentity();
    scalp.restPoints = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    scalp.points = scalp.restPoints;
    scalp.faceVertexCounts = {3};
    scalp.faceVertexIndices = {0, 1, 2};
    desc.surfaces.push_back(scalp);

    usdGen::UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/Tiled/Hair");
    curves.role = usdGen::UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.type = TfToken("cubic");
    curves.basis = TfToken("bspline");
    curves.wrap = TfToken("pinned");
    curves.curveVertexCounts.assign(kCurves, 2);
    curves.curveId.resize(kCurves);
    curves.skinPrim.assign(kCurves, 0);
    curves.skinPrimUv.assign(kCurves, GfVec2f(.25f, .25f));
    curves.points.resize(kCurves * 2);
    for (size_t curve = 0; curve != kCurves; ++curve) {
        float const yLength = curve < 1536 ? 2.0f : .2f;
        float const x = float(curve) * .001f;
        curves.curveId[curve] = curve;
        curves.points[curve * 2] = GfVec3f(x, 0, 0);
        curves.points[curve * 2 + 1] = GfVec3f(x, yLength, 0);
    }
    curves.rest = curves.points;
    desc.curveSets.push_back(curves);

    usdGen::UsdGenNodeDesc source;
    source.path = SdfPath("/Tiled/Source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.surfaces = {scalp.path};
    usdGen::UsdGenNodeDesc length;
    length.path = SdfPath("/Tiled/Length");
    length.type = TfToken("UsdGenLength");
    length.inputs = {source.path};
    length.params = {{TfToken("length:mode"), VtValue(TfToken("cull")), false},
                     {TfToken("cullThreshold"), VtValue(1.0f), false}};
    usdGen::UsdGenNodeDesc width;
    width.path = SdfPath("/Tiled/Width");
    width.type = TfToken("UsdGenWidth");
    width.inputs = {length.path};
    width.params = {{TfToken("width"), VtValue(.1f), false}};
    desc.nodes = {source, length, width};
    desc.terminal = width.path;

    usdGen::UsdGenSession session;
    session.SetDevicePublicationEnabled(true);
    session.SetGraphDesc(desc);
    auto generation = session.Commit(0, usdGen::UsdGenCommitReason::SetTime);
    if (!generation || !generation->device || session.LastDiagnostics().HasErrors()) {
        for (auto const &error : session.LastDiagnostics().errors)
            std::fprintf(stderr, "tiled CUDA fixture: %s\n", error.c_str());
        return {};
    }
    return generation->device;
}

std::shared_ptr<UsdGenCudaBasisCurvesProvider> MakeProvider(
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation,
    std::optional<uint32_t> tileId)
{
    UsdGenCudaBasisCurvesProvider::CreateInfo info;
    info.generation = std::move(generation);
    info.tileId = tileId;
    info.curveType = HdTokens->linear;
    info.curveBasis = HdTokens->linear;
    info.curveWrap = HdTokens->nonperiodic;
    info.conservativeBounds = GfBBox3d(GfRange3d(
        GfVec3d(-.1, -.1, -.1), GfVec3d(1.7, 3.1, .1)));
    return std::make_shared<UsdGenCudaBasisCurvesProvider>(std::move(info));
}

bool CheckTile(Fields const &fields, float rootX, float tipX)
{
    constexpr size_t kPoints = 1024;
    constexpr size_t kRecords = 512;
    if (fields.pointsStride != sizeof(float) * 3 || fields.widthsStride != sizeof(float) ||
        fields.hairTStride != sizeof(float) || fields.indicesStride != sizeof(int32_t) * 2 ||
        fields.primitiveStride != sizeof(int32_t) || fields.countStride != sizeof(uint32_t)) return false;
    auto const firstPoint = Read<std::array<float, 3>>(fields.pointsBuffer, fields.pointsOffset);
    auto const lastPoint = Read<std::array<float, 3>>(fields.pointsBuffer,
        fields.pointsOffset + (kPoints - 1) * fields.pointsStride);
    auto const firstWidth = Read<float>(fields.widthsBuffer, fields.widthsOffset);
    auto const lastWidth = Read<float>(fields.widthsBuffer,
        fields.widthsOffset + (kPoints - 1) * fields.widthsStride);
    auto const firstT = Read<float>(fields.hairTBuffer, fields.hairTOffset);
    auto const lastT = Read<float>(fields.hairTBuffer,
        fields.hairTOffset + (kPoints - 1) * fields.hairTStride);
    auto const firstPair = Read<std::array<int32_t, 2>>(fields.indicesBuffer, fields.indicesOffset);
    auto const lastPair = Read<std::array<int32_t, 2>>(fields.indicesBuffer,
        fields.indicesOffset + (kRecords - 1) * fields.indicesStride);
    auto const firstPrimitive = Read<int32_t>(fields.primitiveBuffer, fields.primitiveOffset);
    auto const lastPrimitive = Read<int32_t>(fields.primitiveBuffer,
        fields.primitiveOffset + (kRecords - 1) * fields.primitiveStride);
    auto const drawCount = Read<uint32_t>(fields.countBuffer, fields.countOffset);
    return SamePoint(firstPoint, rootX, 0) && SamePoint(lastPoint, tipX, 2) &&
        std::fabs(firstWidth - .1f) < 1e-6f && std::fabs(lastWidth - .1f) < 1e-6f &&
        std::fabs(firstT) < 1e-6f && std::fabs(lastT - 1.0f) < 1e-6f &&
        firstPair == std::array<int32_t, 2>{0, 1} &&
        lastPair == std::array<int32_t, 2>{1022, 1023} &&
        firstPrimitive == 0 && lastPrimitive == 511 && drawCount == 1024;
}

} // namespace

int main()
{
    TfErrorMark errors;
    if (!eglctx::MakeHeadlessGLContext()) return 77;
    GarchGLApiLoad();
    HgiGL hgi;
    HdStRenderDelegate delegate;
    HdDriver driver{HgiTokens->renderDriver, VtValue(static_cast<Hgi *>(&hgi))};
    delegate.SetDrivers({&driver});
    auto registry = std::dynamic_pointer_cast<HdStResourceRegistry>(delegate.GetResourceRegistry());
    CHECK(registry);

    auto generation = MakeTiledGeneration();
    CHECK(generation && generation->Identity().generation == 0);
    auto const &tiles = generation->Geometry().tiles;
    CHECK(tiles.size() == 4 && tiles[0].tile == 0 && tiles[0].firstCurve == 0 &&
          tiles[0].curveCount == 512 && tiles[0].firstPoint == 0 && tiles[0].pointCount == 1024 &&
          tiles[1].tile == 1 && tiles[1].firstCurve == 512 && tiles[1].curveCount == 512 &&
          tiles[1].firstPoint == 1024 && tiles[1].pointCount == 1024 &&
          tiles[2].tile == 2 && tiles[2].firstCurve == 1024 && tiles[2].curveCount == 512 &&
          tiles[2].firstPoint == 2048 && tiles[2].pointCount == 1024 &&
          tiles[3].tile == 3 && tiles[3].firstCurve == 1536 && tiles[3].curveCount == 0 &&
          tiles[3].firstPoint == 3072 && tiles[3].pointCount == 0);

    auto tileOne = MakeProvider(generation, 1);
    auto tileTwo = MakeProvider(generation, 2);
    auto tileZero = MakeProvider(generation, 0);
    auto emptyTile = MakeProvider(generation, 3);
    auto wholeGeneration = MakeProvider(generation, std::nullopt);
    auto unknownTile = MakeProvider(generation, 99);
    CHECK(tileOne && tileTwo && tileZero && emptyTile && wholeGeneration && unknownTile);
    HdStBasisCurvesGpuPrepareRequest request;
    request.topologyModes = {HdStBasisCurvesGpuTopologyMode::Curves};

    // UsdGenDeviceGeneration::Create validates individual tile bounds but
    // deliberately leaves complete-partition admission to consumers.  Clone
    // the real immutable backend payload and prove the provider rejects a
    // gapped/out-of-order partition before this registry is committed.
    usdGen::UsdGenDeviceGeneration::CreateInfo malformedInfo;
    malformedInfo.identity = generation->Identity();
    malformedInfo.geometry = generation->Geometry();
    malformedInfo.geometry.tiles = {{0, 1, 511, 2, 1022}, {1, 0, 1, 0, 2}};
    malformedInfo.tool = generation->Tool();
    malformedInfo.channels = generation->Channels();
    malformedInfo.owner = generation->Owner();
    std::string malformedReason;
    auto malformedGeneration = usdGen::UsdGenDeviceGeneration::Create(
        std::move(malformedInfo), &malformedReason);
    CHECK(malformedGeneration);
    auto malformedProvider = MakeProvider(malformedGeneration, 0);
    CHECK(malformedProvider && !malformedProvider->Prepare(registry.get(), request));

    auto oneBundle = tileOne->Prepare(registry.get(), request);
    auto twoBundle = tileTwo->Prepare(registry.get(), request);
    auto zeroBundle = tileZero->Prepare(registry.get(), request);
    auto emptyBundle = emptyTile->Prepare(registry.get(), request);
    auto wholeBundle = wholeGeneration->Prepare(registry.get(), request);
    CHECK(oneBundle && twoBundle && zeroBundle && emptyBundle && wholeBundle);
    CHECK(!unknownTile->Prepare(registry.get(), request));
    registry->Commit(); // One aggregate allocation/commit boundary for all tiles.
    CHECK(oneBundle->Ready() && twoBundle->Ready() && zeroBundle->Ready() &&
          emptyBundle->Ready() && wholeBundle->Ready());
    CHECK(oneBundle->generation == 0 && twoBundle->generation == 0 &&
          zeroBundle->generation == 0 && emptyBundle->generation == 0 &&
          wholeBundle->generation == 0);

    Fields one, two, zero, whole;
    CHECK(GetFields(*oneBundle, &one) && GetFields(*twoBundle, &two) &&
          GetFields(*zeroBundle, &zero) && GetFields(*wholeBundle, &whole));
    CHECK(one.pointsBuffer == two.pointsBuffer && one.widthsBuffer == two.widthsBuffer &&
          one.hairTBuffer == two.hairTBuffer && one.indicesBuffer == two.indicesBuffer &&
          one.primitiveBuffer == two.primitiveBuffer && one.countBuffer == two.countBuffer);
    CHECK(Disjoint(one.pointsOffset, 1024 * one.pointsStride, two.pointsOffset, 1024 * two.pointsStride) &&
          Disjoint(one.widthsOffset, 1024 * one.widthsStride, two.widthsOffset, 1024 * two.widthsStride) &&
          Disjoint(one.hairTOffset, 1024 * one.hairTStride, two.hairTOffset, 1024 * two.hairTStride) &&
          Disjoint(one.indicesOffset, 512 * one.indicesStride, two.indicesOffset, 512 * two.indicesStride) &&
          Disjoint(one.primitiveOffset, 512 * one.primitiveStride, two.primitiveOffset, 512 * two.primitiveStride) &&
          Disjoint(one.countOffset, sizeof(uint32_t), two.countOffset, sizeof(uint32_t)));
    CHECK(CheckTile(zero, 0.0f, .511f) && CheckTile(one, .512f, 1.023f) &&
          CheckTile(two, 1.024f, 1.535f));
    auto wholeVertex = std::dynamic_pointer_cast<HdStBufferArrayRange>(wholeBundle->vertexRange);
    auto const *wholeSlot = wholeBundle->FindTopologyRange(HdStBasisCurvesGpuTopologyMode::Curves);
    auto wholeTopology = wholeSlot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(
        wholeSlot->topologyRange) : nullptr;
    CHECK(wholeVertex && wholeVertex->GetNumElements() == 3072 && wholeTopology &&
          wholeTopology->GetNumElements() == 3072 &&
          Read<uint32_t>(whole.countBuffer, whole.countOffset) == 3072);

    auto const *emptySlot = emptyBundle->FindTopologyRange(HdStBasisCurvesGpuTopologyMode::Curves);
    auto emptyVertex = std::dynamic_pointer_cast<HdStBufferArrayRange>(emptyBundle->vertexRange);
    auto emptyTopology = emptySlot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(
        emptySlot->topologyRange) : nullptr;
    auto emptyCount = emptySlot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(
        emptySlot->drawCountRange) : nullptr;
    auto emptyDraw = emptyCount ? emptyCount->GetResource(TfToken("drawCount")) : nullptr;
    auto *emptyBuffer = emptyDraw && emptyDraw->GetHandle()
        ? dynamic_cast<HgiGLBuffer *>(emptyDraw->GetHandle().Get()) : nullptr;
    CHECK(emptyVertex && emptyVertex->GetNumElements() == 0 && emptyTopology &&
          emptyTopology->GetNumElements() == 0 && emptyCount &&
          emptyCount->GetNumElements() == 1 && emptyDraw && emptyBuffer);
    size_t const emptyOffset = Offset(*emptyCount, TfToken("drawCount"), *emptyDraw);
    CHECK(Read<uint32_t>(emptyBuffer, emptyOffset) == 0 && glGetError() == GL_NO_ERROR &&
          errors.IsClean());

    // A zero-output tile must also work in a fresh registry, where it cannot
    // inherit a nonempty aggregate allocation or a previously mapped channel.
    HgiGL isolatedHgi;
    HdDriver isolatedDriver{HgiTokens->renderDriver,
        VtValue(static_cast<Hgi *>(&isolatedHgi))};
    HdStRenderDelegate isolatedDelegate;
    isolatedDelegate.SetDrivers({&isolatedDriver});
    auto isolatedRegistry = std::dynamic_pointer_cast<HdStResourceRegistry>(
        isolatedDelegate.GetResourceRegistry());
    CHECK(isolatedRegistry && isolatedRegistry.get() != registry.get());
    auto isolatedProvider = MakeProvider(generation, 3);
    auto isolatedBundle = isolatedProvider->Prepare(isolatedRegistry.get(), request);
    CHECK(isolatedBundle);
    isolatedRegistry->Commit();
    CHECK(isolatedBundle->Ready() && isolatedBundle->generation == 0);
    auto isolatedVertex = std::dynamic_pointer_cast<HdStBufferArrayRange>(
        isolatedBundle->vertexRange);
    auto const *isolatedSlot = isolatedBundle->FindTopologyRange(
        HdStBasisCurvesGpuTopologyMode::Curves);
    auto isolatedTopology = isolatedSlot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(
        isolatedSlot->topologyRange) : nullptr;
    auto isolatedCount = isolatedSlot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(
        isolatedSlot->drawCountRange) : nullptr;
    auto isolatedDraw = isolatedCount ? isolatedCount->GetResource(TfToken("drawCount")) : nullptr;
    auto *isolatedCountBuffer = isolatedDraw && isolatedDraw->GetHandle()
        ? dynamic_cast<HgiGLBuffer *>(isolatedDraw->GetHandle().Get()) : nullptr;
    auto isolatedPoints = isolatedVertex ? isolatedVertex->GetResource(HdTokens->points) : nullptr;
    auto isolatedWidths = isolatedVertex ? isolatedVertex->GetResource(HdTokens->widths) : nullptr;
    auto isolatedHairT = isolatedVertex ? isolatedVertex->GetResource(TfToken("hairT")) : nullptr;
    CHECK(isolatedVertex && isolatedVertex->GetNumElements() == 0 && isolatedTopology &&
          isolatedTopology->GetNumElements() == 0 && isolatedCount &&
          isolatedCount->GetNumElements() == 1 && isolatedCountBuffer &&
          (!isolatedPoints || !isolatedPoints->GetHandle()) &&
          (!isolatedWidths || !isolatedWidths->GetHandle()) &&
          (!isolatedHairT || !isolatedHairT->GetHandle()));
    size_t const isolatedCountOffset = Offset(*isolatedCount, TfToken("drawCount"), *isolatedDraw);
    CHECK(Read<uint32_t>(isolatedCountBuffer, isolatedCountOffset) == 0 &&
          glGetError() == GL_NO_ERROR && errors.IsClean());
    std::puts("testUsdGenCudaTiledBasisCurvesProvider: PASS");
    return 0;
}
