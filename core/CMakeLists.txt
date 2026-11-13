crowdsim_add_library(core
  SOURCES
    src/Logging.cpp
    src/JobSystem.cpp
    src/VideoWriter.cpp
  DEPS
    spdlog::spdlog
    glm::glm
    enkiTS::enkiTS
)

# stb_image_write (header-only) encodes video frames as JPEG.
find_path(STB_INCLUDE_DIRS "stb_image_write.h" REQUIRED)
target_include_directories(crowdsim_core SYSTEM PRIVATE "${STB_INCLUDE_DIRS}")

if(CROWDSIM_ENABLE_TRACY)
  target_link_libraries(crowdsim_core PUBLIC Tracy::TracyClient)
  target_compile_definitions(crowdsim_core PUBLIC CROWDSIM_TRACY=1)
endif()

# spdlog: never use the default/global registry from project code.
target_compile_definitions(crowdsim_core PUBLIC SPDLOG_NO_REGISTRY_MUTEX GLM_FORCE_DEPTH_ZERO_TO_ONE)
