#include <doctest/doctest.h>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/sim/Simulation.hpp"

using namespace crowdsim;

namespace {

std::uint64_t runAndHash(std::uint64_t seed, int steps) {
    core::LogService logs(core::LogConfig{.console = false});
    core::JobSystem jobs(2);
    sim::Simulation simulation({logs.logger(), jobs}, sim::SimConfig{.seed = seed});
    for (int i = 0; i < steps; ++i) {
        simulation.step();
    }
    return simulation.stateHash();
}

}  // namespace

TEST_CASE("Simulation is deterministic for a given seed") {
    CHECK(runAndHash(11, 200) == runAndHash(11, 200));
    CHECK(runAndHash(11, 200) != runAndHash(12, 200));
}

TEST_CASE("Simulation reset returns to the initial state") {
    core::LogService logs(core::LogConfig{.console = false});
    core::JobSystem jobs(1);
    sim::Simulation simulation({logs.logger(), jobs}, sim::SimConfig{.seed = 3});
    const auto initial = simulation.stateHash();
    for (int i = 0; i < 10; ++i) {
        simulation.step();
    }
    CHECK(simulation.stateHash() != initial);
    CHECK(simulation.simTimeSeconds() == doctest::Approx(10.0 / 30.0));
    simulation.reset();
    CHECK(simulation.stateHash() == initial);
}
