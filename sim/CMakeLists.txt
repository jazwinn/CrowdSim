# sim/ depends only on core, snapshot (plain data) and header-only libs. It must
# never link render/, ui/ or platform/ — that rule keeps headless mode GPU-free.
crowdsim_add_library(sim
  SOURCES
    src/Simulation.cpp
    src/SpatialHash.cpp
    src/Orca.cpp
    src/Scenarios.cpp
  DEPS
    CrowdSim::core
    CrowdSim::snapshot
    EnTT::EnTT
)
