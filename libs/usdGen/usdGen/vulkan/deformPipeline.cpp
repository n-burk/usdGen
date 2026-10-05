// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

#include "deformPipeline.h"
#include "fencePool.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace usdGen::vulkan {

char const* DeformSemanticName(DeformSemantic s) noexcept {
    switch (s) {
        case DeformSemantic::Ok:            return "Ok";
        case DeformSemantic::BadOffsets:    return "BadOffsets";
        case DeformSemantic::NonFinite:     return "NonFinite";
        case DeformSemantic::BadValue:      return "BadValue";
        case DeformSemantic::RankDeficient: return "RankDeficient";
        case DeformSemantic::SolverError:   return "SolverError";
    }
    return "Invalid";
}

namespace {

// Workgroup widths, each matched to its shader's local_size_x and each at
// its measured best (spin-observed GPU times, stable to +-2us): the fp64
// eval loop runs ~80us faster per 1M-CV pose at 128-wide groups (13.25
// -> 13.17ms; 64 and 32 are no faster), and the memory-bound apply is
// ~4us faster at 256 (0.167 -> 0.163ms), so each dispatch keeps its own
// width.
constexpr uint32_t kEvalLocalSize = 128;
constexpr uint32_t kApplyLocalSize = 256;
constexpr uint32_t kEvalUboBytes = 64;
constexpr uint32_t kApplyUboBytes = 80;
// Eval set: bindings 0,1,2,3,5 = 5 storage + binding 4 = 1 uniform.
constexpr uint32_t kEvalStorage = 5;
// Apply set: bindings 0..8 = 9 storage + binding 9 = 1 uniform.
constexpr uint32_t kApplyStorage = 9;
// Cached-R evaluate: the first kEvalCachePrefixCVs CVs run through the
// cached radius-cubed rows (P*n doubles); points past the prefix run the
// direct shader with a base offset. 598016 CVs at n=100 need 456.25MiB
// of R plus a 6.8MiB proof snapshot: 499MiB next to the ~36MiB of pose
// scratch and uploads, ~13MiB inside the 512MiB bench budget (the pool
// charges exact bytes, so the margin is deterministic run to run). Each
// CV moved off the fp64-sqrt suffix (~13.1ns) onto the streaming prefix
// (~3.2ns) saves ~9.9ns. Stays below 600000 so the 600k-point test still
// covers the suffix dispatch; smaller budgets run the direct shader with
// no added work.
constexpr uint32_t kEvalCachePrefixCVs = 598016;
constexpr uint32_t kVerifyLocalSize = 256;
constexpr uint32_t kFillLocalSize = 256;
constexpr uint32_t kCachedLocalSize = 128;
constexpr uint32_t kVerifyUboBytes = 16;
constexpr uint32_t kFillUboBytes = 48;
constexpr uint32_t kCachedUboBytes = 64;
constexpr uint32_t kFillArgsBytes = 12;
// Verify/fill set: bindings 0,1,2 = 3 storage + binding 3 = 1 uniform.
// The cached set reuses the eval layout (binding 2 carries R instead of
// the samples; same types), so it needs no new layout.
constexpr uint32_t kVerifyFillStorage = 3;
// Miss backoff: two consecutive refills mean the CVs (or rest) churn
// every pose, so the next poses run direct until one probe re-arms the
// cache. Churning inputs then cost ~+1% instead of +15%.
constexpr int kEvalCacheBypassMisses = 2;
constexpr int kEvalCacheBypassPoses = 32;

std::atomic<bool> disableEvalCache{false};
std::atomic<uint64_t> evalCacheHits{0}, evalCacheMisses{0};
std::atomic<uint64_t> evalCacheBypassed{0}, evalCacheUnfunded{0};

uint32_t Groups(uint32_t n, uint32_t size) { return (n + size - 1) / size; }

// Scratch pooling: a pose allocates 8 buffers and frees them at candidate
// destruction, and vkAllocateMemory/vkFreeMemory cost ~70/80us (median) each
// on the qualified driver, so ~1.2ms of every pose is allocation lifecycle.
// All eight are reusable across poses: the proof/eval status words, the
// eval/apply UBOs, the coefficient/sample uploads, the warped staging
// buffer, and outPoints are all GPU-idle once the candidate proves (the
// apply fence signals after the last read) and none is read on the host
// afterwards, except outPoints through a live Output.
// outPoints rejoins its own pool at State destruction rather than at Poll:
// it is exposed through Candidate::output() and the caller may read it
// until the Candidate dies, so Poll-time return would hand a live buffer
// to the next pose. The coefficient/sample buffers
// are allocated at their max (sampleCount is rejected above 100) so a pooled
// set fits any pose; the warped buffer must match the pose's point count
// exactly. Like the fence pool, only proven-idle sets rejoin: a set from a
// failed or unproven submission stays with its owner for quarantine or
// destruction, never pooled.
constexpr int kMaxRbfSamples = 100;
constexpr VkDeviceSize kMaxCoefBytes = VkDeviceSize(kMaxRbfSamples + 4) * 24u;
constexpr VkDeviceSize kMaxSamplesBytes = VkDeviceSize(kMaxRbfSamples) * 24u;
constexpr size_t kMaxIdleScratchSets = 2;
constexpr size_t kMaxIdleOutPoints = 2;

// Domain codes (deformApply.comp constants).
enum : uint32_t {
    kDomainGroom = 1u,
    kDomainPrimitive = 2u,
    kDomainPoint = 4u,
};

struct EvalUbo {
    int32_t n_m[2];
    int32_t counts[2];
    double cx, cy, cz, invScale, scale, _pad2;
};
static_assert(sizeof(EvalUbo) == 64);

// Cached-R UBOs (see the .comp ABI notes). VerifyUbo: words, n.
// FillUbo: n, active prefix CVs, P, then the center/invScale the fill
// consumes. CachedUbo: n, m, active count, P, then the eval tail.
struct VerifyUbo {
    int32_t words_n[4];
};
static_assert(sizeof(VerifyUbo) == 16);
struct FillUbo {
    int32_t shape[4];
    double cx, cy, cz, invScale;
};
static_assert(sizeof(FillUbo) == 48);
static_assert(offsetof(FillUbo, cx) == 16);
struct CachedUbo {
    int32_t nmcs[4];
    double cx, cy, cz, invScale, scale, _pad;
};
static_assert(sizeof(CachedUbo) == 64);
static_assert(offsetof(CachedUbo, cx) == 16);

struct ApplyUbo {
    uint32_t counts[3];       // uvec3: 12B in std140, offset 0
    float groomEnvelope;      // offset 12
    float maskF[4];          // vec4: 16B in std140, offset 16
    float enabledF[4];       // vec4: offset 32
    float lockF[4];          // vec4: offset 48
    uint32_t fieldCounts[3]; // uvec3: 12B in std140, offset 64
    uint32_t strideCandidate; // offset 76; total 80, 16B-aligned block
};
static_assert(sizeof(ApplyUbo) == 80);
static_assert(offsetof(ApplyUbo, groomEnvelope) == 12);
static_assert(offsetof(ApplyUbo, maskF) == 16);
static_assert(offsetof(ApplyUbo, enabledF) == 32);
static_assert(offsetof(ApplyUbo, lockF) == 48);
static_assert(offsetof(ApplyUbo, fieldCounts) == 64);

} // namespace

void TestDisableDeformEvalCache(bool disable) noexcept {
    disableEvalCache.store(disable, std::memory_order_release);
}
uint64_t DeformEvalCacheHitsForTesting() noexcept {
    return evalCacheHits.load(std::memory_order_acquire);
}
uint64_t DeformEvalCacheMissesForTesting() noexcept {
    return evalCacheMisses.load(std::memory_order_acquire);
}
uint64_t DeformEvalCacheBypassedForTesting() noexcept {
    return evalCacheBypassed.load(std::memory_order_acquire);
}
uint64_t DeformEvalCacheUnfundedForTesting() noexcept {
    return evalCacheUnfunded.load(std::memory_order_acquire);
}

struct DeformPipeline::Native {
    std::shared_ptr<DeviceContext> context;
    VkShaderModule evalShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout evalLayout = VK_NULL_HANDLE;
    VkPipelineLayout evalPipelineLayout = VK_NULL_HANDLE;
    VkPipeline evalPipeline = VK_NULL_HANDLE;
    VkShaderModule applyShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout applyLayout = VK_NULL_HANDLE;
    VkPipelineLayout applyPipelineLayout = VK_NULL_HANDLE;
    VkPipeline applyPipeline = VK_NULL_HANDLE;
    // Cached-R evaluate (null without cache SPIR-V). The verify and fill
    // shaders share one set layout (3 storage + 1 uniform at the same
    // bindings); the cached shader reuses the eval set layout and the
    // eval pipeline layout (binding 2 carries R instead of the samples).
    VkShaderModule verifyShader = VK_NULL_HANDLE;
    VkShaderModule fillShader = VK_NULL_HANDLE;
    VkShaderModule cachedShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout verifyFillLayout = VK_NULL_HANDLE;
    VkPipelineLayout verifyFillPipelineLayout = VK_NULL_HANDLE;
    VkPipeline verifyPipeline = VK_NULL_HANDLE;
    VkPipeline fillPipeline = VK_NULL_HANDLE;
    VkPipeline cachedPipeline = VK_NULL_HANDLE;
    // The rest-only factorization survives poses that keep the same rest
    // samples and smoothing; a pose then pays only the right-hand side and
    // the triangular solves. Guarded: Begin may run on several threads.
    std::mutex factorMutex;
    struct FactorCache {
        std::vector<float> rest;
        double smoothing = 0.0;
        int n = 0;
        bool valid = false;
        RbfFactorization factor;
    } factorCache;
    // Pose-invariant R cache. R[j*P+i] is the radius-cubed kernel value
    // for CV i and sample j: it depends only on the CVs, the rest
    // samples, and the rest-derived center/scale, so poses that keep
    // them evaluate the cached prefix through a streaming FMA pass and
    // only the past-prefix suffix through the sqrt loop. The verify
    // shader checks the prefix CV bytes on the device (and snapshots
    // them into `proof`), self-scheduling the fill through the indirect
    // args; the rest and n are host BeginInfo, compared under the mutex
    // (a mismatch forces the refill instead of speculating). Guarded:
    // the whole cached proof phase (decide, submit, wait, bookkeeping)
    // runs under cacheMutex so a force commit is atomic with its fill.
    std::mutex cacheMutex;
    struct EvalCache {
        std::shared_ptr<ChargedBuffer> r;     // R rows, P*rowsAlloc doubles
        std::shared_ptr<ChargedBuffer> proof; // prefix CV snapshot, P float3s
        std::vector<float> rest;              // rest proof (host)
        int n = 0;                            // n at the last fill
        int rowsAlloc = 0;                    // R rows allocated
        uint32_t filled = 0;                  // valid R/proof prefix (CVs)
        int missStreak = 0;                   // consecutive refills
        int bypassLeft = 0;                   // direct poses left in backoff
        bool everFilled = false;
    } evalCache;
    // Idle submit fences: fence create/destroy costs ~0.7ms each on the
    // qualified driver, so per-pose submits check out of this pool instead
    // of creating. Candidates hold Native by shared_ptr, so the pool
    // outlives every checkout.
    VulkanFencePool fencePool;
    // Idle per-pose scratch sets (everything but outPoints): allocation
    // lifecycle costs ~150us per buffer per pose, so proven-idle sets are
    // checked out by the next Begin instead of allocating. Guarded: Begin
    // may run on several threads. At most kMaxIdleScratchSets are kept;
    // surplus sets are destroyed on release. Members destroy after the body
    // with context (declared first) still alive, so no explicit clear.
    struct ScratchSet {
        std::shared_ptr<ChargedBuffer> warped;
        std::shared_ptr<ChargedBuffer> status;
        std::shared_ptr<ChargedBuffer> evalStatus;
        std::shared_ptr<ChargedBuffer> coefBuf;
        std::shared_ptr<ChargedBuffer> samplesBuf;
        std::shared_ptr<ChargedBuffer> evalUbo;
        std::shared_ptr<ChargedBuffer> applyUbo;
        // Cached-R per-pose scratch (null on direct-only pipelines): the
        // three cache UBOs plus the per-pose indirect fill args. The args
        // are per-pose (not cache-owned) so concurrent poses never share
        // a pre-write.
        std::shared_ptr<ChargedBuffer> verifyUbo;
        std::shared_ptr<ChargedBuffer> fillUbo;
        std::shared_ptr<ChargedBuffer> cachedUbo;
        std::shared_ptr<ChargedBuffer> fillArgs;
    };
    std::mutex scratchMutex;
    std::vector<ScratchSet> scratchIdle;
    // Idle outPoints buffers, exact-size matched at checkout: allocation
    // lifecycle costs ~150us per pose, and the buffer is provably idle at
    // State destruction (see ReturnOutPoints). The caller may hold
    // Output::points past the Candidate's death, so a pooled buffer is
    // reused only while the pool is its sole owner; anything still shared
    // stays idle and the pose allocates fresh. Capped; scratchMutex-held.
    std::vector<std::shared_ptr<ChargedBuffer>> outPointsIdle;
    ~Native() {
        if (!context) return;
        auto d = context->device();
        fencePool.Clear(d);
        if (verifyPipeline) vkDestroyPipeline(d, verifyPipeline, nullptr);
        if (fillPipeline) vkDestroyPipeline(d, fillPipeline, nullptr);
        if (cachedPipeline) vkDestroyPipeline(d, cachedPipeline, nullptr);
        if (verifyFillPipelineLayout)
            vkDestroyPipelineLayout(d, verifyFillPipelineLayout, nullptr);
        if (verifyFillLayout) vkDestroyDescriptorSetLayout(d, verifyFillLayout, nullptr);
        if (verifyShader) vkDestroyShaderModule(d, verifyShader, nullptr);
        if (fillShader) vkDestroyShaderModule(d, fillShader, nullptr);
        if (cachedShader) vkDestroyShaderModule(d, cachedShader, nullptr);
        if (evalPipeline) vkDestroyPipeline(d, evalPipeline, nullptr);
        if (evalPipelineLayout) vkDestroyPipelineLayout(d, evalPipelineLayout, nullptr);
        if (evalLayout) vkDestroyDescriptorSetLayout(d, evalLayout, nullptr);
        if (evalShader) vkDestroyShaderModule(d, evalShader, nullptr);
        if (applyPipeline) vkDestroyPipeline(d, applyPipeline, nullptr);
        if (applyPipelineLayout) vkDestroyPipelineLayout(d, applyPipelineLayout, nullptr);
        if (applyLayout) vkDestroyDescriptorSetLayout(d, applyLayout, nullptr);
        if (applyShader) vkDestroyShaderModule(d, applyShader, nullptr);
    }
};

struct DeformPipeline::Candidate::State {
    std::shared_ptr<Native> native;
    BeginInfo inputs;
    std::shared_ptr<ChargedBuffer> outPoints;
    std::shared_ptr<ChargedBuffer> warped;
    std::shared_ptr<ChargedBuffer> status;
    std::shared_ptr<ChargedBuffer> evalStatus;
    std::shared_ptr<ChargedBuffer> coefBuf;
    std::shared_ptr<ChargedBuffer> samplesBuf;
    std::shared_ptr<ChargedBuffer> evalUbo;
    std::shared_ptr<ChargedBuffer> applyUbo;
    std::shared_ptr<ChargedBuffer> verifyUbo;
    std::shared_ptr<ChargedBuffer> fillUbo;
    std::shared_ptr<ChargedBuffer> cachedUbo;
    std::shared_ptr<ChargedBuffer> fillArgs;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE, proofFence = VK_NULL_HANDLE;
    bool pending = false, lost = false, proved = false;
    uint32_t semantic = UINT32_MAX;
    uint32_t pointCount = 0;
    // Exactly-once guard for the scratch release below: Poll returns the
    // set early (so the next Begin reuses it while this candidate is still
    // alive), and the destructor returns it when Poll never ran. Atomic:
    // two threads may Poll the same candidate concurrently.
    std::atomic<bool> scratchReturned{false};
    std::unique_ptr<std::shared_ptr<State>> quarantine;
    // Moves the scratch set back to the pool when it is complete and
    // pool-sized; anything else (the empty-topology early-out, a failure
    // path's partial set) is destroyed with the state. The caller proves
    // GPU idleness: Poll after the apply fence signaled, or the destructor
    // when nothing is pending. Never throws.
    void ReturnScratch() noexcept {
        Native::ScratchSet set;
        set.warped = std::move(warped);
        set.status = std::move(status);
        set.evalStatus = std::move(evalStatus);
        set.coefBuf = std::move(coefBuf);
        set.samplesBuf = std::move(samplesBuf);
        set.evalUbo = std::move(evalUbo);
        set.applyUbo = std::move(applyUbo);
        set.verifyUbo = std::move(verifyUbo);
        set.fillUbo = std::move(fillUbo);
        set.cachedUbo = std::move(cachedUbo);
        set.fillArgs = std::move(fillArgs);
        if (!set.warped || !set.status || !set.evalStatus || !set.coefBuf ||
            !set.samplesBuf || !set.evalUbo || !set.applyUbo)
            return;
        // Direct-only pipelines never allocate the cache scratch; cached
        // pipelines pool only complete sets.
        bool const wantCache = native->cachedPipeline != VK_NULL_HANDLE;
        if (wantCache && (!set.verifyUbo || !set.fillUbo || !set.cachedUbo || !set.fillArgs))
            return;
        // Pool-sized: the fixed buffers fit every pose at or above their
        // nominal size (descriptors range the actual bytes); the warped
        // buffer is matched exactly at checkout.
        if (set.status->sizeBytes() < 4 || set.evalStatus->sizeBytes() < 4 ||
            set.evalUbo->sizeBytes() < kEvalUboBytes ||
            set.applyUbo->sizeBytes() < kApplyUboBytes ||
            set.coefBuf->sizeBytes() < kMaxCoefBytes ||
            set.samplesBuf->sizeBytes() < kMaxSamplesBytes)
            return;
        if (wantCache &&
            (set.verifyUbo->sizeBytes() < kVerifyUboBytes ||
             set.fillUbo->sizeBytes() < kFillUboBytes ||
             set.cachedUbo->sizeBytes() < kCachedUboBytes ||
             set.fillArgs->sizeBytes() < kFillArgsBytes))
            return;
        try {
            std::lock_guard<std::mutex> lock(native->scratchMutex);
            if (native->scratchIdle.size() < kMaxIdleScratchSets)
                native->scratchIdle.push_back(std::move(set));
        } catch (...) {
        }
    }
    // Returns outPoints to the idle pool. Only the destructor calls this:
    // between Poll and destruction the caller may still read output(), so
    // Poll-time return would hand a live buffer to the next pose. The
    // caller requires !pending && !lost (GPU-idle, like ReturnScratch:
    // the apply fence proved the write complete, or nothing submitted);
    // Outputs the caller kept past destruction stay shared, and checkout
    // only reuses sole-owned buffers. Never throws.
    void ReturnOutPoints() noexcept {
        if (!outPoints) return;
        try {
            std::lock_guard<std::mutex> lock(native->scratchMutex);
            if (native->outPointsIdle.size() < kMaxIdleOutPoints)
                native->outPointsIdle.push_back(std::move(outPoints));
        } catch (...) {
        }
    }
    ~State() {
        auto d = native->context->device();
        // A candidate destroyed without Poll still owns a GPU-idle set when
        // nothing is pending (rejected before submit, or the proof wait
        // passed and the apply never submitted): rejoin it, else a pending
        // or lost set stays here for quarantine/destruction, never pooled.
        if (!scratchReturned.exchange(true, std::memory_order_acq_rel) &&
            !pending && !lost)
            ReturnScratch();
        // outPoints rejoins its pool at destruction, not at Poll (Output
        // readers may outlive the proof). Exactly once: this runs once.
        if (!pending && !lost)
            ReturnOutPoints();
        // This runs only when nothing is pending (else the Candidate
        // quarantines instead of destroying), so a proven fence is idle
        // and rejoins the pool; an unproven one is destroyed as before.
        // proofFence is pooled or destroyed inside Begin; a leftover here
        // is unproven by construction.
        if (fence) {
            if (proved) native->fencePool.Release(d, fence);
            else vkDestroyFence(d, fence, nullptr);
        }
        if (proofFence) vkDestroyFence(d, proofFence, nullptr);
        if (commands) vkDestroyCommandPool(d, commands, nullptr);
        if (descriptors) vkDestroyDescriptorPool(d, descriptors, nullptr);
    }
};

DeformPipeline::DeformPipeline(std::shared_ptr<Native> n) : native_(std::move(n)) {}
DeformPipeline::~DeformPipeline() = default;
std::shared_ptr<DeviceContext> const& DeformPipeline::context() const noexcept {
    return native_->context;
}

// ============================ Create ============================

std::shared_ptr<DeformPipeline> DeformPipeline::Create(
    std::shared_ptr<DeviceContext> context,
    std::vector<uint32_t> const& evaluateSpirv,
    std::vector<uint32_t> const& applySpirv,
    VkResult* result,
    DeformEvalCacheSpirv const& cacheSpirv) {
    auto finish = [&](VkResult r) { if (result) *result = r; };
    finish(VK_ERROR_INITIALIZATION_FAILED);
    auto valid = [](std::vector<uint32_t> const& c) {
        return c.size() >= 5 && c.front() == 0x07230203u;
    };
    if (!context || !valid(evaluateSpirv) || !valid(applySpirv)) return {};
    bool const wantCache = !cacheSpirv.verify.empty() || !cacheSpirv.fill.empty() ||
        !cacheSpirv.cached.empty();
    // All three cache shaders or none: a partial set is a wiring bug,
    // failed loudly rather than silently direct.
    if (wantCache && (!valid(cacheSpirv.verify) || !valid(cacheSpirv.fill) ||
                      !valid(cacheSpirv.cached)))
        return {};
    if (!context->shaderFloat64Enabled()) return {};
    VkPhysicalDeviceProperties physical{};
    vkGetPhysicalDeviceProperties(context->physicalDevice(), &physical);
    if (physical.limits.maxComputeWorkGroupInvocations < kApplyLocalSize ||
        physical.limits.maxComputeWorkGroupSize[0] < kApplyLocalSize ||
        physical.limits.maxPerStageDescriptorStorageBuffers < kApplyStorage ||
        physical.limits.maxDescriptorSetStorageBuffers < kApplyStorage) return {};
    try {
        auto n = std::make_shared<Native>();
        n->context = std::move(context);
        auto d = n->context->device();
        auto mkModule = [&](std::vector<uint32_t> const& code, VkShaderModule* out) {
            VkShaderModuleCreateInfo sm{};
            sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm.codeSize = code.size() * sizeof(uint32_t);
            sm.pCode = code.data();
            return vkCreateShaderModule(d, &sm, nullptr, out);
        };
        VkResult r = mkModule(evaluateSpirv, &n->evalShader);
        if (r == VK_SUCCESS) r = mkModule(applySpirv, &n->applyShader);
        if (wantCache && r == VK_SUCCESS) r = mkModule(cacheSpirv.verify, &n->verifyShader);
        if (wantCache && r == VK_SUCCESS) r = mkModule(cacheSpirv.fill, &n->fillShader);
        if (wantCache && r == VK_SUCCESS) r = mkModule(cacheSpirv.cached, &n->cachedShader);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        {
            // Eval layout: 5 storage (0,1,2,3,5) + 1 uniform (4).
            uint32_t const total = kEvalStorage + 1;
            VkDescriptorSetLayoutBinding bindings[total]{};
            uint32_t const storageBindings[] = {0, 1, 2, 3, 5};
            for (uint32_t i = 0; i < total; ++i) {
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                if (i == 4) {
                    bindings[i].binding = 4;
                    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                } else {
                    bindings[i].binding = storageBindings[i < 4 ? i : i - 1];
                    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                }
            }
            VkDescriptorSetLayoutCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ds.bindingCount = total; ds.pBindings = bindings;
            r = vkCreateDescriptorSetLayout(d, &ds, nullptr, &n->evalLayout);
            if (r != VK_SUCCESS) { finish(r); return {}; }
        }
        {
            // Apply layout: 9 storage (0..8) + 1 uniform (9).
            uint32_t const total = kApplyStorage + 1;
            VkDescriptorSetLayoutBinding bindings[total]{};
            for (uint32_t i = 0; i < total; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                if (i == kApplyStorage)
                    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                else
                    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            VkDescriptorSetLayoutCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ds.bindingCount = total; ds.pBindings = bindings;
            r = vkCreateDescriptorSetLayout(d, &ds, nullptr, &n->applyLayout);
            if (r != VK_SUCCESS) { finish(r); return {}; }
        }
        if (wantCache) {
            // Verify/fill layout: 3 storage (0,1,2) + 1 uniform (3),
            // shared by both shaders.
            uint32_t const total = kVerifyFillStorage + 1;
            VkDescriptorSetLayoutBinding bindings[total]{};
            for (uint32_t i = 0; i < total; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                bindings[i].descriptorType =
                    (i == kVerifyFillStorage) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            VkDescriptorSetLayoutCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ds.bindingCount = total; ds.pBindings = bindings;
            r = vkCreateDescriptorSetLayout(d, &ds, nullptr, &n->verifyFillLayout);
            if (r != VK_SUCCESS) { finish(r); return {}; }
        }
        {
            VkPipelineLayoutCreateInfo pl{};
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl.setLayoutCount = 1; pl.pSetLayouts = &n->evalLayout;
            r = vkCreatePipelineLayout(d, &pl, nullptr, &n->evalPipelineLayout);
            if (r == VK_SUCCESS) {
                pl.pSetLayouts = &n->applyLayout;
                r = vkCreatePipelineLayout(d, &pl, nullptr, &n->applyPipelineLayout);
            }
            if (wantCache && r == VK_SUCCESS) {
                pl.pSetLayouts = &n->verifyFillLayout;
                r = vkCreatePipelineLayout(d, &pl, nullptr, &n->verifyFillPipelineLayout);
            }
            if (r != VK_SUCCESS) { finish(r); return {}; }
        }
        auto mkPipeline = [&](VkShaderModule module, VkPipelineLayout layout, VkPipeline* out) {
            VkComputePipelineCreateInfo cp{};
            cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cp.layout = layout;
            cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cp.stage.module = module; cp.stage.pName = "main";
            return vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cp, nullptr, out);
        };
        r = mkPipeline(n->evalShader, n->evalPipelineLayout, &n->evalPipeline);
        if (r == VK_SUCCESS)
            r = mkPipeline(n->applyShader, n->applyPipelineLayout, &n->applyPipeline);
        // The cached shader reuses the eval pipeline layout (same
        // binding types at the same numbers).
        if (wantCache && r == VK_SUCCESS)
            r = mkPipeline(n->verifyShader, n->verifyFillPipelineLayout, &n->verifyPipeline);
        if (wantCache && r == VK_SUCCESS)
            r = mkPipeline(n->fillShader, n->verifyFillPipelineLayout, &n->fillPipeline);
        if (wantCache && r == VK_SUCCESS)
            r = mkPipeline(n->cachedShader, n->evalPipelineLayout, &n->cachedPipeline);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        auto pipeline = std::shared_ptr<DeformPipeline>(new DeformPipeline(std::move(n)));
        finish(VK_SUCCESS);
        return pipeline;
    } catch (std::bad_alloc const&) { finish(VK_ERROR_OUT_OF_HOST_MEMORY); return {}; }
}

// ============================ Candidate ============================

DeformPipeline::Candidate::Candidate(std::shared_ptr<State> s) : state_(std::move(s)) {}
DeformPipeline::Candidate::~Candidate() { if (state_->pending) Quarantine(); }

void DeformPipeline::Candidate::Quarantine() noexcept {
    if (!state_->pending || state_->lost) return;
    state_->lost = true;
    *state_->quarantine = state_;
    (void)state_->quarantine.release();
}

bool DeformPipeline::Candidate::succeeded() const noexcept {
    return state_ && !state_->lost && state_->proved && state_->semantic == 0;
}

DeformPipeline::Candidate::Output DeformPipeline::Candidate::output() const noexcept {
    if (!state_ || !state_->proved || state_->semantic != 0) return {};
    return Output{state_->pointCount ? state_->outPoints : nullptr};
}

std::shared_ptr<const ChargedBuffer> DeformPipeline::Candidate::inputOwner() const noexcept {
    return state_ ? state_->inputs.points : nullptr;
}

std::shared_ptr<DeviceContext> DeformPipeline::Candidate::context() const noexcept {
    return state_ && state_->native ? state_->native->context : nullptr;
}

uint32_t DeformPipeline::Candidate::pointCount() const noexcept {
    return state_ ? state_->pointCount : 0;
}

VkResult DeformPipeline::Candidate::Poll(DeformSemantic* semantic) {
    auto& s = *state_;
    if (s.lost) return VK_ERROR_DEVICE_LOST;
    if (!s.proved) {
        auto d = s.native->context->device();
        VkResult r = vkGetFenceStatus(d, s.fence);
        if (r == VK_NOT_READY) return r;
        if (r != VK_SUCCESS) { Quarantine(); return r; }
        void* data = nullptr;
        r = vkMapMemory(d, s.status->memory(), 0, 4, 0, &data);
        if (r != VK_SUCCESS) return r;
        std::memcpy(&s.semantic, data, 4);
        vkUnmapMemory(d, s.status->memory());
        s.pending = false;
        s.proved = true;
        // The apply fence signaled, so the scratch set is GPU-idle and no
        // host reader remains (Poll copied the only scalar): rejoin the
        // pool now, while this candidate is still alive, so the next pose's
        // Begin reuses it. Exactly once; concurrent Polls share the guard.
        if (!s.scratchReturned.exchange(true, std::memory_order_acq_rel))
            s.ReturnScratch();
    }
    if (semantic) *semantic = static_cast<DeformSemantic>(s.semantic);
    return VK_SUCCESS;
}

// ============================ Begin ============================

std::unique_ptr<DeformPipeline::Candidate> DeformPipeline::Begin(
    BeginInfo info, VkResult* result, DeformSemantic* semantic, BeforeSubmit beforeSubmit) {
    auto finish = [&](VkResult r) { if (result) *result = r; };
    auto reject = [&](DeformSemantic code) {
        if (semantic) *semantic = code;
        finish(VK_ERROR_INITIALIZATION_FAILED);
        return std::unique_ptr<Candidate>{};
    };
    finish(VK_ERROR_INITIALIZATION_FAILED);
    if (semantic) *semantic = DeformSemantic::Ok;
    auto context = info.points ? info.points->context()
                   : (info.curveOffsets ? info.curveOffsets->context() : nullptr);
    if (!context) context = native_->context;
    if (!context || context != native_->context) return {};

    uint32_t const curves = info.curveCount, points = info.pointCount;
    int const n = info.sampleCount;
    int const m = n + 4;

    // Sample count range.
    if (n < 4 || n > 100) return reject(DeformSemantic::BadValue);

    // Sample sizes.
    if ((int)info.restSamples.size() != 3 * n || (int)info.posedSamples.size() != 3 * n)
        return reject(DeformSemantic::BadValue);

    // Finite check.
    for (float v : info.restSamples)
        if (!std::isfinite(v)) return reject(DeformSemantic::NonFinite);
    for (float v : info.posedSamples)
        if (!std::isfinite(v)) return reject(DeformSemantic::NonFinite);

    // Groom envelope.
    if (!std::isfinite(info.groomEnvelope) || info.groomEnvelope < 0.0f)
        return reject(DeformSemantic::BadValue);

    auto check = [&](std::shared_ptr<ChargedBuffer const> const& b, VkDeviceSize bytes) {
        return b && b->context() == context && !b->unproven() && b->buffer() &&
               (b->usage() & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) && b->sizeBytes() >= bytes;
    };

    // Geometry shape.
    if (curves == 0 && points != 0) return reject(DeformSemantic::BadOffsets);
    if (curves > 0) {
        if (!check(info.points, VkDeviceSize(points) * 12u) ||
            !check(info.curveOffsets, VkDeviceSize(curves + 1u) * 4u) ||
            !check(info.rootTargets, VkDeviceSize(curves) * 12u))
            return reject(DeformSemantic::BadOffsets);
    } else {
        if (info.points || info.curveOffsets || info.rootTargets)
            return reject(DeformSemantic::BadOffsets);
    }

    // Field validation.
    auto fieldCount = [&](uint32_t domain) -> uint32_t {
        switch (domain) {
            case kDomainGroom:       return 1u;
            case kDomainPrimitive:   return curves;
            case kDomainPoint:       return points;
            default: return 0;
        }
    };
    auto vScalar = [&](DeformPipeline::ScalarField const& f) {
        if (f.domain != kDomainGroom && f.domain != kDomainPrimitive && f.domain != kDomainPoint)
            return false;
        if (!f.data) {
            if (f.count != 0) return false;
            if (f.domain != kDomainGroom && fieldCount(f.domain) != 0) return false;
            if (!std::isfinite(f.literal)) return false;
            if (f.domain == kDomainGroom) {
                if (f.literal < 0.0f || f.literal > 1.0f) return false;
            }
            return true;
        }
        uint32_t expected = fieldCount(f.domain);
        return f.count == expected && check(f.data, VkDeviceSize(expected) * 4u);
    };
    auto vBool = [&](DeformPipeline::BoolField const& f, bool groomOnly, bool allowPrimitive) {
        if (f.domain != kDomainGroom && f.domain != kDomainPrimitive && f.domain != kDomainPoint)
            return false;
        if (groomOnly && f.domain != kDomainGroom) return false;
        if (!allowPrimitive && f.domain == kDomainPrimitive) return false;
        if (f.domain == kDomainPoint) return false;
        if (!f.data) {
            if (f.domain == kDomainGroom && f.count == 0) return true;
            if (f.domain == kDomainPrimitive && f.count == 0 && curves == 0) return true;
            return false;
        }
        uint32_t expected = fieldCount(f.domain);
        return f.count == expected && check(f.data, VkDeviceSize(expected) * 4u);
    };
    if (!vScalar(info.mask)) return reject(DeformSemantic::BadValue);
    if (!vBool(info.enabled, true, false)) return reject(DeformSemantic::BadValue);
    if (!vBool(info.lockRoots, false, true)) return reject(DeformSemantic::BadValue);

    // Device-resident topology is validated by the apply shader; only its
    // scalar diagnostic is read back before exposing any output.

    // CPU solve, with the factorization cached while rest and smoothing
    // are unchanged (a bitwise hit replays exactly what a rebind would
    // compute, including bind-phase failures).
    RbfState rstate;
    {
        float3 const* rest = reinterpret_cast<float3 const*>(info.restSamples.data());
        float3 const* posed = reinterpret_cast<float3 const*>(info.posedSamples.data());
        std::lock_guard<std::mutex> lock(native_->factorMutex);
        auto& cache = native_->factorCache;
        size_t const restBytes = info.restSamples.size() * sizeof(float);
        bool const hit = cache.valid && cache.n == n &&
            cache.smoothing == info.smoothing &&
            cache.rest.size() == info.restSamples.size() &&
            std::memcmp(cache.rest.data(), info.restSamples.data(), restBytes) == 0;
        if (!hit) {
            cache.rest = info.restSamples;
            cache.n = n;
            cache.smoothing = info.smoothing;
            BindRbf(rest, n, info.smoothing, cache.factor);
            cache.valid = true;
        }
        if (cache.factor.status != RbfStatus::Code::Ok)
            rstate.status = cache.factor.status;
        else
            SolveRbfPosed(cache.factor, rest, posed, rstate);
    }
    double const invScale = 1.0 / rstate.scale;
    if (rstate.status != RbfStatus::Code::Ok) {
        DeformSemantic sem;
        switch (rstate.status) {
            case RbfStatus::Code::RankDeficient: sem = DeformSemantic::RankDeficient; break;
            case RbfStatus::Code::NonFinite:     sem = DeformSemantic::NonFinite; break;
            default:                             sem = DeformSemantic::SolverError; break;
        }
        return reject(sem);
    }

    try {
        auto s = std::make_shared<Candidate::State>();
        s->native = native_;
        s->inputs = info;
        s->pointCount = points;
        s->quarantine = std::make_unique<std::shared_ptr<Candidate::State>>();
        auto candidate = std::unique_ptr<Candidate>(new Candidate(s));
        auto d = context->device();
        VkResult r;
        VkDeviceSize const outBytes = points ? VkDeviceSize(points) * 12u : 4u;

        // Check out a proven-idle scratch set whose warped buffer matches
        // this pose's point count; anything missing is allocated fresh
        // below (at pool sizes, so it rejoins the pool in turn). The
        // empty-topology pose needs no scratch and skips the checkout.
        if (curves != 0) {
            std::lock_guard<std::mutex> lock(native_->scratchMutex);
            for (auto it = native_->scratchIdle.begin();
                 it != native_->scratchIdle.end(); ++it) {
                if (it->warped && it->warped->sizeBytes() == outBytes) {
                    s->warped = std::move(it->warped);
                    s->status = std::move(it->status);
                    s->evalStatus = std::move(it->evalStatus);
                    s->coefBuf = std::move(it->coefBuf);
                    s->samplesBuf = std::move(it->samplesBuf);
                    s->evalUbo = std::move(it->evalUbo);
                    s->applyUbo = std::move(it->applyUbo);
                    s->verifyUbo = std::move(it->verifyUbo);
                    s->fillUbo = std::move(it->fillUbo);
                    s->cachedUbo = std::move(it->cachedUbo);
                    s->fillArgs = std::move(it->fillArgs);
                    native_->scratchIdle.erase(it);
                    break;
                }
            }
            // outPoints: exact-size reuse, but only while the pool is the
            // sole owner — a caller-held Output keeps its buffer shared,
            // and then the pose allocates fresh below. A buffer the pool
            // owns alone has no caller reader: new Outputs only come from
            // live states, never from the pool.
            for (auto it = native_->outPointsIdle.begin();
                 it != native_->outPointsIdle.end(); ++it) {
                if ((*it)->sizeBytes() == outBytes && (*it).use_count() == 1) {
                    s->outPoints = std::move(*it);
                    native_->outPointsIdle.erase(it);
                    break;
                }
            }
        }

        // outPoints: DEVICE_LOCAL.
        {
            if (!s->outPoints) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = outBytes;
                bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                s->outPoints = ChargedBuffer::Create(context, bi,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, UsdGenExecutionResourceKind::Active, &r);
                if (!s->outPoints) { finish(r); return {}; }
            }
        }
        // status: 4B host-visible.
        {
            if (!s->status) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = 4;
                bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                s->status = ChargedBuffer::Create(context, bi,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    UsdGenExecutionResourceKind::Scratch, &r);
                if (!s->status) { finish(r); return {}; }
            }
            void* data = nullptr;
            r = vkMapMemory(d, s->status->memory(), 0, 4, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memset(data, 0, 4);
            vkUnmapMemory(d, s->status->memory());
        }
        // evalStatus: 4B host-visible.
        {
            if (!s->evalStatus) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = 4;
                bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                s->evalStatus = ChargedBuffer::Create(context, bi,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    UsdGenExecutionResourceKind::Scratch, &r);
                if (!s->evalStatus) { finish(r); return {}; }
            }
            void* data = nullptr;
            r = vkMapMemory(d, s->evalStatus->memory(), 0, 4, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memset(data, 0, 4);
            vkUnmapMemory(d, s->evalStatus->memory());
        }

        // Empty topology: early-out.
        if (curves == 0) {
            s->proved = true;
            s->semantic = 0;
            finish(VK_SUCCESS);
            return candidate;
        }

        // Cached-R decision. The whole cached proof phase below (uploads,
        // descriptors, recording, submit, wait, bookkeeping) runs under
        // cacheMutex so a force commit is atomic with its fill; early
        // returns unlock via the guard's destructor. The direct path only
        // borrows the mutex for the bypass countdown.
        bool useCache = false;
        bool forceFill = false;
        uint32_t cacheActive = 0;
        std::unique_lock<std::mutex> cacheLock(native_->cacheMutex, std::defer_lock);
        if (native_->cachedPipeline != VK_NULL_HANDLE &&
            !disableEvalCache.load(std::memory_order_relaxed)) {
            cacheLock.lock();
            auto& cache = native_->evalCache;
            cacheActive = std::min(points, kEvalCachePrefixCVs);
            if (cache.bypassLeft > 0) {
                cache.bypassLeft--;
                evalCacheBypassed.fetch_add(1, std::memory_order_relaxed);
                cacheLock.unlock();
            } else {
                // Ensure the R rows and the proof snapshot (lazy: the first
                // cached pose funds them, later poses reuse; R grows to a
                // high-water n and shrinks never reallocate). Any failure
                // runs this pose direct and retries next pose; whatever
                // succeeded stays funded.
                VkResult cr = VK_SUCCESS;
                if (!cache.proof) {
                    VkBufferCreateInfo bi{};
                    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                    bi.size = VkDeviceSize(kEvalCachePrefixCVs) * 12u;
                    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    cache.proof = ChargedBuffer::Create(context, bi,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        UsdGenExecutionResourceKind::Cache, &cr);
                }
                if (cache.proof && n > cache.rowsAlloc) {
                    VkBufferCreateInfo bi{};
                    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                    bi.size = VkDeviceSize(kEvalCachePrefixCVs) *
                        VkDeviceSize(n) * 8u;
                    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    auto grown = ChargedBuffer::Create(context, bi,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        UsdGenExecutionResourceKind::Cache, &cr);
                    if (grown) {
                        cache.r = std::move(grown);
                        cache.rowsAlloc = n;
                    }
                }
                if (!cache.proof || !cache.r || n > cache.rowsAlloc) {
                    evalCacheUnfunded.fetch_add(1, std::memory_order_relaxed);
                    cacheLock.unlock();
                } else {
                    size_t const restBytes = info.restSamples.size() * sizeof(float);
                    bool const restMatch = cache.everFilled &&
                        cache.rest.size() == info.restSamples.size() &&
                        std::memcmp(cache.rest.data(), info.restSamples.data(),
                                   restBytes) == 0;
                    forceFill = !cache.everFilled || !restMatch ||
                        n != cache.n || cacheActive > cache.filled;
                    useCache = true;
                    // Stays locked through the proof phase below.
                }
            }
        }

        // warped: DEVICE_LOCAL.
        {
            if (!s->warped) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = outBytes;
                bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                s->warped = ChargedBuffer::Create(context, bi,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, UsdGenExecutionResourceKind::Active, &r);
                if (!s->warped) { finish(r); return {}; }
            }
        }
        // coefBuf: HOST_VISIBLE, 3*m doubles. Fresh buffers are allocated
        // at the max so the set rejoins the pool at any sample count; the
        // upload and the descriptor range stay at this pose's actual bytes.
        VkDeviceSize const coefBytes = VkDeviceSize(m) * 24u;
        {
            if (!s->coefBuf) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = std::max(coefBytes, kMaxCoefBytes);
                bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                s->coefBuf = ChargedBuffer::Create(context, bi,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    UsdGenExecutionResourceKind::Scratch, &r);
                if (!s->coefBuf) { finish(r); return {}; }
            }
            void* data = nullptr;
            r = vkMapMemory(d, s->coefBuf->memory(), 0, coefBytes, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memcpy(data, rstate.coef, coefBytes);
            vkUnmapMemory(d, s->coefBuf->memory());
        }
        // samplesBuf: HOST_VISIBLE, 3*n doubles, centered on the host.
        // The evaluate shader used to reload every float sample and subtract
        // the center for every CV; the stored doubles are bitwise what that
        // computed (double(float) - center). The * invScale stays in the
        // shader deliberately: the driver FMA-contracts x - sn*invScale, and
        // keeping the expression shape keeps that contraction (and the bits)
        // identical.
        VkDeviceSize const samplesBytes = VkDeviceSize(n) * 24u;
        {
            if (!s->samplesBuf) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = std::max(samplesBytes, kMaxSamplesBytes);
                bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                s->samplesBuf = ChargedBuffer::Create(context, bi,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    UsdGenExecutionResourceKind::Scratch, &r);
                if (!s->samplesBuf) { finish(r); return {}; }
            }
            std::vector<double> norm(size_t(3) * size_t(n));
            for (int j = 0; j < n; ++j) {
                norm[size_t(3 * j)] = double(info.restSamples[size_t(3 * j)]) - rstate.center[0];
                norm[size_t(3 * j + 1)] = double(info.restSamples[size_t(3 * j + 1)]) - rstate.center[1];
                norm[size_t(3 * j + 2)] = double(info.restSamples[size_t(3 * j + 2)]) - rstate.center[2];
            }
            void* data = nullptr;
            r = vkMapMemory(d, s->samplesBuf->memory(), 0, samplesBytes, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memcpy(data, norm.data(), size_t(samplesBytes));
            vkUnmapMemory(d, s->samplesBuf->memory());
        }
        // evalUbo: HOST_VISIBLE, 64B.
        {
            if (!s->evalUbo) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = kEvalUboBytes;
                bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                s->evalUbo = ChargedBuffer::Create(context, bi,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    UsdGenExecutionResourceKind::Scratch, &r);
                if (!s->evalUbo) { finish(r); return {}; }
            }
            EvalUbo e;
            e.n_m[0] = n; e.n_m[1] = m;
            // On the cached path this UBO feeds only the past-prefix
            // suffix dispatch (base=P); the direct dispatch keeps base=0.
            e.counts[0] = points;
            e.counts[1] = useCache ? int32_t(kEvalCachePrefixCVs) : 0;
            e.cx = rstate.center[0]; e.cy = rstate.center[1]; e.cz = rstate.center[2];
            e.invScale = invScale;
            e.scale = rstate.scale;
            e._pad2 = 0.0;
            void* data = nullptr;
            r = vkMapMemory(d, s->evalUbo->memory(), 0, kEvalUboBytes, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memcpy(data, &e, sizeof(e));
            vkUnmapMemory(d, s->evalUbo->memory());
        }
        // applyUbo: HOST_VISIBLE, 80B.
        {
            if (!s->applyUbo) {
                VkBufferCreateInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = kApplyUboBytes;
                bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                s->applyUbo = ChargedBuffer::Create(context, bi,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    UsdGenExecutionResourceKind::Scratch, &r);
                if (!s->applyUbo) { finish(r); return {}; }
            }
            ApplyUbo a = {};
            a.counts[0] = curves; a.counts[1] = points; a.counts[2] = n;
            a.groomEnvelope = info.groomEnvelope;
            a.maskF[0] = info.mask.literal;
            a.maskF[1] = info.mask.data ? 1.0f : 0.0f;
            a.maskF[2] = float(info.mask.domain);
            a.enabledF[0] = float(info.enabled.literal);
            a.enabledF[1] = info.enabled.data ? 1.0f : 0.0f;
            a.enabledF[2] = float(info.enabled.domain);
            a.lockF[0] = float(info.lockRoots.literal);
            a.lockF[1] = info.lockRoots.data ? 1.0f : 0.0f;
            a.lockF[2] = float(info.lockRoots.domain);
            a.fieldCounts[0] = info.mask.count;
            a.fieldCounts[1] = info.enabled.count;
            a.fieldCounts[2] = info.lockRoots.count;
            // Uniform-stride candidate for the apply shader's point->curve
            // map (verified against the offsets at every use; 0 disables
            // it). Pure scalar math on already-known counts: no producer
            // trust, no scan. The empty-topology pose returns before this
            // fill, so curves is nonzero; the guard is belt and braces.
            a.strideCandidate =
                curves > 0 && points % curves == 0 ? points / curves : 0;
            void* data = nullptr;
            r = vkMapMemory(d, s->applyUbo->memory(), 0, kApplyUboBytes, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memcpy(data, &a, sizeof(a));
            vkUnmapMemory(d, s->applyUbo->memory());
        }
        // Cache per-pose scratch: the three cache UBOs plus the indirect
        // fill args. Allocated on cache-capable pipelines even for direct
        // poses (so pooled sets stay complete); filled below only when
        // this pose runs cached.
        bool const cacheCapable = native_->cachedPipeline != VK_NULL_HANDLE;
        auto allocUbo = [&](std::shared_ptr<ChargedBuffer>& slot, uint32_t bytes,
                            VkBufferUsageFlags usage) {
            if (slot || !cacheCapable) return true;
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = bytes;
            bi.usage = usage;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            slot = ChargedBuffer::Create(context, bi,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                UsdGenExecutionResourceKind::Scratch, &r);
            return bool(slot);
        };
        if (!allocUbo(s->verifyUbo, kVerifyUboBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
            !allocUbo(s->fillUbo, kFillUboBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
            !allocUbo(s->cachedUbo, kCachedUboBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
            !allocUbo(s->fillArgs, kFillArgsBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) {
            finish(r);
            return {};
        }
        if (useCache) {
            // The verify/fill/cached UBO contents plus the indirect args
            // pre-write: (0,1,1) speculates (a verify match leaves a no-op
            // fill), (fillGroups,n,1) forces the refill.
            VerifyUbo v;
            v.words_n[0] = int32_t(3u * cacheActive);
            v.words_n[1] = n;
            v.words_n[2] = 0; v.words_n[3] = 0;
            FillUbo f;
            f.shape[0] = n; f.shape[1] = int32_t(cacheActive);
            f.shape[2] = int32_t(kEvalCachePrefixCVs); f.shape[3] = 0;
            f.cx = rstate.center[0]; f.cy = rstate.center[1]; f.cz = rstate.center[2];
            f.invScale = invScale;
            CachedUbo c;
            c.nmcs[0] = n; c.nmcs[1] = m;
            c.nmcs[2] = int32_t(cacheActive); c.nmcs[3] = int32_t(kEvalCachePrefixCVs);
            c.cx = rstate.center[0]; c.cy = rstate.center[1]; c.cz = rstate.center[2];
            c.invScale = invScale;
            c.scale = rstate.scale;
            c._pad = 0.0;
            uint32_t args[3];
            // The divisor is the fill width, matching the verify shader's
            // mismatch write exactly (same formula, same values).
            args[0] = forceFill ? (cacheActive + kFillLocalSize - 1u) / kFillLocalSize : 0u;
            args[1] = forceFill ? uint32_t(n) : 1u;
            args[2] = 1u;
            struct Fill { std::shared_ptr<ChargedBuffer> const* slot; void const* data; uint32_t bytes; };
            Fill const fills[] = {
                {&s->verifyUbo, &v, kVerifyUboBytes},
                {&s->fillUbo, &f, kFillUboBytes},
                {&s->cachedUbo, &c, kCachedUboBytes},
                {&s->fillArgs, &args, kFillArgsBytes},
            };
            for (auto const& fill : fills) {
                void* data = nullptr;
                r = vkMapMemory(d, (*fill.slot)->memory(), 0, fill.bytes, 0, &data);
                if (r != VK_SUCCESS) { finish(r); return {}; }
                std::memcpy(data, fill.data, fill.bytes);
                vkUnmapMemory(d, (*fill.slot)->memory());
            }
        }

        // Descriptor pool + sets.
        bool const hasSuffix = useCache && points > kEvalCachePrefixCVs;
        VkDescriptorPoolSize sizes[2] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, useCache
                 ? kVerifyFillStorage + kVerifyFillStorage + kEvalStorage +
                       (hasSuffix ? kEvalStorage : 0u) + kApplyStorage
                 : kEvalStorage + kApplyStorage},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, useCache
                 ? (hasSuffix ? 5u : 4u)
                 : 2u},
        };
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = useCache ? (hasSuffix ? 5u : 4u) : 2u;
        dp.poolSizeCount = 2; dp.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(d, &dp, nullptr, &s->descriptors);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        auto allocSet = [&](VkDescriptorSetLayout layout, VkDescriptorSet* set) {
            VkDescriptorSetAllocateInfo da{};
            da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            da.descriptorPool = s->descriptors;
            da.descriptorSetCount = 1; da.pSetLayouts = &layout;
            return vkAllocateDescriptorSets(d, &da, set);
        };
        VkDescriptorSet evalSet = VK_NULL_HANDLE, applySet = VK_NULL_HANDLE;
        VkDescriptorSet verifySet = VK_NULL_HANDLE, fillSet = VK_NULL_HANDLE;
        VkDescriptorSet cachedSet = VK_NULL_HANDLE, suffixSet = VK_NULL_HANDLE;
        if (useCache) {
            r = allocSet(native_->verifyFillLayout, &verifySet);
            if (r == VK_SUCCESS) r = allocSet(native_->verifyFillLayout, &fillSet);
            if (r == VK_SUCCESS) r = allocSet(native_->evalLayout, &cachedSet);
            if (r == VK_SUCCESS && hasSuffix) r = allocSet(native_->evalLayout, &suffixSet);
            if (r == VK_SUCCESS) r = allocSet(native_->applyLayout, &applySet);
        } else {
            r = allocSet(native_->evalLayout, &evalSet);
            if (r == VK_SUCCESS) r = allocSet(native_->applyLayout, &applySet);
        }
        if (r != VK_SUCCESS) { finish(r); return {}; }

        // Fallback planes for null field data.
        auto applyPlane = [&](std::shared_ptr<ChargedBuffer const> const& b, VkDeviceSize bytes)
            -> VkDescriptorBufferInfo {
            if (b && bytes) return {b->buffer(), 0, bytes};
            return {s->status->buffer(), 0, 4u};
        };

        VkDeviceSize const pointsBytes = points ? VkDeviceSize(points) * 12u : 4u;
        VkDeviceSize const offsetsBytes = VkDeviceSize(curves + 1u) * 4u;
        VkDeviceSize const targetsBytes = VkDeviceSize(curves) * 12u;

        // Eval set: b0=cvs, b1=warped, b2=samples, b3=coef, b4=evalUbo, b5=evalStatus.
        auto writeEvalLayoutSet = [&](VkDescriptorSet set, VkDescriptorBufferInfo rOrSamples,
                                      VkDescriptorBufferInfo ubo) {
            VkDescriptorBufferInfo eInfos[kEvalStorage + 1] = {
                applyPlane(info.points, pointsBytes),
                {s->warped->buffer(), 0, outBytes},
                rOrSamples,
                {s->coefBuf->buffer(), 0, coefBytes},
                ubo,
                {s->evalStatus->buffer(), 0, 4u},
            };
            VkWriteDescriptorSet eWrites[kEvalStorage + 1]{};
            uint32_t const eBindings[kEvalStorage + 1] = {0, 1, 2, 3, 4, 5};
            for (uint32_t i = 0; i < kEvalStorage + 1; ++i) {
                eWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                eWrites[i].dstSet = set; eWrites[i].dstBinding = eBindings[i];
                eWrites[i].descriptorCount = 1;
                eWrites[i].descriptorType = (i == 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                     : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                eWrites[i].pBufferInfo = &eInfos[i];
            }
            vkUpdateDescriptorSets(d, kEvalStorage + 1, eWrites, 0, nullptr);
        };
        auto writeVerifyFillSet = [&](VkDescriptorSet set, VkDescriptorBufferInfo b0,
                                      VkDescriptorBufferInfo b1, VkDescriptorBufferInfo b2,
                                      VkDescriptorBufferInfo ubo) {
            VkDescriptorBufferInfo infos[kVerifyFillStorage + 1] = {b0, b1, b2, ubo};
            VkWriteDescriptorSet writes[kVerifyFillStorage + 1]{};
            for (uint32_t i = 0; i < kVerifyFillStorage + 1; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set; writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType =
                    (i == kVerifyFillStorage) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &infos[i];
            }
            vkUpdateDescriptorSets(d, kVerifyFillStorage + 1, writes, 0, nullptr);
        };
        if (useCache) {
            auto const& cache = native_->evalCache;
            // Verify set: b0=cvs, b1=proof, b2=fillArgs, b3=verifyUbo.
            writeVerifyFillSet(verifySet, applyPlane(info.points, pointsBytes),
                               {cache.proof->buffer(), 0, cache.proof->sizeBytes()},
                               {s->fillArgs->buffer(), 0, kFillArgsBytes},
                               {s->verifyUbo->buffer(), 0, kVerifyUboBytes});
            // Fill set: b0=cvs, b1=R, b2=samples, b3=fillUbo.
            writeVerifyFillSet(fillSet, applyPlane(info.points, pointsBytes),
                               {cache.r->buffer(), 0, cache.r->sizeBytes()},
                               {s->samplesBuf->buffer(), 0, samplesBytes},
                               {s->fillUbo->buffer(), 0, kFillUboBytes});
            // Cached set: b0=cvs, b1=warped, b2=R, b3=coef, b4=cachedUbo, b5=evalStatus.
            writeEvalLayoutSet(cachedSet, {cache.r->buffer(), 0, cache.r->sizeBytes()},
                               {s->cachedUbo->buffer(), 0, kCachedUboBytes});
            if (hasSuffix) {
                // Suffix set: the direct eval over [P,points).
                writeEvalLayoutSet(suffixSet,
                                   {s->samplesBuf->buffer(), 0, samplesBytes},
                                   {s->evalUbo->buffer(), 0, kEvalUboBytes});
            }
        } else {
            writeEvalLayoutSet(evalSet, {s->samplesBuf->buffer(), 0, samplesBytes},
                               {s->evalUbo->buffer(), 0, kEvalUboBytes});
        }

        // Apply set: b0=src, b1=warped, b2=offsets, b3=targets, b4=outPoints,
        //            b5=maskData, b6=enabledData, b7=lockData, b8=status, b9=applyUbo.
        VkDescriptorBufferInfo aInfos[kApplyStorage + 1] = {
            applyPlane(info.points, pointsBytes),
            {s->warped->buffer(), 0, outBytes},
            applyPlane(info.curveOffsets, offsetsBytes),
            applyPlane(info.rootTargets, targetsBytes),
            {s->outPoints->buffer(), 0, outBytes},
            applyPlane(info.mask.data,
                info.mask.count ? VkDeviceSize(info.mask.count) * 4u : 4u),
            applyPlane(info.enabled.data,
                info.enabled.count ? VkDeviceSize(info.enabled.count) * 4u : 4u),
            applyPlane(info.lockRoots.data,
                info.lockRoots.count ? VkDeviceSize(info.lockRoots.count) * 4u : 4u),
            {s->status->buffer(), 0, 4u},
            {s->applyUbo->buffer(), 0, kApplyUboBytes},
        };
        VkWriteDescriptorSet aWrites[kApplyStorage + 1]{};
        for (uint32_t i = 0; i < kApplyStorage + 1; ++i) {
            aWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            aWrites[i].dstSet = applySet; aWrites[i].dstBinding = i;
            aWrites[i].descriptorCount = 1;
            aWrites[i].descriptorType = (i == kApplyStorage) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            aWrites[i].pBufferInfo = &aInfos[i];
        }
        vkUpdateDescriptorSets(d, kApplyStorage + 1, aWrites, 0, nullptr);

        // Command pool + buffers.
        VkCommandPoolCreateInfo pc{};
        pc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pc.queueFamilyIndex = context->computeQueueFamily();
        r = vkCreateCommandPool(d, &pc, nullptr, &s->commands);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        VkCommandBufferAllocateInfo ca{};
        ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ca.commandPool = s->commands; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 2;
        VkCommandBuffer cmds[2];
        r = vkAllocateCommandBuffers(d, &ca, cmds);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        // cmd0: evaluate (proof).
        r = vkBeginCommandBuffer(cmds[0], &begin);
        if (r == VK_SUCCESS) {
            VkMemoryBarrier before{};
            before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmds[0],
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
            if (useCache) {
                // Verify the prefix CVs (snapshotting them into the
                // proof), self-schedule the refill through the indirect
                // args, then evaluate the prefix through R and the suffix
                // directly.
                vkCmdBindPipeline(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                    native_->verifyPipeline);
                vkCmdBindDescriptorSets(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                    native_->verifyFillPipelineLayout, 0, 1, &verifySet, 0, nullptr);
                vkCmdDispatch(cmds[0], Groups(3u * cacheActive, kVerifyLocalSize), 1, 1);
                VkMemoryBarrier verifyToFill{};
                verifyToFill.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                verifyToFill.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                verifyToFill.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
                vkCmdPipelineBarrier(cmds[0], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &verifyToFill,
                    0, nullptr, 0, nullptr);
                vkCmdBindPipeline(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                    native_->fillPipeline);
                vkCmdBindDescriptorSets(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                    native_->verifyFillPipelineLayout, 0, 1, &fillSet, 0, nullptr);
                vkCmdDispatchIndirect(cmds[0], s->fillArgs->buffer(), 0);
                VkMemoryBarrier fillToCached{};
                fillToCached.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                fillToCached.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                fillToCached.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmds[0], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fillToCached,
                    0, nullptr, 0, nullptr);
                vkCmdBindPipeline(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                    native_->cachedPipeline);
                vkCmdBindDescriptorSets(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                    native_->evalPipelineLayout, 0, 1, &cachedSet, 0, nullptr);
                vkCmdDispatch(cmds[0], Groups(cacheActive, kCachedLocalSize), 1, 1);
                if (hasSuffix) {
                    vkCmdBindPipeline(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                        native_->evalPipeline);
                    vkCmdBindDescriptorSets(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                        native_->evalPipelineLayout, 0, 1, &suffixSet, 0, nullptr);
                    vkCmdDispatch(cmds[0],
                        Groups(points - kEvalCachePrefixCVs, kEvalLocalSize), 1, 1);
                }
            } else {
                vkCmdBindPipeline(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                    native_->evalPipeline);
                vkCmdBindDescriptorSets(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                    native_->evalPipelineLayout, 0, 1, &evalSet, 0, nullptr);
                vkCmdDispatch(cmds[0], Groups(points, kEvalLocalSize), 1, 1);
            }
            VkMemoryBarrier after{};
            after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                  VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmds[0], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &after, 0, nullptr, 0, nullptr);
            r = vkEndCommandBuffer(cmds[0]);
        }
        if (r != VK_SUCCESS) { finish(r); return {}; }
        // cmd1 is recorded before the eval proof runs, not after it: the
        // recording touches only host-side handles and counts (applySet,
        // the pipelines, curves/points), so it needs nothing the eval
        // produces, and the driver record calls then overlap the 13ms eval
        // instead of serializing behind the proof wait. A failed proof
        // discards the recording with the command pool; the submit below
        // still gates on the proof, the fence, and beforeSubmit.
        // No zero-fill: every apply thread writes its point or reports a
        // status first (each early return calls StatusErr), so status 0
        // implies every point was written, and any other status discards
        // the output; the fill is never read.
        r = vkBeginCommandBuffer(cmds[1], &begin);
        if (r == VK_SUCCESS) {
            VkMemoryBarrier before{};
            before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmds[1],
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(cmds[1], VK_PIPELINE_BIND_POINT_COMPUTE,
                native_->applyPipeline);
            vkCmdBindDescriptorSets(cmds[1], VK_PIPELINE_BIND_POINT_COMPUTE,
                native_->applyPipelineLayout, 0, 1, &applySet, 0, nullptr);
            // Per-point apply: one thread per point (plus curve-span
            // validation for thread i < curves), so the dispatch covers
            // whichever domain is larger.
            vkCmdDispatch(cmds[1], Groups(std::max(curves, points), kApplyLocalSize), 1, 1);
            VkMemoryBarrier after{};
            after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                  VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmds[1], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &after, 0, nullptr, 0, nullptr);
            r = vkEndCommandBuffer(cmds[1]);
        }
        if (r != VK_SUCCESS) { finish(r); return {}; }
        s->proofFence = native_->fencePool.Acquire(d, &r);
        if (!s->proofFence) { finish(r); return {}; }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &cmds[0];
        s->pending = true; // proof owns the same allocations as the final submit
        r = vkQueueSubmit(context->computeQueue(), 1, &submit, s->proofFence);
        if (r == VK_SUCCESS)
            r = vkWaitForFences(d, 1, &s->proofFence, VK_TRUE, 10000000000ull);
        if (r != VK_SUCCESS) { candidate->Quarantine(); finish(r); return {}; }
        s->pending = false;
        uint32_t st = 0;
        if (r == VK_SUCCESS) {
            void* data = nullptr;
            r = vkMapMemory(d, s->evalStatus->memory(), 0, 4, 0, &data);
            if (r == VK_SUCCESS) {
                std::memcpy(&st, data, 4);
                vkUnmapMemory(d, s->evalStatus->memory());
            }
        }
        // The indirect args double as the miss signal: nonzero x means the
        // refill ran (forced or by mismatch). A free 12B read on the
        // already-synchronized proof path.
        uint32_t argsX = 0;
        if (useCache && r == VK_SUCCESS) {
            void* data = nullptr;
            r = vkMapMemory(d, s->fillArgs->memory(), 0, kFillArgsBytes, 0, &data);
            if (r == VK_SUCCESS) {
                std::memcpy(&argsX, data, 4);
                vkUnmapMemory(d, s->fillArgs->memory());
            }
        }
        // The proof wait above succeeded, so the fence is idle and rejoins
        // the pool even when the status readback fails.
        native_->fencePool.Release(d, s->proofFence);
        s->proofFence = VK_NULL_HANDLE;
        if (r != VK_SUCCESS) { finish(r); return {}; }
        if (useCache) {
            // Still under cacheMutex: commit the force (R/proof now hold
            // this pose's fill) and account the hit/miss. The commit is
            // correct even for a NonFinite pose (the fill ran regardless;
            // NaN inputs fill NaN words the cached evaluator flags on).
            auto& cache = native_->evalCache;
            if (forceFill) {
                cache.rest = info.restSamples;
                cache.n = n;
                cache.filled = cacheActive;
                cache.everFilled = true;
            }
            if (argsX != 0) {
                evalCacheMisses.fetch_add(1, std::memory_order_relaxed);
                if (++cache.missStreak >= kEvalCacheBypassMisses) {
                    cache.bypassLeft = kEvalCacheBypassPoses;
                    cache.missStreak = 0;
                }
            } else {
                evalCacheHits.fetch_add(1, std::memory_order_relaxed);
                cache.missStreak = 0;
            }
            // End of the cached proof phase; the apply submit below (and
            // its beforeSubmit hook) runs unlocked, as before.
            cacheLock.unlock();
        }
        if (st != 0) return reject(DeformSemantic::NonFinite);

        // cmd1 was recorded before the eval submit (see above); the submit
        // below still gates on the proof, the fence, and beforeSubmit.
        s->fence = native_->fencePool.Acquire(d, &r);
        if (!s->fence) { finish(r); return {}; }
        if (beforeSubmit) {
            bool admitted = false;
            try { admitted = beforeSubmit(); }
            catch (...) { finish(VK_ERROR_UNKNOWN); return {}; }
            if (!admitted) { finish(VK_ERROR_OUT_OF_DEVICE_MEMORY); return {}; }
        }
        s->pending = true;
        submit.pCommandBuffers = &cmds[1];
        r = vkQueueSubmit(context->computeQueue(), 1, &submit, s->fence);
        if (r != VK_SUCCESS) { candidate->Quarantine(); finish(r); return {}; }
        finish(VK_SUCCESS);
        return candidate;
    } catch (std::bad_alloc const&) { finish(VK_ERROR_OUT_OF_HOST_MEMORY); return {}; }
}

} // namespace usdGen::vulkan
