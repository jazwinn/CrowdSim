#include "crowdsim/ui/SceneEditor.hpp"

#include "crowdsim/ui/Inspect.hpp"

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
#include <set>
#include <string>

#include "crowdsim/assets/BehaviorJson.hpp"
#include "crowdsim/assets/JsonTypes.hpp"
#include "crowdsim/assets/SceneReflection.hpp"
#include "crowdsim/assets/TagCatalog.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"
#include "crowdsim/core/Parse.hpp"
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
/// Drag-and-drop payload: a SceneId being moved into a group.
constexpr const char* kEntityPayload = "CS_SCENE_ENTITY";

/// "Level 1 / Shops": a group's name with the groups it is in.
std::string groupPath(const sim::Simulation& sim, sim::SceneId id) {
    std::string path;
    for (sim::SceneId g = id; g != sim::kNoSceneId; g = sim.parentOf(g)) {
        const sim::Group* group = sim.findGroup(g);
        path = path.empty() ? group->name : group->name + " / " + path;
    }
    return path;
}

/// The show/hide toggle in front of a Scene list row: an open eye when shown, struck
/// through when hidden, dimmed when a group it is in is hidden. Returns true when clicked.
bool eyeToggle(bool hidden, bool hiddenByGroup) {
    const float size = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##eye", ImVec2(size * 0.75f, ImGui::GetTextLineHeight()));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip(
        hidden ? "Hidden: click to show"
               : (hiddenByGroup ? "Hidden with its group (click to hide it too)" : "Shown: click to hide (H)"));
    ImVec4 tint = ImGui::GetStyleColorVec4(hovered ? ImGuiCol_Text
                                                   : (hidden || hiddenByGroup ? ImGuiCol_TextDisabled : ImGuiCol_Text));
    if (!hovered && !hidden && !hiddenByGroup) {
        tint.w *= 0.6f;  // shown: present but quiet
    }
    const ImU32 colour = ImGui::GetColorU32(tint);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(pos.x + size * 0.36f, pos.y + ImGui::GetTextLineHeight() * 0.5f);
    const float w = size * 0.25f;
    const float h = size * 0.15f;
    const float thick = std::max(1.0f, size * 0.06f);
    // Almond outline (two arcs) and a pupil.
    dl->AddBezierQuadratic(ImVec2(c.x - w, c.y), ImVec2(c.x, c.y - 2.0f * h), ImVec2(c.x + w, c.y), colour, thick);
    dl->AddBezierQuadratic(ImVec2(c.x - w, c.y), ImVec2(c.x, c.y + 2.0f * h), ImVec2(c.x + w, c.y), colour, thick);
    if (!hidden) {
        dl->AddCircleFilled(c, h * 0.75f, colour);
    } else {
        dl->AddLine(ImVec2(c.x - w, c.y + w * 0.8f), ImVec2(c.x + w, c.y - w * 0.8f), colour, thick);
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    return clicked;
}

/// A dim note at the right end of the last item; left out where it would run into the
/// item's own label, which ends at `labelEnd` (screen x; negative = do not check).
void rightAlignedNote(const std::string& text, float labelEnd = -1.0f) {
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float x = max.x - size.x - ImGui::GetStyle().FramePadding.x;
    if (labelEnd >= 0.0f && labelEnd + ImGui::GetStyle().ItemSpacing.x > x) {
        return;
    }
    ImGui::GetWindowDrawList()->AddText(ImVec2(x, (min.y + max.y - size.y) * 0.5f),
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
    if (ImGui::MenuItem("Group")) {
        Json group{{"name", "Group"}};
        if (in.sim.findGroup(selected_) != nullptr) {
            group["parent"] = selected_;
        }
        commands.push_back(makeCommand("CreateGroup", {{"group", group}}));
    }
    ImGui::SetItemTooltip("A folder for scene entities (inside the selected group, if any)");
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

    auto heading = [&](const char* title) {
        ImGui::PushFont(fonts_.strong, 0.0f);
        const bool open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen);
        ImGui::PopFont();
        return open;
    };

    // Open the groups around a newly selected entity (picked in the viewport, say) once.
    std::set<sim::SceneId> reveal;
    if (selected_ != revealed_ && assets::entityKindOf(in.sim, selected_) != assets::EntityKind::None) {
        for (sim::SceneId p = in.sim.parentOf(selected_); p != sim::kNoSceneId; p = in.sim.parentOf(p)) {
            reveal.insert(p);
        }
        revealed_ = selected_;
    }
    revealGroups_ = &reveal;

    // Two views of the same entities, picked with a two-way switch: Hierarchy, everything
    // in its groups; Lists, every entity of each kind, flat.
    auto segment = [&](const char* label, bool active, float width) {
        if (active) {
            const ImVec4 a = palette::accent();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(a.x, a.y, a.z, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, a);
        }
        const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
        if (active) {
            ImGui::PopStyleColor(3);
        }
        return clicked && !active;
    };
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 0.5f) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x * 0.5f, 0.0f));
    if (segment("Hierarchy", !showLists_, half)) {
        showLists_ = false;
    }
    // Dropping an entity on the Hierarchy switch takes it out of its group.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            const auto id = *static_cast<const sim::SceneId*>(payload->Data);
            commands.push_back(makeCommand("SetParent", {{"id", id}, {"parent", 0}}));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("Everything in the scene, in its groups. Drop something here to take it out of its group.");
    ImGui::SameLine();
    if (segment("Lists", showLists_, half)) {
        showLists_ = true;
    }
    ImGui::SetItemTooltip("Every goal, spawner, link and object, wherever it is in the hierarchy");
    ImGui::PopStyleVar();
    ImGui::Spacing();
    if (!showLists_) {
        // What is in no group sits at the top level.
        std::vector<sim::SceneId> topLevel;
        for (const auto& ids : {in.sim.groupIds(), in.sim.goalIds(), in.sim.spawnerIds(), in.sim.offMeshLinkIds(),
                                in.sim.staticObjectIds()}) {
            for (sim::SceneId id : ids) {
                if (in.sim.parentOf(id) == sim::kNoSceneId) {
                    topLevel.push_back(id);
                }
            }
        }
        ImGui::Spacing();
        if (topLevel.empty()) {
            ImGui::TextDisabled("Empty scene");
        }
        for (sim::SceneId id : topLevel) {
            if (in.sim.findGroup(id) != nullptr) {
                groupNode(in, id, commands);
            } else {
                entityRow(in, id, commands);
            }
        }
    }
    revealGroups_ = nullptr;

    if (showLists_) {
        ImGui::Spacing();
        auto section = [&](const char* title, const std::vector<sim::SceneId>& ids) {
            const bool open = heading(title);
            rightAlignedNote(std::to_string(ids.size()));
            if (!open) {
                return;
            }
            ImGui::Indent();
            if (ids.empty()) {
                ImGui::TextDisabled("None");
            }
            for (sim::SceneId id : ids) {
                entityRow(in, id, commands);
            }
            ImGui::Unindent();
            ImGui::Spacing();
        };
        section("Goals", in.sim.goalIds());
        section("Spawners", in.sim.spawnerIds());
        section("Links", in.sim.offMeshLinkIds());
        section("Objects", in.sim.staticObjectIds());

        // Agent types are content (agents/*.agent.json), not scene entities, but they are
        // picked and edited the same way.
        const auto& types = in.sim.agentTypes();
        const bool open = heading("Agent types");
        rightAlignedNote(std::to_string(types.size()));
        if (open) {
            ImGui::Indent();
            for (const sim::AgentType& t : types) {
                ImGui::PushID(t.name.c_str());
                if (ImGui::Selectable(t.name.c_str(), selectedType_ == t.name)) {
                    selectedType_ = t.name;
                    selected_ = 0;
                    selectedAgent_ = kNoAgentSelected;
                }
                if (t.behaviour != t.name) {
                    rightAlignedNote(t.behaviour, ImGui::GetItemRectMin().x + ImGui::CalcTextSize(t.name.c_str()).x);
                }
                ImGui::PopID();
            }
            ImGui::Unindent();
            ImGui::Spacing();
        }
    }
}

void SceneEditor::drawAgentTypeInspector(const SceneEditorInput& in, std::vector<Json>& commands) {
    const sim::AgentType* type = in.sim.findAgentType(selectedType_);
    if (type == nullptr) {
        selectedType_.clear();
        return;
    }
    // Working copy while a widget is being edited, so drags are not reset every frame.
    if (typeBuffer_.name != selectedType_ || !ImGui::IsAnyItemActive()) {
        typeBuffer_ = *type;
    }
    ImGui::PushFont(fonts_.strong, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::TextUnformatted(type->name.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("Agent type");
    ImGui::Separator();
    ImGui::Spacing();
    auto custom = [&](std::string_view widget, const char* label, Json& value) -> std::optional<bool> {
        if (widget != "behaviour") {
            return std::nullopt;
        }
        propertyLabel(label);
        bool done = false;
        const std::string current = value.get<std::string>();
        if (ImGui::BeginCombo("##behaviour", current.c_str())) {
            for (const sim::CompiledTree& tree : in.sim.behaviorTrees()) {
                if (ImGui::Selectable(tree.name.c_str(), tree.name == current) && tree.name != current) {
                    value = tree.name;
                    done = true;
                }
            }
            ImGui::EndCombo();
        }
        return done;
    };
    // The name identifies the type (spawners refer to it), so it is not edited here.
    if (inspect(typeBuffer_, custom, "name")) {
        commands.push_back(makeCommand("DefineAgentType", {{"type", assets::toJson(typeBuffer_)}}));
    }
    ImGui::Spacing();
    if (ImGui::Button("Save to file", ImVec2(-1.0f, 0.0f))) {
        commands.push_back(makeCommand("SaveAgentType", {{"name", type->name}}));
    }
    ImGui::SetItemTooltip("Write it to its agents/*.agent.json file; edits otherwise last until the content reloads");
}

void SceneEditor::entityRow(const SceneEditorInput& in, sim::SceneId id, std::vector<Json>& commands) {
    const assets::EntityKind kind = assets::entityKindOf(in.sim, id);
    const Json e = assets::entityToJson(in.sim, id);
    const std::string name = e.value("name", std::string("(unnamed)"));
    ImGui::PushID(static_cast<int>(id));
    const bool hidden = in.sim.isHidden(id);
    const bool shown = in.sim.isVisible(id);
    if (eyeToggle(hidden, !hidden && !shown)) {
        commands.push_back(makeCommand("SetHidden", {{"id", id}, {"hidden", !hidden}}));
    }
    if (!shown) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    if (ImGui::Selectable(name.c_str(), selected_ == id)) {
        selected_ = id;
        selectedAgent_ = kNoAgentSelected;
    }
    if (!shown) {
        ImGui::PopStyleColor();
    }
    const float labelEnd = ImGui::GetItemRectMin().x + ImGui::CalcTextSize(name.c_str()).x;
    if (labelEnd > ImGui::GetItemRectMax().x) {
        ImGui::SetItemTooltip("%s", name.c_str());  // cut off by the panel edge
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &id, sizeof(id));
        ImGui::Text("Move '%s' to a group", e.value("name", std::string("(unnamed)")).c_str());
        ImGui::EndDragDropSource();
    }
    rightAlignedNote(std::format("{} #{}", assets::entityKindName(kind), id), labelEnd);
    if (ImGui::BeginPopupContextItem()) {
        moveToGroupMenu(in, id, commands);
        if (ImGui::MenuItem("Delete")) {
            commands.push_back(makeCommand("DeleteEntity", {{"id", id}}));
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void SceneEditor::groupNode(const SceneEditorInput& in, sim::SceneId id, std::vector<Json>& commands) {
    const sim::Group* group = in.sim.findGroup(id);
    const std::vector<sim::SceneId> children = in.sim.childrenOf(id);
    ImGui::PushID(static_cast<int>(id));
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (selected_ == id) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    const bool hidden = in.sim.isHidden(id);
    const bool shown = in.sim.isVisible(id);
    if (eyeToggle(hidden, !hidden && !shown)) {
        commands.push_back(makeCommand("SetHidden", {{"id", id}, {"hidden", !hidden}}));
    }
    if (!shown) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    if (revealGroups_ != nullptr && revealGroups_->contains(id)) {
        ImGui::SetNextItemOpen(true);  // right before the tree node: the eye is an item too
    }
    const bool open = ImGui::TreeNodeEx("##group", flags, "%s", group->name.c_str());
    if (!shown) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected_ = id;
        selectedAgent_ = kNoAgentSelected;
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &id, sizeof(id));
        ImGui::Text("Move group '%s'", group->name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            const auto child = *static_cast<const sim::SceneId*>(payload->Data);
            if (child != id) {
                commands.push_back(makeCommand("SetParent", {{"id", child}, {"parent", id}}));
            }
        }
        ImGui::EndDragDropTarget();
    }
    rightAlignedNote(std::to_string(children.size()), ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing() +
                                                          ImGui::CalcTextSize(group->name.c_str()).x);
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("New group inside")) {
            commands.push_back(makeCommand("CreateGroup", {{"group", {{"name", "Group"}, {"parent", id}}}}));
        }
        moveToGroupMenu(in, id, commands);
        if (ImGui::MenuItem("Delete group (keep contents)")) {
            commands.push_back(makeCommand("DeleteEntity", {{"id", id}}));
        }
        ImGui::EndPopup();
    }
    if (open) {
        for (sim::SceneId child : children) {
            if (in.sim.findGroup(child) != nullptr) {
                groupNode(in, child, commands);
            } else {
                entityRow(in, child, commands);
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void SceneEditor::moveToGroupMenu(const SceneEditorInput& in, sim::SceneId id, std::vector<Json>& commands) {
    const sim::SceneId current = in.sim.parentOf(id);
    if (!ImGui::BeginMenu("Move to group")) {
        return;
    }
    if (ImGui::MenuItem("(no group)", nullptr, current == sim::kNoSceneId, current != sim::kNoSceneId)) {
        commands.push_back(makeCommand("SetParent", {{"id", id}, {"parent", 0}}));
    }
    for (sim::SceneId g : in.sim.groupIds()) {
        // Not into itself or anything inside it.
        bool inside = false;
        for (sim::SceneId p = g; p != sim::kNoSceneId && !inside; p = in.sim.parentOf(p)) {
            inside = p == id;
        }
        if (inside) {
            continue;
        }
        ImGui::PushID(static_cast<int>(g));
        if (ImGui::MenuItem(groupPath(in.sim, g).c_str(), nullptr, current == g, current != g)) {
            commands.push_back(makeCommand("SetParent", {{"id", id}, {"parent", g}}));
        }
        ImGui::PopID();
    }
    ImGui::EndMenu();
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
            // Behaviour tree name, dimmed and right-aligned, when it adds something: not when it
            // repeats the type's name, and not where it would run into it.
            if (types[t].behaviour != types[t].name) {
                rightAlignedNote(types[t].behaviour,
                                 ImGui::GetItemRectMin().x + ImGui::CalcTextSize(types[t].name.c_str()).x);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return done;
}

namespace {

/// `text` cut to `width` with an ellipsis.
std::string fitText(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        return text;
    }
    std::string out = text;
    while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > width) {
        out.pop_back();
    }
    return out + "...";
}

std::string numberText(float v) {
    return std::format("{:g}", assets::roundFloat(v));
}

}  // namespace

bool SceneEditor::editTags(const char* label, Json& value, const assets::TagCatalog* catalog) {
    std::vector<std::string> tags;
    for (const Json& t : value) {
        tags.push_back(t.get<std::string>());
    }
    bool done = false;
    auto present = [&](const std::string& tag) { return std::find(tags.begin(), tags.end(), tag) != tags.end(); };

    // Heading row: the label, and the Add menu in the value column.
    propertyLabel(label);
    if (ImGui::BeginCombo("##addTag", "Add tag...", ImGuiComboFlags_HeightLarge)) {
        std::string group;
        if (catalog != nullptr) {
            for (const assets::TagSpec* spec : catalog->all()) {
                const bool have = spec->kind != assets::TagKind::Text &&
                                  std::any_of(tags.begin(), tags.end(),
                                              [&](const std::string& t) { return catalog->match(t).spec == spec; });
                if (have) {
                    continue;  // a flag or number tag is there once
                }
                if (spec->group != group) {
                    group = spec->group;
                    ImGui::SeparatorText(group.c_str());
                }
                const std::string shown = spec->kind == assets::TagKind::Text ? spec->name + "..." : spec->name;
                if (ImGui::Selectable(shown.c_str())) {
                    if (!spec->requiredTag.empty() && !present(spec->requiredTag)) {
                        tags.push_back(spec->requiredTag);  // what this tag refines
                    }
                    tags.push_back(assets::TagCatalog::compose(
                        *spec, spec->kind == assets::TagKind::Number ? numberText(spec->defaultValue) : ""));
                    done = true;
                }
                if (!spec->description.empty()) {
                    ImGui::SetItemTooltip("%s", spec->description.c_str());
                }
            }
        }
        ImGui::SeparatorText("Other");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##customTag", "any tag, then Enter", customTag_, sizeof(customTag_),
                                     ImGuiInputTextFlags_EnterReturnsTrue) &&
            customTag_[0] != '\0') {
            if (!present(customTag_)) {
                tags.emplace_back(customTag_);
                done = true;
            }
            customTag_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip(
        "Tags describe the object; engine mechanics and scripts read them (content/tags explains each)");

    // One row per tag: its name, then its value or meaning, and a remove button.
    const float removeW = ImGui::GetFrameHeight();
    std::size_t remove = tags.size();
    ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.5f);
    for (std::size_t i = 0; i < tags.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const assets::TagMatch m = catalog != nullptr ? catalog->match(tags[i]) : assets::TagMatch{};
        // A refinement ("attention:salience") reads as an indented "salience" under its tag.
        std::string name = m.spec != nullptr ? m.spec->name : tags[i];
        const bool refines = m.spec != nullptr && !m.spec->requiredTag.empty() &&
                             name.starts_with(m.spec->requiredTag + ":") &&
                             name.size() > m.spec->requiredTag.size() + 1;
        if (refines) {
            name = "  " + name.substr(m.spec->requiredTag.size() + 1);
        }
        if (m.spec != nullptr && m.spec->kind == assets::TagKind::Text) {
            name += "...";
        }
        propertyLabel(name.c_str());
        const float width = ImGui::GetContentRegionAvail().x - removeW - ImGui::GetStyle().ItemSpacing.x;
        if (m.spec == nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, palette::textDim());
            ImGui::TextUnformatted(fitText("not in the tag catalogue", width).c_str());
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Kept as it is. Describe it in content/tags/*.tags.json to explain it here.");
        } else if (m.spec->kind == assets::TagKind::Number) {
            float v = m.spec->defaultValue;
            (void)core::parseFloat(m.value.data(), m.value.data() + m.value.size(), v);
            const std::string format = "%.2f" + (m.spec->unit.empty() ? std::string() : " " + m.spec->unit);
            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderFloat("##v", &v, m.spec->min, m.spec->max, format.c_str())) {
                tags[i] = assets::TagCatalog::compose(*m.spec, numberText(v));
            }
            done = ImGui::IsItemDeactivatedAfterEdit() || done;
            ImGui::SetItemTooltip("%s", m.spec->description.c_str());
        } else if (m.spec->kind == assets::TagKind::Text) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s", m.value.c_str());
            ImGui::SetNextItemWidth(width);
            if (ImGui::InputText("##v", buf, sizeof(buf))) {
                tags[i] = assets::TagCatalog::compose(*m.spec, buf);
            }
            done = ImGui::IsItemDeactivatedAfterEdit() || done;
            ImGui::SetItemTooltip("%s", m.spec->description.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, palette::textDim());
            ImGui::TextUnformatted(fitText(m.spec->description, width).c_str());
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", m.spec->description.c_str());
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - removeW);
        if (ImGui::Button("x", ImVec2(removeW, 0.0f))) {
            remove = i;
        }
        ImGui::SetItemTooltip("Remove this tag");
        ImGui::PopID();
    }
    ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.5f);
    if (remove < tags.size()) {
        tags.erase(tags.begin() + static_cast<std::ptrdiff_t>(remove));
        done = true;
    }
    value = tags;
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
    ImGui::SetItemTooltip(
        "Each agent this spawner creates picks one of these types at random,\n"
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
        const std::string share =
            total > 0.0 ? std::format("{:.0f}%", 100.0 * entry.value("weight", 1.0) / total) : "-";
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

template <typename T>
void SceneEditor::inspectEntity(const Json& live, Json& patch, const CustomWidget& custom) {
    // The working copy (buffer_) holds the document while a widget is being edited, so a drag
    // carries on from where it is rather than from the scene's value.
    T value{};
    try {
        reflect::fromJson(buffer_, value);
    } catch (const std::exception& e) {
        ImGui::TextColored(palette::warning(), "%s", e.what());
        return;
    }
    const bool committed = inspect(value, custom);
    const Json now = reflect::toJson(value);
    for (const auto& [key, v] : now.items()) {
        buffer_[key] = v;
    }
    if (committed) {
        for (const auto& [key, v] : now.items()) {
            if (!live.contains(key) || live[key] != v) {
                patch[key] = v;
            }
        }
    }
}

void SceneEditor::drawInspector(const SceneEditorInput& in, std::vector<Json>& commands) {
    inspectedSim_ = &in.sim;
    if (selected_ != 0 || selectedAgent_ != kNoAgentSelected) {
        selectedType_.clear();  // something else was picked
    }
    if (!selectedType_.empty()) {
        drawAgentTypeInspector(in, commands);
        return;
    }
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
    {
        const sim::SceneId parent = in.sim.parentOf(selected_);
        propertyLabel("Group");
        if (ImGui::BeginCombo("##parent", parent == sim::kNoSceneId ? "(none)" : groupPath(in.sim, parent).c_str())) {
            if (ImGui::Selectable("(none)", parent == sim::kNoSceneId) && parent != sim::kNoSceneId) {
                commands.push_back(makeCommand("SetParent", {{"id", selected_}, {"parent", 0}}));
            }
            for (sim::SceneId g : in.sim.groupIds()) {
                bool inside = false;
                for (sim::SceneId p = g; p != sim::kNoSceneId && !inside; p = in.sim.parentOf(p)) {
                    inside = p == selected_;
                }
                if (inside) {
                    continue;
                }
                ImGui::PushID(static_cast<int>(g));
                if (ImGui::Selectable(groupPath(in.sim, g).c_str(), g == parent) && g != parent) {
                    commands.push_back(makeCommand("SetParent", {{"id", selected_}, {"parent", g}}));
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    {
        bool visible = !in.sim.isHidden(selected_);
        propertyLabel("Visible");
        if (ImGui::Checkbox("##visible", &visible)) {
            commands.push_back(makeCommand("SetHidden", {{"id", selected_}, {"hidden", !visible}}));
        }
        if (visible && !in.sim.isVisible(selected_)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(its group is hidden)");
        }
        ImGui::SetItemTooltip("Display only: hidden objects still block and carry people (H toggles)");
    }
    // The entity's own fields, from its type's reflection descriptor (assets/SceneReflection.hpp).
    const Json live = assets::entityToJson(in.sim, selected_);
    auto custom = [&](std::string_view widget, const char* label, Json& value) -> std::optional<bool> {
        if (widget == "goal") {
            const sim::Goal* g = value.is_number() ? in.sim.findGoal(value.get<sim::SceneId>()) : nullptr;
            propertyLabel(label);
            bool done = false;
            if (ImGui::BeginCombo("##goal", g != nullptr ? g->name.c_str() : "(none)")) {
                for (sim::SceneId gid : in.sim.goalIds()) {
                    ImGui::PushID(static_cast<int>(gid));
                    if (ImGui::Selectable(in.sim.findGoal(gid)->name.c_str(), g == in.sim.findGoal(gid))) {
                        value = gid;
                        done = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            return done;
        }
        if (widget == "agentType" || widget == "agentTypeName") {
            propertyLabel(label);
            return agentTypeCombo("##agentType", value, widget == "agentType");
        }
        if (widget == "agentMix") {
            ImGui::Spacing();
            return editAgentMix(value);
        }
        if (widget == "objectTags") {
            return editTags(label, value, in.tags);
        }
        return std::nullopt;
    };
    switch (assets::entityKindOf(in.sim, selected_)) {
        case assets::EntityKind::Object:
            inspectEntity<sim::StaticObject>(live, patch, custom);
            break;
        case assets::EntityKind::Link:
            inspectEntity<sim::OffMeshLink>(live, patch, custom);
            break;
        case assets::EntityKind::Goal:
            inspectEntity<sim::Goal>(live, patch, custom);
            break;
        case assets::EntityKind::Spawner:
            inspectEntity<sim::Spawner>(live, patch, custom);
            break;
        case assets::EntityKind::Group:
            inspectEntity<sim::Group>(live, patch, custom);
            break;
        case assets::EntityKind::None:
            break;
    }
    if (!patch.empty()) {
        commands.push_back(makeCommand("UpdateEntity", {{"id", selected_}, {"patch", patch}}));
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
    if (ImGui::IsKeyPressed(ImGuiKey_H, false) && selected_ != 0 &&
        assets::entityKindOf(in.sim, selected_) != assets::EntityKind::None) {
        commands.push_back(makeCommand("SetHidden", {{"id", selected_}, {"hidden", !in.sim.isHidden(selected_)}}));
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
        if (!in.sim.isVisible(goalIds[g])) {
            continue;
        }
        const sim::Goal* goal = in.sim.findGoal(goalIds[g]);
        const bool lit = !focus || selected_ == goalIds[g];
        const ImVec4 c = palette::series(g);
        outline(goal->area, withAlpha(c, lit ? 0.18f : 0.06f), withAlpha(c, lit ? 0.95f : 0.35f), 2.0f * scale);
    }
    const bool labels = spawnerIds.size() <= 6;
    for (const sim::SceneId sid : spawnerIds) {
        const sim::Spawner* s = in.sim.findSpawner(sid);
        const auto gi = std::find(goalIds.begin(), goalIds.end(), s->goal);
        if (gi == goalIds.end() || !in.sim.isVisible(sid)) {
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
            dl->AddText(ImVec2(p0.x + pad + 1.0f * scale, p0.y + pad * 0.7f), IM_COL32(235, 238, 241, lit ? 255 : 130),
                        text.c_str());
        }
    }
    dl->PopClipRect();
}

void SceneEditor::drawGizmo(const SceneEditorInput& in, std::vector<Json>& commands) {
    ImGuizmo::BeginFrame();
    gizmoActive_ = false;
    const assets::EntityKind kind = assets::entityKindOf(in.sim, selected_);
    if (selected_ == 0 || kind == assets::EntityKind::None || kind == assets::EntityKind::Link ||
        kind == assets::EntityKind::Group || in.view == nullptr) {
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
