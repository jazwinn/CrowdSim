# File formats and commands

Everything CrowdSim reads or writes is plain JSON so humans, scripts and AI agents can author,
diff and review it. All paths are relative to the **content root** (`content/` by default,
`--content DIR` to change).

## Scene files — `*.scene.json` (version 1)

```json
{
  "format": "crowdsim.scene",
  "version": 1,
  "name": "corridor_bottleneck",
  "navigation": {"cellSize": 0.2, "cellHeight": 0.1, "agentHeight": 1.9, "agentRadius": 0.3,
                 "agentMaxClimb": 0.45, "agentMaxSlope": 45, "tileSize": 64},
  "nextId": 8,
  "objects": [
    {"id": 1, "name": "Floor", "type": "box", "position": [0, -0.2, 0], "rotationY": 0,
     "size": [80, 0.2, 30], "color": "#5c626a", "tags": ["walkable"]},
    {"id": 2, "name": "Stairs", "type": "stairs", "position": [0, 0, -4], "rotationY": 0,
     "size": [8, 4, 8], "steps": 20, "color": "#968a7d", "tags": ["walkable"]},
    {"id": 3, "name": "Hall", "type": "mesh", "asset": "models/hall.gltf", "position": [0, 0, 0],
     "rotationY": 90, "scale": 1, "tags": []}
  ],
  "links":    [{"id": 4, "start": [-27, 0, -1.5], "end": [-27, 4, 1.5], "radius": 0.6,
                "bidirectional": false, "extraCost": 25}],
  "goals":    [{"id": 5, "name": "Exit", "area": {"center": [36, 0], "halfExtents": [3, 14], "elevation": 0}}],
  "spawners": [{"id": 6, "name": "Entrance", "area": {"center": [-34, 0], "halfExtents": [4, 13], "elevation": 0},
                "rate": 40, "total": 1500, "startTime": 0, "stopTime": null, "goal": 5,
                "agent": {"radius": [0.24, 0.3], "preferredSpeed": [1.15, 1.55], "maxSpeedFactor": 1.35,
                          "maxAcceleration": 3, "color": "#d98c40", "colorJitter": 0.12, "agentType": 0}}]
}
```

- **Coordinates**: metres, +Y up, left-handed (+X right, +Z forward). Areas are rectangles on a
  floor: `center` is `[x, z]`, `elevation` is the floor's Y.
- **Objects**: `type` is `box`, `ramp` (rises towards local +Z), `stairs` (`steps` treads towards
  local +Z), `plane`, or `mesh` (glTF `asset`, uniform `scale`). `position` is the centre of the
  footprint at the object's lowest point; `rotationY` is in degrees (+Z turns towards +X).
- **Tags**: `walkable` (may become navmesh if flat enough), `obstacle` (never walkable, blocks).
  Other tags (`area:<name>`, `door`, ...) are preserved for scripts and metrics.
- **Escalators**: a `stairs` or `ramp` object tagged `escalator` moves its riders from the low
  edge to the high edge (`escalator:down` reverses it); `escalator:speed=<m/s>` is the step speed
  along the slope (default 0.5). Two lanes fit when the object is at least 2.4 m wide (the navmesh
  loses about 0.5 m per side). Everything about how people ride is behaviour, set by tree nodes:
  `SetEscalatorStyle` (side, walking pace, clearing the landing), `SetEscalatorBoarding` (gap,
  squeezing on) and the general `SetQueueManners`. See `scenes/escalator_etiquette.scene.json`
  and `behaviours/escalator_*.bt.json`.
- **Bodies** (optional `"bodies": {...}`; off when absent): physical contact in packed crowds.
  Below contact density agents avoid each other as usual. When bodies touch (a crowd packed past
  what avoidance can keep apart, or people held up for a few seconds pressing into the gaps),
  they are moved by forces instead: bodies are springs down to an incompressible core, each person
  presses forward with their push (`SetPushing`), packed people on a slope lean downhill, and walls
  push back. Forces add up along chains of people, so the front of a packed crowd carries everyone
  pushing behind it. Pressure (N/m) is the stress in each body from its contacts. Large net pushes
  topple people; fallen people lie still, others pushed against them trip and fall on top (pile
  layers), and they get up once the pressure is gone. A present section is enabled unless it says
  `"enabled": false`; every field is optional:

  | Field | Default | Meaning |
  |---|---|---|
  | `stiffness` | 5000 | N per metre two bodies are pressed into each other |
  | `damping` | 400 | N per m/s of approach along the contact normal |
  | `core` | 0.65 | incompressible fraction of the radius (limits density to ~11-14 /m2 for 0.2-0.25 m radii) |
  | `slopeLean` | 0.5 | share of the downhill weight (m g sin a) packed people pass on to those below |
  | `fallForce` | 800 | N of net push above which standing people may topple |
  | `fallRate` | 0.3 | falls per second at twice `fallForce` (rises linearly above it) |
  | `tripForce` | 300 | N of push towards someone lying in the way that trips people over them |
  | `tripRate` | 0.6 | falls per second when tripping (x push / `tripForce`, up to 3x) |
  | `getUpSeconds` | 3 | minimum time on the ground |
  | `getUpPressure` | 300 | N/m; fallen people get up only below this and with no one on top |

  Metrics then report pressure (peak, p90, people above the critical 1000 N/m and how many spent
  30 s or more above it) and falls; `GetHeatmap {kind: "pressure"}` maps the peak pressure. See
  `scenes/itaewon_2022*.scene.json`.
- **Ids** are stable and never reused (`nextId` keeps deleted ids retired). Spawners reference
  goals by id; commands also accept the goal's **name** (`"goal": "Exit"`).
- Files are written in entity creation order with floats rounded to 6 significant digits, so
  save → load → save is byte-identical.
- Newer versions are rejected; older ones go through `assets::migrateScene`.

## glTF import conventions

`ImportGltf` (or a `mesh` object) loads glTF 2.0 (`.gltf`/`.glb`). glTF's right-handed axes are
converted (Z flipped). Surface tags come from nodes and are inherited by children:

| Node | Result |
|---|---|
| name contains `obstacle`, `wall`, `pillar`, `column`, `blocker` | obstacle |
| name contains `nowalk`, `ceiling`, `roof` | not walkable |
| name contains `floor`, `stairs`, `ramp`, `walkable` | walkable |
| `extras: {"crowdsim": {"tags": ["obstacle" \| "walkable" \| "nowalk", ...]}}` | overrides the name |
| anything else | walkable (Recast's slope limit still applies) |

`tools/make_test_gltf.py` generates `content/models/test_room.gltf`, which exercises all three.

## Behaviour trees — `*.bt.json` (version 1)

Files in `content/behaviours/` are loaded at startup and **hot reloaded** when they change (the
GUI polls twice a second; each reload is a logged `SetBehaviorTree` command). Agents running a
reloaded tree restart from its root. Scene files embed the trees they use under `"behaviours"`.

```json
{
  "format": "crowdsim.bt", "version": 1, "name": "wanderer",
  "root": {"type": "Sequence", "id": "root", "children": [
    {"type": "SetState", "params": {"name": "wandering"}},
    {"type": "Repeat", "params": {"count": 3}, "children": [
      {"type": "Sequence", "children": [
        {"type": "Wander", "params": {"radius": 14}},
        {"type": "Wait", "params": {"seconds": 2, "jitter": 3}}]}]},
    {"type": "MoveToGoal"},
    {"type": "Despawn"}]}
}
```

- Every node has a `type`, an optional `id` (shown in the debugger and node editor) and optional
  `params` (number, string or `[x, y, z]`). The node editor stores its layout in
  `"editor": {"positions": {"<node id>": [x, y]}}`, which the runtime ignores; children are
  ordered top to bottom by their position. Files the editor writes load back and save identically.
- Limits: 64 nodes, 24 composite/decorator memory slots and 8 timers per tree. Malformed trees are
  rejected with the offending node's path (`root/Repeat[1]/...`); `ValidateBehaviorTree` checks a
  tree without applying it.
- Trees tick at `decisionHz` (10 Hz by default, staggered across agents). Statuses are
  success / failure / running; composites resume their running child.
- `goal` parameters name a goal; empty means the agent's spawner goal.

| Node | Kind | Children | Params (default) | Behaviour |
|---|---|---|---|---|
| `Sequence` | composite | 1–32 | – | Run children in order; fail on the first failure, succeed when all succeed. |
| `Selector` | composite | 1–32 | – | Run children in order until one succeeds; fail if all fail. |
| `Parallel` | composite | 1–32 | `successThreshold` (1) | Tick all children; succeed once enough succeed, fail when that becomes impossible. |
| `RandomSelector` | composite | 1–32 | – | Pick one child uniformly at random and run it to completion. |
| `WeightedRandom` | composite | 1–32 | `weights` ("1,2,...") | Pick one child by weight and run it to completion. |
| `Inverter` | decorator | 1 | – | Swap success and failure. |
| `Succeeder` | decorator | 1 | – | Succeed once the child finishes. |
| `Repeat` | decorator | 1 | `count` (0 = forever) | Re-run the child while it succeeds; fail when it fails. |
| `Cooldown` | decorator | 1 | `seconds` (5) | After the child finishes, fail for `seconds`. |
| `Timeout` | decorator | 1 | `seconds` (10) | Fail if the child is still running after `seconds`. |
| `MoveToGoal` | action | 0 | `goal` | Walk to a goal along its flow field; succeed inside the goal area. |
| `MoveToPoint` | action | 0 | `point`, `tolerance` (0.5) | Walk to a point along a navmesh path; fail if unreachable. |
| `Wander` | action | 0 | `radius` (8) | Walk to a random reachable point within `radius`. |
| `Wait` | action | 0 | `seconds` (2), `jitter` (0) | Stand still for `seconds` + random jitter. |
| `Queue` | action | 0 | `goal` | Join the goal's line, wait for a free slot (`capacity`), get served for `serviceTime`. |
| `FollowGroup` | action | 0 | `distance` (1.2) | Follow the group leader (keeps running); fails for leaders and solo agents. |
| `FleeFrom` | action | 0 | `tag` ("danger"), `radius` (10) | Run from the nearest object with `tag` within `radius`; fail when none. |
| `AvoidArea` | action | 0 | `tag` | Steer around objects with `tag` from now on. |
| `SetSpeed` | action | 0 | `multiplier` (1) | Scale walking speed. |
| `SetState` | action | 0 | `name` ("walking") | Set the state label (debugger, metrics). |
| `SetQueueManners` | action | 0 | `gap` (0.6), `ease` (0.5) | From now on, slow to a stop `gap` m behind whoever is ahead in your line instead of pressing into them (0 = off, the default). |
| `SetPushing` | action | 0 | `force` (100) | From now on, press this hard (N) towards where you are going once bodies touch (scene bodies). Agents that never run it push with 100 N; 0 = only shuffle along. |
| `SetEscalatorStyle` | action | 0 | `side` ("either"), `walk` (0), `clear` (2) | How to ride escalators from now on: `side` `left`, `right`, `either` (your side; near the middle the shorter line) or `shorter`; `walk` = own pace on the steps × preferred speed (0 = stand); walk `clear` m straight off before turning. Without it: either side, pace 0.45. |
| `SetEscalatorBoarding` | action | 0 | `gap` (0.9), `squeeze` (1) | How to step onto escalators: standers wait until the person ahead in their lane is `gap` m away; `squeeze` 1 = when pushed to the front on the wrong side, step on (or switch lanes) anyway. |
| `Despawn` | action | 0 | – | Remove the agent. |
| `IsNearGoal` | condition | 0 | `distance` (3), `goal` | Within `distance` of the goal area. |
| `IsCrowded` | condition | 0 | `threshold` (6), `radius` (2) | At least `threshold` agents within `radius`. |
| `IsBlocked` | condition | 0 | `seconds` (5) | The agent has wanted to move but moved at under a quarter of that pace for `seconds` (held up in a crowd, or lying on the ground). |
| `IsAreaTagged` | condition | 0 | `tag` | Standing inside the footprint of an object with `tag`. |
| `RandomChance` | condition | 0 | `probability` (0.5) | Succeed with the given probability. |

`ListBehaviorNodeTypes` returns the same list with JSON parameter schemas.

## Agent types — `*.agent.json` (version 1)

Files in `content/agents/` (loaded and hot reloaded like trees, as `DefineAgentType`) bind a
behaviour to locomotion parameters. Scenes embed the types they use under `"agentTypes"`.

```json
{"format": "crowdsim.agent", "version": 1, "name": "queuer", "behaviour": "queuer",
 "radius": [0.24, 0.3], "preferredSpeed": [1.0, 1.4], "maxSpeedFactor": 1.35, "maxAcceleration": 3, "mass": [60, 90],
 "personalSpace": 0.08, "height": 1.75, "color": "#5cbf6a", "colorJitter": 0.1, "tags": []}
```

Ranges (`[min, max]`) are sampled per agent from its own random stream. `personalSpace` (m) is
added to the radius for avoidance. `mass` (kg, default `[75, 75]`) sets who gives way: two
agents of equal mass share avoidance half each; otherwise the lighter takes `1 - r/2` of it and
the heavier `r/2` (`r` = lighter / heavier), so a heavier agent pushes a lighter one aside
while a lighter one goes round a heavier one. A single value draws no random number, so
existing scenes are unchanged. Spawners' inline `agent` parameters take `mass` too. Spawners choose types with
`"agentMix": [{"type": "commuter", "weight": 0.55}, ...]` and spawn groups with
`"groupSize": [1, 3]` (the first agent leads; the others' `FollowGroup` follows it). Goals used as
service points add `"capacity"`, `"serviceTime"` (s), `"queueDirection"` (`[x, z]`, the way the
line extends) and `"queueSpacing"` (m).

## Commands

Every state change is a command: `{"cmd": "<Name>", "params": {...}}`. GUI actions, `--commands`
batches, Lua (M6) and MCP tools (M8) all use them. `crowdsim_headless --list-commands` prints
every command with its JSON Schema.

| Command | Logged | Description |
|---|---|---|
| `CreateScene` | yes | Clear everything and start an empty scene. |
| `RestoreScene` | yes | Replace the scene with an inline scene document (scene.json content). |
| `LoadScene` | yes (as `RestoreScene`) | Load a *.scene.json file. |
| `SaveScene` | no | Save the scene to a *.scene.json file. |
| `LoadBuiltinScenario` | yes (as `RestoreScene`) | stream, counterflow, benchmark, building, plaza. |
| `PlaceObject` | yes | Add a static object (primitive or imported mesh). Returns its id. |
| `ImportGltf` | yes (as `PlaceObject`) | Import a glTF file as a mesh object; reports per-node tags. |
| `CreateSpawner` / `CreateGoal` / `CreateOffMeshLink` | yes | Add an entity. Returns its id. |
| `AddEntity` | yes | Add any entity from its full JSON form (used by undo of deletes). |
| `UpdateEntity` | yes | Merge-patch any entity: `{"id": 5, "patch": {"rate": 30}}`. |
| `TransformObject` | yes | Move / rotate / resize an object. |
| `SetSpawnerConfig` / `SetGoalConfig` | yes | Merge-patch a spawner / goal. |
| `DeleteEntity` | yes | Delete any entity by id. |
| `SetBehaviorTree` | yes | Add or replace (hot reload) a behaviour tree from `*.bt.json` content. |
| `ValidateBehaviorTree` | no | Check a tree; returns `valid`, node / slot counts or the error. |
| `ListBehaviorNodeTypes` | no | Node types with parameter schemas. |
| `GetBehaviorTree` | no | A tree as a document: the file (with editor layout) when it matches the tree in use, else the live tree. |
| `SaveBehaviorTree` | no | Validate and write `content/behaviours/<name>.bt.json`. |
| `DefineAgentType` | yes | Add or replace an agent type from `*.agent.json` content. |
| `ListAgentTypes` / `ListBehaviorTrees` | no | Agent types with parameters; trees with node and agent counts. |
| `QueryAgents` | no | Count agents by type / state / behaviour / area and list an evenly spaced sample. |
| `SpawnAgents` | yes | Spawn N agents in an area now (a one-shot spawner). |
| `SetAgentTypeBehaviour` | yes | Switch an agent type to another tree (its agents restart). |
| `LoadScript` / `ReloadScript` | yes (with source) | Run or hot-reload a Lua script (see `docs/scripting.md`). |
| `SaveScript` | yes (as `LoadScript`) | Write `content/scripts/<name>.lua` and run it (the Script tab's Save). |
| `EvalLua` | yes | Evaluate Lua; returns printed output and the result. |
| `SpawnEntity` | yes | Place an instance of a Lua-registered entity type. |
| `ListScripts` / `GetScript` / `ScriptErrors` / `ListEntityTypes` | no | Scripting queries. |
| `AddGate` / `RemoveGate` | yes | Counting gate `{name, a:[x,z], b:[x,z], elevation}`: directional crossings and flow. |
| `ListGates` / `GetMetrics` / `GetHeatmap` | no | Gate counts; metrics summary (with scene bodies also `pressure` and `falls`); density, speed or peak-pressure heatmap (downsampled). |
| `StartRecording` / `StopRecording` | yes | Trajectory recording at `hz` samples/s. |
| `ExportRecording` / `ExportMetrics` | no | CSV of trajectories / of the metrics time series (`recordings/` by default). |
| `ResetMetrics` | yes | Clear accumulated metrics (gates kept). |
| `RebuildNavmesh` | yes | Rebuild navmesh + flow fields now; returns polygon/tile/wall counts. |
| `SetNavigationSettings` | yes | Change navmesh build settings. |
| `SetSimParams` | yes | Locomotion parameters and/or seed. |
| `ResetSimulation` | yes | Remove all agents, back to t = 0. |
| `Undo` / `Redo` | (the applied inverse is logged) | Undo / redo the last undoable command. |
| `Play` / `Pause` / `SetSimSpeed` | no | Real-time playback (GUI). |
| `Step` / `RunFor` | no | Advance N steps / N simulated seconds. |

Errors (unknown command, bad parameters, unknown ids) are reported in the result and leave the
state unchanged. Every scene-changing command records an inverse for undo; consecutive commands
with the same `"merge"` key (gizmo drags) collapse into one undo step.

## Command batches — `*.commands.json`

A JSON array of commands, executed in order by `crowdsim_headless --commands FILE`; each result is
printed as one JSON line. `Step`/`RunFor` advance the simulation at that point in the batch. See
`content/commands/build_corridor.commands.json`.

## Command logs — `*.cmdlog.json`

```json
{"format": "crowdsim.cmdlog", "version": 1, "seed": 1, "stepHz": 30, "endStep": 3600,
 "entries": [{"step": 0, "cmd": {"cmd": "RestoreScene", "params": {"scene": {...}}}},
             {"step": 0, "cmd": {"cmd": "PlaceObject", "params": {"object": {"id": 1, ...}}}},
             {"step": 1800, "cmd": {"cmd": "UpdateEntity", "params": {...}}}]}
```

A log starts with the full scene at step 0 and records every applied state-changing command with
the step index at which it ran (in canonical, self-contained form: loads are recorded as their
content, created entities carry their assigned ids). `crowdsim_headless --replay FILE` reproduces
the run exactly (same state hash). The GUI saves logs via *File → Save command log* into
`content/logs/`.
