// Escalators: stairs or ramp objects tagged "escalator" become moving belts.
//
// Agents keep ground-frame velocities everywhere, so avoidance, metrics and the
// rest of the step need no special cases: on the belt an agent's preferred
// velocity is the belt velocity plus its own walking (zero for standers) plus a
// small correction towards its lane. Before boarding, agents line up behind the
// lane they chose, and standers wait for a gap of about two steps.
//
// Object tags (besides "escalator"):
//   escalator:speed=<m/s>   step speed along the slope (default 0.5)
//   escalator:walk=<factor> walkers' pace on the steps, x preferred speed (default 0.45)
//   escalator:gap=<m>       spacing between standers in one lane (default 0.9)
//   escalator:down          travel from the top edge to the bottom edge
// How each agent rides (side, standing or walking) is behaviour, set from its
// behaviour tree with the SetEscalatorStyle node; agents that never run it walk
// on whichever side has the shorter line.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

#include "crowdsim/core/Profile.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::sim {

namespace {

constexpr float kApproachLength = 10.0f;  ///< m before the boarding edge where agents line up
constexpr float kApproachWiden = 3.0f;   ///< extra half-width of the approach funnel, m
constexpr float kLevelTolerance = 1.0f;  ///< m of height mismatch still counted as on the belt
constexpr float kBoardWindow = 1.2f;     ///< m before the edge where standers wait for a gap
constexpr float kExitClear = 2.0f;   ///< m people walk straight on after stepping off
constexpr float kQueueLook = 1.6f;   ///< m ahead people look for someone to queue behind
constexpr float kQueueFile = 0.45f;  ///< lateral offset within which someone counts as "in front"
constexpr float kQueueGap = 0.6f;    ///< m (centre to centre) at which people stop behind someone
constexpr float kQueueEase = 0.5f;   ///< m over which they slow down before that

// Escalator style per agent (AgentPool::escalatorStyle): bit 0 = stands,
// bits 1-2 = side (0 either, 1 left, 2 right).
constexpr std::uint8_t kStands = 1;
constexpr std::uint8_t kSideLeft = 2;
constexpr std::uint8_t kSideRight = 4;

/// Value of "escalator:<key>=<number>" among `tags`, or `fallback`.
float tagNumber(const std::vector<std::string>& tags, std::string_view key, float fallback) {
    for (const std::string& t : tags) {
        const std::string_view v(t);
        const std::string prefix = "escalator:" + std::string(key) + "=";
        if (v.starts_with(prefix)) {
            float out = fallback;
            const std::string_view num = v.substr(prefix.size());
            if (std::from_chars(num.data(), num.data() + num.size(), out).ec == std::errc{} && out >= 0.0f) {
                return out;
            }
        }
    }
    return fallback;
}

/// Where `p` is relative to an escalator: s along the travel direction from the
/// boarding edge, l to the right of the centre line.
struct Local {
    float s;
    float l;
};
Local toLocal(const Escalator& e, glm::vec2 p) {
    const glm::vec2 d = p - e.entry;
    return {glm::dot(d, e.forward), glm::dot(d, e.right)};
}
float floorHeight(const Escalator& e, float s) {
    const float t = std::clamp(s / std::max(e.length, 0.01f), 0.0f, 1.0f);
    return e.entryHeight + (e.exitHeight - e.entryHeight) * t;
}
bool onBelt(const Escalator& e, Local q, float height) {
    return q.s >= 0.0f && q.s <= e.length && std::abs(q.l) <= e.halfWidth &&
           std::abs(height - floorHeight(e, q.s)) < kLevelTolerance;
}
bool inExit(const Escalator& e, Local q, float height) {
    return q.s > e.length && q.s <= e.length + kExitClear && std::abs(q.l) <= e.halfWidth + 0.5f &&
           std::abs(height - e.exitHeight) < kLevelTolerance;
}
bool inApproach(const Escalator& e, Local q, float height) {
    return q.s < 0.0f && q.s >= -kApproachLength && std::abs(q.l) <= e.halfWidth + kApproachWiden &&
           std::abs(height - e.entryHeight) < kLevelTolerance;
}

}  // namespace

std::uint8_t Simulation::escalatorStyleOf(std::string_view side, bool walks) {
    std::uint8_t style = walks ? 0 : kStands;
    if (side == "left") {
        style |= kSideLeft;
    } else if (side == "right") {
        style |= kSideRight;
    }
    return style;
}

void Simulation::rebuildEscalators() {
    escalators_.clear();
    for (entt::entity ent : staticObjects_) {
        const StaticObject& o = registry_.get<StaticObject>(ent);
        if (std::find(o.tags.begin(), o.tags.end(), "escalator") == o.tags.end()) {
            continue;
        }
        const geometry::PrimitiveDesc& p = o.primitive;
        if (p.kind != geometry::PrimitiveKind::Stairs && p.kind != geometry::PrimitiveKind::Ramp) {
            continue;
        }
        // Local +Z (up-slope) in world space; see geometry::MeshBuilder::toWorld.
        const glm::vec2 up(std::sin(p.rotationY), std::cos(p.rotationY));
        const glm::vec2 centre(p.position.x, p.position.z);
        const glm::vec2 bottom = centre - up * (0.5f * p.size.z);
        const glm::vec2 top = centre + up * (0.5f * p.size.z);
        const bool down = std::find(o.tags.begin(), o.tags.end(), "escalator:down") != o.tags.end();

        Escalator e;
        e.object = registry_.get<SceneIdComponent>(ent).id;
        e.entry = down ? top : bottom;
        e.forward = down ? -up : up;
        e.right = glm::vec2(e.forward.y, -e.forward.x);  // +Z facing: +X is on the right
        e.length = p.size.z;
        e.halfWidth = 0.5f * p.size.x;
        e.entryHeight = down ? p.position.y + p.size.y : p.position.y;
        e.exitHeight = down ? p.position.y : p.position.y + p.size.y;
        const float slope = std::sqrt(p.size.z * p.size.z + p.size.y * p.size.y);
        e.beltSpeed = tagNumber(o.tags, "speed", 0.5f) * p.size.z / std::max(slope, 0.01f);
        e.walkFactor = tagNumber(o.tags, "walk", 0.45f);
        e.standGap = tagNumber(o.tags, "gap", 0.9f);
        // Keep both lanes inside the walkable strip: Recast erodes the navmesh by the
        // agent radius rounded up to whole cells plus about one more cell where the
        // surface is rasterised, and riders need their own radius from its edge too.
        const NavBuildSettings& nav = config_.navigation;
        const float eroded = std::ceil(nav.agentRadius / nav.cellSize) * nav.cellSize + nav.cellSize;
        const float usable = std::max(0.0f, e.halfWidth - eroded - 0.25f);
        e.laneOffset = std::min(0.3f, usable);
        escalators_.push_back(e);
    }
}

void Simulation::assignEscalatorLanes() {
    if (escalators_.empty()) {
        return;
    }
    CS_PROFILE_ZONE();

    // Serial and in index order, so the choice of lane is deterministic. Agents
    // already in a lane are counted first; newcomers then pick the shorter line.
    std::vector<std::uint32_t> count(escalators_.size() * 2, 0);
    std::vector<std::int16_t> where(agents_.size(), -1);
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        for (std::size_t k = 0; k < escalators_.size(); ++k) {
            const Escalator& e = escalators_[k];
            const Local q = toLocal(e, agents_.position[i]);
            if (inApproach(e, q, agents_.height[i]) || onBelt(e, q, agents_.height[i])) {
                where[i] = static_cast<std::int16_t>(k);
                if (agents_.escalatorLane[i] != 0 && q.s < 3.0f) {
                    ++count[k * 2 + (agents_.escalatorLane[i] > 0 ? 1 : 0)];
                }
                break;
            }
        }
    }
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        if (where[i] < 0) {
            agents_.escalatorLane[i] = 0;  // left the escalator (or never used one)
            continue;
        }
        const Escalator& e = escalators_[static_cast<std::size_t>(where[i])];
        if (agents_.escalatorLane[i] != 0) {
            // Either-side riders stuck at the foot on the other lane's line take that
            // lane instead of cutting across the people waiting there.
            const std::uint8_t r = agents_.escalatorStyle[i];
            const Local q = toLocal(e, agents_.position[i]);
            const float laneL = static_cast<float>(agents_.escalatorLane[i]) * e.laneOffset;
            const bool sided = (r & (kSideLeft | kSideRight)) != 0;
            if (sided && q.s < 0.0f) {
                // The behaviour tree may set the style after a lane was picked (trees tick
                // at a low, staggered rate): until boarding, a chosen side always wins.
                agents_.escalatorLane[i] = (r & kSideLeft) != 0 ? std::int8_t{-1} : std::int8_t{1};
                continue;
            }
            const glm::vec2 v = agents_.velocity[i];
            const bool stuck = glm::dot(v, v) < 0.15f * 0.15f;
            if (!sided && stuck && q.s < 0.0f && q.s > -kBoardWindow - 1.0f &&
                std::abs(q.l - laneL) > e.laneOffset * 1.5f) {
                agents_.escalatorLane[i] = static_cast<std::int8_t>(-agents_.escalatorLane[i]);
            }
            continue;
        }
        // Only agents heading the way the escalator travels take a lane.
        const glm::vec2 want = agents_.preferredVelocity[i];
        if (glm::dot(want, want) > 1e-6f && glm::dot(want, e.forward) <= 0.2f * std::sqrt(glm::dot(want, want))) {
            continue;
        }
        const std::uint8_t role = agents_.escalatorStyle[i];
        std::int8_t lane = 0;
        if ((role & kSideLeft) != 0) {
            lane = -1;
        } else if ((role & kSideRight) != 0) {
            lane = 1;
        } else {
            const std::uint32_t left = count[static_cast<std::size_t>(where[i]) * 2];
            const std::uint32_t right = count[static_cast<std::size_t>(where[i]) * 2 + 1];
            // Keep to the side you are already on; near the middle, join the shorter line.
            const float l = toLocal(e, agents_.position[i]).l;
            if (std::abs(l) > e.laneOffset) {
                lane = l < 0.0f ? std::int8_t{-1} : std::int8_t{1};
            } else if (left != right) {
                lane = left < right ? std::int8_t{-1} : std::int8_t{1};
            } else {
                lane = l < 0.0f ? std::int8_t{-1} : std::int8_t{1};
            }
        }
        agents_.escalatorLane[i] = lane;
        ++count[static_cast<std::size_t>(where[i]) * 2 + (lane > 0 ? 1 : 0)];
    }
}

glm::vec2 Simulation::escalatorSteering(std::uint32_t i, glm::vec2 pref) const {
    const glm::vec2 pos = agents_.position[i];
    const float height = agents_.height[i];
    for (const Escalator& e : escalators_) {
        const Local q = toLocal(e, pos);
        if (inExit(e, q, height)) {
            // Step off and clear the landing before turning towards your destination;
            // turning on the last step pins people against the end of the balustrade.
            // Someone at the edge steps inwards first, rather than along the edge.
            const float speed = agents_.preferredSpeed[i] * agents_.speedMultiplier[i];
            const float edge = std::clamp((std::abs(q.l) - 0.4f) / 0.3f, 0.0f, 0.8f);
            return e.forward * (speed * (1.0f - edge)) - e.right * std::clamp(q.l * 1.5f, -0.8f, 0.8f);
        }
        const bool riding = onBelt(e, q, height);
        if (!riding && !inApproach(e, q, height)) {
            continue;
        }
        const std::int8_t lane = agents_.escalatorLane[i];
        const std::uint8_t role = agents_.escalatorStyle[i];
        const bool stands = (role & kStands) != 0;
        const float speed = agents_.preferredSpeed[i] * agents_.speedMultiplier[i];
        const float laneL = static_cast<float>(lane) * e.laneOffset;

        if (riding) {
            // Carried by the steps; drift into the lane; walkers climb on top of that.
            float sideways = lane != 0 ? std::clamp((laneL - q.l) * 2.0f, -0.4f, 0.4f) : 0.0f;
            if (stands && std::abs(q.l - laneL) > 0.15f) {
                // Someone who stepped on in the other lane only moves across into a
                // proper gap; otherwise they stay where they are.
                bool occupied = false;
                hash_.queryPoints(pos, e.standGap + 0.5f, [&](std::uint32_t j, glm::vec2 other, float otherHeight) {
                    if (j == i || occupied || std::abs(otherHeight - height) > 1.5f) {
                        return;
                    }
                    const Local o = toLocal(e, other);
                    occupied = std::abs(o.l - laneL) < 0.2f && std::abs(o.s - q.s) < e.standGap;
                });
                if (occupied) {
                    sideways = 0.0f;
                }
            }
            if (std::abs(q.l) > e.laneOffset + 0.1f) {
                // Never ride pressed against the balustrade: the edge of the steps turns
                // a corner at the top, where someone touching it would be pinned.
                sideways = q.l > 0.0f ? -0.3f : 0.3f;
            }
            const float own = stands ? 0.0f : speed * e.walkFactor;
            return e.forward * (e.beltSpeed + own) + e.right * sideways;
        }
        if (lane == 0) {
            return pref;  // not boarding (e.g. walking past the foot of the escalator)
        }
        // Approach: line up on the lane's centre line, then walk onto the steps.
        const glm::vec2 aim = e.entry + e.right * laneL + e.forward * std::min(q.s + 1.5f, 0.5f);
        glm::vec2 to = aim - pos;
        const float dist = std::sqrt(glm::dot(to, to));
        to = dist > 1e-4f ? to / dist : e.forward;
        glm::vec2 out = to * speed;
        {
            // Queue manners: one line per lane. Slow to a stop behind whoever is ahead
            // in your lane's line, or directly in your path, instead of pressing into
            // them; while waiting, side-step only slowly. Yielding only to people nearer
            // the escalator keeps it free of cycles.
            float ahead = kApproachLength;
            hash_.queryPoints(pos, kQueueLook, [&](std::uint32_t j, glm::vec2 other, float otherHeight) {
                if (j == i || std::abs(otherHeight - height) > 1.5f) {
                    return;
                }
                const Local o = toLocal(e, other);
                const bool sameLine = agents_.escalatorLane[j] == lane;
                if (o.s > q.s && (sameLine || std::abs(o.l - q.l) < kQueueFile)) {
                    ahead = std::min(ahead, o.s - q.s);
                }
            });
            const float go = std::clamp((ahead - kQueueGap) / kQueueEase, 0.0f, 1.0f);
            const float fwd = glm::dot(out, e.forward);
            const glm::vec2 side = out - e.forward * fwd;
            out = e.forward * (fwd > 0.0f ? fwd * go : fwd) + side * std::max(go, 0.35f);
        }
        if (q.s > -kBoardWindow) {
            // People slow to roughly the step speed as they step on, so they do not
            // run into the rider in front once the belt takes over.
            out = to * std::min(speed, e.beltSpeed + 0.35f);
            // At the foot: step on only when lined up with the lane (so nobody slips
            // past a waiting stander into the other lane), and standers only once the
            // person ahead of them is a gap away.
            const float sideways = std::clamp((laneL - q.l) * 2.0f, -0.4f, 0.4f);
            const float offLane = std::abs(q.l - laneL);
            // Someone pushed to the very front on the wrong side steps on anyway and
            // shuffles across on the steps (blocking the other lane meanwhile, as CNA
            // saw at City Hall); otherwise the two lines could lock each other.
            const glm::vec2 v = agents_.velocity[i];
            const bool pressed = q.s > -0.6f && glm::dot(v, v) < 0.15f * 0.15f;
            bool blocked = offLane > 0.3f && !pressed;
            if (!blocked && offLane > 0.12f) {
                // Nearly lined up (or squeezing on): shuffle forward slowly while side-stepping.
                out = e.right * sideways + e.forward * 0.15f;
            }
            if (stands && !blocked) {
                hash_.queryPoints(pos, e.standGap + 0.5f, [&](std::uint32_t j, glm::vec2 other, float otherHeight) {
                    if (j == i || blocked || std::abs(otherHeight - height) > 1.5f) {
                        return;
                    }
                    const Local o = toLocal(e, other);
                    blocked = std::abs(o.l - laneL) < 0.45f && o.s > q.s && o.s - q.s < e.standGap + 0.15f;
                });
            }
            if (blocked) {
                out = e.right * sideways;
            }
        }
        return out;
    }
    return pref;
}

bool Simulation::onEscalator(glm::vec2 position, float height) const {
    for (const Escalator& e : escalators_) {
        const Local q = toLocal(e, position);
        if (onBelt(e, q, height) || inExit(e, q, height)) {
            return true;
        }
    }
    return false;
}

glm::vec2 Simulation::beltVelocityAt(glm::vec2 position, float height) const {
    for (const Escalator& e : escalators_) {
        if (onBelt(e, toLocal(e, position), height)) {
            return e.forward * e.beltSpeed;
        }
    }
    return glm::vec2(0.0f);
}

}  // namespace crowdsim::sim
