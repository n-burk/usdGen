// End-to-end CUDA group-control publication.  Unlike the bare-provider
// regression this exercises control -> staging -> HdSt controller -> Commit
// -> Poll -> cached ready proxy before a BasisCurves rprim is ever exposed.
#include "usdGenImaging/cudaBasisCurvesProvider.h"
#include "../cudaGlFixture.h"
#include "eglctx.h"

#include <cuda_runtime.h>

#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/engine.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/rprimCollection.h"
#include "pxr/imaging/hd/repr.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupController.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupStagingSceneIndex.h"
#include "pxr/imaging/hdSt/renderDelegate.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/imaging/hgiGL/hgi.h"
#include "pxr/imaging/hdx/renderSetupTask.h"
#include "pxr/imaging/hdx/renderTask.h"
#include "pxr/imaging/hdx/unitTestDelegate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <limits>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using usdGenImaging::UsdGenCudaBasisCurvesProvider;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (false)

class _Control final : public HdStBasisCurvesGpuGroupDataSource {
public:
    HD_DECLARE_DATASOURCE(_Control);
    static Handle Make(HdStBasisCurvesGpuGroupCandidateSharedPtr const &candidate) {
        return _Control::New(candidate, std::make_shared<HdStBasisCurvesGpuGroupMailbox>());
    }
    HdStBasisCurvesGpuGroupCandidateSharedPtr GetCandidate() const override { return _candidate; }
    HdStBasisCurvesGpuGroupMailboxSharedPtr GetMailbox() const override { return _mailbox; }
private:
    _Control(HdStBasisCurvesGpuGroupCandidateSharedPtr const &candidate,
             HdStBasisCurvesGpuGroupMailboxSharedPtr const &mailbox)
        : _candidate(candidate), _mailbox(mailbox) {}
    HdStBasisCurvesGpuGroupCandidateSharedPtr const _candidate;
    HdStBasisCurvesGpuGroupMailboxSharedPtr const _mailbox;
};

class _CudaProvider final : public HdStBasisCurvesGpuDataSource {
public:
    HD_DECLARE_DATASOURCE(_CudaProvider);
    _CudaProvider(std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation,
                  GfBBox3d bounds, bool failReady)
        : _generation(std::move(generation)), _bounds(bounds), _failReady(failReady) {}
    HdStBasisCurvesGpuBundleSharedPtr Prepare(
        HdStResourceRegistry *registry, HdStBasisCurvesGpuPrepareRequest const &request) override {
        ++prepares; modes = request.topologyModes;
        UsdGenCudaBasisCurvesProvider::CreateInfo info;
        info.generation = _generation; info.curveType = HdTokens->linear;
        info.curveBasis = HdTokens->linear; info.curveWrap = HdTokens->nonperiodic;
        info.conservativeBounds = _bounds;
        auto producer = std::make_shared<UsdGenCudaBasisCurvesProvider>(std::move(info));
        last = producer->Prepare(registry, request);
        if (_failReady && last) {
            auto failed = std::make_shared<HdStBasisCurvesGpuBundle>(*last);
            auto ready = failed->ready;
            failed->ready = [ready] { if (ready) (void)ready(); return false; };
            last = failed;
        }
        return last;
    }
    int prepares = 0;
    HdStBasisCurvesGpuTopologyModeVector modes;
    HdStBasisCurvesGpuBundleSharedPtr last;
private:
    std::shared_ptr<const usdGen::UsdGenDeviceGeneration> _generation;
    GfBBox3d _bounds;
    bool _failReady;
};

GfBBox3d _Bounds(double x) {
    GfMatrix4d m(1.0); m.SetTranslate(GfVec3d(x, 0, 0));
    return GfBBox3d(GfRange3d(GfVec3d(-1), GfVec3d(1)), m);
}

HdStBasisCurvesGpuGroupMember _Member(
    uint32_t id, SdfPath const &path, _CudaProvider::Handle const &provider,
    GfBBox3d const &bounds, SdfPath const &material, TfToken const &visibility = TfToken()) {
    HdStBasisCurvesGpuGroupMember m;
    m.id = id; m.rprimPath = path; m.provider = provider;
    m.presentation.curveType = HdTokens->linear;
    m.presentation.curveBasis = HdTokens->linear;
    m.presentation.curveWrap = HdTokens->nonperiodic;
    m.presentation.bounds = bounds; m.presentation.materialPath = material;
    m.presentation.visibility = visibility;
    return m;
}

HdStBasisCurvesGpuGroupCandidateSharedPtr _Candidate(
    uint64_t ticket, uint64_t generation, std::vector<HdStBasisCurvesGpuGroupMember> members) {
    auto c = std::make_shared<HdStBasisCurvesGpuGroupCandidate>();
    c->groupPath = SdfPath("/cudaGpuGroup"); c->ticket = ticket;
    c->generation = generation; c->members = std::move(members); return c;
}

HdContainerDataSourceHandle _ControlData(HdStBasisCurvesGpuGroupCandidateSharedPtr const &candidate) {
    HdDataSourceBaseHandle control = _Control::Make(candidate);
    TfToken const token = HdStGetBasisCurvesGpuGroupDataSourceToken();
    return HdRetainedContainerDataSource::New(1, &token, &control);
}

bool _Finite(float const *pixels, size_t count) {
    if (!pixels && count) return false;
    for (size_t i = 0; i != count; ++i) if (!std::isfinite(pixels[i])) return false;
    return true;
}

std::vector<float> _Pixels(HdRenderBuffer *buffer, size_t *lit) {
    std::vector<float> result;
    if (!buffer || buffer->GetFormat() != HdFormatFloat32Vec4) return result;
    size_t const words = size_t(buffer->GetWidth()) * buffer->GetHeight() * 4;
    auto const *p = static_cast<float const *>(buffer->Map());
    if (!p || !_Finite(p, words)) { if (p) buffer->Unmap(); return result; }
    result.assign(p, p + words); *lit = 0;
    for (size_t i = 0; i != words / 4; ++i) *lit += p[i * 4 + 3] > .001f;
    buffer->Unmap(); return result;
}

void _Execute(HdEngine *engine, HdRenderIndex *index, HdTaskSharedPtrVector *tasks,
              HdRprimCollection const &collection) {
    index->EnqueueCollectionToSync(collection); engine->Execute(index, tasks);
}

uint64_t _Generation(HdSceneIndexBaseRefPtr const &index, SdfPath const &path) {
    HdSceneIndexPrim const prim = index->GetPrim(path);
    auto value = HdTypedSampledDataSource<uint64_t>::Cast(
        prim.dataSource ? prim.dataSource->Get(TfToken("generation")) : nullptr);
    return value ? value->GetTypedValue(0.0f) : std::numeric_limits<uint64_t>::max();
}

bool _HasXform(HdSceneIndexBaseRefPtr const &index, SdfPath const &path,
               GfMatrix4d const &expected) {
    HdSceneIndexPrim const prim = index->GetPrim(path);
    auto xform = HdContainerDataSource::Cast(
        prim.dataSource ? prim.dataSource->Get(TfToken("xform")) : nullptr);
    auto matrix = HdTypedSampledDataSource<GfMatrix4d>::Cast(
        xform ? xform->Get(TfToken("matrix")) : nullptr);
    return matrix && matrix->GetTypedValue(0.0f) == expected;
}
} // namespace

int main() {
    TfErrorMark errors;
    if (!eglctx::MakeHeadlessGLContext()) return 77;
    int cudaDeviceCount = 0;
    if (cudaGetDeviceCount(&cudaDeviceCount) != cudaSuccess || cudaDeviceCount == 0) return 77;
    GarchGLApiLoad(); HgiGL hgi;
    HdDriver driver{HgiTokens->renderDriver, VtValue(static_cast<Hgi *>(&hgi))};
    auto g0 = usdGenTest::MakeCudaGlFixture(.08f, 0);
    auto g1 = usdGenTest::MakeCudaGlFixture(.20f, 1, .25f);
    if (!g0 || !g1) return 1;
    if (g0->Identity().generation != 0 || g1->Identity().generation != 1) return 1;
    SdfPath const a("/cudaGpuGroup/tile_a"), b("/cudaGpuGroup/tile_b"), c("/cudaGpuGroup/tile_c");
    auto a0 = _CudaProvider::New(g0, _Bounds(-.25), false);
    auto b0 = _CudaProvider::New(g0, _Bounds(.25), false);
    auto accepted = _Candidate(1, 0, {_Member(1, a, a0, _Bounds(-.25), SdfPath("/Looks/Accepted")),
        _Member(2, b, b0, _Bounds(.25), SdfPath("/Looks/Accepted"), TfToken("invisible"))});
    auto source = HdRetainedSceneIndex::New();
    source->AddPrims({{SdfPath("/cudaGpuGroup"), TfToken("scope"), _ControlData(accepted)}});
    HdSceneIndexBaseRefPtr wrapped = HdStBasisCurvesGpuGroupStagingSceneIndex::New(source);
    auto *staging = static_cast<HdStBasisCurvesGpuGroupStagingSceneIndex *>(wrapped.operator->());
    HdStRenderDelegate delegate; std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {&driver}));
    if (!index) return 77;
    index->InsertSceneIndex(wrapped, SdfPath::AbsoluteRootPath());
    Hdx_UnitTestDelegate unit(index.get()); unit.AddRenderSetupTask(SdfPath("/setup")); unit.AddRenderTask(SdfPath("/render"));
    auto aovs = unit.AddAovBindings(GfVec2i(128), false); aovs[0].clearValue = VtValue(GfVec4f(0));
    unit.UpdateRenderBuffer(aovs[0].renderBufferId, HdRenderBufferDescriptor(GfVec3i(128,128,1), HdFormatFloat32Vec4, false));
    auto params = unit.GetTaskParam(SdfPath("/setup"), HdTokens->params).Get<HdxRenderTaskParams>();
    params.viewport = GfVec4d(0,0,128,128); params.aovBindings = aovs;
    unit.SetTaskParam(SdfPath("/setup"), HdTokens->params, VtValue(params));
    HdTaskSharedPtrVector tasks{index->GetTask(SdfPath("/setup")), index->GetTask(SdfPath("/render"))};
    HdEngine engine; HdRprimCollection collection(TfToken("cudaGroup"), HdReprSelector(HdReprTokens->smoothHull));
    // First normal Sync/Commit prepares every member; Poll is deliberately
    // outside Sync and only then exposes accepted rprims to the next Execute.
    _Execute(&engine, index.get(), &tasks, collection); staging->Poll(); _Execute(&engine, index.get(), &tasks, collection);
    auto *buffer = dynamic_cast<HdRenderBuffer *>(index->GetBprim(HdPrimTypeTokens->renderBuffer, aovs[0].renderBufferId));
    size_t lit = 0; std::vector<float> baseline = _Pixels(buffer, &lit);
    CHECK(!baseline.empty() && lit != 0 && index->GetRprim(a) && index->GetRprim(b));
    HdRprim const *acceptedA = index->GetRprim(a);
    if (acceptedA) CHECK(acceptedA->GetMaterialId() == SdfPath("/Looks/Accepted"));
    CHECK(_Generation(wrapped, a) == 0 && _Generation(wrapped, b) == 0);
    CHECK(_HasXform(wrapped, a, _Bounds(-.25).GetMatrix()));
    HdStBasisCurvesGpuTopologyModeVector const modes{HdStBasisCurvesGpuTopologyMode::Curves, HdStBasisCurvesGpuTopologyMode::Hull, HdStBasisCurvesGpuTopologyMode::Points};
    CHECK(a0->prepares == 1 && b0->prepares == 1 && a0->modes == modes && b0->modes == modes);

    // One complete producer fails Ready.  Its geometry, xform, visibility,
    // material and membership must remain entirely invisible after Poll.
    auto aBad = _CudaProvider::New(g1, _Bounds(.5), false);
    auto cBad = _CudaProvider::New(g1, _Bounds(-.5), true);
    auto rejected = _Candidate(2, 1, {_Member(1, a, aBad, _Bounds(.5), SdfPath("/Looks/Rejected"), TfToken("invisible")),
        _Member(3, c, cBad, _Bounds(-.5), SdfPath("/Looks/Rejected"))});
    source->AddPrims({{SdfPath("/cudaGpuGroup"), TfToken("scope"), _ControlData(rejected)}});
    _Execute(&engine, index.get(), &tasks, collection); staging->Poll(); _Execute(&engine, index.get(), &tasks, collection);
    size_t retained = 0; std::vector<float> afterRejected = _Pixels(buffer, &retained);
    CHECK(afterRejected == baseline && retained == lit && index->GetRprim(a) && index->GetRprim(b) && !index->GetRprim(c));
    HdRprim const *retainedA = index->GetRprim(a);
    if (retainedA) CHECK(retainedA->GetMaterialId() == SdfPath("/Looks/Accepted"));
    CHECK(_Generation(wrapped, a) == 0 && _Generation(wrapped, b) == 0);
    CHECK(_HasXform(wrapped, a, _Bounds(-.25).GetMatrix()));
    CHECK(aBad->modes == modes && cBad->modes == modes);

    auto a1 = _CudaProvider::New(g1, _Bounds(.5), false);
    auto c1 = _CudaProvider::New(g1, _Bounds(-.5), false);
    auto recovered = _Candidate(3, 1, {_Member(1, a, a1, _Bounds(.5), SdfPath("/Looks/Recovered")),
        _Member(3, c, c1, _Bounds(-.5), SdfPath("/Looks/Recovered"), TfToken("invisible"))});
    source->AddPrims({{SdfPath("/cudaGpuGroup"), TfToken("scope"), _ControlData(recovered)}});
    _Execute(&engine, index.get(), &tasks, collection); staging->Poll(); _Execute(&engine, index.get(), &tasks, collection);
    size_t recoveredLit = 0; std::vector<float> recoveredPixels = _Pixels(buffer, &recoveredLit);
    CHECK(!recoveredPixels.empty() && recoveredPixels != baseline && index->GetRprim(a) && index->GetRprim(c) && !index->GetRprim(b));
    HdRprim const *recoveredA = index->GetRprim(a);
    if (recoveredA) CHECK(recoveredA->GetMaterialId() == SdfPath("/Looks/Recovered"));
    CHECK(_Generation(wrapped, a) == 1 && _Generation(wrapped, c) == 1);
    CHECK(_HasXform(wrapped, a, _Bounds(.5).GetMatrix()) && c1->modes == modes);

    auto empty = _Candidate(4, 1, {});
    source->AddPrims({{SdfPath("/cudaGpuGroup"), TfToken("scope"), _ControlData(empty)}});
    _Execute(&engine, index.get(), &tasks, collection); staging->Poll(); _Execute(&engine, index.get(), &tasks, collection);
    CHECK(!index->GetRprim(a) && !index->GetRprim(b) && !index->GetRprim(c));
    CHECK(errors.IsClean() && glGetError() == GL_NO_ERROR);
    return failures ? 1 : 0;
}
