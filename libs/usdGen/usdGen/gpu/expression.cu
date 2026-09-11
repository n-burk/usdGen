#include "expression.h"

#include <climits>
#include <cmath>
#include <limits>
#include <cuda_fp16.h>

namespace usdGen::gpu {
namespace {
constexpr unsigned kRegisters = 256;
using expr::IROp;
using expr::Variable;
using expr::Domain;
using expr::ScalarType;

bool IsDomain(Domain d) {
    return d == Domain::Groom || d == Domain::Primitive || d == Domain::Point;
}

__device__ double ReadVariable(Variable variable, unsigned component,
                              ExpressionInputs const& in, size_t index) {
    switch (variable) {
    case Variable::Frame: return in.context.frame;
    case Variable::Time: return in.context.time;
    case Variable::Index: return double(index);
    case Variable::Count: return double(in.count);
    case Variable::Seed: return double(in.context.seed);
    case Variable::DescId: return double(in.context.descId);
    default: break;
    }
    const auto v = static_cast<unsigned>(variable);
    if (v == 0 || v >= static_cast<unsigned>(Variable::CountVariables)) return NAN;
    ExpressionField const& field = in.fields[v];
    if (!field.data || component >= field.components) return NAN;
    size_t element = index;
    if (field.domain == Domain::Groom) element = 0;
    else if (field.domain == Domain::Primitive && in.context.domain == Domain::Point) {
        if (!in.pointToPrimitive.data || index >= in.pointToPrimitive.size) return NAN;
        element = in.pointToPrimitive.data[index];
    } else if (field.domain != in.context.domain) return NAN;
    return element < field.count ? field.data[element * field.components + component] : NAN;
}

__device__ bool Store(double value, ExpressionOutput out, size_t i) {
    if (!isfinite(value)) return false;
    switch (out.type) {
    case ScalarType::Bool:
        if (value != 0.0 && value != 1.0) return false;
        static_cast<unsigned char*>(out.data)[i] = value != 0; return true;
    case ScalarType::Int32:
        if (trunc(value) != value || value < double(INT_MIN) || value > double(INT_MAX)) return false;
        static_cast<int32_t*>(out.data)[i] = static_cast<int32_t>(value); return true;
    case ScalarType::UInt32:
        if (trunc(value) != value || value < 0.0 || value > double(UINT_MAX)) return false;
        static_cast<uint32_t*>(out.data)[i] = static_cast<uint32_t>(value); return true;
    case ScalarType::Int64:
        // SeExpr's double arithmetic is exact only through 2^53. Do not
        // suggest that an arbitrary USD int64 was evaluated losslessly.
        if (trunc(value) != value || fabs(value) > 9007199254740992.0) return false;
        static_cast<int64_t*>(out.data)[i] = static_cast<int64_t>(value); return true;
    case ScalarType::UInt64:
        if (trunc(value) != value || value < 0.0 || value > 9007199254740992.0) return false;
        static_cast<uint64_t*>(out.data)[i] = static_cast<uint64_t>(value); return true;
    case ScalarType::Float16: {
        const __half converted = __double2half(value);
        if (!isfinite(__half2float(converted))) return false;
        static_cast<__half*>(out.data)[i] = converted; return true;
    }
    case ScalarType::Float32: {
        const float converted = static_cast<float>(value);
        if (!isfinite(converted)) return false;
        static_cast<float*>(out.data)[i] = converted; return true;
    }
    case ScalarType::Float64: static_cast<double*>(out.data)[i] = value; return true;
    default: return false;
    }
}

struct OutputRegisters { uint16_t value[4]; };
__global__ void Execute(expr::IRInstruction const* code, size_t count, OutputRegisters result,
                        ExpressionInputs inputs, ExpressionOutput output, int* error) {
    const size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= inputs.count) return;
    double r[kRegisters];
    for (size_t pc = 0; pc < count; ++pc) {
        auto const& instruction = code[pc];
        double value = NAN;
        const unsigned a = instruction.a, b = instruction.b, c = instruction.c;
        switch (instruction.op) {
        case IROp::Const: value = instruction.immediate; break;
        case IROp::LoadVariable: value = ReadVariable(instruction.variable, instruction.component, inputs, i); break;
        case IROp::Add: value = r[a] + r[b]; break;
        case IROp::Sub: value = r[a] - r[b]; break;
        case IROp::Mul: value = r[a] * r[b]; break;
        case IROp::Div: value = r[a] / r[b]; break;
        case IROp::Neg: value = -r[a]; break;
        case IROp::Compare:
            // Invalid operands must not be turned into an apparently valid
            // boolean. Poison propagates until a conditional selects a value.
            if (!isfinite(r[a]) || !isfinite(r[b])) break;
            switch (instruction.compare) {
            case '<': value = r[a] < r[b]; break;
            case '>': value = r[a] > r[b]; break;
            case 'l': value = r[a] <= r[b]; break;
            case 'g': value = r[a] >= r[b]; break;
            case '=': value = r[a] == r[b]; break;
            case '!': value = r[a] != r[b]; break;
            }
            break;
        case IROp::Select:
            if (isfinite(r[a])) value = r[a] != 0.0 ? r[b] : r[c];
            break;
        case IROp::Min: if (isfinite(r[a]) && isfinite(r[b])) value = fmin(r[a], r[b]); break;
        case IROp::Max: if (isfinite(r[a]) && isfinite(r[b])) value = fmax(r[a], r[b]); break;
        case IROp::Clamp:
            if (isfinite(r[a]) && isfinite(r[b]) && isfinite(r[c]) && r[b] <= r[c])
                value = fmin(r[c], fmax(r[b], r[a]));
            break;
        case IROp::Abs: value = fabs(r[a]); break;
        case IROp::Sin: value = sin(r[a]); break;
        case IROp::Cos: value = cos(r[a]); break;
        case IROp::Pow: value = pow(r[a], r[b]); break;
        }
        r[instruction.dst] = value;
    }
    // Pure predication: a dead conditional arm may carry NaN, but only the
    // selected result is validated/published. No instruction has side effects.
    for (unsigned c = 0; c < output.components; ++c)
        if (!Store(r[result.value[c]], output, i * output.components + c)) atomicExch(error, 1);
}
} // namespace

CudaExpressionProgram::~CudaExpressionProgram() {
    if (ready_) { cudaEventSynchronize(ready_); cudaEventDestroy(ready_); }
}

ExpressionStatus CudaExpressionProgram::Upload(expr::IRProgram const& program, cudaStream_t stream) {
    if (pending_) return ExpressionStatus::InvalidArgument;
    if (program.instructions.empty() || program.instructions.size() > 4096 ||
        program.registerCount == 0 || program.registerCount > kRegisters ||
        program.result >= program.registerCount || program.outputCount > 4 ||
        program.valueComponents == 0 || program.valueComponents > 4 ||
        (program.outputCount && program.outputCount != program.valueComponents))
        return ExpressionStatus::InvalidProgram;
    bool written[kRegisters]{};
    for (auto const& op : program.instructions) {
        if (op.dst >= program.registerCount || written[op.dst]) return ExpressionStatus::InvalidProgram;
        auto defined = [&](uint16_t r) { return r < program.registerCount && written[r]; };
        switch (op.op) {
        case IROp::Const: break;
        case IROp::LoadVariable:
            if (op.variable == Variable::Invalid || op.variable >= Variable::CountVariables)
                return ExpressionStatus::InvalidProgram;
            // Shared core controls are scalar; geometric fields and $value
            // carry their explicit component indexing in the uploaded IR.
            if (op.component > 3 ||
                (op.variable >= Variable::Frame && op.variable <= Variable::DescId && op.component != 0))
                return ExpressionStatus::InvalidProgram;
            break;
        case IROp::Neg: case IROp::Abs: case IROp::Sin: case IROp::Cos:
            if (!defined(op.a)) return ExpressionStatus::InvalidProgram; break;
        case IROp::Add: case IROp::Sub: case IROp::Mul: case IROp::Div:
        case IROp::Min: case IROp::Max: case IROp::Pow: case IROp::Compare:
            if (!defined(op.a) || !defined(op.b)) return ExpressionStatus::InvalidProgram;
            if (op.op == IROp::Compare && op.compare != '<' && op.compare != '>' &&
                op.compare != 'l' && op.compare != 'g' && op.compare != '=' && op.compare != '!')
                return ExpressionStatus::InvalidProgram;
            break;
        case IROp::Select: case IROp::Clamp:
            if (!defined(op.a) || !defined(op.b) || !defined(op.c)) return ExpressionStatus::InvalidProgram;
            break;
        default: return ExpressionStatus::InvalidProgram;
        }
        written[op.dst] = true;
    }
    if (!written[program.result]) return ExpressionStatus::InvalidProgram;
    for (unsigned c = 0; c < program.outputCount; ++c)
        if (program.output[c] >= program.registerCount || !written[program.output[c]])
            return ExpressionStatus::InvalidProgram;
    instructionCount_ = 0;
    if (!ready_ && cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming) != cudaSuccess)
        return ExpressionStatus::CudaError;
    if (cudaStreamWaitEvent(stream, ready_, 0) != cudaSuccess ||
        code_.reset(program.instructions.size() * sizeof(expr::IRInstruction)) != cudaSuccess ||
        error_.reset(1) != cudaSuccess) return ExpressionStatus::CudaError;
    if (cudaMemcpyAsync(code_.data(), program.instructions.data(), code_.size(),
                        cudaMemcpyHostToDevice, stream) != cudaSuccess ||
        cudaEventRecord(ready_, stream) != cudaSuccess ||
        cudaEventSynchronize(ready_) != cudaSuccess) return ExpressionStatus::CudaError;
    // The caller's host IR need not outlive Upload.
    instructionCount_ = program.instructions.size(); result_ = program.result;
    outputCount_ = program.outputCount ? program.outputCount : 1;
    for (unsigned c = 0; c < outputCount_; ++c)
        output_[c] = program.outputCount ? program.output[c] : program.result;
    return ExpressionStatus::Ok;
}

ExpressionStatus CudaExpressionProgram::Evaluate(ExpressionInputs const& inputs,
                                                ExpressionOutput output, cudaStream_t stream) {
    if (pending_ || !instructionCount_ || !IsDomain(inputs.context.domain) ||
        inputs.count > size_t(INT_MAX) || inputs.count != output.count ||
        (inputs.count && !output.data) || output.type == ScalarType::Invalid ||
        output.components != outputCount_ ||
        (inputs.context.domain == Domain::Groom && inputs.count != 1))
        return ExpressionStatus::InvalidArgument;
    for (auto const& field : inputs.fields) {
        if (!field.data && field.count == 0) continue; // an unused variable
        if (!field.data || !IsDomain(field.domain) || field.components == 0 || field.components > 4 ||
            field.count > size_t(INT_MAX)) return ExpressionStatus::InvalidArgument;
        size_t expected = field.domain == Domain::Groom ? 1 : inputs.count;
        if (field.domain == Domain::Primitive && inputs.context.domain == Domain::Point) {
            expected = inputs.primitiveCount;
            if (!inputs.pointToPrimitive.data || inputs.pointToPrimitive.size != inputs.count)
                return ExpressionStatus::InvalidArgument;
        } else if (field.domain != Domain::Groom && field.domain != inputs.context.domain)
            return ExpressionStatus::InvalidArgument;
        if (field.count != expected) return ExpressionStatus::InvalidArgument;
    }
    if (cudaStreamWaitEvent(stream, ready_, 0) != cudaSuccess ||
        cudaMemsetAsync(error_.data(), 0, sizeof(int), stream) != cudaSuccess)
        return ExpressionStatus::CudaError;
    if (inputs.count) {
        OutputRegisters outputs{};
        for (unsigned c = 0; c < outputCount_; ++c) outputs.value[c] = output_[c];
        Execute<<<(inputs.count + 127) / 128, 128, 0, stream>>>(
            reinterpret_cast<expr::IRInstruction const*>(code_.data()), instructionCount_,
            outputs, inputs, output, error_.data());
        if (cudaGetLastError() != cudaSuccess) return ExpressionStatus::CudaError;
    }
    if (cudaEventRecord(ready_, stream) != cudaSuccess) return ExpressionStatus::CudaError;
    pending_ = true;
    return ExpressionStatus::Ok;
}

ExpressionStatus CudaExpressionProgram::Finish(cudaStream_t stream) {
    if (!pending_) return ExpressionStatus::InvalidArgument;
    int error = 0;
    if (cudaStreamWaitEvent(stream, ready_, 0) != cudaSuccess ||
        cudaMemcpyAsync(&error, error_.data(), sizeof(error), cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
        cudaStreamSynchronize(stream) != cudaSuccess) return ExpressionStatus::CudaError;
    pending_ = false;
    return error ? ExpressionStatus::InvalidValue : ExpressionStatus::Ok;
}
} // namespace usdGen::gpu
