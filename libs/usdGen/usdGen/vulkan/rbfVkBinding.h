// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

// rbfVkBinding.h — Vulkan port of the CudaRbfBinding device path
// (libs/usdGen/usdGen/gpu/rbf.cu).
//
// The admitted plan range rbfSamples [4,100] is host-solved
// (vulkan/deformRbfHost.h); this binding solves on the device so the full
// gap range executes: budgets above 100 (up to kRbfVkMaxSamples),
// below-CUDA-minimum counts (which fail at Bind with the exact CUDA
// InvalidArgument result instead of a plan rejection), and baked
// non-identity sample sets (rbfVkPlan.h poses them in source-local
// space; the solver itself is transform-agnostic, like cuSOLVER).
//
// Phase mapping to rbf.cu: extentKernel -> rbfVkExtent.comp (+ host
// center/scale, as in Bind), polynomialGram -> rbfVkGram.comp (+ host
// fullAffineRank, as in Bind), buildMatrix -> rbfVkBuildMatrix.comp,
// cusolverDnDgetrf -> rbfVkLu.comp, rhsKernel/zeroTail -> rbfVkRhs.comp,
// cusolverDnDgetrs -> rbfVkTriSolve.comp, evalKernel -> rbfVkEvaluate.comp.
// Scalar diagnostics (extent, gram, info, flags) are read back after each
// fenced submit, mirroring the CUDA stream-synchronized queries.
//
// Status codes and diagnostic strings mirror RbfStatus in gpu/rbf.h
// verbatim (DeviceError is the CudaError analog); the parity test asserts
// equal strings on the shared error paths. Production uses BeginBind,
// AdvanceBind, BeginSolve and nonblocking Poll methods. Bind/Solve are synchronous
// (fence-waited, like the CUDA stream-synchronized originals); Evaluate
// is asynchronous and Finish() completes it. Owner-confined: no internal
// locking, like CudaRbfBinding.
#pragma once

#include "usdGen/vulkan/chargedBuffer.h"
#include "usdGen/vulkan/deviceContext.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace usdGen::vulkan {

// Shared CUDA/Vulkan signed matrix-index safety bound: (n+4)^2 <= INT_MAX.
// Actual Vulkan admission additionally checks the selected device storage
// range and charged memory budget. Create allocates no sample storage;
// Bind sizes its buffers to the requested count after proving prior work.
inline constexpr int kRbfVkMaxSamples = 46336;

enum class RbfVkStatus {
    Ok,
    InvalidArgument,
    NonFiniteInput,
    RankDeficient,
    DeviceError, // VkError analog of RbfStatus::CudaError
    SolverError,
};

char const* RbfVkStatusName(RbfVkStatus status) noexcept;

struct RbfVkBindingSpirv {
    std::vector<uint32_t> extent;
    std::vector<uint32_t> gram;
    std::vector<uint32_t> buildMatrix;
    std::vector<uint32_t> lu;
    std::vector<uint32_t> rhs;
    std::vector<uint32_t> triSolve;
    std::vector<uint32_t> evaluate;
};

class RbfVkBinding final : public std::enable_shared_from_this<RbfVkBinding> {
public:
    static std::shared_ptr<RbfVkBinding> Create(
        std::shared_ptr<DeviceContext> context,
        RbfVkBindingSpirv const& spirv,
        VkResult* result = nullptr);

    ~RbfVkBinding();

    RbfVkBinding(RbfVkBinding const&) = delete;
    RbfVkBinding& operator=(RbfVkBinding const&) = delete;

    std::shared_ptr<DeviceContext> const& context() const noexcept;

    using BeforeSubmit = std::function<bool()>;
    // Owner-driven production path: one submit per Begin/Advance and Poll
    // consumes only an exact fence proof. No Vulkan wait occurs here.
    RbfVkStatus BeginBind(float const*, int, double, BeforeSubmit = {});
    RbfVkStatus AdvanceBind(BeforeSubmit = {});
    RbfVkStatus BeginSolve(float const*, int, BeforeSubmit = {});
    VkResult PollSolve(RbfVkStatus* = nullptr);
    VkResult PollEvaluate(RbfVkStatus* = nullptr);
    bool HasPendingSolve() const noexcept;
    VkResult lastResult() const noexcept;

    // Uploads rest samples, runs extent/gram/matrix/LU on the device, and
    // establishes the identity (zero-coefficient) solved state. Mirrors
    // CudaRbfBinding::Bind, including the n<4 InvalidArgument result.
    RbfVkStatus Bind(float const* restSamples, int sampleCount, double smoothing);

    // Uploads posed samples, builds the RHS and triangular-solves on the
    // device. `posedCount` must equal the bound sample count, mirroring
    // CudaRbfBinding::Solve's view-size check.
    RbfVkStatus Solve(float const* posedSamples, int posedCount);

    // Asynchronous device evaluation into `out` (both 3*count floats).
    // Stacking is allowed like CUDA (one shared completion diagnostic);
    // Finish() completes all pending evaluations.
    RbfVkStatus Evaluate(std::shared_ptr<const ChargedBuffer> cvs,
                         std::shared_ptr<ChargedBuffer> out,
                         uint32_t count, BeforeSubmit beforeSubmit = {});
    RbfVkStatus Finish();

    // Synchronous host convenience: upload, Evaluate, Finish, read back.
    RbfVkStatus EvaluateHost(float const* cvs, float* out, uint32_t count);

    bool HasPendingEvaluate() const noexcept;
    int sampleCount() const noexcept;
    int order() const noexcept; // sampleCount + 4, or 0 when unbound
    double center(int axis) const noexcept;
    double scale() const noexcept;
    char const* diagnostic() const noexcept;

private:
    struct Native;
    explicit RbfVkBinding(std::shared_ptr<Native> native);
    RbfVkStatus fail(RbfVkStatus status, char const* what);
    std::shared_ptr<Native> native_;
    int sampleCount_ = 0, order_ = 0;
    bool solved_ = false;
    bool evalPending_ = false;
    double center_[3] = {};
    double scale_ = 1.0;
    std::string diagnostic_;
};

} // namespace usdGen::vulkan
