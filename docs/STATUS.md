# CrowdSim status

Last updated: 2026-11-12

| Milestone | State |
|---|---|
| M0 — Skeleton | done (Windows verified; macOS CI passed once) |
| M1 — Simulation core | done (Windows) |
| M2 — 3D navigation | done (Windows) |
| M3 — Command layer & scene format | done (Windows) |
| M4 — Behaviour trees | done (Windows) |
| M5 — Animated characters | done (Windows) |
| M6 — Lua scripting | done (Windows) |
| M7 — Node graph editor | done (Windows) |
| M8 — MCP server | **done** (Windows) |
| M9 — Metrics & analysis | next |

CI currently builds and tests **Windows only**. Linux and macOS are deferred until the application
is feature-complete (the matrix entries are commented in `.github/workflows/ci.yml`).

## M8 — MCP server

### What works

- **MCP server** (`mcp/`): JSON-RPC 2.0 core (`initialize` with version negotiation, `ping`,
  `tools/list`, `tools/call`, `resources/list`, `resources/read`). It has a **stdio** transport
  (`crowdsim_headless --mcp-stdio`) and a **Streamable HTTP** transport (`--mcp-http
  127.0.0.1:PORT`, GUI or headless, optional `--mcp-token`). Requests run on the engine thread
  between steps or frames. ADR 0011.
- **37 tools**, mapped onto commands: scene (new/load/save/describe, object place/update/delete,
  glTF import, navmesh rebuild, links), crowd (goals, spawners, agent types, burst spawn,
  filtered + sampled agent queries), behaviour (list/get/set/validate/node types), scripting
  (list/get/write/reload/eval), simulation (play/pause/step/run_for/reset/params), metrics, logs,
  errors, and `screenshot` in the editor (with an optional camera).
- **Resources**: current scene, behaviour trees, agent types, Lua scripts, docs.
- New commands: `QueryAgents`, `ListAgentTypes`, `ListBehaviorTrees`, `SpawnAgents`.
- `docs/ai-integration.md` (connecting Claude Code, conventions, worked workflow),
  `docs/mcp.example.json`.

### Done-criterion evidence

- CTest `mcp_stdio_workflow` (`tools/mcp_workflow_test.py`) is an MCP client over stdio. From
  an empty scene it builds a hall with a doorway and defines two agent types with their own
  trees (rusher, dawdler). It then runs `sim_run_for 60` headless: 1,800 steps in ~0.4 s wall,
  all 300 agents spawned, 193 arrived, and alive agents split between `rushing` and `dawdling`.
  It also reads metrics and the scene resource back.
- Editor over HTTP: `initialize`, bearer token enforced (401 without it), `scene_describe` of
  the live plaza, and `screenshot` with a camera returned a 1600×900 PNG.

### Tests

New doctest cases: initialize / notifications / errors; every tool has a description and a
well-formed schema covering the spec's minimum tool list; tool failures are tool errors;
commands run through tools (place, run, query, eval); resources list/read (path traversal
rejected); base64; cross-thread request queue. 66 doctest cases and 9 CTest entries pass.

### Known issues

- `heatmap_get` and recording tools come with M9 (metrics).
- Screenshots are whole editor frames as uncompressed PNG (~5 MB at 1600×900).
- HTTP responses are plain JSON; no SSE stream or server notifications.

## M7 — Node graph editor

### What works

- **Behaviour panel** (imgui-node-editor): open any tree (or the selected agent's), create nodes
  from a categorised right-click menu (built-in and Lua script nodes, with descriptions), link
  parent → child by dragging, delete links and nodes, reorder children by dragging them up or
  down, and edit parameters inline (set values bright, defaults dimmed).
- **Validation inline**: graph rules (child counts, single parent, no cycles, one root, unknown or
  mistyped parameters, duplicate ids) are shown on the offending nodes, and the real compiler runs
  every frame; Apply and Save are disabled while anything is wrong.
- **Apply** = `SetBehaviorTree` (hot reload into running agents); **Save** = `SaveBehaviorTree`
  (writes `content/behaviours/<name>.bt.json` with the layout); **Layout** tidies the graph.
- **Live highlighting**: the selected agent's running nodes and links glow green, last-visited
  nodes yellow.
- `assets::BtGraph`: the editor's model (rules, validation, JSON conversion, auto layout), with
  no UI dependency. Layout is stored in the document's `editor.positions` block. ADR 0010.
- Commands `GetBehaviorTree` (file with layout when it matches the tree in use, else live) and
  `SaveBehaviorTree`; GUI flag `--edit-bt NAME`.

### Done-criterion evidence

- doctest `M7: a tree built in the editor drives agents and round-trips to identical JSON`. The
  tree is built through the same graph operations the panel performs (add, link, edit
  parameters). Graph → JSON → graph → JSON is byte-identical. Applying it with SetBehaviorTree
  and switching a running spawner to it gives agents the editor-set `edited` state, and more than
  50 finish the tree and despawn. SaveBehaviorTree → GetBehaviorTree returns the identical document
  from the file.
- Screenshots (not committed): `screenshots/m7_editor.png` (wanderer tree), `m7_live.png` (live
  highlighting of a selected commuter).

### Tests

New: linking rules and per-node validation; child order follows y; files without layout load in
JSON order; the M7 criterion. 61 doctest cases and 8 CTest entries pass.

### Known issues

- The panel docks with the log by default and is small there; `--edit-bt` (or dragging it out)
  gives it room.
- No undo inside the graph editor itself; Apply is undoable (it is a command).

## M6 — Lua scripting

### What works

- **Script module** (`script/`): one sandboxed LuaJIT state (sol2) owned by the engine. No
  `os/io/debug/ffi/jit/require/dofile/loadfile/string.dump`, text-only `load`. Every call has an
  instruction budget (20 M), a 0.5 s budget and a 256 MB allocator cap; errors are logged, kept
  for the UI / `ScriptErrors`, and never stop the simulation. ADR 0009; API in
  `docs/scripting.md`.
- **API**: `scene.*` (through commands), `entities.register_type/spawn/list`,
  `agents.register_type/count/count_near/get`, `bt.register_node/set/node_types`, `sim.*`,
  `metrics.get`, `events.on` (agent spawned / arrived / despawned, step), `log.*`, a
  world-seeded `math.random`.
- **Script BT nodes**: typed parameters validated at tree compile time; ticked after the parallel
  decision phase, serially, batched per node instance, with a `ctx` for reading and steering
  agents; results arrive on the next decision tick. Deterministic for any thread count.
- **Scripted entity types**: object templates with `on_spawn` / `on_tick` (own rate). They are
  placed with `SpawnEntity` (undoable) or from the editor's *Add… → Scripted (Lua)* menu.
- **Hot reload**: `content/scripts/*.lua` load at start-up (before trees) and re-run when saved;
  nodes, types and event handlers are replaced by name or owner.
- **Scenario scripts**: `--script scenarios/kiosk_square.lua` (GUI and headless).
- **Commands**: `LoadScript`, `ReloadScript`, `EvalLua`, `SpawnEntity`, `ListScripts`,
  `GetScript`, `ScriptErrors`, `ListEntityTypes`. Command logs start with the loaded scripts'
  sources, so they replay in a fresh process.
- **Editor**: Script console (Ctrl+Enter runs through `EvalLua`; output and errors shown inline).
- **Content**: `scripts/info_kiosk.lua` (entity type + `VisitAttraction` node),
  `behaviours/tourist.bt.json`, `agents/tourist.agent.json`, `scenarios/kiosk_square.lua`.

### Done-criterion evidence

- doctest `M6: a BT node and an object type defined in Lua at runtime drive a running
  simulation`: with no content loaded, the stream scenario is already running. A `LoadScript`
  then defines the `Linger` node and the `beacon` entity type. Lua then places a beacon, defines
  a tree and an agent type that use the node, and switches the running spawner to them. New
  agents enter the Lua-set `lingering` state, more than 20 complete the node, and the beacon's
  `on_tick` runs, all with no script errors.
- Kiosk scenario (headless, 120 s): 71 completed kiosk visits, 4,119 browsing-seconds, no
  errors; the saved command log replays in a fresh process to the identical state hash.

### Tests

New doctest cases: sandbox (missing libraries, bytecode rejected, text `load`, seeded
`math.random`); limits (runaway loop, 1 GB allocation, `error()`, unknown command, all recorded,
host still usable); the M6 criterion; hot reload changes running agents' node behaviour; Lua
nodes are identical at 1 and 7 threads and a replayed log matches. All 58 doctest cases and 8
CTest entries pass.

Screenshot (not committed): `screenshots/m6_kiosks.png` — tourists gathering at scripted kiosks,
Script console docked.

### Known issues

- Script nodes report their result one decision tick (100 ms) late, by design.
- Lua globals are shared between scripts; name clashes are possible.
- Entity `on_tick` callbacks that call `scene.update` on an object trigger a navmesh rebuild;
  keep per-tick work to queries and Lua state.

## M5 — Animated characters

### What works

- **Procedural humanoid** (`animation/`): 18-joint skeleton, near/mid skinned meshes (2,412 / 556
  triangles), idle / walk / run / turn-in-place clips from two-bone IK on authored foot paths,
  built and sampled with **ozz-animation** (FetchContent, pinned 0.16.0) and baked into a
  **bone-matrix VAT** (54 × 128 RGBA32F) in ~1 ms at start-up. ADR 0008.
- **Gait controller**: walk/run phase advanced by distance ÷ cycle distance (no foot sliding on
  straight walks), walk↔run cross-fade 2.0–2.7 m/s, idle below 0.45 m/s, turn in place when
  stopped and turning; smoothed blend weights; per-agent desynchronised idles.
- **Instanced skinned rendering**: one draw per LOD batch; per-agent shirt colour from the agent
  type, hashed trousers / skin / hair palettes, uniform size variation from the agent radius.
- **LOD**: near VAT (< 30 m), mid VAT (< 90 m), capsule beyond; configurable in *Rendering* and
  with `--lod NEAR,FAR`.
- **Frustum culling** of camera and shadow casters, parallel on the job system.
- **Shadows**: 2048² directional shadow map with PCF on ground, scene meshes and characters,
  texel-snapped around the camera target; toggle in *Rendering* or `--overlays noshadows`
  (`capsules` turns characters off).
- Stats panel: near / mid / far / culled counts, shadow casters, crowd prepare time.
- Fix: spawner/goal decals are depth tested (they no longer draw over walls in front of them).

### Measured (Windows, i7-13700H, laptop RTX GPU, D3D11, 1600×900, no vsync)

| Measurement | Result | Budget |
|---|---|---|
| 5,000 agents, **all 4,744 visible as full-detail characters**, shadows | avg 6.34 ms, p99 8.33 ms (~158 FPS) | ≤ 16.6 ms |
| 5,000 agents, all visible at mid LOD, shadows | avg 2.62 ms, p99 3.93 ms | |
| 5,000 agents, default LOD, typical view | avg 1.93 ms, p99 5.47 ms | |
| Crowd prepare (cull + LOD + gait), 5k visible | ~1.0 ms CPU | |
| Character bake (`BakeCharacter`) | 1.0 ms | |
| Gait update, 5k / 50k agents, one thread (`GaitUpdate`) | 0.20 ms / 2.07 ms | |

### Tests

New doctest cases: the bake is consistent (texture size, clip rows, LOD meshes index/joint/weight
validity, head height in every frame); **walk and run feet stay planted**: in the baked texture
the stance foot moves backwards at the clip's cycle distance per cycle (within 6 %) at ankle
height; gait controller phase is distance-driven (quarter cycle after walking a quarter of the
cycle distance; 1.25× scale → 0.2), and clip selection by speed and turn rate. GUI smoke test now
bakes and renders characters. All 8 CTest entries and 53 doctest cases pass.

Screenshots (not committed): `screenshots/m5_close.png` (plaza close-up with the BT debugger),
`m5_side.png` (side view of the gait), `m5_bench5k.png` (5k benchmark crowd), `m5_shadow.png`.

### Known issues

- No runtime foot IK: sideways avoidance steps and turning while walking slide the feet a little.
- The capsule far LOD differs in silhouette from the character (switch at 90 m).
- The crowd prepare pass uses a hash map per agent id; ~1 ms at 5k, to be reworked for 50k (M10).

## M4 — Behaviour trees

### What works

- **Decision layer** (`sim/BehaviorTree`, `sim/Decisions.cpp`): JSON trees compiled to flat
  arrays, 25 node types (composites, decorators, movement/queue/group/flee actions, conditions),
  per-agent fixed-size state and RNG, 10 Hz staggered parallel ticks, serial resolution of queue
  slots, arrivals and despawns. ADR 0007; formats and node reference in `docs/formats.md`.
- **Locomotion intents**: flow field to a goal, Detour path corridors with funnel steering to a
  point, stop; speed multipliers, per-type personal space, tag avoidance and fleeing.
- **Queues**: goals with capacity, service time and a line direction; agents take tickets, line
  up, get served and leave.
- **Agent types and groups**: types bind a tree to locomotion parameters; spawners spawn weighted
  type mixes and groups whose members follow the leader.
- **Content**: `content/behaviours/{commuter,wanderer,queuer,follower,evacuee}.bt.json` and the
  matching `content/agents/*.agent.json`, loaded at startup (`--no-content` to skip) and **hot
  reloaded** while running (mtime polling → `SetBehaviorTree` / `DefineAgentType` commands, so
  reloads are logged and undoable). Scenes embed the types and trees their spawners use.
- **Plaza scenario**: fountain, benches, four kiosks with service lines, two gates spawning
  commuters, wanderers, queuers and groups that cross to the opposite exit.
- **Debugger**: click an agent in the viewport → the Inspector shows its type, state, movement
  mode, target, queue stage, group and its behaviour tree with running (green) and last-visited
  (yellow) nodes, memory slots and timers; a ring and target line mark the agent; *Follow* keeps
  the camera on it. GUI `--select-agent ID` for screenshots.
- **Commands**: `SetBehaviorTree`, `ValidateBehaviorTree`, `ListBehaviorNodeTypes`,
  `DefineAgentType`, `SetAgentTypeBehaviour`.
- **Navmesh connectivity**: polygons carry connected-component labels; hollow closed obstacles
  (kiosk buildings) no longer trap agents whose random targets landed inside them.

### Measured (Windows, i7-13700H, Release)

| Measurement | Result | Budget |
|---|---|---|
| Sim step, plaza 5k (4.6k alive, every agent on a tree) | 1.54 ms | ≤ 6 ms |
| Sim step, plaza 20k (9.4k alive) | 2.78 ms | |
| Sim step, plain 5k benchmark crowd (for comparison) | 0.99 ms | |
| Headless plaza, 5,000 agents | all leave by t = 800 s, avg 0.33–0.47 ms/step | |
| GUI, plaza 5k at 12× speed, no vsync, whole run incl. ~4.9k peak | avg 1.50 ms/frame | ≤ 16.6 ms |

### Tests

New doctest cases: compiler rejects malformed trees (unknown type, child counts, parameter kinds,
missing name/root, size limit); JSON → compile → decompile → JSON round trip; every content file
loads; Selector / Inverter / RandomChance semantics; Wait then Despawn; **hot reload restarts
running agents on the new tree**; **plaza: commuters, wanderers, queuers and followers coexist,
kiosk service never exceeds capacity, and everyone leaves**; plaza determinism across 1 vs 8
threads; content watcher (new, modified, unchanged and broken files). New CTest
`scenario_plaza_all_leave` (2,000 agents). All 8 CTest entries and 50 doctest cases pass. Bench:
`PlazaStep`.

Screenshot: plaza at ~2,500 agents with the behaviour-tree debugger
(`screenshots/m4_plaza_debugger.png`, not committed).

### Known issues

- No state-colour overlay yet; agent colours come from their type.
- Queue lines are straight; long lines can run into other geometry.
- Script node types (`Custom`) arrive with M6 (Lua); until then any unregistered type is rejected.
- The plaza's first minutes are dense around the gates (spawn rate scales with agent count).

## M3 — Command layer & scene format

### What works

- **Commands** (`commands/`): JSON commands with JSON-Schema params, a registry of 25 built-ins
  (scene, entities, navigation, simulation, play controls), dispatcher applying them between steps,
  undo/redo via handler-provided inverses (gizmo drags merge into one step), Undo/Redo as commands.
  ADR 0006; reference in `docs/formats.md`.
- **Command log + replay**: self-contained logs (scene snapshot at step 0 + every applied command
  with its step index); `crowdsim_headless --replay` reproduces runs bit-identically.
- **Scene format** (`assets/`): versioned `*.scene.json` with migration hook; byte-identical
  save → load → save; stable entity ids; goal references by id or name.
- **glTF import** (cgltf): node-name and `extras` tag conventions → walkable/obstacle, glTF axes
  converted; mesh objects referenced by content-relative path and cached.
- **Simulation scene API**: add/update/remove for objects, links, goals and spawners by `SceneId`
  (removing a goal removes its agents; moving one retargets them).
- **Editor**: hierarchy with Add menu (floor, wall, pillar, obstacle, ramp, stairs, goal, spawner,
  ladder link, glTF import), JSON-driven inspector for every entity kind, ImGuizmo translate (W) /
  rotate (E) with snapping, click-to-select via per-object Jolt bodies and area picking, amber
  selection outline, Delete / Ctrl+D duplicate / Ctrl+Z / Ctrl+Y, History panel, File menu (new,
  open, save, save as, save command log).
- **Headless**: `--scene`, `--commands` batch (one JSON result line per command), `--replay`,
  `--save-log`, `--export-scene`, `--list-commands`.
- **Content**: `content/scenes/two_floor_building.scene.json`, `corridor_bottleneck.scene.json`,
  `content/commands/build_corridor.commands.json`, `content/models/test_room.gltf`.

### Done-criterion evidence

- doctest `M3: a scene built by commands saves, reloads and replays identically`: build by
  commands, run 150 steps, add a pillar mid-run, run, undo, run; save → load reproduces the scene
  JSON exactly, and replaying the log in a fresh engine gives the identical state hash.
- CTest `commands_replay_identical`: the corridor command batch (create scene, 5 objects, goal,
  spawner, 120 s of simulation with 1,500 agents) replayed in a separate process gives the same
  state hash.
- Screenshot: editor with selection outline, gizmo and inspector (`screenshots/m3_editor.png`,
  not committed).

### Known issues

- Navmesh rebuilds are whole-scene after each edit (deferred during gizmo drags).
- The inspector edits raw JSON fields generically; dedicated widgets (e.g. agent-type mixes) come
  with M4.
- Gizmo rotation direction relies on ImGuizmo's Euler convention, detected at runtime; verified
  visually only for translation so far.

## M2 — 3D navigation

### What works

- **Scene geometry** (`geometry/`): primitives (box, ramp, stairs, plane) with per-triangle
  `walkable` / `obstacle` flags and baked face shading; combined into one triangle mesh.
- **Navmesh** (`sim/NavMesh`): tiled Recast build (parallel tiles, deterministic order), Detour
  queries (one per worker thread), dense polygon index, reverse adjacency, wall segments in a grid,
  off-mesh links with a routing cost. ADR 0005.
- **Flow fields** per goal over navmesh polygons; agents string-pull a short corridor with shrunk
  portals.
- **Constrained locomotion**: `moveAlongSurface` + detail-mesh height every step (no wall
  penetration, floors/ramps/stairs followed), ORCA with navmesh walls as hard constraints, level
  separation, off-mesh link traversal (one-way ladder in the building).
- **Physics** (`physics/`): Jolt static collision behind `IPhysicsWorld`; raycasts drive cursor
  picking in the editor. `JoltRuntime` owns Jolt's library globals.
- **Rendering**: static environment meshes; overlays for navmesh + walls, flow field arrows (per
  goal), off-mesh links, agent velocities; reference grid moved below floors. CLI `--overlays`.
- **Scenarios**: `building` — two floors, 20-step stairs, 14° ramp, ladder link, railings,
  partition with three doorways, pillars, benches; flat scenarios now run on a navmesh too.
- **Diagnostics**: headless `--dump-remaining` lists agents still alive (position, velocities,
  polygon, reachability).

### Measured (Windows, i7-13700H, Release)

| Measurement | Result | Budget |
|---|---|---|
| Sim step, 5k agents, navmesh benchmark scene | 1.37 ms | ≤ 6 ms |
| Sim step, 20k / 50k agents | 5.4 ms / 12.2 ms | (M10) |
| GUI, 5,000 agents alive (stream on navmesh), no vsync | avg 1.06 ms, p99 2.8 ms | ≤ 16.6 ms |
| Building, 2k agents in GUI | avg 1.6 ms, p99 2.3 ms | |
| Building navmesh + flow fields + collision build | ~18 ms (20 tiles, 138 polys) | |
| Building, 1,000 agents, seeds 1–4 | 1,000/1,000 reach the upper floor, none stuck | |
| Building, 3,000 agents | all arrive by t = 533 s (doorway congestion, no stuck agents) | |

### Tests

New: primitives, navmesh build (both floors on the mesh, pillar excluded, goal reachable from the
ground floor), **no wall penetration** (every agent on its navmesh polygon at every checked step
of a 400-agent building run, which also must fully arrive), static-scene snapshot sharing, ORCA
wall constraint, Jolt raycasts (multi-level), `scenario_building_all_arrive` (1,000 agents).
All 6 CTest entries + 37 doctest cases pass.

### Known issues

- The building (60 × 40 m) cannot hold 5,000 agents at once (peak ~2,800), so the 5k frame-rate
  check uses the stream scenario on a navmesh.
- Doorways and stairs produce realistic but slow queues; flow fields are not density-aware.
- Navmesh rebuild is whole-scene (fast for these scenes); per-tile incremental rebuild comes with
  editing in M3.

## M1 — Simulation core

### What works

- **Agents** in a structure-of-arrays pool; **spawners and goals** as EnTT components
  (ADR 0004). Spawners: rectangular area, rate, total, start/stop time, goal, per-agent parameter
  ranges (radius, preferred speed, acceleration, colour jitter) drawn from the seeded world RNG.
  Goals: rectangular area, arrival counting, despawn on arrival.
- **Locomotion** each fixed step: parallel spatial-hash rebuild → spawn with overlap rejection →
  preferred velocity (straight line to a lane target in the goal) → K-nearest neighbours + ORCA
  (parallel, allocation-free) → acceleration-limited integration with turn-rate-limited facing.
- **Determinism**: identical state hashes for identical seeds, across processes and across
  1/3/7/20 worker threads.
- **Snapshots**: sim writes previous+current state into a `RenderSnapshot`, handed over through a
  lock-free triple buffer; the renderer interpolates with the fixed-step alpha.
- **Rendering**: instanced capsule agents (one draw call per batch, 32-byte instance data) with
  a facing marker, ground grid with axes and fog, translucent spawner/goal areas, orbit camera
  (LMB orbit, RMB/MMB pan, wheel zoom, F reset) in the dockspace's central node.
- **Editor**: scenario picker (stream / counterflow / benchmark) with agent count, crowd stats,
  play/pause/step/reset/speed, frame/sim/render timings.
- **CLI**: `--scenario`, `--agents`, plus GUI `--no-vsync`, `--sim-speed`, `--camera x,z,d,yaw,pitch`.
  The GUI logs the average / p99 / max frame time of its final 600 frames on exit.

### Measured (Windows, i7-13700H 14C/20T, laptop RTX GPU, Release)

| Measurement | Result | Budget |
|---|---|---|
| Sim step, 1k agents (bench, 20 threads) | 0.19 ms | |
| Sim step, **5k agents** | **0.67 ms** | ≤ 6 ms |
| Sim step, 5k agents, 1 thread | 3.2 ms | |
| Sim step, 20k agents | 3.7 ms | |
| Sim step, 50k agents | 9.1 ms | (M10) |
| Snapshot write, 5k / 50k | 57 µs / 265 µs | |
| GUI frame at ~5k agents alive, no vsync, D3D11 | avg 2.1 ms, p99 6.5 ms | ≤ 16.6 ms |
| GUI frame at ~5k agents alive, no vsync, Vulkan | avg 2.1 ms, p99 3.5 ms | ≤ 16.6 ms |
| Headless `stream` 5,000 agents | all 5,000 arrive by t = 150 s, avg 0.95 ms/step | |

### Tests

doctest unit tests (RNG, clock, job system, logging, triple buffer incl. a two-thread stress test,
spatial hash vs brute force, ORCA cases incl. head-on pass without overlap, sim determinism and
thread-count invariance, reset replay, spawner totals + arrivals, snapshot contents, CLI);
headless smoke; cross-process determinism; `scenario_stream_all_arrive` (2,000 agents, none
stuck); GUI smoke. 5/5 CTest entries pass.

### Known issues

- Dense head-on **counterflow jams** for a while before draining (92 of 5,000 still present at
  t = 300 s). Classic ORCA symmetry; to be addressed with behaviour-level biases in M4.
- The frame that encodes a screenshot PNG takes ~300 ms (encoding runs in bgfx's callback).
- Capsule hemispheres show visible shading bands (low ring count); cosmetic, replaced by VAT
  characters in M5.
- No frustum culling yet (M5); every agent is submitted.

## M0 — Skeleton (done)

Repo layout, CMake presets + vcpkg manifest, SDL3 window, bgfx (D3D11/D3D12/Vulkan/OpenGL verified),
ImGui docking editor with our own bgfx backend, injected spdlog logger, Tracy toggle, headless
executable, EngineContext composition root, no-global-state checks (script + clang-tidy). See ADRs
0001–0003.

Lesson: bgfx textures created with initial data are immutable and silently ignore updates in
release builds; ImGui's dynamic font atlas must be created empty and then uploaded.

## Next: M2 — 3D navigation

Recast navmesh from scene geometry, multi-level support (ramps, stairs, off-mesh links), per-goal
flow fields, navmesh-constrained movement, Jolt static collision, debug overlays.
