# commands/: the single path for every state mutation (GUI, Lua, MCP, replay).
crowdsim_add_library(commands
  SOURCES
    src/CommandRegistry.cpp
    src/CommandDispatcher.cpp
    src/BuiltinCommands.cpp
  DEPS
    CrowdSim::core
    CrowdSim::sim
    CrowdSim::assets
    nlohmann_json::nlohmann_json
)
