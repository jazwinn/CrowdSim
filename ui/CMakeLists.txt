# ui/: ImGui editor panels. Knows nothing about bgfx or SDL; it reads plain
# data structs and returns requested actions for the app to apply.
crowdsim_add_library(ui
  SOURCES
    src/EditorShell.cpp
  DEPS
    CrowdSim::core
    imgui::imgui
)
