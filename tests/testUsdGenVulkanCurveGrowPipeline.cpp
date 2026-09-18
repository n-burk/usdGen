#include "usdGen/vulkan/curveGrowPipeline.h"
#include "vulkanNativeFixture.h"
#include "floatUlpFixture.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace usdGen;
using namespace usdGen::vulkan;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Vulkan CurveGrow pipeline check failed: %s (%d)\n", #x, __LINE__); return 1; } } while (false)

// Test-only queue proof. Production Candidate::Poll never waits or reads back
// geometry. This fence is submitted after the candidate on its confined queue.
static bool Prove(std::shared_ptr<NativeOwner> const& native) {
    if (vkResetFences(native->device, 1, &native->fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    return vkQueueSubmit(native->queue, 1, &submit, native->fence) == VK_SUCCESS &&
        vkWaitForFences(native->device, 1, &native->fence, VK_TRUE, 10000000000ull) == VK_SUCCESS;
}

template <typename T>
static std::shared_ptr<ChargedBuffer> Upload(std::shared_ptr<DeviceContext> const& context,
    std::shared_ptr<NativeOwner> const& native, std::vector<T> const& values) {
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = values.size() * sizeof(T);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto buffer = ChargedBuffer::Create(context, bi,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        UsdGenExecutionResourceKind::Active);
    if (!buffer) return buffer;
    void* mapped = nullptr;
    if (vkMapMemory(native->device, buffer->memory(), 0, bi.size, 0, &mapped) != VK_SUCCESS)
        return std::shared_ptr<ChargedBuffer>{};
    std::memcpy(mapped, values.data(), bi.size); vkUnmapMemory(native->device, buffer->memory());
    return buffer;
}

template <typename T>
static bool Download(std::shared_ptr<NativeOwner> const& native,
    std::shared_ptr<const ChargedBuffer> const& source, std::vector<T>* values) {
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = values->size() * sizeof(T); bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto staging = ChargedBuffer::Create(source->context(), bi,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        UsdGenExecutionResourceKind::Scratch);
    if (!staging) return false;
    VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = native->commands; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(native->device, &ai, &command) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
    VkBufferCopy region{0, 0, bi.size};
    vkCmdCopyBuffer(command, source->buffer(), staging->buffer(), 1, &region);
    VkMemoryBarrier barrier{}; barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (vkEndCommandBuffer(command) != VK_SUCCESS ||
        vkResetFences(native->device, 1, &native->fence) != VK_SUCCESS) return false;
    struct Keep { std::shared_ptr<NativeOwner> native; std::shared_ptr<const ChargedBuffer> source; };
    auto keep = std::make_shared<Keep>(Keep{native, source});
    if (staging->MarkSubmitted(native->fence, keep) != VK_SUCCESS) return false;
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    if (vkQueueSubmit(native->queue, 1, &submit, native->fence) != VK_SUCCESS ||
        vkWaitForFences(native->device, 1, &native->fence, VK_TRUE, 10000000000ull) != VK_SUCCESS ||
        staging->PollComplete() != VK_SUCCESS) return false;
    void* mapped = nullptr;
    if (vkMapMemory(native->device, staging->memory(), 0, bi.size, 0, &mapped) != VK_SUCCESS) return false;
    std::memcpy(values->data(), mapped, bi.size); vkUnmapMemory(native->device, staging->memory());
    vkFreeCommandBuffers(native->device, native->commands, 1, &command);
    return true;
}

// Deterministic per-curve geometry with dvanced basis (rootT/rootB/rootN).
struct Geometry {
    uint32_t curves, pointCount;
    std::vector<float> points, rest, rootUV;
    std::vector<uint32_t> curveOffsets;
    std::vector<uint64_t> stableIds;
    std::vector<int32_t> rootPrim;
    std::vector<float> rootT, rootB, rootN;
    uint32_t pointsPerCurve = 2;
    Geometry(uint32_t c, uint32_t ppc) : curves(c), pointCount(c*ppc), pointsPerCurve(ppc) {
        curveOffsets.resize(c+1, 0);
        for (uint32_t i = 0; i <= c; ++i) curveOffsets[i] = i*ppc;
        for (uint32_t i = 0; i < c; ++i) {
            for (uint32_t p = 0; p < ppc; ++p) {
                float x = float(i)*10.f + float(p), y = float(i)*10.f + p*2.f, z = 0.5f + float(i);
                points.push_back(x); points.push_back(y); points.push_back(z);
                rest.push_back(x); rest.push_back(y); rest.push_back(z);
            }
            uint64_t id = uint64_t(i+1);
            stableIds.push_back(id);
            rootPrim.push_back(int32_t(i*3 + 1));
            float u = float(i)/float(c), v = 0.25f;
            rootUV.push_back(u); rootUV.push_back(v);
            rootT.push_back(1.f); rootT.push_back(0.f); rootT.push_back(0.f);
            rootB.push_back(0.f); rootB.push_back(0.f); rootB.push_back(1.f);
            rootN.push_back(0.f); rootN.push_back(1.f); rootN.push_back(0.f);
        }
    }
};

int main(int argc, char** argv) {
    bool quarantine = argc == 3 && std::string(argv[2]) == "--quarantine";
    CHECK(argc == 2 || quarantine);
    std::ifstream shader(argv[1], std::ios::binary);
    CHECK(shader);
    std::vector<char> bytes((std::istreambuf_iterator<char>(shader)), {});
    CHECK(!bytes.empty() && bytes.size() % sizeof(uint32_t) == 0);
    std::vector<uint32_t> code(bytes.size() / sizeof(uint32_t));
    std::memcpy(code.data(), bytes.data(), bytes.size());
    bool unavailable = false;
    auto native = CreateNative(&unavailable);
    if (unavailable) return 77;
    CHECK(native);
    {   // grow kernel needs 24 storage-buffer bindings; skip weak devices (77).
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(native->physical, &props);
        if (props.limits.maxPerStageDescriptorStorageBuffers < 24 ||
            props.limits.maxDescriptorSetStorageBuffers < 24) return 77;
    }
    std::weak_ptr<NativeOwner> weakNative = native;
    DeviceContext::CreateInfo info;
    info.instance = native->instance; info.physicalDevice = native->physical;
    info.device = native->device; info.computeQueue = native->queue;
    info.computeQueueFamily = native->family; info.physicalIndex = native->physicalIndex;
    info.resourceDeviceId = 7004; info.nativeLifetime = native;
    info.resources = {size_t{16} << 20, 0};
    auto context = DeviceContext::Create(info);
    CHECK(context); info.nativeLifetime.reset();
    auto pool = context->resources(); auto baseline = pool->Snapshot();
    VkResult status = VK_SUCCESS;
    CHECK(!CurveGrowPipeline::Create(context, {0x07230203u}, &status));
    CHECK(status == VK_ERROR_INITIALIZATION_FAILED);
    auto pipeline = CurveGrowPipeline::Create(context, code, &status);
    CHECK(pipeline && status == VK_SUCCESS);

    // ------------------------------------------------------------------ (a)
    uint32_t const curves = 4, ppc = 8;
    Geometry geo(curves, ppc);
    CurveGrowPipeline::Controls controls;
    controls.cvCount = ppc; controls.seed = 7; controls.length = 1.5;
    controls.randomLo = 1.0; controls.randomHi = 1.0;  // uniform targets
    controls.lift = 0.f; controls.fallbackWidth = 0.02f;
    controls.direction = CurveGrowPipeline::Direction::RootNormal;
    auto targets = CurveGrowPipeline::BuildTargets(geo.stableIds, controls);
    CHECK(targets.size() == curves);
    {
        auto before = pool->Snapshot();
        auto in = CurveGrowPipeline::Input{};
        in.points = Upload(context, native, geo.points);
        in.rest = Upload(context, native, geo.rest);
        in.widths = Upload(context, native, std::vector<float>(geo.pointCount, 0.03f));
        in.curveOffsets = Upload(context, native, geo.curveOffsets);
        in.stableIds = Upload(context, native, geo.stableIds);
        in.rootPrim = Upload(context, native, geo.rootPrim);
        in.rootUV = Upload(context, native, geo.rootUV);
        in.rootT = Upload(context, native, geo.rootT);
        in.rootB = Upload(context, native, geo.rootB);
        in.rootN = Upload(context, native, geo.rootN);
        // frameStableIds unset: root frames indexed by compacted curve ordinal.
        in.targets = Upload(context, native, targets);
        CHECK(in.points && in.rest && in.widths && in.curveOffsets && in.stableIds &&
              in.rootPrim && in.rootUV && in.rootT && in.rootB && in.rootN && in.targets);
        auto cand = pipeline->Begin(native, in, curves, geo.pointCount, controls, &status);
        CHECK(cand && status == VK_SUCCESS && !cand->output() && cand->curveCount() == curves);
        CHECK(Prove(native));
        CurveGrowPipeline::Candidate::Semantic semantic;
        CHECK(cand->Poll(&semantic) == VK_SUCCESS && semantic == CurveGrowPipeline::Candidate::Semantic::Ok);

        // Oracle: exact same host inputs.
        CurveGrowPipeline::Outputs oracle;
        bool ok = CurveGrowPipeline::BuildCpu(geo.points, geo.rest,
            std::vector<float>(geo.pointCount, 0.03f), geo.curveOffsets, geo.stableIds,
            geo.rootPrim, geo.rootUV, geo.rootT, geo.rootB, geo.rootN,
            /*frameStableIds*/{}, targets, controls, &oracle);
        CHECK(ok);

        uint32_t gpuPts = curves * ppc;
        std::vector<float> got; std::vector<uint32_t> gotOff; std::vector<uint64_t> gotId;
        std::vector<int32_t> gotPrim; std::vector<float> gotUv, gotT, gotB, gotN;
        got.resize(gpuPts*3); CHECK(Download(native, cand->output(), &got));
        // Float planes compare within 4 ULPs: the GLSL build kernel fuses
        // mul+add where the host oracle BuildCpu does not, so exact equality
        // is not the frozen contract (same 4-ULP convention as EnvelopeV1).
        CHECK(FloatBytesWithinUlps(got.data(), oracle.points.data(),
                                   got.size() * sizeof(float), 4u));
        got.resize(gpuPts*3); CHECK(Download(native, cand->rest(), &got));
        CHECK(FloatBytesWithinUlps(got.data(), oracle.rest.data(),
                                   got.size() * sizeof(float), 4u));
        got.resize(gpuPts); CHECK(Download(native, cand->widths(), &got));
        CHECK(FloatBytesWithinUlps(got.data(), oracle.widths.data(),
                                   got.size() * sizeof(float), 4u));
        got.resize(gpuPts); CHECK(Download(native, cand->hairT(), &got));
        CHECK(FloatBytesWithinUlps(got.data(), oracle.hairT.data(),
                                   got.size() * sizeof(float), 4u));
        gotOff.resize(curves+1); CHECK(Download(native, cand->offsets(), &gotOff));
        CHECK(gotOff == oracle.offsets);
        gotId.resize(curves); CHECK(Download(native, cand->ids(), &gotId));
        CHECK(gotId == oracle.ids);
        gotPrim.resize(curves); CHECK(Download(native, cand->rootPrim(), &gotPrim));
        CHECK(gotPrim == oracle.rootPrim);
        gotUv.resize(curves*2); CHECK(Download(native, cand->rootUV(), &gotUv));
        CHECK(FloatBytesWithinUlps(gotUv.data(), oracle.rootUV.data(),
                                   gotUv.size() * sizeof(float), 4u));
        gotT.resize(curves*3); CHECK(Download(native, cand->rootT(), &gotT));
        CHECK(FloatBytesWithinUlps(gotT.data(), oracle.rootT.data(),
                                   gotT.size() * sizeof(float), 4u));
        gotB.resize(curves*3); CHECK(Download(native, cand->rootB(), &gotB));
        CHECK(FloatBytesWithinUlps(gotB.data(), oracle.rootB.data(),
                                   gotB.size() * sizeof(float), 4u));
        gotN.resize(curves*3); CHECK(Download(native, cand->rootN(), &gotN));
        CHECK(FloatBytesWithinUlps(gotN.data(), oracle.rootN.data(),
                                   gotN.size() * sizeof(float), 4u));
        cand.reset(); in = CurveGrowPipeline::Input{};
        CHECK(pool->Snapshot().usedBytes == before.usedBytes && pool->Snapshot().byKind == before.byKind);
    }

    // ---------------------------------------------------------- (b) (c) (e)
    // Mapped mode: sorted frame table with all ids present.
    auto frame = std::vector<uint64_t>{geo.stableIds};
    auto frameBuf = Upload(context, native, frame);
    CHECK(frameBuf);
    auto makeInput = [&](Geometry const& g, std::vector<uint64_t> const& ids,
                         std::shared_ptr<ChargedBuffer> const& frameTable, std::vector<float> const& tgt) {
        auto in = CurveGrowPipeline::Input{};
        in.points = Upload(context, native, g.points);
        in.rest = Upload(context, native, g.rest);
        in.widths = Upload(context, native, std::vector<float>(g.pointCount, 0.03f));
        in.curveOffsets = Upload(context, native, g.curveOffsets);
        in.stableIds = Upload(context, native, ids);
        in.rootPrim = Upload(context, native, g.rootPrim);
        in.rootUV = Upload(context, native, g.rootUV);
        in.rootT = Upload(context, native, g.rootT);
        in.rootB = Upload(context, native, g.rootB);
        in.rootN = Upload(context, native, g.rootN);
        in.frameStableIds = frameTable;
        in.targets = Upload(context, native, tgt);
        return in;
    };
    auto tgt = CurveGrowPipeline::BuildTargets(geo.stableIds, controls);

    // (b): one curve's stableId absent from the non-empty sorted frame table.
    {
        auto table = std::vector<uint64_t>{geo.stableIds[0], geo.stableIds[1], geo.stableIds[3]};  // misses id 4
        auto tableBuf = Upload(context, native, table); CHECK(tableBuf);
        auto mIssIds = geo.stableIds;  // all four ids; one absent from table
        // Mapped mode sizes the frame arrays by the table, not the curves.
        auto g3 = geo;
        g3.rootT.resize(table.size() * 3); g3.rootB.resize(table.size() * 3);
        g3.rootN.resize(table.size() * 3);
        auto in = makeInput(g3, mIssIds, tableBuf, tgt);
        auto cand = pipeline->Begin(native, in, curves, geo.pointCount, controls, &status);
        CHECK(cand && status == VK_SUCCESS && Prove(native));
        CurveGrowPipeline::Candidate::Semantic semantic;
        CHECK(cand->Poll(&semantic) == VK_SUCCESS &&
              semantic == CurveGrowPipeline::Candidate::Semantic::InvalidTopology);
        cand.reset();
    }

    // (c): descending (unsorted) frame table -> phase1 validateFrameOrder.
    {
        auto table = std::vector<uint64_t>(geo.stableIds.rbegin(), geo.stableIds.rend());
        auto tableBuf = Upload(context, native, table); CHECK(tableBuf);
        auto in = makeInput(geo, geo.stableIds, tableBuf, tgt);
        // Header contract: host must NOT pre-reject sortedness; Begin runs and
        // the shader's validateFrameOrder reports InvalidTopology.
        auto cand = pipeline->Begin(native, in, curves, geo.pointCount, controls, &status);
        CHECK(cand && status == VK_SUCCESS && Prove(native));
        CurveGrowPipeline::Candidate::Semantic semantic;
        CHECK(cand->Poll(&semantic) == VK_SUCCESS &&
              semantic == CurveGrowPipeline::Candidate::Semantic::InvalidTopology);
        cand.reset();
    }

    // (e): lift rotation qualitative, RootNormal direction.
    {
        uint32_t const eCurves = 3, ePpc = 8;
        Geometry eGeo(eCurves, ePpc);
        auto eIds = eGeo.stableIds;
        auto eTargets = CurveGrowPipeline::BuildTargets(eIds, controls);
        auto lift0 = controls;   lift0.lift = 0.f;
        auto lift45 = controls;  lift45.lift = 45.f;
        auto eFrame = Upload(context, native, eIds); CHECK(eFrame);
        auto inLow = makeInput(eGeo, eIds, eFrame, eTargets);
        auto inHigh = makeInput(eGeo, eIds, eFrame, eTargets);
        auto lo = pipeline->Begin(native, inLow, eCurves, eGeo.pointCount, lift0, &status);
        CHECK(lo && status == VK_SUCCESS && Prove(native));
        CurveGrowPipeline::Candidate::Semantic semLo;
        CHECK(lo->Poll(&semLo) == VK_SUCCESS && semLo == CurveGrowPipeline::Candidate::Semantic::Ok);
        auto hi = pipeline->Begin(native, inHigh, eCurves, eGeo.pointCount, lift45, &status);
        CHECK(hi && status == VK_SUCCESS && Prove(native));
        CurveGrowPipeline::Candidate::Semantic semHi;
        CHECK(hi->Poll(&semHi) == VK_SUCCESS && semHi == CurveGrowPipeline::Candidate::Semantic::Ok);
        std::vector<float> loPts(lo->pointCount()*3), hiPts(hi->pointCount()*3);
        CHECK(Download(native, lo->output(), &loPts) && Download(native, hi->output(), &hiPts));
        CHECK(loPts.size() == hiPts.size());
        float maxDelta = 0.f;
        for (size_t i = 0; i < loPts.size(); ++i)
            maxDelta = std::max(maxDelta, std::fabs(loPts[i] - hiPts[i]));
        CHECK(maxDelta > 0.001f);  // rotated output differs beyond tolerance
        lo.reset(); hi.reset();
    }

    // --------------------------------------------------------- (d) span size
    {
        auto before = pool->Snapshot();
        auto in = CurveGrowPipeline::Input{};
        in.points = Upload(context, native, geo.points);
        in.rest = Upload(context, native, geo.rest);
        in.widths = Upload(context, native, std::vector<float>(geo.pointCount, 0.03f));
        in.curveOffsets = Upload(context, native, geo.curveOffsets);
        in.stableIds = Upload(context, native, geo.stableIds);
        in.rootPrim = Upload(context, native, geo.rootPrim);
        in.rootUV = Upload(context, native, geo.rootUV);
        in.rootT = Upload(context, native, geo.rootT);
        in.rootB = Upload(context, native, geo.rootB);
        in.rootN = Upload(context, native, geo.rootN);
        in.frameStableIds = frameBuf;
        auto wrongTargets = targets; wrongTargets.pop_back();  // curves-1 entries
        in.targets = Upload(context, native, wrongTargets);
        auto spanBroken = pipeline->Begin(native, in, curves, geo.pointCount, controls, &status);
        CHECK(!spanBroken && status == VK_ERROR_INITIALIZATION_FAILED);
        in = CurveGrowPipeline::Input{};
        CHECK(pool->Snapshot().usedBytes == before.usedBytes && pool->Snapshot().byKind == before.byKind);
    }

    // ------------------------------------------------------ quarantine path

    if (quarantine) {
        auto in = CurveGrowPipeline::Input{};
        in.points = Upload(context, native, geo.points);
        in.rest = Upload(context, native, geo.rest);
        in.curveOffsets = Upload(context, native, geo.curveOffsets);
        in.stableIds = Upload(context, native, geo.stableIds);
        in.rootPrim = Upload(context, native, geo.rootPrim);
        in.rootUV = Upload(context, native, geo.rootUV);
        in.rootT = Upload(context, native, geo.rootT);
        in.rootB = Upload(context, native, geo.rootB);
        in.rootN = Upload(context, native, geo.rootN);
        in.targets = Upload(context, native, targets);
        auto cand = pipeline->Begin(native, in, curves, geo.pointCount, controls, &status);
        CHECK(cand && status == VK_SUCCESS);
        auto charged = pool->Snapshot();
        CHECK(charged.usedBytes > baseline.usedBytes);
        std::weak_ptr<const ChargedBuffer> weakIn = in.points;
        CHECK(!cand->output()); // hidden pre-proof; quarantine must never publish
        cand->Quarantine(); cand.reset(); pipeline.reset(); context.reset();
        CHECK(Prove(native)); native.reset();
        CHECK(!weakNative.expired());
        CHECK(!weakIn.expired()); // quarantine pins borrowed inputs forever
        CHECK(pool->Snapshot().usedBytes == charged.usedBytes && pool->Snapshot().byKind == charged.byKind);
        return 0;
    }

    frameBuf.reset();  // last function-scope buffer pinning the context
    CHECK(pool->Snapshot().usedBytes == baseline.usedBytes &&
          pool->Snapshot().byKind == baseline.byKind);
    pool.reset();  // the pool handle itself pins the context
    pipeline.reset(); context.reset(); native.reset();
    CHECK(weakNative.expired());
    return 0;
}