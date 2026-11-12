# Helpers shared by every CrowdSim target.

# Apply the project warning policy to a target. Only project code gets this;
# third-party headers come in through IMPORTED targets and are treated as
# SYSTEM includes, so their warnings never fail our build.
function(crowdsim_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
                                             /external:anglebrackets /external:W0)
    if(CROWDSIM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                             -Wno-sign-conversion -Wnon-virtual-dtor
                                             # Aggregates and designated initializers deliberately rely on
                                             # default member initializers for omitted fields.
                                             -Wno-missing-field-initializers)
    if(CROWDSIM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Declare a project library: crowdsim_add_library(<name> SOURCES ... DEPS ... PRIVATE_DEPS ...)
# Creates target crowdsim_<name> with alias CrowdSim::<name>. Public headers live in
# <module>/include/crowdsim/<name>/.
function(crowdsim_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
  set(target crowdsim_${name})
  add_library(${target} STATIC ${ARG_SOURCES})
  add_library(CrowdSim::${name} ALIAS ${target})
  target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                       PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(${target} PUBLIC ${ARG_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
  target_compile_features(${target} PUBLIC cxx_std_20)
  crowdsim_set_warnings(${target})
endfunction()
