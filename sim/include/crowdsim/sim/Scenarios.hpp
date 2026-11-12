#pragma once

/// @file Scenarios.hpp
/// Built-in test scenes. Replaced by scene assets in M3; kept afterwards for
/// benchmarks and tests.

#include <cstdint>
#include <optional>
#include <string_view>

namespace crowdsim::sim {

class Simulation;

enum class ScenarioKind {
    Stream,       ///< one spawner streaming agents to one goal 120 m away
    Counterflow,  ///< two opposing streams that pass through each other
    Benchmark,    ///< dense counterflow that spawns `agents` almost at once
    Building,     ///< two-floor building: stairs, ramp, ladder link, walls, pillars
    Plaza,        ///< open plaza: commuters, wanderers, queuers at kiosks, groups
};

/// Define the standard behaviour trees and agent types (commuter, wanderer,
/// queuer, follower, evacuee) in `sim`. The content/ files mirror these and
/// replace them when loaded (hot reload).
void defineStandardAgentTypes(Simulation& sim);

[[nodiscard]] std::optional<ScenarioKind> parseScenario(std::string_view name);
[[nodiscard]] std::string_view scenarioName(ScenarioKind kind);

/// Clear `sim` and set up `kind` with `agents` total agents.
void buildScenario(Simulation& sim, ScenarioKind kind, std::uint32_t agents);

}  // namespace crowdsim::sim
