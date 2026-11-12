# CrowdSim status

Last updated: 2026-11-12

| Milestone | State |
|---|---|
| M0 — Skeleton | done (Windows verified; macOS CI passed once) |
| M1 — Simulation core | **done** (Windows) |
| M2 — 3D navigation | next |

CI currently builds and tests **Windows only**. Linux and macOS are deferred until the application
is feature-complete (the matrix entries are commented in `.github/workflows/ci.yml`).

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
