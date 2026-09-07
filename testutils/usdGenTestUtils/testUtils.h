// usdGenTestUtils — Tier-0 helpers. No Hydra, no USD (gate B-1).
#ifndef USDGEN_TESTUTILS_H
#define USDGEN_TESTUTILS_H

#include <cstdint>

namespace usdGenTest {

// Deterministic LCG; stable across thread counts (E-8 dependency).
class DeterministicRng {
public:
    explicit DeterministicRng(uint64_t seed) : _s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t NextUInt64() {
        _s ^= _s << 13; _s ^= _s >> 7; _s ^= _s << 17; return _s;
    }
    // High 24 bits -> [0,1).
    float NextUnit() { return static_cast<float>(NextUInt64() >> 40) * (1.0f / 16777216.0f); }
private:
    uint64_t _s;
};

// A trivial T0 assertion helper.
int Check(bool ok, const char *what);
int ReportChecks(const char *suite, int pass, int fail);

}  // namespace usdGenTest

#endif  // USDGEN_TESTUTILS_H
