#ifndef USDGEN_CUDA_PARAMETERS_H
#define USDGEN_CUDA_PARAMETERS_H
#ifdef USDGEN_ENABLE_CUDA

#include "gpu/expressionContext.h"
#include "gpu/expression.h"
#include "usdGen/graphDesc.h"

#include <memory>
#include <string>
#include <vector>

namespace usdGen {

struct CudaParameterField {
    TfToken destination;
    const void* data = nullptr;
    size_t count = 0;
    expr::ScalarType type = expr::ScalarType::Invalid;
    uint32_t components = 1;
    expr::Domain domain = expr::Domain::Groom;
};

enum class CudaParameterStatus {
    Ok, InvalidArgument, CompileError, UnsupportedType, CudaError, InvalidValue
};

// Compile builds immutable CPU programs and literal staging, never device
// state. Evaluate executes against the incoming geometry, finishes validation,
// and publishes borrowed device fields only when all bindings succeed.
// Fields remain valid until the next Compile/Evaluate or destruction. Consume
// them before reusing this plan. The owner serializes calls and keeps the CUDA
// device/context current for execution and destruction.
class CudaParameterPlan {
public:
    static CudaParameterStatus Compile(
        UsdGenGraphDesc const&, UsdGenNodeDesc const&, CudaParameterPlan*,
        std::vector<std::string>* diagnostics = nullptr);
    CudaParameterStatus Evaluate(
        gpu::DeviceCurveGeometryView, gpu::ExpressionGeometryChannels,
        expr::Context, cudaStream_t,
        std::vector<std::string>* diagnostics = nullptr);

    std::vector<UsdGenExpressionBinding> const& Bindings() const { return bindings_; }
    std::vector<CudaParameterField> const& Fields() const { return fields_; }
    CudaParameterField const* Find(TfToken const& destination) const;

private:
    struct Item {
        UsdGenExpressionBinding binding;
        expr::IRProgram ir;
        std::vector<double> literal;
    };
    std::vector<UsdGenExpressionBinding> bindings_;
    std::vector<Item> items_;
    std::vector<gpu::DeviceBuffer<double>> literals_;
    std::vector<gpu::DeviceBuffer<unsigned char>> outputs_;
    std::vector<std::unique_ptr<gpu::CudaExpressionProgram>> runtime_;
    std::vector<CudaParameterField> fields_;
    std::unique_ptr<gpu::CudaExpressionContext> contexts_[3];
};
} // namespace usdGen
#endif // USDGEN_ENABLE_CUDA
#endif // USDGEN_CUDA_PARAMETERS_H
