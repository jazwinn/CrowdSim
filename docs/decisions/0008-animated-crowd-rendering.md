# 0008 — Animated crowd rendering: bone-matrix VAT, procedural character, LOD and shadows

Status: accepted (M5)

## Context

M5 replaces capsules with animated characters: 5,000 agents at ≥ 60 FPS, natural gait without
visible foot sliding, LOD, frustum culling and shadows. The stack names ozz-animation baked into
vertex animation textures (VAT). The repository ships no character art, and the simulation must
stay independent of rendering.

## Decision

- **Procedural character.** `animation/` builds an 18-joint humanoid, two skinned LOD meshes
  (≈2.4k and ≈0.6k triangles) and four looping clips (idle, walk, run, turn in place) in code, in
  about 1 ms at start-up. Leg poses come from analytic two-bone IK against authored foot
  trajectories (heel strike, flat foot, toe roll, swing). During stance the contact point stays
  on a ground line moving backwards at exactly the clip's natural speed, so each clip's metres
  per cycle is known by construction. Materials are vertex tags (shirt, trousers, skin, hair,
  fixed); the shader tints them per agent (type colour, hashed palettes), so one mesh gives
  visible variety.
- **ozz does the animation work.** Clips are built as `RawAnimation` keyframes, compiled with
  `AnimationBuilder`, sampled with `SamplingJob` and converted with `LocalToModelJob`.
  ozz-animation is not in the vcpkg baseline, so it is fetched with CMake `FetchContent` from the
  official 0.16.0 release tarball, pinned by SHA-256, with only the runtime and offline libraries.
- **Bone-matrix VAT.** The bake stores skinning matrices (model × inverse bind, top 3 rows) in an
  RGBA32F texture: three texels per joint, one row per frame (54 × 128 texels for all clips).
  This variant was chosen over per-vertex position textures because both LOD meshes share one
  small texture and normals skin correctly. The vertex shader fetches two frames of two clips
  for each of a vertex's two joints, interpolates, blends, then applies the instance transform.
- **Distance-driven gait.** A per-agent `GaitState` lives in the renderer, keyed by agent id. The
  shared walk/run phase advances by distance travelled ÷ (scaled) cycle distance, not by time;
  walk and run share a phase layout, so they cross-fade between 2.0 and 2.7 m/s. Idle blends in
  below 0.45 m/s, turn-in-place when nearly stopped and turning. Clip weights use a smoothed
  speed; the phase uses the true speed. The sim exports speed and turn rate in the snapshot.
- **LOD and culling on the CPU, in parallel.** Each frame `CrowdRenderer::prepare` interpolates
  every agent, tests it against the camera frustum and the shadow light's frustum, and picks
  near VAT, mid VAT (default up to 30 / 90 m, `--lod`) or capsule. It advances gait only for
  agents drawn animated, and compacts instance lists in agent order. Each list is one instanced
  draw per batch: 64 bytes per VAT instance, 32 per capsule.
- **Shadows.** One 2048² directional shadow map (D32F, comparison sampler, 5-tap PCF) covers an
  orthographic square around the camera target, sized by camera distance (≤ 70 m) and snapped
  to texels. Casters are the static scene and agents (mid VAT mesh or capsule). Every lit shader
  shares `shaders/common/shadow.sh`. bgfx requires fragment inputs to match vertex outputs, so
  each vertex shader family has its own depth-only fragment shader.
- **Layering.** `animation/` depends on core and snapshot (plain `CharacterAsset`); `render/` uses
  it for the gait controller only. The app bakes the character and hands it to the renderer;
  the simulation knows nothing about animation.

## Consequences

- Measured (D3D11, 1600×900, no vsync): all 4,744 visible agents as full-detail characters with
  shadows, avg 6.3 ms / p99 8.3 ms; all at mid LOD, 2.6 ms; typical views, about 2 ms.
- Feet do not slide while walking straight at any speed (tested on the baked texture). Sideways
  ORCA corrections and turning while walking still slide the feet slightly, since there is no
  foot IK at runtime.
- The far LOD is a capsule, so its silhouette differs from the character. The switch happens at
  90 m, where both are a few pixels tall.
- CPU prepare is about 1 ms for 5k agents (hash-map lookups for gait state dominate). GPU culling
  and indirect draws for 50k agents are M10 work; the instance lists are already compact and in
  a stable order for that.
- Custom character art can replace the procedural humanoid by filling the same
  `CharacterAsset` from glTF skins (cgltf + ozz) without touching the renderer.
