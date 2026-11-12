#include "crowdsim/animation/Gait.hpp"

#include <algorithm>
#include <cmath>

namespace crowdsim::animation {

namespace {

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float wrap01(float x) {
    x -= std::floor(x);
    return x >= 1.0f ? 0.0f : x;
}

/// Cheap integer hash -> [0, 1).
float hash01(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return static_cast<float>(x & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

}  // namespace

GaitState initialGaitState(std::uint32_t seed) {
    GaitState s;
    s.gaitPhase = hash01(seed * 3u + 1u);
    s.idlePhase = hash01(seed * 3u + 2u);
    s.turnPhase = hash01(seed * 3u + 3u);
    return s;
}

void clipRows(const snapshot::AnimationClip& clip, float phase, float& row0, float& row1, float& frac) {
    const float f = wrap01(phase) * static_cast<float>(clip.frameCount);
    const auto i = std::min(static_cast<std::uint32_t>(f), clip.frameCount - 1);
    const std::uint32_t next = (i + 1) % clip.frameCount;
    row0 = static_cast<float>(clip.firstRow + i);
    row1 = static_cast<float>(clip.firstRow + next);
    frac = f - static_cast<float>(i);
}

GaitSample updateGait(const snapshot::CharacterAsset& character, GaitState& state, float speed, float turnRate,
                      float scale, float dt, const GaitTuning& tuning) {
    using snapshot::ClipId;
    const snapshot::AnimationClip& idle = character.clip(ClipId::Idle);
    const snapshot::AnimationClip& walk = character.clip(ClipId::Walk);
    const snapshot::AnimationClip& run = character.clip(ClipId::Run);
    const snapshot::AnimationClip& turn = character.clip(ClipId::Turn);

    dt = std::clamp(dt, 0.0f, 0.25f);
    speed = std::max(0.0f, speed);
    if (!state.initialized) {
        state.blendSpeed = speed;
        state.initialized = true;
    } else {
        const float k = 1.0f - std::exp(-tuning.blendResponse * dt);
        state.blendSpeed += (speed - state.blendSpeed) * k;
    }
    const float s = state.blendSpeed;
    const float runWeight = smoothstep(tuning.walkToRunStart, tuning.walkToRunEnd, s);
    const float moveWeight = smoothstep(tuning.idleToWalkStart, tuning.idleToWalkEnd, s);

    // Distance-driven phase: one cycle per (scaled) cycle distance of the blended gait.
    const float cycle =
        std::max(0.05f, scale * (walk.cycleDistance + (run.cycleDistance - walk.cycleDistance) * runWeight));
    state.gaitPhase = wrap01(state.gaitPhase + speed * dt / cycle);
    state.idlePhase = wrap01(state.idlePhase + dt / std::max(0.1f, idle.duration));
    const float turnSpeed = std::abs(turnRate);
    const float turnWeight = smoothstep(tuning.turnStart, tuning.turnEnd, turnSpeed) * (1.0f - moveWeight);
    state.turnPhase =
        wrap01(state.turnPhase + dt / std::max(0.1f, turn.duration) * (0.6f + 0.4f * std::min(1.0f, turnSpeed / 2.0f)));

    GaitSample out;
    if (runWeight > 0.0f) {
        out.clipA = ClipId::Walk;
        out.clipB = ClipId::Run;
        clipRows(walk, state.gaitPhase, out.rowA0, out.rowA1, out.fracA);
        clipRows(run, state.gaitPhase, out.rowB0, out.rowB1, out.fracB);
        out.blend = runWeight;
    } else if (turnWeight > 0.0f && moveWeight < 0.5f) {
        out.clipA = ClipId::Idle;
        out.clipB = ClipId::Turn;
        clipRows(idle, state.idlePhase, out.rowA0, out.rowA1, out.fracA);
        clipRows(turn, state.turnPhase, out.rowB0, out.rowB1, out.fracB);
        out.blend = turnWeight;
    } else {
        out.clipA = ClipId::Idle;
        out.clipB = ClipId::Walk;
        clipRows(idle, state.idlePhase, out.rowA0, out.rowA1, out.fracA);
        clipRows(walk, state.gaitPhase, out.rowB0, out.rowB1, out.fracB);
        out.blend = moveWeight;
    }
    return out;
}

}  // namespace crowdsim::animation
