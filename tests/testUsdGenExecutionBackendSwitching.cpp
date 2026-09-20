// One Session switching cpu->vulkan->cuda->cpu via descriptor replacement.
//
// Each step commits on the SAME Session object, proving descriptor
// replacement preserves last-good/cache state across backend switches.
// Backends compiled out (CUDA without USDGEN_ENABLE_CUDA, Vulkan without
// USDGEN_ENABLE_VULKAN_RUNTIME) must fail closed with precise
// unavailable-build diagnostics and retained previous generation, never a
// fabricated result from another backend. A built-but-GPU-less backend
// likewise fails closed with a precise no-device diagnostic.
#include "usdGen/compiler.h"
#include "usdGen/executionBackend.h"
#include "usdGen/session.h"

#ifdef USDGEN_ENABLE_VULKAN_RUNTIME
#include "usdGen/vulkan/defaultProvider.h"
#endif

#include <cstdio>
#include <string>

using namespace usdGen;

namespace {
int failures = 0;
void Check(bool value, char const* message) {
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

bool HasError(UsdGenSession::SnapshotPtr const& snapshot, char const* needle) {
    if (!snapshot) return false;
    for (std::string const& error : snapshot->diagnostics.errors)
        if (error.find(needle) != std::string::npos) return true;
    return false;
}

// Minimal Width chain admissible on all three backends: literal width,
// neutral interpolation/mask, no ramps or expressions.
UsdGenGraphDesc MakeSwitchDesc(UsdGenExecutionBackend backend) {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/BackendSwitching");
    desc.executionBackend = backend;
    desc.defaultWidth = 0.5f;
    UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/BackendSwitching/curves");
    curves.role = UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.curveVertexCounts = {3, 2};
    curves.curveId = {20, 10};
    curves.points = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    curves.rest = {{0, 0, 1}, {1, 0, 1}, {2, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    curves.widths = {1, 2, 3, 4, 5};
    curves.widthsInterpolation = TfToken("vertex");
    curves.curveGeneration = 41;
    desc.curveSets.push_back(curves);
    UsdGenNodeDesc source;
    source.path = SdfPath("/BackendSwitching/source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.params = {{TfToken("rebind"), VtValue(TfToken("never")), false}};
    UsdGenNodeDesc width;
    width.path = SdfPath("/BackendSwitching/width");
    width.type = TfToken("UsdGenWidth");
    width.inputs = {source.path};
    width.params = {{TfToken("width"), VtValue(1.25f), false},
                    {TfToken("replace"), VtValue(false), false}};
    desc.nodes = {source, width};
    desc.terminal = width.path;
    return desc;
}
} // namespace

int main() {
    // Contract-level proof first: unavailable builds report precisely.
    {
        UsdGenDiagnostics diagnostics;
#ifdef USDGEN_ENABLE_VULKAN_RUNTIME
        Check(ValidateUsdGenExecutionBackend(UsdGenExecutionBackend::Vulkan,
                                             &diagnostics) &&
                  diagnostics.errors.empty(),
              "Vulkan-enabled build validates Vulkan");
#else
        Check(!ValidateUsdGenExecutionBackend(UsdGenExecutionBackend::Vulkan,
                                              &diagnostics) &&
                  diagnostics.errors.size() == 1 &&
                  diagnostics.errors.front().find(
                      "Vulkan support was not enabled in this build") !=
                      std::string::npos,
              "Vulkan-disabled build reports precise unavailability");
        diagnostics.errors.clear();
#endif
#ifdef USDGEN_ENABLE_CUDA
        Check(ValidateUsdGenExecutionBackend(UsdGenExecutionBackend::Cuda,
                                             &diagnostics) &&
                  diagnostics.errors.empty(),
              "CUDA-enabled build validates CUDA");
#else
        Check(!ValidateUsdGenExecutionBackend(UsdGenExecutionBackend::Cuda,
                                              &diagnostics) &&
                  diagnostics.errors.size() == 1 &&
                  diagnostics.errors.front().find(
                      "CUDA support was not enabled in this build") !=
                      std::string::npos,
              "CUDA-disabled build reports precise unavailability");
#endif
    }

    UsdGenSession session(2, 8);
    session.SetDevicePublicationEnabled(true);

    // 1. CPU baseline succeeds.
    session.SetGraphDesc(MakeSwitchDesc(UsdGenExecutionBackend::CpuReference));
    auto cpuFirst = session.CommitSnapshot(1.0, UsdGenCommitReason::SetTime);
    Check(cpuFirst && !cpuFirst->diagnostics.HasErrors() &&
              cpuFirst->generation,
          "CPU baseline commits on the switching Session");
    if (!cpuFirst || !cpuFirst->generation) {
        std::fprintf(stderr, "testUsdGenExecutionBackendSwitching: FAILED\n");
        return 1;
    }

    // 2. Switch to Vulkan on the same Session.
    session.SetGraphDesc(MakeSwitchDesc(UsdGenExecutionBackend::Vulkan));
    auto vulkan = session.CommitSnapshot(2.0, UsdGenCommitReason::SetTime);
#ifdef USDGEN_ENABLE_VULKAN_RUNTIME
    if (vulkan::HasDefaultVulkanDevice()) {
        Check(vulkan && !vulkan->diagnostics.HasErrors() && vulkan->generation &&
                  vulkan->generation->device,
              "Vulkan switch executes on the default provider");
    } else {
        // Built but GPU-less: precise no-device diagnostic, CPU baseline
        // retained, and no fabricated CPU result for the Vulkan descriptor.
        Check(vulkan && vulkan->generation == cpuFirst->generation &&
                  HasError(vulkan, "Vulkan") &&
                  (HasError(vulkan, "no Vulkan compute device") ||
                   HasError(vulkan, "unavailable")),
              "Vulkan switch without a GPU fails closed with retained CPU generation");
    }
#else
    Check(vulkan && vulkan->generation == cpuFirst->generation &&
              HasError(vulkan, "Vulkan execution backend factory is unavailable") &&
              HasError(vulkan, "Vulkan support was not enabled in this build"),
          "Vulkan-disabled switch reports precise unavailability with retained CPU generation");
#endif

    // 3. Switch to CUDA on the same Session.
    session.SetGraphDesc(MakeSwitchDesc(UsdGenExecutionBackend::Cuda));
    auto cuda = session.CommitSnapshot(3.0, UsdGenCommitReason::SetTime);
#ifdef USDGEN_ENABLE_CUDA
    // A CUDA-enabled build either executes (GPU present) or fails closed
    // with a precise device diagnostic; either way the failure must name
    // CUDA and must not silently produce a CPU result for the CUDA graph.
    if (cuda && !cuda->diagnostics.HasErrors()) {
        Check(cuda->generation && cuda->generation->device,
              "CUDA switch executes on the CUDA lane");
    } else {
        Check(cuda && HasError(cuda, "CUDA"),
              "CUDA switch without a GPU fails closed with a CUDA diagnostic");
    }
#else
    Check(cuda && HasError(cuda, "CUDA execution backend factory is unavailable") &&
              HasError(cuda, "CUDA support was not enabled in this build"),
          "CUDA-disabled switch reports precise unavailability");
    // Retained generation is whatever the Vulkan step published (Vulkan
    // device generation on a GPU host, CPU baseline otherwise).
    Check(cuda && cuda->generation == vulkan->generation,
          "CUDA-disabled switch retains the previous generation");
#endif

    // 4. Switch back to CPU: the Session still cooks after backend failures.
    session.SetGraphDesc(MakeSwitchDesc(UsdGenExecutionBackend::CpuReference));
    auto cpuLast = session.CommitSnapshot(4.0, UsdGenCommitReason::SetTime);
    Check(cpuLast && !cpuLast->diagnostics.HasErrors() && cpuLast->generation,
          "CPU switch-back commits after Vulkan/CUDA steps");

    std::printf("testUsdGenExecutionBackendSwitching: %s\n",
                failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
