# render/: bgfx renderer. Depends on core and snapshot (plain data) only;
# never on sim/, ui/ or platform/. Receives native window handles as plain data.
include(BgfxShaders)

crowdsim_add_library(render
  SOURCES
    src/Renderer.cpp
    src/ImGuiRenderer.cpp
    src/SceneRenderer.cpp
    src/ShaderLibrary.cpp
  DEPS
    CrowdSim::core
    CrowdSim::snapshot
    imgui::imgui
  PRIVATE_DEPS
    bgfx::bgfx
    bgfx::bimg
    bgfx::bx
)

set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
crowdsim_compile_shaders(crowdsim_render
  VARYING "${shader_dir}/imgui/varying.def.sc"
  VERTEX "${shader_dir}/imgui/vs_imgui.sc"
  FRAGMENT "${shader_dir}/imgui/fs_imgui.sc")
crowdsim_compile_shaders(crowdsim_render
  VARYING "${shader_dir}/scene/varying.def.sc"
  VERTEX "${shader_dir}/scene/vs_agent.sc" "${shader_dir}/scene/vs_world.sc"
  FRAGMENT "${shader_dir}/scene/fs_agent.sc" "${shader_dir}/scene/fs_ground.sc" "${shader_dir}/scene/fs_flat.sc")
crowdsim_compile_shaders(crowdsim_render
  VARYING "${shader_dir}/mesh/varying.def.sc"
  VERTEX "${shader_dir}/mesh/vs_mesh.sc" "${shader_dir}/mesh/vs_line.sc"
  FRAGMENT "${shader_dir}/mesh/fs_mesh.sc" "${shader_dir}/mesh/fs_line.sc")
