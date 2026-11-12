#pragma once

/// @file AgentPool.hpp
/// Structure-of-arrays storage for agents. Every hot loop (neighbour search,
/// ORCA, integration, snapshot) walks these contiguous columns by index, which
/// keeps them cache-friendly, trivially parallel and SIMD-ready. Removal is
/// swap-and-pop, which is deterministic given a deterministic removal order.
/// See ADR 0004.

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace crowdsim::sim {

/// Everything needed to create one agent.
struct AgentInit {
    std::uint32_t id = 0;
    glm::vec2 position{0.0f};
    float heading = 0.0f;
    float radius = 0.3f;
    float preferredSpeed = 1.3f;
    float maxSpeed = 1.8f;
    float maxAcceleration = 3.0f;
    glm::vec3 color{1.0f};
    float height = 0.0f;         ///< world Y (floor height under the agent)
    std::uint32_t navPoly = 0;   ///< Detour polygon ref, 0 = not on the navmesh
    glm::vec3 goalTarget{0.0f};  ///< point inside the goal area (on the navmesh)
    std::uint16_t goalIndex = 0;
    std::uint16_t agentType = 0;
    float spawnTime = 0.0f;
};

class AgentPool {
public:
    // Columns (all the same length). Positions are on the ground plane: x = world X, y = world Z.
    std::vector<std::uint32_t> id;
    std::vector<glm::vec2> position;
    std::vector<glm::vec2> prevPosition;
    std::vector<glm::vec2> velocity;
    std::vector<glm::vec2> preferredVelocity;
    std::vector<glm::vec2> newVelocity;
    std::vector<float> heading;
    std::vector<float> prevHeading;
    std::vector<float> radius;
    std::vector<float> preferredSpeed;
    std::vector<float> maxSpeed;
    std::vector<float> maxAcceleration;
    std::vector<glm::vec3> color;
    std::vector<float> height;
    std::vector<float> prevHeight;
    std::vector<std::uint32_t> navPoly;
    std::vector<glm::vec3> goalTarget;
    std::vector<std::uint16_t> goalIndex;
    std::vector<std::uint16_t> agentType;
    std::vector<float> spawnTime;
    std::vector<std::uint8_t> arrived;
    // Off-mesh link traversal (ladders, escalators): active while linkActive != 0.
    std::vector<std::uint8_t> linkActive;
    std::vector<float> linkProgress;
    std::vector<glm::vec3> linkStart;
    std::vector<glm::vec3> linkEnd;
    std::vector<std::uint32_t> linkEndPoly;

    [[nodiscard]] std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(id.size()); }
    [[nodiscard]] bool empty() const noexcept { return id.empty(); }

    void reserve(std::size_t n) {
        forEachColumn([n](auto& column) { column.reserve(n); });
    }

    void clear() {
        forEachColumn([](auto& column) { column.clear(); });
    }

    void add(const AgentInit& a) {
        id.push_back(a.id);
        position.push_back(a.position);
        prevPosition.push_back(a.position);
        velocity.push_back(glm::vec2(0.0f));
        preferredVelocity.push_back(glm::vec2(0.0f));
        newVelocity.push_back(glm::vec2(0.0f));
        heading.push_back(a.heading);
        prevHeading.push_back(a.heading);
        radius.push_back(a.radius);
        preferredSpeed.push_back(a.preferredSpeed);
        maxSpeed.push_back(a.maxSpeed);
        maxAcceleration.push_back(a.maxAcceleration);
        color.push_back(a.color);
        height.push_back(a.height);
        prevHeight.push_back(a.height);
        navPoly.push_back(a.navPoly);
        goalTarget.push_back(a.goalTarget);
        goalIndex.push_back(a.goalIndex);
        agentType.push_back(a.agentType);
        spawnTime.push_back(a.spawnTime);
        arrived.push_back(0);
        linkActive.push_back(0);
        linkProgress.push_back(0.0f);
        linkStart.push_back(glm::vec3(0.0f));
        linkEnd.push_back(glm::vec3(0.0f));
        linkEndPoly.push_back(0);
    }

    /// Remove agent `i` by moving the last agent into its slot.
    void swapRemove(std::uint32_t i) {
        forEachColumn([i](auto& column) {
            column[i] = column.back();
            column.pop_back();
        });
    }

private:
    template <typename F>
    void forEachColumn(F&& f) {
        f(id);
        f(position);
        f(prevPosition);
        f(velocity);
        f(preferredVelocity);
        f(newVelocity);
        f(heading);
        f(prevHeading);
        f(radius);
        f(preferredSpeed);
        f(maxSpeed);
        f(maxAcceleration);
        f(color);
        f(height);
        f(prevHeight);
        f(navPoly);
        f(goalTarget);
        f(goalIndex);
        f(agentType);
        f(spawnTime);
        f(arrived);
        f(linkActive);
        f(linkProgress);
        f(linkStart);
        f(linkEnd);
        f(linkEndPoly);
    }
};

}  // namespace crowdsim::sim
