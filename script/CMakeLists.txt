# script/: the Lua scripting domain (LuaJIT + sol2): sandboxed state, API
# bindings, script BT nodes, entity types and script commands.
include(LuaJIT)
find_package(sol2 CONFIG REQUIRED)

crowdsim_add_library(script
  SOURCES
    src/LuaJson.cpp
    src/ScriptCommands.cpp
    src/ScriptHost.cpp
  DEPS
    CrowdSim::core
    CrowdSim::sim
    CrowdSim::assets
    CrowdSim::commands
  PRIVATE_DEPS
    luajit::luajit
    sol2::sol2
)
# sol2 on LuaJIT, with argument/type checks at the script boundary. sol2 catches C++
# exceptions itself so they reach Lua as errors with their message.
target_compile_definitions(crowdsim_script PRIVATE SOL_LUAJIT=1 SOL_ALL_SAFETIES_ON=1
                                                   SOL_EXCEPTIONS_SAFE_PROPAGATION=0 SOL_PRINT_ERRORS=0)
if(MSVC)
  target_compile_options(crowdsim_script PRIVATE /bigobj)
endif()
