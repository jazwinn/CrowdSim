#include "crowdsim/ui/SceneEditor.hpp"

#include <imgui.h>
// ImGuizmo must come after imgui.h.
#include <ImGuizmo.h>

#include <algorithm>
#include <charconv>
#include <cmath>

#include <cstdio>
#include <format>
#include <glm/vec3.hpp>
#include <string>

#include "crowdsim/assets/JsonTypes.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"
#include "crowdsim/sim/Simulation.hpp"

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

std::string entityLabel(const Json& e) {
    return std::format("{}  #{}", e.value("name", std::string("(unnamed)")), e.value("id", 0u));
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
    if (ImGui::Button("Add...")) {
        ImGui::OpenPopup("AddEntity");
    }
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

    auto section = [&](const char* title, const std::vector<sim::SceneId>& ids) {
        if (!ImGui::CollapsingHeader(std::format("{} ({})###{}", title, ids.size(), title).c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        for (sim::SceneId id : ids) {
            const Json e = assets::entityToJson(in.sim, id);
            ImGui::PushID(static_cast<int>(id));
            if (ImGui::Selectable(entityLabel(e).c_str(), selected_ == id)) {
                selected_ = id;
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete")) {
                    commands.push_back(makeCommand("DeleteEntity", {{"id", id}}));
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    };
    section("Goals", in.sim.goalIds());
    section("Spawners", in.sim.spawnerIds());
    section("Links", in.sim.offMeshLinkIds());
    section("Objects", in.sim.staticObjectIds());
}

bool SceneEditor::editJson(const char* label, Json& value, Json& patch, const std::string& path) {
    ImGui::PushID(path.c_str());
    bool done = false;
    auto finished = [&]() { return ImGui::IsItemDeactivatedAfterEdit(); };
    const std::string key = label;

    if (value.is_boolean()) {
        bool b = value.get<bool>();
        if (ImGui::Checkbox(label, &b)) {
            value = b;
            done = true;
        }
    } else if (value.is_number_integer() || value.is_number_unsigned()) {
        int v = value.get<int>();
        ImGui::DragInt(label, &v, 0.25f);
        value = v;
        done = finished();
    } else if (value.is_number()) {
        float v = value.get<float>();
        ImGui::DragFloat(label, &v, key == "rotationY" ? 1.0f : 0.05f, 0.0f, 0.0f, "%.3f");
        value = assets::roundFloat(v);
        done = finished();
    } else if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (key == "color" && s.size() == 7) {
            unsigned rgb = 0;
            std::from_chars(s.data() + 1, s.data() + s.size(), rgb, 16);
            float c[3] = {static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                          static_cast<float>((rgb >> 8) & 0xff) / 255.0f, static_cast<float>(rgb & 0xff) / 255.0f};
            if (ImGui::ColorEdit3(label, c)) {
                value = std::format("#{:02x}{:02x}{:02x}", static_cast<int>(c[0] * 255.0f + 0.5f),
                                    static_cast<int>(c[1] * 255.0f + 0.5f), static_cast<int>(c[2] * 255.0f + 0.5f));
            }
            done = finished();
        } else {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            ImGui::InputText(label, buf, sizeof(buf));
            value = std::string(buf);
            done = finished();
        }
    } else if (value.is_array() && (value.size() == 2 || value.size() == 3) &&
               std::all_of(value.begin(), value.end(), [](const Json& x) { return x.is_number(); })) {
        float v[3] = {0, 0, 0};
        for (std::size_t i = 0; i < value.size(); ++i) {
            v[i] = value[i].get<float>();
        }
        if (value.size() == 2) {
            ImGui::DragFloat2(label, v, 0.05f, 0.0f, 0.0f, "%.3f");
        } else {
            ImGui::DragFloat3(label, v, 0.05f, 0.0f, 0.0f, "%.3f");
        }
        for (std::size_t i = 0; i < value.size(); ++i) {
            value[i] = assets::roundFloat(v[i]);
        }
        done = finished();
    } else if (value.is_array() &&
               !std::all_of(value.begin(), value.end(), [](const Json& x) { return x.is_string(); })) {
        // Structured list (e.g. a spawner's agentMix of {type, weight}): edit each element;
        // merge-patch replaces arrays, so any change sends the whole array.
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen, "%s (%zu)", label, value.size())) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                Json ignored = Json::object();
                const std::string item = std::format("[{}]", i);
                if (editJson(item.c_str(), value[i], ignored, path + "/" + std::to_string(i))) {
                    done = true;
                }
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
        ImGui::InputText(label, buf, sizeof(buf));
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
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
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
        ImGui::TextDisabled("%s: none", label);
        ImGui::SameLine();
        if (ImGui::SmallButton("set")) {
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
    if (selectedAgent_ != kNoAgentSelected) {
        drawAgentInspector(in);
        return;
    }
    if (selected_ == 0 || assets::entityKindOf(in.sim, selected_) == assets::EntityKind::None) {
        ImGui::TextDisabled("Nothing selected. Click an object or agent in the viewport, or the Scene list.");
        return;
    }
    // Keep a working copy while a widget is being edited, so drags are not reset every frame.
    if (bufferId_ != selected_ || !ImGui::IsAnyItemActive()) {
        buffer_ = assets::entityToJson(in.sim, selected_);
        bufferId_ = selected_;
    }
    ImGui::Text("%s #%u", buffer_.value("kind", std::string("?")).c_str(), selected_);
    ImGui::Separator();
    Json patch = Json::object();
    for (auto it = buffer_.begin(); it != buffer_.end(); ++it) {
        if (it.key() == "id" || it.key() == "kind" || it.key() == "asset") {
            continue;
        }
        if (it.key() == "goal" && buffer_.value("kind", std::string()) == "spawner") {
            // Goal reference: pick from existing goals.
            const auto goals = in.sim.goalIds();
            const sim::SceneId current = it.value().get<sim::SceneId>();
            const sim::Goal* g = in.sim.findGoal(current);
            if (ImGui::BeginCombo("goal", g != nullptr ? std::format("{} #{}", g->name, current).c_str() : "(none)")) {
                for (sim::SceneId gid : goals) {
                    if (ImGui::Selectable(std::format("{} #{}", in.sim.findGoal(gid)->name, gid).c_str(),
                                          gid == current)) {
                        patch["goal"] = gid;
                    }
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
