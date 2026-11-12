# ADR 0005: Navigation — tiled navmesh, polygon flow fields, constrained locomotion

- Status: accepted
- Date: 2026-11-12

## Context

M2 requires multi-level navigation (floors, ramps, stairs, off-mesh links), per-goal flow fields
shared by all agents, navmesh-constrained movement with no wall penetration, and 5k agents at
60 FPS. The spec forbids DetourCrowd for agent simulation.

## Decision

- **Geometry → navmesh.** Scene geometry is a `geometry::TriangleMesh` with per-triangle flags
  (`walkable`, `obstacle`). Obstacle triangles get `RC_NULL_AREA`; walkable triangles keep
  `RC_WALKABLE_AREA` if their slope allows. Recast builds a **tiled** navmesh (64-cell tiles,
  0.2 m cells, 0.3 m agent radius erosion, 0.45 m climb, 45° slope). Tiles are rasterised in
  parallel on the job system and added to the `dtNavMesh` in index order → deterministic.
  Stairs need no special handling: 0.2 m steps are within the climb height. Multi-level floors fall
  out of Recast's layered heightfield.
- **Off-mesh links** (ladders, escalators) are Detour off-mesh connections with an `extraCost`
  (default 25 m) so flow fields only route through them when they save a real detour; without it,
  everyone near a ladder queued for it.
- **Flow fields over polygons, not a grid.** Per goal, Dijkstra from the polygons inside the goal
  area over the *reverse* polygon adjacency (so one-way links are respected) gives each polygon a
  cost-to-goal and a `next` polygon plus the portal into it. Each agent string-pulls a short
  corridor (≤ 8 portals) along `next` with the simple-stupid-funnel algorithm to find its steering
  corner. Portals are shrunk by 0.2 m at each end so corners lie inside free space rather than on
  boundary vertices; a corner is skipped only once it is reached (≤ 0.1 m). Both were needed to
  stop agents stalling at corners.
- **Locomotion with the navmesh.** Agents keep their Detour polygon. Each step the
  acceleration-limited velocity is applied with `dtNavMeshQuery::moveAlongSurface` (one query
  object per worker thread) and the height is taken from the detail mesh. If the move is clipped,
  the velocity becomes the actual displacement, so agents slide along walls. **This is the hard
  guarantee against wall penetration.**
- **Walls in ORCA.** Navmesh boundary edges are indexed in a uniform grid. Up to 8 nearest edges
  on the agent's level become hard ORCA half-planes (`v·n ≤ gap / τ_obstacle`, τ = 0.5 s). Because
  the navmesh is already eroded by the agent radius, the gap is measured to a 5 cm clearance, not
  the agent radius (using the radius double-counted erosion and pinned agents at corners).
  Agents already within the clearance get a "no further approach" constraint, never a push-out.
- **Levels.** Neighbours and walls more than 1 m apart vertically are ignored, so agents on the
  floor above do not avoid agents below.
- **Lazy rebuild.** Adding static objects/links marks the navmesh dirty; adding goals marks flow
  fields dirty. Both rebuild at the next step (or explicitly via `Simulation::updateNavigation`,
  which the app calls after scene changes and times). A `staticVersion` counter tells the
  renderer and the physics world when to refresh.
- **Physics.** Jolt holds the same static mesh for raycasts (editor picking now; line of sight and
  dynamic obstacles later). Agents are not physics bodies. Jolt's factory/type registry is a
  library global, owned by a single `physics::JoltRuntime` in the composition root (ADR 0002).

## Consequences

- No per-agent path queries in the common case: cost is one corridor walk + funnel per agent.
- Flow fields are polygon-granular, so very large polygons give coarse arrows; the funnel hides
  this for steering. A grid flow field can be added later if density-aware routing needs it.
- `dtNavMeshQuery::getPortalPoints` is private in Recast 1.6; `NavMesh::portal` re-implements it
  from public tile data.
- Incremental (per-tile) rebuild is prepared by the tiled layout but not yet wired (M3 editor).
