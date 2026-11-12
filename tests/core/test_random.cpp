#include <doctest/doctest.h>

#include <array>

#include "crowdsim/core/Random.hpp"

using crowdsim::core::Pcg32;

TEST_CASE("Pcg32 matches the reference PCG32 output") {
    // Reference values from pcg32-demo (seed 42, stream 54).
    Pcg32 rng(42u, 54u);
    constexpr std::array<std::uint32_t, 6> expected{0xa15c02b7, 0x7b47f409, 0xba1d3330,
                                                    0x83d2f293, 0xbfa4784b, 0xcbed606e};
    for (std::uint32_t e : expected) {
        CHECK(rng.next() == e);
    }
}

TEST_CASE("Pcg32 is reproducible from a seed and reseedable") {
    Pcg32 a(123), b(123);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(a.next() == b.next());
    }
    a.reseed(123);
    Pcg32 c(123);
    CHECK(a.next() == c.next());
}

TEST_CASE("Pcg32 ranges stay in bounds") {
    Pcg32 rng(9);
    for (int i = 0; i < 10000; ++i) {
        const float f = rng.nextFloat();
        REQUIRE(f >= 0.0f);
        REQUIRE(f < 1.0f);
        REQUIRE(rng.below(7) < 7u);
        const float r = rng.range(-2.0f, 3.0f);
        REQUIRE(r >= -2.0f);
        REQUIRE(r < 3.0f);
    }
}

TEST_CASE("mixSeed decorrelates neighbouring seeds") {
    CHECK(crowdsim::core::mixSeed(1) != crowdsim::core::mixSeed(2));
    static_assert(crowdsim::core::mixSeed(5) == crowdsim::core::mixSeed(5));
}
