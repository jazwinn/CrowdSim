# CrowdSim status

## Current milestone: M0 — Skeleton (complete locally; CI not yet run)

Last updated: 2026-11-12

### What works

- **Build:** CMake 3.25+ presets, vcpkg manifest mode pinned to a baseline. GUI and Tracy are
  manifest features toggled by `CROWDSIM_BUILD_GUI` / `CROWDSIM_ENABLE_TRACY`. Project code builds
  warning-free under MSVC `/W4 /WX` (GCC/Clang flags `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`
  are configured but not yet exercised).
- **Modules:** `core` (LogService, JobSystem over enkiTS, PCG32 RNG, FixedStepClock, typed Ids,
  FNV-1a hash, Tracy macros), `sim` (Simulation stub with EnTT registry, world RNG, state hash),
  `platform` (SDL3 window), `render` (bgfx Renderer, own ImGui bgfx backend, build-time shaders),
  `ui` (dockable editor shell), `app` (EngineContext composition root, GUI + headless executables).
- **GUI (`crowdsim`):** SDL3 window, bgfx clear, ImGui docking layout (Scene, Viewport, Simulation,
  Stats, Inspector, Log), play/pause/step/reset/speed, live log panel, F12 / `--screenshot` PNG
  capture. Verified on **D3D11, D3D12, Vulkan and OpenGL** on Windows (screenshots in `screenshots/`,
  not committed).
- **Headless (`crowdsim_headless`):** no SDL/bgfx/ImGui linked; runs N fixed steps and prints a JSON
  result line on stdout (logs on stderr). A `-DCROWDSIM_BUILD_GUI=OFF` build was verified to
  compile and pass tests with none of the GUI packages installed.
- **No global state:** `tools/check_global_state.py` (+ self-test) and a clang-tidy
  `cppcoreguidelines-avoid-non-const-global-variables` CI step. 0 violations. See ADR 0002.
- **Tests (doctest + CTest):** RNG reference values, fixed-step clock, job system, logging (ring
  buffer, no global registration), sim determinism/reset, command-line parsing; headless smoke test;
  cross-process headless determinism test (same seed → same hash, different seed → different hash);
  GUI smoke test (label `gui`). All 4 pass locally.
- **CI:** `.github/workflows/ci.yml` — lint job (global-state check, clang-format) then Windows
  (MSVC + Ninja), Linux (GCC, GUI smoke under Xvfb with OpenGL) and macOS arm64 builds with vcpkg
  binary caching.

### Measured (Windows, i7-13700H, RTX-class laptop GPU, Release)

| Measurement | Result |
|---|---|
| GUI frame time, empty scene, vsync on | 8.4 ms/frame (D3D11), 8.5 ms (Vulkan): vsync-bound on a 120 Hz display |
| Headless, 300 empty sim steps | < 0.01 ms total |
| Clean vcpkg dependency build | ~26 min (one-off; bgfx tools pull glslang + SPIRV-Tools) |

### Known issues / not yet verified

- **CI has never run**: nothing is committed or pushed yet. Linux and macOS builds are untested; expect
  small portability fixes (Wayland/X11 handle path, Apple Clang warnings, `-Wconversion` noise).
- macOS: bgfx Metal is given the `NSWindow` from SDL; untested.
- `--renderer opengl` reports "OpenGL 2.1" on Windows (bgfx default context) but renders correctly
  with the GLSL 150 shaders; revisit if GL-specific features are needed.
- No Tracy build has been exercised yet (`-DCROWDSIM_ENABLE_TRACY=ON`).

### Lessons recorded

- bgfx textures created with initial data are **immutable**; `updateTexture2D` on them is silently
  ignored in release builds. ImGui's dynamic atlas must be created empty, then uploaded
  (`render/src/ImGuiRenderer.cpp`).

### Next: M1 — Simulation core

EnTT agent components, snapshot double-buffering + interpolation, parallel spatial hash, spawner +
goal on a flat plane, straight-line steering + ORCA, instanced capsule rendering, 5k agents at
≥ 60 FPS, headless determinism with agents.
