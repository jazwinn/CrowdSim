#pragma once

/// @file BtEditor.hpp
/// Behaviour-tree node graph editor (imgui-node-editor). Edits an
/// assets::BtGraph; every change to the simulation goes out as a command:
/// GetBehaviorTree (open), SetBehaviorTree (apply = hot reload into running
/// agents) and SaveBehaviorTree (write content/behaviours/<name>.bt.json with
/// the layout). Shows validation problems on the nodes and highlights the
/// selected agent's running / last-visited nodes live.

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/assets/BtGraph.hpp"

namespace crowdsim::sim {
class Simulation;
}

namespace crowdsim::ui {

class BtEditor {
public:
    BtEditor();
    ~BtEditor();
    BtEditor(const BtEditor&) = delete;
    BtEditor& operator=(const BtEditor&) = delete;

    /// Draw into the current window. `selectedAgent` (or 0xFFFFFFFF) drives highlighting.
    void draw(const sim::Simulation& sim, std::uint32_t selectedAgent, std::vector<nlohmann::json>& commands);

    /// Load a *.bt.json document (e.g. a GetBehaviorTree result).
    void open(const nlohmann::json& doc);
    /// Feedback line under the toolbar (results of Apply / Save).
    void setStatus(std::string text, bool error);

    [[nodiscard]] const assets::BtGraph& graph() const noexcept { return graph_; }

private:
    void toolbar(const sim::Simulation& sim, std::uint32_t selectedAgent, std::vector<nlohmann::json>& commands);
    void canvas(const sim::Simulation& sim, std::uint32_t selectedAgent);
    void newGraph();

    struct Context;
    std::unique_ptr<Context> ctx_;
    assets::BtGraph graph_;
    std::set<int> needsPlacement_;  ///< graph nodes whose position must be pushed to the canvas
    int navigateFrames_ = 0;        ///< frames left to zoom the canvas to fit the graph
    std::string status_;
    bool statusError_ = false;
    char nameBuffer_[64] = {};
};

}  // namespace crowdsim::ui
