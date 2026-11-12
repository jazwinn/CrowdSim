# sim/ depends only on core (and header-only data libs). It must never link
# render/, ui/ or platform/ — that rule is what keeps headless mode GPU-free.
crowdsim_add_library(sim
  SOURCES
    src/Simulation.cpp
  DEPS
    CrowdSim::core
    EnTT::EnTT
)
