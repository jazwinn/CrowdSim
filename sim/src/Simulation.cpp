#include "crowdsim/sim/Simulation.hpp"

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <span>

#include <spdlog/logger.h>
#include <glm/geometric.hpp>

#include "Funnel.hpp"
#include "crowdsim/core/Hash.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/sim/Orca.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

namespace crowdsim::sim {

namespace {

constexpr std::uint64_t kReorderInterval = 64;  ///< steps between spatial re-sorts of the agent pool

constexpr std::uint32_t kParallelChunk = 256;
constexpr std::uint32_t kSpawnAttempts = 8;
constexpr std::uint32_t kCorridorLength = 8;   ///< polygons of look-ahead for string pulling
constexpr float kWallQueryMargin = 1.0f;       ///< m beyond the agent radius
constexpr float kOffMeshTriggerRadius = 0.6f;  ///< m from a link start to begin traversing

float wrapAngle(float a) noexcept {
    constexpr float kPi = std::numbers::pi_v<float>;
    constexpr float kTwoPi = 2.0f * kPi;
    a = std::fmod(a + kPi, kTwoPi);
    if (a < 0.0f) {
        a += kTwoPi;
    }
    return a - kPi;
}

/// Heading convention: 0 faces +Z, positive turns towards +X.
float headingOf(glm::vec2 dir) noexcept {
    return std::atan2(dir.x, dir.y);
}

float distanceToSegment2d(glm::vec2 p, glm::vec2 a, glm::vec2 b) noexcept {
    const glm::vec2 ab = b - a;
    const float lenSq = glm::dot(ab, ab);
    const float t = lenSq > 1e-8f ? std::clamp(glm::dot(p - a, ab) / lenSq, 0.0f, 1.0f) : 0.0f;
    const glm::vec2 d = a + t * ab - p;
    return std::sqrt(glm::dot(d, d));
}

}  // namespace

Simulation::Simulation(SimServices services, const SimConfig& config)
    : services_(services), config_(config), stepSeconds_(1.0 / config.stepHz), rng_(config.seed) {
    config_.locomotion.maxNeighbors = std::min(config_.locomotion.maxNeighbors, orca::kMaxNeighbors);
    customRequests_.resize(std::max<std::uint32_t>(1, services_.jobs.threadCount()));
    // Built-in fallback behaviour: walk to the assigned goal, then leave.
    BtNodeDesc commuter{"Sequence", "root", {}, {}};
    commuter.children.push_back(BtNodeDesc{"MoveToGoal", "walk", {}, {}});
    commuter.children.push_back(BtNodeDesc{"Despawn", "leave", {}, {}});
    setBehaviorTree(compileBehaviorTree(commuter, "commuter"));
    AgentType fallback;
    fallback.name = "default";
    fallback.behaviour = "commuter";
    defineAgentType(fallback);
    services_.log.debug("Simulation created (seed={}, {} Hz)", config_.seed, config_.stepHz);
}

Simulation::~Simulation() = default;

SceneId Simulation::claimId(SceneId requested) {
    if (requested == kNoSceneId) {
        return nextSceneId_++;
    }
    if (idToEntity_.contains(requested)) {
        return kNoSceneId;
    }
    nextSceneId_ = std::max(nextSceneId_, requested + 1);
    return requested;
}

entt::entity Simulation::createEntity(SceneId id) {
    const entt::entity e = registry_.create();
    registry_.emplace<SceneIdComponent>(e, id);
    idToEntity_.emplace(id, e);
    return e;
}

entt::entity Simulation::entityOf(SceneId id) const {
    const auto it = idToEntity_.find(id);
    return it == idToEntity_.end() ? entt::null : it->second;
}

template <typename T>
std::vector<SceneId> Simulation::idsOf(const std::vector<entt::entity>& list) const {
    std::vector<SceneId> ids;
    ids.reserve(list.size());
    for (entt::entity e : list) {
        ids.push_back(registry_.get<SceneIdComponent>(e).id);
    }
    return ids;
}

std::uint32_t Simulation::goalIndexOf(SceneId goal) const {
    for (std::uint32_t i = 0; i < goals_.size(); ++i) {
        if (registry_.get<SceneIdComponent>(goals_[i]).id == goal) {
            return i;
        }
    }
    return kNoPoly;
}

SceneId Simulation::addStaticObject(const StaticObject& object, SceneId id) {
    id = claimId(id);
    if (id == kNoSceneId) {
        return kNoSceneId;
    }
    const entt::entity e = createEntity(id);
    registry_.emplace<StaticObject>(e, object);
    staticObjects_.push_back(e);
    navDirty_ = true;
    return id;
}

SceneId Simulation::addOffMeshLink(const OffMeshLink& link, SceneId id) {
    id = claimId(id);
    if (id == kNoSceneId) {
        return kNoSceneId;
    }
    const entt::entity e = createEntity(id);
    registry_.emplace<OffMeshLink>(e, link);
    offMeshLinks_.push_back(e);
    navDirty_ = true;
    return id;
}

SceneId Simulation::addGoal(const Goal& goal, SceneId id) {
    id = claimId(id);
    if (id == kNoSceneId) {
        return kNoSceneId;
    }
    const entt::entity e = createEntity(id);
    registry_.emplace<Goal>(e, goal);
    goals_.push_back(e);
    flowDirty_ = true;
    return id;
}

SceneId Simulation::addSpawner(const Spawner& spawner, SceneId id) {
    id = claimId(id);
    if (id == kNoSceneId) {
        return kNoSceneId;
    }
    const entt::entity e = createEntity(id);
    registry_.emplace<Spawner>(e, spawner);
    spawners_.push_back(e);
    return id;
}

SceneId Simulation::addGroup(const Group& group, SceneId id) {
    id = claimId(id);
    if (id == kNoSceneId) {
        return kNoSceneId;
    }
    const entt::entity e = createEntity(id);
    registry_.emplace<Group>(e, group);
    groups_.push_back(e);
    return id;
}

bool Simulation::updateGroup(SceneId id, const Group& group) {
    const entt::entity e = entityOf(id);
    if (e == entt::null || !registry_.all_of<Group>(e)) {
        return false;
    }
    registry_.get<Group>(e) = group;
    return true;
}

bool Simulation::setHidden(SceneId id, bool hidden) {
    const entt::entity e = entityOf(id);
    if (e == entt::null) {
        return false;
    }
    if (hidden) {
        registry_.emplace_or_replace<HiddenComponent>(e);
    } else {
        registry_.remove<HiddenComponent>(e);
    }
    ++displayVersion_;
    return true;
}

bool Simulation::isHidden(SceneId id) const {
    const entt::entity e = entityOf(id);
    return e != entt::null && registry_.all_of<HiddenComponent>(e);
}

bool Simulation::isVisible(SceneId id) const {
    for (SceneId p = id; p != kNoSceneId; p = parentOf(p)) {
        if (isHidden(p)) {
            return false;
        }
    }
    return true;
}

bool Simulation::setParent(SceneId id, SceneId parent) {
    const entt::entity e = entityOf(id);
    if (e == entt::null) {
        return false;
    }
    ++displayVersion_;  // may move it into (or out of) a hidden group
    if (parent == kNoSceneId) {
        registry_.remove<ParentComponent>(e);
        return true;
    }
    if (findGroup(parent) == nullptr) {
        return false;
    }
    for (SceneId p = parent; p != kNoSceneId; p = parentOf(p)) {
        if (p == id) {
            return false;  // a group inside itself
        }
    }
    registry_.emplace_or_replace<ParentComponent>(e, parent);
    return true;
}

SceneId Simulation::parentOf(SceneId id) const {
    const entt::entity e = entityOf(id);
    if (e == entt::null) {
        return kNoSceneId;
    }
    const ParentComponent* p = registry_.try_get<ParentComponent>(e);
    return p == nullptr ? kNoSceneId : p->parent;
}

std::vector<SceneId> Simulation::childrenOf(SceneId group) const {
    std::vector<SceneId> out;
    for (const std::vector<entt::entity>* list : {&groups_, &staticObjects_, &offMeshLinks_, &goals_, &spawners_}) {
        for (entt::entity e : *list) {
            const ParentComponent* p = registry_.try_get<ParentComponent>(e);
            if (p != nullptr && p->parent == group) {
                out.push_back(registry_.get<SceneIdComponent>(e).id);
            }
        }
    }
    return out;
}

bool Simulation::updateStaticObject(SceneId id, const StaticObject& object) {
    const entt::entity e = entityOf(id);
    if (e == entt::null || !registry_.all_of<StaticObject>(e)) {
        return false;
    }
    registry_.get<StaticObject>(e) = object;
    navDirty_ = true;
    return true;
}

bool Simulation::updateOffMeshLink(SceneId id, const OffMeshLink& link) {
    const entt::entity e = entityOf(id);
    if (e == entt::null || !registry_.all_of<OffMeshLink>(e)) {
        return false;
    }
    registry_.get<OffMeshLink>(e) = link;
    navDirty_ = true;
    return true;
}

bool Simulation::updateGoal(SceneId id, const Goal& goal) {
    const entt::entity e = entityOf(id);
    if (e == entt::null || !registry_.all_of<Goal>(e)) {
        return false;
    }
    Goal& g = registry_.get<Goal>(e);
    const std::uint32_t arrivals = g.arrivals;
    g = goal;
    g.arrivals = arrivals;
    flowDirty_ = true;
    retargetAgentsOfGoal(goalIndexOf(id));
    return true;
}

bool Simulation::updateSpawner(SceneId id, const Spawner& spawner) {
    const entt::entity e = entityOf(id);
    if (e == entt::null || !registry_.all_of<Spawner>(e)) {
        return false;
    }
    Spawner& s = registry_.get<Spawner>(e);
    const float accumulator = s.accumulator;
    const std::uint32_t spawned = s.spawned;
    s = spawner;
    s.accumulator = accumulator;
    s.spawned = spawned;
    return true;
}

bool Simulation::removeEntity(SceneId id) {
    const entt::entity e = entityOf(id);
    if (e == entt::null) {
        return false;
    }
    auto eraseFrom = [e](std::vector<entt::entity>& list) {
        list.erase(std::remove(list.begin(), list.end(), e), list.end());
    };
    if (registry_.all_of<StaticObject>(e)) {
        eraseFrom(staticObjects_);
        navDirty_ = true;
    } else if (registry_.all_of<OffMeshLink>(e)) {
        eraseFrom(offMeshLinks_);
        navDirty_ = true;
    } else if (registry_.all_of<Goal>(e)) {
        removeAgentsOfGoal(goalIndexOf(id));
        eraseFrom(goals_);
        flowDirty_ = true;
    } else if (registry_.all_of<Spawner>(e)) {
        eraseFrom(spawners_);
    } else if (registry_.all_of<Group>(e)) {
        const SceneId up = parentOf(id);
        for (SceneId child : childrenOf(id)) {
            setParent(child, up);
        }
        eraseFrom(groups_);
    }
    ++displayVersion_;
    registry_.destroy(e);
    idToEntity_.erase(id);
    return true;
}

void Simulation::removeAgentsOfGoal(std::uint32_t goalIndex) {
    if (goalIndex == kNoPoly) {
        return;
    }
    for (std::uint32_t i = agents_.size(); i-- > 0;) {
        if (agents_.goalIndex[i] == goalIndex || agents_.assignedGoal[i] == goalIndex) {
            removeAgentAt(i);
        }
    }
    for (std::vector<std::uint16_t>* list : {&agents_.goalIndex, &agents_.assignedGoal}) {
        for (std::uint16_t& g : *list) {
            if (g > goalIndex) {
                --g;
            }
        }
    }
}

void Simulation::retargetAgentsOfGoal(std::uint32_t goalIndex) {
    if (goalIndex == kNoPoly) {
        return;
    }
    const Area& area = registry_.get<Goal>(goals_[goalIndex]).area;
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        if (agents_.goalIndex[i] == goalIndex) {
            const glm::vec2 lane = area.clamp(agents_.position[i], 0.5f);
            agents_.goalTarget[i] = glm::vec3(lane.x, area.elevation, lane.y);
        }
    }
}

const StaticObject* Simulation::findStaticObject(SceneId id) const {
    const entt::entity e = entityOf(id);
    return e == entt::null ? nullptr : registry_.try_get<StaticObject>(e);
}

const OffMeshLink* Simulation::findOffMeshLink(SceneId id) const {
    const entt::entity e = entityOf(id);
    return e == entt::null ? nullptr : registry_.try_get<OffMeshLink>(e);
}

const Goal* Simulation::findGoal(SceneId id) const {
    const entt::entity e = entityOf(id);
    return e == entt::null ? nullptr : registry_.try_get<Goal>(e);
}

const Spawner* Simulation::findSpawner(SceneId id) const {
    const entt::entity e = entityOf(id);
    return e == entt::null ? nullptr : registry_.try_get<Spawner>(e);
}

const Group* Simulation::findGroup(SceneId id) const {
    const entt::entity e = entityOf(id);
    return e == entt::null ? nullptr : registry_.try_get<Group>(e);
}

std::vector<SceneId> Simulation::staticObjectIds() const {
    return idsOf<StaticObject>(staticObjects_);
}
std::vector<SceneId> Simulation::offMeshLinkIds() const {
    return idsOf<OffMeshLink>(offMeshLinks_);
}
std::vector<SceneId> Simulation::goalIds() const {
    return idsOf<Goal>(goals_);
}
std::vector<SceneId> Simulation::spawnerIds() const {
    return idsOf<Spawner>(spawners_);
}
std::vector<SceneId> Simulation::groupIds() const {
    return idsOf<Group>(groups_);
}

geometry::TriangleMesh Simulation::objectMesh(SceneId id) const {
    const StaticObject* o = findStaticObject(id);
    if (o == nullptr) {
        return {};
    }
    const geometry::PrimitiveDesc& p = o->primitive;
    if (p.kind == geometry::PrimitiveKind::Mesh) {
        if (!o->mesh) {
            return {};
        }
        // A Mesh object's flags override the asset's per-triangle tags only when set explicitly.
        return geometry::transformMesh(*o->mesh, p.position, p.rotationY, p.size.x, 0);
    }
    return geometry::buildPrimitive(p);
}

void Simulation::setNavigationSettings(const NavBuildSettings& settings) {
    config_.navigation = settings;
    navDirty_ = true;
}

void Simulation::setLocomotionParams(const LocomotionParams& params) {
    config_.locomotion = params;
    config_.locomotion.maxNeighbors = std::min(config_.locomotion.maxNeighbors, orca::kMaxNeighbors);
}

void Simulation::clearScene() {
    registry_.clear();
    staticObjects_.clear();
    offMeshLinks_.clear();
    goals_.clear();
    spawners_.clear();
    groups_.clear();
    idToEntity_.clear();
    nextSceneId_ = 1;
    navDirty_ = true;
    flowDirty_ = true;
    reset();
}

void Simulation::reset() {
    agents_.clear();
    for (entt::entity e : spawners_) {
        auto& s = registry_.get<Spawner>(e);
        s.accumulator = 0.0f;
        s.spawned = 0;
    }
    for (entt::entity e : goals_) {
        Goal& g = registry_.get<Goal>(e);
        g.arrivals = 0;
        g.nextTicket = 0;
    }
    rng_.reseed(config_.seed);
    stepIndex_ = 0;
    nextAgentId_ = 0;
    spawnedTotal_ = 0;
    arrivedTotal_ = 0;
    fallsTotal_ = 0;
    despawnedTotal_ = 0;
    idToIndex_.clear();
}

void Simulation::addAgent(const AgentInit& init) {
    if (idToIndex_.size() <= init.id) {
        idToIndex_.resize(init.id + 1, kNoAgent);
    }
    idToIndex_[init.id] = agents_.size();
    agents_.add(init);
    events_.push_back({SimEventType::AgentSpawned, init.id, -1});
}

void Simulation::removeAgentAt(std::uint32_t index) {
    const std::uint32_t last = agents_.size() - 1;
    idToIndex_[agents_.id[index]] = kNoAgent;
    if (index != last) {
        idToIndex_[agents_.id[last]] = index;
    }
    agents_.swapRemove(index);
}

std::uint32_t Simulation::agentIndexOf(std::uint32_t agentId) const noexcept {
    return agentId < idToIndex_.size() ? idToIndex_[agentId] : kNoAgent;
}

bool Simulation::updateNavigation() {
    bool changed = false;
    if (navDirty_) {
        CS_PROFILE_ZONE_NAMED("BuildNavMesh");
        staticMesh_.clear();
        objectTriangles_.clear();
        for (entt::entity e : staticObjects_) {
            const SceneId id = registry_.get<SceneIdComponent>(e).id;
            const std::uint32_t first = staticMesh_.triangleCount();
            staticMesh_.append(objectMesh(id));
            objectTriangles_.push_back({id, first, staticMesh_.triangleCount() - first});
        }
        std::vector<OffMeshLink> links;
        for (entt::entity e : offMeshLinks_) {
            links.push_back(registry_.get<OffMeshLink>(e));
        }
        navMesh_.build(staticMesh_, links, config_.navigation, services_.jobs, services_.log);
        // Agents' polygon refs and path corridors are invalid after a rebuild; re-acquire them.
        for (std::uint32_t i = 0; i < agents_.size(); ++i) {
            const glm::vec3 p(agents_.position[i].x, agents_.height[i], agents_.position[i].y);
            const auto nearest = navMesh_.findNearest(p, {1.0f, 2.0f, 1.0f});
            agents_.navPoly[i] = nearest ? nearest->ref : 0;
            agents_.corridorStatus[i] = 0;
        }
        rebuildTaggedVolumes();
        rebuildEscalators();
        rebuildEmergencyRoutes();
        navDirty_ = false;
        flowDirty_ = true;
        changed = true;
    }
    if (flowDirty_) {
        CS_PROFILE_ZONE_NAMED("BuildFlowFields");
        flowFields_.clear();
        emergencyFlowFields_.clear();
        const bool emergency = std::find(polyEmergency_.begin(), polyEmergency_.end(), 1) != polyEmergency_.end();
        for (entt::entity e : goals_) {
            const Area& area = registry_.get<Goal>(e).area;
            flowFields_.push_back(buildFlowField(navMesh_, area, polyClimb_,
                                                 emergency ? std::span<const std::uint8_t>(polyEmergency_)
                                                           : std::span<const std::uint8_t>()));
            if (emergency) {
                emergencyFlowFields_.push_back(buildFlowField(navMesh_, area, polyClimb_));
            }
        }
        rebuildTreeLookups();  // goal names may have changed
        flowDirty_ = false;
        changed = true;
    }
    if (changed) {
        ++staticVersion_;
        ++displayVersion_;
        staticSceneCache_.reset();
    }
    return changed;
}

const FlowField* Simulation::flowFieldFor(std::uint32_t i) const noexcept {
    const std::uint32_t g = agents_.goalIndex[i];
    const std::vector<FlowField>& fields =
        agents_.emergencyRoutes[i] != 0 && !emergencyFlowFields_.empty() ? emergencyFlowFields_ : flowFields_;
    return g < fields.size() ? &fields[g] : nullptr;
}

void Simulation::rebuildEmergencyRoutes() {
    // Emergency routes (fire stairs, exit doors) are objects tagged "emergency": everyday
    // routing keeps out of them, and agents told to (UseEmergencyRoutes) use them too.
    polyEmergency_.assign(navMesh_.polyCount(), 0);
    const auto it = std::find(tagNames_.begin(), tagNames_.end(), "emergency");
    if (it == tagNames_.end()) {
        return;
    }
    const auto tag = static_cast<std::uint16_t>(it - tagNames_.begin());
    for (std::uint32_t d = 0; d < polyEmergency_.size(); ++d) {
        const glm::vec3 c = navMesh_.polyCenter(d);
        for (const TaggedVolume& v : taggedVolumes_) {
            if (v.tag == tag && c.x >= v.min.x && c.x <= v.max.x && c.z >= v.min.z && c.z <= v.max.z &&
                c.y >= v.min.y - 0.5f && c.y <= v.max.y + 0.5f) {
                polyEmergency_[d] = 1;
                break;
            }
        }
    }
}

void Simulation::step() {
    CS_PROFILE_ZONE();
    using Clock = std::chrono::steady_clock;
    auto mark = Clock::now();
    const auto start = mark;
    auto lap = [&mark](float& slot) {
        const auto now = Clock::now();
        slot = std::chrono::duration<float, std::milli>(now - mark).count();
        mark = now;
    };
    events_.clear();
    updateNavigation();
    lap(profile_.navigation);
    const auto dt = static_cast<float>(stepSeconds_);

    goalAreas_.clear();
    goalsCache_.clear();
    for (entt::entity e : goals_) {
        goalAreas_.push_back(registry_.get<Goal>(e).area);
        goalsCache_.push_back(registry_.get<Goal>(e));
    }

    // Keep agents that are close in space close in memory (cache locality for the
    // neighbour, avoidance and movement passes). Deterministic: sorted by (cell, id).
    if (stepIndex_ % kReorderInterval == 0 && agents_.size() > 1) {
        reorderAgents();
    }
    const float cellSize = config_.locomotion.neighborDistance;
    hash_.build(agents_.position, cellSize, services_.jobs, agents_.height);
    lap(profile_.hash);

    const std::uint32_t before = agents_.size();
    spawnAgents(dt);
    lap(profile_.spawn);
    if (agents_.size() != before) {
        hash_.build(agents_.position, cellSize, services_.jobs, agents_.height);
        float rebuild = 0.0f;
        lap(rebuild);
        profile_.hash += rebuild;
    }

    tickBehaviors();  // decision layer: low rate, staggered, parallel
    lap(profile_.decisions);
    resolveDecisions();  // serial: queues, arrivals (deterministic shared state)
    lap(profile_.resolve);
    assignEscalatorLanes();
    computePreferredVelocities();
    lap(profile_.preferred);
    computeAvoidance(dt);
    if (config_.bodies.enabled) {
        computeBodies(dt);
        applyBodyRequests();
    }
    lap(profile_.avoidance);
    integrate(dt);
    lap(profile_.integrate);
    processDespawns();
    lap(profile_.despawn);
    ++stepIndex_;
    for (StepObserver* observer : stepObservers_) {
        observer->afterStep(*this);
    }
    lap(profile_.observers);
    profile_.total = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
}

void Simulation::reorderAgents() {
    CS_PROFILE_ZONE();
    const std::uint32_t n = agents_.size();
    const float inv = 1.0f / config_.locomotion.neighborDistance;
    // Z-order (Morton) key of the agent's cell, then its id.
    auto spread = [](std::uint32_t v) {
        v &= 0xFFFFu;
        v = (v | (v << 8)) & 0x00FF00FFu;
        v = (v | (v << 4)) & 0x0F0F0F0Fu;
        v = (v | (v << 2)) & 0x33333333u;
        v = (v | (v << 1)) & 0x55555555u;
        return v;
    };
    std::vector<std::uint64_t> keys(n);
    services_.jobs.parallelFor(n, 4096, [&](core::IndexRange r, std::uint32_t) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            const auto cx =
                static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(agents_.position[i].x * inv)) + 32768);
            const auto cz =
                static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(agents_.position[i].y * inv)) + 32768);
            keys[i] = (static_cast<std::uint64_t>(spread(cx) | (spread(cz) << 1)) << 32) | agents_.id[i];
        }
    });
    std::vector<std::uint32_t> order(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return keys[a] < keys[b]; });
    agents_.permute(order);
    for (std::uint32_t i = 0; i < n; ++i) {
        idToIndex_[agents_.id[i]] = i;
    }
}

void Simulation::addStepObserver(StepObserver* observer) {
    if (std::find(stepObservers_.begin(), stepObservers_.end(), observer) == stepObservers_.end()) {
        stepObservers_.push_back(observer);
    }
}

void Simulation::removeStepObserver(StepObserver* observer) {
    stepObservers_.erase(std::remove(stepObservers_.begin(), stepObservers_.end(), observer), stepObservers_.end());
}

void Simulation::spawnAgents(float dt) {
    CS_PROFILE_ZONE();
    const auto now = static_cast<float>(simTimeSeconds());
    const std::uint32_t firstNew = agents_.size();
    const bool useNav = navMesh_.valid();
    const float levelSep = config_.locomotion.levelSeparation;

    // Rejection-sample a free spot on the navmesh near `center` within `half`.
    auto findSpot = [&](glm::vec2 center, glm::vec2 half, float elevation, float radius, glm::vec2& pos, float& height,
                        PolyRef& poly) {
        for (std::uint32_t attempt = 0; attempt < kSpawnAttempts; ++attempt) {
            pos = center + glm::vec2(rng_.range(-1.0f, 1.0f) * half.x, rng_.range(-1.0f, 1.0f) * half.y);
            height = elevation;
            poly = 0;
            if (useNav) {
                const auto nearest = navMesh_.findNearest({pos.x, height, pos.y}, {0.25f, 1.5f, 0.25f});
                if (!nearest) {
                    continue;
                }
                pos = {nearest->point.x, nearest->point.z};
                height = nearest->point.y;
                poly = nearest->ref;
            }
            bool blocked = false;
            auto overlaps = [&](std::uint32_t j) {
                const float minDist = radius + agents_.radius[j];
                const glm::vec2 d = agents_.position[j] - pos;
                return glm::dot(d, d) < minDist * minDist && std::abs(agents_.height[j] - height) < levelSep;
            };
            hash_.query(pos, 2.0f * radius, [&](std::uint32_t j) { blocked = blocked || overlaps(j); });
            for (std::uint32_t j = firstNew; j < agents_.size() && !blocked; ++j) {
                blocked = overlaps(j);
            }
            if (!blocked) {
                return true;
            }
        }
        return false;
    };

    for (entt::entity e : spawners_) {
        Spawner& s = registry_.get<Spawner>(e);
        const std::uint32_t goalIndex = goalIndexOf(s.goal);
        if (now < s.startTime || now >= s.stopTime || goalIndex == kNoPoly) {
            continue;
        }
        s.accumulator += s.rate * dt;
        while (s.accumulator >= 1.0f && (s.total == 0 || s.spawned < s.total)) {
            // Pick the agent type from the spawner's weighted mix (or its inline parameters).
            std::uint16_t typeIndex = 0;
            if (!s.agentMix.empty()) {
                float totalWeight = 0.0f;
                for (const AgentMixEntry& m : s.agentMix) {
                    totalWeight += std::max(0.0f, m.weight);
                }
                float pick = rng_.nextFloat() * totalWeight;
                const AgentMixEntry* chosen = &s.agentMix.back();
                for (const AgentMixEntry& m : s.agentMix) {
                    pick -= std::max(0.0f, m.weight);
                    if (pick < 0.0f) {
                        chosen = &m;
                        break;
                    }
                }
                for (std::uint16_t t = 0; t < agentTypes_.size(); ++t) {
                    if (agentTypes_[t].name == chosen->type) {
                        typeIndex = t;
                    }
                }
            }
            const AgentType& type = agentTypes_[typeIndex];
            const AgentParams& p = s.agentMix.empty() ? s.agent : type.params;
            const std::uint16_t treeIndex = resolveTree(type.behaviour);

            const std::uint32_t groupSize = s.groupSizeMax > s.groupSizeMin
                                                ? s.groupSizeMin + rng_.below(s.groupSizeMax - s.groupSizeMin + 1)
                                                : std::max<std::uint32_t>(1, s.groupSizeMin);
            if (s.total != 0 && s.spawned + groupSize > s.total && s.spawned < s.total) {
                // Last partial group: spawn what is left.
            }

            const float leaderRadius = rng_.range(p.radiusMin, p.radiusMax);
            glm::vec2 leaderPos{};
            float leaderHeight = 0.0f;
            PolyRef leaderPoly = 0;
            if (!findSpot(s.area.center, s.area.halfExtents, s.area.elevation, leaderRadius, leaderPos, leaderHeight,
                          leaderPoly)) {
                break;  // crowded: keep the accumulator, retry next step
            }

            const Area& goalArea = goalAreas_[goalIndex];
            const std::uint32_t groupId = nextAgentId_;
            for (std::uint32_t member = 0; member < groupSize && (s.total == 0 || s.spawned < s.total); ++member) {
                float radius = leaderRadius;
                glm::vec2 pos = leaderPos;
                float height = leaderHeight;
                PolyRef poly = leaderPoly;
                if (member > 0) {
                    radius = rng_.range(p.radiusMin, p.radiusMax);
                    if (!findSpot(leaderPos, glm::vec2(1.5f), leaderHeight, radius, pos, height, poly)) {
                        break;  // no room for the rest of the group
                    }
                }
                const glm::vec2 lane = goalArea.clamp(pos, 0.5f);
                glm::vec3 target(lane.x, goalArea.elevation, lane.y);
                if (useNav) {
                    if (const auto nearest = navMesh_.findNearest(target, {1.0f, 2.0f, 1.0f})) {
                        target = nearest->point;
                    }
                }
                const float prefSpeed = rng_.range(p.preferredSpeedMin, p.preferredSpeedMax);
                // Only draw when the range is open, so default (single-mass) types keep
                // every existing random sequence and result unchanged.
                const float mass = p.massMax > p.massMin ? rng_.range(p.massMin, p.massMax) : p.massMin;
                const glm::vec3 jitter(rng_.range(-1.0f, 1.0f), rng_.range(-1.0f, 1.0f), rng_.range(-1.0f, 1.0f));

                AgentInit init;
                init.id = nextAgentId_++;
                init.position = pos;
                init.height = height;
                init.navPoly = poly;
                init.heading = headingOf(glm::vec2(target.x, target.z) - pos);
                init.radius = radius;
                init.preferredSpeed = prefSpeed;
                init.maxSpeed = prefSpeed * p.maxSpeedFactor;
                init.maxAcceleration = p.maxAcceleration;
                init.personalSpace = s.agentMix.empty() ? config_.locomotion.personalSpace : type.personalSpace;
                init.mass = mass;
                init.color = glm::clamp(p.color + jitter * p.colorJitter, glm::vec3(0.0f), glm::vec3(1.0f));
                init.goalTarget = target;
                init.goalIndex = static_cast<std::uint16_t>(goalIndex);
                init.agentType = typeIndex;
                init.tree = treeIndex;
                init.spawnTime = now;
                init.rngSeed = core::mixSeed(config_.seed ^ (static_cast<std::uint64_t>(init.id) << 20));
                init.groupId = groupSize > 1 ? groupId : kNoAgent;
                addAgent(init);
                agents_.bt.back().reset(trees_[treeIndex].generation);

                ++s.spawned;
                ++spawnedTotal_;
            }
            s.accumulator -= 1.0f;
        }
        if (s.total != 0 && s.spawned >= s.total) {
            s.accumulator = 0.0f;
        }
    }
}

void Simulation::computePreferredVelocities() {
    CS_PROFILE_ZONE();
    const bool useNav = navMesh_.valid();
    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t thread) {
        std::array<glm::vec3, kCorridorLength> lefts{};
        std::array<glm::vec3, kCorridorLength> rights{};
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            const glm::vec2 pos = agents_.position[i];
            const glm::vec3 pos3(pos.x, agents_.height[i], pos.y);
            glm::vec3 steerTo = pos3;
            bool moving = true;

            switch (agents_.moveMode[i]) {
                case MoveMode::Stop:
                    moving = false;
                    break;
                case MoveMode::Point:
                    steerTo = useNav ? corridorSteeringTarget(i, thread) : agents_.moveTarget[i];
                    break;
                case MoveMode::Goal: {
                    const glm::vec3 target = agents_.goalTarget[i];
                    steerTo = target;
                    if (useNav && agents_.linkActive[i] == 0 && flowFieldFor(i) != nullptr) {
                        const FlowField& ff = *flowFieldFor(i);
                        const std::uint32_t dense = navMesh_.denseIndex(agents_.navPoly[i]);
                        if (dense != kNoPoly && ff.next[dense] != kNoPoly) {
                            // Collect a short corridor of portals along the flow field and string-pull it.
                            std::uint32_t count = 0;
                            std::uint32_t u = dense;
                            bool reachedGoal = false;
                            while (count < kCorridorLength && ff.next[u] != kNoPoly) {
                                lefts[count] = ff.portalLeft[u];
                                rights[count] = ff.portalRight[u];
                                funnel::shrinkPortal(lefts[count], rights[count]);
                                ++count;
                                u = ff.next[u];
                                if (ff.source[u] != 0) {
                                    reachedGoal = true;
                                    break;
                                }
                            }
                            const glm::vec3 end = reachedGoal ? target : 0.5f * (lefts[count - 1] + rights[count - 1]);
                            steerTo = funnel::steeringTarget(pos3, lefts.data(), rights.data(), count, end);
                        }
                    }
                    break;
                }
            }

            glm::vec2 pref(0.0f);
            if (moving) {
                const glm::vec2 toTarget(steerTo.x - pos.x, steerTo.z - pos.y);
                const float dist = std::sqrt(glm::dot(toTarget, toTarget));
                const float speed = agents_.preferredSpeed[i] * agents_.speedMultiplier[i];
                // Slow down over the last half metre to a point target instead of overshooting.
                const float arrive = agents_.moveMode[i] == MoveMode::Point ? std::min(1.0f, dist / 0.5f) : 1.0f;
                pref = dist > 1e-4f ? toTarget * (speed * arrive / dist) : glm::vec2(0.0f);
            }
            // Keep out of tagged areas the behaviour asked to avoid.
            if (agents_.avoidTag[i] != 0) {
                for (const TaggedVolume& v : taggedVolumes_) {
                    if (v.tag != agents_.avoidTag[i] || pos3.y < v.min.y - 1.0f || pos3.y > v.max.y + 2.0f) {
                        continue;
                    }
                    const glm::vec2 closest(std::clamp(pos.x, v.min.x, v.max.x), std::clamp(pos.y, v.min.z, v.max.z));
                    const glm::vec2 away = pos - closest;
                    const float d = std::sqrt(glm::dot(away, away));
                    constexpr float kRange = 2.5f;
                    if (d < kRange && d > 1e-4f) {
                        pref += away / d * (agents_.preferredSpeed[i] * (1.0f - d / kRange));
                    }
                }
            }
            if (moving && agents_.linkActive[i] == 0 && !escalators_.empty()) {
                pref = escalatorSteering(i, pref);
            }
            if (moving && agents_.linkActive[i] == 0 && agents_.manners[i].gap > 0.0f) {
                pref = applyQueueManners(i, pref);
            }
            agents_.preferredVelocity[i] = pref;
        }
    });
}

void Simulation::computeAvoidance(float dt) {
    CS_PROFILE_ZONE();
    const LocomotionParams& lp = config_.locomotion;
    const orca::Params params{lp.timeHorizon, dt, lp.obstacleTimeHorizon};
    const bool useNav = navMesh_.valid();
    const bool bodies = config_.bodies.enabled;

    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t) {
        std::array<orca::Neighbor, orca::kMaxNeighbors> neighbors{};
        std::array<float, orca::kMaxNeighbors> distSq{};
        std::array<orca::Wall, 16> wallCandidates{};
        std::array<float, 16> wallDist{};

        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            if (agents_.linkActive[i] != 0) {
                continue;  // on a ladder / escalator: no avoidance
            }
            if (bodies && agents_.fallen[i] != 0) {
                agents_.newVelocity[i] = glm::vec2(0.0f);
                continue;
            }
            const glm::vec2 p = agents_.position[i];
            const float h = agents_.height[i];
            std::uint32_t count = 0;
            // With bodies, someone held up in a crowd stops keeping their distance and
            // presses into the gaps, more readily the harder they push: avoidance then
            // lets bodies close in to their cores, and contact forces take over.
            float shrink = 1.0f;
            if (bodies) {
                const float willing = std::clamp(agents_.push[i] / 300.0f, 0.2f, 1.0f);
                const float accept = willing * std::min(1.0f, agents_.slowTime[i] / 3.0f);
                shrink = 1.0f - accept * (1.0f - config_.bodies.core);
            }

            // Keep the K nearest on the same level, ordered by (distance, index) so ties are deterministic.
            // Positions and heights come from the hash (contiguous); only accepted
            // neighbours touch the agent arrays.
            hash_.queryPoints(p, lp.neighborDistance, [&](std::uint32_t j, glm::vec2 q, float qh) {
                if (j == i || std::abs(qh - h) > lp.levelSeparation) {
                    return;
                }
                const glm::vec2 d = q - p;
                const float d2 = glm::dot(d, d);
                if (count == lp.maxNeighbors &&
                    (d2 > distSq[count - 1] || (d2 == distSq[count - 1] && j > neighbors[count - 1].index))) {
                    return;
                }
                std::uint32_t slot = count < lp.maxNeighbors ? count++ : count - 1;
                while (slot > 0 &&
                       (distSq[slot - 1] > d2 || (distSq[slot - 1] == d2 && neighbors[slot - 1].index > j))) {
                    distSq[slot] = distSq[slot - 1];
                    neighbors[slot] = neighbors[slot - 1];
                    --slot;
                }
                distSq[slot] = d2;
                if (bodies && agents_.fallen[j] != 0) {
                    // Someone lying on the ground: a still obstacle to step around.
                    neighbors[slot] = orca::Neighbor{agents_.position[j], glm::vec2(0.0f),
                                                     agents_.radius[j] * 1.6f, j, 1.0f};
                } else {
                    neighbors[slot] = orca::Neighbor{agents_.position[j], agents_.velocity[j],
                                                     (agents_.radius[j] + agents_.personalSpace[j]) * shrink, j,
                                                     orca::massResponsibility(agents_.mass[i], agents_.mass[j])};
                }
            });

            // Nearby navmesh boundary edges on this level, nearest first. Escalator riders
            // skip them: the steps' edges are already kept by the navmesh, and the corner
            // where the steps meet the landing would otherwise pin anyone touching it.
            const float radius = agents_.radius[i] + agents_.personalSpace[i];
            std::uint32_t numWalls = 0;
            if (useNav && (escalators_.empty() || !onEscalator(p, h, agents_.rider[i].clearLanding))) {
                const float reach = radius + kWallQueryMargin;
                navMesh_.queryWalls({p.x, h, p.y}, reach, [&](const WallSegment& w) {
                    if (std::abs(0.5f * (w.a.y + w.b.y) - h) > lp.levelSeparation) {
                        return;
                    }
                    const glm::vec2 a(w.a.x, w.a.z);
                    const glm::vec2 b(w.b.x, w.b.z);
                    const float d = distanceToSegment2d(p, a, b);
                    if (d > reach) {
                        return;
                    }
                    std::uint32_t slot = 0;
                    if (numWalls < wallCandidates.size()) {
                        slot = numWalls++;
                    } else if (d < wallDist[numWalls - 1]) {
                        slot = numWalls - 1;  // replace the farthest kept wall
                    } else {
                        return;
                    }
                    while (slot > 0 && wallDist[slot - 1] > d) {
                        wallDist[slot] = wallDist[slot - 1];
                        wallCandidates[slot] = wallCandidates[slot - 1];
                        --slot;
                    }
                    wallDist[slot] = d;
                    wallCandidates[slot] = orca::Wall{a, b};
                });
            }

            const orca::AgentState self{p,      agents_.velocity[i], agents_.preferredVelocity[i],
                                        radius * shrink, agents_.maxSpeed[i], i};
            agents_.newVelocity[i] = orca::computeNewVelocity(
                self, std::span(neighbors.data(), count),
                std::span(wallCandidates.data(), std::min<std::uint32_t>(numWalls, orca::kMaxWalls)), params);
        }
    });
}

void Simulation::integrate(float dt) {
    CS_PROFILE_ZONE();
    const float maxTurn = config_.locomotion.maxTurnRate * dt;
    const bool useNav = navMesh_.valid();
    const dtNavMesh* detour = navMesh_.detour();
    const bool bodies = config_.bodies.enabled;

    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t thread) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            agents_.prevPosition[i] = agents_.position[i];
            agents_.prevHeading[i] = agents_.heading[i];
            agents_.prevHeight[i] = agents_.height[i];
            if (bodies && agents_.fallen[i] != 0) {
                agents_.velocity[i] = glm::vec2(0.0f);  // lying still
                agents_.wallForce[i] = glm::vec2(0.0f);
                agents_.slowTime[i] += dt;
                continue;
            }

            if (agents_.linkActive[i] != 0) {
                // Traverse an off-mesh link at walking pace.
                const glm::vec3 a = agents_.linkStart[i];
                const glm::vec3 b = agents_.linkEnd[i];
                const float len = std::max(0.01f, glm::length(b - a));
                agents_.linkProgress[i] += agents_.preferredSpeed[i] * dt / len;
                const float t = std::min(1.0f, agents_.linkProgress[i]);
                const glm::vec3 p = a + (b - a) * t;
                const glm::vec2 dir = glm::vec2(b.x - a.x, b.z - a.z);
                const float dirLen = glm::length(dir);
                agents_.velocity[i] = dirLen > 1e-4f ? dir / dirLen * agents_.preferredSpeed[i] : glm::vec2(0.0f);
                agents_.position[i] = {p.x, p.z};
                agents_.height[i] = p.y;
                if (t >= 1.0f) {
                    agents_.linkActive[i] = 0;
                    agents_.navPoly[i] = agents_.linkEndPoly[i];
                }
            } else {
                glm::vec2 v;
                const bool pressed = bodies && agents_.inContact[i] != 0;
                if (pressed) {
                    v = bodyVelocity(i, dt);  // touching others: moved by forces
                } else {
                    // Acceleration-limited velocity change towards the avoidance result.
                    glm::vec2 dv = agents_.newVelocity[i] - agents_.velocity[i];
                    const float maxDv = agents_.maxAcceleration[i] * dt;
                    const float dvLenSq = glm::dot(dv, dv);
                    if (dvLenSq > maxDv * maxDv) {
                        dv *= maxDv / std::sqrt(dvLenSq);
                    }
                    v = agents_.velocity[i] + dv;
                }
                const glm::vec2 start2 = agents_.position[i];
                if (!escalators_.empty()) {
                    // Escalator steps carry their riders whatever avoidance says: never
                    // slower than the belt in its direction of travel.
                    const glm::vec2 belt = beltVelocityAt(start2, agents_.height[i]);
                    const float beltSpeed = std::sqrt(glm::dot(belt, belt));
                    if (beltSpeed > 0.0f) {
                        const glm::vec2 dir = belt / beltSpeed;
                        const float along = glm::dot(v, dir);
                        if (along < beltSpeed) {
                            v += dir * (beltSpeed - along);
                        }
                    }
                }
                glm::vec2 end2 = start2 + v * dt;

                if (useNav && agents_.navPoly[i] != 0) {
                    // Constrain the move to the navmesh surface (walls, ledges, floors, stairs).
                    dtNavMeshQuery& q = navMesh_.query(thread);
                    const float start[3] = {start2.x, agents_.height[i], start2.y};
                    const float end[3] = {end2.x, agents_.height[i], end2.y};
                    float result[3];
                    std::array<dtPolyRef, 16> visited{};
                    int numVisited = 0;
                    const dtStatus st =
                        q.moveAlongSurface(agents_.navPoly[i], start, end, &navMesh_.filter(), result, visited.data(),
                                           &numVisited, static_cast<int>(visited.size()));
                    if (dtStatusSucceed(st) && numVisited > 0) {
                        const dtPolyRef poly = visited[static_cast<std::size_t>(numVisited - 1)];
                        float h = result[1];
                        if (dtStatusSucceed(q.getPolyHeight(poly, result, &h))) {
                            result[1] = h;
                        }
                        agents_.navPoly[i] = poly;
                        agents_.height[i] = result[1];
                        const glm::vec2 moved(result[0], result[2]);
                        if (glm::dot(moved - end2, moved - end2) > 1e-8f) {
                            const glm::vec2 slid = (moved - start2) / dt;  // slid along a boundary
                            if (pressed) {
                                // What the wall took out of the motion is its push on the body.
                                agents_.wallForce[i] = clampWallForce(agents_.mass[i] * (slid - v) / dt);
                            }
                            v = slid;
                        } else if (pressed) {
                            agents_.wallForce[i] = glm::vec2(0.0f);
                        }
                        end2 = moved;
                    }
                }
                if (!pressed) {
                    agents_.wallForce[i] = glm::vec2(0.0f);
                }
                agents_.velocity[i] = v;
                agents_.position[i] = end2;
                // Time spent wanting to move but hardly moving (IsBlocked).
                const glm::vec2 want = agents_.preferredVelocity[i];
                const float wantSq = glm::dot(want, want);
                if (wantSq > 0.04f && glm::dot(v, want) < 0.25f * wantSq) {
                    agents_.slowTime[i] += dt;
                } else {
                    agents_.slowTime[i] = 0.0f;
                }

                // Start traversing an off-mesh link when the flow field leads onto one.
                if (useNav && detour != nullptr) {
                    // Next polygon on the way: from the flow field (Goal) or the path corridor (Point).
                    PolyRef nextPoly = 0;
                    if (agents_.moveMode[i] == MoveMode::Goal && flowFieldFor(i) != nullptr) {
                        const std::uint32_t dense = navMesh_.denseIndex(agents_.navPoly[i]);
                        const FlowField& ff = *flowFieldFor(i);
                        if (dense != kNoPoly && ff.next[dense] != kNoPoly) {
                            nextPoly = navMesh_.refAt(ff.next[dense]);
                        }
                    } else if (agents_.moveMode[i] == MoveMode::Point && agents_.corridorLength[i] > 1 &&
                               agents_.corridor[i][0] == agents_.navPoly[i]) {
                        nextPoly = agents_.corridor[i][1];
                    }
                    const std::uint32_t nextDense = navMesh_.denseIndex(nextPoly);
                    if (nextDense != kNoPoly && navMesh_.isOffMeshConnection(nextDense)) {
                        float linkA[3];
                        float linkB[3];
                        if (dtStatusSucceed(detour->getOffMeshConnectionPolyEndPoints(agents_.navPoly[i], nextPoly,
                                                                                      linkA, linkB))) {
                            const glm::vec2 toStart(linkA[0] - end2.x, linkA[2] - end2.y);
                            if (glm::dot(toStart, toStart) < kOffMeshTriggerRadius * kOffMeshTriggerRadius) {
                                const glm::vec3 b(linkB[0], linkB[1], linkB[2]);
                                const auto landing = navMesh_.findNearest(b, {0.5f, 1.0f, 0.5f}, thread);
                                if (landing) {
                                    agents_.linkActive[i] = 1;
                                    agents_.linkProgress[i] = 0.0f;
                                    agents_.linkStart[i] = {end2.x, agents_.height[i], end2.y};
                                    agents_.linkEnd[i] = landing->point;
                                    agents_.linkEndPoly[i] = landing->ref;
                                }
                            }
                        }
                    }
                }
            }

            // Turn the body towards the direction of travel at a bounded rate.
            const glm::vec2 v = agents_.velocity[i];
            if (glm::dot(v, v) > 0.05f * 0.05f) {
                const float diff = wrapAngle(headingOf(v) - agents_.heading[i]);
                agents_.heading[i] = wrapAngle(agents_.heading[i] + std::clamp(diff, -maxTurn, maxTurn));
            }
        }
    });
}

SimStats Simulation::stats() const noexcept {
    SimStats s;
    s.alive = agents_.size();
    s.spawned = spawnedTotal_;
    s.arrived = arrivedTotal_;
    s.despawned = despawnedTotal_;
    double speedSum = 0.0;
    for (const glm::vec2& v : agents_.velocity) {
        speedSum += std::sqrt(static_cast<double>(glm::dot(v, v)));
    }
    s.averageSpeed = agents_.empty() ? 0.0f : static_cast<float>(speedSum / agents_.size());
    s.falls = fallsTotal_;
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        s.fallen += agents_.fallen[i];
        s.peakPressure = std::max(s.peakPressure, agents_.pressure[i]);
    }
    return s;
}

std::uint64_t Simulation::stateHash() const noexcept {
    core::Fnv1a64 hash;
    hash.add(stepIndex_);
    hash.add(rng_.state());
    hash.add(nextAgentId_);
    hash.add(spawnedTotal_);
    hash.add(arrivedTotal_);
    hash.addBytes(std::as_bytes(std::span(agents_.id)));
    hash.addBytes(std::as_bytes(std::span(agents_.position)));
    hash.addBytes(std::as_bytes(std::span(agents_.height)));
    hash.addBytes(std::as_bytes(std::span(agents_.velocity)));
    hash.addBytes(std::as_bytes(std::span(agents_.heading)));
    hash.addBytes(std::as_bytes(std::span(agents_.navPoly)));
    hash.addBytes(std::as_bytes(std::span(agents_.stateId)));
    hash.addBytes(std::as_bytes(std::span(agents_.moveMode)));
    hash.addBytes(std::as_bytes(std::span(agents_.fallen)));
    return hash.value();
}

std::shared_ptr<const snapshot::StaticScene> Simulation::buildStaticScene() const {
    auto scene = std::make_shared<snapshot::StaticScene>();
    scene->version = displayVersion_;
    const std::vector<geometry::RenderVertex> vertices = geometry::toRenderVertices(staticMesh_);
    for (const TriangleRange& r : objectTriangles_) {
        if (!isVisible(r.object)) {
            continue;
        }
        for (std::uint32_t v = 3 * r.first; v < 3 * (r.first + r.count) && v < vertices.size(); ++v) {
            const geometry::RenderVertex& x = vertices[v];
            scene->meshVertices.push_back({x.x, x.y, x.z, x.nx, x.ny, x.nz, x.abgr});
        }
    }
    scene->navTriangles = navMesh_.debugTriangles();
    for (const WallSegment& w : navMesh_.walls()) {
        scene->navWalls.push_back(w.a + glm::vec3(0.0f, 0.05f, 0.0f));
        scene->navWalls.push_back(w.b + glm::vec3(0.0f, 0.05f, 0.0f));
    }
    for (const OffMeshLink& l : navMesh_.offMeshLinks()) {
        scene->offMeshLinks.push_back(l.start);
        scene->offMeshLinks.push_back(l.end);
    }
    for (const FlowField& ff : flowFields_) {
        std::vector<glm::vec3>& arrows = scene->flowArrows.emplace_back();
        for (std::uint32_t d = 0; d < ff.next.size(); ++d) {
            if (ff.next[d] == kNoPoly || navMesh_.isOffMeshConnection(d)) {
                continue;
            }
            const glm::vec3 from = navMesh_.polyCenter(d) + glm::vec3(0.0f, 0.1f, 0.0f);
            const glm::vec3 to = 0.5f * (ff.portalLeft[d] + ff.portalRight[d]) + glm::vec3(0.0f, 0.1f, 0.0f);
            arrows.push_back(from);
            arrows.push_back(to);
        }
    }
    return scene;
}

void Simulation::writeSnapshot(snapshot::RenderSnapshot& out) const {
    CS_PROFILE_ZONE();
    out.stepIndex = stepIndex_;
    out.simTimeSeconds = simTimeSeconds();

    const std::uint32_t n = agents_.size();
    out.agents.resize(n);
    services_.jobs.parallelFor(n, 1024, [&](core::IndexRange r, std::uint32_t) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            snapshot::AgentInstance& a = out.agents[i];
            const glm::vec2 prev = agents_.prevPosition[i];
            const glm::vec2 cur = agents_.position[i];
            const glm::vec2 v = agents_.velocity[i];
            a.prevPosition = glm::vec3(prev.x, agents_.prevHeight[i], prev.y);
            a.prevHeading = agents_.prevHeading[i];
            a.position = glm::vec3(cur.x, agents_.height[i], cur.y);
            a.heading = agents_.heading[i];
            a.color = agents_.color[i];
            a.radius = agents_.radius[i];
            a.velocity = v;
            // Gait follows the feet: riders on an escalator are carried, not walking.
            const glm::vec2 own = escalators_.empty() ? v : v - beltVelocityAt(cur, agents_.height[i]);
            a.speed = std::sqrt(glm::dot(own, own));
            float turn = agents_.heading[i] - agents_.prevHeading[i];
            turn -= 6.2831853f * std::floor((turn + 3.14159265f) / 6.2831853f);
            a.turnRate = turn * static_cast<float>(config_.stepHz);
            a.agentType = agents_.agentType[i];
            a.state = agents_.stateId[i];
            a.agentId = agents_.id[i];
            a.pressure = agents_.pressure[i];
            a.fallen = agents_.fallen[i];
            a.pileLayer = agents_.pileLayer[i];
        }
    });

    out.areas.clear();
    for (entt::entity e : spawners_) {
        if (!isVisible(registry_.get<SceneIdComponent>(e).id)) {
            continue;
        }
        const Area& area = registry_.get<Spawner>(e).area;
        out.areas.push_back({area.center3d(), area.halfExtents, glm::vec4(0.95f, 0.65f, 0.2f, 0.35f)});
    }
    for (entt::entity e : goals_) {
        if (!isVisible(registry_.get<SceneIdComponent>(e).id)) {
            continue;
        }
        const Area& area = registry_.get<Goal>(e).area;
        out.areas.push_back({area.center3d(), area.halfExtents, glm::vec4(0.25f, 0.85f, 0.45f, 0.35f)});
    }

    if (!staticSceneCache_ || staticSceneCache_->version != displayVersion_) {
        staticSceneCache_ = buildStaticScene();
    }
    out.staticScene = staticSceneCache_;
}

}  // namespace crowdsim::sim
