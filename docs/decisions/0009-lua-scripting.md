# 0009 — Lua scripting: one sandboxed state, deferred batched nodes, commands for effects

Status: accepted (M6)

## Context

Scripts must add BT nodes, entity types and scenario logic at runtime and hot-reload them,
without breaking the simulation's determinism (ADR 0004), the "everything is a command" rule
(ADR 0006) or the per-step budget, and without letting a buggy or hostile script take down the
process.

## Decision

- **One sandboxed LuaJIT state** (`script::ScriptHost`), created by the composition root and
  injected; sol2 for bindings. Only base, string, table, math and bit are opened. Loaders that
  reach the file system or accept bytecode are removed (`load` is text-only). The JIT is
  disabled so a count hook sees every instruction. Each outermost call gets an instruction and
  wall-time budget (re-entrant calls share it). A counting allocator enforces a memory cap per
  state, falling back to a hook check if a LuaJIT build refused custom allocators. sol2 is built
  to catch C++ exceptions itself (`SOL_EXCEPTIONS_SAFE_PROPAGATION=0`), so binding errors reach
  Lua with their message instead of LuaJIT's generic "C++ exception".
- **Script nodes are deferred and batched.** The parallel decision tick never enters Lua. A
  `Custom` node records `(tree, node, agent)` in a per-thread list and returns Running. After the
  tick, the simulation sorts the requests and calls the injected `CustomNodeRunner` serially,
  once per node instance with a span of agents. Results go into the node's memory slot and are
  returned on the agent's next decision tick. This costs one decision tick of latency (100 ms at
  10 Hz), but results are identical for any thread count and per-call overhead is amortised.
  Node parameters are declared at registration and validated when trees compile (`CustomNodeSpec`).
- **Script control API on the simulation** (`scriptMoveTo`, `scriptSetState`, `scriptRandom`,
  ...), documented as valid only from serial callbacks. Node contexts use it; agents' own
  random streams keep scripted randomness deterministic.
- **Step observers and events.** The simulation records spawn, arrival and despawn events per
  step and calls observers after each step. Entity `on_tick` (at its own rate) and `events.on`
  handlers run there.
- **Effects go through commands, logs carry sources.** Lua's `scene.*` and `bt.set` run command
  handlers directly. They are applied but not logged or undoable individually. What is logged is
  the command that ran the Lua (`LoadScript` with its source, `EvalLua`, `SpawnEntity`); replay
  re-runs the Lua and regenerates the same effects. Every command log starts with a `LoadScript`
  for each loaded script (a log prelude), so logs replay in a fresh process without the files.
- **Hot reload = re-run.** A changed script re-runs in the same state. Nodes, entity types and
  event handlers are replaced by name or owner; compiled trees keep working because nodes are
  referenced by index into a stable registry. Scripts load before behaviour trees at start-up.

## Consequences

- Script nodes react one decision tick late. Behaviours that need the same-tick result should
  be built from built-in nodes.
- Shared Lua globals across scripts are convenient (console access to script state) but allow
  scripts to clash; names should be prefixed by convention.
- Lua's `pairs` order is not stable across processes, so scripts must not let it drive
  decisions (documented; the API returns arrays).
- Performance: the kiosk scenario (600 agents, 60 % tourists on a Lua node) steps in 0.44 ms on
  average. Lua is never on the per-agent locomotion path.
