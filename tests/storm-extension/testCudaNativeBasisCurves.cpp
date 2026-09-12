// Production CUDA provider through the retained scene-index BasisCurves path.
#include "usdGenImaging/cudaBasisCurvesProvider.h"
#include "../cudaGlFixture.h"
#include "eglctx.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/engine.h"
#include "pxr/imaging/hd/materialBindingSchema.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/rprimCollection.h"
#include "pxr/imaging/hd/repr.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/imaging/hdSt/basisCurvesGpuDataSource.h"
#include "pxr/imaging/hdSt/renderDelegate.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgiGL/hgi.h"
#include "pxr/imaging/hdx/unitTestDelegate.h"
#include "pxr/imaging/hdx/renderSetupTask.h"
#include "pxr/imaging/hdx/renderTask.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

using usdGenImaging::UsdGenCudaBasisCurvesProvider;

namespace {
bool FinitePixels(const float *p, size_t n)
{
    if (!p && n) return false;
    for (size_t i = 0; i < n; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

struct Presentation {
    GfMatrix4d xform{1.0};
    bool visible = true;
    SdfPath materialPath;
};

HdContainerDataSourceHandle MakePrimDataSource(
    HdDataSourceBase::Handle const &provider, Presentation const &presentation)
{
    HdBasisCurvesTopologySchema::Builder topology;
    topology.SetBasis(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->linear));
    topology.SetType(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->linear));
    topology.SetWrap(HdRetainedTypedSampledDataSource<TfToken>::New(HdTokens->nonperiodic));
    HdBasisCurvesSchema::Builder curves;
    curves.SetTopology(topology.Build());
    auto xform = HdXformSchema::Builder()
        .SetMatrix(HdRetainedTypedSampledDataSource<GfMatrix4d>::New(
            presentation.xform))
        .SetResetXformStack(HdRetainedTypedSampledDataSource<bool>::New(true))
        .Build();
    auto visibility = HdVisibilitySchema::Builder()
        .SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(
            presentation.visible))
        .Build();
    auto material = HdMaterialBindingSchema::Builder()
        .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(
            presentation.materialPath))
        .Build();
    // allPurpose is the schema's empty-token default child, not the literal
    // string "allPurpose" used by usdGen's app-side publication metadata.
    TfToken const materialPurpose = HdMaterialBindingsSchemaTokens->allPurpose;
    HdDataSourceBaseHandle const materialValue = material;
    auto bindings = HdRetainedContainerDataSource::New(1, &materialPurpose,
        &materialValue);
    TfToken const names[] = {HdBasisCurvesSchemaTokens->basisCurves,
        TfToken("hdStBasisCurvesGpu"), TfToken("xform"), TfToken("visibility"),
        TfToken("materialBindings")};
    HdDataSourceBase::Handle const values[] = {curves.Build(), provider, xform,
        visibility, bindings};
    return HdRetainedContainerDataSource::New(5, names, values);
}

class MutablePrimDataSource final : public HdContainerDataSource {
public:
    HD_DECLARE_DATASOURCE(MutablePrimDataSource);
    explicit MutablePrimDataSource(HdContainerDataSourceHandle value)
        : _value(std::move(value)) {}
    TfTokenVector GetNames() override { return _value->GetNames(); }
    HdDataSourceBaseHandle Get(TfToken const &name) override {
        return _value->Get(name);
    }
    void Set(HdContainerDataSourceHandle value) { _value = std::move(value); }
private:
    HdContainerDataSourceHandle _value;
};
HdStBasisCurvesGpuBundleSharedPtr MakeCudaBundle(
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation,
    HdStResourceRegistry *registry, HdStBasisCurvesGpuPrepareRequest const &request)
{
    UsdGenCudaBasisCurvesProvider::CreateInfo info;
    info.generation = std::move(generation);
    info.curveType = HdTokens->linear;
    info.curveBasis = HdTokens->linear;
    info.curveWrap = HdTokens->nonperiodic;
    info.conservativeBounds = GfBBox3d(GfRange3d(GfVec3d(-1), GfVec3d(1)));
    auto provider = std::make_shared<UsdGenCudaBasisCurvesProvider>(std::move(info));
    return provider->Prepare(registry, request);
}
class CudaProvider final : public HdStBasisCurvesGpuDataSource {
public:
    HD_DECLARE_DATASOURCE(CudaProvider);
    explicit CudaProvider(std::shared_ptr<const usdGen::UsdGenDeviceGeneration> value)
        : generation(std::move(value)) {}
    HdStBasisCurvesGpuBundleSharedPtr Prepare(HdStResourceRegistry *registry,
        HdStBasisCurvesGpuPrepareRequest const &request) override {
        ++prepares;
        if (reject) return {};
        modes = request.topologyModes;
        last = MakeCudaBundle(generation, registry, request);
        bool complete = last && last->IsCompleteFor(modes);
        std::fprintf(stderr, "cuda provider Prepare: modes=%zu bundle=%d complete=%d generation=%llu\n",
            modes.size(), int(bool(last)), int(complete),
            static_cast<unsigned long long>(last ? last->generation : 0));
        if (last) for (auto mode : modes) {
            auto const *range = last->FindTopologyRange(mode);
            std::fprintf(stderr, "cuda provider range mode=%d topology=%d count=%d\n", int(mode),
                int(range && range->topologyRange && range->topologyRange->IsValid()),
                int(range && range->drawCountRange && range->drawCountRange->IsValid()));
        }
        if (last && last->ready) {
            auto clone = std::make_shared<HdStBasisCurvesGpuBundle>(*last);
            auto original = clone->ready;
            clone->ready = [original = std::move(original)] {
                std::fprintf(stderr, "cuda provider Ready ENTER\n");
                bool const result = original();
                std::fprintf(stderr, "cuda provider Ready RESULT=%d\n", int(result));
                return result;
            };
            last = clone;
        }
        return last;
    }
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation;
    int prepares = 0;
    bool reject = false;
    HdStBasisCurvesGpuTopologyModeVector modes;
    HdStBasisCurvesGpuBundleSharedPtr last;
};
}

int main()
{
    TfErrorMark errors;
    if (!eglctx::MakeHeadlessGLContext()) return 77;
    GarchGLApiLoad();
    HgiGL hgi;
    HdDriver driver{HgiTokens->renderDriver, VtValue(static_cast<Hgi *>(&hgi))};
    // Zero is a valid first generation from the engine/session store.
    auto generation = usdGenTest::MakeCudaGlFixture(.08f, 0);
    if (!generation) return 77;
    if (generation->Identity().generation != 0) return 1;

    // The datasource is production code.  The adapter sees no counts or
    // indices in authored topology, so CPU topology fallback cannot render.
    auto provider = CudaProvider::New(generation);
    Presentation acceptedPresentation;
    acceptedPresentation.materialPath = SdfPath("/Looks/Accepted");
    auto primData = MutablePrimDataSource::New(MakePrimDataSource(
        provider, acceptedPresentation));
    HdRetainedSceneIndexRefPtr source = HdRetainedSceneIndex::New();
    SdfPath const path("/cudaNativeCurves");
    source->AddPrims({{path, HdPrimTypeTokens->basisCurves, primData}});
    HdStRenderDelegate renderDelegate;
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&renderDelegate, {&driver}));
    if (!index) return 77;
    index->InsertSceneIndex(source, SdfPath::AbsoluteRootPath());
    if (!index->GetRprim(path)) return 1;

    Hdx_UnitTestDelegate delegate(index.get());
    delegate.AddRenderSetupTask(SdfPath("/setup"));
    delegate.AddRenderTask(SdfPath("/render"));
    auto aovs = delegate.AddAovBindings(GfVec2i(128), false);
    aovs[0].clearValue = VtValue(GfVec4f(0));
    delegate.UpdateRenderBuffer(aovs[0].renderBufferId,
        HdRenderBufferDescriptor(GfVec3i(128,128,1), HdFormatFloat32Vec4, false));
    auto params = delegate.GetTaskParam(SdfPath("/setup"), HdTokens->params).Get<HdxRenderTaskParams>();
    params.viewport = GfVec4d(0,0,128,128); params.aovBindings = aovs;
    delegate.SetTaskParam(SdfPath("/setup"), HdTokens->params, VtValue(params));
    HdTaskSharedPtrVector tasks{index->GetTask(SdfPath("/setup")), index->GetTask(SdfPath("/render"))};
    HdEngine engine;
    HdRprimCollection collection(TfToken("cudaNative"), HdReprSelector(HdReprTokens->smoothHull));
    index->EnqueueCollectionToSync(collection);
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    std::fprintf(stderr, "cuda native phase initial Execute begin\n");
    engine.Execute(index.get(), &tasks);
    std::fprintf(stderr, "cuda native phase initial Execute end\n");
    auto *buffer = dynamic_cast<HdRenderBuffer *>(index->GetBprim(
        HdPrimTypeTokens->renderBuffer, aovs[0].renderBufferId));
    if (glGetError() != GL_NO_ERROR || !buffer || buffer->GetFormat() != HdFormatFloat32Vec4 ||
        !provider->prepares || !provider->last || !provider->last->Ready() ||
        provider->last->generation != 0 || !errors.IsClean()) {
        std::fprintf(stderr, "cuda provider state: prepare=%d bundle=%d ready=%d modes=%zu errors=%d\n",
            provider->prepares, int(bool(provider->last)),
            provider->last ? int(provider->last->Ready()) : 0, provider->modes.size(),
            int(!errors.IsClean()));
        return 1;
    }
    auto const *pixels = static_cast<const float *>(buffer->Map());
    if (!pixels) return 1;
    size_t const pixelWords = size_t(buffer->GetWidth()) * buffer->GetHeight() * 4;
    if (!FinitePixels(pixels, pixelWords)) { buffer->Unmap(); return 1; }
    std::vector<float> accepted(pixels, pixels + pixelWords);
    size_t lit = 0;
    for (unsigned i = 0; i != buffer->GetWidth() * buffer->GetHeight(); ++i)
        lit += pixels[i * 4 + 3] > .001f;
    buffer->Unmap();
    if (!lit) {
        std::fprintf(stderr, "initial accepted candidate rendered no pixels\n");
        return 1;
    }
    HdRprim const *rprim = index->GetRprim(path);
    if (!rprim || rprim->GetMaterialId() != acceptedPresentation.materialPath) {
        std::fprintf(stderr, "initial accepted material mismatch: rprim=%d actual=%s expected=%s\n",
            int(bool(rprim)), rprim ? rprim->GetMaterialId().GetText() : "<none>",
            acceptedPresentation.materialPath.GetText());
        return 1;
    }

    // An in-place rejected candidate preserves the current rprim/bundle.
    // A different source generation proves acceptance would have changed it.
    auto changed = usdGenTest::MakeCudaGlFixture(.2f, 1, .25f);
    if (!changed) return 77;
    if (changed->Identity().generation != 1) return 1;
    provider->generation = changed;
    provider->reject = true;
    Presentation rejectedPresentation = acceptedPresentation;
    rejectedPresentation.xform.SetTranslate(GfVec3d(.45, 0, 0));
    rejectedPresentation.visible = false;
    rejectedPresentation.materialPath = SdfPath("/Looks/Rejected");
    primData->Set(MakePrimDataSource(provider, rejectedPresentation));
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    std::fprintf(stderr, "cuda native phase rejected Execute begin\n");
    engine.Execute(index.get(), &tasks);
    std::fprintf(stderr, "cuda native phase rejected Execute end\n");
    if (glGetError() != GL_NO_ERROR || buffer->GetFormat() != HdFormatFloat32Vec4) return 1;
    pixels = static_cast<const float *>(buffer->Map());
    size_t retained = 0;
    if (pixels) for (unsigned i = 0; i != buffer->GetWidth() * buffer->GetHeight(); ++i)
        retained += pixels[i * 4 + 3] > .001f;
    bool const identical = FinitePixels(pixels, pixelWords) &&
        std::equal(accepted.begin(), accepted.end(), pixels);
    if (pixels) buffer->Unmap();
    SdfPath const currentMaterial = rprim ? rprim->GetMaterialId() : SdfPath();
    if (provider->prepares < 2 || retained != lit || !identical || !rprim ||
        currentMaterial != acceptedPresentation.materialPath) {
        std::fprintf(stderr,
            "rejected candidate leaked presentation: prepares=%d retained=%zu lit=%zu "
            "identical=%d rprim=%d oldMaterial=%s currentMaterial=%s\n",
            provider->prepares, retained, lit, int(identical), int(bool(rprim)),
            acceptedPresentation.materialPath.GetText(), currentMaterial.GetText());
        return 1;
    }
    int const preparesBeforeRecovery = provider->prepares;
    provider->reject = false;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    std::fprintf(stderr, "cuda native phase recovery Execute begin\n");
    engine.Execute(index.get(), &tasks);
    std::fprintf(stderr, "cuda native phase recovery Execute end\n");
    if (!errors.IsClean() || provider->prepares <= preparesBeforeRecovery ||
        !provider->last || provider->last->generation != 1) return 1;
    if (glGetError() != GL_NO_ERROR || buffer->GetFormat() != HdFormatFloat32Vec4) return 1;
    pixels = static_cast<const float *>(buffer->Map());
    if (!pixels) return 1;
    if (!FinitePixels(pixels, pixelWords)) { buffer->Unmap(); return 1; }
    size_t recoveredLit = 0;
    for (unsigned i = 0; i != buffer->GetWidth() * buffer->GetHeight(); ++i)
        recoveredLit += pixels[i * 4 + 3] > .001f;
    bool const changedImage = !std::equal(accepted.begin(), accepted.end(), pixels);
    buffer->Unmap();
    std::fprintf(stderr, "cuda native recovery pixels initial=%zu recovered=%zu changed=%d\n",
        lit, recoveredLit, int(changedImage));
    // The recovered candidate is deliberately invisible. Its blank frame,
    // changed material identity and accepted GPU generation must arrive as one
    // coherent state; a rejected candidate above must leave every one old.
    if (!changedImage || recoveredLit != 0 || !rprim ||
        rprim->GetMaterialId() != rejectedPresentation.materialPath) return 1;
    std::printf("CUDA native retained BasisCurves: PASS (%zu pixels)\n", lit);
    return 0;
}
