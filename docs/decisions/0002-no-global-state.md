# ADR 0002: No singletons or mutable global state

- Status: accepted
- Date: 2026-11-12

## Context

The spec forbids singletons and mutable global/static state, including function-local statics.
Hidden state breaks determinism, makes it impossible to run two worlds side by side (e.g. A/B
experiments, tests), and hides dependencies.

## Decision

- **Composition root.** `app/` builds an `EngineContext` on `main()`'s stack. It owns the
  `LogService`, `JobSystem` and `Simulation` and hands out references. Modules take what they need
  through constructors (`sim::SimServices`, `render::Renderer(logger, config)`, ...).
- **Logging.** The spdlog logger is constructed directly and never registered in spdlog's global
  registry; the default logger is never used. A ring-buffer sink feeds the UI log panel and, later,
  the MCP `log_tail` tool. Console output goes to **stderr** so stdout stays free for MCP stdio.
- **Jobs.** One `enki::TaskScheduler` per `JobSystem` instance; tests create several to prove it.
- **RNG.** `core::Pcg32` objects owned by whoever needs randomness; the sim world owns its own.
- **Enforcement in CI.**
  - `tools/check_global_state.py` (with a self-test) flags non-const `static` variables,
    `thread_local`, and `instance()`/`getInstance()` accessors in project code.
  - clang-tidy `cppcoreguidelines-avoid-non-const-global-variables` as an error, for namespace-scope
    globals the regex cannot see.
  - A reviewed exception can be marked with `// global-state-ok: <reason>` on the line.

## Third-party global state we accept

These libraries keep internal process-wide state we cannot remove. We contain each behind one
RAII owner created by the composition root, and never touch it elsewhere:

| Library | Global state | Owner |
|---|---|---|
| SDL3 | `SDL_Init` subsystem state | `platform::Window` |
| bgfx | the device (`bgfx::init` is process-wide) | `render::Renderer` (one per process) |
| Dear ImGui | `GImGui` current-context pointer | `ImGuiSession` in `app/GuiApp.cpp` |
| Tracy | profiler client (optional build) | compiled out unless `CROWDSIM_ENABLE_TRACY` |

The simulation (`sim/`) and headless binary use none of them.

## Consequences

- Constructors list their dependencies, which is verbose but explicit.
- Code that "just needs a logger" must be passed one.
