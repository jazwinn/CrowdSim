# Build a scene from a command batch (as an AI agent would), save the command
# log, replay it in a fresh process and require the identical final state hash.
set(log "${WORKDIR}/replay_test.cmdlog.json")
execute_process(
  COMMAND "${EXE}" --commands "${COMMANDS}" --steps 0 --save-log "${log}" --log-level warn
  OUTPUT_VARIABLE run_out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "command batch failed (${rc}):\n${run_out}")
endif()
if(run_out MATCHES "\"ok\":false")
  message(FATAL_ERROR "a command failed:\n${run_out}")
endif()
string(REGEX MATCH "\"state_hash\":\"[0-9a-f]+\"" original "${run_out}")

execute_process(COMMAND "${EXE}" --replay "${log}" --log-level warn OUTPUT_VARIABLE replay_out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "replay failed (${rc}):\n${replay_out}")
endif()
string(REGEX MATCH "\"state_hash\":\"[0-9a-f]+\"" replayed "${replay_out}")

if(NOT original OR NOT original STREQUAL replayed)
  message(FATAL_ERROR "replay diverged: original ${original}, replay ${replayed}")
endif()
message(STATUS "replay identical: ${original}")
