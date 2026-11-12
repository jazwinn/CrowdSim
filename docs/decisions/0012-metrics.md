# 0012 — Metrics: a read-only step observer with commands for control and queries

Status: accepted (M9)

## Context

Crowd studies need flow through openings, densities, speeds, time-to-goal and trajectories, in
the editor (live plots, overlays), via MCP (for AI-driven experiments) and as files. Collecting
them must not change the simulation's results or cost much per step.

## Decision

- **Observer, not participant.** `metrics::MetricsCollector` is a `StepObserver`. It runs
  serially after each step and only reads the agent pool and the step's events, so state hashes
  are identical with metrics on or off. It restarts itself when the step counter goes backwards
  (reset / scene replaced).
- **Cheap per step, richer per sample.** Every step it does O(N × gates) segment-crossing tests
  against each agent's last move, adds heatmap time on a ≤ 300 × 300 grid over the scene bounds,
  and records trajectories at a chosen rate. Every 0.5 s it takes a sample (speed percentiles,
  1 m-cell densities, stuck bookkeeping, gate totals). Time-to-goal comes from spawn and arrival
  events.
- **Definitions** (documented in the API): density is agents/m², peak and p90 over occupied 1 m
  cells, and the heatmap value is time-averaged. Flow is crossings per second over the last 10 s
  and per metre of gate width. "Stuck" means trying to move (not stopped, not queueing) at under
  0.15 m/s for 10 s, which is also what persistent jams look like.
- **Commands for everything.** Gates and recording are logged commands (replays reproduce them,
  undo works). Queries and exports are unlogged commands, which the editor panel, Lua and the MCP
  tools all call. Exports write CSV (trajectories, time series) under `content/recordings/` by
  default (git-ignored).
- **Presentation stays in its layer.** The editor reads the collector for ImPlot plots. The
  renderer gets the heatmap as plain data (`render::HeatmapOverlay`) and draws translucent cells,
  rebuilt twice a second.

## Consequences

- Gate checks scale with agents × gates; at 5k agents and a handful of gates this is well under
  0.1 ms per step.
- Recording is in memory (capped at 10 M rows) until exported; long large recordings should be
  exported periodically.
- Gates are not part of the scene format; scripts or MCP recreate them for studies.
