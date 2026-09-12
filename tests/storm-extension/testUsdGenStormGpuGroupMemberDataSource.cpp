// Renderer-neutral retained datasource coverage for an already-ready group
// member. This uses the real group factory; no provider constructor or
// readiness callback is bypassed.
#include "pxr/pxr.h"

#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/dependencySchema.h"
#include "pxr/imaging/hd/extentSchema.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupMemberDataSource.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"

#include <cstdio>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int failures = 0;
#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; \
} } while (false)

class _Range final : public HdStBufferArrayRange {
public:
    _Range() : HdStBufferArrayRange(nullptr) {}
    bool IsValid() const override { return true; }
    bool IsAssigned() const override { return true; }
    bool IsImmutable() const override { return true; }
    bool RequiresStaging() const override { return false; }
    bool Resize(int) override { return false; }
    void CopyData(HdBufferSourceSharedPtr const&) override {}
    VtValue ReadData(TfToken const&) const override { return {}; }
    int GetElementOffset() const override { return 0; }
    int GetByteOffset(TfToken const&) const override { return 0; }
    size_t GetNumElements() const override { return 1; }
    size_t GetVersion() const override { return 1; }
    void IncrementVersion() override {}
    size_t GetMaxNumElements() const override { return 1; }
    HdBufferArrayUsageHint GetUsageHint() const override { return 0; }
    void SetBufferArray(HdBufferArray*) override {}
    void DebugDump(std::ostream&) const override {}
    HdStBufferResourceSharedPtr GetResource() const override { return {}; }
    HdStBufferResourceSharedPtr GetResource(TfToken const&) override { return {}; }
    HdStBufferResourceNamedList const& GetResources() const override {
        static HdStBufferResourceNamedList const empty;
        return empty;
    }
protected:
    void const* _GetAggregation() const override { return this; }
};

class _Provider final : public HdStBasisCurvesGpuDataSource {
public:
    HD_DECLARE_DATASOURCE(_Provider);
    HdStBasisCurvesGpuBundleSharedPtr Prepare(
        HdStResourceRegistry*, HdStBasisCurvesGpuPrepareRequest const&) override {
        return {};
    }
};

GfBBox3d _Bounds(double x) {
    GfMatrix4d matrix(1.0);
    matrix.SetTranslate(GfVec3d(x, 2.0, 3.0));
    return GfBBox3d(GfRange3d(GfVec3d(-1.0, -2.0, -3.0),
                               GfVec3d(4.0, 5.0, 6.0)), matrix);
}

HdStBasisCurvesGpuBundleSharedPtr _Bundle(
    uint64_t generation, GfBBox3d const& bounds) {
    auto result = std::make_shared<HdStBasisCurvesGpuBundle>();
    result->generation = generation;
    result->curveType = TfToken("cubic");
    result->curveBasis = TfToken("bspline");
    result->curveWrap = TfToken("pinned");
    result->bounds = bounds;
    result->vertexRange = std::make_shared<_Range>();
    for (HdStBasisCurvesGpuTopologyMode mode : {
             HdStBasisCurvesGpuTopologyMode::Curves,
             HdStBasisCurvesGpuTopologyMode::Hull,
             HdStBasisCurvesGpuTopologyMode::Points}) {
        HdStBasisCurvesGpuTopologyRange range;
        range.mode = mode;
        range.topologyRange = std::make_shared<_Range>();
        range.drawCountRange = std::make_shared<_Range>();
        result->topologyRanges.push_back(std::move(range));
    }
    result->ready = [] { return true; };
    return result;
}

HdStBasisCurvesGpuGroupCandidate _Candidate(GfBBox3d const& bounds) {
    HdStBasisCurvesGpuGroupCandidate candidate;
    candidate.groupPath = SdfPath("/Groom/__usdGenRender");
    candidate.ticket = 9;
    candidate.generation = 7;
    HdStBasisCurvesGpuGroupMember member;
    member.id = 3;
    member.rprimPath = SdfPath("/Groom/__usdGenRender/tile_3");
    member.presentation.curveType = TfToken("cubic");
    member.presentation.curveBasis = TfToken("bspline");
    member.presentation.curveWrap = TfToken("pinned");
    member.presentation.bounds = bounds;
    member.presentation.refineLevel = 4;
    member.presentation.purpose = TfToken("render");
    member.presentation.visibility = TfToken("invisible");
    member.presentation.materialPath = SdfPath("/Looks/Hair");
    member.presentation.materialPurpose = TfToken("allPurpose");
    member.presentation.primOrigin = SdfPath("/Groom/Source");
    member.presentation.dependencySurface = SdfPath("/Scalp");
    member.provider = _Provider::New();
    candidate.members.push_back(std::move(member));
    return candidate;
}

void _TestMemberDataSource() {
    GfBBox3d const bounds = _Bounds(7.0);
    HdStBasisCurvesGpuGroupCandidate candidate = _Candidate(bounds);
    CHECK(HdStValidateBasisCurvesGpuGroupCandidate(candidate));
    auto owner = std::make_shared<int>(1);
    HdStResourceRegistry* const registry =
        reinterpret_cast<HdStResourceRegistry*>(uintptr_t(0x91));
    auto registryLease = std::shared_ptr<HdStResourceRegistry const>(owner, registry);
    auto identity = std::make_shared<HdStBasisCurvesGpuRegistryIdentity>(
        registry, 91, std::weak_ptr<void const>(registryLease));
    auto result = HdStMakeBasisCurvesGpuGroupReadyResult(
        candidate, identity, {{3, _Bundle(candidate.generation, bounds)}});
    CHECK(result.status == HdStBasisCurvesGpuGroupResultStatus::Ready &&
          result.providers.size() == 1);
    auto ready = result.providers.front();
    CHECK(ready && ready->GetGeneration() == candidate.generation);
    auto const presentation = candidate.members.front().presentation;
    auto data = HdStBuildBasisCurvesGpuGroupMemberDataSource(
        presentation, ready, candidate.generation);
    CHECK(data && !data->Get(TfToken("primvars")) &&
          HdStBasisCurvesGpuReadyProvider::Cast(
              data->Get(TfToken("hdStBasisCurvesGpu"))) == ready);

    auto curves = HdBasisCurvesSchema::GetFromParent(data);
    auto topology = curves.GetTopology();
    auto extent = HdExtentSchema::GetFromParent(data);
    auto xform = HdXformSchema::GetFromParent(data);
    auto visibility = HdVisibilitySchema::GetFromParent(data);
    auto generation = HdTypedSampledDataSource<uint64_t>::Cast(
        data->Get(TfToken("generation")));
    CHECK(curves.IsDefined() && topology.IsDefined() && topology.GetType() &&
          topology.GetBasis() && topology.GetWrap() &&
          topology.GetType()->GetTypedValue(0) == presentation.curveType &&
          topology.GetBasis()->GetTypedValue(0) == presentation.curveBasis &&
          topology.GetWrap()->GetTypedValue(0) == presentation.curveWrap &&
          !topology.GetCurveVertexCounts() && !topology.GetCurveIndices());
    CHECK(extent.IsDefined() && extent.GetMin() && extent.GetMax() &&
          extent.GetMin()->GetTypedValue(0) == bounds.GetRange().GetMin() &&
          extent.GetMax()->GetTypedValue(0) == bounds.GetRange().GetMax());
    CHECK(xform.IsDefined() && xform.GetMatrix() && xform.GetResetXformStack() &&
          xform.GetMatrix()->GetTypedValue(0) == bounds.GetMatrix() &&
          xform.GetResetXformStack()->GetTypedValue(0));
    CHECK(visibility.IsDefined() && visibility.GetVisibility() &&
          !visibility.GetVisibility()->GetTypedValue(0) && generation &&
          generation->GetTypedValue(0) == uint64_t(7));

    auto defaultBinding = HdMaterialBindingsSchema::GetFromParent(data)
        .GetMaterialBinding().GetPath();
    CHECK(defaultBinding && defaultBinding->GetTypedValue(0) == presentation.materialPath);
    auto origin = HdContainerDataSource::Cast(data->Get(TfToken("primOrigin")));
    auto dependencies = HdContainerDataSource::Cast(data->Get(TfToken("__dependencies")));
    auto dependency = HdContainerDataSource::Cast(
        dependencies ? dependencies->Get(TfToken("usdGenSurface")) : nullptr);
    HdDependencySchema dependencySchema(dependency);
    CHECK(origin && HdPathDataSource::Cast(origin->Get(TfToken("scenePath"))) &&
          HdPathDataSource::Cast(origin->Get(TfToken("scenePath")))->GetTypedValue(0) ==
              presentation.primOrigin && dependency &&
          dependencySchema.GetDependedOnPrimPath()->GetTypedValue(0) ==
              presentation.dependencySurface &&
          dependencySchema.GetDependedOnDataSourceLocator()->GetTypedValue(0) ==
              HdDataSourceLocator(TfToken("primvars"), TfToken("points")) &&
          dependencySchema.GetAffectedDataSourceLocator()->GetTypedValue(0) ==
              HdDataSourceLocator(TfToken("hdStBasisCurvesGpu")));

    auto custom = presentation;
    custom.materialPurpose = TfToken("custom");
    auto customData = HdStBuildBasisCurvesGpuGroupMemberDataSource(
        custom, ready, candidate.generation);
    auto customBindings = HdMaterialBindingsSchema::GetFromParent(customData);
    auto customPath = customBindings.GetMaterialBinding(custom.materialPurpose).GetPath();
    CHECK(customData && customPath &&
          customPath->GetTypedValue(0) == custom.materialPath &&
          !customBindings.GetMaterialBinding().GetPath());
    CHECK(!HdStBuildBasisCurvesGpuGroupMemberDataSource(
        presentation, ready, candidate.generation + 1));

    auto changed = presentation;
    changed.curveBasis = TfToken("catmullRom");
    changed.bounds = _Bounds(-9.0);
    auto changedData = HdStBuildBasisCurvesGpuGroupMemberDataSource(
        changed, ready, candidate.generation);
    CHECK(changedData && topology.GetBasis()->GetTypedValue(0) == TfToken("bspline") &&
          extent.GetMin()->GetTypedValue(0) == bounds.GetRange().GetMin() &&
          xform.GetMatrix()->GetTypedValue(0) == bounds.GetMatrix());

    std::weak_ptr<HdStBasisCurvesGpuReadyProvider> weakReady = ready;
    ready.reset();
    result.providers.clear();
    CHECK(!weakReady.expired());
    changedData.reset();
    customData.reset();
    data.reset();
    CHECK(weakReady.expired());
}

} // anonymous namespace

int main() {
    _TestMemberDataSource();
    return failures == 0 ? 0 : 1;
}
