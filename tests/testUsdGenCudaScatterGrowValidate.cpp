// CPU-only contract for CudaScatterGrow::ValidateRoots (no device
// needed, so this test passes where the grow legs SKIP): controls,
// topology, finiteness, duplicate stable ids, and per-curve overflow.
// Pins the check order too (index order, Finite before duplicate
// before overflow), so faster validators must keep the same statuses.
#include "usdGen/gpu/scatterGrow.h"

#include <cuda_runtime.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
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

// Obviously-correct interleaved reference for the differential fuzz:
// same preamble as ValidateRoots, then the naive per-index loop (linear
// duplicate scan, verbatim overflow math copied from scatterGrow.cu).
// The fuzz below checks status equality on random multi-fault inputs,
// so any precedence or minima-combine bug in the radix version shows.
static uint64_t RefHash64(uint64_t key, uint32_t salt)
{
    uint64_t z = (key ^ (uint64_t(salt) * 0x9E3779B97F4A7C15ull)) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static float RefDraw(int seed, uint64_t id, uint32_t salt = 0x47726F77u)
{
    uint64_t key = RefHash64(uint64_t(uint32_t(seed)), salt) ^ id;
    return float(uint32_t(RefHash64(key, salt) >> 32) >> 8) * 0x1.0p-24f;
}
static float3 RefNorm(float3 v)
{
    float const length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 1.0e-12f) || !std::isfinite(length))
        return make_float3(0, 1, 0);
    return make_float3(v.x / length, v.y / length, v.z / length);
}
static float3 RefRot(float3 direction, float3 axis, float degrees)
{
    if (degrees == 0.0f) return direction;
    axis = RefNorm(axis);
    float const radians = degrees * (3.14159265358979323846f / 180.0f);
    float const c = std::cos(radians), s = std::sin(radians);
    float const dot = axis.x * direction.x + axis.y * direction.y + axis.z * direction.z;
    float3 const cross = make_float3(
        axis.y * direction.z - axis.z * direction.y,
        axis.z * direction.x - axis.x * direction.z,
        axis.x * direction.y - axis.y * direction.x);
    float const oneMinusC = 1.0f - c;
    return make_float3(
        direction.x * c + cross.x * s + axis.x * dot * oneMinusC,
        direction.y * c + cross.y * s + axis.y * dot * oneMinusC,
        direction.z * c + cross.z * s + axis.z * dot * oneMinusC);
}
static bool RefFinite(float3 v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
static bool RefFinite2(float2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

static ScatterGrowStatus ReferenceValidate(
    std::shared_ptr<const ScatterGrowRoots> const &r,
    ScatterGrowControls const &c, size_t *total)
{
    if (!r || c.cvCount < 2 || c.cvCount > 64 || !std::isfinite(c.length) ||
        !std::isfinite(c.randomLo) || !std::isfinite(c.randomHi) ||
        !std::isfinite(c.lift) || !std::isfinite(c.fallbackWidth) ||
        c.length < 0 || c.randomLo < 0 || c.randomHi < 0 || c.fallbackWidth < 0 ||
        c.lift < -90.0f || c.lift > 90.0f ||
        c.direction > ScatterGrowDirection::Literal ||
        !std::isfinite(c.azimuth) || c.azimuth < -360.0f || c.azimuth > 360.0f ||
        !std::isfinite(c.azimuthRandom) || c.azimuthRandom < 0.0f ||
        c.azimuthRandom > 1.0f ||
        (c.direction == ScatterGrowDirection::Literal && !RefFinite(c.literalDirection)))
        return ScatterGrowStatus::InvalidArgument;
    size_t n = r->positions.size();
    if (r->stableIds.size() != n || r->rootPrim.size() != n || r->rootUV.size() != n ||
        r->rootT.size() != n || r->rootB.size() != n || r->rootN.size() != n)
        return ScatterGrowStatus::InvalidTopology;
    ScatterGrowRequirements requirements;
    auto requirementStatus = GetScatterGrowRequirements(n, c.cvCount, &requirements);
    if (requirementStatus != ScatterGrowStatus::Ok) return requirementStatus;
    *total = requirements.pointCount;
    for (size_t i = 0; i < n; ++i) {
        if (!RefFinite(r->positions[i]) || !RefFinite2(r->rootUV[i]) ||
            !RefFinite(r->rootT[i]) || !RefFinite(r->rootB[i]) || !RefFinite(r->rootN[i]))
            return ScatterGrowStatus::NonFiniteInput;
        for (size_t j = 0; j < i; ++j)
            if (r->stableIds[j] == r->stableIds[i])
                return ScatterGrowStatus::DuplicateStableId;
        float3 direction = c.direction == ScatterGrowDirection::RootNormal ? r->rootN[i] :
            c.direction == ScatterGrowDirection::RootTangent ? r->rootT[i] : c.literalDirection;
        direction = RefRot(RefNorm(direction), r->rootB[i], c.lift);
        float const azimuth = c.azimuth + c.azimuthRandom * 360.0f *
            (RefDraw(c.seed, r->stableIds[i], 0x4772417Au) - 0.5f);
        direction = RefRot(direction, r->rootN[i], azimuth);
        if (!RefFinite(direction)) return ScatterGrowStatus::NonFiniteInput;
        double const targetDouble = c.length *
            (c.randomLo + double(RefDraw(c.seed, r->stableIds[i])) *
             (c.randomHi - c.randomLo));
        float const target = static_cast<float>(targetDouble);
        if (!std::isfinite(target)) return ScatterGrowStatus::NonFiniteInput;
        float const t = float(c.cvCount - 1) / float(c.cvCount - 1);
        float const distance = target * t;
        float3 const output = make_float3(
            r->positions[i].x + direction.x * distance,
            r->positions[i].y + direction.y * distance,
            r->positions[i].z + direction.z * distance);
        if (!std::isfinite(distance) || !RefFinite(output))
            return ScatterGrowStatus::NonFiniteInput;
    }
    return ScatterGrowStatus::Ok;
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
    // Fold collision without a true duplicate: distinct ids sharing the
    // radix leg's 32-bit fold must not report DuplicateStableId.
    {
        auto r = Roots(4);
        r->stableIds[1] = (uint64_t(1) << 32) | 0u;  // fold 1, != ids[0]
        size_t tRef = 0, tGot = 0;
        CHECK(ReferenceValidate(r, Controls(), &tRef) ==
                  ScatterGrowStatus::Ok &&
              CudaScatterGrow::ValidateRoots(r, Controls(), &tGot) ==
                  ScatterGrowStatus::Ok &&
              tRef == tGot);
    }
    // Oversized fold group (>64 members): 100 distinct ids on one fold
    // with a true dup pair hidden inside; the group-resolve path must
    // still agree with the linear reference.
    {
        auto r = Roots(200);
        for (size_t i = 0; i < 100; ++i)
            r->stableIds[i] = (uint64_t(i) << 32) | uint64_t(i);  // fold 0
        r->stableIds[50] = r->stableIds[7];
        size_t tRef = 0, tGot = 0;
        CHECK(ReferenceValidate(r, Controls(), &tRef) ==
                  ScatterGrowStatus::DuplicateStableId &&
              CudaScatterGrow::ValidateRoots(r, Controls(), &tGot) ==
                  ScatterGrowStatus::DuplicateStableId);
    }
    // ... same group without the dup: all folds collide, all ids
    // distinct, so the input is valid.
    {
        auto r = Roots(200);
        for (size_t i = 0; i < 200; ++i)
            r->stableIds[i] = (uint64_t(i) << 32) | uint64_t(i);  // fold 0
        size_t tRef = 0, tGot = 0;
        CHECK(ReferenceValidate(r, Controls(), &tRef) ==
                  ScatterGrowStatus::Ok &&
              CudaScatterGrow::ValidateRoots(r, Controls(), &tGot) ==
                  ScatterGrowStatus::Ok &&
              tRef == tGot && tGot == 200 * 8);
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
    // Differential fuzz vs the interleaved reference: random multi-fault
    // inputs must agree on status (and on the point total when Ok), so
    // the minima+radix combine keeps exact index-order precedence.
    {
        std::mt19937_64 rng(0x51ab51ab51ab51abull);
        auto pick = [&](std::initializer_list<float> vs) {
            return *(vs.begin() + size_t(rng() % vs.size()));
        };
        int sawOk = 0, sawNonFinite = 0, sawDup = 0;
        for (int trial = 0; trial < 30000; ++trial) {
            size_t const n = size_t(rng() % 13);
            auto r = std::make_shared<ScatterGrowRoots>();
            uint64_t const pool = 1 + size_t(rng() % 5);
            for (size_t i = 0; i < n; ++i) {
                float const px = pick({0.0f, 1.0f, -2.0f, 1e30f, float(FLT_MAX),
                                       std::numeric_limits<float>::infinity(),
                                       std::nanf("")});
                r->positions.push_back(make_float3(px, 0.0f, 0.0f));
                r->stableIds.push_back(rng() % pool);
                r->rootPrim.push_back(0);
                float const uv = pick({0.0f, 0.5f,
                                       std::numeric_limits<float>::infinity()});
                r->rootUV.push_back(make_float2(uv, 0.0f));
                float const frame = pick({0.0f, 1.0f,
                                          std::numeric_limits<float>::quiet_NaN()});
                r->rootT.push_back(make_float3(1.0f, 0.0f, 0.0f));
                r->rootB.push_back(make_float3(0.0f, 1.0f, frame));
                r->rootN.push_back(make_float3(0.0f, 0.0f, 1.0f));
            }
            auto c = Controls();
            c.cvCount = uint32_t(2 + rng() % 8);
            c.seed = int(rng() % 7) - 3;
            c.length = pick({1.0f, 1e30f, float(FLT_MAX)});
            c.randomLo = pick({0.0, 0.8, 1.0});
            c.randomHi = pick({0.8, 1.0, 1.2, 2.0});
            c.lift = pick({0.0f, 45.0f, -90.0f});
            c.azimuth = pick({0.0f, 180.0f, -360.0f});
            c.azimuthRandom = pick({0.0f, 0.5f, 1.0f});
            size_t tRef = 0, tGot = 0;
            ScatterGrowStatus const want =
                ReferenceValidate(r, c, &tRef);
            ScatterGrowStatus const got =
                CudaScatterGrow::ValidateRoots(r, c, &tGot);
            if (want != got || (want == ScatterGrowStatus::Ok && tRef != tGot)) {
                std::fprintf(stderr,
                             "FUZZ mismatch trial %d n=%zu: want %d got %d\n",
                             trial, n, int(want), int(got));
                return 1;
            }
            sawOk += want == ScatterGrowStatus::Ok;
            sawNonFinite += want == ScatterGrowStatus::NonFiniteInput;
            sawDup += want == ScatterGrowStatus::DuplicateStableId;
        }
        CHECK(sawOk > 100 && sawNonFinite > 100 && sawDup > 100);
        // Radix/scan at scale: medium random input with heavy collisions.
        {
            auto r = Roots(2000);
            for (size_t i = 0; i < 2000; ++i)
                r->stableIds[i] = uint64_t(rng() % 500);
            size_t tRef = 0, tGot = 0;
            CHECK(ReferenceValidate(r, Controls(), &tRef) ==
                  CudaScatterGrow::ValidateRoots(r, Controls(), &tGot) &&
                  tRef == tGot);
        }
    }
    std::printf("testUsdGenCudaScatterGrowValidate: PASS\n");
    return 0;
}
