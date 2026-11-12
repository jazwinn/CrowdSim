# 0013 — Scale-up: measured bottlenecks, CPU wins, and a GPU-compute locomotion path

Status: accepted (M10)

## Context

The design target is 5,000 agents at ≥ 60 FPS, which M1–M9 meet comfortably. M10 profiles
20k–50k agents, implements the highest-value improvements, and designs the path beyond the
CPU's reach.

## Measurements (i7-13700H, 20 threads, Release, `crowdsim_headless --scenario benchmark --profile`)

ms per step, typical of repeated runs (the laptop varies ±30 % with temperature):

| Phase | 20k before | 20k after | 50k before | 50k after |
|---|---|---|---|---|
| avoidance (neighbours + ORCA) | 3.36 | 2.40 | 7.74 | 5.4 |
| hash build (+ periodic reorder) | 0.46 | 0.55 | 1.06 | 1.3 |
| integrate (navmesh moves) | 0.58 | 0.54 | 1.33 | 1.2 |
| preferred velocities | 0.53 | 0.49 | 1.14 | 1.05 |
| observers (metrics, scripts) | 0.34 | 0.3 | 0.91 | 0.68 |
| decisions (BTs) | 0.13 | 0.14 | 0.21 | 0.24 |
| **total** | **5.5** | **4.6** | **12.8** | **~10.3** |

Findings:

1. **Avoidance dominates** (60 %). It is memory-bound, not maths-bound: each query scans ~100
   candidates in a 3 × 3 cell block of a dense crowd, and in spawn order those candidates are
   scattered across the agent arrays.
2. **Decisions are cheap** (staggered 10 Hz trees cost 0.2 ms at 50k), so coarser BT rates under
   load would buy little. The stride is configurable (`decisionHz`) if scripts make it heavier.
3. **Serial observers** grow linearly (0.9 ms at 50k) and sit outside the parallel phases.
4. In the editor the **simulation runs on the render thread**. At 50k a 10 ms step lands in some
   frames, so the average is 6.5 ms (≈ 150 FPS) but p99 is 23 ms. Rendering alone (culling, LOD,
   animation, ~4k visible characters) is ~2 ms, with crowd preparation ~1.1 ms.

## Decisions (implemented)

- **Positions in the spatial hash.** Hash entries carry (x, z, height) in bucket order, so the
  neighbour filter reads contiguous memory; only accepted neighbours touch agent arrays. Results
  are bit-identical.
- **Spatial reordering of the agent pool.** Every 64 steps the pool is permuted into Z-order of
  its 2.5 m cells (ties by id). This is deterministic and thread-count independent, and agents
  are referenced across steps by id only. Avoidance −30 % at 20k–50k.
- **Metrics heatmap every third step** with three steps of time (same time average).
- **Renderer gait state** indexed directly by agent id instead of a hash map.
- **Per-phase profiling** is always on (`Simulation::lastStepProfile`, `--profile`).

## Next steps (designed, not implemented)

1. **Simulation thread** (largest effect on frame pacing). Run steps on a worker that owns the
   simulation. Render from the triple-buffered snapshot, which already exists and is the only
   thing the renderer reads. Apply commands at step boundaries through the dispatcher's queue,
   which already exists, and expose a copied UI state for inspector, metrics and picking. Frame
   time then stays near the render cost (~2 ms) regardless of sim cost.
2. **GPU culling + indirect draws** for 50k+ visible agents. A compute pass culls the instance
   buffer (already compact and stable-ordered), writes per-LOD indirect args and drops the CPU
   prepare pass (1.1 ms at 50k).
3. **SIMD ORCA**: four or eight agents per lane group (AoSoA neighbour lists, branch-free
   half-plane construction), worth ~2× in the avoidance kernel once memory stalls are gone.

## GPU-compute locomotion path (future ADR-level design)

Goal: 200k+ agents at 30 Hz. Keep the CPU authoritative for decisions and the scene; move the
per-agent inner loop to compute shaders (bgfx compute, D3D11+/Vulkan/Metal):

- **Data:** structured buffers for position, velocity, preferred velocity, radius, nav polygon
  and flow-field cell. Flow fields are uploaded per goal as polygon→direction buffers (they are
  already per-polygon tables). Navmesh walls are a segment buffer in a uniform grid (as on the
  CPU).
- **Passes per step:** (1) hash: cell keys, GPU radix sort, cell start offsets; (2) preferred
  velocity from the flow-field tables; (3) neighbour gather + ORCA with a fixed K (shared-memory
  tile per cell), solving the 2D LP with the same incremental algorithm; (4) integrate with a
  conservative wall clamp against nearby segments; (5) write the render instance buffer directly
  (no CPU snapshot).
- **What stays on the CPU:** behaviour trees and scripts (10 Hz, reading positions back
  asynchronously one step late), spawning and despawning (compaction via prefix sums), queues,
  metrics (from a downsampled readback), and exact navmesh polygon tracking for corridor-path
  agents (a small subset).
- **Determinism:** GPU float order differs across vendors, so GPU mode would be deterministic
  per machine only. Replays that must be bit-exact across machines keep the CPU path, which stays
  the reference implementation.
- **Cost:** a second implementation of locomotion behind the same `Simulation` interface,
  selected per scene; worth it once targets go beyond ~50k agents.

## Consequences

- 50k agents step in ~10 ms on 20 threads (headless ~100 steps/s, 3× real time). 20k fits a
  60 FPS frame including rendering.
- Agent pool order is no longer spawn order. State hashes changed once with this ADR; they are
  still identical across runs and thread counts.
