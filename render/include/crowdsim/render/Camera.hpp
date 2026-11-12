#pragma once

/// @file Camera.hpp
/// Orbit camera description. Matrix construction lives in the renderer, which
/// knows the backend's depth convention.

#include <glm/vec3.hpp>

namespace crowdsim::render {

struct Camera {
    glm::vec3 target{0.0f, 0.0f, 0.0f};
    float yaw = 0.6f;     ///< radians about +Y
    float pitch = 0.75f;  ///< radians above the horizon
    float distance = 120.0f;
    float fovYDegrees = 50.0f;
    float nearPlane = 0.1f;
    float farPlane = 2000.0f;

    /// World-space eye position (left-handed, +Y up).
    [[nodiscard]] glm::vec3 eye() const;
};

}  // namespace crowdsim::render
