#include "crowdsim/metrics/Metrics.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::metrics {

namespace {

constexpr std::uint64_t kSampleStride = 15;  ///< steps between samples (0.5 s at 30 Hz)
constexpr float kStuckSpeed = 0.15f;
constexpr float kStuckSeconds = 10.0f;
constexpr std::size_t kMaxSamples = 40000;
constexpr std::size_t kMaxRows = 10'000'000;
constexpr int kMaxHeatmapCells = 300;        ///< per axis
constexpr std::uint64_t kHeatmapStride = 3;  ///< steps between heatmap accumulations

float cross(glm::vec2 a, glm::vec2 b) {
    return a.x * b.y - a.y * b.x;
}

float percentile(std::vector<float>& v, float q) {
    if (v.empty()) {
        return 0.0f;
    }
    const auto k = static_cast<std::size_t>(q * static_cast<float>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

double round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

}  // namespace

float Heatmap::density(int x, int z) const {
    const auto i = static_cast<std::size_t>(z * width + x);
    return elapsed > 0.0 ? static_cast<float>(agentSeconds[i] / (cell * cell * elapsed)) : 0.0f;
}

float Heatmap::speed(int x, int z) const {
    const auto i = static_cast<std::size_t>(z * width + x);
    return agentSeconds[i] > 0.0f ? speedSeconds[i] / agentSeconds[i] : 0.0f;
}

MetricsCollector::MetricsCollector(sim::Simulation& sim) : sim_(sim) {
    sim_.addStepObserver(this);
}

MetricsCollector::~MetricsCollector() {
    sim_.removeStepObserver(this);
}

void MetricsCollector::reset() {
    for (Gate& g : gates_) {
        g.forward = 0;
        g.backward = 0;
    }
    series_.clear();
    std::fill(heatmap_.agentSeconds.begin(), heatmap_.agentSeconds.end(), 0.0f);
    std::fill(heatmap_.speedSeconds.begin(), heatmap_.speedSeconds.end(), 0.0f);
    heatmap_.elapsed = 0.0;
    ++heatmap_.version;
    spawnTime_.clear();
    slowFor_.clear();
    timeToGoal_.clear();
    rows_.clear();
}

void MetricsCollector::addGate(Gate gate) {
    gate.forward = 0;
    gate.backward = 0;
    for (Gate& g : gates_) {
        if (g.name == gate.name) {
            g = std::move(gate);
            return;
        }
    }
    gates_.push_back(std::move(gate));
}

bool MetricsCollector::removeGate(const std::string& name) {
    return std::erase_if(gates_, [&](const Gate& g) { return g.name == name; }) > 0;
}

void MetricsCollector::startRecording(double hz) {
    const double stepHz = sim_.config().stepHz;
    recordStride_ = static_cast<std::uint64_t>(std::max(1.0, std::round(stepHz / std::clamp(hz, 0.1, stepHz))));
    recording_ = true;
}

void MetricsCollector::ensureHeatmap() {
    if (sim_.staticVersion() == heatmapStaticVersion_) {
        return;
    }
    heatmapStaticVersion_ = sim_.staticVersion();
    const auto bounds = sim_.staticMesh().bounds();
    Heatmap h;
    h.version = heatmap_.version + 1;
    if (bounds.valid()) {
        const float ex = bounds.max.x - bounds.min.x;
        const float ez = bounds.max.z - bounds.min.z;
        h.cell = std::max(1.0f, std::max(ex, ez) / static_cast<float>(kMaxHeatmapCells));
        h.width = std::max(1, static_cast<int>(std::ceil(ex / h.cell)));
        h.height = std::max(1, static_cast<int>(std::ceil(ez / h.cell)));
        h.origin = {bounds.min.x, bounds.min.z};
        h.agentSeconds.assign(static_cast<std::size_t>(h.width * h.height), 0.0f);
        h.speedSeconds.assign(h.agentSeconds.size(), 0.0f);
    }
    heatmap_ = std::move(h);
}

void MetricsCollector::afterStep(sim::Simulation& sim) {
    if (sim.stepIndex() < lastStep_) {
        reset();  // ResetSimulation / scene replaced: start over
    }
    lastStep_ = sim.stepIndex();
    const auto dt = static_cast<float>(sim.stepSeconds());
    const double now = sim.simTimeSeconds();
    const sim::AgentPool& a = sim.agents();

    // Events: time to goal.
    for (const sim::SimEvent& e : sim.stepEvents()) {
        switch (e.type) {
            case sim::SimEventType::AgentSpawned:
                spawnTime_[e.agentId] = now;
                break;
            case sim::SimEventType::AgentArrived:
                if (const auto it = spawnTime_.find(e.agentId); it != spawnTime_.end()) {
                    timeToGoal_.push_back(static_cast<float>(now - it->second));
                }
                break;
            case sim::SimEventType::AgentDespawned:
                spawnTime_.erase(e.agentId);
                slowFor_.erase(e.agentId);
                break;
        }
    }

    // Gates: segment crossings of each agent's last move.
    for (Gate& g : gates_) {
        const glm::vec2 e = g.b - g.a;
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            if (std::abs(a.height[i] - g.elevation) > 1.5f) {
                continue;
            }
            const glm::vec2 p0 = a.prevPosition[i];
            const glm::vec2 p1 = a.position[i];
            const float s0 = cross(e, p0 - g.a);
            const float s1 = cross(e, p1 - g.a);
            if ((s0 < 0.0f) == (s1 < 0.0f)) {
                continue;
            }
            const glm::vec2 d = p1 - p0;
            const float denom = cross(d, e);
            if (std::abs(denom) < 1e-9f) {
                continue;
            }
            const float u = cross(g.a - p0, d) / denom;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            if (s0 < 0.0f) {
                ++g.forward;
            } else {
                ++g.backward;
            }
        }
    }

    // Heatmap accumulation: every third step with three steps' worth of time (the same
    // time average for a third of the serial work).
    ensureHeatmap();
    if (heatmap_.width > 0 && sim.stepIndex() % kHeatmapStride == 0) {
        const float hdt = dt * static_cast<float>(kHeatmapStride);
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            const int x = static_cast<int>((a.position[i].x - heatmap_.origin.x) / heatmap_.cell);
            const int z = static_cast<int>((a.position[i].y - heatmap_.origin.y) / heatmap_.cell);
            if (x < 0 || z < 0 || x >= heatmap_.width || z >= heatmap_.height) {
                continue;
            }
            const auto c = static_cast<std::size_t>(z * heatmap_.width + x);
            const glm::vec2 v = a.velocity[i];
            heatmap_.agentSeconds[c] += hdt;
            heatmap_.speedSeconds[c] += std::sqrt(v.x * v.x + v.y * v.y) * hdt;
        }
        heatmap_.elapsed += hdt;
        ++heatmap_.version;
    }

    // Trajectory recording.
    if (recording_ && sim.stepIndex() % recordStride_ == 0) {
        if (rows_.size() + a.size() > kMaxRows) {
            recording_ = false;
        } else {
            for (std::uint32_t i = 0; i < a.size(); ++i) {
                rows_.push_back({a.id[i], a.agentType[i], a.stateId[i], static_cast<float>(now), a.position[i].x,
                                 a.height[i], a.position[i].y, a.velocity[i].x, a.velocity[i].y});
            }
        }
    }

    if (sim.stepIndex() % kSampleStride == 0) {
        takeSample();
    }
}

void MetricsCollector::takeSample() {
    const sim::AgentPool& a = sim_.agents();
    const float sampleDt = static_cast<float>(kSampleStride * sim_.stepSeconds());
    Sample s;
    s.time = sim_.simTimeSeconds();
    s.alive = a.size();
    s.arrived = sim_.stats().arrived;

    std::vector<float> speeds(a.size());
    std::unordered_map<std::uint32_t, std::uint32_t> occupancy;
    double sum = 0.0;
    for (std::uint32_t i = 0; i < a.size(); ++i) {
        const glm::vec2 v = a.velocity[i];
        speeds[i] = std::sqrt(v.x * v.x + v.y * v.y);
        sum += speeds[i];
        // 1 m cells for instantaneous density.
        const auto cx = static_cast<std::int32_t>(std::floor(a.position[i].x));
        const auto cz = static_cast<std::int32_t>(std::floor(a.position[i].y));
        ++occupancy[(static_cast<std::uint32_t>(cx) << 16) ^ static_cast<std::uint32_t>(cz & 0xFFFF)];
        // Stuck: wants to move, is not queueing, barely moves.
        const bool trying = a.moveMode[i] != sim::MoveMode::Stop && a.queueStage[i] == 0;
        if (trying && speeds[i] < kStuckSpeed) {
            slowFor_[a.id[i]] += sampleDt;
        } else {
            slowFor_.erase(a.id[i]);
        }
    }
    s.meanSpeed = a.size() > 0 ? static_cast<float>(sum / a.size()) : 0.0f;
    s.p50Speed = percentile(speeds, 0.5f);
    s.p90Speed = percentile(speeds, 0.9f);
    std::vector<float> densities;
    densities.reserve(occupancy.size());
    for (const auto& [cell, n] : occupancy) {
        densities.push_back(static_cast<float>(n));
    }
    s.peakDensity = densities.empty() ? 0.0f : *std::max_element(densities.begin(), densities.end());
    s.p90Density = percentile(densities, 0.9f);
    for (const auto& [id, seconds] : slowFor_) {
        s.stuck += seconds >= kStuckSeconds ? 1u : 0u;
    }
    for (const Gate& g : gates_) {
        s.gateTotals.push_back(g.forward + g.backward);
    }
    series_.push_back(std::move(s));
    if (series_.size() > kMaxSamples) {
        series_.erase(series_.begin(), series_.begin() + static_cast<std::ptrdiff_t>(kMaxSamples / 10));
    }
}

std::vector<std::uint32_t> MetricsCollector::stuckAgents() const {
    std::vector<std::uint32_t> out;
    for (const auto& [id, seconds] : slowFor_) {
        if (seconds >= kStuckSeconds) {
            out.push_back(id);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

Json MetricsCollector::summary() const {
    Json j{{"time", sim_.simTimeSeconds()}};
    if (!series_.empty()) {
        const Sample& s = series_.back();
        j["alive"] = s.alive;
        j["arrived"] = s.arrived;
        j["speed"] = {{"mean", round3(s.meanSpeed)}, {"p50", round3(s.p50Speed)}, {"p90", round3(s.p90Speed)}};
        j["density"] = {{"peak", round3(s.peakDensity)}, {"p90", round3(s.p90Density)}, {"unit", "agents/m²"}};
    }
    // Gate flow over the last 10 s.
    Json gates = Json::array();
    for (std::size_t g = 0; g < gates_.size(); ++g) {
        const Gate& gate = gates_[g];
        double rate = 0.0;
        if (series_.size() >= 2) {
            const Sample& last = series_.back();
            std::size_t k = series_.size() - 1;
            while (k > 0 && last.time - series_[k - 1].time <= 10.0 + 1e-6) {
                --k;
            }
            const Sample& first = series_[k];
            if (g < last.gateTotals.size() && g < first.gateTotals.size() && last.time > first.time) {
                rate = static_cast<double>(last.gateTotals[g] - first.gateTotals[g]) / (last.time - first.time);
            }
        }
        const double width = std::sqrt(std::pow(gate.b.x - gate.a.x, 2.0) + std::pow(gate.b.y - gate.a.y, 2.0));
        gates.push_back(Json{{"name", gate.name},
                             {"a", {round3(gate.a.x), round3(gate.a.y)}},
                             {"b", {round3(gate.b.x), round3(gate.b.y)}},
                             {"forward", gate.forward},
                             {"backward", gate.backward},
                             {"total", gate.forward + gate.backward},
                             {"flowPerSecond", round3(rate)},
                             {"flowPerMetrePerSecond", round3(width > 0.0 ? rate / width : 0.0)}});
    }
    j["gates"] = gates;
    std::vector<float> ttg = timeToGoal_;
    Json t{{"count", ttg.size()}};
    if (!ttg.empty()) {
        double sum = 0.0;
        for (float v : ttg) {
            sum += v;
        }
        t["mean"] = round3(sum / static_cast<double>(ttg.size()));
        t["p50"] = round3(percentile(ttg, 0.5f));
        t["p90"] = round3(percentile(ttg, 0.9f));
        t["max"] = round3(*std::max_element(ttg.begin(), ttg.end()));
    }
    j["timeToGoal"] = t;
    const auto stuck = stuckAgents();
    j["stuck"] = {
        {"count", stuck.size()},
        {"agents",
         Json(std::vector<std::uint32_t>(
             stuck.begin(), stuck.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(20, stuck.size()))))}};
    j["recording"] = {{"active", recording_}, {"rows", rows_.size()}};
    j["samples"] = series_.size();
    return j;
}

Json MetricsCollector::heatmapJson(const std::string& kind, std::size_t maxCells) const {
    if (kind != "density" && kind != "speed") {
        throw std::invalid_argument("heatmap kind must be \"density\" or \"speed\"");
    }
    const Heatmap& h = heatmap_;
    if (h.width == 0) {
        return Json{{"kind", kind}, {"width", 0}, {"height", 0}, {"values", Json::array()}};
    }
    maxCells = std::clamp<std::size_t>(maxCells, 16, 40000);
    const int factor = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(h.width * h.height) /
                                                                        static_cast<double>(maxCells)))));
    const int w = (h.width + factor - 1) / factor;
    const int hh = (h.height + factor - 1) / factor;
    Json rows = Json::array();
    double peak = 0.0;
    for (int z = 0; z < hh; ++z) {
        Json row = Json::array();
        for (int x = 0; x < w; ++x) {
            double agentSeconds = 0.0;
            double speedSeconds = 0.0;
            int cells = 0;
            for (int dz = 0; dz < factor; ++dz) {
                for (int dx = 0; dx < factor; ++dx) {
                    const int sx = x * factor + dx;
                    const int sz = z * factor + dz;
                    if (sx < h.width && sz < h.height) {
                        const auto i = static_cast<std::size_t>(sz * h.width + sx);
                        agentSeconds += h.agentSeconds[i];
                        speedSeconds += h.speedSeconds[i];
                        ++cells;
                    }
                }
            }
            double v = 0.0;
            if (kind == "density") {
                const double area = static_cast<double>(cells) * h.cell * h.cell;
                v = h.elapsed > 0.0 && area > 0.0 ? agentSeconds / (area * h.elapsed) : 0.0;
            } else {
                v = agentSeconds > 0.0 ? speedSeconds / agentSeconds : 0.0;
            }
            peak = std::max(peak, v);
            row.push_back(round3(v));
        }
        rows.push_back(row);
    }
    return Json{{"kind", kind},
                {"unit", kind == "density" ? "agents/m² (time-averaged)" : "m/s (mean while present)"},
                {"width", w},
                {"height", hh},
                {"cellSize", h.cell * static_cast<float>(factor)},
                {"origin", {h.origin.x, h.origin.y}},
                {"rows", "values[z][x]; z grows with world +Z, x with world +X"},
                {"seconds", round3(h.elapsed)},
                {"max", round3(peak)},
                {"values", rows}};
}

std::size_t MetricsCollector::exportTrajectories(const std::filesystem::path& file) const {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "agent_id,type,time,x,y,z,vx,vz,state\n";
    const auto& types = sim_.agentTypes();
    const auto& states = sim_.stateNames();
    for (const TrajectoryRow& r : rows_) {
        out << std::format("{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{}\n", r.id,
                           r.type < types.size() ? types[r.type].name : std::string(), r.time, r.x, r.y, r.z, r.vx,
                           r.vz, r.state < states.size() ? states[r.state] : std::string());
    }
    return rows_.size();
}

std::size_t MetricsCollector::exportSeries(const std::filesystem::path& file) const {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "time,alive,arrived,mean_speed,p50_speed,p90_speed,peak_density,p90_density,stuck";
    for (const Gate& g : gates_) {
        out << ",gate_" << g.name;
    }
    out << '\n';
    for (const Sample& s : series_) {
        out << std::format("{:.2f},{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{}", s.time, s.alive, s.arrived,
                           s.meanSpeed, s.p50Speed, s.p90Speed, s.peakDensity, s.p90Density, s.stuck);
        for (std::size_t g = 0; g < gates_.size(); ++g) {
            out << ',' << (g < s.gateTotals.size() ? s.gateTotals[g] : 0);
        }
        out << '\n';
    }
    return series_.size();
}

}  // namespace crowdsim::metrics
