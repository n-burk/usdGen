#ifdef USDGEN_ENABLE_CUDA
#include "cudaParameters.h"
#include "usdGen/expressions/frontend.h"
#include "pxr/base/gf/half.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/vec2h.h"
#include "pxr/base/gf/vec3h.h"
#include "pxr/base/gf/vec4h.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/gf/vec4i.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE
namespace usdGen {
namespace {
using Type = expr::ScalarType;
using Domain = expr::Domain;

void Error(std::vector<std::string>* diagnostics, std::string const& message) {
    if (diagnostics) diagnostics->push_back(message);
}
TfToken CanonicalName(TfToken const& name) {
    auto value = name.GetString();
    if (value.compare(0, 7, "usdGen:") == 0) value.erase(0, 7);
    return TfToken(value);
}
size_t ScalarBytes(Type type) {
    switch (type) {
    case Type::Bool: return 1;
    case Type::Float16: return 2;
    case Type::Int32: case Type::UInt32: case Type::Float32: return 4;
    case Type::Int64: case Type::UInt64: case Type::Float64: return 8;
    default: return 0;
    }
}
bool SupportedShape(expr::ValueShape const& shape) {
    return ScalarBytes(shape.scalar) && !shape.isArray &&
        shape.elementCount == 1 && shape.rows == 1 && shape.columns == 1 &&
        shape.components >= 1 && shape.components <= 4;
}
bool SameShape(expr::ValueShape const& a, expr::ValueShape const& b) {
    return a.scalar == b.scalar && a.elementCount == b.elementCount &&
        a.components == b.components && a.rows == b.rows &&
        a.columns == b.columns && a.isArray == b.isArray;
}
template<class T>
bool ScalarLiteral(VtValue const& value, std::vector<double>* output) {
    if (!value.IsHolding<T>()) return false;
    output->assign(1, static_cast<double>(value.UncheckedGet<T>()));
    return std::isfinite((*output)[0]);
}
template<class T>
bool VectorLiteral(VtValue const& value, unsigned count, std::vector<double>* output) {
    if (!value.IsHolding<T>() || count != T::dimension) return false;
    output->resize(count);
    auto const& vector = value.UncheckedGet<T>();
    for (unsigned i = 0; i < count; ++i) {
        (*output)[i] = static_cast<double>(vector[i]);
        if (!std::isfinite((*output)[i])) return false;
    }
    return true;
}
bool Literal(VtValue const& value, expr::ValueShape const& shape,
             std::vector<double>* output) {
    unsigned n = shape.components;
    if (n == 1) {
        switch (shape.scalar) {
        case Type::Bool: return ScalarLiteral<bool>(value, output);
        case Type::Int32: return ScalarLiteral<int32_t>(value, output);
        case Type::UInt32: return ScalarLiteral<uint32_t>(value, output);
        case Type::Int64:
            if (!value.IsHolding<int64_t>() ||
                value.UncheckedGet<int64_t>() < -(int64_t(1) << 53) ||
                value.UncheckedGet<int64_t>() > (int64_t(1) << 53)) return false;
            return ScalarLiteral<int64_t>(value, output);
        case Type::UInt64:
            if (!value.IsHolding<uint64_t>() ||
                value.UncheckedGet<uint64_t>() > (uint64_t(1) << 53)) return false;
            return ScalarLiteral<uint64_t>(value, output);
        case Type::Float16: return ScalarLiteral<GfHalf>(value, output);
        case Type::Float32: return ScalarLiteral<float>(value, output);
        case Type::Float64: return ScalarLiteral<double>(value, output);
        default: return false;
        }
    }
    switch (shape.scalar) {
    case Type::Float16:
        return VectorLiteral<GfVec2h>(value, n, output) ||
            VectorLiteral<GfVec3h>(value, n, output) || VectorLiteral<GfVec4h>(value, n, output);
    case Type::Float32:
        return VectorLiteral<GfVec2f>(value, n, output) ||
            VectorLiteral<GfVec3f>(value, n, output) || VectorLiteral<GfVec4f>(value, n, output);
    case Type::Float64:
        return VectorLiteral<GfVec2d>(value, n, output) ||
            VectorLiteral<GfVec3d>(value, n, output) || VectorLiteral<GfVec4d>(value, n, output);
    case Type::Int32:
        return VectorLiteral<GfVec2i>(value, n, output) ||
            VectorLiteral<GfVec3i>(value, n, output) || VectorLiteral<GfVec4i>(value, n, output);
    default: return false;
    }
}
size_t DomainIndex(Domain domain) {
    return domain == Domain::Groom ? 0 : domain == Domain::Primitive ? 1 : 2;
}
} // namespace

CudaParameterField const* CudaParameterPlan::Find(TfToken const& destination) const {
    auto canonical = CanonicalName(destination);
    for (auto const& field : fields_)
        if (CanonicalName(field.destination) == canonical) return &field;
    return nullptr;
}

CudaParameterStatus CudaParameterPlan::Compile(
    UsdGenGraphDesc const& graph, UsdGenNodeDesc const& node,
    CudaParameterPlan* output, std::vector<std::string>* diagnostics) {
    if (!output) {
        Error(diagnostics, "null parameter plan");
        return CudaParameterStatus::InvalidArgument;
    }
    std::vector<UsdGenExpressionBinding> bindings;
    std::vector<Item> items;
    std::set<TfToken> destinations;
    for (auto const& binding : node.expressionBindings) {
        auto fail = [&](CudaParameterStatus status, std::string message) {
            Error(diagnostics, binding.destination.GetString() + ": " + message);
            return status;
        };
        if (binding.destination.IsEmpty() || binding.expression.IsEmpty() ||
            binding.output.IsEmpty() ||
            !destinations.insert(CanonicalName(binding.destination)).second)
            return fail(CudaParameterStatus::InvalidArgument, "empty or duplicate expression destination/path");
        auto expression = std::find_if(graph.expressions.begin(), graph.expressions.end(),
            [&](auto const& candidate) { return candidate.path == binding.expression; });
        if (expression == graph.expressions.end())
            return fail(CudaParameterStatus::CompileError, "missing expression");
        auto result = std::find_if(expression->outputs.begin(), expression->outputs.end(),
            [&](auto const& candidate) { return candidate.name == binding.output; });
        if (result == expression->outputs.end())
            return fail(CudaParameterStatus::CompileError, "missing expression output");
        if (binding.domain != Domain::Groom && binding.domain != Domain::Primitive &&
            binding.domain != Domain::Point)
            return fail(CudaParameterStatus::InvalidArgument, "invalid evaluation domain");
        if (!SupportedShape(binding.destinationShape))
            return fail(CudaParameterStatus::UnsupportedType, "unsupported array/matrix/type shape");
        if (binding.nativeType.IsEmpty() || result->nativeType != binding.nativeType ||
            !SameShape(result->shape, binding.destinationShape))
            return fail(CudaParameterStatus::InvalidArgument, "output native type/shape mismatch");
        Item item;
        item.binding = binding;
        if (!Literal(binding.literal, binding.destinationShape, &item.literal))
            return fail(CudaParameterStatus::UnsupportedType,
                "literal must have the exact finite native type; int64/uint64 currently limited to 2^53");
        auto compiled = expr::Frontend::Compile(expression->source,
            {binding.domain, binding.destinationShape.scalar, binding.destinationShape.components});
        if (!compiled.ok) {
            for (auto const& message : compiled.diagnostics)
                Error(diagnostics, binding.destination.GetString() + ": " + message);
            return fail(CudaParameterStatus::CompileError, "SeExpr compilation failed");
        }
        item.ir = compiled.program.IR();
        bindings.push_back(binding);
        items.push_back(std::move(item));
    }
    // Only immutable CPU data changes here. An unsuccessful compile leaves
    // both the old program and its published fields usable.
    output->bindings_ = std::move(bindings);
    output->items_ = std::move(items);
    output->fields_.clear();
    return CudaParameterStatus::Ok;
}

CudaParameterStatus CudaParameterPlan::Evaluate(
    gpu::DeviceCurveGeometryView geometry, gpu::ExpressionGeometryChannels channels,
    expr::Context controls, cudaStream_t stream, std::vector<std::string>* diagnostics) {
    fields_.clear();
    // A preceding successful Evaluate finished every GPU reader. No published
    // field survives the next invocation; callers must consume fields first.
    runtime_.clear();
    outputs_.clear();
    literals_.clear();
    for (auto& context : contexts_) context.reset();
    for (auto const& item : items_) {
        auto index = DomainIndex(item.binding.domain);
        if (!contexts_[index]) contexts_[index] = std::make_unique<gpu::CudaExpressionContext>();
    }
    for (size_t i = 0; i < 3; ++i) if (contexts_[i]) {
        auto context = controls;
        context.domain = i == 0 ? Domain::Groom : i == 1 ? Domain::Primitive : Domain::Point;
        if (contexts_[i]->Build(geometry, channels, context, stream) != gpu::ExpressionContextStatus::Ok ||
            contexts_[i]->Finish(stream) != gpu::ExpressionContextStatus::Ok) {
            Error(diagnostics, "expression geometry context validation failed");
            return CudaParameterStatus::CudaError;
        }
    }
    literals_.resize(items_.size());
    outputs_.resize(items_.size());
    runtime_.reserve(items_.size());
    std::vector<CudaParameterField> candidate;
    candidate.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        auto const& item = items_[i];
        auto const& binding = item.binding;
        auto inputs = contexts_[DomainIndex(binding.domain)]->Inputs();
        size_t const components = binding.destinationShape.components;
        size_t const stride = ScalarBytes(binding.destinationShape.scalar) * components;
        auto fail = [&](CudaParameterStatus status, char const* message) {
            // Error exits must not leave queued readers using context/literal
            // allocations that the next invocation will replace.
            cudaStreamSynchronize(stream);
            Error(diagnostics, binding.destination.GetString() + ": " + message);
            return status;
        };
        if (inputs.count > std::numeric_limits<size_t>::max() / stride)
            return fail(CudaParameterStatus::InvalidArgument, "output size overflow");
        if (literals_[i].reset(components) != cudaSuccess ||
            cudaMemcpyAsync(literals_[i].data(), item.literal.data(), components * sizeof(double),
                cudaMemcpyHostToDevice, stream) != cudaSuccess ||
            outputs_[i].reset(inputs.count * stride) != cudaSuccess)
            return fail(CudaParameterStatus::CudaError, "device allocation/literal upload failed");
        inputs.fields[static_cast<unsigned>(expr::Variable::Value)] =
            {literals_[i].data(), 1, Domain::Groom, static_cast<uint32_t>(components)};
        runtime_.push_back(std::make_unique<gpu::CudaExpressionProgram>());
        auto& program = *runtime_.back();
        if (program.Upload(item.ir, stream) != gpu::ExpressionStatus::Ok ||
            program.Evaluate(inputs, {outputs_[i].data(), inputs.count,
                binding.destinationShape.scalar, static_cast<uint32_t>(components)}, stream) != gpu::ExpressionStatus::Ok)
            return fail(CudaParameterStatus::CudaError, "CUDA expression launch failed");
        if (program.Finish(stream) != gpu::ExpressionStatus::Ok)
            return fail(CudaParameterStatus::InvalidValue, "CUDA expression value validation failed");
        candidate.push_back({binding.destination, outputs_[i].data(), inputs.count,
            binding.destinationShape.scalar, static_cast<uint32_t>(components), binding.domain});
    }
    fields_ = std::move(candidate);
    return CudaParameterStatus::Ok;
}
} // namespace usdGen
#endif
