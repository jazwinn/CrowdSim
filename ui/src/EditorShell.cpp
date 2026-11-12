#include "crowdsim/ui/EditorShell.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace crowdsim::ui {

namespace {

constexpr const char* kViewportWindow = "Viewport";
constexpr const char* kSimWindow = "Simulation";
constexpr const char* kStatsWindow = "Stats";
constexpr const char* kLogWindow = "Log";
constexpr const char* kHierarchyWindow = "Scene";
constexpr const char* kInspectorWindow = "Inspector";

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
    ImGui::DockBuilderDockWindow(kViewportWindow, center);
    ImGui::DockBuilderFinish(dockspaceId);
}

EditorRequests EditorShell::draw(const EditorFrameData& data) {
    EditorRequests req;
    req.simSpeed = data.simSpeed;

    // Main menu
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save screenshot", "F12")) {
                req.screenshot = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) {
                req.quit = true;
            }
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

    // Viewport: transparent, the 3D scene is rendered behind the UI.
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin(kViewportWindow, nullptr, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled("3D viewport (scene rendering arrives in M1)");
    ImGui::End();

    ImGui::Begin(kHierarchyWindow);
    ImGui::TextDisabled("Empty scene");
    ImGui::End();

    ImGui::Begin(kInspectorWindow);
    ImGui::TextDisabled("Nothing selected");
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
    ImGui::End();

    ImGui::Begin(kStatsWindow);
    const float fps = data.frameMs > 0.0f ? 1000.0f / data.frameMs : 0.0f;
    ImGui::Text("Frame     %.2f ms (%.0f FPS)", data.frameMs, fps);
    ImGui::Text("Sim step  %.3f ms", data.simStepMs);
    ImGui::Text("Renderer  %.*s", static_cast<int>(data.rendererName.size()), data.rendererName.data());
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
