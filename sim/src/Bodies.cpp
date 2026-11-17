// Bodies in packed crowds (BodyParams, scene "bodies").
//
// Below contact density agents avoid each other (ORCA) and never touch. When bodies
// do touch - a crowd packed past what avoidance can keep apart - the touching agents
// are moved by forces instead:
//   - contact: bodies are springs (stiffness x overlap) with an incompressible core,
//     damped along the contact normal;
//   - own push: each person presses towards where they want to go with at most their
//     push (SetPushing), resisting being shoved with their footing;
//   - slope: packed people on an incline lean on those below them (slopeLean x the
//     downhill share of their weight);
//   - walls: the navmesh boundary stops the body; its reaction is measured.
// Forces add up along chains of touching people, so someone at the front of a packed
// crowd is pressed by everyone pushing behind them. Pressure is the stress in the body
// from those contacts (the virial of the contact forces over the floor the body
// occupies), in N/m like the crowd-safety literature.
//
// Falls: a standing person is toppled at a rate that grows with the net push on them
// above fallForce, and trips over someone lying in the direction they are pushed.
// Fallen people lie still (a larger, immovable footprint), others fall on top of them
// (pile layers), and they get up once the pressure on them has gone.
//
// computeBodies runs in parallel and only writes the agent's own columns (neighbours'
// state is read as it was after avoidance); falls are applied serially in index order,
// with each agent's own RNG, so results do not depend on the thread count.

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/geometric.hpp>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::sim {

namespace {

constexpr float kGravity = 9.81f;
constexpr float kLying = 1.6f;            ///< footprint radius of someone lying, x their radius
constexpr float kLyingStiffness = 0.3f;   ///< contact with someone lying down, x stiffness
constexpr float kCoreRelax = 0.5f;        ///< share of a core overlap undone per step
constexpr float kAreaPerRadiusSq = 3.5f;  ///< floor per body at the tightest packing, x r^2
constexpr float kRelax = 0.5f;            ///< s; how quickly people correct their own velocity
constexpr float kMinContactSpeed = 0.3f;  ///< m/s anyone still shuffles at when touching others
constexpr float kFooting = 0.6f;          ///< max own force, x body weight
constexpr float kMaxWallForce = 4000.0f;  ///< N, clamp for the measured wall reaction
constexpr std::uint32_t kMaxContacts = 24;
constexpr std::uint32_t kParallelChunk = 256;

struct Near {
    std::uint32_t index;
    float distSq;
};

}  // namespace

void Simulation::computeBodies(float dt) {
    const BodyParams& bp = config_.bodies;
    const float levelSep = config_.locomotion.levelSeparation;
    const bool useNav = navMesh_.valid();

    services_.jobs.parallelFor(agents_.size(), kParallelChunk, [&](core::IndexRange r, std::uint32_t) {
        std::array<Near, kMaxContacts> near{};
        std::array<glm::vec2, 8> lyingAhead{};
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            agents_.bodyRequest[i] = 0;
            if (agents_.linkActive[i] != 0) {
                agents_.bodyForce[i] = glm::vec2(0.0f);
                agents_.pressure[i] = 0.0f;
                agents_.inContact[i] = 0;
                continue;
            }
            const glm::vec2 p = agents_.position[i];
            const float h = agents_.height[i];
            const float ri = agents_.radius[i];
            const bool down = agents_.fallen[i] != 0;
            const float reach = (down ? ri * kLying : ri) + 0.35f * kLying + 0.05f;

            // Nearest bodies on this level, ordered by (distance, index): sums are then
            // taken in a fixed order whatever the hash layout.
            std::uint32_t count = 0;
            hash_.queryPoints(p, reach, [&](std::uint32_t j, glm::vec2 q, float qh) {
                if (j == i || std::abs(qh - h) > levelSep) {
                    return;
                }
                const glm::vec2 d = q - p;
                const float d2 = glm::dot(d, d);
                if (d2 > reach * reach) {
                    return;
                }
                if (count == kMaxContacts &&
                    (d2 > near[count - 1].distSq || (d2 == near[count - 1].distSq && j > near[count - 1].index))) {
                    return;
                }
                std::uint32_t slot = count < kMaxContacts ? count++ : count - 1;
                while (slot > 0 &&
                       (near[slot - 1].distSq > d2 || (near[slot - 1].distSq == d2 && near[slot - 1].index > j))) {
                    near[slot] = near[slot - 1];
                    --slot;
                }
                near[slot] = Near{j, d2};
            });

            glm::vec2 force(0.0f);
            glm::vec2 separation(0.0f);  // core overlap to undo, as a velocity
            float virial = 0.0f;         // sum of force x centre distance over contacts, N m
            float weight = 0.0f;         // people lying on top of this one, N
            std::uint32_t contacts = 0;
            std::uint32_t numLying = 0;
            for (std::uint32_t k = 0; k < count; ++k) {
                const std::uint32_t j = near[k].index;
                const float rj = agents_.radius[j];
                const bool otherDown = agents_.fallen[j] != 0;
                const float d = std::sqrt(near[k].distSq);
                if (down && otherDown) {
                    // A pile: no sideways contact, but those above weigh on those below.
                    if (agents_.pileLayer[j] > agents_.pileLayer[i] && d < 0.6f * kLying * (ri + rj)) {
                        weight += 0.5f * agents_.mass[j] * kGravity;
                    }
                    continue;
                }
                const bool lyingPair = down || otherDown;
                const float reachSum = (down ? ri * kLying : ri) + (otherDown ? rj * kLying : rj);
                if (d >= reachSum) {
                    continue;
                }
                glm::vec2 n;
                if (d > 1e-5f) {
                    n = (p - agents_.position[j]) / d;
                } else {
                    // Exactly on top of each other: split along a direction fixed by the ids.
                    const float a = 0.618034f * static_cast<float>(std::min(agents_.id[i], agents_.id[j])) * 6.2831853f;
                    n = glm::vec2(std::cos(a), std::sin(a)) * (agents_.id[i] < agents_.id[j] ? 1.0f : -1.0f);
                }
                // A spring up to the incompressible core, where it stops growing; deeper
                // overlap is undone directly (half by each body) instead of by a stiffer
                // spring, which a 30 Hz step could not integrate stably. Someone lying is
                // stepped on rather than pushed: a soft contact without a core.
                const float stiffness = lyingPair ? kLyingStiffness * bp.stiffness : bp.stiffness;
                const float coreDist = lyingPair ? 0.0f : bp.core * reachSum;
                const float squeeze = std::min(reachSum - d, reachSum - coreDist);
                const float approach = glm::dot(agents_.velocity[i] - agents_.velocity[j], n);
                const float f = std::max(0.0f, stiffness * squeeze - bp.damping * approach);
                force += f * n;
                virial += f * d;
                ++contacts;
                if (d < coreDist) {
                    separation += n * (kCoreRelax * 0.5f * (coreDist - d) / dt);
                }
                if (!down && otherDown && numLying < lyingAhead.size()) {
                    lyingAhead[numLying++] = -n;  // towards the person lying there
                }
            }
            // Pressure as body stress: the virial of the contact forces over the body's
            // share of floor (2D: P = sum(f d) / (4 A), each contact shared by two bodies).
            const float bodyArea = kAreaPerRadiusSq * ri * ri;

            if (down) {
                agents_.fallenFor[i] += dt;
                agents_.bodyForce[i] = glm::vec2(0.0f);
                agents_.separation[i] = glm::vec2(0.0f);
                agents_.inContact[i] = 1;
                // Weight of those on top presses the body into the ground over its length.
                agents_.pressure[i] = virial / (4.0f * bodyArea) + weight / (2.0f * kLying * ri);
                if (agents_.fallenFor[i] >= bp.getUpSeconds && weight == 0.0f &&
                    agents_.pressure[i] < bp.getUpPressure) {
                    agents_.bodyRequest[i] = 2;
                }
                continue;
            }

            const bool touching = contacts > 0;
            glm::vec2 lean(0.0f);
            if (touching && useNav && bp.slopeLean > 0.0f) {
                const std::uint32_t dense = navMesh_.denseIndex(agents_.navPoly[i]);
                if (dense != kNoPoly) {
                    const glm::vec2 g = navMesh_.polyGradient(dense);
                    lean = -g * (bp.slopeLean * agents_.mass[i] * kGravity / std::sqrt(1.0f + glm::dot(g, g)));
                }
            }
            const glm::vec2 wall = touching ? agents_.wallForce[i] : glm::vec2(0.0f);
            const float wallLoad = std::sqrt(glm::dot(wall, wall));
            agents_.bodyForce[i] = force + lean;
            agents_.separation[i] = separation;
            agents_.inContact[i] = touching ? 1 : 0;
            agents_.pressure[i] = touching ? (virial + 2.0f * wallLoad * ri) / (4.0f * bodyArea) : 0.0f;

            // Toppling: the net push the person cannot stand against.
            const glm::vec2 net = force + lean + wall;
            const float push = std::sqrt(glm::dot(net, net));
            float rate = 0.0f;
            if (push > bp.fallForce && bp.fallForce > 0.0f) {
                rate += bp.fallRate * (push / bp.fallForce - 1.0f);
            }
            if (bp.tripForce > 0.0f) {
                for (std::uint32_t k = 0; k < numLying; ++k) {
                    const float towards = glm::dot(net, lyingAhead[k]);
                    if (towards > bp.tripForce) {
                        rate += bp.tripRate * std::min(3.0f, towards / bp.tripForce);
                    }
                }
            }
            if (rate > 0.0f && agents_.rng[i].nextFloat() < 1.0f - std::exp(-rate * dt)) {
                agents_.bodyRequest[i] = 1;
            }
        }
    });
}

void Simulation::applyBodyRequests() {
    const float levelSep = config_.locomotion.levelSeparation;
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        const std::uint8_t request = agents_.bodyRequest[i];
        if (request == 1 && agents_.fallen[i] == 0) {
            // Lands on whoever is already down under them.
            std::uint8_t below = 0;
            const glm::vec2 p = agents_.position[i];
            const float ri = agents_.radius[i];
            hash_.query(p, 1.2f, [&](std::uint32_t j) {
                if (j != i && agents_.fallen[j] != 0 && std::abs(agents_.height[j] - agents_.height[i]) < levelSep) {
                    const glm::vec2 d = agents_.position[j] - p;
                    const float reach = 0.6f * kLying * (ri + agents_.radius[j]);
                    if (glm::dot(d, d) < reach * reach) {
                        below = std::max(below, agents_.pileLayer[j]);
                    }
                }
            });
            agents_.fallen[i] = 1;
            agents_.fallenFor[i] = 0.0f;
            agents_.pileLayer[i] = static_cast<std::uint8_t>(std::min(6, below + 1));
            agents_.velocity[i] = glm::vec2(0.0f);
            ++fallsTotal_;
        } else if (request == 2 && agents_.fallen[i] != 0) {
            agents_.fallen[i] = 0;
            agents_.fallenFor[i] = 0.0f;
            agents_.pileLayer[i] = 0;
        }
        agents_.bodyRequest[i] = 0;
    }
}

glm::vec2 Simulation::bodyVelocity(std::uint32_t i, float dt) const {
    const float m = agents_.mass[i];
    const glm::vec2 v = agents_.velocity[i];
    // Where the person wants to go, at no more than their push can drive into a crowd
    // that has stopped moving.
    glm::vec2 wanted = agents_.preferredVelocity[i];
    const float wantedSpeed = std::sqrt(glm::dot(wanted, wanted));
    const float cap = std::max(kMinContactSpeed, agents_.push[i] * kRelax / m);
    if (wantedSpeed > cap) {
        wanted *= cap / wantedSpeed;
    }
    glm::vec2 own = m * (wanted - v) / kRelax;
    const float ownMax = kFooting * m * kGravity;
    const float ownLen = std::sqrt(glm::dot(own, own));
    if (ownLen > ownMax) {
        own *= ownMax / ownLen;
    }
    glm::vec2 out = v + (own + agents_.bodyForce[i]) * (dt / m) + agents_.separation[i];
    const float speed = std::sqrt(glm::dot(out, out));
    const float maxSpeed = std::max(agents_.maxSpeed[i], 1.5f);
    if (speed > maxSpeed) {
        out *= maxSpeed / speed;
    }
    return out;
}

glm::vec2 Simulation::clampWallForce(glm::vec2 f) noexcept {
    const float len = std::sqrt(glm::dot(f, f));
    return len > kMaxWallForce ? f * (kMaxWallForce / len) : f;
}

}  // namespace crowdsim::sim
