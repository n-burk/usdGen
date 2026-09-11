#ifndef USDGEN_GPU_EXPRESSION_H
#define USDGEN_GPU_EXPRESSION_H

#include "deviceBuffer.h"
#include "usdGen/expressions/ir.h"

namespace usdGen::gpu {

// Device fields are indexed in their declared spatial domain. Constants
// broadcast; primitive fields at point rate use the supplied ownership map.
struct ExpressionField {
    const double* data = nullptr;
    size_t count = 0;
    expr::Domain domain = expr::Domain::Groom;
    // Interleaved components; count is the number of spatial samples.
    unsigned components = 1;
};
struct ExpressionInputs {
    expr::Context context;
    size_t count = 1;
    size_t primitiveCount = 0;
    DeviceView<const uint32_t> pointToPrimitive;
    ExpressionField fields[static_cast<unsigned>(expr::Variable::CountVariables)]{};
};
struct ExpressionOutput {
    void* data = nullptr;
    size_t count = 0;
    expr::ScalarType type = expr::ScalarType::Invalid;
    unsigned components = 1;
};
enum class ExpressionStatus { Ok, InvalidArgument, InvalidProgram, InvalidValue, CudaError };

// Upload is compile-time control data. Evaluate reads/writes only device
// fields; Finish validates one scalar diagnostic before publication. An
// instance allows one pending execution, serialized across caller streams.
class CudaExpressionProgram {
public:
    ~CudaExpressionProgram();
    CudaExpressionProgram() = default;
    CudaExpressionProgram(CudaExpressionProgram const&) = delete;
    CudaExpressionProgram& operator=(CudaExpressionProgram const&) = delete;
    ExpressionStatus Upload(expr::IRProgram const&, cudaStream_t);
    ExpressionStatus Evaluate(ExpressionInputs const&, ExpressionOutput, cudaStream_t);
    ExpressionStatus Finish(cudaStream_t);
private:
    DeviceBuffer<unsigned char> code_;
    DeviceBuffer<int> error_;
    cudaEvent_t ready_ = nullptr;
    size_t instructionCount_ = 0;
    uint16_t result_ = 0;
    uint16_t output_[4]{};
    unsigned outputCount_ = 1;
    bool pending_ = false;
};
} // namespace usdGen::gpu
#endif
