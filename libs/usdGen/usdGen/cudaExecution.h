#ifndef USDGEN_CUDA_EXECUTION_H
#define USDGEN_CUDA_EXECUTION_H
#include "usdGen/deviceGeneration.h"
#include "usdGen/graphDesc.h"
#include "usdGen/op.h"

namespace usdGen {
// CUDA backend admission is explicit and shared by compilation/execution.
// Unsupported operators/configurations are errors, never CPU fallbacks.
bool ValidateCudaGraph(UsdGenGraphDesc const&, UsdGenDiagnostics*);
std::shared_ptr<const UsdGenDeviceGeneration> ExecuteCudaGraph(
    UsdGenGraphDesc const&, uint64_t generation, UsdGenDiagnostics*);
}
#endif
