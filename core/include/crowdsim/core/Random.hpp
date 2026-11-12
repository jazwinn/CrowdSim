#pragma once

/// @file Random.hpp
/// Deterministic, seedable random number generation. Every RNG is an explicit
/// object (owned by the sim world or a caller); there is no global RNG.

#include <cstdint>

namespace crowdsim::core {

/// PCG32 (O'Neill, pcg-random.org, XSH-RR variant). Small state, fast, and
/// bit-identical across platforms and compilers, which the determinism test relies on.
class Pcg32 {
public:
    constexpr explicit Pcg32(std::uint64_t seed = 0x853c49e6748fea9bULL,
                             std::uint64_t stream = 0xda3e39cb94b95bdbULL) noexcept {
        reseed(seed, stream);
    }

    constexpr void reseed(std::uint64_t seed, std::uint64_t stream = 0xda3e39cb94b95bdbULL) noexcept {
        state_ = 0;
        inc_ = (stream << 1u) | 1u;
        next();
        state_ += seed;
        next();
    }

    /// Uniform 32-bit value.
    constexpr std::uint32_t next() noexcept {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    /// Uniform float in [0, 1). Uses the top 24 bits so the result is exact.
    constexpr float nextFloat() noexcept { return static_cast<float>(next() >> 8u) * (1.0f / 16777216.0f); }

    /// Uniform float in [lo, hi).
    constexpr float range(float lo, float hi) noexcept { return lo + (hi - lo) * nextFloat(); }

    /// Uniform integer in [0, bound) without modulo bias. `bound` must be > 0.
    constexpr std::uint32_t below(std::uint32_t bound) noexcept {
        const std::uint32_t threshold = (~bound + 1u) % bound;
        for (;;) {
            const std::uint32_t r = next();
            if (r >= threshold) {
                return r % bound;
            }
        }
    }

    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return state_; }

private:
    std::uint64_t state_ = 0;
    std::uint64_t inc_ = 0;
};

/// SplitMix64 finalizer: derive well-mixed sub-seeds (e.g. per entity, per spawner)
/// from a world seed without sharing generator state between them.
[[nodiscard]] constexpr std::uint64_t mixSeed(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27u)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31u);
}

}  // namespace crowdsim::core
