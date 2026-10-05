// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

#include "rbfVkBinding.h"
#include "deformRbfHost.h"
#include "fencePool.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <new>

namespace usdGen::vulkan {

char const* RbfVkStatusName(RbfVkStatus s) noexcept {
    switch (s) {
        case RbfVkStatus::Ok:              return "Ok";
        case RbfVkStatus::InvalidArgument: return "InvalidArgument";
        case RbfVkStatus::NonFiniteInput:  return "NonFiniteInput";
        case RbfVkStatus::RankDeficient:   return "RankDeficient";
        case RbfVkStatus::DeviceError:     return "DeviceError";
        case RbfVkStatus::SolverError:     return "SolverError";
    }
    return "Invalid";
}

namespace {

constexpr uint32_t kLocalSize = 256;
constexpr uint32_t kMaxEvalStack = 16;
constexpr uint64_t kFenceTimeoutNs = 10000000000ull;

// Shared 64-byte solve UBO: identical declaration in all seven solve/eval
// shaders (each reads its subset at these offsets).
struct SolveUbo {
    int32_t n = 0, m = 0, count = 0, reserved = 0;
    double cx = 0, cy = 0, cz = 0, invScale = 1, scale = 1, lambda = 0;
};
static_assert(sizeof(SolveUbo) == 64);
static_assert(offsetof(SolveUbo, cx) == 16);

uint32_t Groups(uint32_t n) { return (n + kLocalSize - 1) / kLocalSize; }

void BeforeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
}

void AfterBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 1, &after, 0, nullptr, 0, nullptr);
}

VkResult ReadBytes(VkDevice device, ChargedBuffer const& buffer,
                   VkDeviceSize bytes, void* out) {
    void* data = nullptr;
    VkResult r = vkMapMemory(device, buffer.memory(), 0, bytes, 0, &data);
    if (r != VK_SUCCESS) return r;
    std::memcpy(out, data, size_t(bytes));
    vkUnmapMemory(device, buffer.memory());
    return VK_SUCCESS;
}

VkResult WriteBytes(VkDevice device, ChargedBuffer const& buffer,
                    VkDeviceSize bytes, void const* in) {
    void* data = nullptr;
    VkResult r = vkMapMemory(device, buffer.memory(), 0, bytes, 0, &data);
    if (r != VK_SUCCESS) return r;
    std::memcpy(data, in, size_t(bytes));
    vkUnmapMemory(device, buffer.memory());
    return VK_SUCCESS;
}

// HostExtent — exact host port of rbfVkExtent.comp: min/max over the
// finite rest samples (non-finite samples set the flag and are skipped,
// as the shader's early return does). Min/max over floats involves no
// rounding, so the host fold matches the device atomics bit for bit,
// except for mixed-sign zeros, which cannot observably diverge: an
// all-zero extent is rank-deficient either way, and adding +-0 to a
// nonzero bound is exact. (The gram/matrix/LU stay on the device: the
// software rasterizer's sqrt/FMA codegen differs from the host
// compiler's by 1 ULP, so a host factorization cannot be bit-identical.)
void HostExtent(float const* rest, int n, float e[6], int* flag) {
    e[0] = e[1] = e[2] = HUGE_VALF;
    e[3] = e[4] = e[5] = -HUGE_VALF;
    *flag = 0;
    for (int i = 0; i < n; ++i) {
        float const x = rest[size_t(3) * size_t(i)];
        float const y = rest[size_t(3) * size_t(i) + 1];
        float const z = rest[size_t(3) * size_t(i) + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            *flag = 1;
            continue;
        }
        e[0] = std::min(e[0], x); e[1] = std::min(e[1], y); e[2] = std::min(e[2], z);
        e[3] = std::max(e[3], x); e[4] = std::max(e[4], y); e[5] = std::max(e[5], z);
    }
}

} // namespace

struct RbfVkBinding::Native {
    std::shared_ptr<DeviceContext> context;
    VkShaderModule extentShader = VK_NULL_HANDLE, gramShader = VK_NULL_HANDLE,
        buildMatrixShader = VK_NULL_HANDLE, luShader = VK_NULL_HANDLE,
        rhsShader = VK_NULL_HANDLE, triSolveShader = VK_NULL_HANDLE,
        evaluateShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout extentLayout = VK_NULL_HANDLE, gramLayout = VK_NULL_HANDLE,
        buildMatrixLayout = VK_NULL_HANDLE, luLayout = VK_NULL_HANDLE,
        rhsLayout = VK_NULL_HANDLE, triSolveLayout = VK_NULL_HANDLE,
        evaluateLayout = VK_NULL_HANDLE;
    VkPipelineLayout extentPipelineLayout = VK_NULL_HANDLE, gramPipelineLayout = VK_NULL_HANDLE,
        buildMatrixPipelineLayout = VK_NULL_HANDLE, luPipelineLayout = VK_NULL_HANDLE,
        rhsPipelineLayout = VK_NULL_HANDLE, triSolvePipelineLayout = VK_NULL_HANDLE,
        evaluatePipelineLayout = VK_NULL_HANDLE;
    VkPipeline extentPipeline = VK_NULL_HANDLE, gramPipeline = VK_NULL_HANDLE,
        buildMatrixPipeline = VK_NULL_HANDLE, luPipeline = VK_NULL_HANDLE,
        rhsPipeline = VK_NULL_HANDLE, triSolvePipeline = VK_NULL_HANDLE,
        evaluatePipeline = VK_NULL_HANDLE;
    std::shared_ptr<ChargedBuffer> restBuf, posedBuf, matrixBuf, rhsBuf, coefBuf,
        gramBuf, extentBuf, flagBuf, infoBuf, permBuf, uboBuf, normBuf, coefStagingBuf;
    VkDescriptorPool solvePool = VK_NULL_HANDLE, evalPool = VK_NULL_HANDLE;
    VkDescriptorSet extentSet = VK_NULL_HANDLE, gramSet = VK_NULL_HANDLE,
        buildMatrixSet = VK_NULL_HANDLE, luSet = VK_NULL_HANDLE,
        rhsSet = VK_NULL_HANDLE, triSolveSet = VK_NULL_HANDLE;
    VkCommandPool solveCommands = VK_NULL_HANDLE, evalCommands = VK_NULL_HANDLE;
    VkFence solveFence = VK_NULL_HANDLE;
    std::vector<VkFence> evalFences;
    std::vector<std::shared_ptr<const ChargedBuffer>> evalOwners;
    // Idle eval-submit fences: fence create/destroy costs ~0.7ms each on
    // the qualified driver, so per-evaluation submits check out of this
    // pool instead of creating.
    VulkanFencePool fencePool;
    bool solvePending = false;
    enum class Phase { Idle, Extent, FactorReady, Factor, Pose };
    Phase phase = Phase::Idle;
    SolveUbo solveUbo;
    VkResult lastResult = VK_SUCCESS;
    int bufferSamples = 0;
    // Rest-only host state for the host-side pose solve: the factored
    // matrix and permutation (read back once per bind) plus the rest
    // samples (copied from BeginBind). A pose then pays no submit, fence,
    // or re-read; the bytes and the arithmetic match the retired rhs +
    // triSolve submits exactly.
    std::vector<double> cachedLu;
    std::vector<int> cachedPerm;
    std::vector<float> cachedRest;
    int cachedM = 0;
    bool cachedLuValid = false;
    // A staged host pose: BeginSolve stashes the posed samples and the
    // admission hook runs there, but the RHS + triangular solve wait for
    // PollSolve, so the Begin/Poll pending protocol is unchanged.
    std::vector<float> hostPosed;
    bool hostPosePending = false;
    // A staged host extent: BeginBind stashes nothing beyond the rest
    // samples and runs the admission hook, and PollSolve runs the extent
    // on the host with no fence. solvePending stays the pending flag, so
    // every refusal and HasPendingSolve is unchanged.
    bool hostBindStaged = false;
    // A factor-cache hit: BeginBind adopted stored factors, so AdvanceBind
    // stages without submitting and Factor consume skips the readbacks.
    bool bindAdopted = false;
    void ResetBuffers() noexcept {
        restBuf.reset(); posedBuf.reset(); matrixBuf.reset(); rhsBuf.reset();
        coefBuf.reset(); gramBuf.reset(); extentBuf.reset(); flagBuf.reset();
        infoBuf.reset(); permBuf.reset(); uboBuf.reset(); normBuf.reset();
        coefStagingBuf.reset(); bufferSamples = 0;
        cachedLu.clear(); cachedPerm.clear(); cachedRest.clear();
        cachedM = 0; cachedLuValid = false;
        hostPosed.clear(); hostPosePending = false;
        hostBindStaged = false;
        bindAdopted = false;
    }
    // The owner has proved every previous submit before reallocating or
    // rewriting descriptor sets. Failed admission leaves no partial charge.
    VkResult AllocateBuffers(int count) {
        if (bufferSamples == count) return VK_SUCCESS;
        ResetBuffers();
        int const m = count + 4;
        auto* n = this;
        VkResult r = VK_SUCCESS;
        auto mkBuf = [&](VkDeviceSize bytes, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props,
                         UsdGenExecutionResourceKind kind,
                         std::shared_ptr<ChargedBuffer>* out) {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = bytes;
            bi.usage = usage;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            *out = ChargedBuffer::Create(n->context, bi, props, kind, &r);
            return *out != nullptr;
        };
        VkMemoryPropertyFlags const host =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!mkBuf(VkDeviceSize(count) * 12u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->restBuf) ||
            !mkBuf(VkDeviceSize(count) * 24u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->normBuf) ||
            !mkBuf(VkDeviceSize(count) * 12u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->posedBuf) ||
            !mkBuf(VkDeviceSize(m) * m * 8u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Active, &n->matrixBuf) ||
            !mkBuf(VkDeviceSize(m) * 24u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Active, &n->rhsBuf) ||
            !mkBuf(VkDeviceSize(m) * 24u,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, UsdGenExecutionResourceKind::Active, &n->coefBuf) ||
            !mkBuf(VkDeviceSize(m) * 24u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->coefStagingBuf) ||
            !mkBuf(128u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->gramBuf) ||
            !mkBuf(24u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->extentBuf) ||
            !mkBuf(4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->flagBuf) ||
            !mkBuf(4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->infoBuf) ||
            !mkBuf(VkDeviceSize(m) * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Active, &n->permBuf) ||
            !mkBuf(64u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   host, UsdGenExecutionResourceKind::Scratch, &n->uboBuf)) {
            ResetBuffers();
            return r;
        }

        auto d = context->device();
        auto writeSet = [&](VkDescriptorSet set,
                            std::vector<std::pair<uint32_t, VkDescriptorBufferInfo>> const& infos,
                            uint32_t uboBinding) {
            std::vector<VkWriteDescriptorSet> writes;
            std::vector<VkDescriptorBufferInfo> held;
            for (auto const& [binding, info] : infos) {
                held.push_back(info);
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = set;
                w.dstBinding = binding;
                w.descriptorCount = 1;
                w.descriptorType = (binding == uboBinding) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes.push_back(w);
            }
            for (size_t i = 0; i < writes.size(); ++i) writes[i].pBufferInfo = &held[i];
            vkUpdateDescriptorSets(d, uint32_t(writes.size()), writes.data(), 0, nullptr);
        };
        auto storage = [](std::shared_ptr<ChargedBuffer> const& b) {
            return VkDescriptorBufferInfo{b->buffer(), 0, b->sizeBytes()};
        };
        writeSet(n->extentSet,
                 {{0, storage(n->restBuf)}, {1, storage(n->extentBuf)},
                  {2, storage(n->flagBuf)}, {3, storage(n->uboBuf)}}, 3);
        writeSet(n->gramSet,
                 {{0, storage(n->restBuf)}, {1, storage(n->gramBuf)},
                  {2, storage(n->uboBuf)}}, 2);
        writeSet(n->buildMatrixSet,
                 {{0, storage(n->restBuf)}, {1, storage(n->matrixBuf)},
                  {2, storage(n->uboBuf)}}, 2);
        writeSet(n->luSet,
                 {{0, storage(n->matrixBuf)}, {1, storage(n->infoBuf)},
                  {2, storage(n->permBuf)}, {3, storage(n->uboBuf)}}, 3);
        writeSet(n->rhsSet,
                 {{0, storage(n->restBuf)}, {1, storage(n->posedBuf)},
                  {2, storage(n->rhsBuf)}, {3, storage(n->flagBuf)},
                  {4, storage(n->uboBuf)}}, 4);
        writeSet(n->triSolveSet,
                 {{0, storage(n->matrixBuf)}, {1, storage(n->rhsBuf)},
                  {2, storage(n->coefBuf)}, {3, storage(n->permBuf)},
                  {4, storage(n->uboBuf)}}, 4);

        bufferSamples = count;
        return VK_SUCCESS;
    }

    std::unique_ptr<std::shared_ptr<Native>> quarantine;
    ~Native() {
        if (!context) return;
        auto d = context->device();
        for (VkFence f : evalFences) if (f) vkDestroyFence(d, f, nullptr);
        fencePool.Clear(d);
        if (solveFence) vkDestroyFence(d, solveFence, nullptr);
        if (solveCommands) vkDestroyCommandPool(d, solveCommands, nullptr);
        if (evalCommands) vkDestroyCommandPool(d, evalCommands, nullptr);
        if (solvePool) vkDestroyDescriptorPool(d, solvePool, nullptr);
        if (evalPool) vkDestroyDescriptorPool(d, evalPool, nullptr);
        VkPipeline pipes[] = {extentPipeline, gramPipeline, buildMatrixPipeline,
            luPipeline, rhsPipeline, triSolvePipeline, evaluatePipeline};
        for (VkPipeline p : pipes) if (p) vkDestroyPipeline(d, p, nullptr);
        VkPipelineLayout pls[] = {extentPipelineLayout, gramPipelineLayout,
            buildMatrixPipelineLayout, luPipelineLayout, rhsPipelineLayout,
            triSolvePipelineLayout, evaluatePipelineLayout};
        for (VkPipelineLayout p : pls) if (p) vkDestroyPipelineLayout(d, p, nullptr);
        VkDescriptorSetLayout dss[] = {extentLayout, gramLayout, buildMatrixLayout,
            luLayout, rhsLayout, triSolveLayout, evaluateLayout};
        for (VkDescriptorSetLayout s : dss) if (s) vkDestroyDescriptorSetLayout(d, s, nullptr);
        VkShaderModule sms[] = {extentShader, gramShader, buildMatrixShader,
            luShader, rhsShader, triSolveShader, evaluateShader};
        for (VkShaderModule s : sms) if (s) vkDestroyShaderModule(d, s, nullptr);
    }
};

RbfVkBinding::RbfVkBinding(std::shared_ptr<Native> n) : native_(std::move(n)) {}
RbfVkBinding::~RbfVkBinding() {
    // A failed or unavailable completion proof never authorizes destruction.
    bool pending = native_ && native_->solvePending;
    if (native_ && evalPending_)
        for (VkFence fence : native_->evalFences)
            pending |= vkGetFenceStatus(native_->context->device(), fence) != VK_SUCCESS;
    if (pending) {
        *native_->quarantine = native_;
        (void)native_->quarantine.release();
    }
}
std::shared_ptr<DeviceContext> const& RbfVkBinding::context() const noexcept {
    return native_->context;
}
RbfVkStatus RbfVkBinding::fail(RbfVkStatus s, char const* why) {
    diagnostic_ = why;
    return s;
}
bool RbfVkBinding::HasPendingEvaluate() const noexcept { return evalPending_; }
bool RbfVkBinding::HasPendingSolve() const noexcept {
    return native_->solvePending || native_->hostPosePending;
}
VkResult RbfVkBinding::lastResult() const noexcept { return native_->lastResult; }
int RbfVkBinding::sampleCount() const noexcept { return sampleCount_; }
int RbfVkBinding::order() const noexcept { return order_; }
double RbfVkBinding::center(int axis) const noexcept {
    return center_[axis >= 0 && axis < 3 ? axis : 0];
}
double RbfVkBinding::scale() const noexcept { return scale_; }
char const* RbfVkBinding::diagnostic() const noexcept { return diagnostic_.c_str(); }

std::shared_ptr<RbfVkBinding> RbfVkBinding::Create(
    std::shared_ptr<DeviceContext> context, RbfVkBindingSpirv const& spirv,
    VkResult* result) {
    auto finish = [&](VkResult r) { if (result) *result = r; };
    finish(VK_ERROR_INITIALIZATION_FAILED);
    auto valid = [](std::vector<uint32_t> const& c) {
        return c.size() >= 5 && c.front() == 0x07230203u;
    };
    if (!context || !valid(spirv.extent) || !valid(spirv.gram) ||
        !valid(spirv.buildMatrix) || !valid(spirv.lu) || !valid(spirv.rhs) ||
        !valid(spirv.triSolve) || !valid(spirv.evaluate))
        return {};
    if (!context->shaderFloat64Enabled()) return {};
    VkPhysicalDeviceProperties physical{};
    vkGetPhysicalDeviceProperties(context->physicalDevice(), &physical);
    if (physical.limits.maxComputeWorkGroupInvocations < kLocalSize ||
        physical.limits.maxComputeWorkGroupSize[0] < kLocalSize ||
        physical.limits.maxPerStageDescriptorStorageBuffers < 5 ||
        physical.limits.maxDescriptorSetStorageBuffers < 5)
        return {};
    try {
        auto n = std::make_shared<Native>();
        n->context = std::move(context);
        n->quarantine = std::make_unique<std::shared_ptr<Native>>();
        auto d = n->context->device();
        auto mkModule = [&](std::vector<uint32_t> const& code, VkShaderModule* out) {
            VkShaderModuleCreateInfo sm{};
            sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm.codeSize = code.size() * sizeof(uint32_t);
            sm.pCode = code.data();
            return vkCreateShaderModule(d, &sm, nullptr, out);
        };
        VkResult r = mkModule(spirv.extent, &n->extentShader);
        if (r == VK_SUCCESS) r = mkModule(spirv.gram, &n->gramShader);
        if (r == VK_SUCCESS) r = mkModule(spirv.buildMatrix, &n->buildMatrixShader);
        if (r == VK_SUCCESS) r = mkModule(spirv.lu, &n->luShader);
        if (r == VK_SUCCESS) r = mkModule(spirv.rhs, &n->rhsShader);
        if (r == VK_SUCCESS) r = mkModule(spirv.triSolve, &n->triSolveShader);
        if (r == VK_SUCCESS) r = mkModule(spirv.evaluate, &n->evaluateShader);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        auto mkLayout = [&](uint32_t storage, uint32_t uboBinding,
                            VkDescriptorSetLayout* out) {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            // Storage bindings are dense from 0 except evaluate (0,1,2,3,5).
            uint32_t const extra = (storage == 5) ? 1 : 0;
            for (uint32_t i = 0; i < storage; ++i) {
                VkDescriptorSetLayoutBinding b{};
                b.binding = (extra && i == storage - 1) ? i + 1 : i;
                b.descriptorCount = 1;
                b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                bindings.push_back(b);
            }
            VkDescriptorSetLayoutBinding u{};
            u.binding = uboBinding;
            u.descriptorCount = 1;
            u.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            u.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings.push_back(u);
            VkDescriptorSetLayoutCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ds.bindingCount = uint32_t(bindings.size());
            ds.pBindings = bindings.data();
            return vkCreateDescriptorSetLayout(d, &ds, nullptr, out);
        };
        r = mkLayout(3, 3, &n->extentLayout);
        if (r == VK_SUCCESS) r = mkLayout(2, 2, &n->gramLayout);
        if (r == VK_SUCCESS) r = mkLayout(2, 2, &n->buildMatrixLayout);
        if (r == VK_SUCCESS) r = mkLayout(3, 3, &n->luLayout);
        if (r == VK_SUCCESS) r = mkLayout(4, 4, &n->rhsLayout);
        if (r == VK_SUCCESS) r = mkLayout(4, 4, &n->triSolveLayout);
        if (r == VK_SUCCESS) r = mkLayout(5, 4, &n->evaluateLayout);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        auto mkPipelineLayout = [&](VkDescriptorSetLayout set, VkPipelineLayout* out) {
            VkPipelineLayoutCreateInfo pl{};
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &set;
            return vkCreatePipelineLayout(d, &pl, nullptr, out);
        };
        r = mkPipelineLayout(n->extentLayout, &n->extentPipelineLayout);
        if (r == VK_SUCCESS) r = mkPipelineLayout(n->gramLayout, &n->gramPipelineLayout);
        if (r == VK_SUCCESS) r = mkPipelineLayout(n->buildMatrixLayout, &n->buildMatrixPipelineLayout);
        if (r == VK_SUCCESS) r = mkPipelineLayout(n->luLayout, &n->luPipelineLayout);
        if (r == VK_SUCCESS) r = mkPipelineLayout(n->rhsLayout, &n->rhsPipelineLayout);
        if (r == VK_SUCCESS) r = mkPipelineLayout(n->triSolveLayout, &n->triSolvePipelineLayout);
        if (r == VK_SUCCESS) r = mkPipelineLayout(n->evaluateLayout, &n->evaluatePipelineLayout);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        auto mkPipeline = [&](VkShaderModule module, VkPipelineLayout layout, VkPipeline* out) {
            VkComputePipelineCreateInfo cp{};
            cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cp.layout = layout;
            cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cp.stage.module = module;
            cp.stage.pName = "main";
            return vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cp, nullptr, out);
        };
        r = mkPipeline(n->extentShader, n->extentPipelineLayout, &n->extentPipeline);
        if (r == VK_SUCCESS) r = mkPipeline(n->gramShader, n->gramPipelineLayout, &n->gramPipeline);
        if (r == VK_SUCCESS) r = mkPipeline(n->buildMatrixShader, n->buildMatrixPipelineLayout, &n->buildMatrixPipeline);
        if (r == VK_SUCCESS) r = mkPipeline(n->luShader, n->luPipelineLayout, &n->luPipeline);
        if (r == VK_SUCCESS) r = mkPipeline(n->rhsShader, n->rhsPipelineLayout, &n->rhsPipeline);
        if (r == VK_SUCCESS) r = mkPipeline(n->triSolveShader, n->triSolvePipelineLayout, &n->triSolvePipeline);
        if (r == VK_SUCCESS) r = mkPipeline(n->evaluateShader, n->evaluatePipelineLayout, &n->evaluatePipeline);
        if (r != VK_SUCCESS) { finish(r); return {}; }

        VkDescriptorPoolSize solveSizes[2] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 18},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6},
        };
        VkDescriptorPoolCreateInfo solveDp{};
        solveDp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        solveDp.maxSets = 6;
        solveDp.poolSizeCount = 2;
        solveDp.pPoolSizes = solveSizes;
        r = vkCreateDescriptorPool(d, &solveDp, nullptr, &n->solvePool);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        VkDescriptorPoolSize evalSizes[2] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * kMaxEvalStack},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxEvalStack},
        };
        VkDescriptorPoolCreateInfo evalDp{};
        evalDp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        evalDp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        evalDp.maxSets = kMaxEvalStack;
        evalDp.poolSizeCount = 2;
        evalDp.pPoolSizes = evalSizes;
        r = vkCreateDescriptorPool(d, &evalDp, nullptr, &n->evalPool);
        if (r != VK_SUCCESS) { finish(r); return {}; }

        auto allocSolve = [&](VkDescriptorSetLayout layout, VkDescriptorSet* set) {
            VkDescriptorSetAllocateInfo da{};
            da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            da.descriptorPool = n->solvePool;
            da.descriptorSetCount = 1;
            da.pSetLayouts = &layout;
            return vkAllocateDescriptorSets(d, &da, set);
        };
        r = allocSolve(n->extentLayout, &n->extentSet);
        if (r == VK_SUCCESS) r = allocSolve(n->gramLayout, &n->gramSet);
        if (r == VK_SUCCESS) r = allocSolve(n->buildMatrixLayout, &n->buildMatrixSet);
        if (r == VK_SUCCESS) r = allocSolve(n->luLayout, &n->luSet);
        if (r == VK_SUCCESS) r = allocSolve(n->rhsLayout, &n->rhsSet);
        if (r == VK_SUCCESS) r = allocSolve(n->triSolveLayout, &n->triSolveSet);
        if (r != VK_SUCCESS) { finish(r); return {}; }

        VkCommandPoolCreateInfo pc{};
        pc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pc.queueFamilyIndex = n->context->computeQueueFamily();
        r = vkCreateCommandPool(d, &pc, nullptr, &n->solveCommands);
        if (r == VK_SUCCESS) r = vkCreateCommandPool(d, &pc, nullptr, &n->evalCommands);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = vkCreateFence(d, &fi, nullptr, &n->solveFence);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        auto binding = std::shared_ptr<RbfVkBinding>(new RbfVkBinding(std::move(n)));
        finish(VK_SUCCESS);
        return binding;
    } catch (std::bad_alloc const&) { finish(VK_ERROR_OUT_OF_HOST_MEMORY); return {}; }
}

namespace {

// Submit without waiting. Pending owners remain retained until Poll proves
// this exact fence; rejected submits never authorize speculative teardown.
VkResult Submit(std::shared_ptr<DeviceContext> const& context,
                VkCommandPool pool, VkFence fence, bool& pending,
                std::function<VkResult(VkCommandBuffer)> const& record,
                std::function<bool()> const& beforeSubmit) {
    auto d = context->device();
    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult r = vkAllocateCommandBuffers(d, &ca, &cmd);
    if (r != VK_SUCCESS) return r;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    r = vkBeginCommandBuffer(cmd, &begin);
    if (r == VK_SUCCESS) r = record(cmd);
    if (r == VK_SUCCESS) r = vkEndCommandBuffer(cmd);
    if (r == VK_SUCCESS) r = vkResetFences(d, 1, &fence);
    if (r == VK_SUCCESS && beforeSubmit) {
        try { if (!beforeSubmit()) r = VK_ERROR_OUT_OF_DEVICE_MEMORY; }
        catch (...) { r = VK_ERROR_UNKNOWN; }
    }
    if (r == VK_SUCCESS) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        pending = true;
        r = vkQueueSubmit(context->computeQueue(), 1, &submit, fence);
    }
    if (!pending) (void)vkResetCommandPool(d, pool, 0);
    return r;
}

} // namespace

std::shared_ptr<RbfVkFactorCache::Entry const> RbfVkFactorCache::Lookup(
    DeviceContext* context, int n, double smoothing, float const* rest) {
    uint64_t smoothingBits = 0;
    static_assert(sizeof(smoothingBits) == sizeof(smoothing), "");
    std::memcpy(&smoothingBits, &smoothing, sizeof(smoothing));
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entry_ || entry_->context != context || entry_->n != n ||
        entry_->smoothingBits != smoothingBits ||
        entry_->rest.size() != size_t(n) * 3 ||
        std::memcmp(entry_->rest.data(), rest, size_t(n) * 3 * sizeof(float)) != 0)
        return nullptr;
    return entry_;
}

void RbfVkFactorCache::Store(DeviceContext* context, int n, double smoothing,
                             std::vector<float> const& rest,
                             std::vector<double> const& lu,
                             std::vector<int> const& perm, int m) {
    uint64_t smoothingBits = 0;
    std::memcpy(&smoothingBits, &smoothing, sizeof(smoothing));
    auto entry = std::make_shared<Entry>();
    entry->context = context;
    entry->n = n;
    entry->m = m;
    entry->smoothingBits = smoothingBits;
    entry->rest = rest;
    entry->lu = lu;
    entry->perm = perm;
    std::lock_guard<std::mutex> lock(mutex_);
    entry_ = std::move(entry);
}

void RbfVkBinding::SetFactorCache(std::shared_ptr<RbfVkFactorCache> cache) {
    factorCache_ = std::move(cache);
}

RbfVkStatus RbfVkBinding::BeginBind(float const* rest, int n, double smoothing,
                                     BeforeSubmit beforeSubmit) {
    if (native_->solvePending)
        return fail(RbfVkStatus::DeviceError, "RBF solve completion proof unavailable");
    if (evalPending_)
        return fail(RbfVkStatus::InvalidArgument,
                    "RBF Finish is required before rebinding a pending evaluation");
    sampleCount_ = order_ = 0;
    solved_ = false;
    diagnostic_.clear();
    native_->lastResult = VK_SUCCESS;
    native_->phase = Native::Phase::Idle;
    native_->cachedLuValid = false;
    native_->hostPosePending = false;
    native_->hostBindStaged = false;
    if (!rest || n < 4 || n > kRbfVkMaxSamples || !std::isfinite(smoothing) || smoothing < 0.0)
        return fail(RbfVkStatus::InvalidArgument,
                    "RBF requires 4+ samples and finite non-negative smoothing");
    int const m = n + 4;
    auto& native = *native_;
    auto d = native.context->device();
    VkPhysicalDeviceProperties physical{};
    vkGetPhysicalDeviceProperties(native.context->physicalDevice(), &physical);
    if (VkDeviceSize(m) * m * sizeof(double) > physical.limits.maxStorageBufferRange)
        return fail(RbfVkStatus::InvalidArgument,
                    "RBF matrix exceeds Vulkan maxStorageBufferRange");
    uint32_t matrixGroups = Groups(uint32_t(m) * uint32_t(m));
    uint32_t matrixX = std::min(matrixGroups, physical.limits.maxComputeWorkGroupCount[0]);
    uint32_t matrixY = (matrixGroups + matrixX - 1) / matrixX;
    if (matrixY > physical.limits.maxComputeWorkGroupCount[1])
        return fail(RbfVkStatus::InvalidArgument,
                    "RBF matrix exceeds Vulkan compute dispatch range");
    VkResult allocated = VK_SUCCESS;
    try { allocated = native.AllocateBuffers(n); }
    catch (std::bad_alloc const&) { native.ResetBuffers(); allocated = VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (allocated != VK_SUCCESS) {
        native.lastResult = allocated;
        return fail(RbfVkStatus::DeviceError,
                    allocated == VK_ERROR_OUT_OF_DEVICE_MEMORY
                        ? "RBF buffers exceed the Vulkan resource budget or device memory"
                        : "RBF per-binding buffer allocation failed");
    }
    if (WriteBytes(d, *native.restBuf, VkDeviceSize(n) * 12u, rest) != VK_SUCCESS)
        return fail(RbfVkStatus::DeviceError, "RBF rest copy failed");
    // Host copy for the host-side pose solve (after AllocateBuffers, which
    // resets the caches when the size changes).
    try {
        native.cachedRest.assign(rest, rest + size_t(n) * 3);
    } catch (std::bad_alloc const&) {
        return fail(RbfVkStatus::DeviceError, "RBF rest copy failed");
    }
    // Factor-cache hit: adopt the stored factors and skip the gram +
    // buildMatrix + LU submit in AdvanceBind. Only successful binds are
    // stored, and the key covers the device, count, smoothing bits, and
    // rest bytes, so the adopted factors are bitwise what a fresh bind
    // would read back. Buffer state, the admission hooks, and the
    // Begin/Poll protocol are unchanged.
    native.bindAdopted = false;
    if (factorCache_) {
        auto adopted = factorCache_->Lookup(native.context.get(), n, smoothing, rest);
        if (adopted) {
            try {
                native.cachedLu = adopted->lu;
                native.cachedPerm = adopted->perm;
            } catch (std::bad_alloc const&) {
                return fail(RbfVkStatus::DeviceError, "RBF rest copy failed");
            }
            native.cachedM = adopted->m;
            native.bindAdopted = true;
        }
    }

    // G1: extent.
    {
        SolveUbo ubo;
        ubo.n = n;
        ubo.m = m;
        ubo.lambda = smoothing;
        native.solveUbo = ubo;
        if (WriteBytes(d, *native.uboBuf, 64, &ubo) != VK_SUCCESS)
            return fail(RbfVkStatus::DeviceError, "RBF extent query failed");
    }
    // The extent runs on the host (exact min/max port, no submits): the
    // admission hook keeps the Submit mapping (false ->
    // OUT_OF_DEVICE_MEMORY, throw -> UNKNOWN), and the extent itself waits
    // for PollSolve, so the Begin/Poll pending protocol (and error timing)
    // is unchanged. The extent pipeline, buffers, and SPV API stay in
    // place for compatibility.
    if (beforeSubmit) {
        bool admitted = false;
        VkResult hr = VK_SUCCESS;
        try {
            admitted = beforeSubmit();
        } catch (...) {
            hr = VK_ERROR_UNKNOWN;
        }
        if (!admitted && hr == VK_SUCCESS) hr = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        native.lastResult = hr;
        if (hr != VK_SUCCESS)
            return fail(RbfVkStatus::DeviceError, "RBF extent query failed");
    }
    native.hostBindStaged = true;
    native.solvePending = true;
    native.lastResult = VK_SUCCESS;
    native.phase = Native::Phase::Extent;
    return RbfVkStatus::Ok;
}


RbfVkStatus RbfVkBinding::AdvanceBind(BeforeSubmit beforeSubmit) {
    auto& native = *native_;
    if (native.solvePending || native.phase != Native::Phase::FactorReady)
        return fail(RbfVkStatus::InvalidArgument, "RBF factorization requires extent proof");
    if (native.bindAdopted) {
        // Adopted factors: no factor submit. The admission hook keeps its
        // Submit mapping, then the bind stages for Factor consume with no
        // fence, like the host-staged extent.
        if (beforeSubmit) {
            VkResult hr = VK_SUCCESS;
            try {
                if (!beforeSubmit()) hr = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            } catch (...) {
                hr = VK_ERROR_UNKNOWN;
            }
            native.lastResult = hr;
            if (hr != VK_SUCCESS)
                return fail(RbfVkStatus::DeviceError, "RBF rank diagnostic query failed");
        }
        native.phase = Native::Phase::Factor;
        native.hostBindStaged = true;
        native.solvePending = true;
        return RbfVkStatus::Ok;
    }
    int const m = native.solveUbo.m;
    VkPhysicalDeviceProperties physical{};
    vkGetPhysicalDeviceProperties(native.context->physicalDevice(), &physical);
    uint32_t matrixGroups = Groups(uint32_t(m) * uint32_t(m));
    uint32_t matrixX = std::min(matrixGroups, physical.limits.maxComputeWorkGroupCount[0]);
    uint32_t matrixY = (matrixGroups + matrixX - 1) / matrixX;
    // G2: gram + matrix build + LU + identity-state zeroing.
    VkResult r = Submit(native.context, native.solveCommands, native.solveFence, native.solvePending,
                [&](VkCommandBuffer cmd) {
        BeforeBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, native.gramPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            native.gramPipelineLayout, 0, 1, &native.gramSet, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, native.buildMatrixPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            native.buildMatrixPipelineLayout, 0, 1, &native.buildMatrixSet, 0, nullptr);
        vkCmdDispatch(cmd, matrixX, matrixY, 1);
        BeforeBarrier(cmd); // buildMatrix writes -> LU reads
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, native.luPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            native.luPipelineLayout, 0, 1, &native.luSet, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        // Identity solved state (rbf.cu:472): a newly bound field is rest.
        vkCmdFillBuffer(cmd, native.coefBuf->buffer(), 0, VK_WHOLE_SIZE, 0u);
        AfterBarrier(cmd);
        return VK_SUCCESS;
    }, beforeSubmit);
    native.lastResult = r;
    if (r != VK_SUCCESS)
        return fail(RbfVkStatus::DeviceError, "RBF rank diagnostic query failed");
    native.phase = Native::Phase::Factor;
    return RbfVkStatus::Ok;
}


VkResult RbfVkBinding::PollSolve(RbfVkStatus* status) {
    if (status) *status = RbfVkStatus::Ok;
    auto& native = *native_;
    auto d = native.context->device();
    if (!native.solvePending && !native.hostPosePending) return VK_SUCCESS;
    if (native.hostPosePending) {
        // Host pose consume: no fence to prove. The RHS in exact
        // rbfVkRhs.comp order against the cached rest samples, then the
        // triangular solve in exact rbfVkTriSolve.comp order against the
        // cached factors; the staged coefficients are bitwise what the
        // retired submits wrote.
        int const hn = native.solveUbo.n, hm = native.solveUbo.m;
        auto hostConsume = [&]() -> RbfVkStatus {
            // Consumed once, like the fenced path (which clears
            // solvePending before consuming): a retry needs a new Begin.
            native.hostPosePending = false;
            double const invScale = native.solveUbo.invScale;
            float const* posed = native.hostPosed.data();
            std::vector<double> rhs(size_t(3) * size_t(hm), 0.0);
            for (int i = 0; i < hn; ++i) {
                float const bx = posed[size_t(3) * size_t(i)];
                float const by = posed[size_t(3) * size_t(i) + 1];
                float const bz = posed[size_t(3) * size_t(i) + 2];
                if (!std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bz))
                    return fail(RbfVkStatus::NonFiniteInput,
                                "RBF current samples contain non-finite values");
                rhs[size_t(i)] =
                    (double(bx) - double(native.cachedRest[size_t(3) * size_t(i)])) * invScale;
                rhs[size_t(hm) + size_t(i)] =
                    (double(by) - double(native.cachedRest[size_t(3) * size_t(i) + 1])) * invScale;
                rhs[size_t(2) * size_t(hm) + size_t(i)] =
                    (double(bz) - double(native.cachedRest[size_t(3) * size_t(i) + 2])) * invScale;
            }
            double const* luPtr = native.cachedLu.data();
            int const* permPtr = native.cachedPerm.data();
            std::vector<double> coef(size_t(3) * size_t(hm));
            for (int k = 0; k < 3; ++k) {
                size_t const base = size_t(k) * size_t(hm);
                for (int i = 0; i < hm; ++i) {
                    double s = rhs[base + size_t(permPtr[i])];
                    for (int j = 0; j < i; ++j) s -= luPtr[size_t(i) * size_t(hm) + size_t(j)] * coef[base + size_t(j)];
                    coef[base + size_t(i)] = s;
                }
                for (int i = hm - 1; i >= 0; --i) {
                    double s = coef[base + size_t(i)];
                    for (int j = i + 1; j < hm; ++j) s -= luPtr[size_t(i) * size_t(hm) + size_t(j)] * coef[base + size_t(j)];
                    coef[base + size_t(i)] = s / luPtr[size_t(i) * size_t(hm) + size_t(i)];
                }
            }
            if (WriteBytes(d, *native.coefStagingBuf, VkDeviceSize(hm) * 24u, coef.data()) != VK_SUCCESS)
                return fail(RbfVkStatus::DeviceError, "RBF solve status query failed");
            solved_ = true;
            native.phase = Native::Phase::Idle;
            return RbfVkStatus::Ok;
        };
        RbfVkStatus consumed = RbfVkStatus::Ok;
        try {
            consumed = hostConsume();
        } catch (std::bad_alloc const&) {
            consumed = fail(RbfVkStatus::DeviceError, "RBF solve status query failed");
        }
        if (status) *status = consumed;
        if (consumed == RbfVkStatus::DeviceError) return native.lastResult = VK_ERROR_MEMORY_MAP_FAILED;
        return VK_SUCCESS;
    }
    // A host-staged bind phase was never submitted, so there is no fence
    // to prove; consumed once, like the fenced path (which clears
    // solvePending before consuming): a retry needs a new Begin.
    bool const hostStaged = native.hostBindStaged;
    if (hostStaged) {
        native.solvePending = false;
        native.hostBindStaged = false;
    } else {
        VkResult r = vkGetFenceStatus(d, native.solveFence);
        if (r != VK_SUCCESS) { native.lastResult = r; return r; }
        native.solvePending = false;
        r = vkResetCommandPool(d, native.solveCommands, 0);
        if (r != VK_SUCCESS) { native.lastResult = r; return r; }
    }
    int const n = native.solveUbo.n, m = native.solveUbo.m;
    auto consume = [&]() -> RbfVkStatus {
        if (native.phase == Native::Phase::Extent) {
            float e[6] = {};
            int flag = 0;
            HostExtent(native.cachedRest.data(), n, e, &flag);
            if (flag)
                return fail(RbfVkStatus::NonFiniteInput, "RBF rest samples contain non-finite values");
            center_[0] = double(e[0]) + (double(e[3]) - double(e[0])) * .5;
            center_[1] = double(e[1]) + (double(e[4]) - double(e[1])) * .5;
            center_[2] = double(e[2]) + (double(e[5]) - double(e[2])) * .5;
            scale_ = std::max(double(e[3]) - e[0], std::max(double(e[4]) - e[1], double(e[5]) - e[2]));
            if (!std::isfinite(scale_) || scale_ <= 0.0)
                return fail(RbfVkStatus::RankDeficient, "RBF rest samples have zero extent");

            SolveUbo ubo = native.solveUbo;
            ubo.n = n;
            ubo.m = m;
            ubo.cx = center_[0];
            ubo.cy = center_[1];
            ubo.cz = center_[2];
            ubo.invScale = 1.0 / scale_;
            ubo.scale = scale_;
            native.solveUbo = ubo;
            if (WriteBytes(d, *native.uboBuf, 64, &ubo) != VK_SUCCESS)
                return fail(RbfVkStatus::DeviceError, "RBF rank diagnostic reset failed");

            // Center the rest samples for the evaluate shader once per bind:
            // the shader used to reload every float sample and subtract the
            // center for every CV. Bitwise the subexpression it computed
            // (double(float) - center); the * invScale stays in-shader so
            // the driver's FMA contraction keeps its shape and bits.
            try {
                std::vector<float> samples(size_t(3) * size_t(n));
                std::vector<double> centered(size_t(3) * size_t(n));
                // cachedRest holds the same bytes restBuf does (no writer
                // touches restBuf after BeginBind), so the host extent
                // skips the readback; a failed copy reports the same
                // status.
                samples = native.cachedRest;
                for (int j = 0; j < n; ++j) {
                    centered[size_t(3 * j)] = double(samples[size_t(3 * j)]) - center_[0];
                    centered[size_t(3 * j + 1)] = double(samples[size_t(3 * j + 1)]) - center_[1];
                    centered[size_t(3 * j + 2)] = double(samples[size_t(3 * j + 2)]) - center_[2];
                }
                if (WriteBytes(d, *native.normBuf, VkDeviceSize(n) * 24u, centered.data()) != VK_SUCCESS)
                    return fail(RbfVkStatus::DeviceError, "RBF rank diagnostic reset failed");
            } catch (std::bad_alloc const&) {
                return fail(RbfVkStatus::DeviceError, "RBF rank diagnostic reset failed");
            }

            native.phase = Native::Phase::FactorReady;
            return RbfVkStatus::Ok;
        }
        if (native.phase == Native::Phase::Factor) {
            if (!native.bindAdopted) {
                double gram[16] = {};
                int info = 0;
                if (ReadBytes(d, *native.gramBuf, 128, gram) != VK_SUCCESS ||
                    ReadBytes(d, *native.infoBuf, 4, &info) != VK_SUCCESS)
                    return fail(RbfVkStatus::DeviceError, "RBF LU status query failed");
                if (!fullAffineRank(gram))
                    return fail(RbfVkStatus::RankDeficient, "RBF samples lack numerically full affine 3D support");
                if (info > 0)
                    return fail(RbfVkStatus::RankDeficient, "RBF augmented LU is singular (including coplanar affine support)");
                if (info < 0)
                    return fail(RbfVkStatus::SolverError, "RBF LU invalid argument");
            }
            // Identity solved state, host side: Evaluate copies the staging
            // buffer over coefBuf on every dispatch, so a bind without a
            // solve must stage zeros just as the device zeroes coefBuf.
            try {
                std::vector<double> zeros(size_t(3) * size_t(m), 0.0);
                if (WriteBytes(d, *native.coefStagingBuf, VkDeviceSize(m) * 24u, zeros.data()) != VK_SUCCESS)
                    return fail(RbfVkStatus::DeviceError, "RBF LU status query failed");
            } catch (std::bad_alloc const&) {
                return fail(RbfVkStatus::DeviceError, "RBF LU status query failed");
            }
            // Cache the factors for the host-side pose solves.
            if (native.bindAdopted) {
                // Adopted in BeginBind; the stored bind already proved the
                // rank, so no gram/info readback runs.
                native.cachedM = m;
                native.cachedLuValid = true;
            } else {
                try {
                    native.cachedLu.resize(size_t(m) * size_t(m));
                    native.cachedPerm.resize(size_t(m));
                    if (ReadBytes(d, *native.matrixBuf, VkDeviceSize(m) * size_t(m) * 8u,
                                  native.cachedLu.data()) != VK_SUCCESS ||
                        ReadBytes(d, *native.permBuf, VkDeviceSize(m) * 4u,
                                  native.cachedPerm.data()) != VK_SUCCESS)
                        return fail(RbfVkStatus::DeviceError, "RBF LU status query failed");
                    native.cachedM = m;
                    native.cachedLuValid = true;
                } catch (std::bad_alloc const&) {
                    return fail(RbfVkStatus::DeviceError, "RBF LU status query failed");
                }
                // Publish successful binds to the shared factor cache (a
                // store failure degrades to the next miss, never an error).
                if (factorCache_) {
                    try {
                        factorCache_->Store(native.context.get(), n,
                                            native.solveUbo.lambda, native.cachedRest,
                                            native.cachedLu, native.cachedPerm, m);
                    } catch (...) {
                    }
                }
            }
            sampleCount_ = n;
            order_ = m;
            solved_ = true;
            native.phase = Native::Phase::Idle;
            return RbfVkStatus::Ok;
        }
        // The pose solve runs on the host inside BeginSolve, so no fenced
        // Pose submit exists anymore; reaching consume outside the Extent /
        // Factor phases means the phase machine itself diverged.
        return fail(RbfVkStatus::DeviceError, "RBF solve completion proof unavailable");
    };
    RbfVkStatus consumed = consume();
    if (status) *status = consumed;
    if (consumed == RbfVkStatus::DeviceError) return native.lastResult = VK_ERROR_MEMORY_MAP_FAILED;
    return VK_SUCCESS;
}

RbfVkStatus RbfVkBinding::Bind(float const* rest, int n, double smoothing) {
    auto status = BeginBind(rest, n, smoothing);
    if (status != RbfVkStatus::Ok) return status;
    for (int phase = 0; phase < 2; ++phase) {
        // A host-staged bind phase was never submitted, so there is no
        // fence to wait on (like the host pose in Solve).
        if (!native_->hostBindStaged) {
            auto w = vkWaitForFences(native_->context->device(), 1, &native_->solveFence,
                                     VK_TRUE, kFenceTimeoutNs);
            if (w != VK_SUCCESS)
                return fail(RbfVkStatus::DeviceError, "RBF bind completion query failed");
        }
        auto r = PollSolve(&status);
        if (r != VK_SUCCESS || status != RbfVkStatus::Ok) return status == RbfVkStatus::Ok ? RbfVkStatus::DeviceError : status;
        if (phase == 0) { status = AdvanceBind(); if (status != RbfVkStatus::Ok) return status; }
    }
    return RbfVkStatus::Ok;
}

RbfVkStatus RbfVkBinding::BeginSolve(float const* posed, int posedCount,
                                      BeforeSubmit beforeSubmit) {
    if (native_->solvePending || native_->hostPosePending)
        return fail(RbfVkStatus::DeviceError, "RBF solve completion proof unavailable");
    if (evalPending_)
        return fail(RbfVkStatus::InvalidArgument,
                    "RBF Finish is required before changing a pending evaluation state");
    solved_ = false;
    if (!sampleCount_ || !posed || posedCount != sampleCount_)
        return fail(RbfVkStatus::InvalidArgument, "RBF Solve samples do not match binding");
    int const n = sampleCount_;
    int const m = native_->solveUbo.m;
    auto& native = *native_;
    if (!native.cachedLuValid || native.cachedM != m ||
        native.cachedRest.size() != size_t(n) * 3)
        return fail(RbfVkStatus::DeviceError, "RBF solve completion proof unavailable");
    // Admission hook first, as before the retired submit.
    if (beforeSubmit) {
        bool admitted = false;
        VkResult r = VK_SUCCESS;
        try {
            admitted = beforeSubmit();
        } catch (...) {
            r = VK_ERROR_UNKNOWN;
        }
        if (!admitted && r == VK_SUCCESS) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        native.lastResult = r;
        if (r != VK_SUCCESS)
            return fail(RbfVkStatus::DeviceError, "RBF input validation failed");
    }
    // Stage only: the RHS + triangular solve wait for PollSolve, so the
    // Begin/Poll pending protocol (and error timing) is unchanged.
    try {
        native.hostPosed.assign(posed, posed + size_t(n) * 3);
    } catch (std::bad_alloc const&) {
        return fail(RbfVkStatus::DeviceError, "RBF current sample copy failed");
    }
    native.hostPosePending = true;
    native.phase = Native::Phase::Pose;
    native.lastResult = VK_SUCCESS;
    return RbfVkStatus::Ok;
}

RbfVkStatus RbfVkBinding::Solve(float const* posed, int posedCount) {
    auto status = BeginSolve(posed, posedCount);
    if (status != RbfVkStatus::Ok) return status;
    // The pose runs on the host in PollSolve: nothing was submitted, so
    // there is no fence to wait on.
    auto r = PollSolve(&status);
    return r == VK_SUCCESS ? status : RbfVkStatus::DeviceError;
}

RbfVkStatus RbfVkBinding::Evaluate(std::shared_ptr<const ChargedBuffer> cvs,
                                   std::shared_ptr<ChargedBuffer> out,
                                   uint32_t count, BeforeSubmit beforeSubmit) {
    if (native_->solvePending)
        return fail(RbfVkStatus::DeviceError, "RBF solve completion proof unavailable");
    if (!sampleCount_ || !solved_ || !cvs || !out || !count || count > uint32_t(INT32_MAX) ||
        cvs->context() != native_->context || out->context() != native_->context ||
        !(cvs->usage() & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !(out->usage() & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        cvs->sizeBytes() < VkDeviceSize(count) * 12u ||
        out->sizeBytes() < VkDeviceSize(count) * 12u)
        return fail(RbfVkStatus::InvalidArgument,
                    "RBF Evaluate requires a solved binding and equal non-empty bounded device views");
    auto& native = *native_;
    auto d = native.context->device();
    VkPhysicalDeviceProperties physical{};
    vkGetPhysicalDeviceProperties(native.context->physicalDevice(), &physical);
    if (VkDeviceSize(count) * 12u > physical.limits.maxStorageBufferRange)
        return fail(RbfVkStatus::InvalidArgument, "RBF evaluation exceeds Vulkan maxStorageBufferRange");
    uint32_t groups = Groups(count);
    uint32_t groupsX = std::min(groups, physical.limits.maxComputeWorkGroupCount[0]);
    uint32_t groupsY = (groups + groupsX - 1) / groupsX;
    if (groupsY > physical.limits.maxComputeWorkGroupCount[1])
        return fail(RbfVkStatus::InvalidArgument, "RBF evaluation exceeds Vulkan compute dispatch range");
    // Stacked evaluations serialize internally: the solve UBO is shared,
    // so a pending evaluation must complete before its count is
    // overwritten. Observable semantics match CUDA (one shared completion
    // diagnostic consumed by a single Finish).
    if (evalPending_ && !native.evalFences.empty()) {
        VkResult r = vkWaitForFences(d, uint32_t(native.evalFences.size()),
            native.evalFences.data(), VK_TRUE, kFenceTimeoutNs);
        if (r != VK_SUCCESS)
            return fail(RbfVkStatus::DeviceError, "RBF evaluation state setup failed");
    }
    SolveUbo ubo;
    ubo.n = sampleCount_;
    ubo.m = order_;
    ubo.count = int32_t(count);
    ubo.cx = center_[0];
    ubo.cy = center_[1];
    ubo.cz = center_[2];
    ubo.invScale = 1.0 / scale_;
    ubo.scale = scale_;
    ubo.lambda = 0.0;
    if (WriteBytes(d, *native.uboBuf, 64, &ubo) != VK_SUCCESS)
        return fail(RbfVkStatus::DeviceError, "RBF evaluation state setup failed");

    VkDescriptorSet set = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo da{};
        da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        da.descriptorPool = native.evalPool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &native.evaluateLayout;
        VkResult r = vkAllocateDescriptorSets(d, &da, &set);
        if (r != VK_SUCCESS)
            return fail(RbfVkStatus::DeviceError, "RBF evaluation state setup failed");
    }
    auto releaseSet = [&]() { vkFreeDescriptorSets(d, native.evalPool, 1, &set); };
    VkDescriptorBufferInfo infos[6] = {
        {cvs->buffer(), 0, VkDeviceSize(count) * 12u},
        {out->buffer(), 0, VkDeviceSize(count) * 12u},
        {native.normBuf->buffer(), 0, VkDeviceSize(sampleCount_) * 24u},
        {native.coefBuf->buffer(), 0, VkDeviceSize(order_) * 24u},
        {native.uboBuf->buffer(), 0, 64},
        {native.flagBuf->buffer(), 0, 4},
    };
    uint32_t const bindings[6] = {0, 1, 2, 3, 4, 5};
    VkWriteDescriptorSet writes[6]{};
    for (uint32_t i = 0; i < 6; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = bindings[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            (i == 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(d, 6, writes, 0, nullptr);

    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = native.evalCommands;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d, &ca, &cmd) != VK_SUCCESS) {
        releaseSet();
        return fail(RbfVkStatus::DeviceError, "RBF evaluation state setup failed");
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VkResult r = vkBeginCommandBuffer(cmd, &begin);
    if (r == VK_SUCCESS) {
        // Like CUDA (rbf.cu:491), the shared flag resets only when no
        // evaluation is pending; stacked submissions accumulate into it.
        if (!evalPending_)
            vkCmdFillBuffer(cmd, native.flagBuf->buffer(), 0, 4, 0u);
        // Host-solved coefficients into device-local coef (replaces the
        // retired device triSolve write).
        VkMemoryBarrier stageVisible{};
        stageVisible.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        stageVisible.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        stageVisible.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &stageVisible, 0, nullptr, 0, nullptr);
        VkBufferCopy coefCopy{0, 0, VkDeviceSize(order_) * 24u};
        vkCmdCopyBuffer(cmd, native.coefStagingBuf->buffer(), native.coefBuf->buffer(), 1, &coefCopy);
        VkMemoryBarrier coefReady{};
        coefReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        coefReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        coefReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &coefReady, 0, nullptr, 0, nullptr);
        BeforeBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, native.evaluatePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            native.evaluatePipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);
        AfterBarrier(cmd);
        r = vkEndCommandBuffer(cmd);
    }
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(d, native.evalCommands, 1, &cmd);
        releaseSet();
        return fail(RbfVkStatus::DeviceError, "RBF evaluation launch failed");
    }
    VkFence fence = native.fencePool.Acquire(d, &r);
    if (!fence) {
        vkFreeCommandBuffers(d, native.evalCommands, 1, &cmd);
        releaseSet();
        return fail(RbfVkStatus::DeviceError, "RBF evaluation launch failed");
    }
    // Allocate retention records before submission; no host allocation can
    // fail after work becomes pending and leave its owners untracked.
    // The fence was never submitted on these paths, so it is idle and
    // rejoins the pool.
    try {
        native.evalFences.reserve(native.evalFences.size() + 1);
        native.evalOwners.reserve(native.evalOwners.size() + 2);
    } catch (std::bad_alloc const&) {
        native.fencePool.Release(d, fence);
        vkFreeCommandBuffers(d, native.evalCommands, 1, &cmd);
        releaseSet();
        return fail(RbfVkStatus::DeviceError, "RBF evaluation launch failed");
    }
    if (beforeSubmit) {
        try { if (!beforeSubmit()) r = VK_ERROR_OUT_OF_DEVICE_MEMORY; }
        catch (...) { r = VK_ERROR_UNKNOWN; }
        if (r != VK_SUCCESS) {
            native.lastResult = r;
            native.fencePool.Release(d, fence);
            vkFreeCommandBuffers(d, native.evalCommands, 1, &cmd);
            releaseSet();
            return fail(RbfVkStatus::DeviceError, "RBF evaluation submission rejected");
        }
    }
    native.evalFences.push_back(fence);
    native.evalOwners.push_back(std::move(cvs));
    native.evalOwners.push_back(std::move(out));
    evalPending_ = true;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    r = vkQueueSubmit(native.context->computeQueue(), 1, &submit, fence);
    native.lastResult = r;
    if (r != VK_SUCCESS)
        return fail(RbfVkStatus::DeviceError, "RBF evaluation launch failed");
    return RbfVkStatus::Ok;
}

VkResult RbfVkBinding::PollEvaluate(RbfVkStatus* status) {
    if (status) *status = RbfVkStatus::Ok;
    if (!evalPending_) return VK_SUCCESS;
    auto& native = *native_;
    auto d = native.context->device();
    VkResult r = VK_SUCCESS;
    for (auto fence : native.evalFences) {
        r = vkGetFenceStatus(d, fence);
        if (r != VK_SUCCESS) return r;
    }
    int flag = 0;
    if (r == VK_SUCCESS) r = ReadBytes(d, *native.flagBuf, 4, &flag) == VK_SUCCESS
        ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
    // Every fence above polled VK_SUCCESS, so each is idle and rejoins the
    // pool even when the flag readback fails.
    for (VkFence f : native.evalFences) if (f) native.fencePool.Release(d, f);
    native.evalFences.clear();
    native.evalOwners.clear();
    (void)vkResetCommandPool(d, native.evalCommands, 0);
    (void)vkResetDescriptorPool(d, native.evalPool, 0);
    evalPending_ = false;
    if (r != VK_SUCCESS)
        return r;
    if (flag) {
        solved_ = false;
        auto code = fail(RbfVkStatus::NonFiniteInput,
                    "RBF evaluation CVs contain non-finite values; generation rejected");
        if (status) *status = code;
    }
    return VK_SUCCESS;
}

RbfVkStatus RbfVkBinding::Finish() {
    if (!evalPending_) return RbfVkStatus::Ok;
    auto r = vkWaitForFences(native_->context->device(), uint32_t(native_->evalFences.size()),
                            native_->evalFences.data(), VK_TRUE, kFenceTimeoutNs);
    if (r != VK_SUCCESS) return fail(RbfVkStatus::DeviceError, "RBF evaluation completion query failed");
    RbfVkStatus status = RbfVkStatus::Ok;
    r = PollEvaluate(&status);
    return r == VK_SUCCESS ? status : RbfVkStatus::DeviceError;
}


RbfVkStatus RbfVkBinding::EvaluateHost(float const* cvs, float* out, uint32_t count) {
    if (!cvs || !out || !count || count > uint32_t(INT32_MAX))
        return fail(RbfVkStatus::InvalidArgument,
                    "RBF Evaluate requires a solved binding and equal non-empty bounded device views");
    auto& native = *native_;
    auto d = native.context->device();
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = VkDeviceSize(count) * 12u;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = VK_SUCCESS;
    VkMemoryPropertyFlags const host =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    auto cvsBuf = ChargedBuffer::Create(native.context, bi, host,
        UsdGenExecutionResourceKind::Scratch, &r);
    if (!cvsBuf) return fail(RbfVkStatus::DeviceError, "RBF evaluation state setup failed");
    auto outBuf = ChargedBuffer::Create(native.context, bi, host,
        UsdGenExecutionResourceKind::Scratch, &r);
    if (!outBuf) return fail(RbfVkStatus::DeviceError, "RBF evaluation state setup failed");
    if (WriteBytes(d, *cvsBuf, bi.size, cvs) != VK_SUCCESS)
        return fail(RbfVkStatus::DeviceError, "RBF evaluation state setup failed");
    RbfVkStatus status = Evaluate(cvsBuf, outBuf, count);
    if (status != RbfVkStatus::Ok) return status;
    status = Finish();
    if (status != RbfVkStatus::Ok) return status;
    if (ReadBytes(d, *outBuf, bi.size, out) != VK_SUCCESS)
        return fail(RbfVkStatus::DeviceError, "RBF evaluation completion query failed");
    return RbfVkStatus::Ok;
}

} // namespace usdGen::vulkan
