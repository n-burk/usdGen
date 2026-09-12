#ifndef USDGEN_CUDA_EXECUTION_H
#define USDGEN_CUDA_EXECUTION_H
#include "usdGen/deviceGeneration.h"
#include "usdGen/graphDesc.h"
#include "usdGen/op.h"

namespace usdGen {
// CUDA backend admission is explicit and shared by compilation/execution.
// Unsupported operators/configurations are errors, never CPU fallbacks.
bool ValidateCudaGraph(UsdGenGraphDesc const&, UsdGenDiagnostics*);
class UsdGenCudaExecutionPlan;
// Immutable CPU programs, LUTs and authored input snapshot; no device state.
std::shared_ptr<const UsdGenCudaExecutionPlan> CompileCudaGraph(
    UsdGenGraphDesc const&, UsdGenDiagnostics*);
struct UsdGenCudaBindingStats {
    SdfPath path;
    uint64_t identity = 0, bindCount = 0, solveCount = 0;
    size_t sampleCount = 0;
};
// One description's mutable execution resources. Calls must belong to its
// serial work node; different descriptions never share a workspace. Plans may
// be shared freely. The workspace owns an explicit nonblocking device stream.
class UsdGenCudaExecutionWorkspace {
public:
    ~UsdGenCudaExecutionWorkspace();
    UsdGenCudaExecutionWorkspace(UsdGenCudaExecutionWorkspace const&) = delete;
    UsdGenCudaExecutionWorkspace& operator=(UsdGenCudaExecutionWorkspace const&) = delete;
    int DeviceIndex() const noexcept;
private:
    UsdGenCudaExecutionWorkspace();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend std::unique_ptr<UsdGenCudaExecutionWorkspace> CreateCudaExecutionWorkspace(int, UsdGenDiagnostics*);
    friend std::vector<UsdGenCudaBindingStats> GetCudaBindingStats(UsdGenCudaExecutionWorkspace const&);
    friend std::shared_ptr<const UsdGenDeviceGeneration> ExecuteCudaGraph(
        UsdGenCudaExecutionPlan const&, UsdGenCudaExecutionWorkspace&, double,
        uint64_t, UsdGenDiagnostics*, std::shared_ptr<const UsdGenDeviceGeneration> const&);
};
// device=-1 captures the caller's current CUDA device before dispatch. Every
// execution explicitly selects that device and restores the worker's previous
// device. Destruction follows draining, never overlaps execution.
std::unique_ptr<UsdGenCudaExecutionWorkspace> CreateCudaExecutionWorkspace(
    int device, UsdGenDiagnostics*);
// Immutable last-completed diagnostics; safe to query while the owner works.
std::vector<UsdGenCudaBindingStats> GetCudaBindingStats(UsdGenCudaExecutionWorkspace const&);
std::shared_ptr<const UsdGenDeviceGeneration> ExecuteCudaGraph(
    UsdGenCudaExecutionPlan const&, UsdGenCudaExecutionWorkspace&, double frame,
    uint64_t generation, UsdGenDiagnostics*,
    std::shared_ptr<const UsdGenDeviceGeneration> const& previous = {});
}
#endif
