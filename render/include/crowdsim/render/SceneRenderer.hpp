#pragma once

/// @file SceneRenderer.hpp
/// Draws the world from a render snapshot: static environment meshes, a
/// reference grid, spawner/goal areas, instanced agents (interpolated between
/// the snapshot's previous and current step) and optional debug overlays.
/// Read-only with respect to the snapshot; never touches the simulation.

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "crowdsim/render/Camera.hpp"
#include "crowdsim/render/SceneRecorder.hpp"

namespace crowdsim::snapshot {
struct RenderSnapshot;
struct StaticScene;
struct CharacterAsset;
}  // namespace crowdsim::snapshot
namespace crowdsim::core {
class JobSystem;
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
    std::uint32_t agentsNear = 0;  ///< full-detail animated characters
    std::uint32_t agentsMid = 0;   ///< reduced animated characters
    std::uint32_t agentsFar = 0;   ///< capsules
    std::uint32_t agentsCulled = 0;
    std::uint32_t shadowCasters = 0;
    float prepareMs = 0.0f;  ///< CPU time to cull, pick LODs and animate
};

/// Crowd rendering quality settings.
struct RenderSettings {
    bool animatedCharacters = true;  ///< VAT characters near/mid; capsules everywhere when off
    float lodNearDistance = 30.0f;   ///< full-detail characters within this distance (m)
    float lodFarDistance = 90.0f;    ///< reduced characters up to here, capsules beyond
    bool shadows = true;
    float shadowDistance = 70.0f;  ///< max half-size of the shadowed square around the camera target
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

/// A coloured grid on the floor (metrics heatmaps). Plain data: the caller fills
/// it and bumps `version`; cells at or below 2 % of `maxValue` stay clear.
struct HeatmapOverlay {
    int width = 0;  ///< 0 = nothing to draw
    int height = 0;
    float cell = 1.0f;
    glm::vec2 origin{0.0f};  ///< (x, z) corner of cell (0, 0)
    float elevation = 0.0f;
    std::vector<float> values;  ///< width * height, row-major by z
    float maxValue = 1.0f;
    std::uint64_t version = 0;
};

class SceneRenderer {
public:
    SceneRenderer();
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    /// Submit the scene to the scene view. `alpha` in [0,1] blends previous -> current step;
    /// `frameSeconds` advances character animation.
    SceneStats render(const snapshot::RenderSnapshot& snap, float alpha, const Camera& camera, const ViewRect& rect,
                      float frameSeconds = 1.0f / 60.0f);

    /// Character used for agents (VAT mesh LODs + baked clips); null = capsules only.
    void setCharacter(std::shared_ptr<const snapshot::CharacterAsset> character);
    /// Job system for parallel culling / animation (optional).
    void setJobSystem(core::JobSystem* jobs) noexcept { jobs_ = jobs; }
    /// True if this backend can render the shadow map (depth compare textures).
    [[nodiscard]] bool shadowsSupported() const noexcept;

    Overlays overlays;
    RenderSettings settings;
    HeatmapOverlay heatmap;
    /// Extra lines for this frame; the caller refills it every frame.
    std::vector<DebugLine> extraLines;
    /// When set (recording), the scene renders into this target at its full size
    /// instead of into the viewport rect of the window.
    OffscreenTarget offscreen;

private:
    void uploadStatic(const snapshot::StaticScene& scene);

    struct Resources;
    std::unique_ptr<Resources> res_;
    core::JobSystem* jobs_ = nullptr;
};

}  // namespace crowdsim::render
