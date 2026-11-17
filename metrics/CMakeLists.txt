# metrics/: crowd metrics and analysis (gates, time series, time to goal, stuck
# detection, heatmaps, trajectory recording, visual attention) as read-only simulation
# observers, plus the commands that expose them.
crowdsim_add_library(metrics
  SOURCES
    src/Metrics.cpp
    src/MetricsCommands.cpp
    src/AttentionModel.cpp
  DEPS
    CrowdSim::core
    CrowdSim::sim
    CrowdSim::physics
    CrowdSim::assets
    CrowdSim::commands
    CrowdSim::reflect
    glm::glm
    nlohmann_json::nlohmann_json
)
