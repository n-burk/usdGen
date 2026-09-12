// Direct production-provider CUDA->Storm topology validation.  This remains
// intentionally below the retained-scene/framebuffer gates: it checks the
// provider's BAR addresses and native index payload after Registry::Commit.
#include "usdGenImaging/cudaBasisCurvesProvider.h"
#include "../cudaGlFixture.h"
#include "eglctx.h"
#include "usdGen/session.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"
#include "pxr/imaging/hdSt/bufferResource.h"
#include "pxr/imaging/hdSt/renderDelegate.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgiGL/buffer.h"
#include "pxr/imaging/hgiGL/hgi.h"

#include <array>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE
using usdGenImaging::UsdGenCudaBasisCurvesProvider;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #x); return 1; } } while (false)

namespace {
struct Fields {
    HgiGLBuffer *indicesBuffer = nullptr;
    HgiGLBuffer *primitiveBuffer = nullptr;
    HgiGLBuffer *countBuffer = nullptr;
    size_t indicesOffset = 0, primitiveOffset = 0, countOffset = 0;
};

bool GetFields(HdStBasisCurvesGpuBundle const &bundle, Fields *out)
{
    auto const *slot = bundle.FindTopologyRange(HdStBasisCurvesGpuTopologyMode::Curves);
    auto topology = slot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(slot->topologyRange) : nullptr;
    auto count = slot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(slot->drawCountRange) : nullptr;
    auto indices = topology ? topology->GetResource(HdTokens->indices) : nullptr;
    auto primitive = topology ? topology->GetResource(HdTokens->primitiveParam) : nullptr;
    auto draw = count ? count->GetResource(TfToken("drawCount")) : nullptr;
    if (!indices || !primitive || !draw || !indices->GetHandle() || !primitive->GetHandle() ||
        !draw->GetHandle()) return false;
    auto *ib = dynamic_cast<HgiGLBuffer *>(indices->GetHandle().Get());
    auto *pb = dynamic_cast<HgiGLBuffer *>(primitive->GetHandle().Get());
    auto *cb = dynamic_cast<HgiGLBuffer *>(draw->GetHandle().Get());
    if (!ib || !pb || !cb) return false;
    out->indicesBuffer = ib;
    out->primitiveBuffer = pb;
    out->countBuffer = cb;
    // Each field can have a distinct backing allocation.  Aggregate behavior
    // is asserted only between the same field in two separately prepared BARs.
    out->indicesOffset = size_t(topology->GetByteOffset(HdTokens->indices)) + size_t(indices->GetOffset());
    out->primitiveOffset = size_t(topology->GetByteOffset(HdTokens->primitiveParam)) + size_t(primitive->GetOffset());
    out->countOffset = size_t(count->GetByteOffset(TfToken("drawCount"))) + size_t(draw->GetOffset());
    return true;
}

std::shared_ptr<UsdGenCudaBasisCurvesProvider> MakeProvider(
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation)
{
    UsdGenCudaBasisCurvesProvider::CreateInfo info;
    info.generation = std::move(generation);
    info.curveType = HdTokens->linear;
    info.curveBasis = HdTokens->bspline;
    info.curveWrap = HdTokens->nonperiodic;
    return std::make_shared<UsdGenCudaBasisCurvesProvider>(std::move(info));
}

// Use the real CUDA length-cull publication path: compaction owns the one
// device offset sentinel even when every curve is removed.
std::shared_ptr<const usdGen::UsdGenDeviceGeneration> MakeEmptyGeneration()
{
    usdGen::UsdGenGraphDesc desc;
    desc.description = SdfPath("/Empty/Description");
    desc.executionBackend = usdGen::UsdGenExecutionBackend::Cuda;
    desc.defaultWidth = .01f;
    usdGen::UsdGenSurfaceDesc surface;
    surface.path = SdfPath("/Empty/Scalp");
    surface.worldMatrix.SetIdentity();
    surface.restPoints = {{-1,-1,0}, {2,-1,0}, {-1,2,0}};
    surface.points = surface.restPoints;
    surface.faceVertexCounts = {3};
    surface.faceVertexIndices = {0, 1, 2};
    desc.surfaces.push_back(surface);
    usdGen::UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/Empty/Hair");
    curves.role = usdGen::UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.type = HdTokens->cubic;
    curves.basis = HdTokens->bspline;
    curves.wrap = HdTokens->pinned;
    curves.curveVertexCounts = {4, 4};
    curves.curveId = {1, 2};
    curves.points = {{0,0,0}, {0,.33f,0}, {0,.66f,0}, {0,1,0},
                     {1,0,0}, {1,.33f,0}, {1,.66f,0}, {1,1,0}};
    curves.rest = curves.points;
    curves.skinPrim = {0, 0};
    curves.skinPrimUv = {{.1f,.1f}, {.2f,.2f}};
    desc.curveSets.push_back(curves);
    usdGen::UsdGenNodeDesc source;
    source.path = SdfPath("/Empty/Source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.surfaces = {surface.path};
    usdGen::UsdGenNodeDesc length;
    length.path = SdfPath("/Empty/Length");
    length.type = TfToken("UsdGenLength");
    length.inputs = {source.path};
    length.params = {{TfToken("length:mode"), VtValue(TfToken("cull")), false},
                     {TfToken("cullThreshold"), VtValue(1000.0f), false}};
    desc.nodes = {source, length};
    desc.terminal = length.path;
    usdGen::UsdGenSession session;
    session.SetDevicePublicationEnabled(true);
    session.SetGraphDesc(desc);
    auto result = session.Commit(1, usdGen::UsdGenCommitReason::SetTime);
    if (!result || !result->device) {
        for (auto const &error : session.LastDiagnostics().errors)
            std::fprintf(stderr, "empty CUDA fixture: %s\n", error.c_str());
    }
    return result ? result->device : nullptr;
}
}

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

    auto first = MakeProvider(usdGenTest::MakeCudaGlFixture(.1f));
    auto second = MakeProvider(usdGenTest::MakeCudaGlFixture(.2f));
    CHECK(first && second);
    HdStBasisCurvesGpuPrepareRequest request;
    request.topologyModes = {HdStBasisCurvesGpuTopologyMode::Curves};
    auto firstBundle = first->Prepare(registry.get(), request);
    auto secondBundle = second->Prepare(registry.get(), request);
    CHECK(firstBundle && secondBundle);
    registry->Commit();
    CHECK(firstBundle->Ready() && secondBundle->Ready());

    Fields a, b;
    CHECK(GetFields(*firstBundle, &a) && GetFields(*secondBundle, &b));
    CHECK(a.indicesBuffer == b.indicesBuffer);
    CHECK(a.indicesOffset != b.indicesOffset); // two ranges, same aggregate field allocation
    std::array<int32_t, 6> indices{};
    std::array<int32_t, 6> secondIndices{};
    std::array<int32_t, 3> primitive{};
    uint32_t drawCount = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, a.indicesBuffer->GetBufferId());
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(a.indicesOffset),
        GLsizeiptr(indices.size() * sizeof(int32_t)), indices.data());
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(b.indicesOffset),
        GLsizeiptr(secondIndices.size() * sizeof(int32_t)), secondIndices.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, a.primitiveBuffer->GetBufferId());
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(a.primitiveOffset),
        GLsizeiptr(primitive.size() * sizeof(int32_t)), primitive.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, a.countBuffer->GetBufferId());
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(a.countOffset), sizeof(drawCount), &drawCount);
    CHECK((indices == std::array<int32_t, 6>{0, 1, 2, 3, 3, 4}));
    CHECK(secondIndices == indices);
    CHECK((primitive == std::array<int32_t, 3>{0, 1, 1}));
    CHECK(drawCount == 6);

    auto emptyGeneration = MakeEmptyGeneration();
    CHECK(emptyGeneration && emptyGeneration->Geometry().curveCount == 0 &&
          emptyGeneration->Geometry().pointCount == 0);
    auto emptyProvider = MakeProvider(emptyGeneration);
    auto emptyBundle = emptyProvider->Prepare(registry.get(), request);
    CHECK(emptyBundle);
    registry->Commit();
    CHECK(emptyBundle->Ready());
    auto const *emptySlot = emptyBundle->FindTopologyRange(HdStBasisCurvesGpuTopologyMode::Curves);
    auto emptyCount = emptySlot ? std::dynamic_pointer_cast<HdStBufferArrayRange>(
        emptySlot->drawCountRange) : nullptr;
    auto emptyDraw = emptyCount ? emptyCount->GetResource(TfToken("drawCount")) : nullptr;
    auto *emptyBuffer = emptyDraw ? dynamic_cast<HgiGLBuffer *>(emptyDraw->GetHandle().Get()) : nullptr;
    CHECK(emptyBuffer && emptyCount->GetNumElements() == 1);
    size_t emptyOffset = size_t(emptyCount->GetByteOffset(TfToken("drawCount"))) +
        size_t(emptyDraw->GetOffset());
    uint32_t emptyDrawCount = ~uint32_t(0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, emptyBuffer->GetBufferId());
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, GLintptr(emptyOffset), sizeof(emptyDrawCount),
        &emptyDrawCount);
    CHECK(emptyDrawCount == 0);
    CHECK(glGetError() == GL_NO_ERROR && errors.IsClean());
    std::puts("testUsdGenCudaBasisCurvesProvider: PASS");
    return 0;
}
