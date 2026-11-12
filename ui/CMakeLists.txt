# ui/: ImGui editor panels. Knows nothing about bgfx or SDL; it reads plain
# data structs and returns requested actions for the app to apply.
crowdsim_add_library(ui
  SOURCES
    src/AgentDebugger.cpp
    src/BtEditor.cpp
    src/EditorShell.cpp
    src/SceneEditor.cpp
  DEPS
    CrowdSim::core
    imgui::imgui
    imguizmo::imguizmo
    unofficial::imgui-node-editor::imgui-node-editor
    CrowdSim::sim
    CrowdSim::assets
    CrowdSim::commands
)
