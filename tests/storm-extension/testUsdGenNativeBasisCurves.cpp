// Retained-scene-index ingress test for the patched Storm BasisCurves path.
//
// This deliberately supplies no host curveVertexCounts/curveIndices.  The
// private HdSt extension must therefore treat the GPU provider as
// authoritative, including when a candidate provider cannot prepare a
// complete bundle.  The test is useful even on machines without CUDA/MDI: it
// exercises the real HdSceneIndex -> HdSceneIndexAdapterSceneDelegate ->
// HdRenderIndex registration path and proves that the host topology fallback
// was not silently selected.
#include "pxr/pxr.h"
#include "eglctx.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/rprimCollection.h"
#include "pxr/imaging/hd/repr.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneIndexAdapterSceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/engine.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgiGL/hgi.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/hdSt/basisCurvesGpuDataSource.h"
#include "pxr/imaging/hdSt/renderDelegate.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/imaging/hdx/unitTestDelegate.h"
#include "pxr/imaging/hdx/renderSetupTask.h"
#include "pxr/imaging/hdx/renderTask.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool SameFiniteRgba(const float *lhs, const float *rhs, size_t scalars)
{
    if (scalars == 0) return true;
    if (!lhs || !rhs) return false;
    for (size_t i = 0; i < scalars; ++i) {
        if (!std::isfinite(lhs[i]) || !std::isfinite(rhs[i]) ||
            lhs[i] != rhs[i]) return false;
    }
    return true;
}

bool SameFiniteRgbaSelfTest()
{
    const float a[] = {1, 2, 3, 4};
    const float b[] = {1, 2, 3, 4};
    const float c[] = {1, 2, 3, 5};
    const float nan[] = {std::numeric_limits<float>::quiet_NaN(), 2, 3, 4};
    const float inf[] = {std::numeric_limits<float>::infinity(), 2, 3, 4};
    return SameFiniteRgba(nullptr, nullptr, 0) &&
        !SameFiniteRgba(nullptr, a, 1) &&
        SameFiniteRgba(a, b, 4) && !SameFiniteRgba(a, c, 4) &&
        !SameFiniteRgba(nan, nan, 4) && !SameFiniteRgba(inf, inf, 4);
}

class RejectingProvider final : public HdStBasisCurvesGpuDataSource {
public:
    HD_DECLARE_DATASOURCE(RejectingProvider);

    HdStBasisCurvesGpuBundleSharedPtr Prepare(
        HdStResourceRegistry *,
        HdStBasisCurvesGpuPrepareRequest const &request) override {
        ++prepareCount;
        lastModes = request.topologyModes;
        return {};
    }

    int prepareCount = 0;
    HdStBasisCurvesGpuTopologyModeVector lastModes;
};

// Test-only complete provider.  Values are uploaded through Storm's real
// HdVtBufferSource path; no CPU topology is authored in the scene index.
class FixtureProvider final : public HdStBasisCurvesGpuDataSource {
public:
    HD_DECLARE_DATASOURCE(FixtureProvider);
    explicit FixtureProvider(bool admit) : _admit(admit) {}
    HdStBasisCurvesGpuBundleSharedPtr Prepare(
        HdStResourceRegistry *registry,
        HdStBasisCurvesGpuPrepareRequest const &request) override {
        ++prepareCount;
        auto bundle = std::make_shared<HdStBasisCurvesGpuBundle>();
        bundle->generation = ++generation;
        bundle->curveType = TfToken("linear");
        bundle->curveBasis = TfToken("linear");
        bundle->curveWrap = TfToken("nonperiodic");
        bundle->bounds = GfBBox3d(GfRange3d(GfVec3d(-1), GfVec3d(1)));
        HdBufferSpecVector vertexSpecs{
            {HdTokens->points, {HdTypeFloatVec3, 1}},
            {HdTokens->widths, {HdTypeFloat, 1}},
            {TfToken("hairT"), {HdTypeFloat, 1}}};
        bundle->vertexRange = registry->AllocateNonUniformBufferArrayRange(
            HdTokens->primvar, vertexSpecs, HdBufferArrayUsageHintBitsVertex);
        registry->AddSource(bundle->vertexRange, std::make_shared<HdVtBufferSource>(
            HdTokens->points, VtValue(VtVec3fArray{
                GfVec3f(-0.8f, -0.2f, 0), GfVec3f(-0.4f, 0.7f, 0),
                GfVec3f(0, -0.1f, 0), GfVec3f(0.4f, 0.7f, 0),
                GfVec3f(0.8f, -0.2f, 0)})));
        registry->AddSource(bundle->vertexRange, std::make_shared<HdVtBufferSource>(
            HdTokens->widths, VtValue(VtFloatArray{0.08f, 0.08f, 0.08f, 0.08f, 0.08f})));
        registry->AddSource(bundle->vertexRange, std::make_shared<HdVtBufferSource>(
                TfToken("hairT"), VtValue(VtFloatArray{0, 0.25f, 0.5f, 0.75f, 1})));
        for (auto mode : request.topologyModes) {
            HdBufferSpecVector specs{
                {HdTokens->indices, {HdTypeInt32Vec2, 1}},
                {HdTokens->primitiveParam, {HdTypeInt32, 1}}};
            HdStBasisCurvesGpuTopologyRange range;
            range.mode = mode;
            range.topologyRange = registry->AllocateNonUniformBufferArrayRange(
                HdTokens->topology, specs, HdBufferArrayUsageHintBitsIndex);
            registry->AddSource(range.topologyRange, std::make_shared<HdVtBufferSource>(
                HdTokens->indices, VtValue(VtVec2iArray{
                    GfVec2i(0,1), GfVec2i(1,2), GfVec2i(2,3), GfVec2i(3,4)})));
            registry->AddSource(range.topologyRange, std::make_shared<HdVtBufferSource>(
                HdTokens->primitiveParam, VtValue(VtIntArray{0,0,0,0})));
            HdBufferSpecVector countSpec{{TfToken("drawCount"), {HdTypeUInt32, 1}}};
            range.drawCountRange = registry->AllocateNonUniformBufferArrayRange(
                HdTokens->topology, countSpec, HdBufferArrayUsageHintBitsStorage);
            registry->AddSource(range.drawCountRange, std::make_shared<HdVtBufferSource>(
                TfToken("drawCount"), VtValue(_drawCount)));
            bundle->topologyRanges.push_back(range);
        }
        bundle->ready = [admit = _admit] { return admit; };
        return bundle;
    }
    bool _admit;
    uint64_t generation = 0;
    int prepareCount = 0;
    uint32_t _drawCount = 2;
};

HdContainerDataSourceHandle MakePrimDataSource(
    HdDataSourceBase::Handle const &provider)
{
    // Empty topology is metadata only.  Supplying counts here would make the
    // test accidentally exercise Storm's legacy CPU index builder.
    HdBasisCurvesTopologySchema::Builder topology;
    topology.SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(
        TfToken("linear")));
    topology.SetType(HdRetainedTypedSampledDataSource<TfToken>::New(
        TfToken("linear")));
    topology.SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(
        TfToken("nonperiodic")));

    HdBasisCurvesSchema::Builder curves;
    curves.SetTopology(topology.Build());

    TfTokenVector names{HdBasisCurvesSchemaTokens->basisCurves,
                        TfToken("hdStBasisCurvesGpu")};
    HdDataSourceBase::Handle values[] = {curves.Build(), provider};
    return HdRetainedContainerDataSource::New(
        static_cast<size_t>(names.size()), names.data(), values);
}

} // namespace

int main()
{
    if (!SameFiniteRgbaSelfTest()) {
        std::fprintf(stderr, "SameFiniteRgba self-test failed\n");
        return 1;
    }
    TfErrorMark errors;
    if (!eglctx::MakeHeadlessGLContext()) {
        std::fprintf(stderr, "headless GL unavailable\n");
        return 77;
    }
    GarchGLApiLoad();
    HgiGL hgi;
    HdDriver driver;
    driver.name = HgiTokens->renderDriver;
    driver.driver = VtValue(static_cast<Hgi *>(&hgi));

    HdRetainedSceneIndexRefPtr source = HdRetainedSceneIndex::New();
    auto provider = FixtureProvider::New(true);
    const SdfPath path("/nativeCurves");
    source->AddPrims({{path, HdPrimTypeTokens->basisCurves,
                       MakePrimDataSource(provider)}});

    HdStRenderDelegate renderDelegate;
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(
        &renderDelegate, {&driver}));
    if (!index) {
        std::fprintf(stderr, "native Storm render index unavailable\n");
        return 77;
    }

    // Let the render index construct its terminal scene-index adapter.  This
    // is the same path used by the native scene-index renderer, and is
    // important because HdSt queries GetTerminalSceneIndex() for providers.
    index->InsertSceneIndex(source, SdfPath::AbsoluteRootPath());
    HdRprim const *rprim = index->GetRprim(path);
    if (!rprim) {
        std::fprintf(stderr, "BasisCurves was not registered in HdRenderIndex\n");
        return 1;
    }

    HdBasisCurvesTopologySchema const authoredTopology =
        HdBasisCurvesSchema::GetFromParent(
            source->GetPrim(path).dataSource).GetTopology();
    if (authoredTopology.GetCurveVertexCounts() ||
        authoredTopology.GetCurveIndices()) {
        std::fprintf(stderr, "fixture accidentally authored CPU topology\n");
        return 1;
    }

    // No host topology exists in the source.  The adapter can still expose
    // the prim, but the provider is the sole route to a drawable bundle.
    // Sync is intentionally driven through the registered rprim.  A null
    // candidate must not make the native path ingest host topology.  The
    // private provider's normal prepare is exercised when the patched Storm
    // target is linked; this fixture also remains valid on CUDA-off builds.
    Hdx_UnitTestDelegate testDelegate(index.get());
    testDelegate.AddRenderSetupTask(SdfPath("/renderSetup"));
    testDelegate.AddRenderTask(SdfPath("/render"));
    HdRenderPassAovBindingVector aovs =
        testDelegate.AddAovBindings(GfVec2i(128, 128), false);
    aovs[0].clearValue = VtValue(GfVec4f(0));
    testDelegate.UpdateRenderBuffer(
        aovs[0].renderBufferId,
        HdRenderBufferDescriptor(GfVec3i(128, 128, 1),
                                 HdFormatFloat32Vec4, false));
    VtValue taskParams = testDelegate.GetTaskParam(
        SdfPath("/renderSetup"), HdTokens->params);
    HdxRenderTaskParams params = taskParams.Get<HdxRenderTaskParams>();
    params.viewport = GfVec4d(0, 0, 128, 128);
    params.aovBindings = aovs;
    testDelegate.SetTaskParam(SdfPath("/renderSetup"), HdTokens->params,
                              VtValue(params));
    HdTaskSharedPtrVector tasks{
        index->GetTask(SdfPath("/renderSetup")),
        index->GetTask(SdfPath("/render"))};
    HdEngine engine;
    HdRprimCollection collection(TfToken("nativeCurves"),
                                 HdReprSelector(HdReprTokens->smoothHull));
    index->EnqueueCollectionToSync(collection);
    provider->_drawCount = 0;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    HdRenderBuffer *baseline = dynamic_cast<HdRenderBuffer *>(
        index->GetBprim(HdPrimTypeTokens->renderBuffer, aovs[0].renderBufferId));
    if (!baseline || baseline->GetFormat() != HdFormatFloat32Vec4) {
        std::fprintf(stderr, "baseline format unavailable: %d\n",
                     baseline ? int(baseline->GetFormat()) : -1);
        return 1;
    }
    const float *basePixels = static_cast<const float *>(baseline->Map());
    if (!basePixels) {
        std::fprintf(stderr, "baseline framebuffer Map failed\n");
        return 1;
    }
    for (unsigned int i = 0; i < baseline->GetWidth() * baseline->GetHeight(); ++i) {
        if (basePixels[i * 4 + 3] > 0.001f) {
            baseline->Unmap();
            std::fprintf(stderr, "zero draw-count rendered pixels\n");
            return 1;
        }
    }
    baseline->Unmap();
    provider->_drawCount = 2;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    if (!errors.IsClean()) {
        std::fprintf(stderr, "Storm reported errors during framebuffer execute\n");
        errors.Clear();
        return 1;
    }
    HdRenderBuffer *buffer = dynamic_cast<HdRenderBuffer *>(
        index->GetBprim(HdPrimTypeTokens->renderBuffer, aovs[0].renderBufferId));
    size_t nonBackground = 0;
    std::vector<float> acceptedPixels;
    if (buffer) {
        const unsigned int width = buffer->GetWidth();
        const unsigned int height = buffer->GetHeight();
        if (buffer->GetFormat() != HdFormatFloat32Vec4) {
            std::fprintf(stderr, "unexpected framebuffer format %d\n",
                         int(buffer->GetFormat()));
            return 1;
        }
        const float *pixels = static_cast<const float *>(buffer->Map());
        if (pixels) {
            acceptedPixels.assign(pixels, pixels + width * height * 4);
            for (unsigned int i = 0; i < width * height; ++i)
                nonBackground += pixels[i * 4 + 3] > 0.001f;
            buffer->Unmap();
        }
    }
    if (!buffer || nonBackground == 0) {
        std::fprintf(stderr, "native framebuffer contained no drawn pixels\n");
        return 1;
    }
    const size_t count2Pixels = nonBackground;
    provider->_drawCount = 4;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    const int beforeCount4Prepare = provider->prepareCount;
    engine.Execute(index.get(), &tasks);
    if (glGetError() != GL_NO_ERROR) {
        std::fprintf(stderr, "GL error during count4 execute\n");
        return 1;
    }
    if (provider->prepareCount <= beforeCount4Prepare) {
        std::fprintf(stderr, "count4 candidate was not prepared\n");
        return 1;
    }
    const float *count4Data = static_cast<const float *>(buffer->Map());
    size_t count4Pixels = 0;
    if (count4Data) {
        for (unsigned int i = 0; i < buffer->GetWidth() * buffer->GetHeight(); ++i)
            count4Pixels += count4Data[i * 4 + 3] > 0.001f;
        buffer->Unmap();
    }
    if (count4Pixels <= count2Pixels) {
        std::fprintf(stderr, "drawCount4 did not draw more than drawCount2\n");
        return 1;
    }
    acceptedPixels.clear();
    const float *accepted4 = static_cast<const float *>(buffer->Map());
    if (!accepted4) {
        std::fprintf(stderr, "count4 framebuffer Map failed\n");
        return 1;
    }
    acceptedPixels.assign(accepted4, accepted4 + buffer->GetWidth() * buffer->GetHeight() * 4);
    buffer->Unmap();
    provider->_drawCount = 0;
    provider->_admit = false;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    const int beforeRejectPrepare = provider->prepareCount;
    engine.Execute(index.get(), &tasks);
    if (glGetError() != GL_NO_ERROR) {
        std::fprintf(stderr, "GL error during rejected execute\n");
        return 1;
    }
    if (provider->prepareCount <= beforeRejectPrepare) {
        std::fprintf(stderr, "rejected candidate was not prepared\n");
        return 1;
    }
    const float *rejectedPixels = static_cast<const float *>(buffer->Map());
    if (!rejectedPixels || acceptedPixels.size() != buffer->GetWidth() * buffer->GetHeight() * 4 ||
        !SameFiniteRgba(acceptedPixels.data(), rejectedPixels,
                        acceptedPixels.size())) {
        if (rejectedPixels) buffer->Unmap();
        std::fprintf(stderr, "rejected candidate replaced visible framebuffer\n");
        return 1;
    }
    buffer->Unmap();
    if (!errors.IsClean()) {
        std::fprintf(stderr, "Storm reported errors after rejected candidate\n");
        errors.Clear();
        return 1;
    }
    std::printf("Native retained BasisCurves framebuffer: PASS (drawCount2=%zu, drawCount4=%zu)\n",
                count2Pixels, count4Pixels);
    return 0;
}
