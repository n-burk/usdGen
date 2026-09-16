#include "usdGen/expressionTargets.h"

#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {
namespace {

using Type = expr::ScalarType;
using Domain = expr::Domain;

bool Reject(std::vector<std::string> *errors, std::string const &message)
{
    if (errors) errors->push_back(message);
    return false;
}

/// scalar/native-type/shape agreement. `vector` marks the one two-component
/// destination (Length's `length:random`).
bool ShapeOk(UsdGenExpressionBinding const &binding, Type scalar, char const *nativeType,
             unsigned components)
{
    auto const &shape = binding.destinationShape;
    return !shape.isArray && shape.elementCount == 1 && shape.rows == 1 &&
        shape.columns == 1 && shape.components == components && shape.scalar == scalar &&
        binding.nativeType == TfToken(nativeType);
}

bool ValidateLength(UsdGenNodeDesc const &node, std::vector<std::string> *errors)
{
    static const std::set<std::string> floats{
        "length:value", "minRemainingLength", "cullThreshold", "mask"};
    std::set<std::string> seen;
    bool ok = true;
    for (auto const &binding : node.expressionBindings) {
        const auto name = UsdGenCanonicalParamName(binding.destination).GetString();
        const bool boolean = name == "enabled", vector = name == "length:random";
        if ((!boolean && !vector && !floats.count(name)) || !seen.insert(name).second ||
            !ShapeOk(binding, boolean ? Type::Bool : Type::Float32,
                     boolean ? "bool" : vector ? "float2" : "float", vector ? 2u : 1u)) {
            ok = Reject(errors, "unsupported or incorrectly typed Length expression " + name);
            continue;
        }
        if ((boolean && binding.domain != Domain::Groom) ||
            ((name == "cullThreshold" || vector) && binding.domain == Domain::Point))
            ok = Reject(errors, "Length enabled requires groom; cullThreshold/random require groom/primitive");
    }
    return ok;
}

bool ValidateWidth(UsdGenNodeDesc const &node, std::vector<std::string> *errors)
{
    static const std::set<std::string> floats{"width", "mask"};
    std::set<std::string> seen;
    bool ok = true;
    for (auto const &binding : node.expressionBindings) {
        const auto name = UsdGenCanonicalParamName(binding.destination).GetString();
        const bool boolean = name == "replace" || name == "enabled";
        if ((!boolean && !floats.count(name)) || !seen.insert(name).second ||
            !ShapeOk(binding, boolean ? Type::Bool : Type::Float32,
                     boolean ? "bool" : "float", 1u)) {
            ok = Reject(errors, "unsupported/incorrectly typed Width expression target " + name);
            continue;
        }
        if (name == "enabled" && binding.domain != Domain::Groom)
            ok = Reject(errors, "Width enabled must evaluate at groom granularity");
    }
    return ok;
}

bool ValidateNoise(UsdGenNodeDesc const &node, std::vector<std::string> *errors)
{
    static const std::set<std::string> floats{
        "noise:magnitude", "noise:frequency", "noise:correlation",
        "noise:lacunarity", "noise:gain", "preserveLength", "mask"};
    std::set<std::string> seen;
    bool ok = true;
    for (auto const &binding : node.expressionBindings) {
        const auto name = UsdGenCanonicalParamName(binding.destination).GetString();
        const bool boolean = name == "enabled" || name == "cumulative";
        const bool integer = name == "noise:octaves" || name == "noise:seed";
        if ((!boolean && !integer && !floats.count(name)) || !seen.insert(name).second ||
            !ShapeOk(binding, boolean ? Type::Bool : integer ? Type::Int32 : Type::Float32,
                     boolean ? "bool" : integer ? "int" : "float", 1u)) {
            ok = Reject(errors, "unsupported or incorrectly typed Noise expression " + name);
            continue;
        }
        if ((name == "enabled" && binding.domain != Domain::Groom) ||
            (name == "cumulative" && binding.domain == Domain::Point) ||
            (name == "noise:seed" && binding.domain == Domain::Point))
            ok = Reject(errors, "Noise expression domain is unsupported for " + name);
    }
    return ok;
}

bool ValidateDeform(UsdGenNodeDesc const &node, std::vector<std::string> *errors)
{
    std::set<std::string> seen;
    bool ok = true;
    for (auto const &binding : node.expressionBindings) {
        const auto name = UsdGenCanonicalParamName(binding.destination).GetString();
        const bool boolean = name == "enabled" || name == "lockRoots";
        const bool integer = name == "rbfSamples";
        if ((!boolean && !integer && name != "mask") || !seen.insert(name).second ||
            !ShapeOk(binding, boolean ? Type::Bool : integer ? Type::Int32 : Type::Float32,
                     boolean ? "bool" : integer ? "int" : "float", 1u)) {
            ok = Reject(errors, "unsupported or incorrectly typed RBF expression " + name);
            continue;
        }
        if (((name == "enabled" || integer) && binding.domain != Domain::Groom) ||
            (name == "lockRoots" && binding.domain == Domain::Point))
            ok = Reject(errors, "RBF structural/enabled controls require groom; root locking requires groom/primitive");
    }
    return ok;
}

bool ValidateCurveSource(UsdGenNodeDesc const &node, std::vector<std::string> *errors)
{
    std::set<std::string> seen;
    bool ok = true;
    for (auto const &binding : node.expressionBindings) {
        const auto name = UsdGenCanonicalParamName(binding.destination).GetString();
        const bool resample = name == "resampleTo" && ShapeOk(binding, Type::Int32, "int", 1u);
        const bool useRest = name == "useRest" && ShapeOk(binding, Type::Bool, "bool", 1u);
        if ((!resample && !useRest) || !seen.insert(name).second ||
            binding.domain != Domain::Groom)
            ok = Reject(errors, "CurveSource only supports groom native resampleTo/useRest expressions");
    }
    return ok;
}

} // namespace

TfToken UsdGenCanonicalParamName(TfToken const &name)
{
    std::string value = name.GetString();
    if (value.compare(0, 7, "usdGen:") == 0) value.erase(0, 7);
    return TfToken(value);
}

bool UsdGenValidateExpressionTargets(UsdGenNodeDesc const &node,
                                     std::vector<std::string> *errors)
{
    if (node.expressionBindings.empty()) return true;
    if (node.type == TfToken("UsdGenWidth")) return ValidateWidth(node, errors);
    if (node.type == TfToken("UsdGenLength")) return ValidateLength(node, errors);
    if (node.type == TfToken("UsdGenNoise")) return ValidateNoise(node, errors);
    if (node.type == TfToken("UsdGenDeform")) return ValidateDeform(node, errors);
    if (node.type == TfToken("UsdGenCurveSource")) return ValidateCurveSource(node, errors);
    // Generators own their topology and consume no connected control: Scatter
    // and Grow would silently ignore one on both lanes, which is worse than a
    // diagnostic. WidthBlend blends two proved widths and owns no parameter
    // evaluation; ReferenceSource accepts no parameters at all.
    return Reject(errors, "operator " + node.type.GetString() + " at " +
        node.path.GetString() + " does not accept connected (expression) parameters");
}

} // namespace usdGen
