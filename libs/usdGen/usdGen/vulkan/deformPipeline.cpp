#include "deformPipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <new>

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

constexpr uint32_t kLocalSize = 256;
constexpr uint32_t kEvalUboBytes = 64;
constexpr uint32_t kApplyUboBytes = 80;
// Eval set: bindings 0,1,2,3,5 = 5 storage + binding 4 = 1 uniform.
constexpr uint32_t kEvalStorage = 5;
// Apply set: bindings 0..8 = 9 storage + binding 9 = 1 uniform.
constexpr uint32_t kApplyStorage = 9;

uint32_t Groups(uint32_t n) { return (n + kLocalSize - 1) / kLocalSize; }

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

struct ApplyUbo {
    uint32_t counts[3];       // uvec3: 12B in std140, offset 0
    float groomEnvelope;      // offset 12
    float maskF[4];          // vec4: 16B in std140, offset 16
    float enabledF[4];       // vec4: offset 32
    float lockF[4];          // vec4: offset 48
    uint32_t fieldCounts[3]; // uvec3: 12B in std140, offset 64
    uint32_t _pad;           // offset 76; total 80, 16B-aligned block
};
static_assert(sizeof(ApplyUbo) == 80);
static_assert(offsetof(ApplyUbo, groomEnvelope) == 12);
static_assert(offsetof(ApplyUbo, maskF) == 16);
static_assert(offsetof(ApplyUbo, enabledF) == 32);
static_assert(offsetof(ApplyUbo, lockF) == 48);
static_assert(offsetof(ApplyUbo, fieldCounts) == 64);

} // namespace

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
    ~Native() {
        if (!context) return;
        auto d = context->device();
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
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false, lost = false, proved = false;
    uint32_t semantic = UINT32_MAX;
    uint32_t pointCount = 0;
    std::unique_ptr<std::shared_ptr<State>> quarantine;
    ~State() {
        auto d = native->context->device();
        if (fence) vkDestroyFence(d, fence, nullptr);
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
    VkResult* result) {
    auto finish = [&](VkResult r) { if (result) *result = r; };
    finish(VK_ERROR_INITIALIZATION_FAILED);
    auto valid = [](std::vector<uint32_t> const& c) {
        return c.size() >= 5 && c.front() == 0x07230203u;
    };
    if (!context || !valid(evaluateSpirv) || !valid(applySpirv)) return {};
    if (!context->shaderFloat64Enabled()) return {};
    VkPhysicalDeviceProperties physical{};
    vkGetPhysicalDeviceProperties(context->physicalDevice(), &physical);
    if (physical.limits.maxComputeWorkGroupInvocations < kLocalSize ||
        physical.limits.maxComputeWorkGroupSize[0] < kLocalSize ||
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
        {
            VkPipelineLayoutCreateInfo pl{};
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl.setLayoutCount = 1; pl.pSetLayouts = &n->evalLayout;
            r = vkCreatePipelineLayout(d, &pl, nullptr, &n->evalPipelineLayout);
            if (r == VK_SUCCESS) {
                pl.pSetLayouts = &n->applyLayout;
                r = vkCreatePipelineLayout(d, &pl, nullptr, &n->applyPipelineLayout);
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
    return Output{state_->outPoints};
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
            case kDomainGroom:       return curves;
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

    // Offset monotonicity.
    if (curves > 0) {
        void* od = nullptr;
        VkResult mr = vkMapMemory(context->device(), info.curveOffsets->memory(),
                                   0, VkDeviceSize(curves + 1u) * 4u, 0, &od);
        if (mr != VK_SUCCESS) return reject(DeformSemantic::BadValue);
        auto* offs = static_cast<uint32_t const*>(od);
        bool bad = (offs[0] != 0) || (offs[curves] != points);
        if (!bad)
            for (uint32_t c = 0; c + 1 <= curves; ++c)
                if (offs[c] >= offs[c + 1]) { bad = true; break; }
        vkUnmapMemory(context->device(), info.curveOffsets->memory());
        if (bad) return reject(DeformSemantic::BadOffsets);
    }

    // CPU solve.
    RbfState rstate;
    SolveRbf(reinterpret_cast<float3 const*>(info.restSamples.data()),
             reinterpret_cast<float3 const*>(info.posedSamples.data()),
             n, info.smoothing, rstate);
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

        // outPoints: DEVICE_LOCAL.
        VkDeviceSize const outBytes = points ? VkDeviceSize(points) * 12u : 4u;
        {
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
        // status: 4B host-visible.
        {
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
            void* data = nullptr;
            r = vkMapMemory(d, s->status->memory(), 0, 4, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memset(data, 0, 4);
            vkUnmapMemory(d, s->status->memory());
        }
        // evalStatus: 4B host-visible.
        {
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

        // warped: DEVICE_LOCAL.
        {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = outBytes;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            s->warped = ChargedBuffer::Create(context, bi,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, UsdGenExecutionResourceKind::Active, &r);
            if (!s->warped) { finish(r); return {}; }
        }
        // coefBuf: HOST_VISIBLE, 3*m doubles.
        VkDeviceSize const coefBytes = VkDeviceSize(m) * 24u;
        {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = coefBytes;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            s->coefBuf = ChargedBuffer::Create(context, bi,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                UsdGenExecutionResourceKind::Scratch, &r);
            if (!s->coefBuf) { finish(r); return {}; }
            void* data = nullptr;
            r = vkMapMemory(d, s->coefBuf->memory(), 0, coefBytes, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memcpy(data, rstate.coef, coefBytes);
            vkUnmapMemory(d, s->coefBuf->memory());
        }
        // samplesBuf: HOST_VISIBLE, 3*n floats.
        VkDeviceSize const samplesBytes = VkDeviceSize(n) * 12u;
        {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = samplesBytes;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            s->samplesBuf = ChargedBuffer::Create(context, bi,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                UsdGenExecutionResourceKind::Scratch, &r);
            if (!s->samplesBuf) { finish(r); return {}; }
            void* data = nullptr;
            r = vkMapMemory(d, s->samplesBuf->memory(), 0, samplesBytes, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memcpy(data, info.restSamples.data(), samplesBytes);
            vkUnmapMemory(d, s->samplesBuf->memory());
        }
        // evalUbo: HOST_VISIBLE, 64B.
        {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = kEvalUboBytes;
            bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            s->evalUbo = ChargedBuffer::Create(context, bi,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                UsdGenExecutionResourceKind::Scratch, &r);
            if (!s->evalUbo) { finish(r); return {}; }
            EvalUbo e;
            e.n_m[0] = n; e.n_m[1] = m;
            e.counts[0] = points; e.counts[1] = 0;
            e.cx = rstate.center[0]; e.cy = rstate.center[1]; e.cz = rstate.center[2];
            e.invScale = 1.0 / rstate.scale;
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
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = kApplyUboBytes;
            bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            s->applyUbo = ChargedBuffer::Create(context, bi,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                UsdGenExecutionResourceKind::Scratch, &r);
            if (!s->applyUbo) { finish(r); return {}; }
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
            void* data = nullptr;
            r = vkMapMemory(d, s->applyUbo->memory(), 0, kApplyUboBytes, 0, &data);
            if (r != VK_SUCCESS) { finish(r); return {}; }
            std::memcpy(data, &a, sizeof(a));
            vkUnmapMemory(d, s->applyUbo->memory());
        }

        // Descriptor pool + sets.
        VkDescriptorPoolSize sizes[2] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kEvalStorage + kApplyStorage},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        };
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = 2; dp.poolSizeCount = 2; dp.pPoolSizes = sizes;
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
        r = allocSet(native_->evalLayout, &evalSet);
        if (r == VK_SUCCESS) r = allocSet(native_->applyLayout, &applySet);
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
        VkDescriptorBufferInfo eInfos[kEvalStorage + 1] = {
            applyPlane(info.points, pointsBytes),
            {s->warped->buffer(), 0, outBytes},
            {s->samplesBuf->buffer(), 0, samplesBytes},
            {s->coefBuf->buffer(), 0, coefBytes},
            {s->evalUbo->buffer(), 0, kEvalUboBytes},
            {s->evalStatus->buffer(), 0, 4u},
        };
        VkWriteDescriptorSet eWrites[kEvalStorage + 1]{};
        uint32_t const eBindings[kEvalStorage + 1] = {0, 1, 2, 3, 4, 5};
        for (uint32_t i = 0; i < kEvalStorage + 1; ++i) {
            eWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            eWrites[i].dstSet = evalSet; eWrites[i].dstBinding = eBindings[i];
            eWrites[i].descriptorCount = 1;
            eWrites[i].descriptorType = (i == 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                 : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            eWrites[i].pBufferInfo = &eInfos[i];
        }
        vkUpdateDescriptorSets(d, kEvalStorage + 1, eWrites, 0, nullptr);

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
            vkCmdBindPipeline(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                native_->evalPipeline);
            vkCmdBindDescriptorSets(cmds[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                native_->evalPipelineLayout, 0, 1, &evalSet, 0, nullptr);
            vkCmdDispatch(cmds[0], Groups(points), 1, 1);
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
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence proofFence = VK_NULL_HANDLE;
        r = vkCreateFence(d, &fi, nullptr, &proofFence);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &cmds[0];
        r = vkQueueSubmit(context->computeQueue(), 1, &submit, proofFence);
        if (r == VK_SUCCESS)
            r = vkWaitForFences(d, 1, &proofFence, VK_TRUE, 10000000000ull);
        uint32_t st = 0;
        if (r == VK_SUCCESS) {
            void* data = nullptr;
            r = vkMapMemory(d, s->evalStatus->memory(), 0, 4, 0, &data);
            if (r == VK_SUCCESS) {
                std::memcpy(&st, data, 4);
                vkUnmapMemory(d, s->evalStatus->memory());
            }
        }
        vkDestroyFence(d, proofFence, nullptr);
        if (r != VK_SUCCESS) { finish(r); return {}; }
        if (st != 0) return reject(DeformSemantic::NonFinite);

        // cmd1: fill output + apply (async on candidate fence).
        r = vkBeginCommandBuffer(cmds[1], &begin);
        if (r == VK_SUCCESS) {
            vkCmdFillBuffer(cmds[1], s->outPoints->buffer(), 0, VK_WHOLE_SIZE, 0u);
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
            vkCmdDispatch(cmds[1], Groups(curves), 1, 1);
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
        r = vkCreateFence(d, &fi, nullptr, &s->fence);
        if (r != VK_SUCCESS) { finish(r); return {}; }
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
