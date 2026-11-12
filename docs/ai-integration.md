# Driving CrowdSim from an AI agent (MCP)

CrowdSim embeds a **Model Context Protocol** server, so an AI client such as Claude Code can build
environments, author behaviours, run experiments headless and read back metrics. Every tool maps
onto a command (validated, logged and undoable like an editor action) or onto a read-only query.

## Connect

**Headless over stdio** (no window; the client starts the process). Copy
[`mcp.example.json`](mcp.example.json) to `.mcp.json` in your project (or merge it into your
client's MCP settings) and fix the path:

```json
{
  "mcpServers": {
    "crowdsim": {
      "command": "C:/path/to/CrowdSim/build/windows-msvc/bin/Release/crowdsim_headless.exe",
      "args": ["--mcp-stdio", "--log-level", "warn"]
    }
  }
}
```

**The editor over HTTP** (watch what the agent does, take screenshots):

```bash
build/windows-msvc/bin/Release/crowdsim --mcp-http 127.0.0.1:8765 --mcp-token change-me
```

```json
{
  "mcpServers": {
    "crowdsim-gui": {
      "type": "http",
      "url": "http://127.0.0.1:8765/mcp",
      "headers": { "Authorization": "Bearer change-me" }
    }
  }
}
```

`crowdsim_headless --mcp-http ADDR` serves HTTP without a window. The HTTP server binds to the
address given (use `127.0.0.1`; it is never exposed by default) and requires the token if
`--mcp-token` is set. Requests run on the engine thread between simulation steps; logs go to
stderr, so stdout carries only protocol messages in stdio mode.

## Tools

| Area | Tools |
|---|---|
| Scene | `scene_new`, `scene_load`, `scene_save`, `scene_describe`, `object_place`, `object_update`, `object_delete`, `gltf_import`, `navmesh_rebuild`, `link_create` |
| Crowd | `goal_create`, `goal_update`, `spawner_create`, `spawner_update`, `agent_type_define`, `agent_type_list`, `agents_spawn`, `agents_query` |
| Behaviour | `bt_list`, `bt_get`, `bt_set`, `bt_validate`, `bt_node_types` |
| Scripting | `script_list`, `script_get`, `script_write`, `script_reload`, `script_eval` |
| Simulation | `sim_play`, `sim_pause`, `sim_step`, `sim_run_for`, `sim_reset`, `sim_set_params` |
| Analysis | `metrics_get` (more in M9: heatmaps, recordings) |
| Logs | `log_tail`, `errors_get` |
| Vision (editor only) | `screenshot` (PNG; optional camera `{target:[x,z], distance, yaw, pitch}`) |

`tools/list` returns each tool's JSON Schema. Results are compact JSON: `agents_query` returns a
count plus an evenly spaced sample (≤ 500), `scene_describe` caps lists at 100, `log_tail` at 300
lines, and script output at 8 KB. Failures come back as tool errors (`isError: true`) with the
reason, e.g. a validation message naming the offending behaviour-tree node.

**Resources:** `crowdsim://scene` (current scene JSON), `crowdsim://bt/<name>`,
`crowdsim://agent-type/<name>`, `crowdsim://script/<name>` and `crowdsim://docs/<file>.md`
(file formats, the Lua API, this guide).

## Conventions

- Metres, +Y up. Areas (goals, spawners) are rectangles `{center:[x,z], halfExtents:[x,z], elevation}`.
- Objects: `{name, type: box|ramp|stairs|plane|mesh, position:[x,y,z], size:[x,y,z], rotationY, tags}`;
  `position` is the centre of the footprint at the object's lowest point. Floors are boxes tagged
  `walkable` (put their top at y = 0), walls and furniture `obstacle`.
- Spawners reference goals by id or name; `agentMix` picks agent types by weight.
- One simulated second is 30 steps. `sim_run_for` runs as fast as possible and returns metrics.

## Workflow: from an empty scene to metrics

This is what `tools/mcp_workflow_test.py` does (it runs in CTest as `mcp_stdio_workflow`):

1. `scene_new {name: "hall"}`
2. `object_place` a 60 × 30 m floor (`tags: ["walkable"]`) and two wall segments leaving a 6 m doorway.
3. `goal_create {goal: {name: "Exit", area: {center: [27, 0], halfExtents: [2, 12], elevation: 0}}}`
4. `bt_set` two trees, e.g. *rusher* (SetSpeed 1.3 → MoveToGoal → Despawn) and *dawdler*
   (Wander → Wait → MoveToGoal → Despawn). Use `bt_node_types` for node parameters and
   `bt_validate` to check a tree first.
5. `agent_type_define {type: {name: "rusher", behaviour: "rusher", color: "#e05040"}}` (and dawdler).
6. `spawner_create {spawner: {name: "Entrance", area: {...}, rate: 10, total: 300, goal: "Exit",
   agentMix: [{type: "rusher", weight: 1}, {type: "dawdler", weight: 1}]}}`
7. `sim_run_for {seconds: 60}` → `{steps: 1800, wallMs: ~400, metrics: {spawned: 300, arrived: …,
   goals: [{name: "Exit", arrivals: …}], aliveByType: {…}, aliveByState: {…}}}`
8. Iterate: widen the doorway (`object_update`), `sim_reset`, run again and compare arrivals; or
   save the scene (`scene_save`) and the trees (they are in the scene file).

Custom behaviour beyond the built-in nodes: `script_write` a Lua file that registers a node with
`bt.register_node{...}` (see `crowdsim://docs/scripting.md`), then use its name in `bt_set`.

## Reproducibility

Everything an agent does is a command, so it lands in the command log. Save the log from the editor
(*File → Save command log*) or with `crowdsim_headless --save-log`, and replay it with
`crowdsim_headless --replay run.cmdlog.json` to get the identical state hash.
