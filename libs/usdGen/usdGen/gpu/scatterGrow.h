#ifndef USDGEN_GPU_SCATTER_GROW_H
#define USDGEN_GPU_SCATTER_GROW_H

#include "curveGeometry.h"
#include "imageSampler.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace usdGen::gpu {

// Immutable, captured Scatter roots.  The producer retains the shared owner
// until the producer stream's terminal callback has been proved and committed.
struct ScatterGrowRoots {
    std::vector<float3> positions;
    std::vector<uint64_t> stableIds;
    std::vector<int32_t> rootPrim;
    std::vector<float2> rootUV;
    std::vector<float3> rootT, rootB, rootN;
};

enum class ScatterGrowDirection : uint8_t { RootNormal, RootTangent, Literal };

// This deliberately accepts literals only.  Maps, masks, animated controls,
// and arbitrary direction primvars have no representation here and therefore
// cannot be silently approximated by this strict Scatter -> Grow slice.
struct ScatterGrowControls {
    uint32_t cvCount = 2;
    int seed = 0;
    // CPU Grow evaluates the authored length/random expression in double and
    // narrows the per-curve target to float only after the deterministic draw.
    // Keep these controls double through the CUDA launch for the same rule.
    double length = 1.0;
    double randomLo = 1.0, randomHi = 1.0;
    float lift = 0.0f;
    float fallbackWidth = 0.01f;
    ScatterGrowDirection direction = ScatterGrowDirection::RootNormal;
    float3 literalDirection = {0.0f, 1.0f, 0.0f};
    // Root-normal rotation after lift; random=1 spans a full circle.
    // Appended to preserve aggregate initialization order.
    float azimuth = 0.0f;
    float azimuthRandom = 0.0f;
};

enum class ScatterGrowStatus {
    Ok, InvalidArgument, NonFiniteInput, InvalidTopology, DuplicateStableId,
    NoPendingUpdate, CudaError
};

struct ScatterGrowRequirements {
    size_t pointCount = 0;
    size_t inputBytes = 0;
    size_t outputBytes = 0;
    // One device status and one pinned host status are part of the producer's
    // transaction and remain retained until its terminal proof.
    size_t statusBytes = 0;
    // Optional length-map texels plus one sampled scalar per captured root.
    size_t peakBytes = 0;
};

// Exact explicit payload sizes, available before selecting a device.  The
// statusBytes field includes one device word and one pinned host relay word;
// native events and other driver allocations are outside this ledger.
// inputBytes is the transient input staging (root positions); ids, prims,
// uvs, and frames upload directly into their published output buffers and
// are counted in outputBytes.
ScatterGrowStatus GetScatterGrowRequirements(size_t curveCount, uint32_t cvCount,
                                            ScatterGrowRequirements* result);

// A fresh, immutable Grow topology owner.  BeginFresh never mutates a
// published generation.  A caller must commit only from the relay which saw
// FinishFreshAsync report cudaSuccess; if that proof is lost it must call
// MarkUnprovenWork, causing destruction to quarantine device allocations.
class CudaScatterGrow {
public:
    CudaScatterGrow() = default;
    ~CudaScatterGrow();
    CudaScatterGrow(CudaScatterGrow const&) = delete;
    CudaScatterGrow& operator=(CudaScatterGrow const&) = delete;

    ScatterGrowStatus BeginFresh(std::shared_ptr<const ScatterGrowRoots> roots,
        ScatterGrowControls controls, cudaStream_t stream,
        UsdGenExecutionMemoryReservation* reservation = nullptr);
    ScatterGrowStatus FinishFreshAsync(cudaStream_t stream,
        void (*callback)(cudaStream_t, cudaError_t, void*) noexcept, void* userdata);
    ScatterGrowStatus CommitFreshFinish();
    void MarkUnprovenWork() noexcept { unprovenWork_ = true; }
    bool HasUnprovenWork() const noexcept { return unprovenWork_; }
    bool pending() const noexcept { return pendingWork_; }
    uint64_t generation() const noexcept { return generation_; }
    int deviceIndex() const noexcept { return deviceIndex_; }
    size_t curveCount() const noexcept { return curves_; }
    size_t pointCount() const noexcept { return points_; }
    DeviceCurveGeometryView view() const;
    DeviceView<const float> hairT() const;
    DeviceView<const int32_t> rootPrim() const;
    DeviceView<const float2> rootUV() const;
    DeviceView<const float3> rootT() const;
    DeviceView<const float3> rootB() const;
    DeviceView<const float3> rootN() const;
    ScatterGrowStatus recordUse(cudaStream_t stream);
    ScatterGrowStatus waitOn(cudaStream_t stream) const;
    // Host-only transition after Session ownership has published this fresh
    // generation.  Staging roots have already been released by commit.
    void ReclassifyPublishedGeneration() noexcept;
    size_t ExclusiveRetainedBytes() const noexcept;
    // CPU-only preflight of grow roots: the same checks BeginFresh runs
    // before touching the device (controls, topology, finiteness,
    // duplicate stable ids, per-curve overflow), without needing a
    // stream or a device.  Writes the point total through `total`
    // (which must be non-null) whenever it reports Ok.
    static ScatterGrowStatus ValidateRoots(
        std::shared_ptr<const ScatterGrowRoots> const& roots,
        ScatterGrowControls const& controls, size_t* total);

private:
    // No restPoints buffer: rest == points elementwise for grown roots
    // (the kernel writes every point; the old D2D rest copy is gone), and
    // a published generation is immutable (BeginFresh never mutates one;
    // downstream styled points override the view with separately-owned
    // buffers), so view() publishes the points buffer under both view
    // slots. Identical bytes, one less allocation and 96MB less traffic
    // per 1M-root/8cv grow.
    struct Storage {
        DeviceBuffer<float3> points, rootT, rootB, rootN;
        DeviceBuffer<float> widths, hairT;
        DeviceBuffer<uint32_t> offsets;
        DeviceBuffer<uint64_t> stableIds;
        DeviceBuffer<int32_t> rootPrim;
        DeviceBuffer<float2> rootUV;
        void quarantine() noexcept;
        size_t bytes() const noexcept;
        ScatterGrowStatus recordUse(cudaStream_t);
        ScatterGrowStatus waitOn(cudaStream_t) const;
        ScatterGrowStatus synchronizeUse() const;
        void Reclassify(UsdGenExecutionResourceKind) noexcept;
    } active_, pending_;
    // Device copy of the immutable captured roots.  It is retained until the
    // terminal callback is committed, rather than borrowing caller memory.
    Storage pendingInput_;

    ScatterGrowStatus validate(std::shared_ptr<const ScatterGrowRoots> const&,
                               ScatterGrowControls const&, size_t* total) const;
    ScatterGrowStatus validateStream(cudaStream_t) const;
    void discardPending() noexcept;
    std::shared_ptr<const ScatterGrowRoots> rootsOwner_;
    // Preallocated before submission so unproved H2D retirement can retain
    // its immutable host source without allocating in a noexcept destructor.
    std::unique_ptr<std::shared_ptr<const ScatterGrowRoots>> rootsQuarantineOwner_;
    DeviceBuffer<int> error_;
    int* hostError_ = nullptr;
    UsdGenExecutionResourcePermit hostErrorPermit_;
    cudaEvent_t ready_ = nullptr;
    // A completion callback proves this producer's uploads/kernel only when
    // it is ordered on the exact stream that submitted them.
    cudaStream_t producerStream_ = nullptr;
    size_t curves_ = 0, points_ = 0, pendingCurves_ = 0, pendingPoints_ = 0;
    uint64_t generation_ = 0;
    int deviceIndex_ = -1;
    bool pendingWork_ = false, finishScheduled_ = false, unprovenWork_ = false;
};

} // namespace usdGen::gpu
#endif
