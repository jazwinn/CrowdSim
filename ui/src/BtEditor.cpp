#include "crowdsim/ui/BtEditor.hpp"

#include <imgui-node-editor/imgui_node_editor.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#include "crowdsim/assets/BehaviorJson.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace ed = ax::NodeEditor;

namespace crowdsim::ui {

namespace {

using Json = nlohmann::json;

// Separate id ranges so nodes, pins and links never collide.
ed::NodeId nodeHandle(int id) {
    return ed::NodeId(static_cast<std::uintptr_t>(id));
}
ed::PinId inputPin(int id) {
    return ed::PinId(static_cast<std::uintptr_t>(1'000'000 + id * 2));
}
ed::PinId outputPin(int id) {
    return ed::PinId(static_cast<std::uintptr_t>(1'000'000 + id * 2 + 1));
}
ed::LinkId linkHandle(int id) {
    return ed::LinkId(static_cast<std::uintptr_t>(5'000'000 + id));
}
/// {node id, is output} for a pin handle.
std::pair<int, bool> pinOwner(ed::PinId pin) {
    const auto v = static_cast<long long>(pin.Get()) - 1'000'000;
    return {static_cast<int>(v / 2), (v % 2) == 1};
}

ImVec4 categoryColor(const std::string& type, const sim::Simulation& sim) {
    if (const sim::BtNodeTypeInfo* info = sim::findBtNodeType(type); info != nullptr && type != "Custom") {
        switch (info->category) {
            case sim::BtCategory::Composite:
                return {0.45f, 0.70f, 1.0f, 1.0f};
            case sim::BtCategory::Decorator:
                return {0.80f, 0.60f, 1.0f, 1.0f};
            case sim::BtCategory::Action:
                return {0.55f, 0.90f, 0.55f, 1.0f};
            case sim::BtCategory::Condition:
                return {1.0f, 0.80f, 0.40f, 1.0f};
        }
    }
    for (const sim::CustomNodeSpec& s : sim.customNodeSpecs()) {
        if (s.name == type) {
            return {0.40f, 0.95f, 0.90f, 1.0f};  // script node
        }
    }
    return {1.0f, 0.4f, 0.4f, 1.0f};
}

/// Number edited in the UI, kept short in the JSON (4 decimals).
double tidy(float v) {
    return std::round(static_cast<double>(v) * 10000.0) / 10000.0;
}

}  // namespace

struct BtEditor::Context {
    ed::EditorContext* editor = nullptr;
    ImVec2 createAt{0.0f, 0.0f};
    Context() {
        ed::Config config;
        config.SettingsFile = nullptr;  // layout lives in the *.bt.json "editor" block
        editor = ed::CreateEditor(&config);
    }
    ~Context() { ed::DestroyEditor(editor); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};

BtEditor::BtEditor() : ctx_(std::make_unique<Context>()) {
    newGraph();
}

BtEditor::~BtEditor() = default;

void BtEditor::newGraph() {
    graph_ = assets::BtGraph{};
    graph_.name = "new_behaviour";
    const int root = graph_.addNode("Sequence", 0.0f, 0.0f);
    needsPlacement_ = {root};
    navigateFrames_ = 3;
    std::snprintf(nameBuffer_, sizeof(nameBuffer_), "%s", graph_.name.c_str());
}

void BtEditor::open(const Json& doc) {
    try {
        graph_ = assets::BtGraph::fromJson(doc);
    } catch (const std::exception& e) {
        setStatus(std::string("Cannot open: ") + e.what(), true);
        return;
    }
    needsPlacement_.clear();
    for (const assets::BtGraphNode& n : graph_.nodes) {
        needsPlacement_.insert(n.id);
    }
    navigateFrames_ = 3;
    std::snprintf(nameBuffer_, sizeof(nameBuffer_), "%s", graph_.name.c_str());
    setStatus("Opened '" + graph_.name + "'", false);
}

void BtEditor::setStatus(std::string text, bool error) {
    status_ = std::move(text);
    statusError_ = error;
}

void BtEditor::draw(const sim::Simulation& sim, std::uint32_t selectedAgent, std::vector<Json>& commands) {
    toolbar(sim, selectedAgent, commands);
    canvas(sim, selectedAgent);
}

void BtEditor::toolbar(const sim::Simulation& sim, std::uint32_t selectedAgent, std::vector<Json>& commands) {
    // Open an existing tree.
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##open", "Open tree...")) {
        for (const sim::CompiledTree& t : sim.behaviorTrees()) {
            if (ImGui::Selectable(t.name.c_str())) {
                commands.push_back(commands::makeCommand("GetBehaviorTree", {{"name", t.name}}));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const auto agent = selectedAgent != 0xFFFFFFFFu ? sim.agentDebug(selectedAgent) : std::nullopt;
    ImGui::BeginDisabled(!agent);
    if (ImGui::Button("Agent's tree") && agent) {
        commands.push_back(commands::makeCommand("GetBehaviorTree", {{"name", agent->treeName}}));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        newGraph();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputText("Name", nameBuffer_, sizeof(nameBuffer_))) {
        graph_.name = nameBuffer_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Layout")) {
        graph_.autoLayout();
        for (const assets::BtGraphNode& n : graph_.nodes) {
            needsPlacement_.insert(n.id);
        }
        navigateFrames_ = 3;
    }

    // Validate: structural issues first, then the real compiler.
    const auto issues = graph_.validate(sim.customNodeSpecs());
    std::string compileError;
    Json doc;
    if (issues.empty()) {
        try {
            doc = graph_.toJson();
            (void)assets::compileBehaviorJson(doc, sim.customNodeSpecs());
        } catch (const std::exception& e) {
            compileError = e.what();
        }
    }
    const bool valid = issues.empty() && compileError.empty();
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Apply")) {
        commands.push_back(commands::makeCommand("SetBehaviorTree", {{"tree", doc}}));
        setStatus("Applied '" + graph_.name + "': agents using it restart it", false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        commands.push_back(commands::makeCommand("SaveBehaviorTree", {{"tree", doc}}));
        setStatus("Saving behaviours/" + graph_.name + ".bt.json", false);
    }
    ImGui::EndDisabled();

    if (!valid) {
        const std::string first = !issues.empty() ? issues.front().message : compileError;
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%zu problem%s: %s",
                           std::max<std::size_t>(1, issues.size()), issues.size() > 1 ? "s" : "", first.c_str());
    } else if (!status_.empty()) {
        ImGui::TextColored(statusError_ ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s",
                           status_.c_str());
    } else {
        ImGui::TextDisabled(
            "Right-click: add node. Drag from a node's right pin to another's left pin to link. "
            "Children run top to bottom.");
    }
}

void BtEditor::canvas(const sim::Simulation& sim, std::uint32_t selectedAgent) {
    const auto custom = std::span<const sim::CustomNodeSpec>(sim.customNodeSpecs());
    const auto issues = graph_.validate(custom);

    // Live highlighting: map the selected agent's compiled node flags onto graph nodes by node id.
    std::map<std::string, int> liveState;  // nodeId -> 2 running, 1 visited
    if (selectedAgent != 0xFFFFFFFFu) {
        if (const auto d = sim.agentDebug(selectedAgent); d && d->tree != nullptr && d->treeName == graph_.name) {
            for (std::size_t i = 0; i < d->tree->nodeIds.size() && i < 64; ++i) {
                const std::uint64_t bit = 1ull << i;
                if ((d->bt.running & bit) != 0) {
                    liveState[d->tree->nodeIds[i]] = 2;
                } else if ((d->bt.visited & bit) != 0) {
                    liveState[d->tree->nodeIds[i]] = 1;
                }
            }
        }
    }

    ed::SetCurrentEditor(ctx_->editor);
    ed::Begin("bt_canvas");
    for (int id : needsPlacement_) {
        if (const assets::BtGraphNode* n = graph_.findNode(id)) {
            ed::SetNodePosition(nodeHandle(id), ImVec2(n->x, n->y));
        }
    }
    const bool placedThisFrame = !needsPlacement_.empty();
    needsPlacement_.clear();

    for (assets::BtGraphNode& n : graph_.nodes) {
        const auto live = liveState.find(n.nodeId);
        const bool hasIssue =
            std::any_of(issues.begin(), issues.end(), [&](const assets::BtGraphIssue& i) { return i.node == n.id; });
        int pushed = 0;
        if (live != liveState.end()) {
            ed::PushStyleColor(ed::StyleColor_NodeBorder,
                               live->second == 2 ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
            ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 3.0f);
            pushed = 1;
        } else if (hasIssue) {
            ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
            ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 2.0f);
            pushed = 1;
        }
        ed::BeginNode(nodeHandle(n.id));
        ImGui::PushID(n.id);
        ed::BeginPin(inputPin(n.id), ed::PinKind::Input);
        ImGui::TextUnformatted(">");
        ed::EndPin();
        ImGui::SameLine();
        ImGui::TextColored(categoryColor(n.type, sim), "%s", n.type.c_str());
        const auto [minChildren, maxChildren] = assets::BtGraph::childLimits(n.type, custom);
        (void)minChildren;
        if (maxChildren > 0) {
            ImGui::SameLine();
            ed::BeginPin(outputPin(n.id), ed::PinKind::Output);
            ImGui::TextUnformatted(">");
            ed::EndPin();
        }
        ImGui::TextDisabled("%s", n.nodeId.c_str());
        for (const assets::BtGraph::ParamInfo& p : assets::BtGraph::paramsOf(n.type, custom)) {
            const bool set = n.params.contains(p.name);
            const Json value = set ? n.params[p.name] : p.defaultValue;
            if (!set) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            ImGui::SetNextItemWidth(120.0f);
            switch (p.kind) {
                case sim::BtParamKind::Number: {
                    float v = value.is_number() ? value.get<float>() : 0.0f;
                    if (ImGui::DragFloat(p.name.c_str(), &v, 0.05f)) {
                        n.params[p.name] = tidy(v);
                    }
                    break;
                }
                case sim::BtParamKind::Text: {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s", value.is_string() ? value.get<std::string>().c_str() : "");
                    if (ImGui::InputText(p.name.c_str(), buf, sizeof(buf))) {
                        n.params[p.name] = std::string(buf);
                    }
                    break;
                }
                case sim::BtParamKind::Vec3: {
                    float v[3] = {0.0f, 0.0f, 0.0f};
                    if (value.is_array() && value.size() == 3) {
                        for (int k = 0; k < 3; ++k) {
                            v[k] = value[static_cast<std::size_t>(k)].get<float>();
                        }
                    }
                    if (ImGui::DragFloat3(p.name.c_str(), v, 0.1f)) {
                        n.params[p.name] = Json::array({tidy(v[0]), tidy(v[1]), tidy(v[2])});
                    }
                    break;
                }
            }
            if (!set) {
                ImGui::PopStyleColor();
            }
        }
        for (const assets::BtGraphIssue& i : issues) {
            if (i.node == n.id) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", i.message.c_str());
            }
        }
        ImGui::PopID();
        ed::EndNode();
        if (pushed != 0) {
            ed::PopStyleVar();
            ed::PopStyleColor();
        }
    }
    for (const assets::BtGraphLink& l : graph_.links) {
        const auto live = liveState.find(graph_.findNode(l.child)->nodeId);
        const ImVec4 color = live != liveState.end() && live->second == 2 ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f)
                                                                          : ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        ed::Link(linkHandle(l.id), outputPin(l.parent), inputPin(l.child), color, 2.0f);
    }

    // New links: drag from a parent's output pin to a child's input pin.
    if (ed::BeginCreate(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 2.0f)) {
        ed::PinId a;
        ed::PinId b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            auto [na, aOut] = pinOwner(a);
            auto [nb, bOut] = pinOwner(b);
            if (aOut == bOut) {
                ed::RejectNewItem(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 2.0f);
            } else {
                const int parent = aOut ? na : nb;
                const int child = aOut ? nb : na;
                std::string why;
                if (!graph_.canLink(parent, child, custom, &why)) {
                    ed::Suspend();
                    ImGui::SetTooltip("%s", why.c_str());
                    ed::Resume();
                    ed::RejectNewItem(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 2.0f);
                } else if (ed::AcceptNewItem(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), 3.0f)) {
                    graph_.link(parent, child, custom);
                }
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) {
        ed::LinkId link;
        while (ed::QueryDeletedLink(&link)) {
            if (ed::AcceptDeletedItem()) {
                graph_.unlink(static_cast<int>(link.Get() - 5'000'000));
            }
        }
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node)) {
            if (ed::AcceptDeletedItem()) {
                graph_.removeNode(static_cast<int>(node.Get()));
            }
        }
    }
    ed::EndDelete();

    // Add nodes from the background context menu.
    const ImVec2 mouse = ImGui::GetMousePos();
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu()) {
        ctx_->createAt = mouse;
        ImGui::OpenPopup("bt_create");
    }
    if (ImGui::BeginPopup("bt_create")) {
        auto item = [&](const std::string& type, const char* description) {
            if (ImGui::MenuItem(type.c_str())) {
                const int id = graph_.addNode(type, ctx_->createAt.x, ctx_->createAt.y);
                needsPlacement_.insert(id);
            }
            if (description != nullptr && description[0] != '\0' && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", description);
            }
        };
        const std::pair<sim::BtCategory, const char*> groups[] = {{sim::BtCategory::Composite, "Composites"},
                                                                  {sim::BtCategory::Decorator, "Decorators"},
                                                                  {sim::BtCategory::Action, "Actions"},
                                                                  {sim::BtCategory::Condition, "Conditions"}};
        for (const auto& [category, label] : groups) {
            if (ImGui::BeginMenu(label)) {
                for (const sim::BtNodeTypeInfo& info : sim::btNodeTypes()) {
                    if (info.category == category && info.type != sim::BtNodeType::Custom) {
                        item(info.name, info.description);
                    }
                }
                ImGui::EndMenu();
            }
        }
        if (!custom.empty() && ImGui::BeginMenu("Script (Lua)")) {
            for (const sim::CustomNodeSpec& s : custom) {
                item(s.name, s.description.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    if (navigateFrames_ > 0 && !placedThisFrame) {
        ed::NavigateToContent(0.0f);  // repeated: node sizes are only known after they are drawn
        --navigateFrames_;
    }
    ed::End();

    // The canvas owns positions while editing; mirror them into the graph (child order, saving).
    if (!placedThisFrame) {
        for (assets::BtGraphNode& n : graph_.nodes) {
            const ImVec2 p = ed::GetNodePosition(nodeHandle(n.id));
            if (std::isfinite(p.x) && std::isfinite(p.y) && p.x != FLT_MAX) {
                n.x = std::round(p.x);
                n.y = std::round(p.y);
            }
        }
    }
    ed::SetCurrentEditor(nullptr);
}

}  // namespace crowdsim::ui
