#include "usdGen/session.h"
#include "usdGen/gpu/generation.h"
#include "usdGenImaging/usdGenGraphDescBuilder.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include <cmath>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdGen;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (false)

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
    std::puts("testUsdGenCudaHierarchy: PASS");
}
