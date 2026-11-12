#include "crowdsim/sim/Simulation.hpp"

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

}  // namespace

Simulation::Simulation(SimServices services, const SimConfig& config)
    : services_(services), config_(config), stepSeconds_(1.0 / config.stepHz), rng_(config.seed) {
    config_.locomotion.maxNeighbors = std::min(config_.locomotion.maxNeighbors, orca::kMaxNeighbors);
    services_.log.debug("Simulation created (seed={}, {} Hz)", config_.seed, config_.stepHz);
}

std::uint32_t Simulation::addGoal(const Goal& goal) {
    const entt::entity e = registry_.create();
    registry_.emplace<Goal>(e, goal);
    goals_.push_back(e);
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
    goals_.clear();
    spawners_.clear();
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

void Simulation::step() {
    CS_PROFILE_ZONE();
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

    for (entt::entity e : spawners_) {
        Spawner& s = registry_.get<Spawner>(e);
        if (now < s.startTime || now >= s.stopTime || s.goal >= goalAreas_.size()) {
            continue;
        }
        s.accumulator += s.rate * dt;
        while (s.accumulator >= 1.0f && (s.total == 0 || s.spawned < s.total)) {
            const AgentParams& p = s.agent;
            const float radius = rng_.range(p.radiusMin, p.radiusMax);

            // Rejection-sample a free spot; give up for this step if the area is crowded.
            bool placed = false;
            glm::vec2 pos{};
            for (std::uint32_t attempt = 0; attempt < kSpawnAttempts && !placed; ++attempt) {
                pos = s.area.center + glm::vec2(rng_.range(-1.0f, 1.0f) * s.area.halfExtents.x,
                                                rng_.range(-1.0f, 1.0f) * s.area.halfExtents.y);
                bool blocked = false;
                hash_.query(pos, 2.0f * radius, [&](std::uint32_t j) {
                    const float minDist = radius + agents_.radius[j];
                    blocked =
                        blocked || glm::dot(agents_.position[j] - pos, agents_.position[j] - pos) < minDist * minDist;
                });
                for (std::uint32_t j = firstNew; j < agents_.size() && !blocked; ++j) {
                    const float minDist = radius + agents_.radius[j];
                    blocked = glm::dot(agents_.position[j] - pos, agents_.position[j] - pos) < minDist * minDist;
                }
                placed = !blocked;
            }
            if (!placed) {
                break;  // keep the accumulator; retry next step
            }

            const Area& goalArea = goalAreas_[s.goal];
            const glm::vec2 target = goalArea.clamp(pos, 0.5f);
            const float prefSpeed = rng_.range(p.preferredSpeedMin, p.preferredSpeedMax);
            const glm::vec3 jitter(rng_.range(-1.0f, 1.0f), rng_.range(-1.0f, 1.0f), rng_.range(-1.0f, 1.0f));

            AgentInit init;
            init.id = nextAgentId_++;
            init.position = pos;
            init.heading = headingOf(target - pos);
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
    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            const glm::vec2 toTarget = agents_.goalTarget[i] - agents_.position[i];
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
    const orca::Params params{lp.timeHorizon, dt};

    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t) {
        std::array<orca::Neighbor, orca::kMaxNeighbors> neighbors{};
        std::array<float, orca::kMaxNeighbors> distSq{};

        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            const glm::vec2 p = agents_.position[i];
            std::uint32_t count = 0;

            // Keep the K nearest, ordered by (distance, index) so ties are deterministic.
            hash_.query(p, lp.neighborDistance, [&](std::uint32_t j) {
                if (j == i) {
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

            const orca::AgentState self{p,
                                        agents_.velocity[i],
                                        agents_.preferredVelocity[i],
                                        agents_.radius[i] + lp.personalSpace,
                                        agents_.maxSpeed[i],
                                        i};
            agents_.newVelocity[i] = orca::computeNewVelocity(self, std::span(neighbors.data(), count), params);
        }
    });
}

void Simulation::integrate(float dt) {
    CS_PROFILE_ZONE();
    const float maxTurn = config_.locomotion.maxTurnRate * dt;
    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            agents_.prevPosition[i] = agents_.position[i];
            agents_.prevHeading[i] = agents_.heading[i];

            // Acceleration-limited velocity change towards the avoidance result.
            glm::vec2 dv = agents_.newVelocity[i] - agents_.velocity[i];
            const float maxDv = agents_.maxAcceleration[i] * dt;
            const float dvLenSq = glm::dot(dv, dv);
            if (dvLenSq > maxDv * maxDv) {
                dv *= maxDv / std::sqrt(dvLenSq);
            }
            const glm::vec2 v = agents_.velocity[i] + dv;
            agents_.velocity[i] = v;
            agents_.position[i] += v * dt;

            // Turn the body towards the direction of travel at a bounded rate.
            const float speedSq = glm::dot(v, v);
            if (speedSq > 0.05f * 0.05f) {
                const float diff = wrapAngle(headingOf(v) - agents_.heading[i]);
                agents_.heading[i] = wrapAngle(agents_.heading[i] + std::clamp(diff, -maxTurn, maxTurn));
            }

            agents_.arrived[i] = goalAreas_[agents_.goalIndex[i]].contains(agents_.position[i]) ? 1 : 0;
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
    hash.addBytes(std::as_bytes(std::span(agents_.velocity)));
    hash.addBytes(std::as_bytes(std::span(agents_.heading)));
    return hash.value();
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
            a.prevPosition = glm::vec3(prev.x, 0.0f, prev.y);
            a.prevHeading = agents_.prevHeading[i];
            a.position = glm::vec3(cur.x, 0.0f, cur.y);
            a.heading = agents_.heading[i];
            a.color = agents_.color[i];
            a.radius = agents_.radius[i];
            a.speed = std::sqrt(glm::dot(v, v));
            a.agentType = agents_.agentType[i];
            a.state = 0;
            a.agentId = agents_.id[i];
        }
    });

    out.areas.clear();
    for (entt::entity e : spawners_) {
        const Area& area = registry_.get<Spawner>(e).area;
        out.areas.push_back(
            {glm::vec3(area.center.x, 0.0f, area.center.y), area.halfExtents, glm::vec4(0.95f, 0.65f, 0.2f, 0.35f)});
    }
    for (entt::entity e : goals_) {
        const Area& area = registry_.get<Goal>(e).area;
        out.areas.push_back(
            {glm::vec3(area.center.x, 0.0f, area.center.y), area.halfExtents, glm::vec4(0.25f, 0.85f, 0.45f, 0.35f)});
    }
}

}  // namespace crowdsim::sim
