#include "usdGen/deviceGeneration.h"

#include <algorithm>
#include <utility>

namespace usdGen {
namespace {

size_t ScalarBytes(UsdGenDeviceValueType type)
{
    switch (type) {
    case UsdGenDeviceValueType::Float32:
    case UsdGenDeviceValueType::Float32x2:
    case UsdGenDeviceValueType::Float32x3:
    case UsdGenDeviceValueType::Float32x4:
    case UsdGenDeviceValueType::Int32:
    case UsdGenDeviceValueType::UInt32:
        return 4;
    case UsdGenDeviceValueType::UInt64:
        return 8;
    }
    return 0;
}

uint32_t TypeArity(UsdGenDeviceValueType type)
{
    switch (type) {
    case UsdGenDeviceValueType::Float32x2: return 2;
    case UsdGenDeviceValueType::Float32x3: return 3;
    case UsdGenDeviceValueType::Float32x4: return 4;
    default: return 1;
    }
}

bool ValidValueType(UsdGenDeviceValueType type)
{
    switch (type) {
    case UsdGenDeviceValueType::Float32:
    case UsdGenDeviceValueType::Float32x2:
    case UsdGenDeviceValueType::Float32x3:
    case UsdGenDeviceValueType::Float32x4:
    case UsdGenDeviceValueType::Int32:
    case UsdGenDeviceValueType::UInt32:
    case UsdGenDeviceValueType::UInt64:
        return true;
    }
    return false;
}

bool ValidDomain(UsdGenDeviceDomain domain)
{
    switch (domain) {
    case UsdGenDeviceDomain::Groom:
    case UsdGenDeviceDomain::Primitive:
    case UsdGenDeviceDomain::Point:
    case UsdGenDeviceDomain::Tile:
    case UsdGenDeviceDomain::Topology:
    case UsdGenDeviceDomain::Tool:
        return true;
    }
    return false;
}

bool ValidSemantic(UsdGenDeviceChannelSemantic semantic)
{
    switch (semantic) {
    case UsdGenDeviceChannelSemantic::Generic:
    case UsdGenDeviceChannelSemantic::Points:
    case UsdGenDeviceChannelSemantic::RestPoints:
    case UsdGenDeviceChannelSemantic::Widths:
    case UsdGenDeviceChannelSemantic::HairT:
    case UsdGenDeviceChannelSemantic::CurveOffsets:
    case UsdGenDeviceChannelSemantic::StableIds:
    case UsdGenDeviceChannelSemantic::RootPrim:
    case UsdGenDeviceChannelSemantic::RootUV:
        return true;
    }
    return false;
}

bool ValidSemanticShape(UsdGenDeviceChannelMetadata const &channel,
                        UsdGenDeviceGeometryMetadata const &geometry)
{
    using S = UsdGenDeviceChannelSemantic;
    switch (channel.semantic) {
    case S::Generic:
        switch (channel.domain) {
        case UsdGenDeviceDomain::Groom:
        case UsdGenDeviceDomain::Tool:
            return channel.elementCount == 1;
        case UsdGenDeviceDomain::Primitive:
            return channel.elementCount == geometry.curveCount;
        case UsdGenDeviceDomain::Point:
            return channel.elementCount == geometry.pointCount;
        case UsdGenDeviceDomain::Tile:
            return channel.elementCount == geometry.tiles.size();
        case UsdGenDeviceDomain::Topology:
            return false;
        }
        return false;
    case S::Points:
    case S::RestPoints:
        return channel.type == UsdGenDeviceValueType::Float32x3 &&
               channel.domain == UsdGenDeviceDomain::Point &&
               channel.elementCount == geometry.pointCount;
    case S::Widths:
    case S::HairT:
        return channel.type == UsdGenDeviceValueType::Float32 &&
               channel.domain == UsdGenDeviceDomain::Point &&
               channel.elementCount == geometry.pointCount;
    case S::CurveOffsets:
        return channel.type == UsdGenDeviceValueType::UInt32 &&
               channel.domain == UsdGenDeviceDomain::Topology &&
               geometry.curveCount != UINT64_MAX &&
               channel.elementCount == geometry.curveCount + 1;
    case S::StableIds:
        return channel.type == UsdGenDeviceValueType::UInt64 &&
               channel.domain == UsdGenDeviceDomain::Primitive &&
               channel.elementCount == geometry.curveCount;
    case S::RootPrim:
        return channel.type == UsdGenDeviceValueType::Int32 &&
               channel.domain == UsdGenDeviceDomain::Primitive &&
               channel.elementCount == geometry.curveCount;
    case S::RootUV:
        return channel.type == UsdGenDeviceValueType::Float32x2 &&
               channel.domain == UsdGenDeviceDomain::Primitive &&
               channel.elementCount == geometry.curveCount;
    }
    return false;
}

bool Fail(std::string *reason, char const *message)
{
    if (reason) *reason = message;
    return false;
}

} // namespace

struct UsdGenDeviceLease::State
{
    State(std::shared_ptr<const UsdGenDeviceOwner> owner_,
          std::unique_ptr<UsdGenDeviceConsumer> consumer_)
        : owner(std::move(owner_)), consumer(std::move(consumer_)) {}

    ~State() { if (consumer) consumer->Complete(); }

    // Declaration order is intentional: State destroys consumer before owner,
    // keeping the raw owner pointer held by a backend consumer valid.
    std::shared_ptr<const UsdGenDeviceOwner> owner;
    std::unique_ptr<UsdGenDeviceConsumer> consumer;
};

UsdGenDeviceLease::~UsdGenDeviceLease() = default;

UsdGenDeviceStream UsdGenDeviceLease::Stream() const noexcept
{
    return _state && _state->consumer ? _state->consumer->Stream()
                                      : UsdGenDeviceStream(0);
}

UsdGenDeviceStatus UsdGenDeviceLease::WaitUntilReady() const noexcept
{
    if (!_state || !_state->consumer) return UsdGenDeviceStatus::InvalidLease;
    return _state->consumer->WaitUntilReady();
}

void UsdGenDeviceLease::Complete() noexcept
{
    // Do not complete the shared state here: another copied lease may still
    // be using the backend handle.  State::~State completes exactly once when
    // this is the final lease copy.
    _state.reset();
}

std::shared_ptr<const UsdGenDeviceGeneration>
UsdGenDeviceGeneration::Create(CreateInfo info, std::string *reason)
{
    if (!info.owner) {
        Fail(reason, "device generation requires an owner");
        return {};
    }
    if (info.identity.backend == UsdGenDeviceBackend::Unknown ||
        info.identity.deviceIndex < 0) {
        Fail(reason, "device generation has invalid backend identity");
        return {};
    }
    if ((info.geometry.pointCount == 0 && info.geometry.curveCount != 0) ||
        (info.geometry.curveCount == 0 && info.geometry.pointCount != 0)) {
        Fail(reason, "device geometry has mismatched curve and point counts");
        return {};
    }

    std::vector<std::string> names;
    names.reserve(info.channels.size());
    for (UsdGenDeviceChannelMetadata const &channel : info.channels) {
        if (channel.name.empty() || channel.arity == 0 ||
            !ValidValueType(channel.type) || !ValidDomain(channel.domain) ||
            !ValidSemantic(channel.semantic) || !channel.readOnly ||
            channel.arity != TypeArity(channel.type)) {
            Fail(reason, "device channel has invalid name or arity");
            return {};
        }
        size_t const scalarBytes = ScalarBytes(channel.type);
        if (scalarBytes == 0 ||
            channel.strideBytes < scalarBytes * channel.arity) {
            Fail(reason, "device channel has invalid stride");
            return {};
        }
        if (!ValidSemanticShape(channel, info.geometry)) {
            Fail(reason, "device channel has invalid semantic shape or domain");
            return {};
        }
        if (std::find(names.begin(), names.end(), channel.name) != names.end()) {
            Fail(reason, "device generation has duplicate channel");
            return {};
        }
        names.push_back(channel.name);
    }

    std::vector<uint32_t> tileIds;
    tileIds.reserve(info.geometry.tiles.size());
    for (UsdGenDeviceTileMetadata const &tile : info.geometry.tiles) {
        if (tile.firstCurve > info.geometry.curveCount ||
            tile.curveCount > info.geometry.curveCount - tile.firstCurve ||
            tile.firstPoint > info.geometry.pointCount ||
            tile.pointCount > info.geometry.pointCount - tile.firstPoint) {
            Fail(reason, "device tile range exceeds geometry");
            return {};
        }
        if (std::find(tileIds.begin(), tileIds.end(), tile.tile) != tileIds.end()) {
            Fail(reason, "device generation has duplicate tile");
            return {};
        }
        tileIds.push_back(tile.tile);
    }

    std::shared_ptr<UsdGenDeviceGeneration> generation(
        new UsdGenDeviceGeneration());
    generation->_identity = std::move(info.identity);
    generation->_geometry = std::move(info.geometry);
    generation->_tool = std::move(info.tool);
    generation->_channels = std::move(info.channels);
    generation->_owner = std::move(info.owner);
    return std::shared_ptr<const UsdGenDeviceGeneration>(std::move(generation));
}

UsdGenDeviceIdentity const &UsdGenDeviceGeneration::Identity() const noexcept
{
    return _identity;
}

UsdGenDeviceGeometryMetadata const &UsdGenDeviceGeneration::Geometry() const noexcept
{
    return _geometry;
}

UsdGenDeviceToolMetadata const &UsdGenDeviceGeneration::Tool() const noexcept
{
    return _tool;
}

std::vector<UsdGenDeviceChannelMetadata> const &
UsdGenDeviceGeneration::Channels() const noexcept
{
    return _channels;
}

std::shared_ptr<const UsdGenDeviceOwner>
UsdGenDeviceGeneration::Owner() const noexcept
{
    return _owner;
}

UsdGenDeviceLease
UsdGenDeviceGeneration::AcquireLease(UsdGenDeviceStream stream) const noexcept
{
    if (!_owner) return {};
    std::unique_ptr<UsdGenDeviceConsumer> consumer =
        _owner->AcquireConsumer(stream);
    if (!consumer) return {};
    if (consumer->Stream() != stream) {
        consumer->Complete();
        return {};
    }
    try {
        return UsdGenDeviceLease(
            std::make_shared<UsdGenDeviceLease::State>(
                _owner, std::move(consumer)));
    } catch (...) {
        // Allocation can fail before State takes ownership. Do not require
        // backend destructors to implement the explicit completion contract.
        if (consumer) consumer->Complete();
        return {};
    }
}

} // namespace usdGen
