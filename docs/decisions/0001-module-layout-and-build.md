# ADR 0001: Module layout and build system

- Status: accepted
- Date: 2026-11-12

## Context

CrowdSim must build on Windows, Linux and macOS from one CMake project, keep the
simulation independent of rendering, and run headless with no GPU libraries loaded.

## Decision

- **One CMake static library per module**, at the repository root as the spec lays out
  (`core/`, `sim/`, `platform/`, `render/`, `ui/`, `app/`, later `physics/`, `assets/`,
  `script/`, `commands/`, `mcp/`). Public headers live in `<module>/include/crowdsim/<module>/`;
  private headers in `<module>/src/`. The helper `crowdsim_add_library()` in
  `cmake/CrowdSimTargets.cmake` applies the warning policy and the `CrowdSim::<module>` alias.
- **Dependency direction is enforced by the link graph**: `sim` links only `core` (+ EnTT);
  `render` links `core`, ImGui and bgfx (privately); `platform` links `core` and SDL3. Only `app/`
  links everything. A header that includes across the forbidden direction fails to compile.
- `platform/` is an added module (the spec mentions it in the `sim/` rule but not in the layout):
  it owns SDL3. Native window handles cross to `render/` as the plain struct
  `core::NativeWindow`, so `render/` never sees SDL and `platform/` never sees bgfx.
- **vcpkg manifest mode**, pinned with `builtin-baseline`. GUI and Tracy are manifest *features*
  selected from CMake options before `project()`, so `-DCROWDSIM_BUILD_GUI=OFF` gives a build
  with no SDL/bgfx/ImGui at all.
- **Static linking everywhere.** bgfx's vcpkg port is static-only on Windows, so Windows uses the
  `x64-windows-static-md` triplet (static libs, dynamic CRT). Linux and macOS triplets are static
  by default. This also means no DLL copying next to executables.
- Presets: `windows-msvc` (Visual Studio 2026 generator) for local work, `windows-ninja`
  (MSVC + Ninja, used by CI from a dev shell), `linux`, `macos` (arm64), `headless`.
- Executables: `crowdsim` (GUI) and `crowdsim_headless`. Both share `crowdsim_app_common`
  (command line, `EngineContext`).

## Consequences

- Adding a module is a directory plus one `crowdsim_add_library()` call.
- First configure is slow (bgfx tools pull in glslang/SPIRV-Tools); CI caches vcpkg binaries.
- ozz-animation has no vcpkg port at this baseline; it will come in via CPM.cmake in M5.
