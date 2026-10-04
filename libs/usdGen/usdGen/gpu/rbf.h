#ifndef USDGEN_GPU_RBF_H
#define USDGEN_GPU_RBF_H

#include "deviceBuffer.h"
#include <cusolverDn.h>
#include <cstdint>
#include <memory>
#include <string>

namespace usdGen { namespace gpu {
enum class RbfStatus { Ok, InvalidArgument, NonFiniteInput, RankDeficient, CudaError, SolverError };
// Test-only fresh-path fault seams.  Each affects one matching Begin call.
void TestFailNextFreshRbfBindAllocation() noexcept;
void TestFailNextFreshRbfBindAfterSubmit() noexcept;
void TestFailNextFreshRbfSolveAllocation() noexcept;
void TestFailNextFreshRbfSolveAfterInputSubmit() noexcept;
void TestFailNextFreshRbfSolveAfterSolverSubmit() noexcept;
void TestFailNextFreshRbfEvaluateAllocation() noexcept;
void TestFailNextFreshRbfEvaluateAfterSubmit() noexcept;
void TestFailNextFreshRbfResolvePreflight() noexcept;
void TestFailNextFreshRbfResolveCommit() noexcept;
uint64_t FreshRbfAcceptAttemptCountForTesting() noexcept;
uint64_t FreshRbfRollbackAttemptCountForTesting() noexcept;

// Queries the selected CUDA implementation's legacy dense-LU workspace
// without allocating matrix storage or submitting device work.
cudaError_t GetCudaRbfLuWorkspaceElements(size_t sampleCount,
                                          size_t* elements) noexcept;

// GPU-only cubic RBF binding.  Bind and Solve copy only scalar diagnostics to
// the host; Evaluate has no geometry readback and is asynchronous.
class CudaRbfBinding {
public:
    CudaRbfBinding();
    ~CudaRbfBinding();
    CudaRbfBinding(const CudaRbfBinding&) = delete;
    CudaRbfBinding& operator=(const CudaRbfBinding&) = delete;
    RbfStatus Bind(DeviceView<const float3> restSamples, double smoothing, cudaStream_t stream);
    RbfStatus Solve(DeviceView<const float3> currentSamples, cudaStream_t stream);
    RbfStatus Evaluate(DeviceView<const float3> restCvs, DeviceView<float3> output, cudaStream_t stream);
    // Completes all Evaluate calls issued since the prior Finish.  Call before
    // publishing their output generation; only a scalar device flag is read.
    RbfStatus Finish(cudaStream_t stream);
    // Fresh rest binding is a private candidate.  Each Commit must be called
    // only after the owner has proved native terminal success and launcher
    // return. Commits are host-only; these low-level fresh primitives do not
    // alter the synchronous compatibility fields above.
    RbfStatus BeginFreshBind(DeviceView<const float3> restSamples, double smoothing, cudaStream_t stream,
                             UsdGenExecutionMemoryReservation* reservation = nullptr);
    RbfStatus CommitFreshBindExtent();
    RbfStatus BeginFreshBindRank(cudaStream_t stream);
    RbfStatus CommitFreshBindRank();
    RbfStatus BeginFreshBindLu(cudaStream_t stream);
    RbfStatus CommitFreshBindLu();
    // A fresh pose never writes the accepted coefficient buffer.  Its input
    // proof must succeed before the triangular solve can be submitted, and
    // the solver proof must succeed before CommitFreshSolve exchanges the
    // candidate coefficients into the accepted binding.
    RbfStatus BeginFreshSolve(DeviceView<const float3> currentSamples, cudaStream_t stream,
                              UsdGenExecutionMemoryReservation* reservation = nullptr);
    RbfStatus CommitFreshSolveInput();
    RbfStatus BeginFreshSolveFactors(cudaStream_t stream);
    RbfStatus CommitFreshSolve();
    // CommitFreshSolve makes the candidate coefficients visible solely to the
    // following fresh evaluation.  The parent must resolve that transaction
    // after all downstream work has terminal proof: Accept retains the new
    // pose, Rollback restores the prior accepted coefficients.  Neither call
    // submits CUDA work, waits, or frees a live CUDA allocation.
    RbfStatus AcceptFreshSolve();
    RbfStatus RollbackFreshSolve();
    // Pure transaction preflight.  A parent that owns both the surface pose
    // and solve candidate must check both sides before mutating either.
    bool CanAcceptFreshSolve() const noexcept;
    bool CanRollbackFreshSolve() const noexcept;
    RbfStatus BeginFreshEvaluate(DeviceView<const float3> restCvs, DeviceView<float3> output, cudaStream_t stream,
                                 UsdGenExecutionMemoryReservation* reservation = nullptr);
    RbfStatus CommitFreshEvaluate();
    void AbandonFresh() noexcept;
    bool HasUnprovenWork() const noexcept;
    // Candidate-only count; becomes useful to the following fresh solve
    // slice, without claiming that this bind has been published.
    size_t freshSampleCount() const;
    size_t sampleCount() const { return sampleCount_; }
    const char* diagnostic() const { return diagnostic_.c_str(); }
private:
    struct FreshState;
    RbfStatus fail(RbfStatus status, const char* what);
    size_t sampleCount_ = 0, order_ = 0;
    bool solved_ = false, evalPending_ = false;
    double smoothing_ = 0.0, center_[3] = {}, scale_ = 1.0;
    std::string diagnostic_;
    cusolverDnHandle_t solver_ = nullptr;
    cudaEvent_t stateReady_ = nullptr, evalReady_ = nullptr; // cross-stream state/output ordering
    DeviceBuffer<float3> rest_, current_;
    DeviceBuffer<double> matrix_, work_, coefficients_, normSamples_;
    DeviceBuffer<double> gram_;
    DeviceBuffer<int> pivots_, info_, flags_, evalFlags_;
    std::unique_ptr<FreshState> fresh_, acceptedFresh_, freshSolve_, freshEval_;
    std::unique_ptr<FreshState> retiredFresh_, retiredSolve_, retiredEval_;
    bool freshSolvePendingAcceptance_ = false;
};
}}
#endif
