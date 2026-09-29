// usdGen — authored attribute maps into groom cooking/instancing.
// See attributeInstance.h for the contract and its distance from Pomade.
#include "usdGen/maps/attributeInstance.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace usdGen {

bool UsdGenAttributeCookInstances(UsdGenAttributeInstanceInput const &input,
                                  UsdGenAttributeInstanceResult *result,
                                  std::string *error)
{
    auto fail = [&](std::string const &message) {
        if (error) *error = message;
        return false;
    };
    if (!input.map) return fail("instance cook needs a map");
    if (input.channel < 0 || input.channel >= input.map->Channels())
        return fail("instance cook channel is outside the map");
    if (input.roots.empty()) return fail("instance cook needs at least one root");
    if (input.numPrototypes < 1)
        return fail("instance cook needs numPrototypes >= 1");
    if (!std::isfinite(input.threshold))
        return fail("instance cook threshold must be finite");
    if (!std::isfinite(input.defaultValue))
        return fail("instance cook default must be finite");
    if (!result) return fail("instance cook needs a result");

    UsdGenAttributeInstanceResult cooked;
    size_t const count = input.roots.size();
    cooked.values.resize(count);
    cooked.keep.resize(count);
    cooked.prototype.resize(count);
    cooked.instanceIndices.resize(size_t(input.numPrototypes));

    for (size_t i = 0; i < count; ++i) {
        UsdGenAttributeInstanceRoot const &root = input.roots[i];
        float value = input.defaultValue;
        bool const sampled =
            input.map->Sample(root.face, root.u, root.v, input.channel,
                              UsdGenAttributeMapInterp::Bilinear, &value);
        if (!sampled) value = input.defaultValue;
        cooked.values[i] = value;
        bool const kept = sampled && value >= input.threshold;
        cooked.keep[i] = kept ? uint8_t(1) : uint8_t(0);
        if (!kept) {
            cooked.prototype[i] = -1;
            continue;
        }
        float const clamped = std::min(1.0f, std::max(0.0f, value));
        int slot = static_cast<int>(clamped * input.numPrototypes);
        if (slot >= input.numPrototypes) slot = input.numPrototypes - 1;
        cooked.prototype[i] = slot;
        cooked.instanceIndices[size_t(slot)].push_back(int(i));
        ++cooked.kept;
    }

    // FNV-1a over the map digest, the parameters and every assignment.
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&](uint64_t word) {
        for (int i = 0; i < 8; ++i) {
            hash ^= static_cast<uint64_t>(word & 0xff);
            hash *= 1099511628211ull;
            word >>= 8;
        }
    };
    mix(input.map->Digest());
    mix(uint64_t(input.channel));
    mix(uint64_t(input.numPrototypes));
    mix(uint64_t(count));
    uint32_t bits = 0;
    std::memcpy(&bits, &input.threshold, sizeof(bits));
    mix(bits);
    std::memcpy(&bits, &input.defaultValue, sizeof(bits));
    mix(bits);
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(&bits, &cooked.values[i], sizeof(bits));
        mix(bits);
        mix(uint64_t(cooked.keep[i]));
        mix(uint64_t(uint32_t(cooked.prototype[i])));
    }
    cooked.digest = hash;

    *result = std::move(cooked);
    return true;
}

}  // namespace usdGen
