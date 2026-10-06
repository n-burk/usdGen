// CPU-only contract for CudaScatterGrow::ValidateRoots (no device
// needed, so this test passes where the grow legs SKIP): controls,
// topology, finiteness, duplicate stable ids, and per-curve overflow.
// Pins the check order too (index order, Finite before duplicate
// before overflow), so faster validators must keep the same statuses.
#include "usdGen/gpu/scatterGrow.h"

#include <cuda_runtime.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace usdGen;
using namespace usdGen::gpu;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK failed: %s (%d)\n", #x, __LINE__); return 1; } } while (false)

static std::shared_ptr<ScatterGrowRoots> Roots(size_t n, uint64_t idBase = 1)
{
    auto r = std::make_shared<ScatterGrowRoots>();
    r->positions.reserve(n);
    r->stableIds.reserve(n);
    r->rootPrim.reserve(n);
    r->rootUV.reserve(n);
    r->rootT.reserve(n);
    r->rootB.reserve(n);
    r->rootN.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        r->positions.push_back(make_float3(float(i), 0.0f, 0.0f));
        r->stableIds.push_back(idBase + i);
        r->rootPrim.push_back(0);
        r->rootUV.push_back(make_float2(0.0f, 0.0f));
        r->rootT.push_back(make_float3(1.0f, 0.0f, 0.0f));
        r->rootB.push_back(make_float3(0.0f, 1.0f, 0.0f));
        r->rootN.push_back(make_float3(0.0f, 0.0f, 1.0f));
    }
    return r;
}

static ScatterGrowControls Controls()
{
    ScatterGrowControls c;
    c.cvCount = 8;
    c.seed = 42;
    c.length = 1.0;
    c.randomLo = 0.8;
    c.randomHi = 1.2;
    return c;
}

int main()
{
    size_t total = 0;
    // Valid roots: Ok + exact point total, deterministic across calls.
    {
        auto r = Roots(1000);
        auto c = Controls();
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
                  ScatterGrowStatus::Ok &&
              total == 1000 * 8);
        size_t again = 0;
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &again) ==
                  ScatterGrowStatus::Ok &&
              again == total);
    }
    // Empty roots validate with zero points.
    {
        auto r = Roots(0);
        CHECK(CudaScatterGrow::ValidateRoots(r, Controls(), &total) ==
                  ScatterGrowStatus::Ok &&
              total == 0);
    }
    // Duplicate stable ids, including at the tail.
    {
        auto r = Roots(100);
        r->stableIds[99] = r->stableIds[0];
        CHECK(CudaScatterGrow::ValidateRoots(r, Controls(), &total) ==
              ScatterGrowStatus::DuplicateStableId);
    }
    // Check order is index order: an earlier NonFinite beats a later dup.
    {
        auto r = Roots(100);
        r->stableIds[6] = r->stableIds[5];
        r->positions[0] = make_float3(HUGE_VALF, 0.0f, 0.0f);
        CHECK(CudaScatterGrow::ValidateRoots(r, Controls(), &total) ==
              ScatterGrowStatus::NonFiniteInput);
    }
    // ... and an earlier dup beats a later NonFinite.
    {
        auto r = Roots(100);
        r->stableIds[6] = r->stableIds[5];
        r->positions[9] = make_float3(HUGE_VALF, 0.0f, 0.0f);
        CHECK(CudaScatterGrow::ValidateRoots(r, Controls(), &total) ==
              ScatterGrowStatus::DuplicateStableId);
    }
    // Non-finite inputs in every plane.
    {
        auto c = Controls();
        auto r = Roots(10);
        r->rootUV[3] = make_float2(0.0f, NAN);
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::NonFiniteInput);
        r = Roots(10);
        r->rootT[3] = make_float3(0.0f, 0.0f, HUGE_VALF);
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::NonFiniteInput);
        r = Roots(10);
        r->rootB[3] = make_float3(NAN, 0.0f, 0.0f);
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::NonFiniteInput);
        r = Roots(10);
        r->rootN[3] = make_float3(0.0f, HUGE_VALF, 0.0f);
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::NonFiniteInput);
    }
    // Topology mismatch.
    {
        auto r = Roots(10);
        r->stableIds.pop_back();
        CHECK(CudaScatterGrow::ValidateRoots(r, Controls(), &total) ==
              ScatterGrowStatus::InvalidTopology);
    }
    // Bad controls, one fault at a time.
    {
        auto r = Roots(4);
        auto c = Controls();
        c.cvCount = 1;
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::InvalidArgument);
        c = Controls();
        c.cvCount = 65;
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::InvalidArgument);
        c = Controls();
        c.length = -1.0;
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::InvalidArgument);
        c = Controls();
        c.lift = 100.0f;
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::InvalidArgument);
        c = Controls();
        c.direction = ScatterGrowDirection::Literal;
        c.literalDirection = make_float3(NAN, 0.0f, 0.0f);
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::InvalidArgument);
        c = Controls();
        c.azimuthRandom = 2.0f;
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::InvalidArgument);
    }
    // Null roots fail closed.
    {
        std::shared_ptr<const ScatterGrowRoots> null;
        CHECK(CudaScatterGrow::ValidateRoots(null, Controls(), &total) ==
              ScatterGrowStatus::InvalidArgument);
    }
    // Overflow at full extension fails; exact cancellation passes.
    {
        auto c = Controls();
        c.length = double(FLT_MAX);
        c.randomLo = 1.0;
        c.randomHi = 1.0;
        auto r = Roots(2);
        r->positions[1] = make_float3(FLT_MAX, 0.0f, 0.0f);
        r->rootN[1] = make_float3(1.0f, 0.0f, 0.0f);
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
              ScatterGrowStatus::NonFiniteInput);
        r = Roots(2);
        r->positions[1] = make_float3(FLT_MAX, 0.0f, 0.0f);
        r->rootN[1] = make_float3(-1.0f, 0.0f, 0.0f);
        CHECK(CudaScatterGrow::ValidateRoots(r, c, &total) ==
                  ScatterGrowStatus::Ok &&
              total == 2 * 8);
    }
    // Unswapped random range (BeginFresh swaps; ValidateRoots does not).
    {
        auto c = Controls();
        c.randomLo = 1.2;
        c.randomHi = 0.8;
        CHECK(CudaScatterGrow::ValidateRoots(Roots(16), c, &total) ==
                  ScatterGrowStatus::Ok &&
              total == 16 * 8);
    }
    std::printf("testUsdGenCudaScatterGrowValidate: PASS\n");
    return 0;
}
