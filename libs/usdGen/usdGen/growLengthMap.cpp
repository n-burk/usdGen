#include "usdGen/growLengthMap.h"

#include "pxr/base/gf/vec2f.h"

#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {
namespace {

bool Fail(std::string const& message, std::string* reason)
{
    if (reason) *reason = "UsdGenGrow: " + message;
    return false;
}

bool ReadToken(UsdGenMapDesc const& map, char const* name,
               TfToken const& fallback, TfToken* result, std::string* reason)
{
    bool seen = false;
    for (UsdGenParamValue const& param : map.params) {
        if (param.name != TfToken(name)) continue;
        if (seen) return Fail(std::string("ImageMap has duplicate '") + name + "'", reason);
        if (param.value.IsHolding<TfToken>())
            *result = param.value.UncheckedGet<TfToken>();
        else if (param.value.IsHolding<std::string>())
            *result = TfToken(param.value.UncheckedGet<std::string>());
        else
            return Fail(std::string("ImageMap parameter '") + name + "' must be a token", reason);
        seen = true;
    }
    if (!seen) *result = fallback;
    return true;
}

bool ReadFloat(UsdGenMapDesc const& map, char const* name, float fallback,
               float* result, std::string* reason)
{
    bool seen = false;
    for (UsdGenParamValue const& param : map.params) {
        if (param.name != TfToken(name)) continue;
        if (seen) return Fail(std::string("ImageMap has duplicate '") + name + "'", reason);
        double value = 0.0;
        if (param.value.IsHolding<float>())
            value = param.value.UncheckedGet<float>();
        else if (param.value.IsHolding<double>())
            value = param.value.UncheckedGet<double>();
        else
            return Fail(std::string("ImageMap parameter '") + name + "' must be a float", reason);
        float const narrowed = static_cast<float>(value);
        if (!std::isfinite(value) || !std::isfinite(narrowed))
            return Fail(std::string("ImageMap parameter '") + name + "' must be finite", reason);
        *result = narrowed;
        seen = true;
    }
    if (!seen) *result = fallback;
    return true;
}

bool ReadClamp(UsdGenMapDesc const& map, UsdGenImageSampleOptions* options,
               std::string* reason)
{
    bool seen = false;
    for (UsdGenParamValue const& param : map.params) {
        if (param.name != TfToken("map:clamp")) continue;
        if (seen || !param.value.IsHolding<GfVec2f>())
            return Fail("ImageMap clamp must be one float2", reason);
        GfVec2f const range = param.value.UncheckedGet<GfVec2f>();
        if (!std::isfinite(range[0]) || !std::isfinite(range[1]))
            return Fail("ImageMap clamp must be finite", reason);
        options->outputMin = range[0];
        options->outputMax = range[1];
        options->clampOutput = range[0] != range[1];
        seen = true;
    }
    return true;
}

bool ReadOptions(UsdGenMapDesc const& map, UsdGenImageSampleOptions* options,
                 std::string* reason)
{
    TfToken domain, uvSet, filter, wrap, channel, colorSpace;
    if (!ReadToken(map, "map:domain", TfToken("root"), &domain, reason) ||
        !ReadToken(map, "map:uvSet", TfToken("st"), &uvSet, reason) ||
        !ReadToken(map, "map:filter", TfToken("bilinear"), &filter, reason) ||
        !ReadToken(map, "map:wrap", TfToken("clamp"), &wrap, reason) ||
        !ReadToken(map, "map:channel", TfToken("r"), &channel, reason) ||
        !ReadToken(map, "map:colorSpace", TfToken("raw"), &colorSpace, reason) ||
        !ReadFloat(map, "map:scale", 1.0f, &options->scale, reason) ||
        !ReadFloat(map, "map:offset", 0.0f, &options->offset, reason) ||
        !ReadFloat(map, "map:default", 0.0f, &options->defaultValue, reason) ||
        !ReadClamp(map, options, reason))
        return false;

    if (domain != TfToken("root") || uvSet != TfToken("st") ||
        colorSpace != TfToken("raw"))
        return Fail("LengthSource ImageMap requires root domain, st UVs, and raw color space", reason);
    if (filter == TfToken("nearest")) options->filter = UsdGenImageFilter::Nearest;
    else if (filter == TfToken("bilinear")) options->filter = UsdGenImageFilter::Bilinear;
    else return Fail("LengthSource ImageMap has unsupported filter", reason);
    if (wrap == TfToken("clamp")) options->wrap = UsdGenImageWrap::Clamp;
    else if (wrap == TfToken("repeat")) options->wrap = UsdGenImageWrap::Repeat;
    else if (wrap == TfToken("mirror")) options->wrap = UsdGenImageWrap::Mirror;
    else if (wrap == TfToken("black")) options->wrap = UsdGenImageWrap::Black;
    else return Fail("LengthSource ImageMap has unsupported wrap", reason);
    if (channel == TfToken("r")) options->channel = UsdGenImageChannel::R;
    else if (channel == TfToken("g")) options->channel = UsdGenImageChannel::G;
    else if (channel == TfToken("b")) options->channel = UsdGenImageChannel::B;
    else if (channel == TfToken("a")) options->channel = UsdGenImageChannel::A;
    else if (channel == TfToken("luminance")) options->channel = UsdGenImageChannel::Luminance;
    else return Fail("LengthSource ImageMap requires a scalar channel", reason);
    return true;
}

} // namespace

bool ResolveUsdGenGrowLengthImageMap(
    UsdGenGraphDesc const& desc,
    UsdGenNodeDesc const& node,
    std::shared_ptr<const UsdGenImagePayload>* image,
    UsdGenImageSampleOptions* options,
    std::string* reason)
{
    if (reason) reason->clear();
    if (!image || !options) return Fail("LengthSource ImageMap resolver has null output", reason);
    *image = {};
    *options = {};

    if (node.mapBindings.empty()) {
        if (!node.maps.empty())
            return Fail("LengthSource ImageMap requires exactly one typed usdGen:length:source binding at " +
                        node.path.GetString(), reason);
        return true;
    }
    if (!node.maps.empty() && node.maps.size() != node.mapBindings.size())
        return Fail("legacy and typed LengthSource map bindings disagree at " +
                    node.path.GetString(), reason);
    if (node.mapBindings.size() != 1 ||
        node.mapBindings.front().purpose != UsdGenMapBindingPurpose::LengthSource ||
        (!node.mapBindings.front().relationship.IsEmpty() &&
         node.mapBindings.front().relationship != TfToken("usdGen:length:source")))
        return Fail("Grow accepts exactly one typed usdGen:length:source ImageMap at " +
                    node.path.GetString(), reason);
    if (!node.maps.empty() && node.maps.front() != node.mapBindings.front().map)
        return Fail("legacy and typed LengthSource map bindings disagree at " +
                    node.path.GetString(), reason);

    UsdGenMapDesc const* found = nullptr;
    for (UsdGenMapDesc const& candidate : desc.maps) {
        if (candidate.path != node.mapBindings.front().map) continue;
        if (found)
            return Fail("LengthSource map path resolves ambiguously at " + node.path.GetString(), reason);
        found = &candidate;
    }
    if (!found || found->type != TfToken("UsdGenImageMap") ||
        !found->imagePayload || !found->imagePayload->IsValid())
        return Fail("LengthSource ImageMap has no valid decoded payload at " +
                    node.path.GetString(), reason);
    if (!ReadOptions(*found, options, reason)) return false;
    std::string validation;
    if (!ValidateUsdGenImageSampleOptions(*found->imagePayload, *options, &validation))
        return Fail("LengthSource ImageMap options are invalid at " + node.path.GetString() +
                    ": " + validation, reason);
    *image = found->imagePayload;
    return true;
}

} // namespace usdGen
