#pragma once

/// @file EditorShell.hpp
/// The dockable editor layout: dockspace, viewport, simulation controls, stats
/// and log. The shell never mutates engine state itself: it returns requests
/// that the app turns into commands (the command layer lands in M3).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace crowdsim::ui {

/// Read-only data the shell displays this frame.
struct EditorFrameData {
    // Simulation
    bool playing = true;
    std::uint64_t stepIndex = 0;
    double simTimeSeconds = 0.0;
    double stepHz = 30.0;
    std::uint64_t stateHash = 0;
    float simSpeed = 1.0f;
    std::uint64_t seed = 0;
    // Crowd
    std::span<const std::string_view> scenarioNames;
    int scenarioIndex = 0;
    std::uint32_t scenarioAgents = 0;
    std::uint32_t agentsAlive = 0;
    std::uint64_t agentsSpawned = 0;
    std::uint64_t agentsArrived = 0;
    float averageSpeed = 0.0f;
    // Frame timing (measured by the app)
    float frameMs = 0.0f;
    float simStepMs = 0.0f;
    float renderSubmitMs = 0.0f;
    // Renderer
    std::string_view rendererName;
    std::uint32_t agentsDrawn = 0;
    std::uint32_t drawCalls = 0;
    std::uint32_t jobThreads = 0;
    std::uint32_t framebufferWidth = 0;
    std::uint32_t framebufferHeight = 0;
    // Log lines, oldest first
    std::span<const std::string> logLines;
};

/// Rectangle in framebuffer pixels.
struct PixelRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

/// What the user asked for this frame.
struct EditorRequests {
    bool togglePlay = false;
    bool stepOnce = false;
    bool reset = false;
    bool quit = false;
    bool screenshot = false;
    float simSpeed = 1.0f;
    /// Rebuild the scenario at `scenarioIndex` with `scenarioAgents` agents.
    bool rebuildScenario = false;
    int scenarioIndex = 0;
    std::uint32_t scenarioAgents = 0;
    // Viewport
    PixelRect viewport;            ///< where the 3D view should be drawn
    bool viewportHovered = false;  ///< mouse is over the 3D view and not over a widget
    bool showAreas = true;
    bool frameAll = false;  ///< reset the camera
};

class EditorShell {
public:
    /// Build all panels. Call between ImGui::NewFrame() and ImGui::Render().
    EditorRequests draw(const EditorFrameData& data);

private:
    void buildDefaultLayout(unsigned int dockspaceId);

    bool layoutBuilt_ = false;
    bool showDemo_ = false;
    bool autoScrollLog_ = true;
    bool showAreas_ = true;
    bool viewportDragging_ = false;
    int pendingScenario_ = -1;
    int pendingAgents_ = -1;
};

}  // namespace crowdsim::ui
