#pragma once

/// @file PhysicsWorld.hpp
/// Physics/collision interface. The simulation's agents are not physics
/// bodies; this world holds environment collision for raycasts (editor
/// picking, spawn snapping, line of sight) and, later, dynamic obstacles.

#include <cstdint>
#include <memory>
#include <optional>

#include <glm/vec3.hpp>

#include "crowdsim/geometry/TriangleMesh.hpp"

namespace spdlog {
class logger;
}

namespace crowdsim::physics {

struct BodyId {
    std::uint32_t value = 0xFFFFFFFFu;
    [[nodiscard]] bool valid() const noexcept { return value != 0xFFFFFFFFu; }
};

struct RayHit {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    BodyId body;
};

class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;

    /// Remove every body.
    virtual void clear() = 0;

    /// Add static, double-sided-for-queries triangle collision. Returns an invalid id on failure.
    virtual BodyId addStaticMesh(const geometry::TriangleMesh& mesh) = 0;

    /// Closest hit along `direction` (need not be normalised) within `maxDistance`.
    [[nodiscard]] virtual std::optional<RayHit> raycast(glm::vec3 origin, glm::vec3 direction,
                                                        float maxDistance) const = 0;

    [[nodiscard]] virtual std::uint32_t bodyCount() const = 0;
};

/// Process-wide Jolt registration (allocator hooks, type factory). Jolt keeps
/// these in library globals; this RAII object is the single owner, created by
/// the composition root before any JoltPhysicsWorld and destroyed after them.
class JoltRuntime {
public:
    JoltRuntime();
    ~JoltRuntime();
    JoltRuntime(const JoltRuntime&) = delete;
    JoltRuntime& operator=(const JoltRuntime&) = delete;
};

/// Create a Jolt-backed world. Requires a live JoltRuntime.
[[nodiscard]] std::unique_ptr<IPhysicsWorld> createJoltPhysicsWorld(const JoltRuntime& runtime, spdlog::logger& log,
                                                                    std::uint32_t maxBodies = 4096);

}  // namespace crowdsim::physics
