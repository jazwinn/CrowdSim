#include "crowdsim/ui/SceneEditor.hpp"

#include <imgui.h>
// ImGuizmo must come after imgui.h.
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

#include <cstdio>
#include <format>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <string>

#include "crowdsim/assets/JsonTypes.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"
#include "crowdsim/sim/Simulation.hpp"
#include "crowdsim/ui/Theme.hpp"

namespace crowdsim::ui {

namespace {

using Json = nlohmann::json;
using commands::makeCommand;

/// +1 if ImGuizmo's Y rotation turns +Z towards +X (CrowdSim's convention), else -1.
float gizmoYawSign() {
    const float t[3] = {0, 0, 0};
    const float r[3] = {0, 90, 0};
    const float s[3] = {1, 1, 1};
    float m[16];
    ImGuizmo::RecomposeMatrixFromComponents(t, r, s, m);
    return m[8] > 0.0f ? 1.0f : -1.0f;  // row 2 = local +Z in world
}

glm::vec3 placementPoint(const SceneEditorInput& in) {
    const float* p = in.hasCursorWorld ? in.cursorWorld : in.cameraTarget;
    return {std::round(p[0] * 4.0f) / 4.0f, std::max(0.0f, std::round(p[1] * 10.0f) / 10.0f),
            std::round(p[2] * 4.0f) / 4.0f};
}

Json v3(glm::vec3 v) {
    return Json::array({assets::roundFloat(v.x), assets::roundFloat(v.y), assets::roundFloat(v.z)});
}

/// Dim text drawn inside the right end of the last item, vertically centred on it.
void rightAlignedNote(const std::string& text) {
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(max.x - size.x - ImGui::GetStyle().FramePadding.x, (min.y + max.y - size.y) * 0.5f),
        ImGui::GetColorU32(ImGuiCol_TextDisabled), text.c_str());
}

}  // namespace

void SceneEditor::addMenu(const SceneEditorInput& in, std::vector<Json>& commands) {
    const glm::vec3 at = placementPoint(in);
    auto place = [&](const char* name, const char* type, glm::vec3 pos, glm::vec3 size, const char* tag,
                     const char* color, int steps = 0) {
        Json o{{"name", name},     {"type", type},  {"position", v3(pos)},
               {"size", v3(size)}, {"tags", {tag}}, {"color", color}};
        if (steps > 0) {
            o["steps"] = steps;
        }
        commands.push_back(makeCommand("PlaceObject", {{"object", o}}));
    };
    if (ImGui::MenuItem("Floor slab 20 x 20")) {
        place("Floor", "box", at - glm::vec3(0, 0.2f, 0), {20, 0.2f, 20}, "walkable", "#5c626a");
    }
    if (ImGui::MenuItem("Wall 10 m")) {
        place("Wall", "box", at, {10, 3, 0.3f}, "obstacle", "#a8b0b8");
    }
    if (ImGui::MenuItem("Pillar")) {
        place("Pillar", "box", at, {0.6f, 3, 0.6f}, "obstacle", "#aaa29a");
    }
    if (ImGui::MenuItem("Obstacle box")) {
        place("Obstacle", "box", at, {2, 1, 1}, "obstacle", "#8a6a4a");
    }
    if (ImGui::MenuItem("Ramp")) {
        place("Ramp", "ramp", at, {4, 2, 8}, "walkable", "#968a7d");
    }
    if (ImGui::MenuItem("Stairs")) {
        place("Stairs", "stairs", at, {4, 3, 6}, "walkable", "#968a7d", 15);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Goal")) {
        commands.push_back(makeCommand(
            "CreateGoal", {{"goal",
                            {{"name", "Goal"},
                             {"area", {{"center", {at.x, at.z}}, {"halfExtents", {3, 3}}, {"elevation", at.y}}}}}}));
    }
    if (ImGui::MenuItem("Spawner")) {
        const auto goals = in.sim.goalIds();
        commands.push_back(makeCommand(
            "CreateSpawner", {{"spawner",
                               {{"name", "Spawner"},
                                {"area", {{"center", {at.x, at.z}}, {"halfExtents", {3, 3}}, {"elevation", at.y}}},
                                {"rate", 10},
                                {"total", 200},
                                {"goal", goals.empty() ? 0u : goals.front()}}}}));
    }
    if (ImGui::MenuItem("Ladder link (up 4 m)")) {
        commands.push_back(makeCommand(
            "CreateOffMeshLink",
            {{"link", {{"start", v3(at)}, {"end", v3(at + glm::vec3(0, 4, 2))}, {"bidirectional", false}}}}));
    }
    if (!in.entityTypes.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Scripted (Lua)");
        for (const std::string& type : in.entityTypes) {
            if (ImGui::MenuItem(type.c_str())) {
                commands.push_back(makeCommand("SpawnEntity", {{"type", type}, {"position", v3(at)}}));
            }
        }
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##gltf", importPath_, sizeof(importPath_));
    ImGui::SameLine();
    if (ImGui::Button("Import glTF")) {
        commands.push_back(makeCommand("ImportGltf", {{"path", std::string(importPath_)}, {"position", v3(at)}}));
        ImGui::CloseCurrentPopup();
    }
}

void SceneEditor::drawHierarchy(const SceneEditorInput& in, std::vector<Json>& commands) {
    if (ImGui::Button("Add")) {
        ImGui::OpenPopup("AddEntity");
    }
    ImGui::SetItemTooltip("Add an object, goal, spawner or link at the cursor");
    if (ImGui::BeginPopup("AddEntity")) {
        addMenu(in, commands);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_ == 0 || assets::entityKindOf(in.sim, selected_) == assets::EntityKind::None);
    if (ImGui::Button("Delete")) {
        commands.push_back(makeCommand("DeleteEntity", {{"id", selected_}}));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snap);
    ImGui::SetItemTooltip("Snap the gizmo to %.2f m and %.0f deg steps", static_cast<double>(translateSnap),
                          static_cast<double>(rotateSnapDegrees));
    ImGui::Spacing();

    auto section = [&](const char* title, const std::vector<sim::SceneId>& ids) {
        ImGui::PushFont(fonts_.strong, 0.0f);
        const bool open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen);
        ImGui::PopFont();
        rightAlignedNote(std::to_string(ids.size()));
        if (!open) {
            return;
        }
        if (ids.empty()) {
            ImGui::Indent();
            ImGui::TextDisabled("None");
            ImGui::Unindent();
        }
        for (sim::SceneId id : ids) {
            const Json e = assets::entityToJson(in.sim, id);
            ImGui::PushID(static_cast<int>(id));
            ImGui::Indent();
            if (ImGui::Selectable(e.value("name", std::string("(unnamed)")).c_str(), selected_ == id)) {
                selected_ = id;
                selectedAgent_ = kNoAgentSelected;
            }
            rightAlignedNote(std::format("#{}", id));
            ImGui::Unindent();
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete")) {
                    commands.push_back(makeCommand("DeleteEntity", {{"id", id}}));
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
    };
    section("Goals", in.sim.goalIds());
    section("Spawners", in.sim.spawnerIds());
    section("Links", in.sim.offMeshLinkIds());
    section("Objects", in.sim.staticObjectIds());
}

bool SceneEditor::agentTypeCombo(const char* id, Json& value, bool byIndex) {
    // Agent types: a dropdown of the types defined right now (content files, Lua, MCP),
    // read from the simulation every frame so new types appear immediately.
    const auto& types = inspectedSim_->agentTypes();
    auto matches = [&](std::size_t t) {
        return byIndex ? value.is_number() && value.get<std::size_t>() == t
                       : value.is_string() && value.get<std::string>() == types[t].name;
    };
    std::string preview = byIndex ? "(unknown type)" : value.get<std::string>() + " (undefined)";
    for (std::size_t t = 0; t < types.size(); ++t) {
        if (matches(t)) {
            preview = types[t].name;
        }
    }
    bool done = false;
    if (ImGui::BeginCombo(id, preview.c_str())) {
        for (std::size_t t = 0; t < types.size(); ++t) {
            const bool selected = matches(t);
            ImGui::PushID(static_cast<int>(t));
            if (ImGui::Selectable(types[t].name.c_str(), selected) && !selected) {
                value = byIndex ? Json(t) : Json(types[t].name);
                done = true;
            }
            // Behaviour tree name, dimmed and right-aligned, so types are told apart at a glance.
            const float w = ImGui::CalcTextSize(types[t].behaviour.c_str()).x;
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(ImGui::GetItemRectMax().x - w - ImGui::GetStyle().ItemInnerSpacing.x, ImGui::GetItemRectMin().y),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), types[t].behaviour.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return done;
}

bool SceneEditor::editAgentMix(Json& value) {
    // A spawner's agentMix: one line per entry (type, weight, resulting share).
    // Merge-patch replaces arrays, so any change sends the whole list.
    bool done = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(palette::textDim(), "Agent mix");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("Each agent this spawner creates picks one of these types at random,\n"
                          "in proportion to its weight. With no entries, every agent uses the\n"
                          "Agent settings above.");
    double total = 0.0;
    for (const Json& e : value) {
        total += e.is_object() ? e.value("weight", 1.0) : 0.0;
    }
    if (value.empty()) {
        ImGui::TextDisabled("No entries: every agent uses the Agent settings.");
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const float removeW = ImGui::GetFrameHeight();
    const float shareW = em * 2.8f;
    const float weightW = em * 4.0f;
    std::size_t remove = value.size();
    for (std::size_t i = 0; i < value.size(); ++i) {
        Json& entry = value[i];
        if (!entry.is_object()) {
            continue;
        }
        ImGui::PushID(static_cast<int>(i));
        const float typeW = ImGui::GetContentRegionAvail().x - weightW - shareW - removeW - style.ItemSpacing.x * 3.0f;
        ImGui::SetNextItemWidth(std::max(typeW, em * 4.0f));
        Json type = entry.value("type", Json("default"));
        if (inspectedSim_ != nullptr && agentTypeCombo("##type", type, false)) {
            entry["type"] = type;
            done = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(weightW);
        float weight = static_cast<float>(entry.value("weight", 1.0));
        ImGui::DragFloat("##weight", &weight, 0.01f, 0.0f, 1000.0f, "%.2f");
        ImGui::SetItemTooltip("Weight (relative; does not need to add up to 1)");
        entry["weight"] = assets::roundFloat(std::max(weight, 0.0f));
        done = done || ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        const std::string share = total > 0.0 ? std::format("{:.0f}%", 100.0 * entry.value("weight", 1.0) / total) : "-";
        ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + shareW - ImGui::CalcTextSize(share.c_str()).x);
        ImGui::TextDisabled("%s", share.c_str());
        ImGui::SetItemTooltip("Share of agents with this type");
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(removeW, 0))) {
            remove = i;
        }
        ImGui::SetItemTooltip("Remove this type from the mix");
        ImGui::PopID();
    }
    if (remove < value.size()) {
        value.erase(value.begin() + static_cast<std::ptrdiff_t>(remove));
        done = true;
    }
    if (ImGui::Button("Add type", ImVec2(-1.0f, 0))) {
        const auto& types = inspectedSim_ != nullptr ? inspectedSim_->agentTypes() : std::vector<sim::AgentType>{};
        value.push_back(Json{{"type", types.empty() ? std::string("default") : types.front().name}, {"weight", 1.0}});
        done = true;
    }
    return done;
}

bool SceneEditor::editJson(const char* label, Json& value, Json& patch, const std::string& path) {
    ImGui::PushID(path.c_str());
    bool done = false;
    auto finished = [&]() { return ImGui::IsItemDeactivatedAfterEdit(); };
    const std::string key = label;
    const std::string nice = humanize(key);
    auto row = [&]() { propertyLabel(nice.c_str()); };

    // `agentType` (spawner default parameters) stores an index into the agent types.
    if (inspectedSim_ != nullptr && key == "agentType" && value.is_number()) {
        row();
        done = agentTypeCombo("##agentType", value, true);
    } else if (key == "agentMix" && value.is_array()) {
        ImGui::Spacing();
        done = editAgentMix(value);
    } else if (value.is_boolean()) {
        bool b = value.get<bool>();
        row();
        if (ImGui::Checkbox("##v", &b)) {
            value = b;
            done = true;
        }
    } else if (value.is_number_integer() || value.is_number_unsigned()) {
        int v = value.get<int>();
        row();
        ImGui::DragInt("##v", &v, 0.25f);
        value = v;
        done = finished();
    } else if (value.is_number()) {
        float v = value.get<float>();
        row();
        ImGui::DragFloat("##v", &v, key == "rotationY" ? 1.0f : 0.05f, 0.0f, 0.0f,
                         key == "rotationY" ? "%.1f deg" : "%.3f");
        value = assets::roundFloat(v);
        done = finished();
    } else if (value.is_string()) {
        const std::string s = value.get<std::string>();
        row();
        if (key == "color" && s.size() == 7) {
            unsigned rgb = 0;
            std::from_chars(s.data() + 1, s.data() + s.size(), rgb, 16);
            float c[3] = {static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                          static_cast<float>((rgb >> 8) & 0xff) / 255.0f, static_cast<float>(rgb & 0xff) / 255.0f};
            if (ImGui::ColorEdit3("##v", c, ImGuiColorEditFlags_DisplayHex)) {
                value = std::format("#{:02x}{:02x}{:02x}", static_cast<int>(c[0] * 255.0f + 0.5f),
                                    static_cast<int>(c[1] * 255.0f + 0.5f), static_cast<int>(c[2] * 255.0f + 0.5f));
            }
            done = finished();
        } else {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            ImGui::InputText("##v", buf, sizeof(buf));
            value = std::string(buf);
            done = finished();
        }
    } else if (value.is_array() && (value.size() == 2 || value.size() == 3) &&
               std::all_of(value.begin(), value.end(), [](const Json& x) { return x.is_number(); })) {
        float v[3] = {0, 0, 0};
        for (std::size_t i = 0; i < value.size(); ++i) {
            v[i] = value[i].get<float>();
        }
        row();
        if (value.size() == 2) {
            ImGui::DragFloat2("##v", v, 0.05f, 0.0f, 0.0f, "%.2f");
        } else {
            ImGui::DragFloat3("##v", v, 0.05f, 0.0f, 0.0f, "%.2f");
        }
        for (std::size_t i = 0; i < value.size(); ++i) {
            value[i] = assets::roundFloat(v[i]);
        }
        done = finished();
    } else if (value.is_array() &&
               !std::all_of(value.begin(), value.end(), [](const Json& x) { return x.is_string(); })) {
        // Other structured lists: edit, add and remove elements; any change sends the whole array.
        if (ImGui::TreeNodeEx("##list", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%zu)",
                              nice.c_str(), value.size())) {
            std::size_t remove = value.size();
            for (std::size_t i = 0; i < value.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                Json ignored = Json::object();
                if (editJson(std::format("item {}", i + 1).c_str(), value[i], ignored, path + "/" + std::to_string(i))) {
                    done = true;
                }
                if (ImGui::SmallButton("Remove")) {
                    remove = i;
                }
                ImGui::PopID();
            }
            if (remove < value.size()) {
                value.erase(value.begin() + static_cast<std::ptrdiff_t>(remove));
                done = true;
            }
            if (!value.empty() && ImGui::SmallButton("Add")) {
                value.push_back(value.back());
                done = true;
            }
            ImGui::TreePop();
        }
    } else if (value.is_array()) {  // tags (all strings)
        std::string joined;
        for (const Json& t : value) {
            joined += (joined.empty() ? "" : ", ") + t.get<std::string>();
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", joined.c_str());
        row();
        ImGui::InputTextWithHint("##v", "comma, separated", buf, sizeof(buf));
        if ((done = finished())) {
            Json tags = Json::array();
            std::string token;
            for (const char* c = buf;; ++c) {
                if (*c == ',' || *c == '\0') {
                    token.erase(0, token.find_first_not_of(' '));
                    token.erase(token.find_last_not_of(' ') + 1);
                    if (!token.empty()) {
                        tags.push_back(token);
                    }
                    token.clear();
                    if (*c == '\0') {
                        break;
                    }
                } else {
                    token += *c;
                }
            }
            value = tags;
        }
    } else if (value.is_object()) {
        ImGui::Spacing();
        if (ImGui::TreeNodeEx("##group", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s",
                              nice.c_str())) {
            Json sub = Json::object();
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (editJson(it.key().c_str(), it.value(), sub, path + "/" + it.key())) {
                    done = true;
                }
            }
            if (done) {
                patch[key] = sub;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        return done;
    } else if (value.is_null()) {
        row();
        if (ImGui::Button("Not set: click to set", ImVec2(-1.0f, 0))) {
            value = 60.0;
            done = true;
        }
    }
    if (done) {
        patch[key] = value;
    }
    ImGui::PopID();
    return done;
}

void SceneEditor::drawInspector(const SceneEditorInput& in, std::vector<Json>& commands) {
    inspectedSim_ = &in.sim;
    if (selectedAgent_ != kNoAgentSelected) {
        drawAgentInspector(in);
        return;
    }
    if (selected_ == 0 || assets::entityKindOf(in.sim, selected_) == assets::EntityKind::None) {
        ImGui::Spacing();
        ImGui::TextWrapped("Nothing selected.");
        ImGui::PushStyleColor(ImGuiCol_Text, palette::textDim());
        ImGui::TextWrapped("Click an object or an agent in the viewport, or pick one from the Scene list.");
        ImGui::PopStyleColor();
        return;
    }
    // Keep a working copy while a widget is being edited, so drags are not reset every frame.
    if (bufferId_ != selected_ || !ImGui::IsAnyItemActive()) {
        buffer_ = assets::entityToJson(in.sim, selected_);
        bufferId_ = selected_;
    }
    // Heading: the entity's name, with its kind and id underneath.
    ImGui::PushFont(fonts_.strong, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::TextUnformatted(buffer_.value("name", std::string("(unnamed)")).c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s  #%u", humanize(buffer_.value("kind", std::string("?"))).c_str(), selected_);
    ImGui::Separator();
    ImGui::Spacing();
    Json patch = Json::object();
    // Fields in alphabetical order (the JSON order), except the name, which comes first.
    std::vector<std::string> keys;
    if (buffer_.contains("name")) {
        keys.push_back("name");
    }
    for (auto it = buffer_.begin(); it != buffer_.end(); ++it) {
        if (it.key() != "name") {
            keys.push_back(it.key());
        }
    }
    for (const std::string& key : keys) {
        const auto it = buffer_.find(key);
        if (it.key() == "id" || it.key() == "kind" || it.key() == "asset") {
            continue;
        }
        if (it.key() == "goal" && buffer_.value("kind", std::string()) == "spawner") {
            // Goal reference: pick from existing goals.
            const auto goals = in.sim.goalIds();
            const sim::SceneId current = it.value().get<sim::SceneId>();
            const sim::Goal* g = in.sim.findGoal(current);
            propertyLabel("Goal");
            if (ImGui::BeginCombo("##goal", g != nullptr ? g->name.c_str() : "(none)")) {
                for (sim::SceneId gid : goals) {
                    ImGui::PushID(static_cast<int>(gid));
                    if (ImGui::Selectable(in.sim.findGoal(gid)->name.c_str(), gid == current)) {
                        patch["goal"] = gid;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            continue;
        }
        editJson(it.key().c_str(), it.value(), patch, "/" + it.key());
    }
    if (!patch.empty()) {
        commands.push_back(makeCommand("UpdateEntity", {{"id", selected_}, {"patch", patch}}));
    }
}

void SceneEditor::drawHistory(const SceneEditorInput& in, std::vector<Json>& commands) {
    ImGui::BeginDisabled(!in.dispatcher.canUndo());
    if (ImGui::Button("Undo")) {
        commands.push_back(makeCommand("Undo"));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!in.dispatcher.canRedo());
    if (ImGui::Button("Redo")) {
        commands.push_back(makeCommand("Redo"));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu logged", in.dispatcher.log().size());
    ImGui::Separator();
    for (const commands::HistoryItem& item : in.dispatcher.history()) {
        if (item.undone) {
            ImGui::TextDisabled("  %s", item.label.c_str());
        } else {
            ImGui::BulletText("%s", item.label.c_str());
        }
    }
}

void SceneEditor::handleShortcuts(const SceneEditorInput& in, std::vector<Json>& commands) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && selected_ != 0 &&
        assets::entityKindOf(in.sim, selected_) != assets::EntityKind::None) {
        commands.push_back(makeCommand("DeleteEntity", {{"id", selected_}}));
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && selected_ != 0) {
        Json e = assets::entityToJson(in.sim, selected_);
        if (!e.is_null()) {
            e.erase("id");
            if (e.contains("position")) {
                e["position"][0] = e["position"][0].get<float>() + 1.0f;
            }
            commands.push_back(makeCommand("AddEntity", {{"entity", e}}));
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
        gizmoTranslate_ = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        gizmoTranslate_ = false;
    }
}

void SceneEditor::drawRoutes(const SceneEditorInput& in) const {
    if (in.view == nullptr || in.proj == nullptr || in.viewportW <= 0.0f || in.viewportH <= 0.0f) {
        return;
    }
    // World -> viewport (ImGui) coordinates; bx matrices are for row vectors.
    auto project = [&](glm::vec3 p, ImVec2& out) {
        const float* v = in.view;
        const float* m = in.proj;
        const float x = p.x * v[0] + p.y * v[4] + p.z * v[8] + v[12];
        const float y = p.x * v[1] + p.y * v[5] + p.z * v[9] + v[13];
        const float z = p.x * v[2] + p.y * v[6] + p.z * v[10] + v[14];
        const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        if (cw < 0.05f) {
            return false;  // behind the camera
        }
        out = ImVec2(in.viewportX + (cx / cw * 0.5f + 0.5f) * in.viewportW,
                     in.viewportY + (0.5f - cy / cw * 0.5f) * in.viewportH);
        return true;
    };
    auto withAlpha = [](ImVec4 c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); };

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(in.viewportX, in.viewportY),
                     ImVec2(in.viewportX + in.viewportW, in.viewportY + in.viewportH), true);
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const auto goalIds = in.sim.goalIds();
    const auto spawnerIds = in.sim.spawnerIds();
    const bool focus = std::find(goalIds.begin(), goalIds.end(), selected_) != goalIds.end() ||
                       std::find(spawnerIds.begin(), spawnerIds.end(), selected_) != spawnerIds.end();

    auto outline = [&](const sim::Area& a, ImU32 fill, ImU32 edge, float thickness) {
        const float y = a.elevation + 0.06f;
        const glm::vec3 corners[4] = {{a.center.x - a.halfExtents.x, y, a.center.y - a.halfExtents.y},
                                      {a.center.x + a.halfExtents.x, y, a.center.y - a.halfExtents.y},
                                      {a.center.x + a.halfExtents.x, y, a.center.y + a.halfExtents.y},
                                      {a.center.x - a.halfExtents.x, y, a.center.y + a.halfExtents.y}};
        ImVec2 pts[4];
        for (int k = 0; k < 4; ++k) {
            if (!project(corners[k], pts[k])) {
                return;
            }
        }
        dl->AddConvexPolyFilled(pts, 4, fill);
        dl->AddPolyline(pts, 4, edge, ImDrawFlags_Closed, thickness);
    };

    // Goals first (outlined once in their colour), then one arc per spawner.
    for (std::size_t g = 0; g < goalIds.size(); ++g) {
        const sim::Goal* goal = in.sim.findGoal(goalIds[g]);
        const bool lit = !focus || selected_ == goalIds[g];
        const ImVec4 c = palette::series(g);
        outline(goal->area, withAlpha(c, lit ? 0.18f : 0.06f), withAlpha(c, lit ? 0.95f : 0.35f), 2.0f * scale);
    }
    const bool labels = spawnerIds.size() <= 6;
    for (const sim::SceneId sid : spawnerIds) {
        const sim::Spawner* s = in.sim.findSpawner(sid);
        const auto gi = std::find(goalIds.begin(), goalIds.end(), s->goal);
        if (gi == goalIds.end()) {
            continue;
        }
        const std::size_t g = static_cast<std::size_t>(gi - goalIds.begin());
        const sim::Goal* goal = in.sim.findGoal(s->goal);
        const bool lit = !focus || selected_ == sid || selected_ == s->goal;
        const ImVec4 c = palette::series(g);
        const ImU32 col = withAlpha(c, lit ? 1.0f : 0.3f);
        outline(s->area, withAlpha(c, lit ? 0.12f : 0.04f), col, 1.5f * scale);

        // Arc from the spawner to the goal, lifted so it reads above walls and floors.
        const glm::vec3 a(s->area.center.x, s->area.elevation + 0.1f, s->area.center.y);
        const glm::vec3 b(goal->area.center.x, goal->area.elevation + 0.1f, goal->area.center.y);
        const float lift = std::max(2.5f, 0.3f * glm::length(glm::vec2(b.x - a.x, b.z - a.z)));
        glm::vec3 mid = 0.5f * (a + b);
        mid.y = std::max(a.y, b.y) + lift;
        constexpr int kSegments = 40;
        std::array<ImVec2, kSegments + 1> pts{};
        int count = 0;
        for (int k = 0; k <= kSegments; ++k) {
            const float t = static_cast<float>(k) / kSegments;
            const glm::vec3 p = (1 - t) * (1 - t) * a + 2 * (1 - t) * t * mid + t * t * b;
            if (project(p, pts[static_cast<std::size_t>(count)])) {
                ++count;
            }
        }
        if (count < 2) {
            continue;
        }
        const float thickness = (selected_ == sid ? 4.0f : 2.5f) * scale;
        dl->AddPolyline(pts.data(), count, col, ImDrawFlags_None, thickness);
        dl->AddCircleFilled(pts[0], 4.0f * scale, col);
        // Arrowhead at the goal end.
        const ImVec2 tip = pts[static_cast<std::size_t>(count - 1)];
        const ImVec2 prev = pts[static_cast<std::size_t>(std::max(0, count - 4))];
        const float dx = tip.x - prev.x;
        const float dy = tip.y - prev.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-3f) {
            const float ux = dx / len;
            const float uy = dy / len;
            const float sz = 11.0f * scale;
            dl->AddTriangleFilled(tip, ImVec2(tip.x - ux * sz - uy * sz * 0.55f, tip.y - uy * sz + ux * sz * 0.55f),
                                  ImVec2(tip.x - ux * sz + uy * sz * 0.55f, tip.y - uy * sz - ux * sz * 0.55f), col);
        }
        // Label at the top of the arc: "spawner -> goal".
        if (labels || selected_ == sid) {
            const std::string text = s->name + "  \xE2\x86\x92  " + goal->name;
            const ImVec2 at = pts[static_cast<std::size_t>(count / 2)];
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const float pad = 4.0f * scale;
            const ImVec2 p0(at.x - size.x * 0.5f - pad, at.y - size.y - pad * 3.0f);
            dl->AddRectFilled(p0, ImVec2(p0.x + size.x + pad * 2.0f, p0.y + size.y + pad * 1.4f),
                              IM_COL32(18, 20, 23, lit ? 210 : 110), 4.0f * scale);
            dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f * scale, p0.y + size.y + pad * 1.4f), col, 4.0f * scale,
                              ImDrawFlags_RoundCornersLeft);
            dl->AddText(ImVec2(p0.x + pad + 1.0f * scale, p0.y + pad * 0.7f),
                        IM_COL32(235, 238, 241, lit ? 255 : 130), text.c_str());
        }
    }
    dl->PopClipRect();
}

void SceneEditor::drawGizmo(const SceneEditorInput& in, std::vector<Json>& commands) {
    ImGuizmo::BeginFrame();
    gizmoActive_ = false;
    const assets::EntityKind kind = assets::entityKindOf(in.sim, selected_);
    if (selected_ == 0 || kind == assets::EntityKind::None || kind == assets::EntityKind::Link || in.view == nullptr) {
        wasUsingGizmo_ = false;
        return;
    }
    const Json e = assets::entityToJson(in.sim, selected_);
    float t[3];
    float yawDeg = 0.0f;
    if (kind == assets::EntityKind::Object) {
        for (int i = 0; i < 3; ++i) {
            t[i] = e["position"][i].get<float>();
        }
        yawDeg = e.value("rotationY", 0.0f);
    } else {
        const Json& area = e["area"];
        t[0] = area["center"][0].get<float>();
        t[1] = area.value("elevation", 0.0f);
        t[2] = area["center"][1].get<float>();
    }
    const float sign = gizmoYawSign();
    const float r[3] = {0.0f, yawDeg * sign, 0.0f};
    const float s[3] = {1.0f, 1.0f, 1.0f};
    float model[16];
    ImGuizmo::RecomposeMatrixFromComponents(t, r, s, model);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(in.viewportX, in.viewportY, in.viewportW, in.viewportH);
    const bool translate = gizmoTranslate_ || kind != assets::EntityKind::Object;
    const float translateSnapValues[3] = {translateSnap, translateSnap, translateSnap};
    const float rotateSnapValues[3] = {rotateSnapDegrees, rotateSnapDegrees, rotateSnapDegrees};
    const bool changed =
        ImGuizmo::Manipulate(in.view, in.proj, translate ? ImGuizmo::TRANSLATE : ImGuizmo::ROTATE_Y, ImGuizmo::WORLD,
                             model, nullptr, snap ? (translate ? translateSnapValues : rotateSnapValues) : nullptr);
    const bool using_ = ImGuizmo::IsUsing();
    gizmoActive_ = using_ || ImGuizmo::IsOver();
    if (using_ && !wasUsingGizmo_) {
        ++dragSerial_;  // new drag: a new undo step
    }
    wasUsingGizmo_ = using_;
    if (!changed) {
        return;
    }
    float nt[3];
    float nr[3];
    float ns[3];
    ImGuizmo::DecomposeMatrixToComponents(model, nt, nr, ns);
    Json cmd;
    if (kind == assets::EntityKind::Object) {
        Json params{{"id", selected_},
                    {"position",
                     Json::array({assets::roundFloat(nt[0]), assets::roundFloat(nt[1]), assets::roundFloat(nt[2])})}};
        if (!translate) {
            params["rotationY"] = assets::roundFloat(nr[1] * sign);
        }
        cmd = makeCommand("TransformObject", params);
    } else {
        Json area;
        area["center"] = Json::array({assets::roundFloat(nt[0]), assets::roundFloat(nt[2])});
        area["elevation"] = assets::roundFloat(nt[1]);
        Json params;
        params["id"] = selected_;
        params["patch"]["area"] = area;
        cmd = makeCommand("UpdateEntity", params);
    }
    cmd["merge"] = std::format("gizmo:{}:{}", selected_, dragSerial_);
    commands.push_back(std::move(cmd));
}

}  // namespace crowdsim::ui
