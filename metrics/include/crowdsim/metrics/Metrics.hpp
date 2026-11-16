#pragma once

/// @file Metrics.hpp
/// Crowd metrics and analysis, collected after every simulation step (a
/// read-only StepObserver, so it never affects determinism):
///  * gates: named line segments counting directional crossings and flow rate;
///  * a time series every 0.5 s: alive, arrivals, speed (mean / p50 / p90),
///    density (peak and p90 of occupied 1 m cells), stuck agents, gate totals;
///  * time-to-goal distribution (spawn -> goal arrival);
///  * stuck detection: agents trying to move but slower than 0.15 m/s for 10 s;
///  * with scene bodies: crowd pressure (peak, p90, agents above the critical level),
///    falls, and the time each agent has spent above the critical pressure;
///  * density, speed and peak-pressure heatmaps over the scene's walkable bounds;
///  * trajectory recording (id, type, time, position, velocity, state) with CSV export.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec2.hpp>
#include <nlohmann/json.hpp>

#include "crowdsim/sim/ScriptHooks.hpp"

namespace crowdsim::sim {
class Simulation;
}
namespace crowdsim::commands {
class CommandRegistry;
}

namespace crowdsim::metrics {

using Json = nlohmann::json;

/// Crowd pressure that is dangerous when sustained (N/m; about 1000 N/m for prolonged
/// exposure in the crowd-safety literature). Pressure metrics count agents at or above it.
inline constexpr float kCriticalPressure = 1000.0f;
/// Time above kCriticalPressure after which an agent counts as critically exposed.
inline constexpr float kCriticalSeconds = 30.0f;

struct Gate {
    std::string name;
    glm::vec2 a{0.0f};  ///< endpoints on the ground plane (x, z)
    glm::vec2 b{0.0f};
    float elevation = 0.0f;
    /// Crossings towards n = (-(b-a).z, (b-a).x), i.e. a->b turned 90 degrees (the side the
    /// editor's tick points to); `backward` counts the opposite direction.
    std::uint64_t forward = 0;
    std::uint64_t backward = 0;
};

struct Sample {
    double time = 0.0;
    std::uint32_t alive = 0;
    std::uint64_t arrived = 0;
    float meanSpeed = 0.0f;
    float p50Speed = 0.0f;
    float p90Speed = 0.0f;
    float peakDensity = 0.0f;  ///< agents / m² in the busiest 1 m cell
    float p90Density = 0.0f;   ///< 90th percentile over occupied cells
    std::uint32_t stuck = 0;
    float peakPressure = 0.0f;   ///< N/m, highest on any body
    float p90Pressure = 0.0f;    ///< N/m, 90th percentile over bodies under any pressure
    std::uint32_t critical = 0;  ///< agents at or above kCriticalPressure now
    std::uint32_t fallen = 0;    ///< agents on the ground now
    std::uint64_t falls = 0;     ///< falls so far
    std::vector<std::uint64_t> gateTotals;  ///< forward + backward per gate, cumulative
};

struct Heatmap {
    int width = 0;
    int height = 0;
    float cell = 1.0f;
    glm::vec2 origin{0.0f};  ///< (x, z) of cell (0, 0)'s corner
    float elevation = 0.0f;
    std::vector<float> agentSeconds;  ///< per cell
    std::vector<float> speedSeconds;  ///< per cell: sum of speed * dt
    std::vector<float> peakPressure;  ///< per cell: highest pressure seen, N/m
    std::vector<float> heightSeconds; ///< per cell: sum of floor height * dt
    double elapsed = 0.0;             ///< seconds accumulated
    std::uint64_t version = 0;        ///< bumps whenever the data changes

    /// Time-averaged agents per m² in a cell.
    [[nodiscard]] float density(int x, int z) const;
    /// Mean floor height agents stood on in a cell (`elevation` where none were): where
    /// to draw the cell on slopes and upper floors.
    [[nodiscard]] float floorHeight(int x, int z) const;
    /// Mean speed of agents seen in a cell (0 where none were).
    [[nodiscard]] float speed(int x, int z) const;
};

struct TrajectoryRow {
    std::uint32_t id;
    std::uint16_t type;
    std::uint16_t state;
    float time;
    float x, y, z;
    float vx, vz;
};

class MetricsCollector final : public sim::StepObserver {
public:
    explicit MetricsCollector(sim::Simulation& sim);
    ~MetricsCollector() override;
    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;

    void afterStep(sim::Simulation& sim) override;

    /// Clear accumulated data (gates keep their geometry, counts restart).
    void reset();

    // Gates
    void addGate(Gate gate);  ///< replaces a gate with the same name
    bool removeGate(const std::string& name);
    [[nodiscard]] const std::vector<Gate>& gates() const noexcept { return gates_; }

    // Recording
    void startRecording(double hz);
    void stopRecording() noexcept { recording_ = false; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] std::size_t recordedRows() const noexcept { return rows_.size(); }
    /// Write the recording as CSV; returns the number of rows.
    std::size_t exportTrajectories(const std::filesystem::path& file) const;
    /// Write the time series as CSV; returns the number of samples.
    std::size_t exportSeries(const std::filesystem::path& file) const;

    // Queries
    [[nodiscard]] const std::vector<Sample>& series() const noexcept { return series_; }
    [[nodiscard]] const Heatmap& heatmap() const noexcept { return heatmap_; }
    [[nodiscard]] const std::vector<float>& timeToGoal() const noexcept { return timeToGoal_; }
    [[nodiscard]] std::vector<std::uint32_t> stuckAgents() const;
    /// Current aggregates, gate counts and rates, time-to-goal statistics, stuck agents.
    [[nodiscard]] Json summary() const;
    /// Agents that have spent kCriticalSeconds above kCriticalPressure (including ones gone).
    [[nodiscard]] std::uint64_t criticallyExposed() const noexcept { return criticallyExposed_; }
    /// Heatmap ("density", "speed" or "pressure") downsampled to at most `maxCells` cells.
    [[nodiscard]] Json heatmapJson(const std::string& kind, std::size_t maxCells) const;

private:
    void ensureHeatmap();
    void takeSample();

    sim::Simulation& sim_;
    std::vector<Gate> gates_;
    std::vector<Sample> series_;
    Heatmap heatmap_;
    std::uint64_t heatmapStaticVersion_ = ~0ull;
    std::uint64_t lastStep_ = 0;

    std::unordered_map<std::uint32_t, double> spawnTime_;  ///< agent id -> spawn time
    std::unordered_map<std::uint32_t, float> slowFor_;     ///< agent id -> seconds below the stuck speed
    std::unordered_map<std::uint32_t, float> criticalFor_; ///< agent id -> seconds above kCriticalPressure
    std::uint64_t criticallyExposed_ = 0;                  ///< agents that reached kCriticalSeconds
    std::vector<float> timeToGoal_;

    bool recording_ = false;
    std::uint64_t recordStride_ = 5;
    std::vector<TrajectoryRow> rows_;
};

/// AddGate, RemoveGate, ListGates, GetMetrics, GetHeatmap, StartRecording,
/// StopRecording, ExportRecording, ExportMetrics, ResetMetrics.
void registerMetricsCommands(commands::CommandRegistry& registry, MetricsCollector& metrics);

}  // namespace crowdsim::metrics
