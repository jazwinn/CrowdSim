#include "crowdsim/sim/Simulation.hpp"

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>

#include <spdlog/logger.h>
#include <glm/geometric.hpp>

#include "crowdsim/core/Hash.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/sim/Orca.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

namespace crowdsim::sim {

namespace {

constexpr std::uint32_t kParallelChunk = 256;
constexpr std::uint32_t kSpawnAttempts = 8;
constexpr std::uint32_t kCorridorLength = 8;    ///< polygons of look-ahead for string pulling
constexpr float kWallQueryMargin = 1.0f;        ///< m beyond the agent radius
constexpr float kOffMeshTriggerRadius = 0.6f;   ///< m from a link start to begin traversing
constexpr float kGoalVerticalTolerance = 1.5f;  ///< m
constexpr float kPortalShrink = 0.2f;           ///< m pulled in from each portal end

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

struct Corner {
    glm::vec3 point;
    std::uint32_t portalIndex;  ///< portal the corner lies on; == count when it is `end`
};

/// First corner of the string-pulled path through `count` portals towards `end`
/// (simple stupid funnel, same orientation conventions as Detour).
Corner firstCorner(glm::vec3 start, const glm::vec3* lefts, const glm::vec3* rights, std::uint32_t count,
                   glm::vec3 end) noexcept {
    const float apex[3] = {start.x, start.y, start.z};
    float portalLeft[3] = {start.x, start.y, start.z};
    float portalRight[3] = {start.x, start.y, start.z};
    std::uint32_t leftIndex = 0;
    std::uint32_t rightIndex = 0;
    for (std::uint32_t i = 0; i <= count; ++i) {
        const glm::vec3 l = i < count ? lefts[i] : end;
        const glm::vec3 r = i < count ? rights[i] : end;
        const float left[3] = {l.x, l.y, l.z};
        const float right[3] = {r.x, r.y, r.z};

        if (dtTriArea2D(apex, portalRight, right) <= 0.0f) {
            if (dtVequal(apex, portalRight) || dtTriArea2D(apex, portalLeft, right) > 0.0f) {
                dtVcopy(portalRight, right);
                rightIndex = i;
            } else {
                return {{portalLeft[0], portalLeft[1], portalLeft[2]}, leftIndex};
            }
        }
        if (dtTriArea2D(apex, portalLeft, left) >= 0.0f) {
            if (dtVequal(apex, portalLeft) || dtTriArea2D(apex, portalRight, left) < 0.0f) {
                dtVcopy(portalLeft, left);
                leftIndex = i;
            } else {
                return {{portalRight[0], portalRight[1], portalRight[2]}, rightIndex};
            }
        }
    }
    return {end, count};
}

/// Steering target along the corridor: the first corner, or the one after it if
/// the agent is already close to the first (otherwise agents stall at corners,
/// which sit exactly on the navmesh boundary).
glm::vec3 steeringTarget(glm::vec3 start, const glm::vec3* lefts, const glm::vec3* rights, std::uint32_t count,
                         glm::vec3 end) noexcept {
    constexpr float kCornerSkipDistance = 0.1f;  // only once the corner is reached
    Corner c = firstCorner(start, lefts, rights, count, end);
    for (int k = 0; k < 2 && c.portalIndex < count; ++k) {
        const glm::vec2 d(c.point.x - start.x, c.point.z - start.z);
        if (glm::dot(d, d) > kCornerSkipDistance * kCornerSkipDistance) {
            break;
        }
        const std::uint32_t next = c.portalIndex + 1;
        Corner after = firstCorner(c.point, lefts + next, rights + next, count - next, end);
        after.portalIndex += next;
        c = after;
    }
    return c.point;
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
    services_.log.debug("Simulation created (seed={}, {} Hz)", config_.seed, config_.stepHz);
}

Simulation::~Simulation() = default;

std::uint32_t Simulation::addStaticObject(const StaticObject& object) {
    const entt::entity e = registry_.create();
    registry_.emplace<StaticObject>(e, object);
    staticObjects_.push_back(e);
    navDirty_ = true;
    return static_cast<std::uint32_t>(staticObjects_.size() - 1);
}

void Simulation::addOffMeshLink(const OffMeshLink& link) {
    offMeshLinks_.push_back(link);
    navDirty_ = true;
}

std::uint32_t Simulation::addGoal(const Goal& goal) {
    const entt::entity e = registry_.create();
    registry_.emplace<Goal>(e, goal);
    goals_.push_back(e);
    flowDirty_ = true;
    return static_cast<std::uint32_t>(goals_.size() - 1);
}

std::uint32_t Simulation::addSpawner(const Spawner& spawner) {
    const entt::entity e = registry_.create();
    registry_.emplace<Spawner>(e, spawner);
    spawners_.push_back(e);
    return static_cast<std::uint32_t>(spawners_.size() - 1);
}

void Simulation::clearScene() {
    registry_.clear();
    staticObjects_.clear();
    offMeshLinks_.clear();
    goals_.clear();
    spawners_.clear();
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
        registry_.get<Goal>(e).arrivals = 0;
    }
    rng_.reseed(config_.seed);
    stepIndex_ = 0;
    nextAgentId_ = 0;
    spawnedTotal_ = 0;
    arrivedTotal_ = 0;
}

bool Simulation::updateNavigation() {
    bool changed = false;
    if (navDirty_) {
        CS_PROFILE_ZONE_NAMED("BuildNavMesh");
        staticMesh_.clear();
        for (entt::entity e : staticObjects_) {
            staticMesh_.append(geometry::buildPrimitive(registry_.get<StaticObject>(e).primitive));
        }
        navMesh_.build(staticMesh_, offMeshLinks_, config_.navigation, services_.jobs, services_.log);
        // Agents' polygon refs are invalid after a rebuild; re-acquire them.
        for (std::uint32_t i = 0; i < agents_.size(); ++i) {
            const glm::vec3 p(agents_.position[i].x, agents_.height[i], agents_.position[i].y);
            const auto nearest = navMesh_.findNearest(p, {1.0f, 2.0f, 1.0f});
            agents_.navPoly[i] = nearest ? nearest->ref : 0;
        }
        navDirty_ = false;
        flowDirty_ = true;
        changed = true;
    }
    if (flowDirty_) {
        CS_PROFILE_ZONE_NAMED("BuildFlowFields");
        flowFields_.clear();
        for (entt::entity e : goals_) {
            flowFields_.push_back(buildFlowField(navMesh_, registry_.get<Goal>(e).area));
        }
        flowDirty_ = false;
        changed = true;
    }
    if (changed) {
        ++staticVersion_;
        staticSceneCache_.reset();
    }
    return changed;
}

void Simulation::step() {
    CS_PROFILE_ZONE();
    updateNavigation();
    const auto dt = static_cast<float>(stepSeconds_);

    goalAreas_.clear();
    for (entt::entity e : goals_) {
        goalAreas_.push_back(registry_.get<Goal>(e).area);
    }

    const float cellSize = config_.locomotion.neighborDistance;
    hash_.build(agents_.position, cellSize, services_.jobs);

    const std::uint32_t before = agents_.size();
    spawnAgents(dt);
    if (agents_.size() != before) {
        hash_.build(agents_.position, cellSize, services_.jobs);
    }

    computePreferredVelocities();
    computeAvoidance(dt);
    integrate(dt);
    removeArrivedAgents();
    ++stepIndex_;
}

void Simulation::spawnAgents(float dt) {
    CS_PROFILE_ZONE();
    const auto now = static_cast<float>(simTimeSeconds());
    const std::uint32_t firstNew = agents_.size();
    const bool useNav = navMesh_.valid();
    const float levelSep = config_.locomotion.levelSeparation;

    for (entt::entity e : spawners_) {
        Spawner& s = registry_.get<Spawner>(e);
        if (now < s.startTime || now >= s.stopTime || s.goal >= goalAreas_.size()) {
            continue;
        }
        s.accumulator += s.rate * dt;
        while (s.accumulator >= 1.0f && (s.total == 0 || s.spawned < s.total)) {
            const AgentParams& p = s.agent;
            const float radius = rng_.range(p.radiusMin, p.radiusMax);

            // Rejection-sample a free spot on the navmesh; give up for this step if crowded.
            bool placed = false;
            glm::vec2 pos{};
            float height = s.area.elevation;
            PolyRef poly = 0;
            for (std::uint32_t attempt = 0; attempt < kSpawnAttempts && !placed; ++attempt) {
                pos = s.area.center + glm::vec2(rng_.range(-1.0f, 1.0f) * s.area.halfExtents.x,
                                                rng_.range(-1.0f, 1.0f) * s.area.halfExtents.y);
                height = s.area.elevation;
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
                placed = !blocked;
            }
            if (!placed) {
                break;  // keep the accumulator; retry next step
            }

            const Area& goalArea = goalAreas_[s.goal];
            const glm::vec2 lane = goalArea.clamp(pos, 0.5f);
            glm::vec3 target(lane.x, goalArea.elevation, lane.y);
            if (useNav) {
                if (const auto nearest = navMesh_.findNearest(target, {1.0f, 2.0f, 1.0f})) {
                    target = nearest->point;
                }
            }
            const float prefSpeed = rng_.range(p.preferredSpeedMin, p.preferredSpeedMax);
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
            init.color = glm::clamp(p.color + jitter * p.colorJitter, glm::vec3(0.0f), glm::vec3(1.0f));
            init.goalTarget = target;
            init.goalIndex = static_cast<std::uint16_t>(s.goal);
            init.agentType = p.agentType;
            init.spawnTime = now;
            agents_.add(init);

            s.accumulator -= 1.0f;
            ++s.spawned;
            ++spawnedTotal_;
        }
        if (s.total != 0 && s.spawned >= s.total) {
            s.accumulator = 0.0f;
        }
    }
}

void Simulation::computePreferredVelocities() {
    CS_PROFILE_ZONE();
    const bool useNav = navMesh_.valid();
    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t) {
        std::array<glm::vec3, kCorridorLength> lefts{};
        std::array<glm::vec3, kCorridorLength> rights{};
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            const glm::vec2 pos = agents_.position[i];
            const glm::vec3 pos3(pos.x, agents_.height[i], pos.y);
            const glm::vec3 target = agents_.goalTarget[i];
            glm::vec3 steerTo = target;

            if (useNav && agents_.linkActive[i] == 0) {
                const FlowField& ff = flowFields_[agents_.goalIndex[i]];
                const std::uint32_t dense = navMesh_.denseIndex(agents_.navPoly[i]);
                if (dense != kNoPoly && ff.next[dense] != kNoPoly) {
                    // Collect a short corridor of portals along the flow field and string-pull it.
                    std::uint32_t count = 0;
                    std::uint32_t u = dense;
                    bool reachedGoal = false;
                    while (count < kCorridorLength && ff.next[u] != kNoPoly) {
                        // Shrink portals so string-pulled corners sit inside walkable
                        // space instead of exactly on navmesh boundary vertices.
                        const glm::vec3 l = ff.portalLeft[u];
                        const glm::vec3 rr = ff.portalRight[u];
                        const float width = glm::length(glm::vec2(rr.x - l.x, rr.z - l.z));
                        const float shrink = width > 1e-3f ? std::min(kPortalShrink, 0.25f * width) / width : 0.0f;
                        lefts[count] = l + (rr - l) * shrink;
                        rights[count] = rr + (l - rr) * shrink;
                        ++count;
                        u = ff.next[u];
                        if (ff.source[u] != 0) {
                            reachedGoal = true;
                            break;
                        }
                    }
                    const glm::vec3 end = reachedGoal ? target : 0.5f * (lefts[count - 1] + rights[count - 1]);
                    steerTo = steeringTarget(pos3, lefts.data(), rights.data(), count, end);
                }
            }

            const glm::vec2 toTarget(steerTo.x - pos.x, steerTo.z - pos.y);
            const float dist = std::sqrt(glm::dot(toTarget, toTarget));
            agents_.preferredVelocity[i] =
                dist > 1e-4f ? toTarget * (agents_.preferredSpeed[i] / dist) : glm::vec2(0.0f);
        }
    });
}

void Simulation::computeAvoidance(float dt) {
    CS_PROFILE_ZONE();
    const LocomotionParams& lp = config_.locomotion;
    const float rangeSq = lp.neighborDistance * lp.neighborDistance;
    const orca::Params params{lp.timeHorizon, dt, lp.obstacleTimeHorizon};
    const bool useNav = navMesh_.valid();

    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t) {
        std::array<orca::Neighbor, orca::kMaxNeighbors> neighbors{};
        std::array<float, orca::kMaxNeighbors> distSq{};
        std::array<orca::Wall, 16> wallCandidates{};
        std::array<float, 16> wallDist{};

        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            if (agents_.linkActive[i] != 0) {
                continue;  // on a ladder / escalator: no avoidance
            }
            const glm::vec2 p = agents_.position[i];
            const float h = agents_.height[i];
            std::uint32_t count = 0;

            // Keep the K nearest on the same level, ordered by (distance, index) so ties are deterministic.
            hash_.query(p, lp.neighborDistance, [&](std::uint32_t j) {
                if (j == i || std::abs(agents_.height[j] - h) > lp.levelSeparation) {
                    return;
                }
                const glm::vec2 d = agents_.position[j] - p;
                const float d2 = glm::dot(d, d);
                if (d2 >= rangeSq) {
                    return;
                }
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
                neighbors[slot] =
                    orca::Neighbor{agents_.position[j], agents_.velocity[j], agents_.radius[j] + lp.personalSpace, j};
            });

            // Nearby navmesh boundary edges on this level, nearest first.
            const float radius = agents_.radius[i] + lp.personalSpace;
            std::uint32_t numWalls = 0;
            if (useNav) {
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
                                        radius, agents_.maxSpeed[i], i};
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

    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t thread) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            agents_.prevPosition[i] = agents_.position[i];
            agents_.prevHeading[i] = agents_.heading[i];
            agents_.prevHeight[i] = agents_.height[i];

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
                // Acceleration-limited velocity change towards the avoidance result.
                glm::vec2 dv = agents_.newVelocity[i] - agents_.velocity[i];
                const float maxDv = agents_.maxAcceleration[i] * dt;
                const float dvLenSq = glm::dot(dv, dv);
                if (dvLenSq > maxDv * maxDv) {
                    dv *= maxDv / std::sqrt(dvLenSq);
                }
                glm::vec2 v = agents_.velocity[i] + dv;
                const glm::vec2 start2 = agents_.position[i];
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
                            v = (moved - start2) / dt;  // slid along a boundary
                        }
                        end2 = moved;
                    }
                }
                agents_.velocity[i] = v;
                agents_.position[i] = end2;

                // Start traversing an off-mesh link when the flow field leads onto one.
                if (useNav && detour != nullptr) {
                    const std::uint32_t dense = navMesh_.denseIndex(agents_.navPoly[i]);
                    const FlowField& ff = flowFields_[agents_.goalIndex[i]];
                    if (dense != kNoPoly && ff.next[dense] != kNoPoly && navMesh_.isOffMeshConnection(ff.next[dense])) {
                        float linkA[3];
                        float linkB[3];
                        if (dtStatusSucceed(detour->getOffMeshConnectionPolyEndPoints(
                                agents_.navPoly[i], navMesh_.refAt(ff.next[dense]), linkA, linkB))) {
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

            const glm::vec3 p3(agents_.position[i].x, agents_.height[i], agents_.position[i].y);
            agents_.arrived[i] =
                agents_.linkActive[i] == 0 && goalAreas_[agents_.goalIndex[i]].contains3d(p3, kGoalVerticalTolerance)
                    ? 1
                    : 0;
        }
    });
}

void Simulation::removeArrivedAgents() {
    CS_PROFILE_ZONE();
    // Descending order: the element swapped into slot i always comes from a higher,
    // already-checked index, so every arrived agent is removed exactly once.
    for (std::uint32_t i = agents_.size(); i-- > 0;) {
        if (agents_.arrived[i] != 0) {
            registry_.get<Goal>(goals_[agents_.goalIndex[i]]).arrivals += 1;
            ++arrivedTotal_;
            agents_.swapRemove(i);
        }
    }
}

SimStats Simulation::stats() const noexcept {
    SimStats s;
    s.alive = agents_.size();
    s.spawned = spawnedTotal_;
    s.arrived = arrivedTotal_;
    double speedSum = 0.0;
    for (const glm::vec2& v : agents_.velocity) {
        speedSum += std::sqrt(static_cast<double>(glm::dot(v, v)));
    }
    s.averageSpeed = agents_.empty() ? 0.0f : static_cast<float>(speedSum / agents_.size());
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
    return hash.value();
}

std::shared_ptr<const snapshot::StaticScene> Simulation::buildStaticScene() const {
    auto scene = std::make_shared<snapshot::StaticScene>();
    scene->version = staticVersion_;
    for (const geometry::RenderVertex& v : geometry::toRenderVertices(staticMesh_)) {
        scene->meshVertices.push_back({v.x, v.y, v.z, v.nx, v.ny, v.nz, v.abgr});
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
            a.speed = std::sqrt(glm::dot(v, v));
            a.agentType = agents_.agentType[i];
            a.state = agents_.linkActive[i] != 0 ? 1 : 0;
            a.agentId = agents_.id[i];
        }
    });

    out.areas.clear();
    for (entt::entity e : spawners_) {
        const Area& area = registry_.get<Spawner>(e).area;
        out.areas.push_back({area.center3d(), area.halfExtents, glm::vec4(0.95f, 0.65f, 0.2f, 0.35f)});
    }
    for (entt::entity e : goals_) {
        const Area& area = registry_.get<Goal>(e).area;
        out.areas.push_back({area.center3d(), area.halfExtents, glm::vec4(0.25f, 0.85f, 0.45f, 0.35f)});
    }

    if (!staticSceneCache_ || staticSceneCache_->version != staticVersion_) {
        staticSceneCache_ = buildStaticScene();
    }
    out.staticScene = staticSceneCache_;
}

}  // namespace crowdsim::sim
