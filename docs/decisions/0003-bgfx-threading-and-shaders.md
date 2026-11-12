# ADR 0003: bgfx threading mode and shader pipeline

- Status: accepted
- Date: 2026-11-12

## Context

bgfx can run its render thread itself (multithreaded) or on the caller's thread. Shaders must be
compiled per backend (DXBC, SPIR-V, Metal, GLSL) with bgfx's `shaderc`.

## Decision

- **Single-threaded bgfx** (call `bgfx::renderFrame()` before `bgfx::init`). The main thread owns
  the window, UI and rendering; parallelism comes from the job system running simulation and
  culling work. This keeps SDL event handling and swap-chain resizing on one thread, which macOS
  requires. Revisit if profiling at 50k agents shows submission bound on the main thread.
- **Shaders compiled at build time.** `cmake/BgfxShaders.cmake` runs vcpkg's `shaderc` for each
  `.sc` file and every backend the host can target (SPIR-V, GLSL 150, Metal everywhere; DXBC
  `s_5_0` additionally on Windows hosts) and emits `--bin2c` headers that are embedded in the
  binary. `ShaderLibrary` picks the blob for the active renderer. No shader files ship at runtime.
- **ImGui backend** is our own (`render/ImGuiRenderer`), implementing ImGui 1.92+'s dynamic texture
  protocol (`ImGuiBackendFlags_RendererHasTextures`), since vcpkg's ImGui has no bgfx binding.
- **View ids:** 0 = scene; 255 = ImGui (drawn last). More scene/debug/shadow views will take ids in
  between.

## Consequences

- A clean build needs vcpkg's `bgfx[tools]`, which builds glslang and SPIRV-Tools (slow once, then
  cached).
- D3D12 uses the DXBC blobs, which bgfx supports.
