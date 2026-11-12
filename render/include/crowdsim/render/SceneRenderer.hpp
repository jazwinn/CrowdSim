#pragma once

/// @file SceneRenderer.hpp
/// Draws the world from a render snapshot: ground grid, spawner/goal areas and
/// instanced agents, interpolated between the snapshot's previous and current
/// step. Read-only with respect to the snapshot; never touches the simulation.

#include <cstdint>
#include <memory>

#include "crowdsim/render/Camera.hpp"

namespace crowdsim::snapshot {
struct RenderSnapshot;
}

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

class SceneRenderer {
public:
    SceneRenderer();
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    /// Submit the scene to the scene view. `alpha` in [0,1] blends previous -> current step.
    SceneStats render(const snapshot::RenderSnapshot& snap, float alpha, const Camera& camera, const ViewRect& rect);

    bool showAreas = true;
    bool showGround = true;

private:
    struct Resources;
    std::unique_ptr<Resources> res_;
};

}  // namespace crowdsim::render
