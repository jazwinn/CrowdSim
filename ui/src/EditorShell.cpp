#include "crowdsim/ui/EditorShell.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <cstdio>

#include <format>
#include <string>

#include "crowdsim/commands/Command.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"
#include "crowdsim/ui/Theme.hpp"

namespace crowdsim::ui {

namespace {

constexpr const char* kSimWindow = "Simulation";
constexpr const char* kStatsWindow = "Stats";
constexpr const char* kLogWindow = "Log";
constexpr const char* kHierarchyWindow = "Scene";
constexpr const char* kInspectorWindow = "Inspector";
constexpr const char* kHistoryWindow = "History";
constexpr const char* kScriptWindow = "Script";
constexpr const char* kBehaviourWindow = "Behaviour";
constexpr const char* kMetricsWindow = "Metrics";

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
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    ImGuiID rightBottom = 0;
    const ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.5f, nullptr, &rightBottom);

    ImGui::DockBuilderDockWindow(kHierarchyWindow, left);
    ImGui::DockBuilderDockWindow(kSimWindow, rightTop);
    ImGui::DockBuilderDockWindow(kStatsWindow, rightTop);
    ImGui::DockBuilderDockWindow(kMetricsWindow, rightTop);
    ImGui::DockBuilderDockWindow(kInspectorWindow, rightBottom);
    ImGui::DockBuilderDockWindow(kLogWindow, bottom);
    ImGui::DockBuilderDockWindow(kHistoryWindow, bottom);
    ImGui::DockBuilderDockWindow(kScriptWindow, bottom);
    ImGui::DockBuilderDockWindow(kBehaviourWindow, bottom);
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
    if (ImGui::MenuItem(data.recording ? "Stop recording" : "Record 3D view (video)", "F9", false,
                        data.recordingSupported || data.recording)) {
        req.toggleRecording = true;
    }
    if (!data.recordingSupported && !data.recording) {
        ImGui::SetItemTooltip("This graphics backend cannot read frames back; try --renderer d3d11 or vulkan");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Alt+F4")) {
        req.quit = true;
    }
    ImGui::EndMenu();
}

EditorShell::EditorShell(const OverlayToggles& initialOverlays) : overlays_(initialOverlays) {
    ImPlot::CreateContext();
    applyPlotTheme();
}

EditorShell::~EditorShell() {
    // Panels keep settings in imgui.ini through handlers that go away with them, so
    // save now, while they are registered, and skip ImGui's own save at shutdown
    // (it would rewrite the file without those sections).
    ImGuiIO& io = ImGui::GetIO();
    if (io.IniFilename != nullptr) {
        ImGui::SaveIniSettingsToDisk(io.IniFilename);
        io.IniFilename = nullptr;
    }
    ImPlot::DestroyContext();
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
        if (ImGui::IsKeyPressed(ImGuiKey_F9, false) && (data.recordingSupported || data.recording)) {
            req.toggleRecording = true;
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

        // Viewport HUD: a quiet status chip top-left, navigation help bottom-left.
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const float em = ImGui::GetFontSize();
        const float pad = em * 0.5f;
        auto chip = [&](ImVec2 at, const char* strong, const char* rest, bool alignBottom) {
            const ImVec2 a = ImGui::CalcTextSize(strong);
            const ImVec2 b = rest != nullptr ? ImGui::CalcTextSize(rest) : ImVec2(0, 0);
            const float w = a.x + (rest != nullptr ? em * 0.6f + b.x : 0.0f) + pad * 2.0f;
            const float h = a.y + pad * 1.2f;
            if (alignBottom) {
                at.y -= h;
            }
            dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(18, 20, 23, 190), em * 0.35f);
            dl->AddText(fonts_.strong, 0.0f, ImVec2(at.x + pad, at.y + pad * 0.6f), IM_COL32(235, 238, 241, 255),
                        strong);
            if (rest != nullptr) {
                dl->AddText(ImVec2(at.x + pad + a.x + em * 0.6f, at.y + pad * 0.6f), IM_COL32(150, 158, 168, 255),
                            rest);
            }
        };
        const std::string agents = std::format("{} agents", data.agentsAlive);
        std::string where;
        if (data.hasCursorWorld) {
            where = std::format("x {:.1f}   y {:.1f}   z {:.1f}", data.cursorWorld[0], data.cursorWorld[1],
                                data.cursorWorld[2]);
        }
        chip(ImVec2(origin.x + pad * 1.5f, origin.y + pad * 1.5f), agents.c_str(),
             where.empty() ? (data.playing ? nullptr : "paused") : where.c_str(), false);
        // Recording indicator, top right (UI: never part of the video).
        if (data.recording) {
            const auto total = static_cast<int>(data.recordingSeconds);
            const std::string text = std::format("REC  {}:{:02}", total / 60, total % 60);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const float w = size.x + pad * 2.0f + em * 0.9f;
            const float h = size.y + pad * 1.2f;
            const ImVec2 p0(origin.x + central->Size.x - pad * 1.5f - w, origin.y + pad * 1.5f);
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(18, 20, 23, 200), em * 0.35f);
            const bool blink = static_cast<int>(data.recordingSeconds * 2.0) % 2 == 0;
            dl->AddCircleFilled(ImVec2(p0.x + pad + em * 0.3f, p0.y + h * 0.5f), em * 0.28f,
                                blink ? IM_COL32(229, 72, 72, 255) : IM_COL32(120, 40, 40, 255));
            dl->AddText(fonts_.strong, 0.0f, ImVec2(p0.x + pad + em * 0.9f, p0.y + pad * 0.6f),
                        IM_COL32(235, 238, 241, 255), text.c_str());
        }
        // Camera controls: each input in a key cap followed by what it does. Shown only
        // while it fits inside the viewport; it is a hint, not a control.
        struct Hint {
            const char* input;
            const char* action;
        };
        constexpr Hint kHints[] = {{"Drag", "Orbit"}, {"Right drag", "Pan"}, {"Wheel", "Zoom"}, {"F", "Frame all"}};
        const float capPad = em * 0.35f;
        const float inner = em * 0.4f;  // between a cap and its action
        const float between = em * 1.1f;  // between pairs
        float width = pad * 2.0f;
        for (const Hint& h : kHints) {
            width += ImGui::CalcTextSize(h.input).x + capPad * 2.0f + inner + ImGui::CalcTextSize(h.action).x;
        }
        width += between * static_cast<float>(std::size(kHints) - 1);
        if (width + em * 2.0f < central->Size.x) {
            const float height = em + pad * 1.4f;
            const ImVec2 p0(origin.x + pad * 1.5f, origin.y + central->Size.y - pad * 1.5f - height);
            dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), IM_COL32(18, 20, 23, 190), em * 0.35f);
            float x = p0.x + pad;
            const float textY = p0.y + (height - em) * 0.5f;
            for (const Hint& h : kHints) {
                const float capW = ImGui::CalcTextSize(h.input).x + capPad * 2.0f;
                const ImVec2 c0(x, textY - em * 0.12f);
                const ImVec2 c1(x + capW, textY + em * 1.12f);
                dl->AddRectFilled(c0, c1, IM_COL32(52, 57, 65, 255), em * 0.25f);
                dl->AddRect(c0, c1, IM_COL32(78, 85, 95, 255), em * 0.25f);
                dl->AddText(fonts_.strong, 0.0f, ImVec2(x + capPad, textY), IM_COL32(235, 238, 241, 255), h.input);
                x += capW + inner;
                dl->AddText(ImVec2(x, textY), IM_COL32(160, 168, 178, 255), h.action);
                x += ImGui::CalcTextSize(h.action).x + between;
            }
        }
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        req.frameAll = true;
    }

    if (data.editor != nullptr) {
        if (overlays_.routes && overlays_.areas) {
            sceneEditor_.drawRoutes(*data.editor);
        }
        sceneEditor_.drawGizmo(*data.editor, req.commands);
    }

    ImGui::Begin(kHierarchyWindow);
    ImGui::PushFont(fonts_.strong, 0.0f);
    ImGui::Text("%.*s", static_cast<int>(data.sceneName.size()), data.sceneName.data());
    ImGui::PopFont();
    ImGui::TextDisabled("Navmesh: %u polygons in %u tiles", data.navPolygons, data.navTiles);
    ImGui::Spacing();
    if (sectionToggle("Overlays", fonts_.strong)) {
        ImGui::Checkbox("Reference grid", &overlays_.ground);
        ImGui::Checkbox("Spawner / goal areas", &overlays_.areas);
        ImGui::Checkbox("Navmesh + walls", &overlays_.navmesh);
        ImGui::Checkbox("Off-mesh links", &overlays_.links);
        ImGui::Checkbox("Spawner routes", &overlays_.routes);
        ImGui::SetItemTooltip("An arc from each spawner to its goal, one colour per goal");
        ImGui::Checkbox("Velocities", &overlays_.velocities);
        ImGui::Checkbox("Flow field", &overlays_.flowField);
        propertyLabel("Heatmap");
        ImGui::Combo("##heatmap", &overlays_.heatmap, "Off\0Density\0Speed\0Peak pressure\0");
        ImGui::SetItemTooltip("Peak pressure: the highest crowd pressure on any body in each cell (scene bodies)");
        if (overlays_.flowField && data.goals > 1) {
            propertyLabel("Flow goal");
            ImGui::SliderInt("##flowgoal", &overlays_.flowGoal, 0, static_cast<int>(data.goals) - 1);
        }
        overlays_.flowGoal = std::clamp(overlays_.flowGoal, 0, std::max(0, static_cast<int>(data.goals) - 1));
    }
    if (sectionToggle("Rendering", fonts_.strong)) {
        ImGui::Checkbox("Animated characters", &overlays_.animatedCharacters);
        ImGui::Checkbox("Shadows", &overlays_.shadows);
        propertyLabel("Full detail within");
        ImGui::SliderFloat("##lodnear", &overlays_.lodNearDistance, 5.0f, 150.0f, "%.0f m");
        propertyLabel("Characters within");
        ImGui::SliderFloat("##lodfar", &overlays_.lodFarDistance, 10.0f, 400.0f, "%.0f m");
        overlays_.lodFarDistance = std::max(overlays_.lodFarDistance, overlays_.lodNearDistance);
    }
    ImGui::Spacing();
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
    {
        // Transport: play/pause is the primary action, so it alone carries the accent.
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float third = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
        const ImVec4 accent = palette::accent();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x * 0.85f, accent.y * 0.85f, accent.z * 0.85f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.10f, 0.08f, 1.0f));
        ImGui::PushFont(fonts_.strong, 0.0f);
        if (ImGui::Button(data.playing ? "Pause" : "Play", ImVec2(third, 0))) {
            req.togglePlay = true;
        }
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        ImGui::SetItemTooltip("Space");
        ImGui::SameLine();
        ImGui::BeginDisabled(data.playing);
        if (ImGui::Button("Step", ImVec2(third, 0))) {
            req.stepOnce = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("S, while paused");
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(-1.0f, 0))) {
            req.reset = true;
        }
    }
    {
        // Record the 3D view (no UI) to video; red while recording, showing the time.
        const auto total = static_cast<int>(data.recordingSeconds);
        const std::string label = data.recording ? std::format("Stop recording   {}:{:02}###record", total / 60, total % 60)
                                                 : std::string("Record video###record");
        if (data.recording) {
            const ImVec4 red = palette::error();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(red.x, red.y, red.z, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, red);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(red.x * 0.85f, red.y * 0.85f, red.z * 0.85f, 1.0f));
        }
        ImGui::BeginDisabled(!data.recordingSupported && !data.recording);
        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0))) {
            req.toggleRecording = true;
        }
        ImGui::EndDisabled();
        if (data.recording) {
            ImGui::PopStyleColor(3);
        }
        if (!data.recordingSupported && !data.recording) {
            ImGui::SetItemTooltip("This graphics backend cannot read frames back; try --renderer d3d11 or vulkan");
        } else if (data.recording) {
            ImGui::SetItemTooltip("Saving to content/recordings/%.*s (F9)", static_cast<int>(data.recordingFile.size()),
                                  data.recordingFile.data());
        } else {
            ImGui::SetItemTooltip("Record the 3D view, without the editor UI, to content/recordings (F9)");
        }
    }
    propertyLabel("Speed");
    ImGui::SliderFloat("##speed", &req.simSpeed, 0.0f, 8.0f, "%.2fx");

    sectionHeading("Clock", fonts_);
    if (beginReadout("clock")) {
        readout("Step", "%llu", static_cast<unsigned long long>(data.stepIndex));
        readout("Sim time", "%.2f s", data.simTimeSeconds);
        readout("Rate", "%.0f Hz", data.stepHz);
        readout("Seed", "%llu", static_cast<unsigned long long>(data.seed));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(palette::textDim(), "State hash");
        ImGui::TableNextColumn();
        ImGui::PushFont(fonts_.mono, ImGui::GetStyle().FontSizeBase * 0.9f);
        ImGui::Text("%016llx", static_cast<unsigned long long>(data.stateHash));
        ImGui::PopFont();
        endReadout();
    }

    sectionHeading("Crowd", fonts_);
    if (beginReadout("crowd")) {
        readout("Alive", "%u", data.agentsAlive);
        readout("Spawned", "%llu", static_cast<unsigned long long>(data.agentsSpawned));
        readout("Arrived", "%llu", static_cast<unsigned long long>(data.agentsArrived));
        readout("Average speed", "%.2f m/s", data.averageSpeed);
        endReadout();
    }

    sectionHeading("Scenario", fonts_);
    if (pendingScenario_ < 0) {
        pendingScenario_ = data.scenarioIndex;
        pendingAgents_ = static_cast<int>(data.scenarioAgents);
    }
    if (!data.scenarioNames.empty()) {
        const std::string current(data.scenarioNames[static_cast<std::size_t>(pendingScenario_)]);
        propertyLabel("Scenario");
        if (ImGui::BeginCombo("##scenario", current.c_str())) {
            for (std::size_t i = 0; i < data.scenarioNames.size(); ++i) {
                const std::string name(data.scenarioNames[i]);
                if (ImGui::Selectable(name.c_str(), static_cast<int>(i) == pendingScenario_)) {
                    pendingScenario_ = static_cast<int>(i);
                }
            }
            ImGui::EndCombo();
        }
    }
    propertyLabel("Agents");
    ImGui::InputInt("##agents", &pendingAgents_, 500, 5000);
    pendingAgents_ = std::clamp(pendingAgents_, 0, 200000);
    if (ImGui::Button("Load scenario", ImVec2(-1.0f, 0))) {
        req.rebuildScenario = true;
        req.scenarioIndex = pendingScenario_;
        req.scenarioAgents = static_cast<std::uint32_t>(pendingAgents_);
    }
    ImGui::End();

    ImGui::Begin(kStatsWindow);
    const float fps = data.frameMs > 0.0f ? 1000.0f / data.frameMs : 0.0f;
    sectionHeading("Frame", fonts_);
    if (beginReadout("frame")) {
        readout("Frame", "%.2f ms  (%.0f FPS)", data.frameMs, fps);
        readout("Sim step", "%.3f ms", data.simStepMs);
        readout("Crowd prep", "%.3f ms", data.crowdPrepareMs);
        readout("Render submit", "%.3f ms", data.renderSubmitMs);
        endReadout();
    }
    sectionHeading("Rendering", fonts_);
    if (beginReadout("rendering")) {
        readout("Renderer", "%.*s", static_cast<int>(data.rendererName.size()), data.rendererName.data());
        readout("Viewport", "%u x %u", data.framebufferWidth, data.framebufferHeight);
        readout("Agents drawn", "%u", data.agentsDrawn);
        readout("Draw calls", "%u", data.drawCalls);
        readout("Full detail", "%u", data.agentsNear);
        readout("Mid detail", "%u", data.agentsMid);
        readout("Far detail", "%u", data.agentsFar);
        readout("Culled", "%u", data.agentsCulled);
        readout("Shadow casters", "%u", data.shadowCasters);
        endReadout();
    }
    sectionHeading("Jobs", fonts_);
    if (beginReadout("jobs")) {
        readout("Worker threads", "%u", data.jobThreads);
        endReadout();
    }
    ImGui::End();

    ImGui::Begin(kLogWindow);
    const ImGuiID logDock = ImGui::GetWindowDockID();
    ImGui::Checkbox("Auto-scroll", &autoScrollLog_);
    ImGui::Separator();
    ImGui::BeginChild("LogLines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(fonts_.mono, 0.0f);
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
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::End();

    // Script console: evaluates Lua through the EvalLua command (logged and replayable).
    // Layouts saved before the console existed get it docked next to the log.
    if (logDock != 0) {
        ImGui::SetNextWindowDockID(logDock, ImGuiCond_FirstUseEver);
    }
    ImGui::Begin(kScriptWindow);
    drawScriptPanel(data, req);
    ImGui::End();

    // Metrics: plots, gates, recording. Docks with Simulation / Stats in older layouts.
    ImGui::Begin(kStatsWindow);
    const ImGuiID statsDock = ImGui::GetWindowDockID();
    ImGui::End();
    if (statsDock != 0) {
        ImGui::SetNextWindowDockID(statsDock, ImGuiCond_FirstUseEver);
    }
    if (focusMetrics_ > 0) {
        ImGui::SetNextWindowFocus();
        --focusMetrics_;
    }
    ImGui::Begin(kMetricsWindow);
    if (data.metrics != nullptr) {
        const float gatePoint[2] = {data.editor != nullptr ? data.editor->cameraTarget[0] : 0.0f,
                                    data.editor != nullptr ? data.editor->cameraTarget[2] : 0.0f};
        metricsPanel_.draw(*data.metrics, gatePoint, fonts_, req.commands);
    }
    ImGui::End();

    // Behaviour tree node editor.
    if (logDock != 0) {
        ImGui::SetNextWindowDockID(logDock, ImGuiCond_FirstUseEver);
    }
    if (focusBehaviour_) {
        // Opened on request (e.g. --edit-bt): float it large over the viewport.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.19f, vp->WorkPos.y + 30.0f),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.60f, vp->WorkSize.y * 0.70f), ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        focusBehaviour_ = false;
    }
    ImGui::Begin(kBehaviourWindow);
    if (data.editor != nullptr) {
        btEditor_.draw(data.editor->sim, sceneEditor_.selectedAgent(), req.commands);
    }
    ImGui::End();

    if (showDemo_) {
        ImGui::ShowDemoWindow(&showDemo_);
    }
    return req;
}

namespace {

/// std::string-backed multiline input (ImGui writes into the string's buffer).
int resizeString(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(d->UserData);
        text->resize(static_cast<std::size_t>(d->BufTextLen));
        d->Buf = text->data();
    }
    return 0;
}

constexpr const char* kNewScriptTemplate =
    "-- Loaded when CrowdSim starts and again whenever this file is saved.\n"
    "-- API: docs/scripting.md (scene, entities, bt, agents, sim, metrics, events, log).\n\n";

}  // namespace

void EditorShell::openScript(const ScriptView& script) {
    openScript_ = std::string(script.name);
    scriptBuffer_ = std::string(script.source);
    scriptLoaded_ = scriptBuffer_;
    scriptDirty_ = false;
    selectScriptTab_ = true;
}

void EditorShell::drawScriptPanel(const EditorFrameData& data, EditorRequests& req) {
    // Script list on the left; the console and the open script as tabs on the right.
    const float listWidth = ImGui::GetFontSize() * 13.0f;
    ImGui::BeginChild("ScriptList", ImVec2(listWidth, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    drawScriptList(data, req);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::BeginTabBar("ScriptTabs")) {
        if (ImGui::BeginTabItem("Console")) {
            drawConsole(req);
            ImGui::EndTabItem();
        }
        if (!openScript_.empty()) {
            bool open = true;
            const std::string label = openScript_ + ".lua" + (scriptDirty_ ? " *" : "") + "###openscript";
            const ImGuiTabItemFlags flags = (selectScriptTab_ ? ImGuiTabItemFlags_SetSelected : 0) |
                                            (scriptDirty_ ? ImGuiTabItemFlags_UnsavedDocument : 0);
            selectScriptTab_ = false;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                drawScriptEditor(data, req);
                ImGui::EndTabItem();
            }
            if (!open) {
                openScript_.clear();
                scriptBuffer_.clear();
                scriptDirty_ = false;
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndGroup();
}

void EditorShell::drawScriptList(const EditorFrameData& data, EditorRequests& req) {
    sectionHeading("Scripts", fonts_);
    if (data.scripts.empty()) {
        ImGui::TextDisabled("None loaded.");
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const ScriptView& s : data.scripts) {
        const std::string name(s.name);
        ImGui::PushID(name.c_str());
        // Status dot: green loaded, red failed (hover for the error).
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float r = ImGui::GetFontSize() * 0.22f;
        if (ImGui::Selectable(("      " + name).c_str(), openScript_ == name)) {
            openScript(s);
        }
        dl->AddCircleFilled(ImVec2(at.x + r * 2.0f, at.y + ImGui::GetTextLineHeight() * 0.5f), r,
                            ImGui::GetColorU32(s.ok ? palette::accent() : palette::error()));
        if (!s.ok) {
            ImGui::SetItemTooltip("%.*s", static_cast<int>(s.error.size()), s.error.data());
        } else {
            ImGui::SetItemTooltip("content/scripts/%s.lua: click to open", name.c_str());
        }
        ImGui::PopID();
    }

    sectionHeading("New script", fonts_);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##newname", "name, e.g. my_study", newScriptName_, sizeof(newScriptName_));
    const std::string newName(newScriptName_);
    ImGui::BeginDisabled(newName.empty());
    if (ImGui::Button("Create", ImVec2(-1.0f, 0))) {
        req.commands.push_back(commands::makeCommand("SaveScript", {{"name", newName}, {"source", kNewScriptTemplate}}));
        openScript(ScriptView{newName, kNewScriptTemplate, true, {}});
        newScriptName_[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Creates content/scripts/<name>.lua and opens it");

    sectionHeading("Load a file", fonts_);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##loadpath", "scripts/file.lua", loadScriptPath_, sizeof(loadScriptPath_));
    if (ImGui::Button("Load and run", ImVec2(-1.0f, 0))) {
        req.commands.push_back(commands::makeCommand("LoadScript", {{"path", std::string(loadScriptPath_)}}));
    }
    ImGui::SetItemTooltip("Path relative to the content folder, or absolute");

    if (!data.scriptErrors.empty()) {
        sectionHeading(std::format("Errors ({})", data.scriptErrors.size()).c_str(), fonts_);
        ImGui::PushStyleColor(ImGuiCol_Text, palette::error());
        const std::size_t first = data.scriptErrors.size() > 8 ? data.scriptErrors.size() - 8 : 0;
        for (std::size_t k = data.scriptErrors.size(); k-- > first;) {  // newest first
            const ScriptErrorView& e = data.scriptErrors[k];
            ImGui::TextWrapped("%.*s: %.*s", static_cast<int>(e.where.size()), e.where.data(),
                               static_cast<int>(e.message.size()), e.message.data());
        }
        ImGui::PopStyleColor();
    }
}

void EditorShell::drawConsole(EditorRequests& req) {
    // Evaluates Lua through the EvalLua command (logged and replayable).
    const float inputHeight = ImGui::GetTextLineHeight() * 5.0f;
    ImGui::BeginChild("Transcript", ImVec2(0, -inputHeight - ImGui::GetFrameHeightWithSpacing() * 1.5f),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(fonts_.mono, 0.0f);
    if (consoleLines_.empty()) {
        ImGui::TextDisabled("Type Lua below and press Run. print() output and returned values appear here.");
    }
    for (const ConsoleLine& line : consoleLines_) {
        if (line.error) {
            ImGui::PushStyleColor(ImGuiCol_Text, palette::error());
        }
        ImGui::TextUnformatted(line.text.c_str());
        if (line.error) {
            ImGui::PopStyleColor();
        }
    }
    if (consoleScroll_) {
        ImGui::SetScrollHereY(1.0f);
        consoleScroll_ = false;
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PushFont(fonts_.mono, 0.0f);
    ImGui::InputTextMultiline("##lua", consoleInput_, sizeof(consoleInput_), ImVec2(-1.0f, inputHeight),
                              ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopFont();
    const bool ctrlEnter = ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
    if (ImGui::Button("Run") || ctrlEnter) {
        const std::string source(consoleInput_);
        consoleLines_.push_back({"> " + source, false});
        consoleScroll_ = true;
        req.commands.push_back(commands::makeCommand("EvalLua", {{"source", source}}));
    }
    ImGui::SetItemTooltip("Ctrl+Enter");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        consoleLines_.clear();
    }
}

void EditorShell::drawScriptEditor(const EditorFrameData& data, EditorRequests& req) {
    // Follow reloads from disk (file watcher, Reload) while there are no unsaved edits.
    const ScriptView* loaded = nullptr;
    for (const ScriptView& s : data.scripts) {
        if (s.name == openScript_) {
            loaded = &s;
        }
    }
    if (loaded != nullptr && !scriptDirty_ && loaded->source != scriptLoaded_) {
        scriptBuffer_ = std::string(loaded->source);
        scriptLoaded_ = scriptBuffer_;
    }

    const bool save = ImGui::Button("Save") ||
                      (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) &&
                       ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
    ImGui::SetItemTooltip("Write content/scripts/%s.lua and run it (Ctrl+S)", openScript_.c_str());
    if (save) {
        req.commands.push_back(
            commands::makeCommand("SaveScript", {{"name", openScript_}, {"source", scriptBuffer_}}));
        scriptLoaded_ = scriptBuffer_;
        scriptDirty_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Run without saving")) {
        req.commands.push_back(commands::makeCommand("LoadScript", {{"name", openScript_}, {"source", scriptBuffer_}}));
    }
    ImGui::SetItemTooltip("Run this text now; the file stays as it is");
    ImGui::SameLine();
    ImGui::BeginDisabled(!scriptDirty_);
    if (ImGui::Button("Revert")) {
        scriptBuffer_ = scriptLoaded_;
        scriptDirty_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload from file")) {
        req.commands.push_back(commands::makeCommand("ReloadScript", {{"name", openScript_}}));
        scriptDirty_ = false;
    }
    if (!scriptStatus_.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(scriptStatusError_ ? palette::error() : palette::textDim(), "%s", scriptStatus_.c_str());
    }
    if (loaded != nullptr && !loaded->ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette::error());
        ImGui::TextWrapped("%.*s", static_cast<int>(loaded->error.size()), loaded->error.data());
        ImGui::PopStyleColor();
    }

    ImGui::PushFont(fonts_.mono, 0.0f);
    if (ImGui::InputTextMultiline("##source", scriptBuffer_.data(), scriptBuffer_.capacity() + 1, ImVec2(-1.0f, -1.0f),
                                  ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize,
                                  resizeString, &scriptBuffer_)) {
        scriptDirty_ = scriptBuffer_ != scriptLoaded_;
    }
    ImGui::PopFont();
}

void EditorShell::consoleOutput(const std::string& text, bool error) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() || end != std::string::npos) {
            consoleLines_.push_back({line, error});
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    while (consoleLines_.size() > 2000) {
        consoleLines_.erase(consoleLines_.begin());
    }
    consoleScroll_ = true;
}

}  // namespace crowdsim::ui
