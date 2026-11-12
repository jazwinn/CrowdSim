# mcp/: Model Context Protocol server (JSON-RPC 2.0) with stdio and Streamable
# HTTP transports, and the CrowdSim tools/resources mapped onto commands.
find_package(httplib CONFIG REQUIRED)

crowdsim_add_library(mcp
  SOURCES
    src/CrowdSimTools.cpp
    src/McpServer.cpp
    src/Transports.cpp
  DEPS
    CrowdSim::core
    CrowdSim::sim
    CrowdSim::assets
    CrowdSim::commands
  PRIVATE_DEPS
    httplib::httplib
)
