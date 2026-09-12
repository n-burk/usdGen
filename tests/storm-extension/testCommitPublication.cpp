// Actual HdSt commit boundaries, not a mock callback executor. A private
// patched HdSt is required; the unmodified shared SDK remains untouched.
#include "eglctx.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/rprimSharedData.h"
#include "pxr/imaging/hdSt/bufferArrayRange.h"
#include "pxr/imaging/hdSt/bufferResource.h"
#include "pxr/imaging/hdSt/computation.h"
#include "pxr/imaging/hdSt/drawItem.h"
#include "pxr/imaging/hdSt/drawItemInstance.h"
#include "pxr/imaging/hdSt/dispatchBuffer.h"
#include "pxr/imaging/hdSt/indirectDrawCount.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hdSt/primUtils.h"
#include "pxr/imaging/hdSt/renderParam.h"
#include "pxr/imaging/hgiGL/hgi.h"
#include "pxr/imaging/hgiGL/buffer.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "pxr/base/gf/range3d.h"

PXR_NAMESPACE_USING_DIRECTIVE
namespace {
int failures = 0;
void Check(bool okay, char const* what) {
    if (!okay) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}

// An intentionally fallible producer. Status is owned by this computation,
// not inferred from the mere existence of a post-commit callback.
class Producer final : public HdStComputation {
public:
    explicit Producer(bool admit) : _admit(admit) {}
    int GetNumOutputElements() const override { return 3; }
    void GetBufferSpecs(HdBufferSpecVector* specs) const override {
        specs->emplace_back(TfToken("points"), HdTupleType{HdTypeFloatVec3, 1});
    }
    void Execute(HdBufferArrayRangeSharedPtr const& range, HdResourceRegistry*) override {
        executed = true;
        const auto bar = std::dynamic_pointer_cast<HdStBufferArrayRange>(range);
        const auto resource = bar ? bar->GetResource(TfToken("points")) : nullptr;
        success = _admit && resource && resource->GetHandle() && range->GetNumElements() == 3;
    }
    bool executed = false;
    bool success = false;
private:
    bool _admit;
};

void Publication(HdStResourceRegistry& registry) {
    HdRprimSharedData shared(1);
    shared.rprimID = SdfPath("/pendingGroom");
    HdStRenderParam renderParam;
    HdChangeTracker tracker;
    const auto& visible = shared.barContainer.Get(0);
    auto const owner = std::this_thread::get_id();
    auto queue = [&](bool admit) {
        auto producer = std::make_shared<Producer>(admit);
        HdBufferSpecVector specs;
        producer->GetBufferSpecs(&specs);
        auto range = registry.AllocateNonUniformBufferArrayRange(
            TfToken("stagedPublication"), specs, HdBufferArrayUsageHintBitsVertex);
        registry.AddComputation(range, producer, HdStComputeQueueZero);
        registry.AddPostCommitCallback([&, producer, range, owner] {
            Check(std::this_thread::get_id() == owner, "publication on commit caller");
            Check(producer->executed, "producer executed before publication");
            if (producer->success) {
                HdStUpdateDrawItemBAR(range, 0, &shared, &renderParam, &tracker);
            }
        });
        return range;
    };
    auto first = queue(true);
    const auto initialVersion = renderParam.GetDrawBatchesVersion();
    Check(!visible, "pending initial range remains unpublished");
    registry.Commit();
    Check(visible == first, "successful initial range published");
    Check(renderParam.GetDrawBatchesVersion() != initialVersion,
          "post-commit attachment invalidates real Storm draw batches");
    const auto firstVersion = renderParam.GetDrawBatchesVersion();
    auto rejected = queue(false);
    Check(visible == first, "pending failed replacement retains old range");
    registry.Commit();
    Check(visible == first && visible != rejected, "failed replacement never published");
    Check(renderParam.GetDrawBatchesVersion() == firstVersion,
          "failed replacement does not invalidate visible draw batches");
    auto replacement = queue(true);
    Check(visible == first, "successful replacement not visible before commit");
    registry.Commit();
    Check(visible == replacement, "successful replacement published after commit");
    Check(first->IsAggregatedWith(replacement) &&
          first->GetElementOffset() != replacement->GetElementOffset(),
          "replacement exercises distinct offsets in one aggregate");
    Check(renderParam.GetDrawBatchesVersion() != firstVersion,
          "new aggregate offset invalidates dispatch commands at commit");
}

void CallbackPhases(HdStResourceRegistry& registry) {
    int outer = 0, inner = 0, afterThrow = 0;
    registry.AddPostCommitCallback([&] {
        ++outer;
        registry.AddPostCommitCallback([&] { ++inner; });
    });
    registry.AddPostCommitCallback([] { throw std::runtime_error("expected callback failure"); });
    registry.AddPostCommitCallback([&] { ++afterThrow; });
    registry.Commit();
    Check(outer == 1 && inner == 0 && afterThrow == 1,
          "callback failure isolated and nested callback deferred");
    registry.Commit();
    Check(outer == 1 && inner == 1 && afterThrow == 1, "callbacks execute exactly once");
    registry.AddPostCommitCallback([&] {
        registry.AddPostCommitCallback([&] { ++inner; });
        TfErrorMark recursiveError;
        registry.Commit(); // Expected rejection, not nested consumption.
        Check(!recursiveError.IsClean(), "recursive commit reports a diagnostic");
        recursiveError.Clear();
        Check(inner == 1, "recursive commit cannot publish the next batch");
    });
    registry.Commit();
    Check(inner == 1, "recursive commit rejected");
    registry.Commit();
    Check(inner == 2, "outer commit remains usable after recursive rejection");
}

void ConcurrentRegistration(HdStResourceRegistry& registry) {
    constexpr int threads = 8, each = 128;
    std::vector<int> visited(threads * each);
    std::atomic<int> wrongThread{0};
    const auto owner = std::this_thread::get_id();
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) workers.emplace_back([&, t] {
        for (int i = 0; i < each; ++i) registry.AddPostCommitCallback([&, t, i] {
            if (std::this_thread::get_id() != owner) ++wrongThread;
            ++visited[t * each + i];
        });
    });
    for (auto& worker : workers) worker.join(); // Same Sync -> Commit phase contract.
    registry.Commit();
    registry.Commit();
    for (int count : visited) Check(count == 1, "parallel registration delivered exactly once");
    Check(wrongThread == 0, "parallel registrations execute serially on graphics owner");
}

void VisibilityPolicy() {
    HdRprimSharedData shared(1);
    HdStDrawItem item(&shared);
    shared.bounds.SetRange(GfRange3d(GfVec3d(-0.25), GfVec3d(0.25)));
    Check(item.IntersectsViewVolume(GfMatrix4d(1)),
          "ordinary CPU draw item remains visible in frustum");
    shared.bounds.SetRange(GfRange3d(GfVec3d(10), GfVec3d(11)));
    Check(!item.IntersectsViewVolume(GfMatrix4d(1)),
          "ordinary CPU draw item remains culled outside frustum");
}

void IndirectDrawCountCopy(HdStResourceRegistry& registry, HgiGL& hgi) {
    HdBufferSpecVector specs;
    specs.emplace_back(TfToken("drawCount"), HdTupleType{HdTypeUInt32, 1});
    auto countRange = registry.AllocateNonUniformBufferArrayRange(
        HdTokens->primvar, specs, HdBufferArrayUsageHintBitsStorage);
    registry.AddSource(countRange, std::make_shared<HdVtBufferSource>(
        TfToken("drawCount"), VtValue(uint32_t(3))));
    registry.Commit();

    auto dispatch = std::make_shared<HdStDispatchBuffer>(
        &registry, HdTokens->drawDispatch, 1, 5);
    dispatch->AddBufferResourceView(HdTokens->drawDispatch,
        HdTupleType{HdTypeUInt32, 1}, 0);
    dispatch->CopyData({99, 7, 8, 9, 10});

    HdRprimSharedData shared(1);
    HdStDrawItem item(&shared);
    item.SetIndirectDrawCountRange(countRange);
    HdStDrawItemInstance instance(&item);
    std::vector<HdStDrawItemInstance const *> items{&instance};
    Check(HdStApplyIndirectDrawCounts(items, dispatch, &registry),
          "valid GPU draw-count range accepted");
    registry.SubmitBlitWork(HgiSubmitWaitTypeWaitUntilCompleted);

    auto *buffer = dynamic_cast<HgiGLBuffer *>(
        dispatch->GetEntireResource()->GetHandle().Get());
    uint32_t words[5] = {};
    if (buffer) {
        glBindBuffer(GL_ARRAY_BUFFER, buffer->GetRawResource());
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(words), words);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    Check(buffer && words[0] == 3 && words[1] == 7 && words[2] == 8 &&
              words[3] == 9 && words[4] == 10,
          "GPU draw-count copy replaces only the count word");

    shared.bounds.SetRange(GfRange3d(GfVec3d(10), GfVec3d(11)));
    Check(item.IntersectsViewVolume(GfMatrix4d(1)),
          "GPU-count item remains conservatively visible outside CPU bounds");
    item.SetIndirectDrawCountRange({});
    Check(!item.IntersectsViewVolume(GfMatrix4d(1)),
          "removing GPU count restores ordinary CPU bounds culling");

    HdBufferSpecVector badSpecs;
    badSpecs.emplace_back(TfToken("drawCount"), HdTupleType{HdTypeUInt32, 1});
    auto badRange = registry.AllocateNonUniformBufferArrayRange(
        HdTokens->primvar, badSpecs, HdBufferArrayUsageHintBitsStorage);
    registry.AddSource(badRange, std::make_shared<HdVtBufferSource>(
        TfToken("drawCount"), VtValue(VtUIntArray{1, 2})));
    registry.Commit();
    item.SetIndirectDrawCountRange(badRange);
    Check(!HdStApplyIndirectDrawCounts(items, dispatch, &registry),
          "non-singleton GPU draw-count range rejected");
}
} // namespace

int main() {
    TfErrorMark errors;
    if (!eglctx::MakeHeadlessGLContext()) return 77;
    GarchGLApiLoad();
    HgiGL hgi;
    {
        HdStResourceRegistry registry(&hgi);
        Publication(registry);
        CallbackPhases(registry);
        ConcurrentRegistration(registry);
        VisibilityPolicy();
        IndirectDrawCountCopy(registry, hgi);
    }
    std::weak_ptr<int> retained;
    bool invoked = false;
    {
        HdStResourceRegistry registry(&hgi);
        auto value = std::make_shared<int>(1);
        retained = value;
        registry.AddPostCommitCallback([&, value] { invoked = true; });
        value.reset();
        Check(!retained.expired(), "pending callback retains its captured generation");
    }
    Check(retained.expired() && !invoked, "registry destruction cancels pending callbacks");
    Check(errors.IsClean(), "no unexpected Tf errors");
    std::printf("Storm commit publication: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
