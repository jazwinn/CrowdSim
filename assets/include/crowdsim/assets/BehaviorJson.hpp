#pragma once

/// @file BehaviorJson.hpp
/// *.bt.json (behaviour trees) and *.agent.json (agent types).
///
/// Behaviour tree (version 1):
///   {
///     "format": "crowdsim.bt", "version": 1, "name": "wanderer",
///     "root": {"type": "Sequence", "id": "root", "children": [
///         {"type": "Repeat", "id": "loop", "params": {"count": 3}, "children": [
///             {"type": "Sequence", "children": [
///                 {"type": "Wander", "params": {"radius": 12}},
///                 {"type": "Wait", "params": {"seconds": 2, "jitter": 2}}]}]},
///         {"type": "MoveToGoal"},
///         {"type": "Despawn"}]},
///     "editor": {...}   // node editor layout; preserved, ignored by the runtime
///   }
///
/// Agent type (version 1):
///   {"format": "crowdsim.agent", "version": 1, "name": "wanderer", "behaviour": "wanderer",
///    "radius": [0.24, 0.3], "preferredSpeed": [0.9, 1.3], "maxSpeedFactor": 1.3,
///    "maxAcceleration": 2.5, "personalSpace": 0.1, "height": 1.75,
///    "color": "#4aa3df", "colorJitter": 0.1, "tags": []}

#include <nlohmann/json.hpp>

#include "crowdsim/sim/BehaviorTree.hpp"
#include "crowdsim/sim/Environment.hpp"

namespace crowdsim::assets {

using Json = nlohmann::json;

/// Node JSON -> description. Throws std::invalid_argument on malformed input.
[[nodiscard]] sim::BtNodeDesc btNodeFromJson(const Json& node, const std::string& path = "root");
[[nodiscard]] Json btNodeToJson(const sim::BtNodeDesc& node);

/// Whole *.bt.json document -> compiled tree (validates format, version, name and nodes).
[[nodiscard]] sim::CompiledTree compileBehaviorJson(const Json& doc,
                                                    const std::map<std::string, int>& customNodes = {});
/// Compiled tree -> *.bt.json document (decompiled; editor layout not included).
[[nodiscard]] Json behaviorToJson(const sim::CompiledTree& tree);

[[nodiscard]] sim::AgentType agentTypeFromJson(const Json& doc);
[[nodiscard]] Json toJson(const sim::AgentType& type);

/// Built-in node types with parameter schemas (bt_node_types).
[[nodiscard]] Json btNodeTypesJson();

}  // namespace crowdsim::assets
