#pragma once

/// @file Simulation.hpp
/// The simulation world. Owns agents (SoA pool), environment entities (EnTT
/// registry: static objects, spawners, goals), the navigation mesh and flow
/// fields, the world RNG and the fixed-step counter. Deterministic: the same
/// scene, seed and inputs give bit-identical state on the same build,
/// regardless of thread count.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>

#include "crowdsim/core/Random.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/geometry/TriangleMesh.hpp"
#include "crowdsim/sim/AgentPool.hpp"
#include "crowdsim/sim/Environment.hpp"
#include "crowdsim/sim/FlowField.hpp"
#include "crowdsim/sim/NavMesh.hpp"
#include "crowdsim/sim/SpatialHash.hpp"

namespace spdlog {
class logger;
}

namespace crowdsim::core {
class JobSystem;
}

namespace crowdsim::snapshot {
struct RenderSnapshot;
struct StaticScene;
}  // namespace crowdsim::snapshot

namespace crowdsim::sim {

/// Locomotion-layer tuning shared by all agents.
struct LocomotionParams {
    float neighborDistance = 2.5f;     ///< m; also the spatial-hash cell size
    std::uint32_t maxNeighbors = 10;   ///< clamped to orca::kMaxNeighbors
    float timeHorizon = 2.0f;          ///< s; ORCA look-ahead for agents
    float obstacleTimeHorizon = 0.5f;  ///< s; ORCA look-ahead for walls
    float personalSpace = 0.06f;       ///< m added to each radius during avoidance
    float maxTurnRate = 6.0f;          ///< rad/s for the facing direction
    float levelSeparation = 1.0f;      ///< m; agents further apart vertically ignore each other
};

struct SimConfig {
    std::uint64_t seed = 1;
    double stepHz = 30.0;
    LocomotionParams locomotion;
    NavBuildSettings navigation;
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

/// A piece of static environment geometry (ECS component).
struct StaticObject {
    std::string name;
    geometry::PrimitiveDesc primitive;
};

class Simulation {
public:
    Simulation(SimServices services, const SimConfig& config);
    ~Simulation();

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // --- Environment setup (becomes commands in M3) ---------------------------------
    std::uint32_t addStaticObject(const StaticObject& object);
    void addOffMeshLink(const OffMeshLink& link);
    /// Returns the goal index used by Spawner::goal.
    std::uint32_t addGoal(const Goal& goal);
    std::uint32_t addSpawner(const Spawner& spawner);
    /// Remove all agents, static objects, links, spawners and goals.
    void clearScene();

    /// Rebuild navmesh and flow fields now if the scene changed (otherwise this
    /// happens lazily at the next step). Returns true if anything was rebuilt.
    bool updateNavigation();

    // --- Stepping ---------------------------------------------------------------------
    /// Advance exactly one fixed step. No wall-clock time is read in here.
    void step();

    /// Back to t = 0 with the configured seed: agents removed, scene kept with
    /// spawner and goal runtime counters cleared.
    void reset();

    // --- Queries ----------------------------------------------------------------------
    [[nodiscard]] std::uint64_t stepIndex() const noexcept { return stepIndex_; }
    [[nodiscard]] double simTimeSeconds() const noexcept { return static_cast<double>(stepIndex_) * stepSeconds_; }
    [[nodiscard]] double stepSeconds() const noexcept { return stepSeconds_; }
    [[nodiscard]] const SimConfig& config() const noexcept { return config_; }
    [[nodiscard]] const AgentPool& agents() const noexcept { return agents_; }
    [[nodiscard]] SimStats stats() const noexcept;
    [[nodiscard]] const NavMesh& navMesh() const noexcept { return navMesh_; }
    [[nodiscard]] const std::vector<FlowField>& flowFields() const noexcept { return flowFields_; }
    /// Combined static geometry (valid after updateNavigation / the first step).
    [[nodiscard]] const geometry::TriangleMesh& staticMesh() const noexcept { return staticMesh_; }
    /// Increments whenever static geometry, navmesh or flow fields change.
    [[nodiscard]] std::uint64_t staticVersion() const noexcept { return staticVersion_; }

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
    [[nodiscard]] std::shared_ptr<const snapshot::StaticScene> buildStaticScene() const;

    SimServices services_;
    SimConfig config_;
    double stepSeconds_;
    std::uint64_t stepIndex_ = 0;
    core::Pcg32 rng_;

    entt::registry registry_;
    std::vector<entt::entity> staticObjects_;
    std::vector<OffMeshLink> offMeshLinks_;
    std::vector<entt::entity> goals_;     ///< creation order = goal index
    std::vector<entt::entity> spawners_;  ///< creation order = processing order
    std::vector<Area> goalAreas_;         ///< per-step copy for the hot loops

    // Navigation (derived from the scene; rebuilt when dirty)
    geometry::TriangleMesh staticMesh_;
    NavMesh navMesh_;
    std::vector<FlowField> flowFields_;
    bool navDirty_ = true;
    bool flowDirty_ = true;
    std::uint64_t staticVersion_ = 0;
    mutable std::shared_ptr<const snapshot::StaticScene> staticSceneCache_;

    AgentPool agents_;
    SpatialHash hash_;
    std::uint32_t nextAgentId_ = 0;
    std::uint64_t spawnedTotal_ = 0;
    std::uint64_t arrivedTotal_ = 0;
};

}  // namespace crowdsim::sim
