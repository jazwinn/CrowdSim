#pragma once

/// @file SceneEditor.hpp
/// Scene editing panels: hierarchy, inspector, gizmo and command history. The
/// editor reads the simulation (const) and expresses every change as a command
/// (JSON) for the app to submit, so the GUI, Lua, MCP and replay share one path.

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/ui/Theme.hpp"

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
    std::span<const std::string> entityTypes;    ///< Lua-registered entity types (Add menu)
};

class SceneEditor {
public:
    /// Draw the Scene (hierarchy) panel contents into the current window.
    void drawHierarchy(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);
    /// Draw the Inspector panel contents into the current window.
    void drawInspector(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);
    /// Draw the History panel contents into the current window.
    void drawHistory(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);
    /// Draw an arc from every spawner to its goal over the viewport, one colour per
    /// goal, with both areas outlined and labelled (call once per frame, outside windows).
    void drawRoutes(const SceneEditorInput& in) const;
    /// Draw the transform gizmo over the viewport (call once per frame, outside windows).
    void drawGizmo(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);

    /// Shortcuts: Delete removes the selection; Ctrl+D duplicates it; H hides or shows it.
    void handleShortcuts(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);

    [[nodiscard]] std::uint32_t selected() const noexcept { return selected_; }
    /// Select a scene entity (clears the agent selection).
    void select(std::uint32_t id) noexcept {
        selected_ = id;
        selectedAgent_ = kNoAgentSelected;
    }
    /// Select an agent by id for the behaviour-tree debugger (clears the entity selection).
    void selectAgent(std::uint32_t agentId) noexcept {
        selectedAgent_ = agentId;
        selected_ = 0;
    }
    [[nodiscard]] std::uint32_t selectedAgent() const noexcept { return selectedAgent_; }
    static constexpr std::uint32_t kNoAgentSelected = 0xFFFFFFFFu;
    /// Inspector "Follow" toggle: the app keeps the camera on the selected agent.
    bool followAgent = false;
    /// True while the gizmo is hovered or dragged (the app must not orbit/pick then).
    [[nodiscard]] bool gizmoActive() const noexcept { return gizmoActive_; }

    void setFonts(const ThemeFonts& fonts) noexcept { fonts_ = fonts; }

    bool snap = true;
    float translateSnap = 0.25f;
    float rotateSnapDegrees = 15.0f;

private:
    void addMenu(const SceneEditorInput& in, std::vector<nlohmann::json>& commands);
    /// One entity row in the Scene list: selectable, draggable into groups, context menu.
    void entityRow(const SceneEditorInput& in, std::uint32_t id, std::vector<nlohmann::json>& commands);
    /// A group and everything in it, as a tree node that entities can be dropped on.
    void groupNode(const SceneEditorInput& in, std::uint32_t id, std::vector<nlohmann::json>& commands);
    /// "Move to group" menu items for `id` (every group it may go in, and none).
    void moveToGroupMenu(const SceneEditorInput& in, std::uint32_t id, std::vector<nlohmann::json>& commands);
    void drawAgentInspector(const SceneEditorInput& in);
    bool editJson(const char* label, nlohmann::json& value, nlohmann::json& patch, const std::string& path);
    bool agentTypeCombo(const char* id, nlohmann::json& value, bool byIndex);
    bool editAgentMix(nlohmann::json& value);

    ThemeFonts fonts_;
    const sim::Simulation* inspectedSim_ = nullptr;  ///< set each frame by drawInspector (agent type lists)

    std::uint32_t selected_ = 0;
    std::uint32_t selectedAgent_ = kNoAgentSelected;
    bool gizmoActive_ = false;
    bool gizmoTranslate_ = true;
    std::uint32_t dragSerial_ = 0;
    bool wasUsingGizmo_ = false;
    char importPath_[256] = "models/test_room.gltf";
    nlohmann::json buffer_;  ///< inspector working copy of the selected entity
    std::uint32_t bufferId_ = 0;
    bool showLists_ = false;      ///< Scene panel view: false = Hierarchy, true = Lists
    std::uint32_t revealed_ = 0;  ///< selection whose groups were last opened in the Scene list
    const std::set<std::uint32_t>* revealGroups_ = nullptr;  ///< groups to open this frame
};

}  // namespace crowdsim::ui
