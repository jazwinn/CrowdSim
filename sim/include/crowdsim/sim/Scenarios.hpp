#pragma once

/// @file Scenarios.hpp
/// Built-in test scenes on a flat plane. Replaced by scene assets in M3; kept
/// afterwards for benchmarks and tests.

#include <cstdint>
#include <optional>
#include <string_view>

namespace crowdsim::sim {

class Simulation;

enum class ScenarioKind {
    Stream,       ///< one spawner streaming agents to one goal 120 m away
    Counterflow,  ///< two opposing streams that pass through each other
    Benchmark,    ///< dense counterflow that spawns `agents` almost at once
};

[[nodiscard]] std::optional<ScenarioKind> parseScenario(std::string_view name);
[[nodiscard]] std::string_view scenarioName(ScenarioKind kind);

/// Clear `sim` and set up `kind` with `agents` total agents.
void buildScenario(Simulation& sim, ScenarioKind kind, std::uint32_t agents);

}  // namespace crowdsim::sim
