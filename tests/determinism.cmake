# Runs the headless binary twice with the same seed and once with a different seed;
# the first two state hashes must match and the third must differ.
foreach(run a b)
  execute_process(COMMAND "${EXE}" --steps 500 --seed 7 --log-level off
                  OUTPUT_VARIABLE out_${run} RESULT_VARIABLE rc_${run})
  if(NOT rc_${run} EQUAL 0)
    message(FATAL_ERROR "headless run ${run} failed (${rc_${run}})")
  endif()
endforeach()
execute_process(COMMAND "${EXE}" --steps 500 --seed 8 --log-level off OUTPUT_VARIABLE out_c)

string(REGEX MATCH "\"state_hash\":\"[0-9a-f]+\"" hash_a "${out_a}")
string(REGEX MATCH "\"state_hash\":\"[0-9a-f]+\"" hash_b "${out_b}")
string(REGEX MATCH "\"state_hash\":\"[0-9a-f]+\"" hash_c "${out_c}")

if(NOT hash_a OR NOT hash_a STREQUAL hash_b)
  message(FATAL_ERROR "non-deterministic: '${hash_a}' vs '${hash_b}'")
endif()
if(hash_a STREQUAL hash_c)
  message(FATAL_ERROR "different seeds produced the same hash '${hash_a}'")
endif()
message(STATUS "deterministic: ${hash_a}")
