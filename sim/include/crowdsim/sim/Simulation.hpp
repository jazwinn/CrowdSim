#pragma once

/// @file Simulation.hpp
/// The simulation world. Owns agents (SoA pool), environment entities (EnTT
/// registry: spawners, goals), the world RNG and the fixed-step counter.
/// Deterministic: the same scene, seed and inputs give bit-identical state on
/// the same build, regardless of thread count.

#include <cstdint>
#include <vector>

#include <entt/entity/registry.hpp>

#include "crowdsim/core/Random.hpp"
#include "crowdsim/sim/AgentPool.hpp"
#include "crowdsim/sim/Environment.hpp"
#include "crowdsim/sim/SpatialHash.hpp"

namespace spdlog {
class logger;
}

namespace crowdsim::core {
class JobSystem;
}

namespace crowdsim::snapshot {
struct RenderSnapshot;
}

namespace crowdsim::sim {

/// Locomotion-layer tuning shared by all agents.
struct LocomotionParams {
    float neighborDistance = 2.5f;    ///< m; also the spatial-hash cell size
    std::uint32_t maxNeighbors = 10;  ///< clamped to orca::kMaxNeighbors
    float timeHorizon = 2.0f;         ///< s; ORCA look-ahead
    float personalSpace = 0.06f;      ///< m added to each radius during avoidance
    float maxTurnRate = 6.0f;         ///< rad/s for the facing direction
};

struct SimConfig {
    std::uint64_t seed = 1;
    double stepHz = 30.0;
    LocomotionParams locomotion;
};

/// Services the simulation borrows from the composition root.
struct SimServices {
    spdlog::logger& log;
    core::JobSystem& jobs;
};

struct SimStats {
    std::uint32_t alive = 0;
    std::uint64_t spawned = 0;
    std::uint64_t arrived = 0;
    float averageSpeed = 0.0f;
};

class Simulation {
public:
    Simulation(SimServices services, const SimConfig& config);

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // --- Environment setup (becomes commands in M3) ---------------------------------
    /// Returns the goal index used by Spawner::goal.
    std::uint32_t addGoal(const Goal& goal);
    std::uint32_t addSpawner(const Spawner& spawner);
    /// Remove all agents, spawners and goals.
    void clearScene();

    // --- Stepping ---------------------------------------------------------------------
    /// Advance exactly one fixed step. No wall-clock time is read in here.
    void step();

    /// Back to t = 0 with the configured seed: agents removed, spawners and goals kept
    /// with their runtime counters cleared.
    void reset();

    // --- Queries ----------------------------------------------------------------------
    [[nodiscard]] std::uint64_t stepIndex() const noexcept { return stepIndex_; }
    [[nodiscard]] double simTimeSeconds() const noexcept { return static_cast<double>(stepIndex_) * stepSeconds_; }
    [[nodiscard]] double stepSeconds() const noexcept { return stepSeconds_; }
    [[nodiscard]] const SimConfig& config() const noexcept { return config_; }
    [[nodiscard]] const AgentPool& agents() const noexcept { return agents_; }
    [[nodiscard]] SimStats stats() const noexcept;

    /// Hash of all simulation state; equal hashes after equal inputs prove determinism.
    [[nodiscard]] std::uint64_t stateHash() const noexcept;

    /// Fill `out` with the current render state (positions at this and the previous step).
    void writeSnapshot(snapshot::RenderSnapshot& out) const;

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }

private:
    void spawnAgents(float dt);
    void computePreferredVelocities();
    void computeAvoidance(float dt);
    void integrate(float dt);
    void removeArrivedAgents();

    SimServices services_;
    SimConfig config_;
    double stepSeconds_;
    std::uint64_t stepIndex_ = 0;
    core::Pcg32 rng_;

    entt::registry registry_;
    std::vector<entt::entity> goals_;     ///< creation order = goal index
    std::vector<entt::entity> spawners_;  ///< creation order = processing order
    std::vector<Area> goalAreas_;         ///< per-step copy for the hot loops

    AgentPool agents_;
    SpatialHash hash_;
    std::uint32_t nextAgentId_ = 0;
    std::uint64_t spawnedTotal_ = 0;
    std::uint64_t arrivedTotal_ = 0;
};

}  // namespace crowdsim::sim
