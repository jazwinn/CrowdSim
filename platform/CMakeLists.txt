# platform/: OS window, input and event pump (SDL3). Used only by the GUI app.
crowdsim_add_library(platform
  SOURCES
    src/Window.cpp
  DEPS
    CrowdSim::core
    SDL3::SDL3
)
