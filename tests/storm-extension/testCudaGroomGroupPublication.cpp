// Real stage -> Groom CUDA ingress regression. The GPU path is explicitly
// enabled only for the private GL-group capability; ordinary/direct Groom
// construction keeps device admission disabled.
#include "usdGen/opRegistry.h"
#include "usdGenImaging/groomSceneIndexPlugin.h"
#include "usdGenImaging/usdGenGraphDescBuilder.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupController.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"

#include <cuda_runtime_api.h>

#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (false)

namespace {

SdfPath const kGroomPath("/Character/Groom");
SdfPath const kWidthPath("/Character/Groom/hair/Ops/width");

// The schema-backed UsdGenGroom intentionally resolves an absent backend to
// cuda.  For this display-transition test only, remove the mapped backend
// leaf from a real Hydra source to exercise the legacy CPU-reference lane.
// The filter never authors a fictitious "cpu" token.  It also removes the
// CUDA-only SeExpr width binding while preserving its literal fallback.
HdContainerDataSourceHandle
_Without(HdContainerDataSourceHandle const& source, TfToken const& omitted)
{
    if (!source) return {};
    TfTokenVector names;
    std::vector<HdDataSourceBaseHandle> values;
    for (TfToken const& name : source->GetNames()) {
        if (name == omitted) continue;
        if (HdDataSourceBaseHandle const value = source->Get(name)) {
            names.push_back(name);
            values.push_back(value);
        }
    }
    return HdRetainedContainerDataSource::New(names.size(), names.data(), values.data());
}

class _CpuReferenceLane final : public HdSingleInputFilteringSceneIndexBase {
public:
    static TfRefPtr<_CpuReferenceLane> New(HdSceneIndexBaseRefPtr const& input) {
        return TfCreateRefPtr(new _CpuReferenceLane(input));
    }
    // The next source dirty belongs to the CUDA replacement candidate.  Do
    // not let the CPU reference lane recook it first: this makes the
    // last-good snapshot retained through rejected CUDA admission observable.
    void HoldCpuSnapshotForCudaTransition() { _holdInputDirties = true; }
    void EnableCuda() {
        if (_cuda) return;
        _cuda = true;
        _holdInputDirties = false;
        HdDataSourceLocatorSet all;
        all.insert(HdDataSourceLocator());
        _SendPrimsDirtied({{kGroomPath, std::move(all)}});
    }
    HdSceneIndexPrim GetPrim(SdfPath const& path) const override {
        HdSceneIndexPrim result = _GetInputSceneIndex()->GetPrim(path);
        if (path == kGroomPath && !_cuda)
            result.dataSource = _Without(result.dataSource, TfToken("execution"));
        else if (path == kWidthPath)
            result.dataSource = _Without(result.dataSource, TfToken("expressionBindings"));
        return result;
    }
    SdfPathVector GetChildPrimPaths(SdfPath const& path) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }
protected:
    explicit _CpuReferenceLane(HdSceneIndexBaseRefPtr const& input)
        : HdSingleInputFilteringSceneIndexBase(input) {}
    void _PrimsAdded(HdSceneIndexBase const&,
                     HdSceneIndexObserver::AddedPrimEntries const& entries) override {
        _SendPrimsAdded(entries);
    }
    void _PrimsRemoved(HdSceneIndexBase const&,
                       HdSceneIndexObserver::RemovedPrimEntries const& entries) override {
        _SendPrimsRemoved(entries);
    }
    void _PrimsDirtied(HdSceneIndexBase const&,
                       HdSceneIndexObserver::DirtiedPrimEntries const& entries) override {
        if (_holdInputDirties) return;
        _SendPrimsDirtied(entries);
    }
private:
    bool _cuda = false;
    bool _holdInputDirties = false;
};

HdStBasisCurvesGpuGroupDataSourceHandle
_Control(HdSceneIndexBaseRefPtr const& index, SdfPath const& scope)
{
    HdSceneIndexPrim const prim = index->GetPrim(scope);
    return HdStBasisCurvesGpuGroupDataSource::Cast(
        prim.dataSource ? prim.dataSource->Get(
            HdStGetBasisCurvesGpuGroupDataSourceToken()) : nullptr);
}

bool
_MatchesCpuSnapshot(HdSceneIndexBaseRefPtr const& index, SdfPath const& scope,
                    SdfPathVector const& paths,
                    std::vector<HdContainerDataSourceHandle> const& dataSources)
{
    SdfPathVector const actualPaths = index->GetChildPrimPaths(scope);
    if (actualPaths != paths || paths.size() != dataSources.size()) return false;
    for (size_t i = 0; i != paths.size(); ++i) {
        if (index->GetPrim(paths[i]).dataSource != dataSources[i]) return false;
    }
    return true;
}

class _ScopeObserver final : public HdSceneIndexObserver {
public:
    explicit _ScopeObserver(SdfPath scope) : _scope(std::move(scope)) {}
    void PrimsAdded(HdSceneIndexBase const&, AddedPrimEntries const& entries) override {
        for (auto const& entry : entries) if (entry.primPath == _scope) ++added;
    }
    void PrimsRemoved(HdSceneIndexBase const&, RemovedPrimEntries const& entries) override {
        for (auto const& entry : entries) if (entry.primPath == _scope) ++removed;
    }
    void PrimsDirtied(HdSceneIndexBase const&, DirtiedPrimEntries const&) override {}
    void PrimsRenamed(HdSceneIndexBase const&, RenamedPrimEntries const&) override {}
    unsigned added = 0;
    unsigned removed = 0;
private:
    SdfPath const _scope;
};

} // namespace

int main()
{
    int deviceCount = 0;
    if (cudaGetDeviceCount(&deviceCount) != cudaSuccess || deviceCount == 0)
        return 77;
    CHECK(cudaSetDevice(0) == cudaSuccess);
    CHECK(cudaFree(nullptr) == cudaSuccess);

    TfErrorMark errors;
    auto stage = UsdStage::Open(std::string(USDGEN_TEST_SOURCE_DIR) +
        "/plan/examples/cuda-length-network.usda");
    CHECK(stage);
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    auto indices = UsdImagingCreateSceneIndices(info);
    indices.stageSceneIndex->SetTime(UsdTimeCode(1));
    indices.stageSceneIndex->ApplyPendingUpdates();

    // This is the production Hydra capture path, not a helper-built desc.
    auto desc = usdGenImaging::BuildGraphDescFromHydra(
        *indices.finalSceneIndex, SdfPath("/Character/Groom/hair"));
    CHECK(desc.validationErrors.empty());
    CHECK(desc.executionBackend == usdGen::UsdGenExecutionBackend::Cuda);
    CHECK(desc.nodes.size() == 3 &&
          desc.nodes[0].type == TfToken("UsdGenCurveSource") &&
          desc.nodes[1].type == TfToken("UsdGenLength") &&
          desc.nodes[2].type == TfToken("UsdGenWidth"));

    usdGen::usdGenRegisterM1Operators();
    int const renderInstance = 9041;
    auto groom = UsdGenGroomSceneIndex::New(
        indices.finalSceneIndex, renderInstance, /* enableDeviceGroupIngress = */ true);
    auto *owner = dynamic_cast<UsdGenGroomSceneIndex *>(groom.operator->());
    CHECK(owner);
    owner->Synchronize();

    SdfPath const renderPath("/Character/Groom/hair/__usdGenRender");
    auto const render = groom->GetPrim(renderPath);
    CHECK(render.primType == TfToken("scope") && render.dataSource);
    auto const control = HdStBasisCurvesGpuGroupDataSource::Cast(
        render.dataSource->Get(HdStGetBasisCurvesGpuGroupDataSourceToken()));

    // Raw ingress exposes control only.  Candidate members become rprims
    // exclusively after the private renderer staging filter acknowledges the
    // complete group, so no raw basisCurves may appear at this boundary.
    CHECK(control);
    auto const candidate = control->GetCandidate();
    CHECK(candidate && candidate->groupPath == renderPath && candidate->ticket != 0 &&
          candidate->generation == 0 && candidate->ownsSubtree && !candidate->members.empty());
    CHECK(groom->GetChildPrimPaths(renderPath).empty());

    // Use the real stage/Hydra source behind an explicit test-only filtering
    // seam to start in CpuReference.  No production backend policy changes:
    // the seam represents an absent backend leaf and a literal Width input.
    auto referenceInput = _CpuReferenceLane::New(indices.finalSceneIndex);
    auto reference = UsdGenGroomSceneIndex::New(
        referenceInput, renderInstance + 1, /* enableDeviceGroupIngress = */ true);
    auto *referenceOwner = dynamic_cast<UsdGenGroomSceneIndex *>(reference.operator->());
    CHECK(referenceOwner);
    referenceOwner->Synchronize();
    CHECK(!_Control(reference, renderPath));
    SdfPathVector const cpuTiles = reference->GetChildPrimPaths(renderPath);
    CHECK(!cpuTiles.empty());
    std::vector<HdContainerDataSourceHandle> cpuTileData;
    cpuTileData.reserve(cpuTiles.size());
    for (SdfPath const& tile : cpuTiles)
        cpuTileData.push_back(reference->GetPrim(tile).dataSource);
    _ScopeObserver scopeNotices(renderPath);
    reference->AddObserver(TfCreateWeakPtr(&scopeNotices));

    // `linear` is a recognized BasisCurves type but is rejected by the CUDA
    // C3 admission path.  Flush the real stage update before synchronizing
    // Groom, then enable CUDA in the test seam.  The failed candidate must
    // retain the CPU snapshot at the same render scope.
    UsdAttribute const curveType = stage->GetPrimAtPath(
        SdfPath("/Character/Hair")).GetAttribute(TfToken("type"));
    referenceInput->HoldCpuSnapshotForCudaTransition();
    CHECK(curveType && curveType.Set(TfToken("linear")));
    indices.stageSceneIndex->ApplyPendingUpdates();
    referenceOwner->Synchronize();
    referenceInput->EnableCuda();
    referenceOwner->Synchronize();
    CHECK(!_Control(reference, renderPath));
    CHECK(_MatchesCpuSnapshot(reference, renderPath, cpuTiles, cpuTileData));
    CHECK(scopeNotices.added == 0 && scopeNotices.removed == 0);

    // Restore valid C3 geometry.  This stage edit is again explicitly
    // flushed; recovery publishes raw control while retaining CPU children
    // until the per-renderer HdSt staging filter accepts the group.
    CHECK(curveType.Set(TfToken("cubic")));
    indices.stageSceneIndex->ApplyPendingUpdates();
    referenceOwner->Synchronize();
    auto const recovered = _Control(reference, renderPath);
    CHECK(recovered && recovered->GetCandidate());
    CHECK(recovered->GetCandidate()->ticket != 0 &&
          recovered->GetCandidate()->ownsSubtree);
    CHECK(_MatchesCpuSnapshot(reference, renderPath, cpuTiles, cpuTileData));
    CHECK(scopeNotices.added == 0 && scopeNotices.removed == 0);
    reference->RemoveObserver(TfCreateWeakPtr(&scopeNotices));

    // Capability must be explicit. The same CUDA stage through the ordinary
    // direct constructor never enables device publication or emits a private
    // GPU control merely because its headers are linked.
    auto disabled = UsdGenGroomSceneIndex::New(indices.finalSceneIndex, renderInstance + 2);
    auto *disabledOwner = dynamic_cast<UsdGenGroomSceneIndex *>(disabled.operator->());
    CHECK(disabledOwner);
    disabledOwner->Synchronize();
    auto const disabledRender = disabled->GetPrim(renderPath);
    CHECK(disabledRender.primType == TfToken("scope") && disabledRender.dataSource);
    CHECK(!HdStBasisCurvesGpuGroupDataSource::Cast(
        disabledRender.dataSource->Get(HdStGetBasisCurvesGpuGroupDataSourceToken())));
    CHECK(errors.IsClean());

    std::puts("cuda Groom group ingress: PASS");
    return 0;
}
