#pragma once

/// @file AdAttention.hpp
/// Advertising attention: who could see each ad, who did, for how long and how well.
///
/// Ads are static objects tagged `ad` (a thin box: its large vertical face is the ad).
/// `ad:salience=<0..1>` says how eye-catching it is (bright, backlit, moving: high) and
/// `ad:range=<m>` how far its zone reaches (default 20 m).
///
/// Each agent gets a simple gaze model, evaluated a few times a second:
///  * the head points the way the agent walks, pitched down to watch the footing;
///  * two nested cones: a small central (foveal) one for reading and a wide field of view;
///    ads in the periphery may draw a glance (a head turn, within the neck's reach), more
///    likely the brighter and bigger they are, less likely with more ads competing for
///    attention (clutter) and with the agent's own inclination (SetAttention `interest`);
///  * agents spend a share of the time looking down at a phone (SetAttention `phone`).
/// Lines of sight run from the eyes to the four corners and the centre of the ad face,
/// blocked by static objects (physics raycasts) and by other people's bodies.
///
/// Per ad it reports opportunity to see (agents entering its zone), viewability (at least
/// half the ad visible inside the field of view for a continuous second), dwell time (the
/// ad's centre inside the foveal cone), noticing (a fixation long enough to read it at a
/// readable angle and size), angular size and viewing angle.
///
/// A read-only StepObserver: it never changes the simulation, and its random numbers are
/// hashed from agent id and time, so results are deterministic and independent of threads.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include "crowdsim/sim/Environment.hpp"
#include "crowdsim/sim/ScriptHooks.hpp"

namespace crowdsim::sim {
class Simulation;
}
namespace crowdsim::core {
class JobSystem;
}
namespace crowdsim::physics {
class IPhysicsWorld;
}
namespace crowdsim::commands {
class CommandRegistry;
}

namespace crowdsim::metrics {

/// Physiology and attention constants (degrees, metres, seconds).
struct AdParams {
    float evaluationHz = 5.0f;       ///< gaze updates per second
    float eyeHeight = 0.94f;         ///< eye height, x body height
    float gazePitch = -12.0f;        ///< head pitch while walking (watching the footing)
    float phonePitch = -45.0f;       ///< head pitch while looking at a phone
    float fovealHalfAngle = 7.5f;    ///< central vision cone (15 deg wide): reading
    float fieldHalfAngle = 60.0f;    ///< field of view for viewability (120 deg wide)
    float peripheralHalfAngle = 90.0f;  ///< what can draw a glance (180 deg wide)
    float maxHeadYaw = 60.0f;        ///< how far the head turns from the walking direction
    float glanceRate = 0.6f;         ///< glances per second at a fully salient, large ad, alone in view
    float glanceMin = 0.6f;          ///< glance duration range
    float glanceMax = 2.2f;
    float phoneEpisode = 15.0f;      ///< mean time looking at the phone at a stretch
    float viewableShare = 0.5f;      ///< share of the ad that must be visible...
    float viewableSeconds = 1.0f;    ///< ...for this long, continuously (industry standard)
    float noticeSeconds = 0.4f;      ///< a fixation this long, readable, counts as noticed
    float readableAngle = 60.0f;     ///< steeper viewing angles distort the ad too much
    float minAngularSize = 1.0f;     ///< % of the field of view below which it cannot be read
    float defaultRange = 20.0f;      ///< zone of an ad without `ad:range`
    float defaultSalience = 0.5f;
};

/// One ad face, from an object tagged `ad`.
struct AdSurface {
    sim::SceneId object = sim::kNoSceneId;
    std::string name;
    glm::vec3 centre{0.0f};
    glm::vec3 normal{1.0f, 0.0f, 0.0f};  ///< across the thin axis (either face may be the front)
    glm::vec3 across{0.0f, 0.0f, 1.0f};  ///< horizontal along the face
    float halfWidth = 0.5f;
    float halfHeight = 0.5f;
    float halfThickness = 0.0f;
    float salience = 0.5f;
    float range = 20.0f;
};

/// What one agent did with one ad.
struct AdExposure {
    bool inZone = false;     ///< opportunity to see
    bool viewable = false;   ///< met the viewability standard at least once
    bool noticed = false;    ///< fixated long enough at a readable angle and size
    float viewRun = 0.0f;    ///< current continuous viewable time
    float fixation = 0.0f;   ///< current continuous foveal time
    float dwell = 0.0f;      ///< total time in the foveal cone, s
    float visibleTime = 0.0f;  ///< total time at least half visible in the field of view, s
    float steepTime = 0.0f;    ///< ...of which at a steeper angle than readable
    float smallTime = 0.0f;    ///< ...of which smaller than readable
    double sizeSum = 0.0;      ///< angular size (% of field) x seconds, while visible
    double angleSum = 0.0;     ///< viewing angle (deg) x seconds, while visible
    std::uint32_t glances = 0;
};

/// Where an agent is looking right now (for display): eye position, head direction
/// (unit), and what the gaze model says it is doing.
struct GazeView {
    glm::vec3 eye{0.0f};
    glm::vec3 head{0.0f, 0.0f, 1.0f};
    bool phone = false;       ///< looking down at a phone
    bool glancing = false;    ///< head turned towards an ad
    std::int32_t lookingAt = -1;  ///< ad (index into ads()) in central vision at the last update, or -1
};

class AdAttention final : public sim::StepObserver {
public:
    AdAttention(sim::Simulation& sim, core::JobSystem& jobs, const physics::IPhysicsWorld& world);
    ~AdAttention() override;
    AdAttention(const AdAttention&) = delete;
    AdAttention& operator=(const AdAttention&) = delete;

    void afterStep(sim::Simulation& sim) override;
    void reset();

    [[nodiscard]] const std::vector<AdSurface>& ads() const noexcept { return ads_; }
    /// Gaze of the agent at pool index `index`, from the current positions (works in scenes
    /// without ads too: the head then follows the walking direction).
    [[nodiscard]] GazeView gaze(std::uint32_t index) const;
    /// Per-ad report (see docs/formats.md, GetAdMetrics).
    [[nodiscard]] nlohmann::json summary() const;
    /// Write the per-ad report and one row per agent and ad seen as CSV files.
    void exportCsv(const std::filesystem::path& perAd, const std::filesystem::path& perViewer) const;

    AdParams params;

private:
    struct Gaze {
        bool phone = false;
        float phoneLeft = 0.0f;
        std::int32_t glanceAd = -1;
        std::int32_t lookingAt = -1;  ///< ad in central vision at the last evaluation
        float glanceLeft = 0.0f;
        bool started = false;
        std::map<std::uint32_t, AdExposure> exposure;  ///< by ad index
    };

    void rebuildAds();
    void evaluate(float dt);

    sim::Simulation& sim_;
    core::JobSystem& jobs_;
    const physics::IPhysicsWorld& world_;
    std::vector<AdSurface> ads_;
    std::uint64_t adsVersion_ = ~0ull;
    std::uint64_t lastStep_ = 0;
    std::uint64_t evaluations_ = 0;
    std::uint32_t stepsSinceEval_ = 0;
    std::map<std::uint32_t, Gaze> gaze_;  ///< by agent id (kept after they leave)
};

void registerAdCommands(commands::CommandRegistry& registry, AdAttention& ads);

}  // namespace crowdsim::metrics
