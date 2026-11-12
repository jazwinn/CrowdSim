#pragma once

/// @file McpInstructions.hpp
/// The "instructions" an MCP client receives at initialize: how to drive CrowdSim.

namespace crowdsim::app {

inline constexpr const char* kMcpInstructions =
    "CrowdSim is a real-time 3D crowd simulator. Units are metres, +Y is up; areas are rectangles "
    "{center:[x,z], halfExtents:[x,z], elevation}. Typical workflow: scene_new -> object_place floor slabs "
    "(tags ['walkable']) and walls/obstacles (tags ['obstacle']) -> goal_create -> bt_set behaviour trees "
    "(see bt_node_types) -> agent_type_define (behaviour = tree name) -> spawner_create (goal by name, "
    "agentMix [{type, weight}]) -> sim_run_for seconds (returns metrics) -> agents_query / metrics_get -> "
    "iterate. Every change is a command: undoable in the editor and recorded in the replayable command "
    "log. Lua (script_write / script_eval) can add BT nodes and entity types. Read crowdsim://docs/formats.md "
    "for file formats and crowdsim://docs/ai-integration.md for worked examples.";

}  // namespace crowdsim::app
