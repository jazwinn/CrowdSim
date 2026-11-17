// Queue manners (SetQueueManners): instead of pressing into the person in front,
// slow to a stop `gap` m behind them and side-step only slowly while waiting.
//
// Who counts as "in front" depends on the situation:
// - lining up for an escalator: anyone nearer the foot in the same lane's line, or
//   directly in your path, measured along the escalator (an order, so no cycles);
// - anywhere else: anyone directly in your path, measured along your own heading.
//   Two people can each be "in front" of the other (merging at an angle); then only
//   the one with the higher id waits, so they never wait on each other.
//
// Reads only state that is fixed during the parallel preferred-velocity pass
// (positions, last step's velocities, lanes assigned in the serial pass).

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::sim {

namespace {

constexpr float kFile = 0.45f;         ///< lateral offset within which someone is "in your path"
constexpr float kWaitingSide = 0.35f;  ///< side-step speed kept while waiting (fraction)
constexpr float kMovingSpeed = 0.1f;   ///< m/s above which another agent's heading is known

}  // namespace

glm::vec2 Simulation::applyQueueManners(std::uint32_t i, glm::vec2 pref) const {
    const QueueManners m = agents_.manners[i];
    const glm::vec2 pos = agents_.position[i];
    const float height = agents_.height[i];
    const float prefSpeed = std::sqrt(glm::dot(pref, pref));
    if (prefSpeed < 1e-3f) {
        return pref;
    }
    if (!escalators_.empty() && onEscalator(pos, height, agents_.rider[i].clearLanding)) {
        return pref;  // carried by the steps, or stepping off: stopping there blocks the riders behind
    }
    bool atFoot = false;
    const Escalator* line = escalators_.empty() ? nullptr : queueingFor(i, atFoot);
    if (atFoot) {
        return pref;  // stepping on: the escalator's boarding rules decide
    }
    const glm::vec2 fwd = line != nullptr ? line->forward : pref / prefSpeed;
    const glm::vec2 right(fwd.y, -fwd.x);
    const std::int8_t lane = agents_.escalatorLane[i];

    const float look = m.gap + m.ease + 0.5f;
    float ahead = look;
    hash_.queryPoints(pos, look, [&](std::uint32_t j, glm::vec2 other, float otherHeight) {
        if (j == i || std::abs(otherHeight - height) > 1.5f) {
            return;
        }
        const glm::vec2 d = other - pos;
        const float along = glm::dot(d, fwd);
        if (along <= 0.0f) {
            return;
        }
        const bool inPath = std::abs(glm::dot(d, right)) < kFile;
        if (line != nullptr) {
            if (inPath ||
                (agents_.escalatorLane[j] == lane && agents_.escalatorIndex[j] == agents_.escalatorIndex[i])) {
                ahead = std::min(ahead, along);
            }
            return;
        }
        if (!inPath) {
            return;
        }
        // Would they wait for me too? Then the lower id goes first.
        const glm::vec2 vj = agents_.velocity[j];
        const float sj = std::sqrt(glm::dot(vj, vj));
        if (sj > kMovingSpeed) {
            const glm::vec2 fj = vj / sj;
            const glm::vec2 back = -d;
            const bool mutual = glm::dot(back, fj) > 0.0f && std::abs(glm::dot(back, glm::vec2(fj.y, -fj.x))) < kFile;
            if (mutual && agents_.id[i] < agents_.id[j]) {
                return;
            }
        }
        ahead = std::min(ahead, along);
    });

    const float go = std::clamp((ahead - m.gap) / m.ease, 0.0f, 1.0f);
    const float forward = glm::dot(pref, fwd);
    const glm::vec2 side = pref - fwd * forward;
    return fwd * (forward > 0.0f ? forward * go : forward) + side * std::max(go, kWaitingSide);
}

}  // namespace crowdsim::sim
