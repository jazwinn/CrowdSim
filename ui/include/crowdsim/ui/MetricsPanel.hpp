#pragma once

/// @file MetricsPanel.hpp
/// Metrics panel: live ImPlot plots (crowd size, arrivals, speed, density, gate
/// totals, time-to-goal histogram), gate management and recording / export
/// buttons. Reads a MetricsCollector; every action is a command.

#include <vector>

#include <nlohmann/json.hpp>

namespace crowdsim::metrics {
class MetricsCollector;
}

namespace crowdsim::ui {

class MetricsPanel {
public:
    /// Draw into the current window. `gatePoint` = where a new gate is centred (x, z).
    void draw(const metrics::MetricsCollector& metrics, const float gatePoint[2],
              std::vector<nlohmann::json>& commands);

private:
    char exportPath_[128] = "recordings/trajectories.csv";
    char gateName_[32] = "gate1";
    float gateA_[2] = {0.0f, -3.0f};
    float gateB_[2] = {0.0f, 3.0f};
    float recordHz_ = 6.0f;
};

}  // namespace crowdsim::ui
