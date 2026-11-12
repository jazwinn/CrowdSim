#pragma once

/// @file EditorShell.hpp
/// The dockable editor layout: dockspace, simulation controls, stats and log.
/// The shell never mutates engine state itself: it returns requests that the
/// app turns into commands (the command layer lands in M3).

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
    // Frame timing (measured by the app)
    float frameMs = 0.0f;
    float simStepMs = 0.0f;
    // Environment
    std::string_view rendererName;
    std::uint32_t jobThreads = 0;
    std::uint32_t framebufferWidth = 0;
    std::uint32_t framebufferHeight = 0;
    // Log lines, oldest first
    std::span<const std::string> logLines;
};

/// What the user asked for this frame.
struct EditorRequests {
    bool togglePlay = false;
    bool stepOnce = false;
    bool reset = false;
    bool quit = false;
    bool screenshot = false;
    float simSpeed = 1.0f;
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
};

}  // namespace crowdsim::ui
