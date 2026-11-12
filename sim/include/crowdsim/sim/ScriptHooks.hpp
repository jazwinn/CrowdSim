#pragma once

/// @file ScriptHooks.hpp
/// Extension points the scripting layer plugs into, without the simulation
/// knowing anything about Lua.
///
/// * Script BT nodes. A `Custom` node never runs script code inside the
///   parallel decision tick. Reaching one records a request and returns
///   Running; after the tick the simulation hands all requests, grouped per
///   node instance and in agent order, to the CustomNodeRunner (serially). The
///   result is stored in the node's memory and returned on the agent's next
///   decision tick. Script nodes therefore cost one decision tick of latency
///   and stay deterministic for any thread count.
/// * Step observers run serially at the end of every step (entity scripts,
///   event handlers, metrics) and may read events recorded during the step.

#include <cstdint>
#include <span>

namespace crowdsim::sim {

class Simulation;
struct CompiledTree;

enum class CustomResult : std::uint8_t { Running = 0, Success = 1, Failure = 2 };

/// All agents that reached one script node instance in this step.
struct CustomNodeBatch {
    std::uint16_t customIndex = 0;  ///< index into Simulation::customNodeSpecs()
    const CompiledTree* tree = nullptr;
    std::uint16_t node = 0;                 ///< node index in `tree` (its params: tree->customParams)
    std::span<const std::uint32_t> agents;  ///< agent pool indices, ascending
    std::span<CustomResult> results;        ///< one per agent, preset to Running
};

class CustomNodeRunner {
public:
    virtual ~CustomNodeRunner() = default;
    /// Called once per step that has requests, between the decision tick and
    /// locomotion. May use Simulation's script control API (scriptMoveTo, ...).
    virtual void runCustomNodes(Simulation& sim, std::span<const CustomNodeBatch> batches) = 0;
};

enum class SimEventType : std::uint8_t { AgentSpawned, AgentArrived, AgentDespawned };

struct SimEvent {
    SimEventType type = SimEventType::AgentSpawned;
    std::uint32_t agentId = 0;
    std::int32_t goalIndex = -1;  ///< arrivals: goal reached
};

class StepObserver {
public:
    virtual ~StepObserver() = default;
    /// After every step, serially. `sim.stepEvents()` holds this step's events.
    virtual void afterStep(Simulation& sim) = 0;
};

}  // namespace crowdsim::sim
