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
// CPU-only compilation. Runtime device buffers belong to this graph plan.
std::shared_ptr<UsdGenCudaExecutionPlan> CompileCudaGraph(
    UsdGenGraphDesc const&, UsdGenDiagnostics*,
    std::shared_ptr<UsdGenCudaExecutionPlan> const& previous = {});
struct UsdGenCudaBindingStats {
    SdfPath path;
    uint64_t identity = 0, bindCount = 0, solveCount = 0;
    size_t sampleCount = 0;
};
// Diagnostic control data only; no geometry readback or CUDA pointers.
std::vector<UsdGenCudaBindingStats> GetCudaBindingStats(UsdGenCudaExecutionPlan&);
std::shared_ptr<const UsdGenDeviceGeneration> ExecuteCudaGraph(
    UsdGenCudaExecutionPlan&, UsdGenGraphDesc const&, double frame,
    uint64_t generation, UsdGenDiagnostics*);
}
#endif
