#pragma once

/// @file Simulation.hpp
/// The simulation world. Owns agents (SoA pool), environment entities (EnTT
/// registry: static objects, spawners, goals), the navigation mesh and flow
/// fields, the world RNG and the fixed-step counter. Deterministic: the same
/// scene, seed and inputs give bit-identical state on the same build,
/// regardless of thread count.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <entt/entity/registry.hpp>

#include "crowdsim/core/Random.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/geometry/TriangleMesh.hpp"
#include "crowdsim/sim/AgentPool.hpp"
#include "crowdsim/sim/BehaviorTree.hpp"
#include "crowdsim/sim/Environment.hpp"
#include "crowdsim/sim/FlowField.hpp"
#include "crowdsim/sim/NavMesh.hpp"
#include "crowdsim/sim/ScriptHooks.hpp"
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
    double decisionHz = 10.0;  ///< behaviour-tree tick rate per agent (staggered across agents)
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
    std::uint64_t arrived = 0;  ///< goal arrivals (MoveToGoal / Queue completions)
    std::uint64_t despawned = 0;
    float averageSpeed = 0.0f;
};

/// Wall time of each phase of the last step, in milliseconds (always measured;
/// a dozen clock reads per step).
struct StepProfile {
    float navigation = 0.0f;  ///< navmesh / flow field rebuilds (usually 0)
    float hash = 0.0f;        ///< spatial hash builds
    float spawn = 0.0f;
    float decisions = 0.0f;  ///< behaviour trees (parallel) + script nodes (serial)
    float resolve = 0.0f;    ///< queues, arrivals (serial)
    float preferred = 0.0f;  ///< preferred velocities: flow fields, corridors (parallel)
    float avoidance = 0.0f;  ///< neighbours + ORCA (parallel)
    float integrate = 0.0f;  ///< movement on the navmesh (parallel)
    float despawn = 0.0f;
    float observers = 0.0f;  ///< scripts, metrics
    float total = 0.0f;
};

/// Read-only view of one agent's decision state, for the BT debugger.
struct AgentDebugInfo {
    std::uint32_t id = 0;
    std::uint32_t index = 0;
    std::string typeName;
    std::string treeName;
    std::string stateName;
    std::string goalName;
    const CompiledTree* tree = nullptr;
    BtState bt;
    MoveMode moveMode = MoveMode::Stop;
    glm::vec3 moveTarget{0.0f};
    glm::vec3 position{0.0f};
    float speed = 0.0f;
    float speedMultiplier = 1.0f;
    float mass = 75.0f;  ///< kg
    std::uint8_t queueStage = 0;
    std::uint32_t groupId = kNoAgent;
};

/// An object footprint carrying free-form tags (danger, area:<name>, ...).
struct TaggedVolume {
    std::uint16_t tag = 0;
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

/// A moving staircase or walkway, built from a stairs or ramp object tagged
/// "escalator" (see docs/formats.md). Agents on it are carried at belt speed; how
/// each one rides is behaviour (EscalatorRider, set from behaviour trees).
struct Escalator {
    SceneId object = kNoSceneId;
    glm::vec2 entry{0.0f};       ///< centre of the boarding edge (x, z)
    glm::vec2 forward{0.0f};     ///< unit direction of travel (x, z)
    glm::vec2 right{0.0f};       ///< unit right-hand side when facing `forward`
    float length = 0.0f;         ///< horizontal length, m
    float halfWidth = 0.0f;      ///< m
    float entryHeight = 0.0f;    ///< floor height at the boarding edge
    float exitHeight = 0.0f;     ///< floor height at the far edge
    float beltSpeed = 0.5f;      ///< horizontal component of the step speed, m/s
    float laneOffset = 0.3f;     ///< lane centre distance from the middle, m
};

/// A piece of static environment geometry (ECS component). Either a parametric
/// primitive, or (primitive.kind == Mesh) an imported mesh placed by
/// primitive.position / rotationY with uniform scale primitive.size.x.
struct StaticObject {
    std::string name;
    geometry::PrimitiveDesc primitive;
    std::string meshAsset;                               ///< asset path for Mesh objects
    std::shared_ptr<const geometry::TriangleMesh> mesh;  ///< local-space geometry for Mesh objects
    std::vector<std::string> tags;  ///< free-form tags beyond walkable/obstacle (danger, area:<name>, ...)
};

class Simulation {
public:
    Simulation(SimServices services, const SimConfig& config);
    ~Simulation();

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // --- Scene editing (driven by the command layer) --------------------------------
    // Every scene entity has a stable SceneId. `add*` takes an explicit id (used when
    // loading scenes and undoing deletes) or kNoSceneId to allocate the next one, and
    // returns the id, or kNoSceneId if the requested id is already taken.
    SceneId addStaticObject(const StaticObject& object, SceneId id = kNoSceneId);
    SceneId addOffMeshLink(const OffMeshLink& link, SceneId id = kNoSceneId);
    SceneId addGoal(const Goal& goal, SceneId id = kNoSceneId);
    SceneId addSpawner(const Spawner& spawner, SceneId id = kNoSceneId);

    /// Replace an entity's description. Returns false if `id` is not of that kind.
    bool updateStaticObject(SceneId id, const StaticObject& object);
    bool updateOffMeshLink(SceneId id, const OffMeshLink& link);
    bool updateGoal(SceneId id, const Goal& goal);     ///< keeps the arrival counter
    bool updateSpawner(SceneId id, const Spawner& s);  ///< keeps spawn progress

    /// Remove one entity. Removing a goal also removes the agents heading to it.
    bool removeEntity(SceneId id);

    [[nodiscard]] const StaticObject* findStaticObject(SceneId id) const;
    [[nodiscard]] const OffMeshLink* findOffMeshLink(SceneId id) const;
    [[nodiscard]] const Goal* findGoal(SceneId id) const;
    [[nodiscard]] const Spawner* findSpawner(SceneId id) const;

    /// Ids in creation order.
    [[nodiscard]] std::vector<SceneId> staticObjectIds() const;
    [[nodiscard]] std::vector<SceneId> offMeshLinkIds() const;
    [[nodiscard]] std::vector<SceneId> goalIds() const;
    [[nodiscard]] std::vector<SceneId> spawnerIds() const;
    [[nodiscard]] SceneId nextSceneId() const noexcept { return nextSceneId_; }
    /// Never allocate ids below `next` (keeps ids of deleted entities retired across save/load).
    void reserveSceneIds(SceneId next) noexcept { nextSceneId_ = std::max(nextSceneId_, next); }

    /// Navmesh build settings (marks the navmesh dirty).
    void setNavigationSettings(const NavBuildSettings& settings);
    /// Locomotion tuning and seed (the seed takes effect at the next reset()).
    void setLocomotionParams(const LocomotionParams& params);
    void setSeed(std::uint64_t seed) noexcept { config_.seed = seed; }

    // --- Behaviours and agent types ---------------------------------------------------
    /// Add or replace a compiled tree (matched by name). Replacing bumps its
    /// generation so running agents restart it from the root (hot reload).
    std::uint16_t setBehaviorTree(CompiledTree tree);
    [[nodiscard]] const CompiledTree* findBehaviorTree(std::string_view name) const;
    [[nodiscard]] const std::vector<CompiledTree>& behaviorTrees() const noexcept { return trees_; }
    /// Add or replace an agent type (matched by name).
    std::uint16_t defineAgentType(const AgentType& type);
    [[nodiscard]] const AgentType* findAgentType(std::string_view name) const;
    [[nodiscard]] const std::vector<AgentType>& agentTypes() const noexcept { return agentTypes_; }
    /// Escalators found in the scene (objects tagged "escalator"), rebuilt with the navmesh.
    [[nodiscard]] const std::vector<Escalator>& escalators() const noexcept { return escalators_; }
    [[nodiscard]] const std::vector<std::string>& stateNames() const noexcept { return stateNames_; }

    // --- Scripting hooks (see ScriptHooks.hpp) ------------------------------------------
    /// Add or replace (by name) a script-defined BT node type; returns its index.
    /// Trees must be (re)compiled with customNodeSpecs() to use it.
    std::uint16_t defineCustomNode(const CustomNodeSpec& spec);
    [[nodiscard]] const std::vector<CustomNodeSpec>& customNodeSpecs() const noexcept { return customNodes_; }
    /// Non-owning; null disables script nodes (they then fail).
    void setCustomNodeRunner(CustomNodeRunner* runner) noexcept { customRunner_ = runner; }
    void addStepObserver(StepObserver* observer);
    void removeStepObserver(StepObserver* observer);
    /// Events recorded during the last step (spawns, arrivals, despawns).
    [[nodiscard]] const std::vector<SimEvent>& stepEvents() const noexcept { return events_; }

    // Script control API: valid only from serial callbacks (CustomNodeRunner,
    // StepObserver). `index` is an agent pool index.
    void scriptMoveTo(std::uint32_t index, glm::vec3 target);
    void scriptMoveToGoal(std::uint32_t index);
    void scriptStop(std::uint32_t index);
    void scriptSetSpeed(std::uint32_t index, float multiplier);
    void scriptSetState(std::uint32_t index, std::string_view state);
    /// Next value of the agent's own random stream in [0, 1) (deterministic).
    float scriptRandom(std::uint32_t index);
    /// Agents within `radius` of `p` (ground plane, same floor), up to `limit`.
    [[nodiscard]] std::uint32_t countAgentsNear(glm::vec3 p, float radius) const;
    /// Decision state of one agent by id (nullopt if gone).
    [[nodiscard]] std::optional<AgentDebugInfo> agentDebug(std::uint32_t agentId) const;
    /// Agent index for an id, or kNoAgent.
    [[nodiscard]] std::uint32_t agentIndexOf(std::uint32_t agentId) const noexcept;

    /// World-space triangles of one static object.
    [[nodiscard]] geometry::TriangleMesh objectMesh(SceneId id) const;

    /// Remove all agents, static objects, links, spawners and goals.
    void clearScene();

    /// Rebuild navmesh and flow fields now if the scene changed (otherwise this
    /// happens lazily at the next step). Returns true if anything was rebuilt.
    bool updateNavigation();

    // --- Stepping ---------------------------------------------------------------------
    /// Advance exactly one fixed step. Wall-clock time is only read to fill
    /// lastStepProfile(); it never influences the simulation.
    void step();
    [[nodiscard]] const StepProfile& lastStepProfile() const noexcept { return profile_; }

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
    void runCustomNodes();
    void reorderAgents();
    void spawnAgents(float dt);
    void computePreferredVelocities();
    void computeAvoidance(float dt);
    void integrate(float dt);
    void addAgent(const AgentInit& init);
    void removeAgentAt(std::uint32_t index);
    void tickBehaviors();
    void resolveDecisions();
    void processDespawns();
    BtStatus tickNode(const CompiledTree& tree, std::uint16_t node, std::uint32_t agent, std::uint32_t thread);
    void resetSubtree(const CompiledTree& tree, std::uint16_t node, BtState& state) const;
    void setPointTarget(std::uint32_t agent, glm::vec3 target);
    bool planCorridor(std::uint32_t agent, std::uint32_t thread);
    glm::vec3 corridorSteeringTarget(std::uint32_t agent, std::uint32_t thread);
    [[nodiscard]] std::uint16_t internName(std::vector<std::string>& table, std::string_view name);
    void rebuildTreeLookups();
    void rebuildTaggedVolumes();
    void rebuildEscalators();
    [[nodiscard]] static std::int8_t escalatorSideOf(std::string_view side);
    void assignEscalatorLanes();
    [[nodiscard]] glm::vec2 escalatorSteering(std::uint32_t agent, glm::vec2 pref) const;
    [[nodiscard]] glm::vec2 beltVelocityAt(glm::vec2 position, float height) const;
    /// On the steps, or within `clear` m of stepping off.
    [[nodiscard]] bool onEscalator(glm::vec2 position, float height, float clear) const;
    /// Queue manners (QueueManners): slow `pref` behind whoever is ahead in the agent's line.
    [[nodiscard]] glm::vec2 applyQueueManners(std::uint32_t agent, glm::vec2 pref) const;
    /// The escalator an agent is lining up for (holding a lane, before the steps), or null;
    /// `atFoot` is set when it is inside the boarding window, where boarding rules apply.
    [[nodiscard]] const Escalator* queueingFor(std::uint32_t agent, bool& atFoot) const;
    [[nodiscard]] std::uint16_t resolveTree(const std::string& name) const;
    [[nodiscard]] std::shared_ptr<const snapshot::StaticScene> buildStaticScene() const;
    SceneId claimId(SceneId requested);
    entt::entity createEntity(SceneId id);
    [[nodiscard]] entt::entity entityOf(SceneId id) const;
    template <typename T>
    [[nodiscard]] std::vector<SceneId> idsOf(const std::vector<entt::entity>& list) const;
    [[nodiscard]] std::uint32_t goalIndexOf(SceneId goal) const;
    void removeAgentsOfGoal(std::uint32_t goalIndex);
    void retargetAgentsOfGoal(std::uint32_t goalIndex);

    SimServices services_;
    SimConfig config_;
    double stepSeconds_;
    std::uint64_t stepIndex_ = 0;
    core::Pcg32 rng_;

    entt::registry registry_;
    std::vector<entt::entity> staticObjects_;
    std::vector<entt::entity> offMeshLinks_;
    std::unordered_map<SceneId, entt::entity> idToEntity_;
    SceneId nextSceneId_ = 1;
    std::vector<entt::entity> goals_;     ///< creation order = goal index
    std::vector<entt::entity> spawners_;  ///< creation order = processing order
    std::vector<Area> goalAreas_;         ///< per-step copy for the hot loops
    std::vector<Goal> goalsCache_;        ///< per-step copy of goal settings for the decision layer

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
    std::uint64_t despawnedTotal_ = 0;
    std::vector<std::uint32_t> idToIndex_;  ///< agent id -> pool index (kNoAgent when gone)

    // Decision layer
    std::vector<CompiledTree> trees_;
    struct TreeLookup {
        std::vector<std::uint16_t> stateOf;  ///< string index -> state id
        std::vector<std::uint16_t> tagOf;    ///< string index -> tag id
        std::vector<std::int32_t> goalOf;    ///< string index -> goal index (-1 = assigned goal)
    };
    std::vector<TreeLookup> treeLookups_;
    std::vector<AgentType> agentTypes_;
    std::vector<std::string> stateNames_{"walking"};
    std::vector<CustomNodeSpec> customNodes_;
    CustomNodeRunner* customRunner_ = nullptr;
    struct CustomRequest {
        std::uint32_t agent;
        std::uint16_t tree;
        std::uint16_t node;
    };
    std::vector<std::vector<CustomRequest>> customRequests_;  ///< per worker thread
    std::vector<StepObserver*> stepObservers_;
    std::vector<SimEvent> events_;
    StepProfile profile_;
    std::vector<std::string> tagNames_{""};
    std::vector<TaggedVolume> taggedVolumes_;
    std::vector<Escalator> escalators_;
};

}  // namespace crowdsim::sim
