crowdsim_add_library(core
  SOURCES
    src/Logging.cpp
    src/JobSystem.cpp
  DEPS
    spdlog::spdlog
    glm::glm
    enkiTS::enkiTS
)

if(CROWDSIM_ENABLE_TRACY)
  target_link_libraries(crowdsim_core PUBLIC Tracy::TracyClient)
  target_compile_definitions(crowdsim_core PUBLIC CROWDSIM_TRACY=1)
endif()

# spdlog: never use the default/global registry from project code.
target_compile_definitions(crowdsim_core PUBLIC SPDLOG_NO_REGISTRY_MUTEX GLM_FORCE_DEPTH_ZERO_TO_ONE)
