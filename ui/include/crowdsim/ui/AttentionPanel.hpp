#pragma once

/// @file AttentionPanel.hpp
/// Targets panel: how well each target in the scene is seen (metrics::AttentionModel), as a sortable
/// table, a few headline numbers and the details of the selected target. Clicking a target
/// selects it in the scene; exporting and resetting are commands.

#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/metrics/AttentionModel.hpp"
#include "crowdsim/ui/Theme.hpp"

namespace crowdsim::ui {

class AttentionPanel {
public:
    /// Draw into the current window. `selected` is the scene selection (its details are
    /// shown when it is a target). Returns the scene id of a target the user clicked, or 0.
    std::uint32_t draw(const metrics::AttentionModel& targets, double simTime, std::uint32_t selected,
                       const ThemeFonts& fonts, std::vector<nlohmann::json>& commands);

private:
    /// The model's settings, from their reflection descriptor; edits become SetAttentionParams.
    void drawSettings(const metrics::AttentionModel& model, const ThemeFonts& fonts,
                      std::vector<nlohmann::json>& commands);

    metrics::AttentionParams settings_;  ///< working copy while a setting is being edited
    nlohmann::json report_;      ///< last summary (refreshed twice a simulated second)
    double reportTime_ = -1.0;
};

}  // namespace crowdsim::ui
