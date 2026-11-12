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
  app/test_command_line.cpp
)
target_link_libraries(crowdsim_tests PRIVATE crowdsim_app_common doctest::doctest)
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
