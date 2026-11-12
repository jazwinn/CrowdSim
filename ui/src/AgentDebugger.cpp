/// @file AgentDebugger.cpp
/// Inspector view of one agent: type, state, locomotion intent, blackboard and
/// its behaviour tree with the nodes visited / running on the last decision tick.

#include <imgui.h>

#include <cstdio>
#include <format>
#include <string>

#include "crowdsim/sim/Simulation.hpp"
#include "crowdsim/ui/SceneEditor.hpp"

namespace crowdsim::ui {

namespace {

const char* moveModeName(sim::MoveMode m) {
    switch (m) {
        case sim::MoveMode::Stop:
            return "stop";
        case sim::MoveMode::Goal:
            return "goal (flow field)";
        case sim::MoveMode::Point:
            return "point (path corridor)";
    }
    return "?";
}

const char* statusName(sim::BtStatus s) {
    switch (s) {
        case sim::BtStatus::Success:
            return "success";
        case sim::BtStatus::Failure:
            return "failure";
        case sim::BtStatus::Running:
            return "running";
    }
    return "?";
}

/// "Wait 2 (+3)" / "Queue \"Kiosk A\"" style summary of a node's parameters.
std::string paramSummary(const sim::CompiledTree& tree, const sim::BtNode& n) {
    std::string s;
    if (n.text != 0 && n.text < tree.strings.size()) {
        s += std::format(" \"{}\"", tree.strings[n.text]);
    }
    if (n.number[0] != 0.0f || n.number[1] != 0.0f) {
        s += std::format(" {:g}", n.number[0]);
        if (n.number[1] != 0.0f) {
            s += std::format(" / {:g}", n.number[1]);
        }
    }
    if (n.hasVec) {
        s += std::format(" ({:.1f}, {:.1f}, {:.1f})", n.vec.x, n.vec.y, n.vec.z);
    }
    return s;
}

void drawNode(const sim::CompiledTree& tree, const sim::BtState& bt, std::uint16_t index) {
    const sim::BtNode& n = tree.nodes[index];
    const std::uint64_t bit = index < 64 ? (1ull << index) : 0ull;
    const bool running = (bt.running & bit) != 0;
    const bool visited = (bt.visited & bit) != 0;
    const ImVec4 color = running   ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
                         : visited ? ImVec4(0.95f, 0.8f, 0.35f, 1.0f)
                                   : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    const std::string& id = index < tree.nodeIds.size() ? tree.nodeIds[index] : std::string();
    const std::string& type = index < tree.nodeTypes.size() ? tree.nodeTypes[index] : std::string("?");
    std::string label = type;
    if (!id.empty()) {
        label += " [" + id + "]";
    }
    label += paramSummary(tree, n);
    if (n.memSlot != sim::kBtNoSlot) {
        label += std::format("  mem={}", bt.memory[n.memSlot]);
    }
    if (n.timerSlot != sim::kBtNoSlot && bt.timers[n.timerSlot] > 0.0f) {
        label += std::format("  t={:.1f}", bt.timers[n.timerSlot]);
    }
    const auto kids = tree.children(n);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (kids.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(index) + 1), flags, "%s%s",
                                        running ? "> " : "  ", label.c_str());
    ImGui::PopStyleColor();
    if (open && !kids.empty()) {
        for (const std::uint16_t c : kids) {
            drawNode(tree, bt, c);
        }
        ImGui::TreePop();
    }
}

}  // namespace

void SceneEditor::drawAgentInspector(const SceneEditorInput& in) {
    const auto info = in.sim.agentDebug(selectedAgent_);
    if (!info) {
        ImGui::Text("Agent #%u", selectedAgent_);
        ImGui::TextDisabled("This agent has left the simulation.");
        if (ImGui::Button("Clear selection")) {
            selectedAgent_ = sim::kNoAgent;
        }
        return;
    }
    ImGui::Text("Agent #%u  (%s)", info->id, info->typeName.c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &followAgent);
    ImGui::Separator();
    if (ImGui::BeginTable("agent", 2, ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char* key, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", key);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };
        row("state", info->stateName);
        row("behaviour",
            std::format("{} (gen {})", info->treeName, info->tree != nullptr ? info->tree->generation : 0));
        row("goal", info->goalName.empty() ? std::string("-") : info->goalName);
        row("move", moveModeName(info->moveMode));
        if (info->moveMode == sim::MoveMode::Point) {
            row("target",
                std::format("({:.1f}, {:.1f}, {:.1f})", info->moveTarget.x, info->moveTarget.y, info->moveTarget.z));
        }
        row("position", std::format("({:.1f}, {:.1f}, {:.1f})", info->position.x, info->position.y, info->position.z));
        row("speed", std::format("{:.2f} m/s (x{:.2f})", info->speed, info->speedMultiplier));
        if (info->queueStage != 0) {
            static constexpr const char* kStages[] = {"-", "waiting in line", "being served", "served", "done"};
            row("queue", kStages[info->queueStage < 5 ? info->queueStage : 0]);
        }
        if (info->groupId != sim::kNoAgent && info->groupId != info->id) {
            row("group", std::format("follows #{}", info->groupId));
        }
        row("last tick", statusName(info->bt.lastStatus));
        if (info->bt.hasTarget != 0) {
            row("bb.target",
                std::format("({:.1f}, {:.1f}, {:.1f})", info->bt.target.x, info->bt.target.y, info->bt.target.z));
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Behaviour tree  (green: running, yellow: visited last tick)");
    if (info->tree != nullptr && !info->tree->nodes.empty()) {
        drawNode(*info->tree, info->bt, 0);
    }
}

}  // namespace crowdsim::ui
