#ifndef USDGEN_GPU_RBF_H
#define USDGEN_GPU_RBF_H

#include "deviceBuffer.h"
#include <cusolverDn.h>
#include <string>

namespace usdGen { namespace gpu {
enum class RbfStatus { Ok, InvalidArgument, NonFiniteInput, RankDeficient, CudaError, SolverError };

// GPU-only cubic RBF binding.  Bind and Solve copy only scalar diagnostics to
// the host; Evaluate has no geometry readback and is asynchronous.
class CudaRbfBinding {
public:
    CudaRbfBinding() = default;
    ~CudaRbfBinding();
    CudaRbfBinding(const CudaRbfBinding&) = delete;
    CudaRbfBinding& operator=(const CudaRbfBinding&) = delete;
    RbfStatus Bind(DeviceView<const float3> restSamples, double smoothing, cudaStream_t stream);
    RbfStatus Solve(DeviceView<const float3> currentSamples, cudaStream_t stream);
    RbfStatus Evaluate(DeviceView<const float3> restCvs, DeviceView<float3> output, cudaStream_t stream);
    // Completes all Evaluate calls issued since the prior Finish.  Call before
    // publishing their output generation; only a scalar device flag is read.
    RbfStatus Finish(cudaStream_t stream);
    size_t sampleCount() const { return sampleCount_; }
    const char* diagnostic() const { return diagnostic_.c_str(); }
private:
    RbfStatus fail(RbfStatus status, const char* what);
    size_t sampleCount_ = 0, order_ = 0;
    bool solved_ = false, evalPending_ = false;
    double smoothing_ = 0.0, center_[3] = {}, scale_ = 1.0;
    std::string diagnostic_;
    cusolverDnHandle_t solver_ = nullptr;
    cudaEvent_t stateReady_ = nullptr, evalReady_ = nullptr; // cross-stream state/output ordering
    DeviceBuffer<float3> rest_, current_;
    DeviceBuffer<double> matrix_, work_, coefficients_;
    DeviceBuffer<double> gram_;
    DeviceBuffer<int> pivots_, info_, flags_, evalFlags_;
};
}}
#endif
