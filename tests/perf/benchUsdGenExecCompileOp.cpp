// benchUsdGenExecCompileOp — measures compile and operator/execution times
// across CPU, CUDA, and Vulkan backends for the usdGen graph execution engine.
//
// Usage:
//   benchUsdGenExecCompileOp [path/to/width.spv]
//
// If the width.spv path is omitted or the file cannot be opened, the Vulkan
// section is skipped.  The CUDA section is gated at compile time by
// USDGEN_BENCH_CUDA; the Vulkan section by USDGEN_BENCH_VULKAN.
//
// Output: one "METRIC <name>=<value_ms>" line per measured quantity.
//
// Determinism: fixed seeds, no network, median-of-N timing.

#include "usdGen/compiler.h"
#include "usdGen/graph.h"
#include "usdGen/graphDesc.h"
#include "usdGen/opRegistry.h"
#include "usdGen/scheduler.h"
#include "usdGen/types.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef USDGEN_BENCH_CUDA
#include "usdGen/cudaExecution.h"
#include "usdGen/cudaExecutionQueue.h"
#endif

#ifdef USDGEN_BENCH_VULKAN
#include "usdGen/vulkan/planExecutor.h"
#include "usdGen/vulkan/executionPlan.h"
#include "usdGen/vulkan/deviceGenerationAdapter.h"
#include "usdGen/vulkan/completionService.h"
#include "usdGen/executionPipeline.h"
#include "usdGen/executionTaskGraph.h"
#include "../vulkanNativeFixture.h"
#include <future>
#endif

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdGen;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

double Median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double NowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void EmitMetric(char const* name, double ms) {
    std::printf("METRIC %s=%.4f\n", name, ms);
}

// G3 workload: 100k curves via Scatter on 400x250 surface, chain:
//   Scatter -> Grow -> Noise -> Length -> Width
// Mirrors benchUsdGenChain.cpp MakeG3().
UsdGenGraphDesc MakeG3()
{
    UsdGenGraphDesc d;
    d.description = SdfPath("/groom");
    d.terminal = SdfPath("/groom/width");
    d.time = 0.0;

    UsdGenSurfaceDesc s;
    s.path = SdfPath("/groom/surface");
    s.id = 0;
    const int NX = 400, NY = 250;
    s.restPoints = VtVec3fArray((NX + 1) * (NY + 1));
    s.uv = VtVec2fArray((NX + 1) * (NY + 1));
    for (int j = 0; j <= NY; ++j)
        for (int i = 0; i <= NX; ++i) {
            const int k = j * (NX + 1) + i;
            s.restPoints[k] = GfVec3f(i * 0.1f, j * 0.1f, 0.0f);
            s.uv[k] = GfVec2f(float(i) / NX, float(j) / NY);
        }
    s.faceVertexCounts = VtIntArray(NX * NY, 4);
    s.faceVertexIndices = VtIntArray(NX * NY * 4);
    for (int j = 0; j < NY; ++j)
        for (int i = 0; i < NX; ++i) {
            const int a = j * (NX + 1) + i;
            const int o = (j * NX + i) * 4;
            s.faceVertexIndices[o + 0] = a;
            s.faceVertexIndices[o + 1] = a + 1;
            s.faceVertexIndices[o + 2] = a + NX + 2;
            s.faceVertexIndices[o + 3] = a + 1;
        }
    d.surfaces.push_back(std::move(s));

    auto addNode = [&](std::string const& name, TfToken type,
                       std::string const& input, int seed) {
        UsdGenNodeDesc n;
        n.path = SdfPath("/groom/" + name);
        n.type = type;
        n.enabled = true;
        n.seed = seed;
        if (!input.empty()) n.inputs.push_back(SdfPath("/groom/" + input));
        if (type == TfToken("UsdGenScatter"))
            n.surfaces.push_back(SdfPath("/groom/surface"));
        d.nodes.push_back(std::move(n));
    };
    addNode("scatter", TfToken("UsdGenScatter"), "", 42);
    addNode("grow", TfToken("UsdGenGrow"), "scatter", 43);
    addNode("noise", TfToken("UsdGenNoise"), "grow", 44);
    addNode("length", TfToken("UsdGenLength"), "noise", 45);
    addNode("width", TfToken("UsdGenWidth"), "length", 46);

    auto setp = [&](std::string const& name, TfToken p, VtValue v) {
        for (auto& n : d.nodes)
            if (n.path == SdfPath("/groom/" + name))
                n.params.push_back(UsdGenParamValue{p, v, false});
    };
    setp("grow", TfToken("segments"), VtValue(8));
    setp("grow", TfToken("length"), VtValue(1.0));
    setp("noise", TfToken("noise:magnitude"), VtValue(0.05));
    setp("noise", TfToken("noise:frequency"), VtValue(3.0));
    setp("noise", TfToken("noise:octaves"), VtValue(2));
    setp("noise", TfToken("noise:correlation"), VtValue(0.5));
    setp("length", TfToken("length:mode"), VtValue(TfToken("scale")));
    setp("length", TfToken("length:value"), VtValue(1.2));
    setp("width", TfToken("width"), VtValue(0.02));
    return d;
}

// Force every chunk of every node fully value-dirty.
void ForceFullDirty(UsdGenGraph& g) {
    for (int i = 0; i < g.NodeCount(); ++i)
        for (uint8_t& byte : g.Node(i).chunkDirty)
            byte = UsdGenDirtyParameter;
}

// n-node chain of Scatter+Grow (for recompile measurement).
UsdGenGraphDesc MakeChain(int n) {
    UsdGenGraphDesc d;
    d.description = SdfPath("/chain");
    d.time = 0.0;
    UsdGenSurfaceDesc s;
    s.path = SdfPath("/chain/surface");
    s.id = 0;
    s.restPoints = VtVec3fArray(4, GfVec3f(0.f, 0.f, 0.f));
    s.uv = VtVec2fArray(4, GfVec2f(0.f, 0.f));
    s.faceVertexCounts = VtIntArray(1, 4);
    s.faceVertexIndices = VtIntArray({0, 1, 2, 3});
    d.surfaces.push_back(std::move(s));
    for (int i = 0; i < n; ++i) {
        UsdGenNodeDesc nd;
        nd.path = SdfPath("/chain/n" + std::to_string(i));
        nd.type = i == 0 ? TfToken("UsdGenScatter") : TfToken("UsdGenGrow");
        nd.enabled = true;
        nd.seed = 2000 + i;
        if (i == 0) nd.surfaces.push_back(SdfPath("/chain/surface"));
        else nd.inputs.push_back(SdfPath("/chain/n" + std::to_string(i - 1)));
        d.nodes.push_back(std::move(nd));
    }
    d.terminal = d.nodes.back().path;
    return d;
}

// ---------------------------------------------------------------------------
// CPU benchmark
// ---------------------------------------------------------------------------
struct CpuResults {
    double compileMs = 0;
    double recompileMs = 0;
    double commitMs = 0;
};

CpuResults RunCpuBench() {
    usdGenRegisterM1Operators();
    CpuResults results;

    // Compile (median of 5 fresh compiles of G3)
    {
        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            UsdGenCompiler c;
            UsdGenGraph g;
            double t0 = NowMs();
            UsdGenCompileResult r = c.Compile(MakeG3(), &g);
            times.push_back(NowMs() - t0);
            if (!r.ok) {
                std::fprintf(stderr, "CPU compile failed: ");
                for (auto& e : r.errors) std::fprintf(stderr, "%s ", e.c_str());
                std::fprintf(stderr, "\n");
                return results;
            }
        }
        results.compileMs = Median(std::move(times));
    }

    // Recompile (median of 5, chain-200 -> append 1 node)
    {
        UsdGenGraphDesc base = MakeChain(200);
        UsdGenGraphDesc grown = base;
        {
            UsdGenNodeDesc nd;
            nd.path = SdfPath("/chain/n200");
            nd.type = TfToken("UsdGenScatter");
            nd.enabled = true;
            nd.seed = 2200;
            nd.surfaces.push_back(base.surfaces[0].path);
            grown.nodes.push_back(std::move(nd));
            grown.terminal = grown.nodes.back().path;
        }
        UsdGenCompiler c;
        UsdGenGraph g;
        if (!c.Compile(base, &g).ok) return results;

        // Warmup
        c.Recompile(grown, &g);
        c.Recompile(base, &g);

        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            c.Recompile(base, &g);
            double t0 = NowMs();
            c.Recompile(grown, &g);
            times.push_back(NowMs() - t0);
        }
        results.recompileMs = Median(std::move(times));
    }

    // Full-dirty commit @8 threads (median of 5, after 2 warmup)
    {
        UsdGenCompiler c;
        UsdGenGraph g;
        if (!c.Compile(MakeG3(), &g).ok) return results;
        UsdGenEvalContext ctx;
        ctx.desc = &g.Desc();
        UsdGenScheduler s8(8);
        uint64_t gen = 0;

        // Warmup
        s8.Run(g, ctx, ++gen);
        ForceFullDirty(g);
        s8.Run(g, ctx, ++gen);

        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            ForceFullDirty(g);
            double t0 = NowMs();
            s8.Run(g, ctx, ++gen);
            times.push_back(NowMs() - t0);
        }
        results.commitMs = Median(std::move(times));
    }

    return results;
}

// ---------------------------------------------------------------------------
// CUDA benchmark
// ---------------------------------------------------------------------------
#ifdef USDGEN_BENCH_CUDA

UsdGenGraphDesc MakeCudaDesc(int numCurves) {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/Bench/Hair");
    desc.executionBackend = UsdGenExecutionBackend::Cuda;
    desc.time = 1.0;

    // Small 5-vertex surface for scatter/skin.
    UsdGenSurfaceDesc surface;
    surface.path = SdfPath("/Bench/Scalp");
    surface.id = 0;
    surface.restPoints = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    surface.points = surface.restPoints;
    surface.faceVertexCounts = {3,3,3};
    surface.faceVertexIndices = {0,1,2, 0,1,3, 1,2,4};
    desc.surfaces.push_back(surface);

    // Curve source: numCurves curves, each with 8 vertices.
    int const CV = 8;
    UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/Bench/Curves");
    curves.role = UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.curveVertexCounts = VtIntArray(numCurves, CV);
    curves.points = VtVec3fArray(numCurves * CV);
    curves.rest = curves.points;
    curves.curveId = VtArray<uint64_t>(numCurves);
    curves.skinPrim = VtIntArray(numCurves, 0);
    curves.skinPrimUv = VtVec2fArray(numCurves);
    for (int c = 0; c < numCurves; ++c) {
        curves.curveId[c] = c;
        curves.skinPrimUv[c] = GfVec2f(0.5f, 0.5f);
        for (int v = 0; v < CV; ++v) {
            int const idx = c * CV + v;
            curves.points[idx] = GfVec3f(
                float(c % 64) * 0.01f,
                float(c / 64) * 0.01f,
                float(v) * 0.01f);
        }
    }
    desc.curveSets.push_back(curves);

    UsdGenNodeDesc source;
    source.path = SdfPath("/Bench/Ops/source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.surfaces = {surface.path};

    UsdGenNodeDesc width;
    width.path = SdfPath("/Bench/Ops/width");
    width.type = TfToken("UsdGenWidth");
    width.inputs = {source.path};
    width.params.push_back({TfToken("width"), VtValue(0.03f), false});

    UsdGenNodeDesc length;
    length.path = SdfPath("/Bench/Ops/length");
    length.type = TfToken("UsdGenLength");
    length.inputs = {width.path};
    length.params.push_back({TfToken("length:mode"), VtValue(TfToken("scale")), false});
    length.params.push_back({TfToken("length:value"), VtValue(1.5f), false});

    desc.nodes = {source, width, length};
    desc.terminal = length.path;
    return desc;
}

struct CudaResults {
    bool available = false;
    double compileMs = 0;
    double execMs = 0;
};

CudaResults RunCudaBench() {
    CudaResults results;
    UsdGenDiagnostics diag;
    UsdGenGraphDesc desc = MakeCudaDesc(4096);

    // Compile (median of 5)
    {
        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            UsdGenDiagnostics d;
            double t0 = NowMs();
            auto plan = CompileCudaGraph(desc, &d);
            times.push_back(NowMs() - t0);
            if (!plan || d.HasErrors()) {
                for (auto& e : d.errors)
                    std::fprintf(stderr, "CUDA compile: %s\n", e.c_str());
                return results;
            }
        }
        results.compileMs = Median(std::move(times));
    }

    // Execute (median of 5, after 2 warmup)
    {
        auto plan = CompileCudaGraph(desc, &diag);
        if (!plan || diag.HasErrors()) return results;
        auto ws = CreateCudaExecutionWorkspace(-1, &diag);
        if (!ws) return results;
        results.available = true;

        uint64_t gen = 0;
        // Warmup
        for (int i = 0; i < 2; ++i) {
            UsdGenDiagnostics d;
            auto g = ExecuteCudaGraph(*plan, *ws, 1.0, ++gen, &d);
            if (!g) {
                for (auto& e : d.errors)
                    std::fprintf(stderr, "CUDA warmup exec: %s\n", e.c_str());
                results.available = false;
                return results;
            }
        }

        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            UsdGenDiagnostics d;
            double t0 = NowMs();
            auto g = ExecuteCudaGraph(*plan, *ws, 1.0, ++gen, &d);
            times.push_back(NowMs() - t0);
            if (!g) {
                for (auto& e : d.errors)
                    std::fprintf(stderr, "CUDA exec: %s\n", e.c_str());
                results.available = false;
                return results;
            }
        }
        results.execMs = Median(std::move(times));
    }

    return results;
}

#endif // USDGEN_BENCH_CUDA

// ---------------------------------------------------------------------------
// Vulkan benchmark
// ---------------------------------------------------------------------------
#ifdef USDGEN_BENCH_VULKAN

UsdGenGraphDesc MakeVulkanWidthDesc(int numCurves) {
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/VulkanBench");
    desc.executionBackend = UsdGenExecutionBackend::Vulkan;
    desc.defaultWidth = 0.5f;

    UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/VulkanBench/Curves");
    curves.role = UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    int const CV = 3;
    curves.curveVertexCounts = VtIntArray(numCurves, CV);
    curves.points = VtVec3fArray(numCurves * CV);
    curves.rest = curves.points;
    curves.curveId = VtArray<uint64_t>(numCurves);
    curves.widths = VtFloatArray(numCurves * CV, 1.0f);
    curves.widthsInterpolation = TfToken("vertex");
    curves.curveGeneration = 1;
    curves.frozenEpoch = "locked";
    for (int c = 0; c < numCurves; ++c) {
        curves.curveId[c] = c;
        for (int v = 0; v < CV; ++v) {
            int const idx = c * CV + v;
            curves.points[idx] = GfVec3f(
                float(c % 128) * 0.1f,
                float(c / 128) * 0.1f,
                float(v) * 0.1f);
        }
    }
    desc.curveSets = {curves};

    UsdGenNodeDesc source;
    source.path = SdfPath("/VulkanBench/Ops/source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.params = {{TfToken("useRest"), VtValue(true), false},
                     {TfToken("idSource"), VtValue(TfToken("primvar")), false},
                     {TfToken("expectEpoch"), VtValue(std::string("locked")), false},
                     {TfToken("staleAction"), VtValue(TfToken("block")), false},
                     {TfToken("rebind"), VtValue(TfToken("never")), false}};

    UsdGenNodeDesc width;
    width.path = SdfPath("/VulkanBench/Ops/width");
    width.type = TfToken("UsdGenWidth");
    width.inputs = {source.path};
    width.params = {{TfToken("width"), VtValue(0.5f), false},
                    {TfToken("replace"), VtValue(true), false}};

    desc.nodes = {source, width};
    desc.terminal = width.path;
    return desc;
}

struct VulkanResults {
    bool available = false;
    double compileMs = 0;
    double execMs = 0;
};

VulkanResults RunVulkanBench(char const* spvPath) {
    VulkanResults results;

    // Load SPIR-V
    std::ifstream spirvFile(spvPath, std::ios::binary);
    if (!spirvFile) {
        std::fprintf(stderr, "Vulkan: cannot open SPIR-V: %s\n", spvPath);
        return results;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(spirvFile)), {});
    if (raw.empty() || raw.size() % sizeof(uint32_t) != 0) {
        std::fprintf(stderr, "Vulkan: invalid SPIR-V size %zu\n", raw.size());
        return results;
    }
    std::vector<uint32_t> spirv(raw.size() / sizeof(uint32_t));
    std::memcpy(spirv.data(), raw.data(), raw.size());

    // Probe for NVIDIA GPU
    bool unavailable = false;
    auto probe = CreateNative(&unavailable);
    if (unavailable || !probe) {
        std::fprintf(stderr, "Vulkan: no NVIDIA GPU available, skipping execution\n");
        // Still measure compile (host-only)
        UsdGenDiagnostics diag;
        UsdGenGraphDesc desc = MakeVulkanWidthDesc(1024);
        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            double t0 = NowMs();
            auto handle = usdGen::vulkan::CompileVulkanSourceWidthPlan(desc, &diag);
            times.push_back(NowMs() - t0);
            if (!handle) break;
        }
        if (!times.empty()) {
            results.compileMs = Median(times);
            results.available = true; // compile-only
        }
        return results;
    }

    // Check timeline semaphore support
    VkPhysicalDeviceTimelineSemaphoreFeatures supported{};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &supported;
    vkGetPhysicalDeviceFeatures2(probe->physical, &features);
    if (!supported.timelineSemaphore) {
        std::fprintf(stderr, "Vulkan: no timeline semaphore, skipping execution\n");
        return results;
    }

    // Create logical device with timeline semaphore
    probe.reset();
    supported.timelineSemaphore = VK_TRUE;
    auto native = CreateNative(&unavailable, {}, &supported, nullptr);
    if (!native) {
        std::fprintf(stderr, "Vulkan: device creation failed\n");
        return results;
    }

    // Device context
    usdGen::vulkan::DeviceContext::CreateInfo ci;
    ci.instance = native->instance;
    ci.physicalDevice = native->physical;
    ci.device = native->device;
    ci.computeQueue = native->queue;
    ci.computeQueueFamily = native->family;
    ci.physicalIndex = native->physicalIndex;
    ci.resourceDeviceId = 7044;
    ci.nativeLifetime = native;
    ci.timelineSemaphoreEnabled = true;
    ci.resources = {size_t{64} << 20, size_t{4} << 20};
    VkResult vkResult = VK_SUCCESS;
    auto context = usdGen::vulkan::DeviceContext::Create(ci, &vkResult);
    if (!context) {
        std::fprintf(stderr, "Vulkan: DeviceContext::Create failed (VkResult=%d)\n", int(vkResult));
        return results;
    }

    // Execution pipeline and completion service
    UsdGenExecutionRuntime runtime{4};
    auto queueOwner = std::make_shared<UsdGenExecutionPipeline>(runtime, 16, 16);
    auto service = usdGen::vulkan::VulkanCompletionService::Create(
        {context, queueOwner.get(), 8});
    if (!service) {
        std::fprintf(stderr, "Vulkan: completion service failed\n");
        return results;
    }

    // Width pipeline
    auto pipeline = usdGen::vulkan::WidthPipeline::Create(context, spirv);
    if (!pipeline) {
        std::fprintf(stderr, "Vulkan: WidthPipeline::Create failed\n");
        return results;
    }

    // Domain
    std::string reason;
    auto domain = usdGen::vulkan::VulkanGenerationAdapterDomain::Create(
        {queueOwner, service}, &reason);
    if (!domain) {
        std::fprintf(stderr, "Vulkan: domain creation failed: %s\n", reason.c_str());
        return results;
    }

    // Preparer
    auto preparer = UsdGenExecutionTaskGraph::GetOrCreate(runtime, "bench", 7044);

    // Executor
    auto executor = usdGen::vulkan::VulkanPlanExecutor::Create(
        {domain, pipeline, preparer}, &reason);
    if (!executor) {
        std::fprintf(stderr, "Vulkan: executor creation failed: %s\n", reason.c_str());
        return results;
    }

    // Compile (median of 5)
    {
        UsdGenGraphDesc desc = MakeVulkanWidthDesc(1024);
        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            UsdGenDiagnostics diag;
            double t0 = NowMs();
            auto handle = usdGen::vulkan::CompileVulkanSourceWidthPlan(desc, &diag);
            times.push_back(NowMs() - t0);
            if (!handle) {
                for (auto& e : diag.errors)
                    std::fprintf(stderr, "Vulkan compile: %s\n", e.c_str());
                break;
            }
        }
        if (!times.empty()) {
            results.compileMs = Median(times);
            results.available = true;
        }
    }

    // Execute (median of 5, after 2 warmup)
    {
        UsdGenGraphDesc desc = MakeVulkanWidthDesc(1024);
        UsdGenDiagnostics diag;
        auto handle = usdGen::vulkan::CompileVulkanSourceWidthPlan(desc, &diag);
        if (!handle) return results;
        auto plan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(handle->Payload());
        if (!plan) return results;

        auto runOnce = [&](uint64_t generation) -> double {
            auto promise = std::make_shared<std::promise<bool>>();
            auto future = promise->get_future();
            auto requestLifetime = std::make_shared<int>(42);
            double t0 = NowMs();
            queueOwner->InvokeOwner([&] {
                usdGen::vulkan::VulkanPlanExecutor::Request req;
                req.context = context;
                req.capabilityVersion = 1;
                req.generation = generation;
                req.tool = {"bench", 1, generation};
                req.requestLifetime = std::move(requestLifetime);
                bool ok = executor->Submit(plan, std::move(req),
                    [promise](auto, auto) { promise->set_value(true); });
                if (!ok) promise->set_value(false);
            });
            auto status = future.wait_for(std::chrono::seconds(30));
            double t = NowMs() - t0;
            queueOwner->Drain();
            if (status != std::future_status::ready) return -1.0;
            return t;
        };

        // Warmup
        for (int i = 0; i < 2; ++i) {
            double t = runOnce(uint64_t(i + 1));
            if (t < 0) {
                std::fprintf(stderr, "Vulkan: warmup execution timed out\n");
                return results;
            }
        }

        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            double t = runOnce(uint64_t(100 + i));
            if (t < 0) {
                std::fprintf(stderr, "Vulkan: execution timed out\n");
                break;
            }
            times.push_back(t);
        }
        if (!times.empty()) {
            results.execMs = Median(times);
            results.available = true;
        }
    }

    // Shutdown
    executor->Shutdown();
    preparer->Drain();
    queueOwner->Drain();
    domain->Close();
    service->CloseAndJoin();

    return results;
}

#endif // USDGEN_BENCH_VULKAN

} // anonymous namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    char const* vulkanSpv = nullptr;
    if (argc > 1) vulkanSpv = argv[1];

    double total = 0.0;

    // CPU: compile + full-dirty commit always measured.
    auto cpu = RunCpuBench();
    EmitMetric("cpu_compile_ms", cpu.compileMs);
    EmitMetric("cpu_recompile_ms", cpu.recompileMs);
    EmitMetric("cpu_commit_ms", cpu.commitMs);
    total += cpu.compileMs + cpu.commitMs;

#ifdef USDGEN_BENCH_CUDA
    auto cuda = RunCudaBench();
    EmitMetric("cuda_compile_ms", cuda.compileMs);
    EmitMetric("cuda_exec_ms", cuda.execMs);
    total += cuda.compileMs + cuda.execMs;
#else
    EmitMetric("cuda_compile_ms", 0.0);
    EmitMetric("cuda_exec_ms", 0.0);
#endif

#ifdef USDGEN_BENCH_VULKAN
    if (vulkanSpv) {
        auto vulkan = RunVulkanBench(vulkanSpv);
        EmitMetric("vulkan_compile_ms", vulkan.compileMs);
        EmitMetric("vulkan_exec_ms", vulkan.execMs);
        total += vulkan.compileMs + vulkan.execMs;
    } else {
        std::fprintf(stderr, "bench: no Vulkan SPIR-V path; skipping Vulkan section\n");
        EmitMetric("vulkan_compile_ms", 0.0);
        EmitMetric("vulkan_exec_ms", 0.0);
    }
#else
    EmitMetric("vulkan_compile_ms", 0.0);
    EmitMetric("vulkan_exec_ms", 0.0);
#endif

    // Primary metric: summed compile + operator/execution cost across all
    // enabled backends (unavailable backends contribute 0).
    EmitMetric("exec_total_ms", total);
    return 0;
}
