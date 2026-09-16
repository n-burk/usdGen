#include "usdGen/cpuParameters.h"

#include "usdGen/expressions/frontend.h"
#include "usdGen/expressions/irExec.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec2h.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3h.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/vec4h.h"
#include "pxr/base/gf/vec4i.h"

#include <algorithm>
#include <cmath>
#include <cstring>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {
namespace {

using Type = expr::ScalarType;
using Domain = expr::Domain;

size_t ScalarBytes(Type type)
{
    switch (type) {
    case Type::Bool: return 1;
    case Type::Float16: return 2;
    case Type::Int32: case Type::UInt32: case Type::Float32: return 4;
    case Type::Int64: case Type::UInt64: case Type::Float64: return 8;
    default: return 0;
    }
}

uint64_t Mix(uint64_t h, uint64_t value)
{
    h ^= value; h *= 0x100000001b3ULL;
    return h;
}
uint64_t MixText(uint64_t h, char const *text)
{
    for (auto const *p = reinterpret_cast<unsigned char const *>(text); p && *p; ++p)
        h = Mix(h, *p);
    return h;
}
uint64_t MixDouble(uint64_t h, double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return Mix(h, bits);
}

template <class T>
bool ScalarLiteral(VtValue const &value, std::vector<double> *output)
{
    if (!value.IsHolding<T>()) return false;
    output->assign(1, static_cast<double>(value.UncheckedGet<T>()));
    return std::isfinite((*output)[0]);
}
template <class T>
bool VectorLiteral(VtValue const &value, unsigned count, std::vector<double> *output)
{
    if (!value.IsHolding<T>() || count != T::dimension) return false;
    output->resize(count);
    auto const &vector = value.UncheckedGet<T>();
    for (unsigned i = 0; i < count; ++i) {
        (*output)[i] = static_cast<double>(vector[i]);
        if (!std::isfinite((*output)[i])) return false;
    }
    return true;
}

/// The literal the expression receives as $value. Identical admission to
/// cudaParameters.cpp's Literal(): the exact native type, finite, and int64
/// limited to the range SeExpr's doubles represent exactly.
bool Literal(VtValue const &value, expr::ValueShape const &shape,
             std::vector<double> *output)
{
    const unsigned n = shape.components;
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

bool SupportedShape(expr::ValueShape const &shape)
{
    return ScalarBytes(shape.scalar) && !shape.isArray && shape.elementCount == 1 &&
        shape.rows == 1 && shape.columns == 1 && shape.components >= 1 &&
        shape.components <= 4;
}
bool SameShape(expr::ValueShape const &a, expr::ValueShape const &b)
{
    return a.scalar == b.scalar && a.elementCount == b.elementCount &&
        a.components == b.components && a.rows == b.rows && a.columns == b.columns &&
        a.isArray == b.isArray;
}

size_t DomainIndex(Domain domain)
{
    return domain == Domain::Groom ? 0 : domain == Domain::Primitive ? 1 : 2;
}

/// Decodes one element of a typed destination buffer back to a double. The
/// buffer was written by expr::Store, so the value is already known to be
/// exactly representable in the destination type.
double Decode(unsigned char const *bytes, Type type, size_t index)
{
    switch (type) {
    case Type::Bool: return bytes[index] ? 1.0 : 0.0;
    case Type::Int32: { int32_t v; std::memcpy(&v, bytes + index * 4, 4); return double(v); }
    case Type::UInt32: { uint32_t v; std::memcpy(&v, bytes + index * 4, 4); return double(v); }
    case Type::Int64: { int64_t v; std::memcpy(&v, bytes + index * 8, 8); return double(v); }
    case Type::UInt64: { uint64_t v; std::memcpy(&v, bytes + index * 8, 8); return double(v); }
    case Type::Float16: {
        uint16_t bits; std::memcpy(&bits, bytes + index * 2, 2);
        GfHalf half; half.setBits(bits); return double(float(half));
    }
    case Type::Float32: { float v; std::memcpy(&v, bytes + index * 4, 4); return double(v); }
    case Type::Float64: { double v; std::memcpy(&v, bytes + index * 8, 8); return v; }
    default: return 0.0;
    }
}

} // namespace

bool UsdGenCpuParameters::Clear() noexcept
{
    const bool had = !values_.empty();
    values_.clear();
    digest_ = 0;
    return had;
}

UsdGenExpressionValue const *UsdGenCpuParameters::Find(TfToken const &destination) const
{
    const TfToken canonical = UsdGenCanonicalParamName(destination);
    for (auto const &value : values_)
        if (value.destination == canonical) return &value;
    return nullptr;
}

bool UsdGenCpuParameters::Compile(UsdGenGraphDesc const &desc, UsdGenNodeDesc const &node,
                                  std::vector<std::string> *diagnostics)
{
    auto fail = [&](std::string const &destination, std::string const &message) {
        if (diagnostics) diagnostics->push_back(destination + ": " + message);
        items_.clear();
        compiled_ = false;
        return false;
    };
    items_.clear();
    compiled_ = false;
    for (auto const &binding : node.expressionBindings) {
        auto const name = binding.destination.GetString();
        if (binding.destination.IsEmpty() || binding.expression.IsEmpty())
            return fail(name, "empty expression destination or path");
        auto expression = std::find_if(desc.expressions.begin(), desc.expressions.end(),
            [&](auto const &candidate) { return candidate.path == binding.expression; });
        if (expression == desc.expressions.end())
            return fail(name, "missing expression " + binding.expression.GetString());
        // An empty output is a connection to the expression PRIM; it resolves
        // to outputs:result, or to a single declared output.
        UsdGenExpressionOutputDesc const *result =
            UsdGenFindExpressionOutput(*expression, binding.output);
        if (!result)
            return fail(name, binding.output.IsEmpty()
                ? "connection to prim " + binding.expression.GetString() +
                      " declaring no outputs:result and no single outputs:* attribute"
                : "missing expression output " + binding.output.GetString());
        if (!expr::ExprIsDomain(binding.domain))
            return fail(name, "invalid evaluation domain");
        if (!SupportedShape(binding.destinationShape))
            return fail(name, "unsupported array/matrix/type shape");
        if (binding.nativeType.IsEmpty() || result->nativeType != binding.nativeType ||
            !SameShape(result->shape, binding.destinationShape))
            return fail(name, "output native type/shape mismatch");
        Item item;
        item.binding = binding;
        item.canonical = UsdGenCanonicalParamName(binding.destination);
        if (!Literal(binding.literal, binding.destinationShape, &item.literal))
            return fail(name, "literal must have the exact finite native type; "
                              "int64/uint64 currently limited to 2^53");
        auto compiled = expr::Frontend::Compile(expression->source,
            {binding.domain, binding.destinationShape.scalar,
             binding.destinationShape.components});
        if (!compiled.ok) {
            if (diagnostics)
                for (auto const &message : compiled.diagnostics)
                    diagnostics->push_back(name + ": " + message);
            return fail(name, "SeExpr compilation failed");
        }
        item.ir = compiled.program.IR();
        items_.push_back(std::move(item));
    }
    compiled_ = true;
    return true;
}

bool UsdGenCpuParameters::Evaluate(UsdGenGraphDesc const &desc, UsdGenNodeDesc const &node,
                                   UsdGenCurveBuffer const &geometry, double frame,
                                   double time, uint32_t seed, bool *changed,
                                   std::vector<std::string> *diagnostics)
{
    const uint64_t previous = digest_;
    auto publish = [&](bool ok) {
        if (changed) *changed = digest_ != previous;
        return ok;
    };
    if (node.expressionBindings.empty()) {
        Clear();
        return publish(true);
    }
    // Recompile whenever a binding or its source text moved. This is what lets
    // an expression edit land without the graph being rebuilt underneath us.
    uint64_t sourceDigest = 1469598103934665603ULL;
    for (auto const &binding : node.expressionBindings) {
        sourceDigest = MixText(sourceDigest, binding.destination.GetText());
        sourceDigest = MixText(sourceDigest, binding.expression.GetText());
        sourceDigest = MixText(sourceDigest, binding.output.GetText());
        sourceDigest = MixText(sourceDigest, binding.nativeType.GetText());
        sourceDigest = Mix(sourceDigest, uint64_t(binding.domain));
        sourceDigest = Mix(sourceDigest, uint64_t(binding.destinationShape.scalar));
        sourceDigest = Mix(sourceDigest, binding.destinationShape.components);
        sourceDigest = Mix(sourceDigest,
            binding.literal.IsEmpty() ? 0u : uint64_t(binding.literal.GetHash()));
        auto expression = std::find_if(desc.expressions.begin(), desc.expressions.end(),
            [&](auto const &candidate) { return candidate.path == binding.expression; });
        if (expression != desc.expressions.end())
            sourceDigest = MixText(sourceDigest, expression->source.c_str());
    }
    if (!compiled_ || sourceDigest != sourceDigest_) {
        sourceDigest_ = sourceDigest;
        if (!Compile(desc, node, diagnostics)) { Clear(); return publish(false); }
    }

    // --- geometry view over the node's INPUT curves ------------------------
    // Uniform buffers carry no cvOffsets; synthesize the offsets the shared
    // context builder needs so both layouts take the same path.
    const size_t curveCount = geometry.totalCurves;
    const size_t pointCount = geometry.totalCvs;
    offsets_.assign(curveCount + 1, 0);
    if (!geometry.cvOffsets.empty() &&
        size_t(geometry.cvOffsets.size()) == curveCount + 1) {
        for (size_t c = 0; c <= curveCount; ++c)
            offsets_[c] = static_cast<uint32_t>(geometry.cvOffsets[c]);
    } else if (curveCount) {
        const uint32_t perCurve = static_cast<uint32_t>(pointCount / curveCount);
        for (size_t c = 0; c <= curveCount; ++c)
            offsets_[c] = static_cast<uint32_t>(c * perCurve);
    }
    // A generic source may publish no stable ids; $id/$idLo/$idHi then read the
    // curve index, which is what the CPU operators use as the id fallback.
    ids_.clear();
    uint64_t const *ids = geometry.curveId.empty() ? nullptr : geometry.curveId.cdata();
    if (!ids && curveCount) {
        ids_.resize(curveCount);
        for (size_t c = 0; c < curveCount; ++c) ids_[c] = c;
        ids = ids_.data();
    }
    expr::CpuCurveGeometryView view;
    view.px = geometry.px.empty() ? nullptr : geometry.px.cdata();
    view.py = geometry.py.empty() ? nullptr : geometry.py.cdata();
    view.pz = geometry.pz.empty() ? nullptr : geometry.pz.cdata();
    view.rest = geometry.rest.empty() ? nullptr
        : reinterpret_cast<float const *>(geometry.rest.cdata());
    view.widths = geometry.width.empty() ? nullptr : geometry.width.cdata();
    view.hairT = geometry.hairT.empty() ? nullptr : geometry.hairT.cdata();
    view.rootUV = geometry.rootUV.empty() ? nullptr
        : reinterpret_cast<float const *>(geometry.rootUV.cdata());
    // The rest root frame and the surface face index feed $N/$Nref, $dPdu/
    // $dPduref, $dPdv/$dPdvref and $faceId. A producer that did not publish a
    // full per-curve plane leaves the channel null, which keeps the variable
    // unavailable rather than inventing a frame.
    auto rootFrame = [&](VtVec3fArray const &plane) -> float const * {
        return curveCount && plane.size() == curveCount
            ? reinterpret_cast<float const *>(plane.cdata()) : nullptr;
    };
    view.rootN = rootFrame(geometry.rootN);
    view.rootT = rootFrame(geometry.rootT);
    view.rootB = rootFrame(geometry.rootB);
    view.rootPrim = curveCount && geometry.rootPrim.size() == curveCount
        ? geometry.rootPrim.cdata() : nullptr;
    view.stableIds = ids;
    view.curveOffsets = offsets_.data();
    view.curveCount = curveCount;
    view.pointCount = pointCount;

    expr::Context controls;
    controls.frame = frame;
    controls.time = time;
    controls.seed = static_cast<int32_t>(seed);
    controls.descId = expr::DescriptionId(desc.description.GetText());

    expr::CpuExpressionContext contexts[3];
    bool built[3]{};
    for (auto const &item : items_) {
        const size_t index = DomainIndex(item.binding.domain);
        if (built[index]) continue;
        auto domainControls = controls;
        domainControls.domain = index == 0 ? Domain::Groom
            : index == 1 ? Domain::Primitive : Domain::Point;
        std::string diagnostic;
        if (contexts[index].Build(view, domainControls, &diagnostic) !=
            expr::CpuExpressionStatus::Ok) {
            if (diagnostics)
                diagnostics->push_back(node.path.GetString() +
                    ": expression geometry context validation failed: " + diagnostic);
            Clear();
            return publish(false);
        }
        built[index] = true;
    }

    std::vector<UsdGenExpressionValue> candidate;
    candidate.reserve(items_.size());
    for (auto const &item : items_) {
        auto inputs = contexts[DomainIndex(item.binding.domain)].Inputs();
        const unsigned components = item.binding.destinationShape.components;
        const Type type = item.binding.destinationShape.scalar;
        // $value is the authored literal, broadcast at groom rate exactly as
        // CudaParameterEvaluator installs it.
        inputs.fields[static_cast<unsigned>(expr::Variable::Value)] =
            {item.literal.data(), 1, Domain::Groom, components};
        std::vector<unsigned char> raw(inputs.count * components * ScalarBytes(type), 0);
        expr::CpuExpressionOutput output;
        output.data = raw.empty() ? nullptr : raw.data();
        output.count = inputs.count;
        output.type = type;
        output.components = components;
        const auto status = expr::EvaluateProgram(item.ir, inputs, output);
        if (status != expr::CpuExpressionStatus::Ok) {
            if (diagnostics)
                diagnostics->push_back(item.binding.destination.GetString() + ": " +
                    (status == expr::CpuExpressionStatus::InvalidValue
                        ? "expression produced a value the destination cannot represent"
                        : "expression evaluation was refused"));
            Clear();
            return publish(false);
        }
        UsdGenExpressionValue value;
        value.destination = item.canonical;
        value.domain = item.binding.domain;
        value.type = type;
        value.components = components;
        value.count = inputs.count;
        value.values.resize(inputs.count * components);
        for (size_t i = 0; i < value.values.size(); ++i)
            value.values[i] = Decode(raw.data(), type, i);
        candidate.push_back(std::move(value));
    }

    uint64_t digest = 1469598103934665603ULL;
    for (auto const &value : candidate) {
        digest = MixText(digest, value.destination.GetText());
        digest = Mix(digest, uint64_t(value.domain));
        digest = Mix(digest, uint64_t(value.type));
        digest = Mix(digest, value.components);
        digest = Mix(digest, value.count);
        for (double v : value.values) digest = MixDouble(digest, v);
    }
    values_ = std::move(candidate);
    digest_ = digest;
    return publish(true);
}

} // namespace usdGen
