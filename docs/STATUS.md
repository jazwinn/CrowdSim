# CrowdSim status

Last updated: 2026-11-12

| Milestone | State |
|---|---|
| M0 — Skeleton | done (Windows verified; macOS CI passed once) |
| M1 — Simulation core | done (Windows) |
| M2 — 3D navigation | done (Windows) |
| M3 — Command layer & scene format | **done** (Windows) |
| M4 — Behaviour trees | next |

CI currently builds and tests **Windows only**. Linux and macOS are deferred until the application
is feature-complete (the matrix entries are commented in `.github/workflows/ci.yml`).

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
