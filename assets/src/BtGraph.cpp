#include "crowdsim/assets/BtGraph.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <stdexcept>

namespace crowdsim::assets {

namespace {

constexpr float kColumn = 260.0f;  ///< auto layout: x step per depth
constexpr float kRow = 150.0f;     ///< auto layout: y step per leaf

const char* kindName(sim::BtParamKind k) {
    switch (k) {
        case sim::BtParamKind::Number:
            return "number";
        case sim::BtParamKind::Text:
            return "string";
        case sim::BtParamKind::Vec3:
            return "vec3";
    }
    return "number";
}

}  // namespace

std::pair<int, int> BtGraph::childLimits(const std::string& type, std::span<const sim::CustomNodeSpec> custom) {
    if (const sim::BtNodeTypeInfo* info = sim::findBtNodeType(type);
        info != nullptr && info->type != sim::BtNodeType::Custom) {
        return {info->minChildren, info->maxChildren};
    }
    (void)custom;  // script nodes are leaves
    return {0, 0};
}

std::vector<BtGraph::ParamInfo> BtGraph::paramsOf(const std::string& type,
                                                  std::span<const sim::CustomNodeSpec> custom) {
    std::vector<ParamInfo> out;
    if (const sim::BtNodeTypeInfo* info = sim::findBtNodeType(type);
        info != nullptr && info->type != sim::BtNodeType::Custom) {
        for (std::uint8_t i = 0; i < info->paramCount; ++i) {
            const sim::BtParamSpec& p = info->params[i];
            ParamInfo pi{p.name, p.kind, nullptr, p.description};
            if (p.kind == sim::BtParamKind::Number) {
                pi.defaultValue = p.defaultNumber;
            } else if (p.kind == sim::BtParamKind::Text) {
                pi.defaultValue = p.defaultText;
            }
            out.push_back(std::move(pi));
        }
        return out;
    }
    for (const sim::CustomNodeSpec& spec : custom) {
        if (spec.name == type) {
            for (const sim::CustomParamSpec& p : spec.params) {
                ParamInfo pi{p.name, p.kind, nullptr, p.description};
                if (p.kind == sim::BtParamKind::Number) {
                    pi.defaultValue = p.defaultNumber;
                } else if (p.kind == sim::BtParamKind::Text) {
                    pi.defaultValue = p.defaultText;
                } else {
                    pi.defaultValue = Json::array({p.defaultVec.x, p.defaultVec.y, p.defaultVec.z});
                }
                out.push_back(std::move(pi));
            }
        }
    }
    return out;
}

std::string BtGraph::uniqueNodeId(const std::string& type) const {
    std::string base;
    for (char c : type) {
        base += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (int n = 1;; ++n) {
        const std::string candidate = std::format("{}{}", base, n);
        if (std::none_of(nodes.begin(), nodes.end(), [&](const BtGraphNode& g) { return g.nodeId == candidate; })) {
            return candidate;
        }
    }
}

int BtGraph::addNode(const std::string& type, float x, float y) {
    BtGraphNode n;
    n.id = nextId_++;
    n.nodeId = uniqueNodeId(type);
    n.type = type;
    n.x = x;
    n.y = y;
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

void BtGraph::removeNode(int id) {
    std::erase_if(links, [&](const BtGraphLink& l) { return l.parent == id || l.child == id; });
    std::erase_if(nodes, [&](const BtGraphNode& n) { return n.id == id; });
}

BtGraphNode* BtGraph::findNode(int id) {
    for (BtGraphNode& n : nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

const BtGraphNode* BtGraph::findNode(int id) const {
    return const_cast<BtGraph*>(this)->findNode(id);
}

int BtGraph::parentOf(int id) const {
    for (const BtGraphLink& l : links) {
        if (l.child == id) {
            return l.parent;
        }
    }
    return 0;
}

std::vector<int> BtGraph::children(int id) const {
    std::vector<int> out;
    for (const BtGraphLink& l : links) {
        if (l.parent == id) {
            out.push_back(l.child);
        }
    }
    std::sort(out.begin(), out.end(), [&](int a, int b) {
        const BtGraphNode* na = findNode(a);
        const BtGraphNode* nb = findNode(b);
        return std::tie(na->y, na->x, a) < std::tie(nb->y, nb->x, b);
    });
    return out;
}

std::vector<int> BtGraph::roots() const {
    std::vector<int> out;
    for (const BtGraphNode& n : nodes) {
        if (parentOf(n.id) == 0) {
            out.push_back(n.id);
        }
    }
    return out;
}

bool BtGraph::canLink(int parent, int child, std::span<const sim::CustomNodeSpec> custom, std::string* why) const {
    auto fail = [&](const char* message) {
        if (why != nullptr) {
            *why = message;
        }
        return false;
    };
    const BtGraphNode* p = findNode(parent);
    const BtGraphNode* c = findNode(child);
    if (p == nullptr || c == nullptr || parent == child) {
        return fail("invalid nodes");
    }
    const auto [minChildren, maxChildren] = childLimits(p->type, custom);
    (void)minChildren;
    if (maxChildren == 0) {
        return fail("this node takes no children");
    }
    if (static_cast<int>(children(parent).size()) >= maxChildren) {
        return fail("this node is full");
    }
    if (parentOf(child) != 0) {
        return fail("a node has only one parent");
    }
    for (int a = parent; a != 0; a = parentOf(a)) {
        if (a == child) {
            return fail("that would make a cycle");
        }
    }
    return true;
}

int BtGraph::link(int parent, int child, std::span<const sim::CustomNodeSpec> custom) {
    if (!canLink(parent, child, custom)) {
        return 0;
    }
    BtGraphLink l{nextId_++, parent, child};
    links.push_back(l);
    return l.id;
}

void BtGraph::unlink(int linkId) {
    std::erase_if(links, [&](const BtGraphLink& l) { return l.id == linkId; });
}

std::vector<BtGraphIssue> BtGraph::validate(std::span<const sim::CustomNodeSpec> custom) const {
    std::vector<BtGraphIssue> issues;
    const auto r = roots();
    if (nodes.empty()) {
        issues.push_back({0, "the tree is empty"});
    } else if (r.size() != 1) {
        issues.push_back({0, std::format("{} unconnected nodes: connect everything to one root", r.size())});
    }
    std::map<std::string, int> seenIds;
    for (const BtGraphNode& n : nodes) {
        const bool builtin = sim::findBtNodeType(n.type) != nullptr && n.type != "Custom";
        const bool script =
            std::any_of(custom.begin(), custom.end(), [&](const sim::CustomNodeSpec& s) { return s.name == n.type; });
        if (!builtin && !script) {
            issues.push_back({n.id, "unknown node type " + n.type});
            continue;
        }
        const auto [minC, maxC] = childLimits(n.type, custom);
        const auto count = static_cast<int>(children(n.id).size());
        if (count < minC) {
            issues.push_back({n.id, std::format("needs at least {} child{}", minC, minC == 1 ? "" : "ren")});
        }
        if (count > maxC) {
            issues.push_back({n.id, std::format("takes at most {} children", maxC)});
        }
        if (++seenIds[n.nodeId] > 1) {
            issues.push_back({n.id, "duplicate node id " + n.nodeId});
        }
        const auto params = paramsOf(n.type, custom);
        for (auto it = n.params.begin(); it != n.params.end(); ++it) {
            const auto p =
                std::find_if(params.begin(), params.end(), [&](const ParamInfo& pi) { return pi.name == it.key(); });
            if (p == params.end()) {
                issues.push_back({n.id, "unknown parameter " + it.key()});
                continue;
            }
            const Json& v = it.value();
            const bool ok = (p->kind == sim::BtParamKind::Number && v.is_number()) ||
                            (p->kind == sim::BtParamKind::Text && v.is_string()) ||
                            (p->kind == sim::BtParamKind::Vec3 && v.is_array() && v.size() == 3);
            if (!ok) {
                issues.push_back({n.id, std::format("{} must be a {}", it.key(), kindName(p->kind))});
            }
        }
    }
    return issues;
}

Json BtGraph::toJson() const {
    const auto r = roots();
    if (r.size() != 1) {
        throw std::invalid_argument(std::format("the graph needs exactly one root node (has {})", r.size()));
    }
    std::function<Json(int)> emit = [&](int id) {
        const BtGraphNode& n = *findNode(id);
        Json j{{"type", n.type}, {"id", n.nodeId}};
        if (!n.params.empty()) {
            j["params"] = n.params;
        }
        const auto kids = children(id);
        if (!kids.empty()) {
            Json c = Json::array();
            for (int k : kids) {
                c.push_back(emit(k));
            }
            j["children"] = c;
        }
        return j;
    };
    Json positions = Json::object();
    for (const BtGraphNode& n : nodes) {
        positions[n.nodeId] = Json::array({std::lround(n.x), std::lround(n.y)});
    }
    return Json{{"format", "crowdsim.bt"},
                {"version", 1},
                {"name", name},
                {"root", emit(r.front())},
                {"editor", Json{{"positions", positions}}}};
}

BtGraph BtGraph::fromJson(const Json& doc) {
    BtGraph g;
    g.name = doc.value("name", std::string("behaviour"));
    if (!doc.contains("root")) {
        throw std::invalid_argument("behaviour tree needs a \"root\" node");
    }
    const Json positions = doc.contains("editor") ? doc["editor"].value("positions", Json::object()) : Json::object();
    bool allPlaced = true;
    std::function<int(const Json&, int)> add = [&](const Json& node, int parent) -> int {
        BtGraphNode n;
        n.id = g.nextId_++;
        n.type = node.at("type").get<std::string>();
        n.nodeId = node.value("id", std::string());
        if (n.nodeId.empty()) {
            n.nodeId = g.uniqueNodeId(n.type);
        }
        if (node.contains("params")) {
            n.params = node["params"];
        }
        if (positions.contains(n.nodeId)) {
            n.x = positions[n.nodeId].at(0).get<float>();
            n.y = positions[n.nodeId].at(1).get<float>();
        } else {
            allPlaced = false;
        }
        g.nodes.push_back(n);
        const int id = n.id;
        if (parent != 0) {
            g.links.push_back({g.nextId_++, parent, id});
        }
        for (const Json& child : node.value("children", Json::array())) {
            add(child, id);
        }
        return id;
    };
    add(doc["root"], 0);
    if (!allPlaced) {
        g.autoLayout();
    }
    return g;
}

void BtGraph::autoLayout() {
    // Children in their current order (by y; ties fall back to creation order, which
    // for a freshly loaded document is the JSON order).
    std::map<int, std::vector<int>> kids;
    for (const BtGraphNode& n : nodes) {
        kids[n.id] = children(n.id);
    }
    float nextRow = 0.0f;
    std::function<float(int, int)> place = [&](int id, int depth) -> float {
        BtGraphNode* n = findNode(id);
        n->x = static_cast<float>(depth) * kColumn;
        const auto it = kids.find(id);
        if (it == kids.end() || it->second.empty()) {
            n->y = nextRow;
            nextRow += kRow;
            return n->y;
        }
        float first = 0.0f;
        float last = 0.0f;
        for (std::size_t k = 0; k < it->second.size(); ++k) {
            const float y = place(it->second[k], depth + 1);
            first = k == 0 ? y : first;
            last = y;
        }
        n->y = std::round((first + last) * 0.5f);
        return n->y;
    };
    for (int r : roots()) {
        place(r, 0);
        nextRow += kRow * 0.5f;
    }
}

}  // namespace crowdsim::assets
