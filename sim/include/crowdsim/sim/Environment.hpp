#pragma once

/// @file Environment.hpp
/// Environment entities (ECS components): spawners and goals. These are few in
/// number and live in the EnTT registry; agents live in the AgentPool.

#include <cstdint>
#include <limits>
#include <string>

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace crowdsim::sim {

/// Axis-aligned rectangle on the ground plane (x = world X, y = world Z).
struct Area {
    glm::vec2 center{0.0f};
    glm::vec2 halfExtents{1.0f};

    [[nodiscard]] bool contains(glm::vec2 p) const noexcept {
        const glm::vec2 d = glm::abs(p - center);
        return d.x <= halfExtents.x && d.y <= halfExtents.y;
    }
    [[nodiscard]] glm::vec2 clamp(glm::vec2 p, float margin = 0.0f) const noexcept {
        const glm::vec2 h = glm::max(halfExtents - glm::vec2(margin), glm::vec2(0.0f));
        return glm::clamp(p, center - h, center + h);
    }
};

/// Per-agent parameters drawn when an agent is spawned. Ranges are sampled
/// uniformly with the world RNG, so variation is reproducible from the seed.
struct AgentParams {
    float radiusMin = 0.24f;
    float radiusMax = 0.30f;
    float preferredSpeedMin = 1.15f;
    float preferredSpeedMax = 1.55f;
    float maxSpeedFactor = 1.35f;  ///< max speed = preferred speed * factor
    float maxAcceleration = 3.0f;  ///< m/s^2
    glm::vec3 color{0.85f, 0.55f, 0.25f};
    float colorJitter = 0.12f;
    std::uint16_t agentType = 0;
};

struct Goal {
    std::string name;
    Area area;
    // Runtime state (reset with the simulation)
    std::uint32_t arrivals = 0;
};

struct Spawner {
    std::string name;
    Area area;
    float rate = 10.0f;       ///< agents per second
    std::uint32_t total = 0;  ///< 0 = unlimited
    float startTime = 0.0f;   ///< seconds of sim time
    float stopTime = std::numeric_limits<float>::infinity();
    std::uint32_t goal = 0;  ///< index into Simulation goals
    AgentParams agent;
    // Runtime state (reset with the simulation)
    float accumulator = 0.0f;
    std::uint32_t spawned = 0;
};

}  // namespace crowdsim::sim
