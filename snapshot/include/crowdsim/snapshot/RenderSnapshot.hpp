#pragma once

/// @file RenderSnapshot.hpp
/// Everything the renderer needs from one simulation step. The simulation
/// writes a snapshot at the end of every step; the renderer interpolates
/// between each agent's previous and current state using the frame's alpha.
/// Snapshots are plain data: no pointers into the simulation.

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace crowdsim::snapshot {

/// One agent, in a layout that maps directly onto GPU instance data.
struct AgentInstance {
    glm::vec3 prevPosition{0.0f};  ///< position at the previous step
    float prevHeading = 0.0f;      ///< radians about +Y, 0 = facing +Z
    glm::vec3 position{0.0f};      ///< position at this step
    float heading = 0.0f;
    glm::vec3 color{1.0f};
    float radius = 0.3f;
    float speed = 0.0f;  ///< m/s, drives animation playback later
    std::uint16_t agentType = 0;
    std::uint16_t state = 0;  ///< behaviour state id, for debug colouring
    std::uint32_t agentId = 0;
};

/// An axis-aligned rectangle on a floor, used for spawner/goal gizmos.
struct AreaMarker {
    glm::vec3 center{0.0f};
    glm::vec2 halfExtents{1.0f};
    glm::vec4 color{1.0f};
};

struct RenderSnapshot {
    std::uint64_t stepIndex = 0;
    double simTimeSeconds = 0.0;
    std::vector<AgentInstance> agents;
    std::vector<AreaMarker> areas;

    void clear() {
        agents.clear();
        areas.clear();
    }
};

}  // namespace crowdsim::snapshot
