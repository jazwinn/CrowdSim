cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Options. GUI/Tracy map onto vcpkg manifest features, so they must be decided
# before project() runs the vcpkg toolchain.
# ---------------------------------------------------------------------------
option(CROWDSIM_BUILD_GUI "Build the windowed editor (SDL3 + bgfx + ImGui)" ON)
option(CROWDSIM_ENABLE_TRACY "Compile in the Tracy profiler client" OFF)
option(CROWDSIM_BUILD_TESTS "Build unit and integration tests" ON)
option(CROWDSIM_BUILD_BENCH "Build benchmarks" ON)
option(CROWDSIM_WARNINGS_AS_ERRORS "Treat warnings in project code as errors" ON)

set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON)
if(CROWDSIM_BUILD_GUI)
  list(APPEND VCPKG_MANIFEST_FEATURES "gui")
endif()
if(CROWDSIM_ENABLE_TRACY)
  list(APPEND VCPKG_MANIFEST_FEATURES "tracy")
endif()

project(CrowdSim VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
  enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CrowdSimTargets)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Third-party packages (vcpkg manifest mode)
# ---------------------------------------------------------------------------
find_package(spdlog CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(enkiTS CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(recastnavigation CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_path(CGLTF_INCLUDE_DIR cgltf.h REQUIRED)
include(Ozz)  # ozz-animation (FetchContent, pinned)
if(CROWDSIM_BUILD_GUI)
  find_package(imguizmo CONFIG REQUIRED)
  find_package(unofficial-imgui-node-editor CONFIG REQUIRED)
endif()

if(CROWDSIM_ENABLE_TRACY)
  find_package(Tracy CONFIG REQUIRED)
endif()

if(CROWDSIM_BUILD_GUI)
  find_package(SDL3 CONFIG REQUIRED)
  find_package(bgfx CONFIG REQUIRED)
  find_package(imgui CONFIG REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Modules. Dependency direction is enforced by target_link_libraries:
#   core <- physics, sim <- script, commands <- mcp ; render/ui/platform <- app
# sim never links render/ui/platform and vice versa.
# ---------------------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(geometry)
add_subdirectory(snapshot)
add_subdirectory(sim)
add_subdirectory(physics)
add_subdirectory(assets)
add_subdirectory(commands)
add_subdirectory(animation)
add_subdirectory(script)

if(CROWDSIM_BUILD_GUI)
  add_subdirectory(platform)
  add_subdirectory(render)
  add_subdirectory(ui)
endif()

add_subdirectory(app)

if(CROWDSIM_BUILD_TESTS)
  enable_testing()
  find_package(doctest CONFIG REQUIRED)
  add_subdirectory(tests)
endif()

if(CROWDSIM_BUILD_BENCH)
  find_package(benchmark CONFIG REQUIRED)
  add_subdirectory(bench)
endif()
