# CrowdSim

Real-time 3D crowd simulator in C++20: thousands of animated agents driven by behaviour trees,
editable at runtime, scriptable in Lua and controllable by AI agents over MCP.

Current milestone and measurements: [docs/STATUS.md](docs/STATUS.md).
Design decisions: [docs/decisions/](docs/decisions/).

## Building

Requirements: CMake ≥ 3.25, a C++20 compiler (MSVC 19.4x+, GCC 13+, Clang 17+ / Apple Clang 16+),
Ninja (Linux/macOS), and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.
Dependencies are installed automatically from `vcpkg.json` on first configure.

```bash
# Windows (Visual Studio 2026)
cmake --preset windows-msvc
cmake --build build/windows-msvc --config Release
ctest --preset windows-msvc-release -LE gui

# Linux / macOS
cmake --preset linux        # or: macos
cmake --build build/linux --config Release
ctest --preset linux-release -LE gui

# Headless-only build (no SDL/bgfx/ImGui)
cmake --preset headless
```

## Running

```bash
build/windows-msvc/bin/Release/crowdsim                       # editor
build/windows-msvc/bin/Release/crowdsim --renderer vulkan     # pick a backend
build/windows-msvc/bin/Release/crowdsim_headless --scenario stream --agents 5000 --steps 2700
build/windows-msvc/bin/Release/crowdsim --scenario counterflow --agents 5000 --sim-speed 4
build/windows-msvc/bin/Release/crowdsim --scenario building --agents 2000 --overlays navmesh,flow
build/windows-msvc/bin/Release/crowdsim --scene scenes/two_floor_building.scene.json
build/windows-msvc/bin/Release/crowdsim_headless --commands content/commands/build_corridor.commands.json --save-log run.cmdlog.json
build/windows-msvc/bin/Release/crowdsim_headless --replay run.cmdlog.json
build/windows-msvc/bin/Release/crowdsim_bench
```

File formats and the full command list: [docs/formats.md](docs/formats.md).

`crowdsim_headless` prints a one-line JSON result on stdout (logs go to stderr).

## Layout

| Dir | Purpose |
|---|---|
| `core/` | logging, job system, RNG, IDs, hashing, fixed-step clock, triple buffer |
| `geometry/` | triangle meshes and primitives (box, ramp, stairs, plane) |
| `snapshot/` | plain render-snapshot data passed from sim to render |
| `sim/` | agent pool, spawners/goals (EnTT), navmesh (Recast/Detour), flow fields, ORCA, scenarios (no rendering dependencies) |
| `physics/` | Jolt static collision and raycasts behind `IPhysicsWorld` |
| `assets/` | scene JSON, glTF import, asset library |
| `commands/` | command registry, dispatcher, undo/redo, command log + replay |
| `platform/` | SDL3 window and events |
| `render/` | bgfx renderer, ImGui backend, embedded shaders |
| `ui/` | ImGui editor panels |
| `app/` | composition root: GUI and headless executables |
| `content/` | sample scenes, command batches, models |
| `tests/` | doctest unit tests and headless smoke/determinism/scenario tests |
| `bench/` | Google Benchmark suite (sim step at 1k–50k agents) |
| `tools/` | CI checks (e.g. no mutable global state) |
