// Registry-built live Groom -> private staging -> EGL acceptance regression.
// CMake registration is intentionally owned by the integration coordinator.
#include "eglctx.h"

#include "usdGen/opRegistry.h"
#include "usdGenImaging/groomSceneIndexPlugin.h"

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
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"
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

    // This call is the capability test: registry inputArgs supplies GL, so
    // Groom may expose the private raw control and the private staging plugin
    // is present in the same actual renderer branch.
    auto chain = HdSceneIndexPluginRegistry::GetInstance().AppendSceneIndicesForRenderer(
        "GL", indices.finalSceneIndex, "cuda-groom-egl");
    auto* groom = _Find<UsdGenGroomSceneIndex>(chain);
    auto* staging = _Find<HdStBasisCurvesGpuGroupStagingSceneIndex>(chain);
    CHECK(groom && staging);
    if (!groom || !staging) return 1;
    groom->Synchronize();
    SdfPath const scope("/Character/Groom/hair/__usdGenRender");
    auto control = _Control(*groom, scope);
    CHECK(control && control->GetCandidate() && control->GetCandidate()->ownsSubtree &&
          !control->GetCandidate()->members.empty());
    if (!control || !control->GetCandidate() || control->GetCandidate()->members.empty()) return 1;
    CHECK(groom->GetChildPrimPaths(scope).empty());
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
    CHECK(width && width.Set(.12f)); indices.stageSceneIndex->ApplyPendingUpdates(); groom->Synchronize();
    _Execute(engine, *index, &tasks, collection); staging->Poll(); _Execute(engine, *index, &tasks, collection);
    size_t changedLit = 0; auto after = _Pixels(buffer, &changedLit);
    CHECK(!after.empty() && changedLit != 0 && after != before);
    CHECK(errors.IsClean());
    return failures ? 1 : 0;
}
