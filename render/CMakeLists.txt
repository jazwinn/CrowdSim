# render/: bgfx renderer. Depends on core only (plus snapshot types from M1);
# never on sim/, ui/ or platform/. Receives native window handles as plain data.
include(BgfxShaders)

crowdsim_add_library(render
  SOURCES
    src/Renderer.cpp
    src/ImGuiRenderer.cpp
    src/ShaderLibrary.cpp
  DEPS
    CrowdSim::core
    imgui::imgui
  PRIVATE_DEPS
    bgfx::bgfx
    bgfx::bimg
    bgfx::bx
)

crowdsim_compile_shaders(crowdsim_render
  VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/imgui/varying.def.sc"
  VERTEX "${CMAKE_CURRENT_SOURCE_DIR}/shaders/imgui/vs_imgui.sc"
  FRAGMENT "${CMAKE_CURRENT_SOURCE_DIR}/shaders/imgui/fs_imgui.sc")
