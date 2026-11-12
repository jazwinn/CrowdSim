# 0011 — MCP server: protocol core, tools over commands, engine-thread execution

Status: accepted (M8)

## Context

AI agents must be able to drive CrowdSim end to end: build scenes, define agent types and
behaviours, run experiments headless and read results. That has to work both headless (the
client spawns the process) and against a running editor, without bypassing the command layer or
creating races with the simulation.

## Decision

- **Own small MCP implementation** (`mcp/`): JSON-RPC 2.0 over nlohmann::json with `initialize`
  (protocol version negotiation; 2025-06-18 by default), `ping`, `tools/list`, `tools/call`,
  `resources/list`, `resources/read` and empty prompt/template lists. No SDK dependency; the
  protocol surface we need is small and testable in-process.
- **Two transports.** stdio: newline-delimited messages on the engine thread, used for headless
  runs; logs go to stderr. Streamable HTTP: `POST /mcp` via cpp-httplib on a background thread.
  It binds to the given address (documented as localhost), returns JSON responses (no SSE
  stream), and enforces an optional bearer token.
- **The engine thread executes everything.** HTTP threads only parse and wait. Messages go
  through `McpRequestQueue` to the main loop, which handles them between steps or frames. This
  keeps the single-writer rule and the command layer's ordering intact; no locks around the
  simulation.
- **Tools map onto commands.** Mutating tools execute commands, so they are validated, undoable,
  logged and replayable exactly like editor actions. Read-only tools use query commands
  (`QueryAgents`, `ListAgentTypes`, `ListBehaviorTrees`) or summaries (`scene_describe`,
  `metrics_get`). Tool failures are returned as tool results with `isError` (the model can read
  and fix them), not protocol errors.
- **LLM-sized results.** Samples instead of dumps (`agents_query` count plus evenly spaced
  sample), caps (100 entities, 300 log lines, 8 KB of script output), and `sim_run_for` returns a
  metrics summary so a whole experiment is one call.
- **Async tools.** A tool may return a future (the editor's `screenshot`: bgfx writes the PNG a
  few frames later). The response future is deferred and resolved on the transport thread, never
  blocking the engine.

## Consequences

- One request at a time touches the engine; a long `sim_run_for` blocks other MCP requests and,
  in the editor, rendering for its duration (60 s of a 300-agent scene: ~0.4 s).
- Screenshots are full editor frames at window resolution, sent as uncompressed PNGs of several
  MB; clients with image size limits should shrink the window.
- No server-initiated notifications (list changes, progress); clients re-list when needed.
