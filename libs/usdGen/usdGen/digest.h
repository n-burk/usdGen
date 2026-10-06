// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT
//
// usdGen — internal multi-lane digest for capture-class hashing.
//
// FNV-1a is one serial xor/multiply chain per byte, so a single lane runs
// at multiply latency (~2-4 cycles/byte). Four independent lanes over
// striped inputs let the CPU overlap four chains and run at ~4x the
// throughput; UsdGenDigestCombine4 folds the lanes order-sensitively.
//
// Contract: digests are INTERNAL cache keys, compared for equality only
// (scheduler capture epochs, cook digests). Values are NOT stable across
// usdGen versions and must never be persisted or golden-tested — only
// determinism (same bytes -> same digest in every process) and input
// sensitivity are guaranteed. Groom outputs are unaffected by the lane
// count. (Scatter epochs already vary per process through TfToken bits;
// this changes nothing about that.)
#ifndef USDGEN_DIGEST_H
#define USDGEN_DIGEST_H

#include <cstddef>
#include <cstdint>

namespace usdGen {

constexpr uint64_t UsdGenDigestOffset = 14695981039346656037ull;
constexpr uint64_t UsdGenDigestPrime = 1099511628211ull;

// One FNV-1a word feed, byte order preserved (low byte first).
inline void UsdGenDigestMixWord(uint64_t &h, uint64_t word)
{
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<uint64_t>(word & 0xffu);
        h *= UsdGenDigestPrime;
        word >>= 8;
    }
}

// Order-sensitive 4-lane fold with a splitmix-style final avalanche.
// Non-crypto, as FNV-1a itself is.
inline uint64_t UsdGenDigestCombine4(uint64_t h0, uint64_t h1, uint64_t h2,
                                     uint64_t h3)
{
    uint64_t h = UsdGenDigestOffset;
    h ^= h0 + 0x9E3779B97F4A7C15ull;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h1 + 0xBF58476D1CE4E5B9ull;
    h *= 0x94D049BB133111EBull;
    h ^= h2 + 0x94D049BB133111EBull;
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h3 + 0xD1B54A32D192ED03ull;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}

// 4-lane FNV-1a over raw bytes; byte i feeds lane (i & 3). Lanes start
// domain-separated from `seed` so empty/short inputs still mix.
inline uint64_t UsdGenDigestBytes(void const *data, size_t n, uint64_t seed)
{
    auto const *b = static_cast<unsigned char const *>(data);
    uint64_t h[4] = {seed, seed ^ 0x9E3779B97F4A7C15ull,
                     seed ^ 0xBF58476D1CE4E5B9ull,
                     seed ^ 0x94D049BB133111EBull};
    size_t i = 0;
    size_t const n4 = n & ~size_t(3);
    for (; i < n4; i += 4) {
        h[0] ^= uint64_t(b[i + 0]);
        h[0] *= UsdGenDigestPrime;
        h[1] ^= uint64_t(b[i + 1]);
        h[1] *= UsdGenDigestPrime;
        h[2] ^= uint64_t(b[i + 2]);
        h[2] *= UsdGenDigestPrime;
        h[3] ^= uint64_t(b[i + 3]);
        h[3] *= UsdGenDigestPrime;
    }
    for (; i < n; ++i) {
        h[i & 3] ^= uint64_t(b[i]);
        h[i & 3] *= UsdGenDigestPrime;
    }
    return UsdGenDigestCombine4(h[0], h[1], h[2], h[3]);
}

}  // namespace usdGen

#endif  // USDGEN_DIGEST_H
