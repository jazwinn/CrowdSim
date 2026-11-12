#include <doctest/doctest.h>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/sim/Simulation.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

using namespace crowdsim;

namespace {

std::uint64_t runAndHash(std::uint64_t seed, int steps, std::uint32_t threads,
                         sim::ScenarioKind kind = sim::ScenarioKind::Counterflow) {
    core::LogService logs(core::LogConfig{.console = false});
    core::JobSystem jobs(threads);
    sim::Simulation simulation({logs.logger(), jobs}, sim::SimConfig{.seed = seed});
    sim::buildScenario(simulation, kind, 1500);
    for (int i = 0; i < steps; ++i) {
        simulation.step();
    }
    return simulation.stateHash();
}

}  // namespace

TEST_CASE("Simulation with agents is deterministic for a given seed") {
    CHECK(runAndHash(11, 300, 4) == runAndHash(11, 300, 4));
    CHECK(runAndHash(11, 300, 4) != runAndHash(12, 300, 4));
}

TEST_CASE("Simulation result does not depend on the number of threads") {
    CHECK(runAndHash(3, 300, 1) == runAndHash(3, 300, 7));
}

TEST_CASE("Simulation reset replays identically") {
    core::LogService logs(core::LogConfig{.console = false});
    core::JobSystem jobs(2);
    sim::Simulation simulation({logs.logger(), jobs}, sim::SimConfig{.seed = 3});
    sim::buildScenario(simulation, sim::ScenarioKind::Stream, 800);
    for (int i = 0; i < 200; ++i) {
        simulation.step();
    }
    const auto first = simulation.stateHash();
    CHECK(simulation.agents().size() > 0);
    simulation.reset();
    CHECK(simulation.agents().size() == 0);
    for (int i = 0; i < 200; ++i) {
        simulation.step();
    }
    CHECK(simulation.stateHash() == first);
}

TEST_CASE("Spawner respects its total and agents arrive at the goal") {
    core::LogService logs(core::LogConfig{.console = false});
    core::JobSystem jobs(4);
    sim::Simulation simulation({logs.logger(), jobs}, sim::SimConfig{.seed = 1});

    const std::uint32_t goal = simulation.addGoal(sim::Goal{.name = "g", .area = {{0.0f, 15.0f}, {5.0f, 1.0f}}});
    sim::Spawner spawner;
    spawner.area = {{0.0f, -15.0f}, {5.0f, 2.0f}};
    spawner.rate = 20.0f;
    spawner.total = 100;
    spawner.goal = goal;
    simulation.addSpawner(spawner);

    // 100 agents at 20/s spawn within ~5 s; a 30 m walk at >= 1.15 m/s takes < 30 s.
    for (int i = 0; i < 60 * 30; ++i) {
        simulation.step();
    }
    const sim::SimStats stats = simulation.stats();
    CHECK(stats.spawned == 100);
    CHECK(stats.arrived == 100);
    CHECK(stats.alive == 0);
}

TEST_CASE("Snapshot mirrors the agent pool") {
    core::LogService logs(core::LogConfig{.console = false});
    core::JobSystem jobs(2);
    sim::Simulation simulation({logs.logger(), jobs}, sim::SimConfig{.seed = 4});
    sim::buildScenario(simulation, sim::ScenarioKind::Stream, 300);
    for (int i = 0; i < 90; ++i) {
        simulation.step();
    }
    snapshot::RenderSnapshot snap;
    simulation.writeSnapshot(snap);
    REQUIRE(snap.agents.size() == simulation.agents().size());
    CHECK(snap.stepIndex == simulation.stepIndex());
    CHECK(snap.areas.size() == 2);  // one spawner + one goal
    const auto& pool = simulation.agents();
    for (std::uint32_t i = 0; i < pool.size(); ++i) {
        CHECK(snap.agents[i].position.x == pool.position[i].x);
        CHECK(snap.agents[i].position.z == pool.position[i].y);
        CHECK(snap.agents[i].agentId == pool.id[i]);
    }
}
