#pragma once

/// @file AttentionModel.hpp
/// Visual attention: who could see each target, who did, for how long and how well.
///
/// A target is anything people may look at (an ad, a sign, an exit, an artwork): a static
/// box tagged `attention` (a thin box: its large vertical face is what is looked at).
/// `attention:salience=<0..1>` says how eye-catching it is (bright, backlit, moving: high)
/// and `attention:range=<m>` how far its zone reaches (default 20 m). The older `ad`,
/// `ad:salience` and `ad:range` tags are read too.
///
/// Each agent gets a simple gaze model, evaluated a few times a second:
///  * the head points the way the agent walks, pitched down to watch the footing;
///  * two nested cones: a small central (foveal) one for reading and a wide field of view;
///    targets in the periphery may draw a glance (a head turn, within the neck's reach), more
///    likely the brighter and bigger they are, less likely with more targets competing for
///    attention (clutter) and with the agent's own inclination (SetAttention `interest`);
///  * agents spend a share of the time looking down at a phone (SetAttention `phone`).
/// Lines of sight run from the eyes to the four corners and the centre of the target face,
/// blocked by static objects (physics raycasts) and by other people's bodies.
///
/// Per target it reports opportunity to see (agents entering its zone), viewability (at least
/// half the target visible inside the field of view for a continuous second), dwell time (the
/// target's centre inside the foveal cone), noticing (a fixation long enough to read it at a
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

#include "crowdsim/reflect/Reflect.hpp"
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
namespace crowdsim::assets {
class SceneSections;
}

namespace crowdsim::metrics {

/// Physiology and attention constants (degrees, metres, seconds).
struct AttentionParams {
    float evaluationHz = 5.0f;          ///< gaze updates per second
    float eyeHeight = 0.94f;            ///< eye height, x body height
    float gazePitch = -12.0f;           ///< head pitch while walking (watching the footing)
    float phonePitch = -45.0f;          ///< head pitch while looking at a phone
    float fovealHalfAngle = 7.5f;       ///< central vision cone (15 deg wide): reading
    float fieldHalfAngle = 60.0f;       ///< field of view for viewability (120 deg wide)
    float peripheralHalfAngle = 90.0f;  ///< what can draw a glance (180 deg wide)
    float maxHeadYaw = 60.0f;           ///< how far the head turns from the walking direction
    float glanceRate = 0.6f;            ///< glances per second at a fully salient, large target, alone in view
    float glanceMin = 0.6f;             ///< glance duration range
    float glanceMax = 2.2f;
    float phoneEpisode = 15.0f;    ///< mean time looking at the phone at a stretch
    float viewableShare = 0.5f;    ///< share of the target that must be visible...
    float viewableSeconds = 1.0f;  ///< ...for this long, continuously (industry standard)
    float noticeSeconds = 0.4f;    ///< a fixation this long, readable, counts as noticed
    float readableAngle = 60.0f;   ///< steeper viewing angles distort the target too much
    float minAngularSize = 1.0f;   ///< % of the field of view below which it cannot be read
    float defaultRange = 20.0f;    ///< zone of a target without `attention:range`
    float defaultSalience = 0.5f;
};

/// One target face, from an object tagged `attention`.
struct AttentionTarget {
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

/// What one agent did with one target.
struct Exposure {
    bool inZone = false;       ///< opportunity to see
    bool viewable = false;     ///< met the viewability standard at least once
    bool noticed = false;      ///< fixated long enough at a readable angle and size
    float viewRun = 0.0f;      ///< current continuous viewable time
    float fixation = 0.0f;     ///< current continuous foveal time
    float dwell = 0.0f;        ///< total time in the foveal cone, s
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
    bool phone = false;           ///< looking down at a phone
    bool glancing = false;        ///< head turned towards a target
    std::int32_t lookingAt = -1;  ///< target (index into targets()) in central vision at the last update, or -1
};

class AttentionModel final : public sim::StepObserver {
public:
    AttentionModel(sim::Simulation& sim, core::JobSystem& jobs, const physics::IPhysicsWorld& world);
    ~AttentionModel() override;
    AttentionModel(const AttentionModel&) = delete;
    AttentionModel& operator=(const AttentionModel&) = delete;

    void afterStep(sim::Simulation& sim) override;
    void reset();

    [[nodiscard]] const std::vector<AttentionTarget>& targets() const noexcept { return targets_; }
    /// Gaze of the agent at pool index `index`, from the current positions (works in scenes
    /// without targets too: the head then follows the walking direction).
    [[nodiscard]] GazeView gaze(std::uint32_t index) const;
    /// Per-target report (see docs/formats.md, GetAttentionMetrics).
    [[nodiscard]] nlohmann::json summary() const;
    /// Write the per-target report and one row per agent and target seen as CSV files.
    void exportCsv(const std::filesystem::path& perTarget, const std::filesystem::path& perViewer) const;

    AttentionParams params;

private:
    struct Gaze {
        bool phone = false;
        float phoneLeft = 0.0f;
        std::int32_t glanceTarget = -1;
        std::int32_t lookingAt = -1;  ///< target in central vision at the last evaluation
        float glanceLeft = 0.0f;
        bool started = false;
        std::map<std::uint32_t, Exposure> exposure;  ///< by target index
    };

    void rebuildTargets();
    void evaluate(float dt);

    sim::Simulation& sim_;
    core::JobSystem& jobs_;
    const physics::IPhysicsWorld& world_;
    std::vector<AttentionTarget> targets_;
    std::uint64_t targetsVersion_ = ~0ull;
    std::uint64_t lastStep_ = 0;
    std::uint64_t evaluations_ = 0;
    std::uint32_t stepsSinceEval_ = 0;
    std::map<std::uint32_t, Gaze> gaze_;  ///< by agent id (kept after they leave)
};

void registerAttentionCommands(commands::CommandRegistry& registry, AttentionModel& targets);

/// The scene section "attention": the model's settings, saved with a scene when they differ
/// from the defaults and restored (or reset to the defaults) when one is loaded.
void registerAttentionSceneSection(assets::SceneSections& sections, AttentionModel& model);

}  // namespace crowdsim::metrics

// clang-format off
CROWDSIM_REFLECT(crowdsim::metrics::AttentionParams,
    field("evaluationHz", &T::evaluationHz).between(1, 30).unit("/s").label("Gaze updates").tip(
        "How often each person's gaze is evaluated"),
    field("eyeHeight", &T::eyeHeight).between(0.5, 1).label("Eye height").tip("Eye height as a share of body height"),
    field("gazePitch", &T::gazePitch).between(-60, 30).unit("deg").tip(
        "Head pitch while walking (negative: looking down at the footing)"),
    field("phonePitch", &T::phonePitch).between(-90, 0).unit("deg").tip("Head pitch while looking at a phone"),
    field("fovealHalfAngle", &T::fovealHalfAngle).between(1, 30).unit("deg").label("Central cone").tip(
        "Half-angle of central (foveal) vision, used for reading and dwell"),
    field("fieldHalfAngle", &T::fieldHalfAngle).between(10, 90).unit("deg").label("Field of view").tip(
        "Half-angle of the field of view used for viewability"),
    field("peripheralHalfAngle", &T::peripheralHalfAngle).between(10, 110).unit("deg").label("Periphery").tip(
        "Half-angle of peripheral vision: what can draw a glance"),
    field("maxHeadYaw", &T::maxHeadYaw).between(0, 90).unit("deg").label("Head turn").tip(
        "How far the head turns from the walking direction to look at something"),
    field("glanceRate", &T::glanceRate).atLeast(0).unit("/s").tip(
        "Glances per second at a fully salient, large target alone in view (scaled by interest)"),
    field("glanceMin", &T::glanceMin).above(0).unit("s").label("Glance min").tip("Shortest glance"),
    field("glanceMax", &T::glanceMax).above(0).unit("s").label("Glance max").tip("Longest glance"),
    field("phoneEpisode", &T::phoneEpisode).above(0).unit("s").label("Phone stretch").tip(
        "Mean time spent looking at a phone at a stretch"),
    field("viewableShare", &T::viewableShare).between(0, 1).tip(
        "Share of a target that must be visible to count as viewable (industry standard: 0.5)"),
    field("viewableSeconds", &T::viewableSeconds).atLeast(0).unit("s").tip(
        "Continuous time visible to count as viewable (industry standard: 1 s)"),
    field("noticeSeconds", &T::noticeSeconds).atLeast(0).unit("s").tip(
        "Fixation time, at a readable angle and size, that counts as noticed"),
    field("readableAngle", &T::readableAngle).between(0, 90).unit("deg").tip(
        "Viewing angles steeper than this distort a target too much to read"),
    field("minAngularSize", &T::minAngularSize).atLeast(0).unit("%").label("Readable size").tip(
        "Share of the field of view below which a target is too small to read"),
    field("defaultRange", &T::defaultRange).above(0).unit("m").tip("Zone of a target without attention:range"),
    field("defaultSalience", &T::defaultSalience).between(0, 1).tip(
        "How eye-catching a target without attention:salience is"),
    check<T>([](const T& p) {
        if (p.glanceMax < p.glanceMin) {
            throw crowdsim::reflect::FieldError("glanceMax", "must be at least glanceMin");
        }
    }))
// clang-format on
