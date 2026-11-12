# ADR 0004: Agent storage and the M1 locomotion pipeline

- Status: accepted
- Date: 2026-11-12

## Context

The spec asks for an EnTT world and SoA-friendly, job-parallel systems that scale from 5k to 50k+
agents with deterministic results. EnTT stores components in paged sparse sets: iteration is fast,
but indexing "agent i" across several components from many threads (needed for neighbour queries
and ORCA, which read *other* agents by index) is awkward and not contiguous.

## Decision

- **Agents live in `sim::AgentPool`**, a structure-of-arrays with one `std::vector` per column
  (position, velocity, radius, goal, colour, ...). All hot loops index these arrays directly.
  Removal is swap-and-pop in descending index order, so the layout after removals is deterministic.
- **Environment entities live in the EnTT registry** (`Spawner`, `Goal` components now; scene
  objects, gates and so on later). They are few, heterogeneous, and benefit from ECS flexibility.
  Agents can get an EnTT handle later if per-agent extension components are needed; the pool
  stays the hot-path storage.
- **Step order** (`Simulation::step`): build spatial hash → spawn (rejection-sampled against the
  hash) → rebuild hash if anyone spawned → preferred velocity (parallel) → neighbour search + ORCA
  (parallel) → acceleration-limited integration and arrival flags (parallel) → despawn (serial).
- **Spatial hash**: hashed uniform grid, cell size = neighbour distance, rebuilt every step with a
  counting sort whose scatter runs in index order. Queries dedupe colliding buckets and allocate
  nothing.
- **ORCA**: agent–agent ORCA following van den Berg et al. (RVO2's formulation), written from the
  paper/algorithm, on fixed-capacity arrays (≤ 16 neighbours, K nearest kept with
  `(distance, index)` ordering so ties are deterministic). A small *personal space* margin is
  added to radii during avoidance instead of a separate social-force term for now.
- **Determinism**: per-agent work in parallel reads the previous state and writes only its own
  slot; everything order-dependent (hash scatter, spawning, despawning, RNG use) is serial. Results
  are bit-identical across thread counts (tested with 1, 3, 7 and 20 threads).
- **Render hand-off**: `Simulation::writeSnapshot` fills a `snapshot::RenderSnapshot` (in the
  header-only `snapshot/` module) containing each agent's previous and current position/heading,
  published through a lock-free `core::TripleBuffer`. The renderer interpolates with the fixed-step
  clock's alpha and never touches the simulation.

## Consequences

- Adding a per-agent attribute means adding a column (in one place: `AgentPool::forEachColumn`).
- Ground-plane 2D positions for now; M2 adds floor height / navmesh polygon per agent.
- Known limitation: dense head-on counterflow can jam for a while (ORCA symmetry). Behaviour-level
  fixes (keep-right bias, lane formation) belong to M4.
