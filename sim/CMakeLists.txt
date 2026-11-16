# sim/ depends only on core, geometry, snapshot (plain data) and header-only /
# navigation libs. It must never link render/, ui/ or platform/ — that rule
# keeps headless mode GPU-free.
crowdsim_add_library(sim
  SOURCES
    src/Simulation.cpp
    src/SpatialHash.cpp
    src/Orca.cpp
    src/NavMesh.cpp
    src/FlowField.cpp
    src/Scenarios.cpp
    src/BehaviorTree.cpp
    src/Decisions.cpp
    src/Escalators.cpp
    src/QueueManners.cpp
    src/Bodies.cpp
  DEPS
    CrowdSim::core
    CrowdSim::geometry
    CrowdSim::snapshot
    EnTT::EnTT
  PRIVATE_DEPS
    RecastNavigation::Recast
    RecastNavigation::Detour
)
