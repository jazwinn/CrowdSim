# ADR 0006: Command layer, scene identity and replay

- Status: accepted
- Date: 2026-11-12

## Context

M3 requires every mutation to go through commands (GUI, Lua, MCP, replay), undo/redo, a replayable
command log, and a JSON scene format that saves, reloads and replays identically.

## Decision

- **Commands are JSON**, `{"cmd": name, "params": {...}}`, handled by a `CommandRegistry` of
  named handlers with a description and a JSON Schema. Plain JSON (rather than a C++ variant of
  structs) is what Lua, MCP and log files speak anyway; the schema doubles as the MCP tool
  definition in M8. Handlers validate by parsing into typed sim structs (throwing on bad input);
  nothing is applied if parsing fails.
- **Handlers return their own inverse.** Undo = executing the inverse command; redo = executing
  the original again (whose fresh inverse goes back on the stack). Deletes return `AddEntity` with
  the full entity JSON including its id; scene replacements return `RestoreScene` with the
  previous scene inline. Consecutive commands with the same `merge` key (gizmo drags) collapse
  into one undo step. Undo/redo are themselves accepted as commands.
- **Stable scene ids.** Every scene entity has a `SceneId` (never reused; `nextId` persisted), so
  commands, undo records, logs and selections refer to entities robustly across edits, deletes
  and save/load. Spawners reference goals by id (or name in commands).
- **Applied between steps.** The dispatcher queues commands and the app flushes the queue once per
  frame before stepping; headless runs execute commands immediately in sequence. Play controls
  (`Play`, `Pause`, `Step`, `RunFor`) edit an app-owned `SimControl`; the main loop runs the steps.
- **Self-contained logs.** A log starts with a `RestoreScene` snapshot at step 0 (taken whenever
  the scene is replaced or reset) and records each applied state-changing command with its step
  index, in canonical form: file loads are recorded as their content, created entities carry
  their assigned ids. Replay clears, re-applies commands at their step indices, and steps to
  `endStep`; the simulation's determinism (ADR 0004) makes the result bit-identical.
- **Scene format.** `assets/` owns `*.scene.json` (ADR-free details in `docs/formats.md`):
  versioned with a migration hook, floats rounded to 6 significant digits and entities in creation
  order so save → load → save is byte-identical.
- **Editor.** The ImGui editor (`ui::SceneEditor`) reads the simulation const and emits commands
  only: hierarchy, a generic JSON-driven inspector (works for every entity kind and grows with the
  format), ImGuizmo translate/rotate with snapping, history panel. Navmesh/collision sync is
  deferred while a gizmo drag is active.

## Consequences

- Any new state needs a command (and its inverse) before the GUI, Lua or MCP can change it — by
  design.
- JSON parsing per command is slower than direct calls, but commands are edits, not per-step work.
- Navmesh rebuild after an edit is whole-scene (~10–20 ms for the sample scenes). Per-tile
  incremental rebuild is the next step if large imported scenes make editing sluggish.
