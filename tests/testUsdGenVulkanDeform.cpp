// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

#include "usdGen/vulkan/deviceContext.h"
#include "usdGen/vulkan/chargedBuffer.h"
#include "usdGen/vulkan/deformPipeline.h"
#include "usdGen/vulkan/deformRbfHost.h"

#include "vulkanNativeFixture.h"
#include "vulkanReadbackFixture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

using namespace usdGen;
using namespace usdGen::vulkan;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (false)

namespace {

inline float3 V(float x, float y, float z) { return {x, y, z}; }
inline bool Near(float3 a, float3 b, float tol) {
    return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol &&
           std::fabs(a.z - b.z) <= tol;
}

inline std::vector<uint32_t> Code(char const* p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.empty() || b.size() % 4) return {};
    std::vector<uint32_t> r(b.size() / 4);
    std::memcpy(r.data(), b.data(), b.size());
    return r;
}

inline bool Prove(std::shared_ptr<NativeOwner> const& native) {
    if (vkResetFences(native->device, 1, &native->fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    return vkQueueSubmit(native->queue, 1, &submit, native->fence) == VK_SUCCESS &&
        vkWaitForFences(native->device, 1, &native->fence, VK_TRUE, 10000000000ull) == VK_SUCCESS;
}

inline std::shared_ptr<const ChargedBuffer> Upload(
    std::shared_ptr<DeviceContext> const& c, void const* data, VkDeviceSize bytes) {
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes ? bytes : 4;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto b = ChargedBuffer::Create(c, bi,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        UsdGenExecutionResourceKind::Scratch);
    if (!b) return {};
    if (bytes) {
        void* p = nullptr;
        if (vkMapMemory(c->device(), b->memory(), 0, bytes, 0, &p) != VK_SUCCESS) return {};
        std::memcpy(p, data, bytes);
        vkUnmapMemory(c->device(), b->memory());
    }
    return b;
}

// Read back the candidate's output points.
inline bool ReadOutput(std::shared_ptr<NativeOwner> const& native,
                       std::shared_ptr<DeviceContext> const& context,
                       std::unique_ptr<DeformPipeline::Candidate> const& c,
                       std::vector<float>* out) {
    auto o = c->output();
    std::vector<uint8_t> bytes;
    if (!ReadVulkanBytes(native, context, o.points->buffer(),
                         o.points->sizeBytes(), o.points, &bytes)) return false;
    out->resize(bytes.size() / sizeof(float));
    std::memcpy(out->data(), bytes.data(), bytes.size());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s deformEvaluate.spv deformApply.spv\n", argv[0]);
        return 1;
    }
    auto evalSpv = Code(argv[1]);
    auto applySpv = Code(argv[2]);
    CHECK(!evalSpv.empty() && !applySpv.empty());

    bool unavailable = false;
    VkPhysicalDeviceFeatures fp64 = {};
    fp64.shaderFloat64 = VK_TRUE;
    auto native = CreateNative(&unavailable, {}, nullptr, &fp64);
    if (unavailable) return 77;
    CHECK(native);

    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(native->physical, &feats);
    if (!feats.shaderFloat64) return 77;

    DeviceContext::CreateInfo ci;
    ci.instance = native->instance;
    ci.physicalDevice = native->physical;
    ci.device = native->device;
    ci.computeQueue = native->queue;
    ci.computeQueueFamily = native->family;
    ci.physicalIndex = native->physicalIndex;
    ci.resourceDeviceId = 8020;
    ci.nativeLifetime = native;
    ci.resources = {size_t{32} << 20, 0};
    ci.shaderFloat64Enabled = true;
    auto context = DeviceContext::Create(ci);
    CHECK(context);

    VkResult status = VK_SUCCESS;
    auto pipe = DeformPipeline::Create(context, evalSpv, applySpv, &status);
    CHECK(pipe && status == VK_SUCCESS);

    // ---- Base geometry: 2 curves, 6 CVs ----
    // Curve 0: 3 points along +x near origin
    // Curve 1: 3 points along +x at x=1
    const std::vector<float> points{
        0.0f, 0.0f, 0.0f,   // curve 0, point 0
        0.1f, 0.0f, 0.1f,   // curve 0, point 1
        0.2f, 0.0f, 0.2f,   // curve 0, point 2
        1.0f, 0.0f, 0.0f,   // curve 1, point 0
        1.1f, 0.0f, 0.1f,   // curve 1, point 1
        1.2f, 0.0f, 0.2f,   // curve 1, point 2
    };
    const std::vector<uint32_t> offsets{0, 3, 6};
    const std::vector<float> rootTargets{
        0.0f, 0.0f, 0.0f,   // curve 0 root
        1.0f, 0.0f, 0.0f,   // curve 1 root
    };

    // 5 non-coplanar rest samples.
    const std::vector<float> restSamples{
        0.0f, 0.0f, 0.0f,   // sample 0
        1.0f, 0.0f, 0.0f,   // sample 1
        0.0f, 1.0f, 0.0f,   // sample 2
        0.0f, 0.0f, 1.0f,   // sample 3
        0.5f, 0.5f, 0.5f,   // sample 4
    };

    auto pointsBuf = Upload(context, points.data(), points.size() * sizeof(float));
    auto offsetsBuf = UploadVulkanDeviceBytes(native, context, offsets.data(),
        offsets.size() * sizeof(uint32_t));
    auto targetsBuf = Upload(context, rootTargets.data(), rootTargets.size() * sizeof(float));
    CHECK(pointsBuf && offsetsBuf && targetsBuf);

    auto makeParams = [](uint32_t lockLiteral = 0, float maskLiteral = 1.0f,
                         uint32_t enabledLiteral = 1, float groomEnvelope = 1.0f) {
        DeformApplyParams p;
        p.groomEnvelope = groomEnvelope;
        p.maskLiteral = maskLiteral; p.maskHasData = false; p.maskDomain = 1;
        p.enabledLiteral = enabledLiteral; p.enabledHasData = false; p.enabledDomain = 1;
        p.lockLiteral = lockLiteral; p.lockHasData = false; p.lockDomain = 1;
        return p;
    };

    // ---- Case 10: a caller-held Output keeps its bytes across reuse ----
    // Runs first, against an empty pool: the outPoints pool reuses a
    // destroyed candidate's buffer for the next same-size pose, but only
    // while the pool owns it alone. An Output held past destruction pins
    // its buffer (the next pose allocates fresh), and dropping the pin lets
    // a later pose reuse it with correct output.
    {
        const std::vector<float> points12{
            0.0f, 0.0f, 0.0f, 0.1f, 0.0f, 0.1f, 0.2f, 0.0f, 0.2f,
            1.0f, 0.0f, 0.0f, 1.1f, 0.0f, 0.1f, 1.2f, 0.0f, 0.2f,
            2.0f, 0.0f, 0.0f, 2.1f, 0.0f, 0.1f, 2.2f, 0.0f, 0.2f,
            3.0f, 0.0f, 0.0f, 3.1f, 0.0f, 0.1f, 3.2f, 0.0f, 0.2f,
        };
        const std::vector<uint32_t> offsets12{0, 3, 6, 9, 12};
        const std::vector<float> roots12{
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            2.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f,
        };
        auto pointsBuf12 = Upload(context, points12.data(), points12.size() * sizeof(float));
        auto offsetsBuf12 = UploadVulkanDeviceBytes(native, context, offsets12.data(),
            offsets12.size() * sizeof(uint32_t));
        auto targetsBuf12 = Upload(context, roots12.data(), roots12.size() * sizeof(float));
        CHECK(pointsBuf12 && offsetsBuf12 && targetsBuf12);
        auto makeInfo = [&](std::vector<float> const& posed) {
            DeformPipeline::BeginInfo info;
            info.points = pointsBuf12;
            info.curveOffsets = offsetsBuf12;
            info.rootTargets = targetsBuf12;
            info.curveCount = 4;
            info.pointCount = 12;
            info.restSamples = restSamples;
            info.posedSamples = posed;
            info.sampleCount = 5;
            info.smoothing = 0.0;
            info.mask = {1.0f, 1, nullptr, 0};
            info.enabled = {1, 1, nullptr, 0};
            info.lockRoots = {0, 1, nullptr, 0};
            info.groomEnvelope = 1.0f;
            return info;
        };
        auto runPose = [&](std::vector<float> const& posed, std::vector<float>* out) {
            DeformSemantic sem = DeformSemantic::Ok;
            auto c = pipe->Begin(makeInfo(posed), &status, &sem);
            if (!c || status != VK_SUCCESS) return false;
            if (!Prove(native)) return false;
            if (c->Poll(&sem) != VK_SUCCESS) return false;
            if (sem != DeformSemantic::Ok || !c->succeeded()) return false;
            return ReadOutput(native, context, c, out);
        };
        std::vector<float> posedA = restSamples, posedB = restSamples;
        for (size_t i = 0; i < posedA.size(); i += 3) posedA[i] += 0.5f;
        for (size_t i = 0; i < posedB.size(); i += 3) posedB[i] -= 0.25f;

        DeformSemantic sem = DeformSemantic::Ok;
        auto cA = pipe->Begin(makeInfo(posedA), &status, &sem);
        CHECK(cA && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(cA->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok && cA->succeeded());
        auto held = cA->output();   // pins A's buffer past cA's death
        CHECK(held.points);
        std::vector<float> outA;
        CHECK(ReadOutput(native, context, cA, &outA));
        cA.reset();                 // A's buffer rejoins the pool, still shared

        std::vector<float> outB;
        CHECK(runPose(posedB, &outB));
        CHECK(outB.size() == outA.size() && outA.size() == 36 && outB != outA);
        for (int i = 0; i < 12; ++i) {
            CHECK(Near(V(outA[size_t(i * 3)], outA[size_t(i * 3 + 1)], outA[size_t(i * 3 + 2)]),
                       V(points12[size_t(i * 3)] + 0.5f, points12[size_t(i * 3 + 1)],
                         points12[size_t(i * 3 + 2)]), 1e-3f));
            CHECK(Near(V(outB[size_t(i * 3)], outB[size_t(i * 3 + 1)], outB[size_t(i * 3 + 2)]),
                       V(points12[size_t(i * 3)] - 0.25f, points12[size_t(i * 3 + 1)],
                         points12[size_t(i * 3 + 2)]), 1e-3f));
        }
        // The held Output still reads pose A's bytes: B must have allocated
        // fresh (or reused an older idle buffer), never A's pinned buffer.
        std::vector<uint8_t> bytesA;
        CHECK(ReadVulkanBytes(native, context, held.points->buffer(),
                              held.points->sizeBytes(), held.points, &bytesA));
        CHECK(bytesA.size() == outA.size() * sizeof(float));
        CHECK(std::memcmp(bytesA.data(), outA.data(), bytesA.size()) == 0);
        // Dropping the pin lets a later same-size pose reuse A's buffer;
        // the output is bitwise the earlier pose-B output, so no stale byte
        // survives the reuse.
        held.points.reset();
        std::vector<float> outC;
        CHECK(runPose(posedB, &outC));
        CHECK(outC.size() == outB.size());
        CHECK(std::memcmp(outC.data(), outB.data(), outC.size() * sizeof(float)) == 0);
        std::puts("Case 10 (held Output survives reuse): PASS");
    }

    // ---- Case 1: Empty topology ----
    {
        DeformPipeline::BeginInfo info;
        info.curveCount = 0;
        info.pointCount = 0;
        info.sampleCount = 5;
        info.restSamples = restSamples;
        info.posedSamples = restSamples;  // identity
        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c->succeeded());
        CHECK(c->pointCount() == 0);
        CHECK(!c->output().points);
        std::puts("Case 1 (empty topology): PASS");
    }

    // ---- Case 2: Identity pose (posed == rest) → output == input ----
    {
        std::vector<float> posed = restSamples;
        DeformPipeline::BeginInfo info;
        info.points = pointsBuf;
        info.curveOffsets = offsetsBuf;
        info.rootTargets = targetsBuf;
        info.curveCount = 2;
        info.pointCount = 6;
        info.restSamples = restSamples;
        info.posedSamples = posed;
        info.sampleCount = 5;
        info.smoothing = 0.0;
        info.mask = {1.0f, 1, nullptr, 0};
        info.enabled = {1, 1, nullptr, 0};
        info.lockRoots = {0, 1, nullptr, 0};
        info.groomEnvelope = 1.0f;

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c->succeeded());

        std::vector<float> out;
        CHECK(ReadOutput(native, context, c, &out));
        CHECK(out.size() == 18);
        for (int i = 0; i < 6; ++i) {
            float3 got = V(out[i*3], out[i*3+1], out[i*3+2]);
            float3 exp = V(points[i*3], points[i*3+1], points[i*3+2]);
            CHECK(Near(got, exp, 1e-4f));
        }
        std::puts("Case 2 (identity pose): PASS");
    }

    // ---- Case 3: Rigid translation → output = input + d ----
    {
        const float d[3] = {0.3f, -0.2f, 0.5f};
        std::vector<float> posed(15);
        for (int i = 0; i < 5; ++i)
            for (int ax = 0; ax < 3; ++ax)
                posed[i*3+ax] = restSamples[i*3+ax] + d[ax];

        DeformPipeline::BeginInfo info;
        info.points = pointsBuf;
        info.curveOffsets = offsetsBuf;
        info.rootTargets = targetsBuf;
        info.curveCount = 2;
        info.pointCount = 6;
        info.restSamples = restSamples;
        info.posedSamples = posed;
        info.sampleCount = 5;
        info.smoothing = 0.0;
        info.mask = {1.0f, 1, nullptr, 0};
        info.enabled = {1, 1, nullptr, 0};
        info.lockRoots = {0, 1, nullptr, 0};
        info.groomEnvelope = 1.0f;

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c->succeeded());

        std::vector<float> out;
        CHECK(ReadOutput(native, context, c, &out));
        CHECK(out.size() == 18);
        // Print all points first to diagnose.
        for (int i = 0; i < 6; ++i) {
            float3 got = V(out[i*3], out[i*3+1], out[i*3+2]);
            float3 exp = V(points[i*3]+d[0], points[i*3+1]+d[1], points[i*3+2]+d[2]);
            std::fprintf(stderr, "  case3 p%d got(%.6f,%.6f,%.6f) exp(%.6f,%.6f,%.6f)\n", i,
                got.x, got.y, got.z, exp.x, exp.y, exp.z);
        }
        for (int i = 0; i < 6; ++i) {
            float3 got = V(out[i*3], out[i*3+1], out[i*3+2]);
            float3 exp = V(points[i*3]+d[0], points[i*3+1]+d[1], points[i*3+2]+d[2]);
            CHECK(Near(got, exp, 1e-3f));
        }
        std::puts("Case 3 (rigid translation): PASS");
    }

    // Connected Groom fields contain one element and broadcast over two curves.
    {
        std::vector<float> posed=restSamples;
        for(size_t i=0;i<posed.size();i+=3){posed[i]+=.4f;posed[i+1]+=.2f;posed[i+2]+=.6f;}
        float mask=.25f;uint32_t enabled=1,lock=0;
        DeformPipeline::BeginInfo info;
        info.points=pointsBuf;info.curveOffsets=offsetsBuf;info.rootTargets=targetsBuf;
        info.curveCount=2;info.pointCount=6;info.restSamples=restSamples;info.posedSamples=posed;info.sampleCount=5;
        info.mask={1,1,Upload(context,&mask,4),1};
        info.enabled={1,1,Upload(context,&enabled,4),1};
        info.lockRoots={1,1,Upload(context,&lock,4),1};
        auto c=pipe->Begin(info,&status);CHECK(c&&status==VK_SUCCESS&&Prove(native));
        DeformSemantic sem=DeformSemantic::Ok;CHECK(c->Poll(&sem)==VK_SUCCESS&&sem==DeformSemantic::Ok&&c->succeeded());
        std::vector<float> out;CHECK(ReadOutput(native,context,c,&out));
        for(size_t i=0;i<points.size();i+=3)
            CHECK(Near(V(out[i],out[i+1],out[i+2]),V(points[i]+.1f,points[i+1]+.05f,points[i+2]+.15f),1e-4f));
        enabled=0;info.enabled.data=Upload(context,&enabled,4);
        c=pipe->Begin(info,&status);CHECK(c&&Prove(native)&&c->Poll(&sem)==VK_SUCCESS&&c->succeeded());
        CHECK(ReadOutput(native,context,c,&out)&&out==points);
        std::puts("Groom connected fields broadcast over every curve: PASS");
    }

    // ---- Case 4: General non-rigid pose vs CPU oracle ----
    {
        // Non-rigid: each sample displaced by a different amount.
        std::vector<float> posed(15);
        for (int i = 0; i < 5; ++i) {
            posed[i*3+0] = restSamples[i*3+0] + 0.1f * i;
            posed[i*3+1] = restSamples[i*3+1] + 0.05f * (i * i);
            posed[i*3+2] = restSamples[i*3+2] + 0.02f * i * (i + 1);
        }

        DeformPipeline::BeginInfo info;
        info.points = pointsBuf;
        info.curveOffsets = offsetsBuf;
        info.rootTargets = targetsBuf;
        info.curveCount = 2;
        info.pointCount = 6;
        info.restSamples = restSamples;
        info.posedSamples = posed;
        info.sampleCount = 5;
        info.smoothing = 0.0;
        info.mask = {1.0f, 1, nullptr, 0};
        info.enabled = {1, 1, nullptr, 0};
        info.lockRoots = {0, 1, nullptr, 0};
        info.groomEnvelope = 1.0f;

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c->succeeded());

        std::vector<float> out;
        CHECK(ReadOutput(native, context, c, &out));
        CHECK(out.size() == 18);

        // CPU oracle.
        auto params = makeParams();
        DeformApplyParams p = params;
        RbfState rstate;
        SolveRbf(reinterpret_cast<float3 const*>(restSamples.data()),
                 reinterpret_cast<float3 const*>(posed.data()),
                 5, 0.0, rstate);
        CHECK(rstate.status == RbfStatus::Code::Ok);
        std::vector<float3> warped(6);
        for (int i = 0; i < 6; ++i) {
            float3 pt = V(points[i*3], points[i*3+1], points[i*3+2]);
            warped[i] = RbfEvaluate(rstate, reinterpret_cast<float3 const*>(restSamples.data()), pt);
        }
        p.offsets = offsets.data();
        p.targets = reinterpret_cast<float3 const*>(rootTargets.data());
        p.curveCount = 2; p.pointCount = 6;
        std::vector<float3> expected;
        CHECK(RbfApply(p, reinterpret_cast<float3 const*>(points.data()),
                       warped.data(), &expected));
        for (int i = 0; i < 6; ++i) {
            float3 got = V(out[i*3], out[i*3+1], out[i*3+2]);
            CHECK(Near(got, expected[i], 2e-3f));
        }
        std::puts("Case 4 (non-rigid vs oracle): PASS");
    }

    // ---- Case 5: lockRoots = 1 → oracle with correction ----
    {
        std::vector<float> posed(15);
        for (int i = 0; i < 5; ++i) {
            posed[i*3+0] = restSamples[i*3+0] + 0.05f * i;
            posed[i*3+1] = restSamples[i*3+1] + 0.03f * (i * i);
            posed[i*3+2] = restSamples[i*3+2] + 0.01f * i;
        }

        DeformPipeline::BeginInfo info;
        info.points = pointsBuf;
        info.curveOffsets = offsetsBuf;
        info.rootTargets = targetsBuf;
        info.curveCount = 2;
        info.pointCount = 6;
        info.restSamples = restSamples;
        info.posedSamples = posed;
        info.sampleCount = 5;
        info.smoothing = 0.0;
        info.mask = {1.0f, 1, nullptr, 0};
        info.enabled = {1, 1, nullptr, 0};
        info.lockRoots = {1, 1, nullptr, 0};  // lock enabled
        info.groomEnvelope = 1.0f;

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c->succeeded());

        std::vector<float> out;
        CHECK(ReadOutput(native, context, c, &out));
        CHECK(out.size() == 18);

        // CPU oracle with lock.
        DeformApplyParams p = makeParams(1);  // lockLiteral=1
        RbfState rstate;
        SolveRbf(reinterpret_cast<float3 const*>(restSamples.data()),
                 reinterpret_cast<float3 const*>(posed.data()),
                 5, 0.0, rstate);
        CHECK(rstate.status == RbfStatus::Code::Ok);
        std::vector<float3> warped(6);
        for (int i = 0; i < 6; ++i) {
            float3 pt = V(points[i*3], points[i*3+1], points[i*3+2]);
            warped[i] = RbfEvaluate(rstate, reinterpret_cast<float3 const*>(restSamples.data()), pt);
        }
        p.offsets = offsets.data();
        p.targets = reinterpret_cast<float3 const*>(rootTargets.data());
        p.curveCount = 2; p.pointCount = 6;
        std::vector<float3> expected;
        CHECK(RbfApply(p, reinterpret_cast<float3 const*>(points.data()),
                       warped.data(), &expected));
        for (int i = 0; i < 6; ++i) {
            float3 got = V(out[i*3], out[i*3+1], out[i*3+2]);
            CHECK(Near(got, expected[i], 2e-3f));
        }
        std::puts("Case 5 (lockRoots): PASS");
    }

    // ---- Case 6: mask literal 0.0 → bitwise input passthrough ----
    {
        std::vector<float> posed(15);
        for (int i = 0; i < 5; ++i)
            for (int ax = 0; ax < 3; ++ax)
                posed[i*3+ax] = restSamples[i*3+ax] + 0.1f * i;

        DeformPipeline::BeginInfo info;
        info.points = pointsBuf;
        info.curveOffsets = offsetsBuf;
        info.rootTargets = targetsBuf;
        info.curveCount = 2;
        info.pointCount = 6;
        info.restSamples = restSamples;
        info.posedSamples = posed;
        info.sampleCount = 5;
        info.smoothing = 0.0;
        info.mask = {0.0f, 1, nullptr, 0};  // mask = 0
        info.enabled = {1, 1, nullptr, 0};
        info.lockRoots = {0, 1, nullptr, 0};
        info.groomEnvelope = 1.0f;

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c->succeeded());

        std::vector<float> out;
        CHECK(ReadOutput(native, context, c, &out));
        CHECK(out.size() == 18);
        CHECK(std::memcmp(out.data(), points.data(), points.size() * sizeof(float)) == 0);
        std::puts("Case 6 (mask=0 passthrough): PASS");
    }

    // ---- Case 7: enabled literal 0 → bitwise input passthrough ----
    {
        std::vector<float> posed(15);
        for (int i = 0; i < 5; ++i)
            for (int ax = 0; ax < 3; ++ax)
                posed[i*3+ax] = restSamples[i*3+ax] + 0.1f * i;

        DeformPipeline::BeginInfo info;
        info.points = pointsBuf;
        info.curveOffsets = offsetsBuf;
        info.rootTargets = targetsBuf;
        info.curveCount = 2;
        info.pointCount = 6;
        info.restSamples = restSamples;
        info.posedSamples = posed;
        info.sampleCount = 5;
        info.smoothing = 0.0;
        info.mask = {1.0f, 1, nullptr, 0};
        info.enabled = {0, 1, nullptr, 0};  // disabled
        info.lockRoots = {0, 1, nullptr, 0};
        info.groomEnvelope = 1.0f;

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c->succeeded());

        std::vector<float> out;
        CHECK(ReadOutput(native, context, c, &out));
        CHECK(out.size() == 18);
        CHECK(std::memcmp(out.data(), points.data(), points.size() * sizeof(float)) == 0);
        std::puts("Case 7 (enabled=0 passthrough): PASS");
    }

    // ---- Case 8: Device offsets and host sample validation ----
    // 8a: Bad initial/final endpoints, zero-length and decreasing segments
    // are semantic failures, discovered asynchronously from device memory.
    for (auto const& badOffsets : std::vector<std::vector<uint32_t>>{
            {1, 3, 6}, {0, 3, 5}, {0, 0, 6}, {0, 7, 6}, {0, 3, 3}}) {
        auto badOffBuf = UploadVulkanDeviceBytes(native, context, badOffsets.data(),
            badOffsets.size() * sizeof(uint32_t));
        CHECK(badOffBuf);

        DeformPipeline::BeginInfo info;
        info.points = pointsBuf;
        info.curveOffsets = badOffBuf;
        info.rootTargets = targetsBuf;
        info.curveCount = 2;
        info.pointCount = 6;
        info.restSamples = restSamples;
        info.posedSamples = restSamples;
        info.sampleCount = 5;
        info.mask = {1.0f, 1, nullptr, 0};
        info.enabled = {1, 1, nullptr, 0};
        info.lockRoots = {0, 1, nullptr, 0};

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::BadOffsets && !c->succeeded());
        std::puts("Case 8a (reject bad offsets): PASS");
    }
    // 8b: NaN in restSamples → NonFinite
    {
        std::vector<float> nanSamples = restSamples;
        nanSamples[0] = std::nanf("");
        DeformPipeline::BeginInfo info;
        info.points = pointsBuf;
        info.curveOffsets = offsetsBuf;
        info.rootTargets = targetsBuf;
        info.curveCount = 2;
        info.pointCount = 6;
        info.restSamples = nanSamples;
        info.posedSamples = restSamples;
        info.sampleCount = 5;
        info.mask = {1.0f, 1, nullptr, 0};
        info.enabled = {1, 1, nullptr, 0};
        info.lockRoots = {0, 1, nullptr, 0};

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(!c && status == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(sem == DeformSemantic::NonFinite);
        std::puts("Case 8b (reject NaN samples): PASS");
    }
    // 8c: the same coplanar rest twice rejects identically. The first Begin
    // binds and fails; the second replays the cached bind failure.
    {
        std::vector<float> flatSamples = restSamples;
        for (size_t i = 0; i < 5; ++i) flatSamples[i * 3 + 2] = 0.0f;
        for (int attempt = 0; attempt < 2; ++attempt) {
            DeformPipeline::BeginInfo info;
            info.points = pointsBuf;
            info.curveOffsets = offsetsBuf;
            info.rootTargets = targetsBuf;
            info.curveCount = 2;
            info.pointCount = 6;
            info.restSamples = flatSamples;
            info.posedSamples = restSamples;
            info.sampleCount = 5;
            info.mask = {1.0f, 1, nullptr, 0};
            info.enabled = {1, 1, nullptr, 0};
            info.lockRoots = {0, 1, nullptr, 0};

            DeformSemantic sem = DeformSemantic::Ok;
            auto c = pipe->Begin(std::move(info), &status, &sem);
            CHECK(!c && status == VK_ERROR_INITIALIZATION_FAILED);
            CHECK(sem == DeformSemantic::RankDeficient);
        }
        std::puts("Case 8c (repeat coplanar rest rejects twice): PASS");
    }

    // ---- Case 9: ragged spans (1, 2, 3 points) with per-curve fields ----
    // The per-point apply binary-searches offsets for its curve; ragged
    // spans (including a span of 1) plus primitive-domain fields pin the
    // mapping (groom literals are curve-independent and cannot). Under a
    // rigid translation d: a primitive mask of [0, 1, 0] holds curves 0
    // and 2 at their input bitwise while curve 1 moves by d; a primitive
    // lock of [1, 0, 1] holds the locked curves at their roots while the
    // unlocked one moves by d.
    {
        const std::vector<float> rPoints{
            0.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f, 1.1f, 0.0f, 0.1f,
            2.0f, 0.0f, 0.0f, 2.1f, 0.0f, 0.1f, 2.2f, 0.0f, 0.2f,
        };
        const std::vector<uint32_t> rOffsets{0, 1, 3, 6};
        const std::vector<float> rTargets{
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f,
        };
        const std::vector<float> rMask{0.0f, 1.0f, 0.0f};
        const std::vector<uint32_t> rLock{1u, 0u, 1u};
        auto rPointsBuf = Upload(context, rPoints.data(), rPoints.size() * sizeof(float));
        auto rOffsetsBuf = UploadVulkanDeviceBytes(native, context, rOffsets.data(),
            rOffsets.size() * sizeof(uint32_t));
        auto rTargetsBuf = Upload(context, rTargets.data(), rTargets.size() * sizeof(float));
        auto rMaskBuf = Upload(context, rMask.data(), rMask.size() * sizeof(float));
        auto rLockBuf = Upload(context, rLock.data(), rLock.size() * sizeof(uint32_t));
        CHECK(rPointsBuf && rOffsetsBuf && rTargetsBuf && rMaskBuf && rLockBuf);
        const float d[3] = {0.3f, -0.2f, 0.5f};
        std::vector<float> posed(15);
        for (int i = 0; i < 5; ++i)
            for (int ax = 0; ax < 3; ++ax)
                posed[size_t(i * 3 + ax)] = restSamples[size_t(i * 3 + ax)] + d[ax];

        // 9a: primitive mask [0, 1, 0].
        DeformPipeline::BeginInfo info;
        info.points = rPointsBuf;
        info.curveOffsets = rOffsetsBuf;
        info.rootTargets = rTargetsBuf;
        info.curveCount = 3;
        info.pointCount = 6;
        info.restSamples = restSamples;
        info.posedSamples = posed;
        info.sampleCount = 5;
        info.smoothing = 0.0;
        info.mask = {0.0f, 2, rMaskBuf, 3};
        info.enabled = {1, 1, nullptr, 0};
        info.lockRoots = {0, 1, nullptr, 0};
        info.groomEnvelope = 1.0f;

        DeformSemantic sem = DeformSemantic::Ok;
        auto c = pipe->Begin(std::move(info), &status, &sem);
        CHECK(c && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c->succeeded());

        std::vector<float> out;
        CHECK(ReadOutput(native, context, c, &out));
        CHECK(out.size() == 18);
        // Mask-0 curves pass the input through bitwise.
        CHECK(std::memcmp(out.data(), rPoints.data(), 3 * sizeof(float)) == 0);
        CHECK(std::memcmp(out.data() + 9, rPoints.data() + 9, 9 * sizeof(float)) == 0);
        for (int i = 1; i < 3; ++i) {
            float3 got = V(out[size_t(i * 3)], out[size_t(i * 3 + 1)], out[size_t(i * 3 + 2)]);
            float3 exp = V(rPoints[size_t(i * 3)] + d[0], rPoints[size_t(i * 3 + 1)] + d[1],
                           rPoints[size_t(i * 3 + 2)] + d[2]);
            CHECK(Near(got, exp, 1e-4f));
        }
        std::puts("Case 9a (ragged primitive mask): PASS");

        // 9b: primitive lock [1, 0, 1].
        DeformPipeline::BeginInfo info2;
        info2.points = rPointsBuf;
        info2.curveOffsets = rOffsetsBuf;
        info2.rootTargets = rTargetsBuf;
        info2.curveCount = 3;
        info2.pointCount = 6;
        info2.restSamples = restSamples;
        info2.posedSamples = posed;
        info2.sampleCount = 5;
        info2.smoothing = 0.0;
        info2.mask = {1.0f, 1, nullptr, 0};
        info2.enabled = {1, 1, nullptr, 0};
        info2.lockRoots = {0, 2, rLockBuf, 3};
        info2.groomEnvelope = 1.0f;
        auto c2 = pipe->Begin(std::move(info2), &status, &sem);
        CHECK(c2 && status == VK_SUCCESS);
        CHECK(Prove(native));
        CHECK(c2->Poll(&sem) == VK_SUCCESS);
        CHECK(sem == DeformSemantic::Ok);
        CHECK(c2->succeeded());
        std::vector<float> out2;
        CHECK(ReadOutput(native, context, c2, &out2));
        CHECK(out2.size() == 18);
        for (int i = 0; i < 6; ++i) {
            bool const locked = (i < 1 || i >= 3);
            float3 got = V(out2[size_t(i * 3)], out2[size_t(i * 3 + 1)], out2[size_t(i * 3 + 2)]);
            float3 exp = locked ? V(rPoints[size_t(i * 3)], rPoints[size_t(i * 3 + 1)],
                                          rPoints[size_t(i * 3 + 2)])
                                : V(rPoints[size_t(i * 3)] + d[0], rPoints[size_t(i * 3 + 1)] + d[1],
                                    rPoints[size_t(i * 3 + 2)] + d[2]);
            CHECK(Near(got, exp, 1e-4f));
        }
        std::puts("Case 9b (ragged primitive lock): PASS");
    }

    std::puts("Vulkan deform pipeline: PASS");
    return 0;
}
