#include "crowdsim/sim/Scenarios.hpp"

#include <algorithm>

#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::sim {

std::optional<ScenarioKind> parseScenario(std::string_view name) {
    if (name == "stream")
        return ScenarioKind::Stream;
    if (name == "counterflow")
        return ScenarioKind::Counterflow;
    if (name == "benchmark")
        return ScenarioKind::Benchmark;
    return std::nullopt;
}

std::string_view scenarioName(ScenarioKind kind) {
    switch (kind) {
        case ScenarioKind::Stream:
            return "stream";
        case ScenarioKind::Counterflow:
            return "counterflow";
        case ScenarioKind::Benchmark:
            return "benchmark";
    }
    return "unknown";
}

void buildScenario(Simulation& sim, ScenarioKind kind, std::uint32_t agents) {
    sim.clearScene();

    switch (kind) {
        case ScenarioKind::Stream: {
            // All agents spawn within ~40 s; the walk takes ~90 s, so the full
            // population is alive at once around t = 40..85 s.
            const std::uint32_t goal = sim.addGoal(Goal{.name = "Exit", .area = {{0.0f, 60.0f}, {30.0f, 3.0f}}});
            Spawner s;
            s.name = "Entrance";
            s.area = {{0.0f, -60.0f}, {30.0f, 6.0f}};
            s.rate = std::max(1.0f, static_cast<float>(agents) / 40.0f);
            s.total = agents;
            s.goal = goal;
            sim.addSpawner(s);
            break;
        }
        case ScenarioKind::Counterflow:
        case ScenarioKind::Benchmark: {
            const bool burst = kind == ScenarioKind::Benchmark;
            // Benchmark: ~1.5 m^2 per agent so even 50k agents fit in the two spawn areas.
            const float halfWidth = burst ? std::max(60.0f, 0.0075f * static_cast<float>(agents)) : 30.0f;
            const float spawnDepth = burst ? 25.0f : 6.0f;
            const float rate = burst ? static_cast<float>(agents) : std::max(1.0f, static_cast<float>(agents) / 80.0f);
            const std::uint32_t north = sim.addGoal(Goal{.name = "North", .area = {{0.0f, 70.0f}, {halfWidth, 3.0f}}});
            const std::uint32_t south = sim.addGoal(Goal{.name = "South", .area = {{0.0f, -70.0f}, {halfWidth, 3.0f}}});

            Spawner a;
            a.name = "South entrance";
            a.area = {{0.0f, -55.0f + spawnDepth * 0.5f}, {halfWidth, spawnDepth}};
            a.rate = rate;
            a.total = agents / 2;
            a.goal = north;
            a.agent.color = {0.9f, 0.55f, 0.25f};
            sim.addSpawner(a);

            Spawner b = a;
            b.name = "North entrance";
            b.area = {{0.0f, 55.0f - spawnDepth * 0.5f}, {halfWidth, spawnDepth}};
            b.total = agents - agents / 2;
            b.goal = south;
            b.agent.color = {0.3f, 0.6f, 0.95f};
            b.agent.agentType = 1;
            sim.addSpawner(b);
            break;
        }
    }
}

}  // namespace crowdsim::sim
