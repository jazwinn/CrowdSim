#pragma once

/// @file MetricsPanel.hpp
/// Metrics panel: live ImPlot plots (crowd size, arrivals, speed, density, gate
/// totals, time-to-goal histogram), gate management and recording / export
/// buttons. Reads a MetricsCollector; every action is a command.

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/ui/Theme.hpp"

namespace crowdsim::metrics {
class MetricsCollector;
}

namespace crowdsim::ui {

class MetricsPanel {
public:
    /// Registers an imgui.ini section ("[CrowdSimCharts]") that keeps the chart colours
    /// and hidden series between sessions. Needs a current ImGui context, before the
    /// first NewFrame (when the .ini is read).
    MetricsPanel();
    ~MetricsPanel();
    MetricsPanel(const MetricsPanel&) = delete;
    MetricsPanel& operator=(const MetricsPanel&) = delete;

    /// Draw into the current window. `gatePoint` = where a new gate is centred (x, z).
    void draw(const metrics::MetricsCollector& metrics, const float gatePoint[2], const ThemeFonts& fonts,
              std::vector<nlohmann::json>& commands);

private:
    /// A line series whose legend entry opens a colour / visibility popup when clicked.
    void line(const char* plot, const char* label, const double* x, const double* y, int n);
    /// The legend popup for the last plotted item `label` of `plot`.
    void legendMenu(const char* plot, const char* label);
    void settingsChanged() const;

public:  // imgui.ini persistence (called from the settings handler)
    void readSetting(const char* line);
    [[nodiscard]] std::string writeSettings() const;

private:
    std::map<std::string, std::array<float, 4>> seriesColors_;  ///< "plot/label" -> RGBA set by the user
    std::set<std::string> hiddenSeries_;                        ///< "plot/label" hidden by the user
    char exportPath_[128] = "recordings/trajectories.csv";
    char gateName_[32] = "gate1";
    float gateA_[2] = {0.0f, -3.0f};
    float gateB_[2] = {0.0f, 3.0f};
    float recordHz_ = 6.0f;
};

}  // namespace crowdsim::ui
