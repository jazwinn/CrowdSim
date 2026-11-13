#pragma once

/// @file EditorShell.hpp
/// The dockable editor layout: dockspace, viewport, simulation controls, stats
/// and log. The shell never mutates engine state itself: it returns requests
/// that the app turns into commands (the command layer lands in M3).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/ui/BtEditor.hpp"
#include "crowdsim/ui/MetricsPanel.hpp"
#include "crowdsim/ui/SceneEditor.hpp"
#include "crowdsim/ui/Theme.hpp"

namespace crowdsim::ui {

/// Debug-draw layers the user can toggle (mirrors render::Overlays).
struct OverlayToggles {
    bool ground = true;
    bool areas = true;
    bool navmesh = false;
    bool flowField = false;
    int flowGoal = 0;
    bool links = true;
    bool velocities = false;
    // Crowd rendering (mirrors render::RenderSettings)
    bool animatedCharacters = true;
    bool shadows = true;
    float lodNearDistance = 30.0f;
    float lodFarDistance = 90.0f;
    int heatmap = 0;  ///< 0 off, 1 density, 2 speed
    bool routes = true;  ///< spawner-to-goal arcs (one colour per goal)
};

/// A loaded Lua script, as the Script tab lists it.
struct ScriptView {
    std::string_view name;
    std::string_view source;
    bool ok = true;
    std::string_view error;
};

/// A recent Lua error (load, node tick, entity callback, event handler).
struct ScriptErrorView {
    std::string_view where;
    std::string_view message;
    std::uint64_t step = 0;
};

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
    // Scene / navigation
    std::uint32_t staticObjects = 0;
    std::uint32_t navPolygons = 0;
    std::uint32_t navTiles = 0;
    std::uint32_t goals = 0;
    bool hasCursorWorld = false;
    float cursorWorld[3] = {0.0f, 0.0f, 0.0f};
    // Frame timing (measured by the app)
    float frameMs = 0.0f;
    float simStepMs = 0.0f;
    float renderSubmitMs = 0.0f;
    // Renderer
    std::string_view rendererName;
    std::uint32_t agentsDrawn = 0;
    std::uint32_t drawCalls = 0;
    std::uint32_t agentsNear = 0;
    std::uint32_t agentsMid = 0;
    std::uint32_t agentsFar = 0;
    std::uint32_t agentsCulled = 0;
    std::uint32_t shadowCasters = 0;
    float crowdPrepareMs = 0.0f;
    std::uint32_t jobThreads = 0;
    std::uint32_t framebufferWidth = 0;
    std::uint32_t framebufferHeight = 0;
    // Log lines, oldest first
    std::span<const std::string> logLines;
    // Video recording of the 3D view (no UI)
    bool recordingSupported = false;
    bool recording = false;
    double recordingSeconds = 0.0;
    std::string_view recordingFile;
    // Scripts (Script tab)
    std::span<const ScriptView> scripts;
    std::span<const ScriptErrorView> scriptErrors;
    // Scene editing
    const SceneEditorInput* editor = nullptr;  ///< null disables editing panels
    std::span<const std::string> sceneFiles;   ///< content-relative *.scene.json files
    std::string_view sceneName;
    const metrics::MetricsCollector* metrics = nullptr;  ///< for the Metrics panel
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
    bool toggleRecording = false;  ///< start or stop recording the 3D view
    float simSpeed = 1.0f;
    /// Rebuild the scenario at `scenarioIndex` with `scenarioAgents` agents.
    bool rebuildScenario = false;
    int scenarioIndex = 0;
    std::uint32_t scenarioAgents = 0;
    // Viewport
    PixelRect viewport;            ///< where the 3D view should be drawn
    bool viewportHovered = false;  ///< mouse is over the 3D view and not over a widget
    OverlayToggles overlays;
    bool frameAll = false;  ///< reset the camera
    /// Commands produced by editing this frame (submit to the dispatcher in order).
    std::vector<nlohmann::json> commands;
    bool saveCommandLog = false;
};

class EditorShell {
public:
    explicit EditorShell(const OverlayToggles& initialOverlays = {});
    ~EditorShell();
    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    /// Build all panels. Call between ImGui::NewFrame() and ImGui::Render().
    EditorRequests draw(const EditorFrameData& data);

private:
    void buildDefaultLayout(unsigned int dockspaceId);
    void fileMenu(const EditorFrameData& data, EditorRequests& req);
    void drawScriptPanel(const EditorFrameData& data, EditorRequests& req);
    void drawScriptList(const EditorFrameData& data, EditorRequests& req);
    void drawConsole(EditorRequests& req);
    void drawScriptEditor(const EditorFrameData& data, EditorRequests& req);
    void openScript(const ScriptView& script);

public:
    [[nodiscard]] SceneEditor& sceneEditor() noexcept { return sceneEditor_; }
    [[nodiscard]] BtEditor& behaviourEditor() noexcept { return btEditor_; }
    /// Bring the Behaviour (node editor) panel to the front next frame.
    void focusBehaviourEditor() noexcept { focusBehaviour_ = true; }
    /// Bring the Metrics panel to the front next frame.
    void focusMetrics() noexcept { focusMetrics_ = 3; }
    /// Fonts for headings and code (from ui::loadFonts); also handed to the scene editor.
    void setFonts(const ThemeFonts& fonts) noexcept {
        fonts_ = fonts;
        sceneEditor_.setFonts(fonts);
    }
    /// Result of a script load / save / reload, shown in the Script tab.
    void scriptStatus(const std::string& text, bool error = false) {
        scriptStatus_ = text;
        scriptStatusError_ = error;
    }
    /// Append a result / error to the Script console transcript.
    void consoleOutput(const std::string& text, bool error = false);

private:
    SceneEditor sceneEditor_;
    BtEditor btEditor_;
    MetricsPanel metricsPanel_;
    ThemeFonts fonts_;
    char saveAsName_[128] = "my_scene";

    bool layoutBuilt_ = false;
    bool showDemo_ = false;
    bool autoScrollLog_ = true;
    char consoleInput_[8192] = "print(agents.count(), sim.time())";
    struct ConsoleLine {
        std::string text;
        bool error = false;
    };
    std::vector<ConsoleLine> consoleLines_;
    bool consoleScroll_ = false;
    // Script tab: the open script's working copy and the list's inputs.
    std::string openScript_;
    std::string scriptBuffer_;
    std::string scriptLoaded_;  ///< source the buffer was opened from (to notice reloads)
    bool scriptDirty_ = false;
    bool selectScriptTab_ = false;
    char newScriptName_[64] = "";
    char loadScriptPath_[256] = "scripts/";
    std::string scriptStatus_;
    bool scriptStatusError_ = false;
    bool focusBehaviour_ = false;
    int focusMetrics_ = 0;  ///< frames left to keep the Metrics tab in front (docking settles first)
    OverlayToggles overlays_;
    bool viewportDragging_ = false;
    int pendingScenario_ = -1;
    int pendingAgents_ = -1;
};

}  // namespace crowdsim::ui
