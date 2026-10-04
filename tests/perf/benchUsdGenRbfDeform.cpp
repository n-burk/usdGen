// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

// benchUsdGenRbfDeform — RBF deformation execution-time breakdown.
//
// Workload D1: scatter -> grow -> deform (surface-driven, rbfSamples=100) ->
// width over a wavy 400x250 grid surface (spans 3D so the RBF binds), plus a
// direct rbf::CubicField microbench that isolates Bind/Solve/Displacement.
//
// Usage: benchUsdGenRbfDeform [--threads N] [--curves N]
// Output: one "METRIC <name>=<value_ms>" line per measured quantity
// (median of 9, fixture built once outside every timing window), plus
// CHECKSUM lines (FNV-1a over float bits) for bit-identity tracking.
// Exit is nonzero on hard failure only (compile/run errors, NaN output,
// nondeterministic repeat); timings are informational.
//
// With USDGEN_BENCH_CUDA the CUDA CudaRbfBinding path is measured too
// (Bind/Solve/Evaluate via CUDA events, n=100 samples, 1M CVs).

#include "usdGen/compiler.h"
#include "usdGen/graph.h"
#include "usdGen/graphDesc.h"
#include "usdGen/opRegistry.h"
#include "usdGen/ops/rbfField.h"
#include "usdGen/scheduler.h"
#include "usdGen/types.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/vt/array.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef USDGEN_BENCH_CUDA
#include "usdGen/gpu/rbf.h"
#include <cuda_runtime.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdGen;

namespace {

int g_hardFail = 0;

double Median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double NowMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void EmitMetric(char const *name, double ms)
{
    std::printf("METRIC %s=%.4f\n", name, ms);
}

uint64_t Fnv1a(void const *data, size_t bytes)
{
    auto const *p = static_cast<unsigned char const *>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= uint64_t(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

double Hash01(uint64_t k)
{
    k += 0x9e3779b97f4a7c15ULL;
    k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ULL;
    k = (k ^ (k >> 27)) * 0x94d049bb133111ebULL;
    return double((k ^ (k >> 31)) >> 11) / double(1ull << 53);
}

// --- field microbench fixture (built once, outside timing) ------------------

struct FieldFixture {
    std::vector<GfVec3d> rest;
    std::vector<GfVec3d> posed;
    std::vector<GfVec3d> queries;
};

FieldFixture MakeFieldFixture(int samples, size_t queryCount)
{
    FieldFixture f;
    f.rest.resize(size_t(samples));
    f.posed.resize(size_t(samples));
    for (int i = 0; i < samples; ++i) {
        GfVec3d p(Hash01(uint64_t(i) * 3), Hash01(uint64_t(i) * 3 + 1),
                  Hash01(uint64_t(i) * 3 + 2));
        f.rest[i] = p * 2.0 - GfVec3d(1.0);
        f.posed[i] = f.rest[i] + GfVec3d(0.2 * std::sin(3.0 * p[1]), 0.1 * p[0] * p[2],
                                         -0.15 * std::cos(2.0 * p[0]));
    }
    f.queries.resize(queryCount);
    for (size_t i = 0; i < queryCount; ++i)
        f.queries[i] = GfVec3d(Hash01(1001 + i * 3), Hash01(1001 + i * 3 + 1),
                               Hash01(1001 + i * 3 + 2)) * 2.0 - GfVec3d(1.0);
    return f;
}

// --- D1 graph fixture --------------------------------------------------------
// Wavy grid: the RBF needs samples spanning 3D, so rest z is a smooth bump
// field, not a plane.

UsdGenGraphDesc MakeD1(double density)
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
            float const x = float(i) * 0.1f, y = float(j) * 0.1f;
            float const z = 2.0f * std::sin(x * 0.3f) * std::cos(y * 0.3f);
            s.restPoints[k] = GfVec3f(x, y, z);
            s.uv[k] = GfVec2f(float(i) / NX, float(j) / NY);
        }
    s.points = s.restPoints;
    s.faceVertexCounts = VtIntArray(NX * NY, 4);
    s.faceVertexIndices = VtIntArray(NX * NY * 4);
    for (int j = 0; j < NY; ++j)
        for (int i = 0; i < NX; ++i) {
            const int a = j * (NX + 1) + i;
            const int o = (j * NX + i) * 4;
            s.faceVertexIndices[o + 0] = a;
            s.faceVertexIndices[o + 1] = a + 1;
            s.faceVertexIndices[o + 2] = a + NX + 2;
            s.faceVertexIndices[o + 3] = a + NX + 1;
        }
    d.surfaces.push_back(std::move(s));

    auto addNode = [&](std::string const &name, TfToken type,
                       std::string const &input, int seed) {
        UsdGenNodeDesc n;
        n.path = SdfPath("/groom/" + name);
        n.type = type;
        n.enabled = true;
        n.seed = seed;
        if (!input.empty()) n.inputs.push_back(SdfPath("/groom/" + input));
        if (type == TfToken("UsdGenScatter") || type == TfToken("UsdGenDeform"))
            n.surfaces.push_back(SdfPath("/groom/surface"));
        d.nodes.push_back(std::move(n));
    };
    addNode("scatter", TfToken("UsdGenScatter"), "", 42);
    addNode("grow", TfToken("UsdGenGrow"), "scatter", 43);
    addNode("deform", TfToken("UsdGenDeform"), "grow", 44);
    addNode("width", TfToken("UsdGenWidth"), "deform", 45);

    auto setp = [&](std::string const &name, TfToken p, VtValue v) {
        for (auto &n : d.nodes)
            if (n.path == SdfPath("/groom/" + name))
                n.params.push_back(UsdGenParamValue{p, v, false});
    };
    setp("scatter", TfToken("density"), VtValue(density));
    setp("grow", TfToken("segments"), VtValue(8));
    setp("grow", TfToken("length"), VtValue(1.0));
    setp("deform", TfToken("rbfSamples"), VtValue(100));
    setp("deform", TfToken("lockRoots"), VtValue(true));
    setp("width", TfToken("width"), VtValue(0.02));
    return d;
}

// Deterministic animated pose k over the rest surface (smooth bend).
void PoseSurface(UsdGenSurfaceDesc *s, int k)
{
    size_t const n = s->restPoints.size();
    s->points.resize(n);
    double const t = 0.35 * double(k);
    for (size_t i = 0; i < n; ++i) {
        GfVec3f const r = s->restPoints[i];
        float const bend = float(0.6 * std::sin(0.05 * r[0] + t) * std::cos(0.04 * r[1] - 0.5 * t));
        s->points[i] = GfVec3f(r[0], r[1], r[2] + bend);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    int threads = 8;
    double density = 25.0;   // ~25k curves, 225k CVs at 8 segments
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        if (std::string(argv[i]) == "--density" && i + 1 < argc) density = std::atof(argv[++i]);
    }
    usdGenRegisterM1Operators();

    // ---- field microbench: Bind / Solve / Displacement ----------------------
    {
        FieldFixture const f = MakeFieldFixture(100, 200000);
        std::string error;
        rbf::CubicField field;
        double t0 = NowMs();
        bool const bound = field.Bind(f.rest, &error);
        double const bindMs = NowMs() - t0;
        if (!bound) {
            std::printf("FIELD bind failed: %s\n", error.c_str());
            return 1;
        }
        EmitMetric("cpu_rbf_bind", bindMs);

        std::vector<double> tSolve, tDisp;
        uint64_t checksum = 0;
        for (int i = 0; i < 9; ++i) {
            t0 = NowMs();
            if (!field.Solve(f.posed, &error)) {
                std::printf("FIELD solve failed: %s\n", error.c_str());
                return 1;
            }
            tSolve.push_back(NowMs() - t0);
            t0 = NowMs();
            double acc[3] = {0, 0, 0};
            for (GfVec3d const &q : f.queries) {
                GfVec3d const d = field.Displacement(q);
                acc[0] += d[0]; acc[1] += d[1]; acc[2] += d[2];
            }
            tDisp.push_back(NowMs() - t0);
            if (i == 0) checksum = Fnv1a(acc, sizeof(acc));
            if (!std::isfinite(acc[0] + acc[1] + acc[2])) {
                std::printf("FIELD displacement produced non-finite output\n");
                return 1;
            }
        }
        EmitMetric("cpu_rbf_solve", Median(tSolve));
        EmitMetric("cpu_rbf_displace_200k", Median(tDisp));
        std::printf("CHECKSUM cpu_rbf_field=%016llx\n", (unsigned long long)checksum);
    }

    // ---- D1 graph: compile once, then animated re-commit --------------------
    UsdGenGraphDesc base = MakeD1(density);
    UsdGenCompiler compiler;
    UsdGenGraph graph;
    {
        std::vector<double> tCompile;
        for (int i = 0; i < 9; ++i) {
            UsdGenGraph g;
            double t0 = NowMs();
            UsdGenCompileResult const r = compiler.Compile(base, &g);
            tCompile.push_back(NowMs() - t0);
            if (!r.ok) {
                std::printf("D1 compile failed:");
                for (auto const &e : r.errors) std::printf(" %s", e.c_str());
                std::printf("\n");
                return 1;
            }
            if (i == 0) graph = std::move(g);
        }
        EmitMetric("cpu_compile", Median(tCompile));
    }
    UsdGenEvalContext ctx;
    UsdGenScheduler scheduler(threads);
    uint64_t gen = 0;
    UsdGenGraphDesc desc = base;
    PoseSurface(&desc.surfaces[0], 0);
    ctx.desc = nullptr;
    UsdGenCompileResult const warm = compiler.Recompile(desc, &graph);
    ctx.desc = &graph.Desc();
    if (!warm.ok || scheduler.Run(graph, ctx, ++gen).diagnostics.HasErrors()) {
        std::printf("D1 warmup run failed\n");
        return 1;
    }
    // Repeatability: the same pose twice must be bitwise identical.
    auto terminalChecksum = [&]() {
        UsdGenCurveBuffer const &t = graph.Node(graph.NodeIdForPath(desc.terminal)).buffer;
        uint64_t h = Fnv1a(t.px.cdata(), t.px.size() * sizeof(float));
        h ^= Fnv1a(t.py.cdata(), t.py.size() * sizeof(float)) * 1099511628211ULL;
        h ^= Fnv1a(t.pz.cdata(), t.pz.size() * sizeof(float)) * 1099511628211ULL;
        return h;
    };
    uint64_t const repeat0 = terminalChecksum();
    compiler.Recompile(desc, &graph);
    ctx.desc = &graph.Desc();
    scheduler.Run(graph, ctx, ++gen);
    if (terminalChecksum() != repeat0) {
        std::printf("D1 repeatability FAIL: same pose cooked different bits\n");
        return 1;
    }
    std::printf("  curves=%u cvs=%u\n",
                graph.Node(graph.NodeIdForPath(desc.terminal)).buffer.totalCurves,
                graph.Node(graph.NodeIdForPath(desc.terminal)).buffer.totalCvs);

    std::vector<double> tCommit, tDeformCapture, tDeformEval;
    uint64_t lastChecksum = 0;
    for (int i = 1; i <= 9; ++i) {
        PoseSurface(&desc.surfaces[0], i);
        double t0 = NowMs();
        UsdGenCompileResult const rr = compiler.Recompile(desc, &graph);
        ctx.desc = &graph.Desc();
        UsdGenRunResult const run = scheduler.Run(graph, ctx, ++gen);
        tCommit.push_back(NowMs() - t0);
        if (!rr.ok || run.diagnostics.HasErrors()) {
            std::printf("D1 commit %d failed\n", i);
            return 1;
        }
        for (UsdGenNodeRunStats const &st : run.nodeStats)
            if (graph.Node(st.id).type == TfToken("UsdGenDeform")) {
                tDeformCapture.push_back(st.captureMs);
                tDeformEval.push_back(st.evalMs);
            }
        lastChecksum = terminalChecksum();
    }
    EmitMetric("cpu_commit_pose", Median(tCommit));
    EmitMetric("cpu_deform_capture", Median(tDeformCapture));
    EmitMetric("cpu_deform_evaluate", Median(tDeformEval));
    std::printf("CHECKSUM cpu_d1_terminal=%016llx\n", (unsigned long long)lastChecksum);

#ifdef USDGEN_BENCH_CUDA
    // ---- CUDA: direct CudaRbfBinding Bind/Solve/Evaluate --------------------
    {
        const int n = 100, cvs = 1000000, m = n + 4;
        std::vector<float3> rest(n), posed(n), cv(cvs);
        for (int i = 0; i < n; ++i) {
            rest[i] = make_float3(float(2 * Hash01(uint64_t(i) * 3) - 1),
                                  float(2 * Hash01(uint64_t(i) * 3 + 1) - 1),
                                  float(2 * Hash01(uint64_t(i) * 3 + 2) - 1));
            posed[i] = make_float3(rest[i].x + 0.1f * float(std::sin(double(i))),
                                   rest[i].y - 0.05f * float(i % 7),
                                   rest[i].z + 0.07f * float(std::cos(2.0 * double(i))));
        }
        for (int i = 0; i < cvs; ++i)
            cv[i] = make_float3(float(2 * Hash01(1001 + uint64_t(i) * 3) - 1),
                                float(2 * Hash01(1001 + uint64_t(i) * 3 + 1) - 1),
                                float(2 * Hash01(1001 + uint64_t(i) * 3 + 2) - 1));
        float3 *dRest = nullptr, *dPosed = nullptr, *dCv = nullptr, *dOut = nullptr;
        cudaStream_t stream = nullptr;
        cudaEvent_t start = nullptr, stop = nullptr;
        if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess ||
            cudaEventCreate(&start) != cudaSuccess || cudaEventCreate(&stop) != cudaSuccess ||
            cudaMalloc(&dRest, size_t(n) * sizeof(float3)) != cudaSuccess ||
            cudaMalloc(&dPosed, size_t(n) * sizeof(float3)) != cudaSuccess ||
            cudaMalloc(&dCv, size_t(cvs) * sizeof(float3)) != cudaSuccess ||
            cudaMalloc(&dOut, size_t(cvs) * sizeof(float3)) != cudaSuccess) {
            std::printf("CUDA setup failed\n");
            return 1;
        }
        cudaMemcpy(dRest, rest.data(), size_t(n) * sizeof(float3), cudaMemcpyHostToDevice);
        cudaMemcpy(dPosed, posed.data(), size_t(n) * sizeof(float3), cudaMemcpyHostToDevice);
        cudaMemcpy(dCv, cv.data(), size_t(cvs) * sizeof(float3), cudaMemcpyHostToDevice);
        auto eventMs = [&](cudaEvent_t a, cudaEvent_t b) {
            float ms = 0;
            cudaEventElapsedTime(&ms, a, b);
            return double(ms);
        };
        gpu::CudaRbfBinding binding;
        std::vector<double> tBind, tSolve, tEval;
        for (int i = 0; i < 9; ++i) {
            cudaEventRecord(start, stream);
            gpu::RbfStatus st = binding.Bind({dRest, size_t(n)}, 0.0, stream);
            cudaEventRecord(stop, stream);
            cudaEventSynchronize(stop);
            if (st != gpu::RbfStatus::Ok) {
                std::printf("CUDA bind failed: %s\n", binding.diagnostic());
                return 1;
            }
            tBind.push_back(eventMs(start, stop));
            cudaEventRecord(start, stream);
            st = binding.Solve({dPosed, size_t(n)}, stream);
            cudaEventRecord(stop, stream);
            cudaEventSynchronize(stop);
            if (st != gpu::RbfStatus::Ok) {
                std::printf("CUDA solve failed: %s\n", binding.diagnostic());
                return 1;
            }
            tSolve.push_back(eventMs(start, stop));
            cudaEventRecord(start, stream);
            st = binding.Evaluate({dCv, size_t(cvs)}, {dOut, size_t(cvs)}, stream);
            if (st == gpu::RbfStatus::Ok) st = binding.Finish(stream);
            cudaEventRecord(stop, stream);
            cudaEventSynchronize(stop);
            if (st != gpu::RbfStatus::Ok) {
                std::printf("CUDA evaluate failed: %s\n", binding.diagnostic());
                return 1;
            }
            tEval.push_back(eventMs(start, stop));
        }
        EmitMetric("cuda_rbf_bind", Median(tBind));
        EmitMetric("cuda_rbf_solve", Median(tSolve));
        EmitMetric("cuda_rbf_evaluate_1m", Median(tEval));
        std::vector<float3> out;
        out.resize(size_t(cvs));
        cudaMemcpy(out.data(), dOut, size_t(cvs) * sizeof(float3), cudaMemcpyDeviceToHost);
        uint64_t h = Fnv1a(out.data(), size_t(cvs) * sizeof(float3));
        std::printf("CHECKSUM cuda_rbf_eval=%016llx\n", (unsigned long long)h);
        (void)m;
        cudaEventDestroy(start);
        cudaEventDestroy(stop);
        cudaStreamDestroy(stream);
        cudaFree(dRest);
        cudaFree(dPosed);
        cudaFree(dCv);
        cudaFree(dOut);
    }
#endif

    if (g_hardFail) return 1;
    std::printf("rbf-deform bench: done\n");
    return 0;
}
