#pragma once

/// @file SceneRenderer.hpp
/// Draws the world from a render snapshot: static environment meshes, a
/// reference grid, spawner/goal areas, instanced agents (interpolated between
/// the snapshot's previous and current step) and optional debug overlays.
/// Read-only with respect to the snapshot; never touches the simulation.

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/vec3.hpp>

#include "crowdsim/render/Camera.hpp"

namespace crowdsim::snapshot {
struct RenderSnapshot;
struct StaticScene;
}  // namespace crowdsim::snapshot

namespace crowdsim::render {

/// Pixel rectangle of the 3D view inside the framebuffer.
struct ViewRect {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

struct SceneStats {
    std::uint32_t agentsDrawn = 0;
    std::uint32_t drawCalls = 0;
};

/// A coloured line segment drawn this frame (selection outlines, gizmos, debug).
struct DebugLine {
    glm::vec3 a;
    glm::vec3 b;
    std::uint32_t abgr = 0xffffffffu;
};

/// Toggleable layers.
struct Overlays {
    bool ground = true;      ///< reference grid below the scene
    bool areas = true;       ///< spawner / goal rectangles
    bool navmesh = false;    ///< navmesh polygons and boundary walls
    bool flowField = false;  ///< flow arrows for `flowGoal`
    int flowGoal = 0;
    bool links = true;        ///< off-mesh links
    bool velocities = false;  ///< per-agent velocity lines
};

class SceneRenderer {
public:
    SceneRenderer();
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    /// Submit the scene to the scene view. `alpha` in [0,1] blends previous -> current step.
    SceneStats render(const snapshot::RenderSnapshot& snap, float alpha, const Camera& camera, const ViewRect& rect);

    Overlays overlays;
    /// Extra lines for this frame; the caller refills it every frame.
    std::vector<DebugLine> extraLines;

private:
    void uploadStatic(const snapshot::StaticScene& scene);

    struct Resources;
    std::unique_ptr<Resources> res_;
};

}  // namespace crowdsim::render
