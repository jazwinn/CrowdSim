#pragma once

/// @file Orca.hpp
/// Optimal Reciprocal Collision Avoidance (van den Berg, Guy, Snape, Lin,
/// Manocha: "Reciprocal n-body Collision Avoidance", ISRR 2011), agent-agent
/// case. Pure functions on fixed-capacity arrays: no allocation, no virtual
/// calls, safe to run in parallel per agent.

#include <cstdint>
#include <span>

#include <glm/vec2.hpp>

namespace crowdsim::sim::orca {

inline constexpr std::uint32_t kMaxNeighbors = 16;
inline constexpr std::uint32_t kMaxWalls = 8;

/// A static obstacle edge on the ground plane.
struct Wall {
    glm::vec2 a;
    glm::vec2 b;
};

/// A neighbour as seen from the agent being solved.
struct Neighbor {
    glm::vec2 position;
    glm::vec2 velocity;
    float radius;
    std::uint32_t index;  ///< tie-breaker for degenerate (exact overlap) cases
};

struct AgentState {
    glm::vec2 position;
    glm::vec2 velocity;
    glm::vec2 preferredVelocity;
    float radius;
    float maxSpeed;
    std::uint32_t index;
};

struct Params {
    float timeHorizon = 2.0f;  ///< seconds of look-ahead for other agents
    float timeStep = 1.0f / 30.0f;
    float obstacleTimeHorizon = 0.5f;  ///< seconds of look-ahead for walls
    float wallClearance = 0.05f;       ///< m kept between agent centres and navmesh boundary edges
};

/// Velocity closest to `self.preferredVelocity` (within maxSpeed) that is
/// collision-free for `timeHorizon` under ORCA's reciprocity assumption. Walls
/// are hard constraints (at most kMaxWalls are used).
[[nodiscard]] glm::vec2 computeNewVelocity(const AgentState& self, std::span<const Neighbor> neighbors,
                                           std::span<const Wall> walls, const Params& params) noexcept;

}  // namespace crowdsim::sim::orca
