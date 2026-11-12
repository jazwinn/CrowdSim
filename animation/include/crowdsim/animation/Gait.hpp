#pragma once

/// @file Gait.hpp
/// Per-agent gait controller: turns ground speed and turn rate into two baked
/// clip samples and a blend weight for the crowd vertex shader.
///
/// Foot sliding is avoided by advancing the shared walk/run phase by distance
/// travelled divided by the clip's cycle distance (rather than by time), so a
/// planted foot moves backwards exactly as fast as the agent moves forwards.
/// Walk and run are authored with the same phase layout (left heel strike at 0)
/// so they blend without feet crossing.

#include <cstdint>

#include "crowdsim/snapshot/CharacterAsset.hpp"

namespace crowdsim::animation {

struct GaitState {
    float gaitPhase = 0.0f;   ///< [0, 1) shared by walk and run
    float idlePhase = 0.0f;   ///< [0, 1)
    float turnPhase = 0.0f;   ///< [0, 1)
    float blendSpeed = 0.0f;  ///< smoothed speed used for clip weights (not for the phase)
    bool initialized = false;
};

/// Two clip samples (texture rows + interpolation fraction) and their blend.
struct GaitSample {
    float rowA0 = 0.0f;
    float rowA1 = 0.0f;
    float fracA = 0.0f;
    float blend = 0.0f;  ///< 0 = clip A only, 1 = clip B only
    float rowB0 = 0.0f;
    float rowB1 = 0.0f;
    float fracB = 0.0f;
    snapshot::ClipId clipA = snapshot::ClipId::Idle;
    snapshot::ClipId clipB = snapshot::ClipId::Walk;
};

struct GaitTuning {
    float idleToWalkStart = 0.06f;  ///< m/s where walking starts to blend in
    float idleToWalkEnd = 0.45f;    ///< m/s at which the pose is fully walking
    float walkToRunStart = 2.0f;
    float walkToRunEnd = 2.7f;
    float turnStart = 0.5f;  ///< rad/s where turning in place blends in (when nearly stopped)
    float turnEnd = 1.3f;
    float blendResponse = 6.0f;  ///< 1/s smoothing of the speed used for clip weights
};

/// Initial state for an agent; `seed` (e.g. the agent id) desynchronises idles.
[[nodiscard]] GaitState initialGaitState(std::uint32_t seed);

/// Advance `state` by `dt` seconds at ground `speed` (m/s) and `turnRate`
/// (rad/s), for a character scaled by `scale`, and return what to draw.
GaitSample updateGait(const snapshot::CharacterAsset& character, GaitState& state, float speed, float turnRate,
                      float scale, float dt, const GaitTuning& tuning = {});

/// Rows and fraction for `clip` at `phase` in [0, 1).
void clipRows(const snapshot::AnimationClip& clip, float phase, float& row0, float& row1, float& frac);

}  // namespace crowdsim::animation
