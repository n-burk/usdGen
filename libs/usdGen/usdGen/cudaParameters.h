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

// CudaParameterProgram is immutable CPU IR and literal data. It owns no CUDA
// allocations and can safely be shared by independent evaluators and frames.
class CudaParameterProgram {
public:
    static CudaParameterStatus Compile(
        UsdGenGraphDesc const&, UsdGenNodeDesc const&,
        std::shared_ptr<const CudaParameterProgram>*,
        std::vector<std::string>* diagnostics = nullptr);

    std::vector<UsdGenExpressionBinding> const& Bindings() const {
        return bindings_;
    }

private:
    struct Item {
        UsdGenExpressionBinding binding;
        expr::IRProgram ir;
        std::vector<double> literal;
    };
    std::vector<UsdGenExpressionBinding> bindings_;
    std::vector<Item> items_;

    friend class CudaParameterEvaluator;
};

// A mutable GPU execution workspace. Every evaluator has independent CUDA
// programs, contexts, and output buffers, while sharing only the immutable
// compiled CudaParameterProgram.
// Fields are borrowed until the next Evaluate or destruction. The work owner
// must finish all consumers before reuse/destruction and select the owning
// CUDA device; a single evaluator is never invoked concurrently.
class CudaParameterEvaluator {
public:
    CudaParameterStatus Evaluate(
        CudaParameterProgram const&, gpu::DeviceCurveGeometryView,
        gpu::ExpressionGeometryChannels, expr::Context, cudaStream_t,
        std::vector<std::string>* diagnostics = nullptr);

    std::vector<CudaParameterField> const& Fields() const { return fields_; }
    CudaParameterField const* Find(TfToken const& destination) const;

private:
    std::vector<gpu::DeviceBuffer<double>> literals_;
    std::vector<gpu::DeviceBuffer<unsigned char>> outputs_;
    std::vector<std::unique_ptr<gpu::CudaExpressionProgram>> runtime_;
    std::vector<CudaParameterField> fields_;
    std::unique_ptr<gpu::CudaExpressionContext> contexts_[3];
};

// Compatibility facade. Compilation delegates to an immutable program and
// evaluation delegates to one mutable evaluator owned by this facade.
class CudaParameterPlan {
public:
    static CudaParameterStatus Compile(
        UsdGenGraphDesc const&, UsdGenNodeDesc const&, CudaParameterPlan*,
        std::vector<std::string>* diagnostics = nullptr);
    CudaParameterStatus Evaluate(
        gpu::DeviceCurveGeometryView, gpu::ExpressionGeometryChannels,
        expr::Context, cudaStream_t,
        std::vector<std::string>* diagnostics = nullptr);

    std::vector<UsdGenExpressionBinding> const& Bindings() const {
        static std::vector<UsdGenExpressionBinding> const empty;
        return program_ ? program_->Bindings() : empty;
    }
    std::vector<CudaParameterField> const& Fields() const {
        static std::vector<CudaParameterField> const empty;
        return evaluator_ ? evaluator_->Fields() : empty;
    }
    CudaParameterField const* Find(TfToken const& destination) const;

private:
    std::shared_ptr<const CudaParameterProgram> program_;
    std::unique_ptr<CudaParameterEvaluator> evaluator_;
};
} // namespace usdGen
#endif // USDGEN_ENABLE_CUDA
#endif // USDGEN_CUDA_PARAMETERS_H
