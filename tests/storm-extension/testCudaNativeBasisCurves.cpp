// Production CUDA provider through the retained scene-index BasisCurves path.
#include "usdGenImaging/cudaBasisCurvesProvider.h"
#include "../cudaGlFixture.h"
#include "eglctx.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/engine.h"
#include "pxr/imaging/hd/instancedBySchema.h"
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
#include "pxr/imaging/hdSt/instancer.h"
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
    SdfPath instancerPath;
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
    auto instancedBy = HdInstancedBySchema::Builder()
        .SetPaths(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
            VtArray<SdfPath>{presentation.instancerPath}))
        .Build();
    // allPurpose is the schema's empty-token default child, not the literal
    // string "allPurpose" used by usdGen's app-side publication metadata.
    TfToken const materialPurpose = HdMaterialBindingsSchemaTokens->allPurpose;
    HdDataSourceBaseHandle const materialValue = material;
    auto bindings = HdRetainedContainerDataSource::New(1, &materialPurpose,
        &materialValue);
    TfToken const names[] = {HdBasisCurvesSchemaTokens->basisCurves,
        TfToken("hdStBasisCurvesGpu"), TfToken("xform"), TfToken("visibility"),
        TfToken("materialBindings"), HdInstancedBySchemaTokens->instancedBy};
    HdDataSourceBase::Handle const values[] = {curves.Build(), provider, xform,
        visibility, bindings, instancedBy};
    return HdRetainedContainerDataSource::New(6, names, values);
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

// The retained scene-index rprim is intentionally not registered through the
// legacy unit-test delegate.  Supply its prototype relationship explicitly so
// the real HdSt instancer generates a non-empty flattened index tuple.
class InstancerFixtureDelegate final : public Hdx_UnitTestDelegate {
public:
    InstancerFixtureDelegate(HdRenderIndex *index, SdfPath prototype,
                             SdfPath instancerA, SdfPath instancerB,
                             SdfPath instancerRoot)
        : Hdx_UnitTestDelegate(index)
        , _prototype(std::move(prototype))
        , _instancerA(std::move(instancerA))
        , _instancerB(std::move(instancerB))
        , _instancerRoot(std::move(instancerRoot))
    {
    }

    SdfPathVector GetInstancerPrototypes(
        SdfPath const &instancerId) override
    {
        if (instancerId == _instancerA || instancerId == _instancerB) {
            return {_prototype};
        }
        if (instancerId == _instancerRoot) {
            return {_instancerB};
        }
        return Hdx_UnitTestDelegate::GetInstancerPrototypes(instancerId);
    }

    VtIntArray GetInstanceIndices(SdfPath const &instancerId,
                                  SdfPath const &prototypeId) override
    {
        if ((instancerId == _instancerA || instancerId == _instancerB) &&
            prototypeId == _prototype) {
            return {0};
        }
        if (instancerId == _instancerRoot && prototypeId == _instancerB) {
            return {0};
        }
        return Hdx_UnitTestDelegate::GetInstanceIndices(instancerId,
                                                         prototypeId);
    }

private:
    SdfPath _prototype;
    SdfPath _instancerA;
    SdfPath _instancerB;
    SdfPath _instancerRoot;
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
            const bool failReady = readyFailure;
            clone->ready = [original = std::move(original), failReady] {
                std::fprintf(stderr, "cuda provider Ready ENTER\n");
                bool const result = original();
                bool const published = result && !failReady;
                std::fprintf(stderr, "cuda provider Ready RESULT=%d\n",
                    int(published));
                return published;
            };
            last = clone;
        }
        return last;
    }
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation;
    int prepares = 0;
    bool reject = false;
    bool readyFailure = false;
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
    SdfPath const path("/cudaNativeCurves");
    SdfPath const instancerA("/instancerA");
    SdfPath const instancerB("/instancerB");
    SdfPath const instancerRoot("/instancerRoot");
    Presentation acceptedPresentation;
    acceptedPresentation.materialPath = SdfPath("/Looks/Accepted");
    acceptedPresentation.instancerPath = instancerA;
    auto primData = MutablePrimDataSource::New(MakePrimDataSource(
        provider, acceptedPresentation));
    HdRetainedSceneIndexRefPtr source = HdRetainedSceneIndex::New();
    source->AddPrims({{path, HdPrimTypeTokens->basisCurves, primData}});
    HdStRenderDelegate renderDelegate;
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&renderDelegate, {&driver}));
    if (!index) return 77;
    index->InsertSceneIndex(source, SdfPath::AbsoluteRootPath());
    if (!index->GetRprim(path)) return 1;

    InstancerFixtureDelegate delegate(index.get(), path, instancerA, instancerB,
                                      instancerRoot);
    GfMatrix4f instancerBRoot(1);
    instancerBRoot.SetTranslate(GfVec3f(.15f, 0, 0));
    delegate.AddInstancer(instancerA);
    delegate.AddInstancer(instancerRoot);
    delegate.AddInstancer(instancerB, instancerRoot, instancerBRoot);
    delegate.SetInstancerProperties(
        instancerA, VtIntArray{0}, VtVec3fArray{GfVec3f(1)},
        VtVec4fArray{GfVec4f(0)}, VtVec3fArray{GfVec3f(0)});
    delegate.SetInstancerProperties(
        instancerB, VtIntArray{0}, VtVec3fArray{GfVec3f(1)},
        VtVec4fArray{GfVec4f(0)}, VtVec3fArray{GfVec3f(.2f, 0, 0)});
    delegate.SetInstancerProperties(
        instancerRoot, VtIntArray{0}, VtVec3fArray{GfVec3f(1)},
        VtVec4fArray{GfVec4f(0)}, VtVec3fArray{GfVec3f(0)});
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
    if (!rprim || rprim->GetMaterialId() != acceptedPresentation.materialPath ||
        rprim->GetInstancerId() != instancerA) {
        std::fprintf(stderr, "initial accepted state mismatch: rprim=%d material=%s instancer=%s\n",
            int(bool(rprim)), rprim ? rprim->GetMaterialId().GetText() : "<none>",
            rprim ? rprim->GetInstancerId().GetText() : "<none>");
        return 1;
    }
    HdStInstancer * const acceptedInstancer =
        static_cast<HdStInstancer *>(index->GetInstancer(instancerA));
    VtIntArray const acceptedInstanceIndices = acceptedInstancer
        ? acceptedInstancer->GetInstanceIndices(path) : VtIntArray();
    // HdStUpdateInstancerData adds its required leading culling sentinel, so
    // this one-level [global-index, instance-index] tuple produces a real
    // three-word rprim instance-index BAR rather than an empty placeholder.
    if (acceptedInstanceIndices.size() != 2 ||
        acceptedInstanceIndices[0] != 0 || acceptedInstanceIndices[1] != 0) {
        return 1;
    }

    // A same-ID instance primvar update must be staged into a new candidate
    // BAR.  Ready(false) may not redirect the accepted draw item to the
    // mutable HdStInstancer range.
    delegate.SetInstancerProperties(
        instancerA, VtIntArray{0}, VtVec3fArray{GfVec3f(1)},
        VtVec4fArray{GfVec4f(0)}, VtVec3fArray{GfVec3f(.25f, 0, 0)});
    index->GetChangeTracker().MarkInstancerDirty(
        instancerA, HdChangeTracker::DirtyPrimvar |
            HdChangeTracker::DirtyInstanceIndex);
    provider->readyFailure = true;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    if (!provider->last || provider->last->Ready()) return 1;
    pixels = static_cast<const float *>(buffer->Map());
    bool const sameIdRejectedIdentical = FinitePixels(pixels, pixelWords) &&
        std::equal(accepted.begin(), accepted.end(), pixels);
    if (pixels) buffer->Unmap();
    if (!sameIdRejectedIdentical || !rprim ||
        rprim->GetInstancerId() != instancerA) return 1;

    provider->readyFailure = false;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    pixels = static_cast<const float *>(buffer->Map());
    if (!pixels) return 1;
    bool const sameIdRecoveryChanged = FinitePixels(pixels, pixelWords) &&
        !std::equal(accepted.begin(), accepted.end(), pixels);
    size_t sameIdRecoveryLit = 0;
    for (unsigned i = 0; i != buffer->GetWidth() * buffer->GetHeight(); ++i)
        sameIdRecoveryLit += pixels[i * 4 + 3] > .001f;
    if (sameIdRecoveryChanged) {
        accepted.assign(pixels, pixels + pixelWords);
    }
    buffer->Unmap();
    if (!sameIdRecoveryChanged || !sameIdRecoveryLit || !rprim ||
        rprim->GetInstancerId() != instancerA) return 1;
    lit = sameIdRecoveryLit;

    // Empty instance primvars are a valid zero-instance candidate.  A
    // Ready(false) attempt must retain the already accepted A BARs, while a
    // successful candidate clears both the primvar and index BAR slots so its
    // indirect count reaches zero rather than drawing a sentinel instance.
    delegate.SetInstancerProperties(
        instancerA, VtIntArray(), VtVec3fArray(), VtVec4fArray(),
        VtVec3fArray());
    index->GetChangeTracker().MarkInstancerDirty(
        instancerA, HdChangeTracker::DirtyPrimvar |
            HdChangeTracker::DirtyInstanceIndex);
    provider->readyFailure = true;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    if (!provider->last || provider->last->Ready() || !acceptedInstancer ||
        !acceptedInstancer->GetInstanceIndices(path).empty()) return 1;
    pixels = static_cast<const float *>(buffer->Map());
    bool const emptyRejectedRetained = FinitePixels(pixels, pixelWords) &&
        std::equal(accepted.begin(), accepted.end(), pixels);
    if (pixels) buffer->Unmap();
    if (!emptyRejectedRetained || !rprim ||
        rprim->GetInstancerId() != instancerA) return 1;

    provider->readyFailure = false;
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    pixels = static_cast<const float *>(buffer->Map());
    if (!pixels) return 1;
    size_t emptyInstanceLit = 0;
    for (unsigned i = 0; i != buffer->GetWidth() * buffer->GetHeight(); ++i)
        emptyInstanceLit += pixels[i * 4 + 3] > .001f;
    bool const emptyInstanceFinite = FinitePixels(pixels, pixelWords);
    buffer->Unmap();
    if (!emptyInstanceFinite || emptyInstanceLit != 0 || !rprim ||
        rprim->GetInstancerId() != instancerA) return 1;

    delegate.SetInstancerProperties(
        instancerA, VtIntArray{0}, VtVec3fArray{GfVec3f(1)},
        VtVec4fArray{GfVec4f(0)}, VtVec3fArray{GfVec3f(.25f, 0, 0)});
    index->GetChangeTracker().MarkInstancerDirty(
        instancerA, HdChangeTracker::DirtyPrimvar |
            HdChangeTracker::DirtyInstanceIndex);
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    pixels = static_cast<const float *>(buffer->Map());
    if (!pixels) return 1;
    size_t emptyRecoveryLit = 0;
    for (unsigned i = 0; i != buffer->GetWidth() * buffer->GetHeight(); ++i)
        emptyRecoveryLit += pixels[i * 4 + 3] > .001f;
    bool const emptyRecoveryFinite = FinitePixels(pixels, pixelWords);
    buffer->Unmap();
    if (!emptyRecoveryFinite || !emptyRecoveryLit || !rprim ||
        rprim->GetInstancerId() != instancerA) return 1;
    lit = emptyRecoveryLit;

    // An in-place rejected candidate preserves the current rprim/bundle.
    // A different source generation proves acceptance would have changed it.
    auto changed = usdGenTest::MakeCudaGlFixture(.2f, 1, .25f);
    if (!changed) return 77;
    if (changed->Identity().generation != 1) return 1;
    provider->generation = changed;
    provider->reject = true;
    Presentation rejectedPresentation = acceptedPresentation;
    rejectedPresentation.xform.SetTranslate(GfVec3d(.45, 0, 0));
    // Keep it visible so the identical old frame proves that a changed
    // transform cannot leak when Prepare rejects the candidate.
    rejectedPresentation.visible = true;
    rejectedPresentation.materialPath = SdfPath("/Looks/Rejected");
    rejectedPresentation.instancerPath = instancerB;
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
        currentMaterial != acceptedPresentation.materialPath ||
        rprim->GetInstancerId() != instancerA) {
        std::fprintf(stderr,
            "rejected candidate leaked presentation: prepares=%d retained=%zu lit=%zu "
            "identical=%d rprim=%d oldMaterial=%s currentMaterial=%s\n",
            provider->prepares, retained, lit, int(identical), int(bool(rprim)),
            acceptedPresentation.materialPath.GetText(), currentMaterial.GetText());
        return 1;
    }

    // A bundle can pass Prepare and still fail only after the registry commits
    // its work.  This candidate changes visibility and material while keeping
    // the translated transform; all accepted presentation state must remain.
    provider->reject = false;
    provider->readyFailure = true;
    rejectedPresentation.visible = false;
    primData->Set(MakePrimDataSource(provider, rejectedPresentation));
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    std::fprintf(stderr, "cuda native phase ready-false Execute begin\n");
    engine.Execute(index.get(), &tasks);
    std::fprintf(stderr, "cuda native phase ready-false Execute end\n");
    if (glGetError() != GL_NO_ERROR || !provider->last ||
        provider->last->Ready()) return 1;
    pixels = static_cast<const float *>(buffer->Map());
    retained = 0;
    if (pixels) for (unsigned i = 0; i != buffer->GetWidth() * buffer->GetHeight(); ++i)
        retained += pixels[i * 4 + 3] > .001f;
    bool const readyIdentical = FinitePixels(pixels, pixelWords) &&
        std::equal(accepted.begin(), accepted.end(), pixels);
    if (pixels) buffer->Unmap();
    if (retained != lit || !readyIdentical || !rprim ||
        rprim->GetMaterialId() != acceptedPresentation.materialPath ||
        rprim->GetInstancerId() != instancerA) {
        std::fprintf(stderr,
            "ready-false candidate leaked presentation: retained=%zu lit=%zu "
            "identical=%d material=%s\n", retained, lit, int(readyIdentical),
            rprim ? rprim->GetMaterialId().GetText() : "<none>");
        return 1;
    }

    int const preparesBeforeRecovery = provider->prepares;
    provider->readyFailure = false;
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
        rprim->GetMaterialId() != rejectedPresentation.materialPath ||
        rprim->GetInstancerId() != instancerB) return 1;

    // The recovered B hierarchy has a distinct instance translation.  Its
    // visible follow-up proves that the accepted instance index/primvar BARs
    // render the retained curve, rather than merely updating rprim identity.
    rejectedPresentation.visible = true;
    primData->Set(MakePrimDataSource(provider, rejectedPresentation));
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    pixels = static_cast<const float *>(buffer->Map());
    if (!pixels) return 1;
    size_t visibleBRecoveryLit = 0;
    for (unsigned i = 0; i != buffer->GetWidth() * buffer->GetHeight(); ++i)
        visibleBRecoveryLit += pixels[i * 4 + 3] > .001f;
    bool const visibleBRecoveryChanged = FinitePixels(pixels, pixelWords) &&
        !std::equal(accepted.begin(), accepted.end(), pixels);
    buffer->Unmap();
    if (!visibleBRecoveryLit || !visibleBRecoveryChanged || !rprim ||
        rprim->GetInstancerId() != instancerB) return 1;

    // B is intentionally nested under instancerRoot.  Its flattened tuple
    // contains global/B/root indices, proving the accepted GPU path staged
    // both hierarchy levels rather than only the leaf identity.
    HdStInstancer * const nestedInstancer =
        static_cast<HdStInstancer *>(index->GetInstancer(instancerB));
    VtIntArray const nestedInstanceIndices = nestedInstancer
        ? nestedInstancer->GetInstanceIndices(path) : VtIntArray();
    if (nestedInstanceIndices.size() != 3 ||
        nestedInstanceIndices[0] != 0 || nestedInstanceIndices[1] != 0 ||
        nestedInstanceIndices[2] != 0) return 1;

    // Removing a GPU provider must drop the accepted GPU dependency before
    // ordinary Sync re-admits the same authored instancer identity.
    primData->Set(MakePrimDataSource(HdDataSourceBase::Handle(),
                                     rejectedPresentation));
    source->DirtyPrims({{path, HdDataSourceLocatorSet(HdDataSourceLocator())}});
    engine.Execute(index.get(), &tasks);
    if (!rprim || rprim->GetInstancerId() != instancerB) return 1;
    std::printf("CUDA native retained BasisCurves: PASS (%zu pixels)\n", lit);
    return 0;
}
