#ifndef USDGEN_GPU_EXPRESSION_CONTEXT_H
#define USDGEN_GPU_EXPRESSION_CONTEXT_H

#include "curveGeometry.h"
#include "expression.h"

namespace usdGen::gpu {

struct ExpressionGeometryChannels {
    DeviceView<const float> hairT;      // one value per CV, optional
    DeviceView<const float2> rootUV;    // one value per primitive, optional
};

enum class ExpressionContextStatus { Ok, InvalidArgument, InvalidGeometry,
                                     InvalidChannel, CudaError };

// Builds one domain-specialized expression invocation context without a
// geometry readback.  The object owns converted device fields until the next
// Build/destruction and must outlive expression evaluation using Inputs().
class CudaExpressionContext {
public:
    ~CudaExpressionContext();
    CudaExpressionContext() = default;
    CudaExpressionContext(CudaExpressionContext const&) = delete;
    CudaExpressionContext& operator=(CudaExpressionContext const&) = delete;

    ExpressionContextStatus Build(DeviceCurveGeometryView geometry,
                                   ExpressionGeometryChannels channels,
                                   expr::Context context, cudaStream_t stream);
    ExpressionInputs const& Inputs() const;
    ExpressionContextStatus Wait(cudaStream_t stream) const;
    ExpressionContextStatus Finish(cudaStream_t stream);

private:
    DeviceBuffer<double> fields_[static_cast<unsigned>(expr::Variable::CountVariables)];
    DeviceBuffer<uint32_t> owners_;
    DeviceBuffer<double> arcLength_;
    DeviceBuffer<int> error_;
    cudaEvent_t ready_ = nullptr;
    ExpressionInputs inputs_{};
    bool pending_ = false;
    bool usable_ = false;
};

} // namespace usdGen::gpu
#endif
