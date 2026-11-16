#pragma once

/// @file AdsPanel.hpp
/// Ads panel: how well each ad in the scene is seen (metrics::AdAttention), as a sortable
/// table, a few headline numbers and the details of the selected ad. Clicking an ad
/// selects it in the scene; exporting and resetting are commands.

#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/ui/Theme.hpp"

namespace crowdsim::metrics {
class AdAttention;
}

namespace crowdsim::ui {

class AdsPanel {
public:
    /// Draw into the current window. `selected` is the scene selection (its details are
    /// shown when it is an ad). Returns the scene id of an ad the user clicked, or 0.
    std::uint32_t draw(const metrics::AdAttention& ads, double simTime, std::uint32_t selected,
                       const ThemeFonts& fonts, std::vector<nlohmann::json>& commands);

private:
    nlohmann::json report_;      ///< last summary (refreshed twice a simulated second)
    double reportTime_ = -1.0;
};

}  // namespace crowdsim::ui
