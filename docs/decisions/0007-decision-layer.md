# 0007 — Decision layer: compiled behaviour trees on top of locomotion

Status: accepted (M4)

## Context

M4 adds agent intelligence: commuters, wanderers, queuers, groups and evacuees, authored as
data (JSON, later the node editor and Lua), hot-reloadable while agents run, and cheap enough
for 5,000+ agents inside the 6 ms step budget, without breaking determinism (ADR 0004).

## Decision

- **Trees are data, compiled to a flat array.** `*.bt.json` → `BtNodeDesc` → `CompiledTree`:
  pre-order nodes with child index lists, interned strings and parameters resolved once
  (unknown types, wrong child counts, bad parameter kinds and size limits are rejected with the
  node path). No virtual calls or allocations per tick; `tickNode` is a switch over node types.
  Limits: 64 nodes, 24 memory slots, 8 timers per tree; this keeps per-agent state fixed-size.
- **Per-agent state lives in the agent pool** (`BtState`: memory slots for composites and
  decorators, timers, a small blackboard target, visited/running masks for the debugger). Trees
  are shared; state carries the tree generation it belongs to.
- **Decisions run at a lower rate than locomotion.** Each agent ticks its tree every
  `round(stepHz / decisionHz)` steps (10 Hz by default), staggered by agent id so the cost is
  spread evenly. Ticks run in parallel; a tick only writes its own agent's slot.
- **Trees express intent, locomotion executes it.** Actions set a `MoveMode` (Stop, Goal = flow
  field, Point = Detour path corridor of up to 16 polygons with funnel steering), a speed
  multiplier, a state label and avoidance tags. ORCA, navmesh constraints and the flow fields
  from M1/M2 are unchanged underneath, so every behaviour inherits no-penetration and avoidance.
- **Shared resources are resolved serially.** Anything contended — queue tickets and service
  slots, arrivals, despawns — is requested during the parallel tick and granted in a serial,
  index-ordered pass (`resolveDecisions`), which keeps results identical for any thread count.
  Queues are per goal with a capacity, service time and a line direction/spacing.
- **Randomness is per agent.** Each agent has its own PCG32 stream seeded from the world seed and
  its id, so decisions do not depend on evaluation order.
- **Hot reload = new generation.** Replacing a tree bumps its generation; agents whose state is
  from an older generation restart from the root on their next tick. Files under
  `content/behaviours` and `content/agents` are watched (mtime polling, twice a second) and
  applied as `SetBehaviorTree` / `DefineAgentType` commands, so reloads are logged, undoable and
  replayable like any other edit. Scene files embed the trees and types they use, so scenes and
  command logs stay self-contained.
- **Agent types** bind a behaviour to locomotion parameters, personal space and tags. Spawners pick
  types from a weighted mix and can spawn groups (followers reference their leader's id).
- **Navmesh connectivity.** Closed obstacle boxes rasterize as hollow shells and leave walkable
  islands inside them. Polygons get connected-component labels; random point targets must lie on
  the agent's component, and a partial path that cannot leave the current polygon fails the action
  instead of replanning forever.

## Consequences

- Behaviours cost ~0.5 ms per step on the 5k plaza (1.54 ms total vs ~1 ms for the plain
  benchmark crowd) and scale with agents, not tree size.
- A 64-node / 24-slot ceiling per tree; larger behaviours must be split into subtrees (M7 node
  editor) or script nodes (M6 Lua, reserved as `Custom`).
- 100 ms decision latency at 10 Hz is visible only as slightly late reactions; urgent reactions
  (fleeing) can raise `SimConfig::decisionHz`.
- Content files and the built-in C++ definitions of the standard types must stay equivalent; a
  test loads every content file, and the plaza gives the same state hash either way.
