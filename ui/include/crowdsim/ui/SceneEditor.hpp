#pragma once

/// @file SceneEditor.hpp
/// Scene editing panels: hierarchy, inspector, gizmo and command history. The
/// editor reads the simulation (const) and expresses every change as a command
/// (JSON) for the app to submit, so the GUI, Lua, MCP and replay share one path.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace crowdsim::sim {
class Simulation;
}
namespace crowdsim::commands {
class CommandDispatcher;
}

namespace crowdsim::ui {

/// Everything the editor reads in a frame.
struct SceneEditorInput {
    const sim::Simulation& sim;
    const commands::CommandDispatcher& dispatcher;
    const float* view = nullptr;  ///< camera view matrix (bx layout)
    const float* proj = nullptr;  ///< camera projection matrix
    float viewportX = 0.0f;       ///< 3D viewport in ImGui (logical) pixels
    float viewportY = 0.0f;
    float viewportW = 0.0f;
    float viewportH = 0.0f;
    bool hasCursorWorld = false;
    float cursorWorld[3] = {0.0f, 0.0f, 0.0f};
    float cameraTarget[3] = {0.0f, 0.0f, 0.0f};  ///< fallback placement point
};

class SceneEditor {
public:
    /// Draw the Scene (hierarchy) panel contents into the current window.
    void drawHierarchy(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);
    /// Draw the Inspector panel contents into the current window.
    void drawInspector(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);
    /// Draw the History panel contents into the current window.
    void drawHistory(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);
    /// Draw the transform gizmo over the viewport (call once per frame, outside windows).
    void drawGizmo(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);

    /// Shortcuts: Delete removes the selection; Ctrl+D duplicates it.
    void handleShortcuts(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);

    [[nodiscard]] std::uint32_t selected() const noexcept { return selected_; }
    void select(std::uint32_t id) noexcept { selected_ = id; }
    /// True while the gizmo is hovered or dragged (the app must not orbit/pick then).
    [[nodiscard]] bool gizmoActive() const noexcept { return gizmoActive_; }

    bool snap = true;
    float translateSnap = 0.25f;
    float rotateSnapDegrees = 15.0f;

private:
    void addMenu(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);
    bool editJson(const char* label, nlohmann::json& value, nlohmann::json& patch, const std::string& path);

    std::uint32_t selected_ = 0;
    bool gizmoActive_ = false;
    bool gizmoTranslate_ = true;
    std::uint32_t dragSerial_ = 0;
    bool wasUsingGizmo_ = false;
    char importPath_[256] = "models/test_room.gltf";
    nlohmann::json buffer_;  ///< inspector working copy of the selected entity
    std::uint32_t bufferId_ = 0;
};

}  // namespace crowdsim::ui
