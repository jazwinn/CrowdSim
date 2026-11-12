#pragma once

/// @file BtGraph.hpp
/// Editable graph form of a behaviour tree, used by the node graph editor
/// (and testable without any UI). Converts to and from *.bt.json; node
/// positions travel in the document's "editor" block, which the runtime ignores:
///
///   "editor": {"positions": {"<node id>": [x, y], ...}}
///
/// The tree grows left to right: a node's children are ordered by their y
/// position (top first). A document written by toJson() loads back with
/// fromJson() and writes out identical JSON.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/sim/BehaviorTree.hpp"

namespace crowdsim::assets {

using Json = nlohmann::json;

struct BtGraphNode {
    int id = 0;          ///< graph-local handle (stable while editing)
    std::string nodeId;  ///< the tree's node id ("id" in JSON)
    std::string type;
    Json params = Json::object();  ///< only the parameters that are set
    float x = 0.0f;
    float y = 0.0f;
};

struct BtGraphLink {
    int id = 0;
    int parent = 0;
    int child = 0;
};

struct BtGraphIssue {
    int node = 0;  ///< graph node id, 0 = the whole graph
    std::string message;
};

class BtGraph {
public:
    std::string name = "new_behaviour";
    std::vector<BtGraphNode> nodes;
    std::vector<BtGraphLink> links;

    /// Add a node of `type` at (x, y) with a fresh unique node id; returns its graph id.
    int addNode(const std::string& type, float x, float y);
    void removeNode(int id);
    [[nodiscard]] BtGraphNode* findNode(int id);
    [[nodiscard]] const BtGraphNode* findNode(int id) const;

    /// Whether `parent` may take `child` (child count limits, one parent, no cycles).
    [[nodiscard]] bool canLink(int parent, int child, std::span<const sim::CustomNodeSpec> custom,
                               std::string* why = nullptr) const;
    /// Link parent -> child; returns the link id or 0 if not allowed.
    int link(int parent, int child, std::span<const sim::CustomNodeSpec> custom);
    void unlink(int linkId);

    [[nodiscard]] int parentOf(int id) const;
    /// Children ordered by y (then x).
    [[nodiscard]] std::vector<int> children(int id) const;
    /// Nodes without a parent (a valid tree has exactly one: the root).
    [[nodiscard]] std::vector<int> roots() const;

    /// Structural and parameter problems, per node where possible.
    [[nodiscard]] std::vector<BtGraphIssue> validate(std::span<const sim::CustomNodeSpec> custom) const;

    /// *.bt.json document (with the "editor" block). Throws std::invalid_argument
    /// unless the graph has exactly one root.
    [[nodiscard]] Json toJson() const;
    /// Graph from a *.bt.json document; positions come from its "editor" block,
    /// or from autoLayout() when it has none.
    [[nodiscard]] static BtGraph fromJson(const Json& doc);

    /// Tidy left-to-right layout keeping each node's current child order.
    void autoLayout();

    /// Parameter schema of a node type (built-in or script node); empty if unknown.
    struct ParamInfo {
        std::string name;
        sim::BtParamKind kind = sim::BtParamKind::Number;
        Json defaultValue;
        std::string description;
    };
    [[nodiscard]] static std::vector<ParamInfo> paramsOf(const std::string& type,
                                                         std::span<const sim::CustomNodeSpec> custom);
    /// Child limits of a node type: {min, max}; {0, 0} for leaves and unknown types.
    [[nodiscard]] static std::pair<int, int> childLimits(const std::string& type,
                                                         std::span<const sim::CustomNodeSpec> custom);

private:
    [[nodiscard]] std::string uniqueNodeId(const std::string& type) const;
    int nextId_ = 1;
};

}  // namespace crowdsim::assets
