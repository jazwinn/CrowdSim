add_executable(crowdsim_tests
  main.cpp
  core/test_random.cpp
  core/test_fixed_step_clock.cpp
  core/test_job_system.cpp
  core/test_logging.cpp
  core/test_triple_buffer.cpp
  sim/test_determinism.cpp
  sim/test_spatial_hash.cpp
  sim/test_orca.cpp
  sim/test_navigation.cpp
  sim/test_behavior.cpp
  animation/test_animation.cpp
  script/test_scripting.cpp
  assets/test_bt_graph.cpp
  mcp/test_mcp.cpp
  physics/test_physics.cpp
  commands/test_commands.cpp
  app/test_command_line.cpp
)
target_link_libraries(crowdsim_tests PRIVATE crowdsim_app_common CrowdSim::animation doctest::doctest)
crowdsim_set_warnings(crowdsim_tests)

add_test(NAME unit COMMAND crowdsim_tests)

# Smoke tests: the binaries start, parse arguments and exit cleanly.
add_test(NAME headless_smoke COMMAND crowdsim_headless --steps 120 --seed 42 --agents 500 --log-level warn)
set_tests_properties(headless_smoke PROPERTIES PASS_REGULAR_EXPRESSION "\"steps\":120")

# Headless determinism across processes: same seed -> same hash, checked by a CMake script.
add_test(NAME headless_determinism
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:crowdsim_headless>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/determinism.cmake)

# Scenario test: a 2,000-agent stream must fully arrive, i.e. nobody gets stuck.
add_test(NAME scenario_stream_all_arrive
  COMMAND crowdsim_headless --scenario stream --agents 2000 --steps 4500 --log-level warn)
set_tests_properties(scenario_stream_all_arrive PROPERTIES
  PASS_REGULAR_EXPRESSION "\"alive\":0,.*\"spawned\":2000,\"arrived\":2000")

if(CROWDSIM_BUILD_GUI)
  # Needs a display (CI provides one via xvfb on Linux); excluded with -LE gui.
  add_test(NAME gui_smoke COMMAND crowdsim --frames 30 --log-level warn)
  set_tests_properties(gui_smoke PROPERTIES LABELS gui TIMEOUT 60)
endif()

# M2 scenario test: two-floor building, everyone reaches the upper floor (no stuck agents).
add_test(NAME scenario_building_all_arrive
  COMMAND crowdsim_headless --scenario building --agents 1000 --steps 9000 --log-level warn)
set_tests_properties(scenario_building_all_arrive PROPERTIES
  PASS_REGULAR_EXPRESSION "\"alive\":0,.*\"spawned\":1000,\"arrived\":1000")

# M3: a scene built by commands replays to the identical state in a fresh process.
add_test(NAME commands_replay_identical
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:crowdsim_headless>
          -DCOMMANDS=${CMAKE_SOURCE_DIR}/content/commands/build_corridor.commands.json
          -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/replay.cmake)

# M4: the plaza (commuters, wanderers, queuers at kiosks, groups) fully drains.
add_test(NAME scenario_plaza_all_leave
  COMMAND crowdsim_headless --scenario plaza --agents 2000 --steps 15000 --log-level warn)
set_tests_properties(scenario_plaza_all_leave PROPERTIES
  PASS_REGULAR_EXPRESSION "\"alive\":0,.*\"spawned\":2000")

# M8: an MCP client builds a scene, defines two agent types with trees, runs 60 s and reads metrics.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME mcp_stdio_workflow
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mcp_workflow_test.py $<TARGET_FILE:crowdsim_headless>)
  set_tests_properties(mcp_stdio_workflow PROPERTIES PASS_REGULAR_EXPRESSION "\"ok\": true" TIMEOUT 120)
endif()
