#include "crowdsim/ui/EditorShell.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>

#include <string>
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"

namespace crowdsim::ui {

namespace {

constexpr const char* kSimWindow = "Simulation";
constexpr const char* kStatsWindow = "Stats";
constexpr const char* kLogWindow = "Log";
constexpr const char* kHierarchyWindow = "Scene";
constexpr const char* kInspectorWindow = "Inspector";
constexpr const char* kHistoryWindow = "History";

}  // namespace

void EditorShell::buildDefaultLayout(unsigned int dockspaceId) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspaceId);
    // DockSpace is a private (imgui_internal) flag of a different enum type.
    ImGui::DockBuilderAddNode(dockspaceId, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) |
                                               ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    ImGuiID rightBottom = 0;
    const ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.5f, nullptr, &rightBottom);

    ImGui::DockBuilderDockWindow(kHierarchyWindow, left);
    ImGui::DockBuilderDockWindow(kSimWindow, rightTop);
    ImGui::DockBuilderDockWindow(kStatsWindow, rightTop);
    ImGui::DockBuilderDockWindow(kInspectorWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kLogWindow, bottom);
    ImGui::DockBuilderDockWindow(kHistoryWindow, bottom);
    // The central node stays empty: it is the 3D viewport.
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorShell::fileMenu(const EditorFrameData& data, EditorRequests& req) {
    if (!ImGui::BeginMenu("File")) {
        return;
    }
    if (ImGui::MenuItem("New scene")) {
        req.commands.push_back(commands::makeCommand("CreateScene", {{"name", "untitled"}}));
    }
    if (ImGui::BeginMenu("Open scene", !data.sceneFiles.empty())) {
        for (const std::string& file : data.sceneFiles) {
            if (ImGui::MenuItem(file.c_str())) {
                req.commands.push_back(commands::makeCommand("LoadScene", {{"path", file}}));
            }
        }
        ImGui::EndMenu();
    }
    const std::string current(data.sceneName);
    if (ImGui::MenuItem(("Save scenes/" + current + ".scene.json").c_str(), "Ctrl+S")) {
        req.commands.push_back(commands::makeCommand("SaveScene", {{"path", "scenes/" + current + ".scene.json"}}));
    }
    if (ImGui::BeginMenu("Save as")) {
        ImGui::InputText("name", saveAsName_, sizeof(saveAsName_));
        if (ImGui::Button("Save")) {
            const std::string name(saveAsName_);
            req.commands.push_back(
                commands::makeCommand("SaveScene", {{"path", "scenes/" + name + ".scene.json"}, {"name", name}}));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save command log")) {
        req.saveCommandLog = true;
    }
    if (ImGui::MenuItem("Save screenshot", "F12")) {
        req.screenshot = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Alt+F4")) {
        req.quit = true;
    }
    ImGui::EndMenu();
}

EditorRequests EditorShell::draw(const EditorFrameData& data) {
    EditorRequests req;
    req.simSpeed = data.simSpeed;
    req.overlays = overlays_;

    // Main menu
    if (ImGui::BeginMainMenuBar()) {
        fileMenu(data, req);
        if (ImGui::BeginMenu("Edit")) {
            const bool canUndo = data.editor != nullptr && data.editor->dispatcher.canUndo();
            const bool canRedo = data.editor != nullptr && data.editor->dispatcher.canRedo();
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
                req.commands.push_back(commands::makeCommand("Undo"));
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
                req.commands.push_back(commands::makeCommand("Redo"));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete selected", "Del", false, sceneEditor_.selected() != 0)) {
                req.commands.push_back(commands::makeCommand("DeleteEntity", {{"id", sceneEditor_.selected()}}));
            }
            ImGui::MenuItem("Snap gizmo", nullptr, &sceneEditor_.snap);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Simulation")) {
            if (ImGui::MenuItem(data.playing ? "Pause" : "Play", "Space")) {
                req.togglePlay = true;
            }
            if (ImGui::MenuItem("Step", "S", false, !data.playing)) {
                req.stepOnce = true;
            }
            if (ImGui::MenuItem("Reset")) {
                req.reset = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset layout")) {
                layoutBuilt_ = false;
            }
            ImGui::MenuItem("ImGui demo", nullptr, &showDemo_);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Keyboard shortcuts (ignored while typing into a widget)
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            req.togglePlay = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !data.playing) {
            req.stepOnce = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
            req.screenshot = true;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            req.commands.push_back(commands::makeCommand(io.KeyShift ? "Redo" : "Undo"));
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            req.commands.push_back(commands::makeCommand("Redo"));
        }
        if (data.editor != nullptr) {
            sceneEditor_.handleShortcuts(*data.editor, req.commands);
        }
    }

    const ImGuiID dockspaceId = ImGui::GetID("CrowdSimDockspace");
    if (!layoutBuilt_) {
        // Respect a layout restored from imgui.ini; otherwise build the default one.
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
        if (node == nullptr || !node->IsSplitNode()) {
            buildDefaultLayout(dockspaceId);
        }
        layoutBuilt_ = true;
    }
    ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    // 3D viewport = the dockspace's central node, left empty so ImGui passes it
    // through and draws nothing there. The scene renders into its rectangle.
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId); central != nullptr) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 base = ImGui::GetMainViewport()->Pos;
        const ImVec2 scale = io.DisplayFramebufferScale;
        const ImVec2 origin = central->Pos;
        req.viewport = PixelRect{(origin.x - base.x) * scale.x, (origin.y - base.y) * scale.y,
                                 central->Size.x * scale.x, central->Size.y * scale.y};
        const ImVec2 mouse = io.MousePos;
        const bool insideRect = mouse.x >= origin.x && mouse.y >= origin.y && mouse.x < origin.x + central->Size.x &&
                                mouse.y < origin.y + central->Size.y;
        // Keep ownership while dragging even if the cursor leaves the rectangle.
        viewportDragging_ = viewportDragging_ && ImGui::IsAnyMouseDown();
        if (insideRect && !io.WantCaptureMouse && ImGui::IsAnyMouseDown() && !sceneEditor_.gizmoActive()) {
            viewportDragging_ = true;
        }
        req.viewportHovered =
            ((insideRect && !io.WantCaptureMouse) || viewportDragging_) && !sceneEditor_.gizmoActive();

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        std::string overlay = std::to_string(data.agentsAlive) + " agents";
        if (data.hasCursorWorld) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "   cursor (%.2f, %.2f, %.2f)", static_cast<double>(data.cursorWorld[0]),
                          static_cast<double>(data.cursorWorld[1]), static_cast<double>(data.cursorWorld[2]));
            overlay += buf;
        }
        dl->AddText(ImVec2(origin.x + 10.0f, origin.y + 8.0f), IM_COL32(230, 230, 230, 220), overlay.c_str());
        dl->AddText(ImVec2(origin.x + 10.0f, origin.y + 8.0f + ImGui::GetTextLineHeightWithSpacing()),
                    IM_COL32(160, 160, 160, 200), "LMB orbit   RMB/MMB pan   wheel zoom   F reset view");
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        req.frameAll = true;
    }

    if (data.editor != nullptr) {
        sceneEditor_.drawGizmo(*data.editor, req.commands);
    }

    ImGui::Begin(kHierarchyWindow);
    ImGui::Text("%.*s", static_cast<int>(data.sceneName.size()), data.sceneName.data());
    ImGui::TextDisabled("Navmesh %u polys, %u tiles", data.navPolygons, data.navTiles);
    if (ImGui::CollapsingHeader("Overlays")) {
        ImGui::Checkbox("Reference grid", &overlays_.ground);
        ImGui::Checkbox("Spawner / goal areas", &overlays_.areas);
        ImGui::Checkbox("Navmesh + walls", &overlays_.navmesh);
        ImGui::Checkbox("Off-mesh links", &overlays_.links);
        ImGui::Checkbox("Velocities", &overlays_.velocities);
        ImGui::Checkbox("Flow field", &overlays_.flowField);
        if (overlays_.flowField && data.goals > 1) {
            ImGui::SliderInt("Goal", &overlays_.flowGoal, 0, static_cast<int>(data.goals) - 1);
        }
        overlays_.flowGoal = std::clamp(overlays_.flowGoal, 0, std::max(0, static_cast<int>(data.goals) - 1));
    }
    ImGui::Separator();
    if (data.editor != nullptr) {
        sceneEditor_.drawHierarchy(*data.editor, req.commands);
    }
    ImGui::End();

    ImGui::Begin(kInspectorWindow);
    if (data.editor != nullptr) {
        sceneEditor_.drawInspector(*data.editor, req.commands);
    }
    ImGui::End();

    ImGui::Begin(kHistoryWindow);
    if (data.editor != nullptr) {
        sceneEditor_.drawHistory(*data.editor, req.commands);
    }
    ImGui::End();

    ImGui::Begin(kSimWindow);
    if (ImGui::Button(data.playing ? "Pause" : "Play", ImVec2(80, 0))) {
        req.togglePlay = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(data.playing);
    if (ImGui::Button("Step", ImVec2(80, 0))) {
        req.stepOnce = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(80, 0))) {
        req.reset = true;
    }
    ImGui::SliderFloat("Speed", &req.simSpeed, 0.0f, 8.0f, "%.2fx");
    ImGui::Separator();
    ImGui::Text("Step      %llu", static_cast<unsigned long long>(data.stepIndex));
    ImGui::Text("Sim time  %.2f s", data.simTimeSeconds);
    ImGui::Text("Rate      %.0f Hz", data.stepHz);
    ImGui::Text("Seed      %llu", static_cast<unsigned long long>(data.seed));
    ImGui::Text("Hash      %016llx", static_cast<unsigned long long>(data.stateHash));

    ImGui::SeparatorText("Crowd");
    ImGui::Text("Alive     %u", data.agentsAlive);
    ImGui::Text("Spawned   %llu", static_cast<unsigned long long>(data.agentsSpawned));
    ImGui::Text("Arrived   %llu", static_cast<unsigned long long>(data.agentsArrived));
    ImGui::Text("Avg speed %.2f m/s", data.averageSpeed);

    ImGui::SeparatorText("Scenario");
    if (pendingScenario_ < 0) {
        pendingScenario_ = data.scenarioIndex;
        pendingAgents_ = static_cast<int>(data.scenarioAgents);
    }
    if (!data.scenarioNames.empty()) {
        const std::string current(data.scenarioNames[static_cast<std::size_t>(pendingScenario_)]);
        if (ImGui::BeginCombo("Scene", current.c_str())) {
            for (std::size_t i = 0; i < data.scenarioNames.size(); ++i) {
                const std::string name(data.scenarioNames[i]);
                if (ImGui::Selectable(name.c_str(), static_cast<int>(i) == pendingScenario_)) {
                    pendingScenario_ = static_cast<int>(i);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputInt("Agents", &pendingAgents_, 500, 5000);
    pendingAgents_ = std::clamp(pendingAgents_, 0, 200000);
    if (ImGui::Button("Load scenario")) {
        req.rebuildScenario = true;
        req.scenarioIndex = pendingScenario_;
        req.scenarioAgents = static_cast<std::uint32_t>(pendingAgents_);
    }
    ImGui::End();

    ImGui::Begin(kStatsWindow);
    const float fps = data.frameMs > 0.0f ? 1000.0f / data.frameMs : 0.0f;
    ImGui::Text("Frame     %.2f ms (%.0f FPS)", data.frameMs, fps);
    ImGui::Text("Sim step  %.3f ms", data.simStepMs);
    ImGui::Text("Render    %.3f ms (CPU submit)", data.renderSubmitMs);
    ImGui::Text("Renderer  %.*s", static_cast<int>(data.rendererName.size()), data.rendererName.data());
    ImGui::Text("Drawn     %u agents, %u draws", data.agentsDrawn, data.drawCalls);
    ImGui::Text("Viewport  %u x %u", data.framebufferWidth, data.framebufferHeight);
    ImGui::Text("Jobs      %u threads", data.jobThreads);
    ImGui::End();

    ImGui::Begin(kLogWindow);
    ImGui::Checkbox("Auto-scroll", &autoScrollLog_);
    ImGui::Separator();
    ImGui::BeginChild("LogLines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(data.logLines.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const std::string& line = data.logLines[static_cast<std::size_t>(i)];
            ImGui::TextUnformatted(line.data(), line.data() + line.size());
        }
    }
    if (autoScrollLog_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();

    if (showDemo_) {
        ImGui::ShowDemoWindow(&showDemo_);
    }
    return req;
}

}  // namespace crowdsim::ui
