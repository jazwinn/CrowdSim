# CrowdSim

A **generic** real-time 3D crowd simulator and editor (C++20). It must stay general-purpose:
any venue, any crowd, any scenario is built from content, never from engine code written for
one case.

## The rule: mechanics in the engine, behaviour and scenarios in content

Decide where a change belongs before writing it:

| It describes... | It goes in | Examples |
|---|---|---|
| How the world physically works | engine, as a generic mechanism driven by data | navmesh, avoidance, moving escalator steps (`escalator` object tag), queues at goals with `capacity` |
| What a person decides to do | a **behaviour tree node** with parameters, used from `content/behaviours/*.bt.json` | `MoveToGoal`, `Queue`, `AvoidArea`, `SetEscalatorStyle {side, walk}` |
| A particular population | `content/agents/*.agent.json` (parameters + which tree) | `escalator_stander_left` = speed/radius + tree `escalator_stand_left` |
| A particular place or study | `content/scenes/*.scene.json`, `content/scripts/*.lua`, `tools/make_*.py` | `escalator_etiquette` scene, `escalator_trial.lua` report |

Do not:

- Branch on scene, object, goal, spawner or agent-type **names** in engine code.
- Encode behaviour in agent-type `tags` or object tags that the engine interprets as decisions
  (tags describe *things*; trees decide *actions*). Object tags may configure environment
  mechanics (`walkable`, `obstacle`, `escalator`, `escalator:speed=0.5`).
- Add a scenario-specific code path because it is quicker. Add or extend a reusable node or
  mechanism, expose its knobs as node parameters or tags, and put the scenario in `content/`.

When adding a behaviour node: add it to `BtNodeType` and the `kNodeTypes` table
(`sim/src/BehaviorTree.cpp`, with parameter specs and a one-line description), implement its
tick in `Simulation::tickNode` (`sim/src/Decisions.cpp`) by setting per-agent state in
`AgentPool` that locomotion reads, validate parameters at compile time, document it in the node
table in `docs/formats.md`, and test it in `tests/`. The node editor, MCP and Lua pick it up
from the table. Prefer Lua custom nodes (`docs/scripting.md`) for one-off experiments.

Remember that trees tick at a low, staggered rate: state a node sets may arrive a few steps
after an agent spawns, so locomotion must cope with it changing.

## Build and test (Windows)

```bash
cmake --preset windows-msvc
cmake --build build/windows-msvc --config Release --parallel
ctest --test-dir build/windows-msvc -C Release
```

- Binaries: `build/windows-msvc/bin/Release/` — `crowdsim.exe` (editor), `crowdsim_headless.exe`,
  `crowdsim_tests.exe` (doctest), `crowdsim_bench.exe`.
- Close a running editor before rebuilding (`Get-Process crowdsim | Stop-Process`), or linking
  `crowdsim.exe` fails.
- Linux/macOS use the `linux` / `macos` presets; CI builds and tests all three.
- Headless runs are the quickest check of a scene: `crowdsim_headless --scene scenes/<name>.scene.json
  --steps 9000 --seed N`. `--script` runs a Lua file (content-relative), `--no-content` proves a
  scene is self-contained. Compare several seeds before drawing conclusions.
- Editor screenshots: `crowdsim --scene ... --frames N --screenshot out.png --camera x,z,dist,yaw,pitch`.
  Run it from an empty folder to get the default layout instead of the user's `imgui.ini`.

## Architecture (see `docs/decisions/` for the ADRs)

`core` (jobs, logging, RNG) · `geometry` · `sim` (agents SoA, navmesh, flow fields, ORCA,
behaviour trees, escalators) · `physics` · `assets` (JSON formats) · `commands` (every edit is
a command: GUI, Lua, MCP and replay share one path) · `animation` · `render` (bgfx) · `ui`
(ImGui panels; reads data, returns commands) · `script` (sandboxed Lua) · `mcp` · `metrics` ·
`app`.

- No singletons or mutable globals (ADR 0002); pass services explicitly.
- The simulation is deterministic: fixed step, seeded per-agent RNG, serial passes for shared
  state, results identical across thread counts. Keep it that way (tests check it).
- Formats are documented in `docs/formats.md`; Lua API in `docs/scripting.md`; MCP in
  `docs/ai-integration.md`. Update them with the code.

## Conventions

- Conventional Commits, imperative subject, body explains why; no attribution trailers.
  One logical change per commit; push to `origin/main` when a piece of work is done.
- Comments explain non-obvious reasons, not what the code says.
- Verify before calling something done: build, tests, a headless run or a screenshot, and say
  what was not verified.
