#include "crowdsim/metrics/AttentionModel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/assets/SceneSections.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/physics/PhysicsWorld.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::metrics {

namespace {

using Json = nlohmann::json;

constexpr float kDegToRad = 0.017453292f;
constexpr float kRadToDeg = 57.29578f;
constexpr int kSamples = 5;  ///< centre and four corners of the target face

/// Uniform [0, 1) from (agent, evaluation, purpose): deterministic, no shared state.
float hashed(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    std::uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) * 0xBF58476D1CE4E5B9ull ^
                      (c + 1) * 0x94D049BB133111EBull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<float>(x >> 40) * (1.0f / 16777216.0f);
}

bool hasTag(const std::vector<std::string>& tags, std::string_view tag) {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

float tagNumber(const std::vector<std::string>& tags, std::string_view key, float fallback) {
    for (const std::string& t : tags) {
        if (t.starts_with(key) && t.size() > key.size() && t[key.size()] == '=') {
            float v = fallback;
            const char* b = t.data() + key.size() + 1;
            if (std::from_chars(b, t.data() + t.size(), v).ec == std::errc{}) {
                return v;
            }
        }
    }
    return fallback;
}

float angleBetween(glm::vec3 a, glm::vec3 b) {
    const float la = glm::length(a);
    const float lb = glm::length(b);
    if (la < 1e-6f || lb < 1e-6f) {
        return 0.0f;
    }
    return std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f)) * kRadToDeg;
}

glm::vec3 direction(glm::vec2 horizontal, float pitchDeg) {
    const float p = pitchDeg * kDegToRad;
    return {horizontal.x * std::cos(p), std::sin(p), horizontal.y * std::cos(p)};
}

/// People as vertical cylinders in a coarse grid, for lines of sight through the crowd.
class BodyGrid {
public:
    static constexpr float kCell = 1.5f;

    void build(const sim::Simulation& sim) {
        cells_.clear();
        const sim::AgentPool& a = sim.agents();
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            const float r = a.radius[i];
            const glm::vec2 p = a.position[i];
            const int x0 = cellOf(p.x - r), x1 = cellOf(p.x + r);
            const int z0 = cellOf(p.y - r), z1 = cellOf(p.y + r);
            for (int x = x0; x <= x1; ++x) {
                for (int z = z0; z <= z1; ++z) {
                    cells_[key(x, z)].push_back(i);
                }
            }
        }
    }

    /// True if someone other than `self` stands between `eye` and `target`.
    [[nodiscard]] bool blocked(const sim::Simulation& sim, std::uint32_t self, glm::vec3 eye, glm::vec3 target,
                               std::vector<std::int64_t>& seen) const {
        const sim::AgentPool& a = sim.agents();
        const glm::vec2 e(eye.x, eye.z);
        const glm::vec2 t(target.x, target.z);
        const glm::vec2 d = t - e;
        const float len = glm::length(d);
        const int steps = std::max(1, static_cast<int>(len / (kCell * 0.5f)));
        seen.clear();
        for (int s = 0; s <= steps; ++s) {
            const glm::vec2 p = e + d * (static_cast<float>(s) / static_cast<float>(steps));
            const std::int64_t k = key(cellOf(p.x), cellOf(p.y));
            if (std::find(seen.begin(), seen.end(), k) != seen.end()) {
                continue;
            }
            seen.push_back(k);
            const auto it = cells_.find(k);
            if (it == cells_.end()) {
                continue;
            }
            for (std::uint32_t j : it->second) {
                if (j == self) {
                    continue;
                }
                const glm::vec2 c = a.position[j];
                const float u = len > 1e-4f ? std::clamp(glm::dot(c - e, d) / (len * len), 0.0f, 1.0f) : 0.0f;
                if (u <= 0.0f || u >= 1.0f) {
                    continue;  // not between the eyes and the target
                }
                const glm::vec2 closest = e + d * u;
                if (glm::length(c - closest) > a.radius[j] * 0.85f) {  // shoulders, not the avoidance disc
                    continue;
                }
                const float y = eye.y + (target.y - eye.y) * u;
                const float h =
                    a.agentType[j] < sim.agentTypes().size() ? sim.agentTypes()[a.agentType[j]].height : 1.75f;
                if (y >= a.height[j] && y <= a.height[j] + h) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    static int cellOf(float v) { return static_cast<int>(std::floor(v / kCell)); }
    static std::int64_t key(int x, int z) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(z));
    }
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells_;
};

}  // namespace

AttentionModel::AttentionModel(sim::Simulation& sim, core::JobSystem& jobs, const physics::IPhysicsWorld& world)
    : sim_(sim), jobs_(jobs), world_(world) {
    sim_.addStepObserver(this);
}

AttentionModel::~AttentionModel() {
    sim_.removeStepObserver(this);
}

void AttentionModel::reset() {
    gaze_.clear();
    evaluations_ = 0;
    stepsSinceEval_ = 0;
}

void AttentionModel::rebuildTargets() {
    targets_.clear();
    for (sim::SceneId id : sim_.staticObjectIds()) {
        const sim::StaticObject* o = sim_.findStaticObject(id);
        // `ad` is the tag's older name, still read so earlier scenes keep working.
        if (o == nullptr || !(hasTag(o->tags, "attention") || hasTag(o->tags, "ad")) ||
            o->primitive.kind != geometry::PrimitiveKind::Box) {
            continue;
        }
        const geometry::PrimitiveDesc& p = o->primitive;
        // Local axes in world space (see geometry::MeshBuilder::toWorld).
        const float c = std::cos(p.rotationY);
        const float s = std::sin(p.rotationY);
        const glm::vec3 localX(c, 0.0f, -s);
        const glm::vec3 localZ(s, 0.0f, c);
        AttentionTarget target;
        target.object = id;
        target.name = o->name;
        target.centre = p.position + glm::vec3(0.0f, 0.5f * p.size.y, 0.0f);
        // The face is across the thinner horizontal axis.
        const bool thinX = p.size.x <= p.size.z;
        target.normal = thinX ? localX : localZ;
        target.across = thinX ? localZ : localX;
        target.halfThickness = 0.5f * (thinX ? p.size.x : p.size.z);
        target.halfWidth = 0.5f * (thinX ? p.size.z : p.size.x);
        target.halfHeight = 0.5f * p.size.y;
        const float salience = tagNumber(o->tags, "ad:salience", params.defaultSalience);
        const float range = tagNumber(o->tags, "ad:range", params.defaultRange);
        target.salience = std::clamp(tagNumber(o->tags, "attention:salience", salience), 0.0f, 1.0f);
        target.range = std::max(0.5f, tagNumber(o->tags, "attention:range", range));
        targets_.push_back(target);
    }
}

void AttentionModel::afterStep(sim::Simulation& sim) {
    if (sim.stepIndex() < lastStep_) {
        reset();  // ResetSimulation / scene replaced
    }
    lastStep_ = sim.stepIndex();
    if (sim.staticVersion() != targetsVersion_) {
        targetsVersion_ = sim.staticVersion();
        rebuildTargets();
        reset();  // target indices changed
    }
    if (targets_.empty()) {
        return;
    }
    const auto every = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(1.0 / (sim.stepSeconds() * params.evaluationHz))));
    if (++stepsSinceEval_ < every) {
        return;
    }
    stepsSinceEval_ = 0;
    evaluate(static_cast<float>(sim.stepSeconds() * every));
}

void AttentionModel::evaluate(float dt) {
    const sim::Simulation& sim = sim_;
    const sim::AgentPool& a = sim.agents();
    const std::uint32_t n = a.size();
    ++evaluations_;
    BodyGrid bodies;
    bodies.build(sim);

    // Each agent's state, looked up serially so the parallel pass only touches its own.
    std::vector<Gaze*> states(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        states[i] = &gaze_[a.id[i]];
    }

    const AttentionParams& P = params;
    const float fieldSolidAngle = 6.2831853f * (1.0f - std::cos(P.fieldHalfAngle * kDegToRad));
    const std::uint64_t tick = evaluations_;

    jobs_.parallelFor(n, 16, [&](core::IndexRange range, std::uint32_t) {
        std::vector<std::int64_t> seen;
        struct Candidate {
            std::uint32_t target;
            float weight;
        };
        std::vector<Candidate> candidates;
        for (std::uint32_t i = range.begin; i < range.end; ++i) {
            Gaze& g = *states[i];
            const std::uint64_t id = a.id[i];
            const sim::Attention& att = a.attention[i];

            // Phone: alternating episodes so that, on average, `phone` of the time is on it.
            if (!g.started) {
                g.started = true;
                g.phone = hashed(id, 0, 1) < att.phone;
                g.phoneLeft = hashed(id, 0, 2) * P.phoneEpisode;
            }
            if (g.phone) {
                g.phoneLeft -= dt;
                if (g.phoneLeft <= 0.0f && att.phone < 1.0f) {
                    g.phone = false;
                }
            } else if (att.phone > 0.0f) {
                const float rate = att.phone >= 1.0f ? 1e9f : att.phone / ((1.0f - att.phone) * P.phoneEpisode);
                if (hashed(id, tick, 3) < 1.0f - std::exp(-rate * dt)) {
                    g.phone = true;
                    g.phoneLeft = P.phoneEpisode * (0.5f + hashed(id, tick, 4));
                }
            }
            if (g.phone) {
                g.glanceTarget = -1;
            }

            // Where the head points: the walking direction, down a little; or a glanced target.
            const glm::vec2 v = a.velocity[i];
            const float speed = glm::length(v);
            const glm::vec2 walk = speed > 0.2f ? v / speed : glm::vec2(std::sin(a.heading[i]), std::cos(a.heading[i]));
            const float bodyHeight =
                a.agentType[i] < sim.agentTypes().size() ? sim.agentTypes()[a.agentType[i]].height : 1.75f;
            const glm::vec3 eye(a.position[i].x, a.height[i] + P.eyeHeight * bodyHeight, a.position[i].y);
            glm::vec3 head = direction(walk, g.phone ? P.phonePitch : P.gazePitch);
            if (g.glanceTarget >= 0) {
                const AttentionTarget& target = targets_[static_cast<std::size_t>(g.glanceTarget)];
                const glm::vec3 to = target.centre - eye;
                const float yaw = angleBetween(glm::vec3(to.x, 0.0f, to.z), glm::vec3(walk.x, 0.0f, walk.y));
                g.glanceLeft -= dt;
                if (g.glanceLeft <= 0.0f || yaw > P.maxHeadYaw) {
                    g.glanceTarget = -1;
                } else {
                    head = to;
                }
            }

            candidates.clear();
            g.lookingAt = -1;
            for (std::uint32_t k = 0; k < targets_.size(); ++k) {
                const AttentionTarget& target = targets_[k];
                const glm::vec3 toCentre = target.centre - eye;
                if (glm::length(glm::vec2(toCentre.x, toCentre.z)) > target.range || toCentre.y < -2.5f ||
                    toCentre.y > 4.0f) {
                    continue;  // outside its zone, or on another floor
                }
                Exposure& x = g.exposure[k];
                x.inZone = true;

                // The face towards the viewer, just in front of the panel.
                const float side = glm::dot(eye - target.centre, target.normal) >= 0.0f ? 1.0f : -1.0f;
                const glm::vec3 front = target.normal * side;
                const glm::vec3 face = target.centre + front * (target.halfThickness + 0.02f);
                const glm::vec3 up(0.0f, 1.0f, 0.0f);
                const std::array<glm::vec3, kSamples> points{
                    face, face + target.across * (0.9f * target.halfWidth) + up * (0.9f * target.halfHeight),
                    face - target.across * (0.9f * target.halfWidth) + up * (0.9f * target.halfHeight),
                    face + target.across * (0.9f * target.halfWidth) - up * (0.9f * target.halfHeight),
                    face - target.across * (0.9f * target.halfWidth) - up * (0.9f * target.halfHeight)};
                int inField = 0;
                int visible = 0;
                bool centreVisible = false;
                for (int s = 0; s < kSamples; ++s) {
                    const glm::vec3 d = points[static_cast<std::size_t>(s)] - eye;
                    const float dist = glm::length(d);
                    if (angleBetween(head, d) > P.peripheralHalfAngle || dist < 0.1f) {
                        continue;  // behind the viewer: not even in the periphery
                    }
                    const bool clear = !world_.raycast(eye, d, dist - 0.05f).has_value() &&
                                       !bodies.blocked(sim, i, eye, points[static_cast<std::size_t>(s)], seen);
                    if (!clear) {
                        continue;
                    }
                    ++visible;
                    centreVisible = centreVisible || s == 0;
                    if (angleBetween(head, d) <= P.fieldHalfAngle) {
                        ++inField;
                    }
                }
                const glm::vec3 toFace = face - eye;
                const float dist = std::max(0.1f, glm::length(toFace));
                const float cosIncidence = std::max(0.0f, glm::dot(-toFace / dist, front));
                const float incidence = std::acos(std::min(1.0f, cosIncidence)) * kRadToDeg;
                const float size = std::min(100.0f, 100.0f * 4.0f * target.halfWidth * target.halfHeight *
                                                        cosIncidence / (dist * dist) / fieldSolidAngle);

                // Viewability: enough of it visible inside the field of view, continuously.
                if (static_cast<float>(inField) >= P.viewableShare * kSamples) {
                    x.viewRun += dt;
                    x.viewable = x.viewable || x.viewRun >= P.viewableSeconds - 1e-4f;
                    x.visibleTime += dt;
                    x.sizeSum += static_cast<double>(size * dt);
                    x.angleSum += static_cast<double>(incidence * dt);
                    x.steepTime += incidence > P.readableAngle ? dt : 0.0f;
                    x.smallTime += size < P.minAngularSize ? dt : 0.0f;
                } else {
                    x.viewRun = 0.0f;
                }
                // Dwell: the target's centre in the foveal cone (not while reading a phone).
                if (!g.phone && centreVisible && angleBetween(head, toFace) <= P.fovealHalfAngle) {
                    x.dwell += dt;
                    x.fixation += dt;
                    g.lookingAt = static_cast<std::int32_t>(k);
                    if (x.fixation >= P.noticeSeconds - 1e-4f && incidence <= P.readableAngle &&
                        size >= P.minAngularSize) {
                        x.noticed = true;
                    }
                } else {
                    x.fixation = 0.0f;
                }
                // Could it draw a glance? Seen in the periphery, within the neck's reach.
                const float yaw = angleBetween(glm::vec3(toFace.x, 0.0f, toFace.z), glm::vec3(walk.x, 0.0f, walk.y));
                if (!g.phone && visible >= 2 && yaw <= P.maxHeadYaw && size >= 0.5f * P.minAngularSize) {
                    const float sizeFactor = std::clamp(size / 4.0f, 0.25f, 1.0f);
                    candidates.push_back({k, target.salience * sizeFactor});
                }
            }

            // A glance: the targets in view compete for it (clutter), the agent's interest scales it.
            if (!g.phone && g.glanceTarget < 0 && !candidates.empty() && att.interest > 0.0f) {
                float total = 0.0f;
                for (const Candidate& c : candidates) {
                    total += c.weight;
                }
                const float rate = P.glanceRate * att.interest * total / static_cast<float>(candidates.size());
                if (hashed(id, tick, 5) < 1.0f - std::exp(-rate * dt)) {
                    float pick = hashed(id, tick, 6) * total;
                    std::uint32_t chosen = candidates.back().target;
                    for (const Candidate& c : candidates) {
                        pick -= c.weight;
                        if (pick < 0.0f) {
                            chosen = c.target;
                            break;
                        }
                    }
                    g.glanceTarget = static_cast<std::int32_t>(chosen);
                    g.glanceLeft = P.glanceMin + (P.glanceMax - P.glanceMin) * hashed(id, tick, 7);
                    ++g.exposure[chosen].glances;
                }
            }
        }
    });
}

GazeView AttentionModel::gaze(std::uint32_t index) const {
    const sim::AgentPool& a = sim_.agents();
    GazeView view;
    if (index >= a.size()) {
        return view;
    }
    const glm::vec2 v = a.velocity[index];
    const float speed = glm::length(v);
    const glm::vec2 walk = speed > 0.2f ? v / speed : glm::vec2(std::sin(a.heading[index]), std::cos(a.heading[index]));
    const float bodyHeight =
        a.agentType[index] < sim_.agentTypes().size() ? sim_.agentTypes()[a.agentType[index]].height : 1.75f;
    view.eye = glm::vec3(a.position[index].x, a.height[index] + params.eyeHeight * bodyHeight, a.position[index].y);
    view.head = direction(walk, params.gazePitch);
    const auto it = gaze_.find(a.id[index]);
    if (it == gaze_.end()) {
        return view;
    }
    const Gaze& g = it->second;
    view.phone = g.phone;
    view.lookingAt = g.lookingAt < static_cast<std::int32_t>(targets_.size()) ? g.lookingAt : -1;
    if (g.phone) {
        view.head = direction(walk, params.phonePitch);
    } else if (g.glanceTarget >= 0 && g.glanceTarget < static_cast<std::int32_t>(targets_.size())) {
        const glm::vec3 to = targets_[static_cast<std::size_t>(g.glanceTarget)].centre - view.eye;
        if (glm::length(to) > 0.1f) {
            view.head = glm::normalize(to);
            view.glancing = true;
        }
    }
    return view;
}

Json AttentionModel::summary() const {
    struct Totals {
        std::uint32_t ots = 0, viewable = 0, noticed = 0, viewers = 0, glances = 0;
        double dwell = 0.0, visible = 0.0, size = 0.0, angle = 0.0, steep = 0.0, small = 0.0;
        std::vector<float> dwells;
    };
    std::vector<Totals> t(targets_.size());
    for (const auto& [id, g] : gaze_) {
        for (const auto& [k, x] : g.exposure) {
            if (k >= t.size() || !x.inZone) {
                continue;
            }
            Totals& s = t[k];
            ++s.ots;
            s.viewable += x.viewable ? 1u : 0u;
            s.noticed += x.noticed ? 1u : 0u;
            s.glances += x.glances;
            s.dwell += x.dwell;
            s.visible += x.visibleTime;
            s.size += x.sizeSum;
            s.angle += x.angleSum;
            s.steep += x.steepTime;
            s.small += x.smallTime;
            if (x.dwell > 0.0f) {
                ++s.viewers;
                s.dwells.push_back(x.dwell);
            }
        }
    }
    // Rounded to the nearest double of a decimal (so JSON shows 61.9, not 61.900000000000006).
    auto round = [](double v, double unit) {
        const double scale = std::round(1.0 / unit);
        return std::round(v * scale) / scale;
    };
    Json targets = Json::array();
    for (std::size_t k = 0; k < targets_.size(); ++k) {
        Totals& s = t[k];
        std::sort(s.dwells.begin(), s.dwells.end());
        const double median = s.dwells.empty() ? 0.0 : s.dwells[s.dwells.size() / 2];
        const auto rate = [&](std::uint32_t c) { return s.ots > 0 ? round(100.0 * c / s.ots, 0.1) : 0.0; };
        targets.push_back(Json{
            {"id", targets_[k].object},
            {"name", targets_[k].name},
            {"salience", round(targets_[k].salience, 0.01)},
            {"ots", s.ots},
            {"viewable", s.viewable},
            {"viewabilityRate", rate(s.viewable)},
            {"noticed", s.noticed},
            {"noticeRate", rate(s.noticed)},
            {"glances", s.glances},
            {"dwellSeconds", round(s.dwell, 0.01)},
            {"meanDwellPerOts", s.ots > 0 ? round(s.dwell / s.ots, 0.001) : 0.0},
            {"meanDwellPerViewer", s.viewers > 0 ? round(s.dwell / s.viewers, 0.001) : 0.0},
            {"medianDwellPerViewer", round(median, 0.001)},
            {"visibleSeconds", round(s.visible, 0.01)},
            {"meanAngularSize", s.visible > 0.0 ? round(s.size / s.visible, 0.01) : 0.0},
            {"meanViewingAngle", s.visible > 0.0 ? round(s.angle / s.visible, 0.1) : 0.0},
            {"steepShare", s.visible > 0.0 ? round(100.0 * s.steep / s.visible, 0.1) : 0.0},
            {"smallShare", s.visible > 0.0 ? round(100.0 * s.small / s.visible, 0.1) : 0.0},
        });
    }
    return Json{{"targets", targets}, {"agentsSeen", gaze_.size()}, {"params", reflect::toJson(params)}};
}

void AttentionModel::exportCsv(const std::filesystem::path& perTarget, const std::filesystem::path& perViewer) const {
    for (const auto& file : {perTarget, perViewer}) {
        if (file.has_parent_path()) {
            std::filesystem::create_directories(file.parent_path());
        }
    }
    const Json s = summary();
    std::ofstream out(perTarget, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write '" + perTarget.string() + "'");
    }
    const std::array<const char*, 18> columns{"id",
                                              "name",
                                              "salience",
                                              "ots",
                                              "viewable",
                                              "viewabilityRate",
                                              "noticed",
                                              "noticeRate",
                                              "glances",
                                              "dwellSeconds",
                                              "meanDwellPerOts",
                                              "meanDwellPerViewer",
                                              "medianDwellPerViewer",
                                              "visibleSeconds",
                                              "meanAngularSize",
                                              "meanViewingAngle",
                                              "steepShare",
                                              "smallShare"};
    for (std::size_t c = 0; c < columns.size(); ++c) {
        out << (c ? "," : "") << columns[c];
    }
    out << '\n';
    for (const Json& target : s["targets"]) {
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const Json& v = target[columns[c]];
            out << (c ? "," : "") << (v.is_string() ? "\"" + v.get<std::string>() + "\"" : v.dump());
        }
        out << '\n';
    }
    std::ofstream rows(perViewer, std::ios::binary);
    if (!rows) {
        throw std::runtime_error("cannot write '" + perViewer.string() + "'");
    }
    rows << "agent,target,viewable,noticed,glances,dwell,visibleSeconds,meanAngularSize,meanViewingAngle\n";
    for (const auto& [id, g] : gaze_) {
        for (const auto& [k, x] : g.exposure) {
            if (k >= targets_.size() || !x.inZone) {
                continue;
            }
            const double vis = x.visibleTime;
            rows << std::format("{},\"{}\",{},{},{},{:.2f},{:.2f},{:.2f},{:.1f}\n", id, targets_[k].name,
                                x.viewable ? 1 : 0, x.noticed ? 1 : 0, x.glances, x.dwell, vis,
                                vis > 0 ? x.sizeSum / vis : 0.0, vis > 0 ? x.angleSum / vis : 0.0);
        }
    }
}

void registerAttentionCommands(commands::CommandRegistry& r, AttentionModel& model) {
    using commands::CommandContext;
    using commands::CommandOutput;
    const Json noParams{{"type", "object"}, {"properties", Json::object()}};
    r.add({"GetAttentionMetrics",
           "How well each attention target (objects tagged `attention`) is seen: opportunity to see, viewability, "
           "noticing, dwell time, "
           "angular size and viewing angle, from each agent's simulated gaze.",
           noParams, [&model](const Json&, CommandContext&) { return CommandOutput{model.summary()}; }, false});
    r.add({"ExportAttentionMetrics",
           "Write the target report as CSV (default recordings/attention.csv) and one row per agent and target seen "
           "(recordings/attention_viewers.csv).",
           Json{{"type", "object"},
                {"properties",
                 {{"path", {{"type", "string"}, {"description", "content-relative CSV path, per target"}}},
                  {"viewersPath", {{"type", "string"}, {"description", "content-relative CSV path, per viewer"}}}}}},
           [&model](const Json& p, CommandContext& ctx) {
               CommandOutput out;
               if (!ctx.replaying) {
                   const auto perTarget = ctx.assets.resolve(p.value("path", std::string("recordings/attention.csv")));
                   const auto perViewer =
                       ctx.assets.resolve(p.value("viewersPath", std::string("recordings/attention_viewers.csv")));
                   model.exportCsv(perTarget, perViewer);
                   out.result = Json{{"path", ctx.assets.relativize(perTarget)},
                                     {"viewersPath", ctx.assets.relativize(perViewer)}};
               }
               return out;
           },
           false});
    r.add({"SetAttentionParams",
           "Change the attention model's settings (merge: only the keys given). Clears the attention record, "
           "since counts under different settings do not add up.",
           Json{{"type", "object"},
                {"properties",
                 {{"params",
                   {{"type", "object"}, {"description", "settings to change (GetAttentionMetrics lists them)"}}}}},
                {"required", Json::array({"params"})}},
           [&model](const Json& p, CommandContext&) {
               AttentionParams next = model.params;
               reflect::fromJson(p.at("params"), next);  // validates; throws before anything changes
               CommandOutput out;
               out.undo = commands::makeCommand("SetAttentionParams", Json{{"params", reflect::toJson(model.params)}});
               out.label = "Attention settings";
               model.params = next;
               model.reset();
               return out;
           }});
    r.add({"ResetAttentionMetrics", "Clear the attention record and start counting again from now.", noParams,
           [&model](const Json&, CommandContext&) {
               model.reset();
               return CommandOutput{};
           }});
}

void registerAttentionSceneSection(assets::SceneSections& sections, AttentionModel& model) {
    sections.add({"attention",
                  [&model]() -> Json {
                      const Json now = reflect::toJson(model.params);
                      // Left out at the defaults, so scenes that do not study attention stay as they were.
                      return now == reflect::toJson(AttentionParams{}) ? Json(nullptr) : now;
                  },
                  [&model](const Json* section) -> std::function<void()> {
                      AttentionParams next;
                      if (section != nullptr) {
                          reflect::fromJson(*section, next);  // throws on an invalid setting
                      }
                      return [&model, next] {
                          if (reflect::toJson(model.params) != reflect::toJson(next)) {
                              model.params = next;
                              model.reset();  // counts under other settings do not add up
                          }
                      };
                  }});
}

}  // namespace crowdsim::metrics
