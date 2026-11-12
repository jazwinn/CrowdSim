#include <doctest/doctest.h>

#include "crowdsim/core/FixedStepClock.hpp"

using crowdsim::core::FixedStepClock;

TEST_CASE("FixedStepClock emits whole steps and keeps the remainder") {
    FixedStepClock clock(10.0);  // 0.1 s steps
    CHECK(clock.advance(0.05) == 0);
    CHECK(clock.alpha() == doctest::Approx(0.5));
    CHECK(clock.advance(0.06) == 1);
    CHECK(clock.alpha() == doctest::Approx(0.1).epsilon(1e-6));
    CHECK(clock.advance(0.25) == 2);
}

TEST_CASE("FixedStepClock caps catch-up steps") {
    FixedStepClock clock(30.0, 4);
    CHECK(clock.advance(10.0) == 4);
    CHECK(clock.alpha() == doctest::Approx(0.0));
}

TEST_CASE("FixedStepClock ignores negative time") {
    FixedStepClock clock(30.0);
    CHECK(clock.advance(-1.0) == 0);
    CHECK(clock.alpha() == doctest::Approx(0.0));
}
