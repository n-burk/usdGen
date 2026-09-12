// Registry-built live Groom -> private staging -> EGL acceptance regression.
// CMake registration is intentionally owned by the integration coordinator.
#include "eglctx.h"

#include "usdGen/opRegistry.h"
#include "usdGenImaging/groomSceneIndexPlugin.h"
#include "usdGenImaging/usdGenImagingSession.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/engine.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/rprimCollection.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupController.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupStagingSceneIndex.h"
#include "pxr/imaging/hdSt/renderDelegate.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgiGL/hgi.h"
#include "pxr/imaging/hdx/renderSetupTask.h"
#include "pxr/imaging/hdx/renderTask.h"
#include "pxr/imaging/hdx/unitTestDelegate.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (false)

template <class T>
T* _Find(HdSceneIndexBaseRefPtr const& root) {
    std::vector<HdSceneIndexBaseRefPtr> pending{root};
    std::set<HdSceneIndexBase const*> seen;
    for (size_t i = 0; i != pending.size(); ++i) {
        auto const& node = pending[i];
        if (!node || !seen.insert(node.operator->()).second) continue;
        if (auto* found = dynamic_cast<T*>(node.operator->())) return found;
        if (auto* filter = dynamic_cast<HdFilteringSceneIndexBase*>(node.operator->()))
            for (auto const& input : filter->GetInputScenes()) pending.push_back(input);
    }
    return nullptr;
}

bool _Finite(float const* pixels, size_t count, size_t* lit) {
    *lit = 0;
    if (!pixels && count) return false;
    for (size_t i = 0; i != count; ++i) {
        if (!std::isfinite(pixels[i])) return false;
        if (i % 4 == 3 && pixels[i] > .001f) ++*lit;
    }
    return true;
}

std::vector<float> _Pixels(HdRenderBuffer* buffer, size_t* lit) {
    std::vector<float> result;
    if (!buffer || buffer->GetFormat() != HdFormatFloat32Vec4) return result;
    size_t const words = size_t(buffer->GetWidth()) * buffer->GetHeight() * 4;
    float const* pixels = static_cast<float const*>(buffer->Map());
    if (!pixels || !_Finite(pixels, words, lit)) { if (pixels) buffer->Unmap(); return result; }
    result.assign(pixels, pixels + words); buffer->Unmap(); return result;
}

void _Execute(HdEngine& engine, HdRenderIndex& index, HdTaskSharedPtrVector* tasks,
              HdRprimCollection const& collection) {
    index.EnqueueCollectionToSync(collection); engine.Execute(&index, tasks);
}

HdStBasisCurvesGpuGroupDataSourceHandle _Control(HdSceneIndexBase const& index,
                                                   SdfPath const& scope) {
    HdSceneIndexPrim const prim = index.GetPrim(scope);
    return HdStBasisCurvesGpuGroupDataSource::Cast(prim.dataSource ?
        prim.dataSource->Get(HdStGetBasisCurvesGpuGroupDataSourceToken()) : nullptr);
}
} // namespace

int main() {
    if (!eglctx::MakeHeadlessGLContext()) return 77;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) return 77;
    CHECK(cudaSetDevice(0) == cudaSuccess && cudaFree(nullptr) == cudaSuccess);
    TfErrorMark errors;
    auto stage = UsdStage::Open(std::string(USDGEN_TEST_SOURCE_DIR) +
        "/plan/examples/cuda-length-network.usda");
    CHECK(stage);
    UsdImagingCreateSceneIndicesInfo info; info.stage = stage;
    auto indices = UsdImagingCreateSceneIndices(info);
    indices.stageSceneIndex->SetTime(UsdTimeCode(1));
    indices.stageSceneIndex->ApplyPendingUpdates();
    usdGen::usdGenRegisterM1Operators();

    // Both renderer chains deliberately use the same authored session id.
    // CUDA generations must still receive distinct renderer-local sessions.
    UsdPrim groomPrim = stage->GetPrimAtPath(SdfPath("/Character/Groom"));
    UsdAttribute sessionId = groomPrim.CreateAttribute(
        TfToken("usdGen:sessionId"), SdfValueTypeNames->String,
        /*custom=*/false, SdfVariabilityUniform);
    CHECK(sessionId && sessionId.Set(std::string("shared-groom-render-session")));
    indices.stageSceneIndex->ApplyPendingUpdates();

    // Registry inputArgs supplies GL, so Groom may expose the private raw
    // control and staging is present in this actual renderer branch.  Create
    // two renderer branches with the exact same authored session id: their
    // CUDA keys must nevertheless be renderer-local.
    auto &registry = HdSceneIndexPluginRegistry::GetInstance();
    auto chain = registry.AppendSceneIndicesForRenderer(
        "GL", indices.finalSceneIndex, "cuda-groom-egl-a");
    auto secondChain = registry.AppendSceneIndicesForRenderer(
        "GL", indices.finalSceneIndex, "cuda-groom-egl-b");
    auto* groom = _Find<UsdGenGroomSceneIndex>(chain);
    auto* staging = _Find<HdStBasisCurvesGpuGroupStagingSceneIndex>(chain);
    auto* secondGroom = _Find<UsdGenGroomSceneIndex>(secondChain);
    CHECK(groom && staging && secondGroom);
    if (!groom || !staging || !secondGroom) return 1;
    groom->Synchronize();
    secondGroom->Synchronize();
    SdfPath const scope("/Character/Groom/hair/__usdGenRender");
    auto control = _Control(*groom, scope);
    auto secondControl = _Control(*secondGroom, scope);
    CHECK(control && control->GetCandidate() && control->GetCandidate()->ownsSubtree &&
          !control->GetCandidate()->members.empty() && secondControl &&
          secondControl->GetCandidate() && secondControl != control &&
          secondControl->GetCandidate() != control->GetCandidate());
    if (!control || !control->GetCandidate() || control->GetCandidate()->members.empty() ||
        !secondControl || !secondControl->GetCandidate()) return 1;
    CHECK(groom->GetChildPrimPaths(scope).empty());
    CHECK(secondGroom->GetChildPrimPaths(scope).empty());

    size_t rendererLocalSessions = 0;
    for (auto const& session : usdGenImaging::UsdGenSessionStore::GetInstance().LiveSessions()) {
        auto const& key = session->Key();
        if (key.sessionId == "shared-groom-render-session" &&
            key.groomRoot == SdfPath("/Character/Groom") && key.rendererLocal) {
            ++rendererLocalSessions;
        }
    }
    CHECK(rendererLocalSessions == 2);

    auto const firstCandidate = control->GetCandidate();
    auto const secondCandidate = secondControl->GetCandidate();
    SdfPath const rprim = control ? control->GetCandidate()->members.front().rprimPath : SdfPath();

    GarchGLApiLoad(); HgiGL hgi;
    HdDriver driver{HgiTokens->renderDriver, VtValue(static_cast<Hgi*>(&hgi))};
    HdStRenderDelegate delegate;
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {&driver}));
    CHECK(index); if (!index) return 1;
    index->InsertSceneIndex(chain, SdfPath::AbsoluteRootPath());
    Hdx_UnitTestDelegate unit(index.get());
    unit.AddRenderSetupTask(SdfPath("/setup")); unit.AddRenderTask(SdfPath("/render"));
    auto aovs = unit.AddAovBindings(GfVec2i(128), false);
    aovs[0].clearValue = VtValue(GfVec4f(0));
    unit.UpdateRenderBuffer(aovs[0].renderBufferId,
        HdRenderBufferDescriptor(GfVec3i(128, 128, 1), HdFormatFloat32Vec4, false));
    auto params = unit.GetTaskParam(SdfPath("/setup"), HdTokens->params).Get<HdxRenderTaskParams>();
    params.viewport = GfVec4d(0, 0, 128, 128); params.aovBindings = aovs;
    unit.SetTaskParam(SdfPath("/setup"), HdTokens->params, VtValue(params));
    HdTaskSharedPtrVector tasks{index->GetTask(SdfPath("/setup")), index->GetTask(SdfPath("/render"))};
    HdRprimCollection collection(TfToken("liveGroom"), HdReprSelector(HdReprTokens->smoothHull));
    collection.SetRootPath(scope);
    HdEngine engine;
    _Execute(engine, *index, &tasks, collection); staging->Poll(); _Execute(engine, *index, &tasks, collection);
    auto* buffer = dynamic_cast<HdRenderBuffer*>(index->GetBprim(HdPrimTypeTokens->renderBuffer,
        aovs[0].renderBufferId));
    size_t lit = 0; auto before = _Pixels(buffer, &lit);
    CHECK(!before.empty() && lit != 0 && !rprim.IsEmpty() && index->GetRprim(rprim));

    UsdAttribute width = stage->GetAttributeAtPath(
        SdfPath("/Character/Groom/hair/Ops/width.usdGen:width"));
    CHECK(width && width.Set(.12f)); indices.stageSceneIndex->ApplyPendingUpdates();
    groom->Synchronize(); secondGroom->Synchronize();
    auto changedControl = _Control(*groom, scope);
    auto changedSecondControl = _Control(*secondGroom, scope);
    CHECK(changedControl && changedControl->GetCandidate() && changedSecondControl &&
          changedSecondControl->GetCandidate() &&
          changedControl->GetCandidate()->ticket > firstCandidate->ticket &&
          changedSecondControl->GetCandidate()->ticket > secondCandidate->ticket &&
          changedControl->GetCandidate()->generation > firstCandidate->generation &&
          changedSecondControl->GetCandidate()->generation > secondCandidate->generation &&
          changedControl->GetCandidate() != changedSecondControl->GetCandidate());
    _Execute(engine, *index, &tasks, collection); staging->Poll(); _Execute(engine, *index, &tasks, collection);
    size_t changedLit = 0; auto after = _Pixels(buffer, &changedLit);
    CHECK(!after.empty() && changedLit != 0 && after != before);

    // The plugin is registered for all renderers, but private control
    // publication is a GL capability, not a header-presence side effect.
    auto nonGlChain = registry.AppendSceneIndicesForRenderer(
        "JsonMetadataOnly", indices.finalSceneIndex, "cuda-groom-non-gl");
    auto* nonGlGroom = _Find<UsdGenGroomSceneIndex>(nonGlChain);
    CHECK(nonGlGroom); if (nonGlGroom) {
        nonGlGroom->Synchronize();
        CHECK(!_Control(*nonGlGroom, scope));
    }

    // Append the loaded Groom plugin directly without registry inputArgs:
    // missing __rendererDisplayName must also retain the CPU-only route.
    auto noDisplayChain = registry.AppendSceneIndex(
        TfToken("UsdGenGroomSceneIndexPlugin"), indices.finalSceneIndex, nullptr,
        "cuda-groom-no-display");
    auto* noDisplayGroom = _Find<UsdGenGroomSceneIndex>(noDisplayChain);
    CHECK(noDisplayGroom); if (noDisplayGroom) {
        noDisplayGroom->Synchronize();
        CHECK(!_Control(*noDisplayGroom, scope));
    }
    CHECK(errors.IsClean());
    return failures ? 1 : 0;
}
