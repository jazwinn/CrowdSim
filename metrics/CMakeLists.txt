# metrics/: crowd metrics and analysis (gates, time series, time to goal, stuck
# detection, heatmaps, trajectory recording) as a read-only simulation observer,
# plus the commands that expose it.
crowdsim_add_library(metrics
  SOURCES
    src/Metrics.cpp
    src/MetricsCommands.cpp
  DEPS
    CrowdSim::core
    CrowdSim::sim
    CrowdSim::assets
    CrowdSim::commands
    glm::glm
    nlohmann_json::nlohmann_json
)
