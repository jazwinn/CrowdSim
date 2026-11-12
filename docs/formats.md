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
| `LoadBuiltinScenario` | yes (as `RestoreScene`) | stream, counterflow, benchmark, building. |
| `PlaceObject` | yes | Add a static object (primitive or imported mesh). Returns its id. |
| `ImportGltf` | yes (as `PlaceObject`) | Import a glTF file as a mesh object; reports per-node tags. |
| `CreateSpawner` / `CreateGoal` / `CreateOffMeshLink` | yes | Add an entity. Returns its id. |
| `AddEntity` | yes | Add any entity from its full JSON form (used by undo of deletes). |
| `UpdateEntity` | yes | Merge-patch any entity: `{"id": 5, "patch": {"rate": 30}}`. |
| `TransformObject` | yes | Move / rotate / resize an object. |
| `SetSpawnerConfig` / `SetGoalConfig` | yes | Merge-patch a spawner / goal. |
| `DeleteEntity` | yes | Delete any entity by id. |
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
