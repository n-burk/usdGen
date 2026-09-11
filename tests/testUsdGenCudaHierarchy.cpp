#include "usdGen/session.h"
#include "usdGen/cudaExecution.h"
#include "usdGen/gpu/generation.h"
#include "usdGenImaging/usdGenGraphDescBuilder.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include <cmath>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdGen;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (false)

class RestNotice final : public HdSceneIndexObserver {
public:
    bool rest = false;
    void PrimsAdded(HdSceneIndexBase const&, AddedPrimEntries const&) override {}
    void PrimsRemoved(HdSceneIndexBase const&, RemovedPrimEntries const&) override {}
    void PrimsRenamed(HdSceneIndexBase const&, RenamedPrimEntries const&) override {}
    void PrimsDirtied(HdSceneIndexBase const&, DirtiedPrimEntries const& entries) override {
        for (auto const& e : entries)
            rest |= e.primPath == SdfPath("/Scalp") && e.dirtyLocators.Intersects(
                HdDataSourceLocator(TfToken("usdGen"), TfToken("rest"), TfToken("points")));
    }
};

int main() {
    auto stage = UsdStage::Open(std::string(USDGEN_TEST_SOURCE_DIR) +
        "/plan/examples/cuda-width-network.usda");
    CHECK(stage);
    UsdImagingCreateSceneIndicesInfo info; info.stage = stage;
    auto indices = UsdImagingCreateSceneIndices(info);
    indices.stageSceneIndex->SetTime(UsdTimeCode(24));
    auto desc = usdGenImaging::BuildGraphDescFromHydra(
        *indices.finalSceneIndex, SdfPath("/Groom/hair"));
    CHECK(desc.validationErrors.empty());
    CHECK(desc.nodes.size() == 2 && desc.nodes[0].type == TfToken("UsdGenCurveSource"));
    CHECK(desc.nodes[0].inputs.empty() && desc.nodes[1].inputs == SdfPathVector{desc.nodes[0].path});
    CHECK(desc.nodes[1].expressionBindings.size() == 1);
    CHECK(desc.nodes[1].expressionBindings[0].domain == expr::Domain::Point);
    CHECK(desc.curveSets.size() == 1 && !desc.curveSets[0].restFromCurrentPoints);
    CHECK(desc.curveSets[0].rest.size() == 5 && desc.curveSets[0].rest[0][2] == 0);
    UsdGenSession session;
    session.SetDevicePublicationEnabled(true);
    session.SetGraphDesc(desc);
    auto generation = session.Commit(24, UsdGenCommitReason::SetTime);
    if (session.LastDiagnostics().HasErrors())
        for (auto const& error : session.LastDiagnostics().errors) std::fprintf(stderr, "%s\n", error.c_str());
    CHECK(generation && generation->device && generation->tiles.empty());
    auto lease = gpu::AcquireGeometry(generation->device, nullptr);
    CHECK(lease && lease.Geometry().pointCount == 5);
    float widths[5]{};
    float3 points[5]{};
    float3 rest[5]{};
    CHECK(cudaMemcpy(widths, lease.Geometry().widths.data, sizeof(widths), cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(cudaMemcpy(points, lease.Geometry().points.data, sizeof(points), cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(cudaMemcpy(rest, lease.Geometry().restPoints.data, sizeof(rest), cudaMemcpyDeviceToHost) == cudaSuccess);
    float const expected[5]{.04f, .03f, .02f, .04f, .02f};
    for (size_t i = 0; i < 5; ++i) {
        CHECK(std::abs(widths[i] - expected[i]) < 1e-6f);
        // CurveSource keeps loaded points and the Default-time rest channel
        // separate; useRest declares space, it does not overwrite points.
        CHECK(points[i].z == 5 && rest[i].z == 0);
    }
    auto rbfStage = UsdStage::Open(std::string(USDGEN_TEST_SOURCE_DIR) +
        "/plan/examples/cuda-rbf-network.usda");
    CHECK(rbfStage);
    UsdImagingCreateSceneIndicesInfo rbfInfo; rbfInfo.stage = rbfStage;
    auto rbfIndices = UsdImagingCreateSceneIndices(rbfInfo);
    rbfIndices.stageSceneIndex->SetTime(UsdTimeCode(24));
    auto rbfDesc = usdGenImaging::BuildGraphDescFromHydra(
        *rbfIndices.finalSceneIndex, SdfPath("/Groom/hair"));
    CHECK(rbfDesc.nodes.size() == 3 && rbfDesc.nodes[1].type == TfToken("UsdGenDeform"));
    CHECK(rbfDesc.validationErrors.empty());
    CHECK(rbfDesc.surfaces.size() == 1 && rbfDesc.surfaces[0].restPoints.size() == 5 &&
          rbfDesc.surfaces[0].restPoints[0][0] == 0 && !rbfDesc.surfaces[0].restFromCurrentPoints);
    session.SetGraphDesc(rbfDesc);
    auto deformed = session.Commit(24, UsdGenCommitReason::SetTime);
    for (auto const& error : session.LastDiagnostics().errors) std::fprintf(stderr, "%s\n", error.c_str());
    CHECK(deformed && deformed != generation && deformed->device && deformed->tiles.empty());
    CHECK(deformed->device->Geometry().alreadyDeformed);
    auto deformedLease = gpu::AcquireGeometry(deformed->device, nullptr);
    CHECK(deformedLease && deformedLease.Geometry().pointCount == 5);
    CHECK(cudaMemcpy(points, deformedLease.Geometry().points.data, sizeof(points), cudaMemcpyDeviceToHost) == cudaSuccess);
    CHECK(cudaMemcpy(widths, deformedLease.Geometry().widths.data, sizeof(widths), cudaMemcpyDeviceToHost) == cudaSuccess);
    for (size_t i = 0; i < 5; ++i) {
        float x = i < 3 ? 1.25f : 1.75f;
        CHECK(std::abs(points[i].x - x) < 1e-5f);
        CHECK(std::abs(widths[i] - .02f * (1 + x)) < 1e-6f);
    }
    auto bindings = GetCudaBindingStats(*session.Graph().CudaPlan());
    CHECK(bindings.size() == 1 && bindings[0].bindCount == 1 && bindings[0].sampleCount == 5);
    RestNotice notice;
    auto observer = TfCreateWeakPtr(&notice);
    rbfIndices.finalSceneIndex->AddObserver(observer);
    // A Default-time rest edit must invalidate/rebuild the persistent bind,
    // even though the animated current pose remains at frame 24.
    auto newRest = rbfDesc.surfaces[0].restPoints;
    newRest[0][0] = -.1f;
    UsdGeomMesh scalp(rbfStage->GetPrimAtPath(SdfPath("/Scalp")));
    CHECK(scalp.GetPointsAttr().Set(newRest, UsdTimeCode::Default()));
    rbfIndices.stageSceneIndex->ApplyPendingUpdates();
    CHECK(notice.rest);
    auto editedDesc = usdGenImaging::BuildGraphDescFromHydra(
        *rbfIndices.finalSceneIndex, SdfPath("/Groom/hair"));
    CHECK(editedDesc.validationErrors.empty() && editedDesc.surfaces.size() == 1);
    CHECK(editedDesc.surfaces[0].restPoints == newRest &&
          editedDesc.surfaces[0].points == rbfDesc.surfaces[0].points);
    session.SetGraphDesc(editedDesc);
    auto edited = session.Commit(24, UsdGenCommitReason::SetTime);
    CHECK(edited && edited != deformed && !session.LastDiagnostics().HasErrors());
    auto editedBinding = GetCudaBindingStats(*session.Graph().CudaPlan());
    CHECK(editedBinding.size() == 1 && editedBinding[0].identity != bindings[0].identity);
    // A failed edit must not replace the last published immutable generation.
    CHECK(scalp.GetFaceVertexIndicesAttr().Set(VtIntArray{99,1,2,0,1,3,1,2,4}));
    rbfIndices.stageSceneIndex->ApplyPendingUpdates();
    auto invalidDesc = usdGenImaging::BuildGraphDescFromHydra(
        *rbfIndices.finalSceneIndex, SdfPath("/Groom/hair"));
    session.SetGraphDesc(invalidDesc);
    CHECK(session.Commit(24, UsdGenCommitReason::SetTime) == edited);
    CHECK(session.LastDiagnostics().HasErrors());
    rbfIndices.finalSceneIndex->RemoveObserver(observer);
    std::puts("testUsdGenCudaHierarchy: PASS");
}
