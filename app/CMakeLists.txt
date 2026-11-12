# app/ is the composition root: the only place that knows every concrete type.

add_library(crowdsim_app_common STATIC
  src/AppConfig.cpp
  src/ContentWatcher.cpp
  src/EngineContext.cpp
)
target_include_directories(crowdsim_app_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(crowdsim_app_common PUBLIC CrowdSim::core CrowdSim::sim CrowdSim::physics CrowdSim::assets
                                                  CrowdSim::commands CrowdSim::script)
target_compile_definitions(crowdsim_app_common PUBLIC CROWDSIM_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content")
target_compile_definitions(crowdsim_app_common PUBLIC CROWDSIM_VERSION="${PROJECT_VERSION}")
crowdsim_set_warnings(crowdsim_app_common)

# Headless executable: never links SDL, bgfx or ImGui.
add_executable(crowdsim_headless src/headless_main.cpp)
target_link_libraries(crowdsim_headless PRIVATE crowdsim_app_common)
crowdsim_set_warnings(crowdsim_headless)

if(CROWDSIM_BUILD_GUI)
  add_executable(crowdsim src/gui_main.cpp src/GuiApp.cpp)
  target_link_libraries(crowdsim PRIVATE crowdsim_app_common CrowdSim::platform CrowdSim::render CrowdSim::ui
                                         CrowdSim::animation)
  crowdsim_set_warnings(crowdsim)
  if(WIN32)
    # Keep a console so logs are visible; switch to WIN32 subsystem for release packaging later.
    set_target_properties(crowdsim PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  endif()
endif()
