// Escalators: stairs or ramp objects tagged "escalator" become moving belts.
//
// This file is mechanics only: the steps carry their riders, there are two lanes,
// people line up on a lane before stepping on, keep off the balustrade, move
// across only into real space, and step inwards off the edge at the top. How a
// person rides - which side, standing or walking and how fast, the gap they leave,
// whether they squeeze on when pushed, how far they walk before turning - is
// behaviour: AgentPool::rider, set from behaviour trees by SetEscalatorStyle and
// SetEscalatorBoarding. Queue manners at the foot are the generic SetQueueManners
// (QueueManners.cpp), which uses the lanes here to define who is in your line.
//
// Agents keep ground-frame velocities everywhere, so avoidance, metrics and the
// rest of the step need no special cases: on the belt an agent's preferred velocity
// is the belt velocity plus its own walking plus a small correction towards its lane.
//
// Object tags (besides "escalator"):
//   escalator:speed=<m/s>   step speed along the slope (default 0.5)
//   escalator:down          travel from the top edge to the bottom edge

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
constexpr float kApproachWiden = 3.0f;    ///< extra half-width of the approach funnel, m
constexpr float kLevelTolerance = 1.0f;   ///< m of height mismatch still counted as on the belt
constexpr float kBoardWindow = 1.2f;      ///< m before the edge where boarding rules apply

// EscalatorRider::side values.
constexpr std::int8_t kLeft = -1;
constexpr std::int8_t kRight = 1;
constexpr std::int8_t kEither = 0;
constexpr std::int8_t kShorter = 2;

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
bool inExit(const Escalator& e, Local q, float height, float clear) {
    return q.s > e.length && q.s <= e.length + clear && std::abs(q.l) <= e.halfWidth + 0.5f &&
           std::abs(height - e.exitHeight) < kLevelTolerance;
}
bool inApproach(const Escalator& e, Local q, float height) {
    return q.s < 0.0f && q.s >= -kApproachLength && std::abs(q.l) <= e.halfWidth + kApproachWiden &&
           std::abs(height - e.entryHeight) < kLevelTolerance;
}

}  // namespace

std::int8_t Simulation::escalatorSideOf(std::string_view side) {
    if (side == "left") {
        return kLeft;
    }
    if (side == "right") {
        return kRight;
    }
    return side == "shorter" ? kShorter : kEither;
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
        // Keep both lanes inside the walkable strip: Recast erodes the navmesh by the
        // agent radius rounded up to whole cells plus about one more cell where the
        // surface is rasterised, and riders need their own radius from its edge too.
        const NavBuildSettings& nav = config_.navigation;
        const float eroded = std::ceil(nav.agentRadius / nav.cellSize) * nav.cellSize + nav.cellSize;
        const float usable = std::max(0.0f, e.halfWidth - eroded - 0.25f);
        e.laneOffset = std::min(0.3f, usable);
        escalators_.push_back(e);
    }

    // Escalators are one-way: flow fields never route people against the steps, so
    // up and down escalators side by side each carry their own direction.
    polyClimb_.assign(escalators_.empty() ? 0 : navMesh_.polyCount(), 0);
    for (std::uint32_t d = 0; d < polyClimb_.size(); ++d) {
        if (navMesh_.isOffMeshConnection(d)) {
            continue;
        }
        const glm::vec3 c = navMesh_.polyCenter(d);
        for (const Escalator& e : escalators_) {
            const Local q = toLocal(e, glm::vec2(c.x, c.z));
            if (q.s >= 0.0f && q.s <= e.length && std::abs(q.l) <= e.halfWidth + 0.3f &&
                std::abs(c.y - floorHeight(e, q.s)) < kLevelTolerance) {
                polyClimb_[d] = e.exitHeight > e.entryHeight ? 1 : -1;
                break;
            }
        }
    }
}

void Simulation::assignEscalatorLanes() {
    if (escalators_.empty()) {
        return;
    }
    CS_PROFILE_ZONE();

    // Serial and in index order, so the choice of lane is deterministic. Agents
    // already in a lane are counted first; newcomers then pick their lane.
    //
    // Each agent belongs to at most one escalator: the one whose steps it stands on,
    // else the one it took a lane for while still in that one's approach, else the
    // first whose approach it is in and whose steps its way on leads onto. (Approaches
    // overlap where escalators chain or pair up, so the lane must stay with its own.)
    std::vector<std::uint32_t> count(escalators_.size() * 2, 0);
    std::vector<std::int16_t> where(agents_.size(), -1);
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        const float h = agents_.height[i];
        std::int16_t found = -1;
        for (std::size_t k = 0; k < escalators_.size() && found < 0; ++k) {
            if (onBelt(escalators_[k], toLocal(escalators_[k], agents_.position[i]), h)) {
                found = static_cast<std::int16_t>(k);
            }
        }
        const std::uint16_t own = agents_.escalatorIndex[i];
        if (found < 0 && agents_.escalatorLane[i] != 0 && own < escalators_.size() &&
            inApproach(escalators_[own], toLocal(escalators_[own], agents_.position[i]), h)) {
            found = static_cast<std::int16_t>(own);
        }
        if (found >= 0 && agents_.escalatorLane[i] != 0 && own != static_cast<std::uint16_t>(found)) {
            agents_.escalatorLane[i] = 0;  // a lane on another escalator does not carry over
        }
        for (std::size_t k = 0; k < escalators_.size() && found < 0; ++k) {
            const Escalator& e = escalators_[k];
            if (inApproach(e, toLocal(e, agents_.position[i]), h) && boardsEscalator(i, e)) {
                found = static_cast<std::int16_t>(k);
            }
        }
        where[i] = found;
        if (found >= 0 && agents_.escalatorLane[i] != 0 &&
            toLocal(escalators_[static_cast<std::size_t>(found)], agents_.position[i]).s < 3.0f) {
            ++count[static_cast<std::size_t>(found) * 2 + (agents_.escalatorLane[i] > 0 ? 1 : 0)];
        }
    }
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        if (where[i] < 0) {
            agents_.escalatorLane[i] = 0;  // left the escalator (or never used one)
            continue;
        }
        const Escalator& e = escalators_[static_cast<std::size_t>(where[i])];
        const EscalatorRider& r = agents_.rider[i];
        const bool sided = r.side == kLeft || r.side == kRight;
        if (agents_.escalatorLane[i] != 0) {
            const Local q = toLocal(e, agents_.position[i]);
            if (sided && q.s < 0.0f) {
                // The behaviour tree may set the side after a lane was picked (trees tick
                // at a low, staggered rate): until boarding, a chosen side always wins.
                agents_.escalatorLane[i] = r.side;
                continue;
            }
            // Riders without a fixed side who are stuck at the foot on the other lane's
            // line take that lane, if they squeeze, instead of waiting to cross.
            const float laneL = static_cast<float>(agents_.escalatorLane[i]) * e.laneOffset;
            const glm::vec2 v = agents_.velocity[i];
            const bool stuck = glm::dot(v, v) < 0.15f * 0.15f;
            if (!sided && r.squeeze && stuck && q.s < 0.0f && q.s > -kBoardWindow - 1.0f &&
                std::abs(q.l - laneL) > e.laneOffset * 1.5f) {
                agents_.escalatorLane[i] = static_cast<std::int8_t>(-agents_.escalatorLane[i]);
            }
            continue;
        }
        std::int8_t lane = 0;
        const std::uint32_t left = count[static_cast<std::size_t>(where[i]) * 2];
        const std::uint32_t right = count[static_cast<std::size_t>(where[i]) * 2 + 1];
        const float l = toLocal(e, agents_.position[i]).l;
        const std::int8_t ownSide = l < 0.0f ? std::int8_t{-1} : std::int8_t{1};
        if (sided) {
            lane = r.side;
        } else if (r.side == kShorter) {
            lane = left != right ? (left < right ? std::int8_t{-1} : std::int8_t{1}) : ownSide;
        } else if (std::abs(l) > e.laneOffset || left == right) {
            lane = ownSide;  // either: keep to the side you are on
        } else {
            lane = left < right ? std::int8_t{-1} : std::int8_t{1};  // near the middle: shorter line
        }
        agents_.escalatorLane[i] = lane;
        agents_.escalatorIndex[i] = static_cast<std::uint16_t>(where[i]);
        ++count[static_cast<std::size_t>(where[i]) * 2 + (lane > 0 ? 1 : 0)];
    }
}

bool Simulation::boardsEscalator(std::uint32_t i, const Escalator& e) const {
    const std::uint32_t g = agents_.goalIndex[i];
    if (agents_.moveMode[i] == MoveMode::Goal && g < flowFields_.size()) {
        // Follow the flow field a few polygons on from where the agent stands.
        constexpr int kLookAhead = 6;
        const FlowField& f = flowFields_[g];
        std::uint32_t d = navMesh_.denseIndex(agents_.navPoly[i]);
        for (int hop = 0; hop < kLookAhead && d < f.next.size(); ++hop) {
            const glm::vec3 c = navMesh_.polyCenter(d);
            if (onBelt(e, toLocal(e, glm::vec2(c.x, c.z)), c.y)) {
                return true;
            }
            d = f.next[d];
        }
        return false;
    }
    // Other movement (paths to points): going the way the escalator travels.
    const glm::vec2 want = agents_.preferredVelocity[i];
    return glm::dot(want, want) <= 1e-6f || glm::dot(want, e.forward) > 0.2f * std::sqrt(glm::dot(want, want));
}

glm::vec2 Simulation::escalatorSteering(std::uint32_t i, glm::vec2 pref) const {
    const glm::vec2 pos = agents_.position[i];
    const float height = agents_.height[i];
    const EscalatorRider& r = agents_.rider[i];
    for (const Escalator& e : escalators_) {
        const Local q = toLocal(e, pos);
        if (inExit(e, q, height, r.clearLanding)) {
            // Step off and clear the landing before turning towards your destination.
            // Someone at the edge steps inwards first: the edge of the steps turns a
            // corner onto the landing, where walking along it pins people.
            const float speed = agents_.preferredSpeed[i] * agents_.speedMultiplier[i];
            const float edge = std::clamp((std::abs(q.l) - 0.4f) / 0.3f, 0.0f, 0.8f);
            return e.forward * (speed * (1.0f - edge)) - e.right * std::clamp(q.l * 1.5f, -0.8f, 0.8f);
        }
    }
    const std::uint16_t own = agents_.escalatorIndex[i];
    for (std::size_t k = 0; k < escalators_.size(); ++k) {
        const Escalator& e = escalators_[k];
        const Local q = toLocal(e, pos);
        const bool riding = onBelt(e, q, height);
        if (!riding && !(k == own && inApproach(e, q, height))) {
            continue;
        }
        // A lane counts only on the escalator it was taken for.
        const std::int8_t lane = k == own ? agents_.escalatorLane[i] : std::int8_t{0};
        const bool stands = r.pace <= 0.0f;
        const float speed = agents_.preferredSpeed[i] * agents_.speedMultiplier[i];
        const float laneL = static_cast<float>(lane) * e.laneOffset;

        if (riding) {
            // Carried by the steps; drift into the lane; walkers climb on top of that.
            float sideways = lane != 0 ? std::clamp((laneL - q.l) * 2.0f, -0.4f, 0.4f) : 0.0f;
            if (stands && std::abs(q.l - laneL) > 0.15f) {
                // Someone who stepped on in the other lane only moves across into a
                // real space (one boarding gap); otherwise they stay where they are.
                const float space = 0.75f * r.boardGap;
                bool occupied = false;
                hash_.queryPoints(pos, space + 0.5f, [&](std::uint32_t j, glm::vec2 other, float otherHeight) {
                    if (j == i || occupied || std::abs(otherHeight - height) > 1.5f) {
                        return;
                    }
                    const Local o = toLocal(e, other);
                    occupied = std::abs(o.l - laneL) < 0.2f && std::abs(o.s - q.s) < space;
                });
                if (occupied) {
                    sideways = 0.0f;
                }
            }
            if (std::abs(q.l) > e.laneOffset + 0.1f) {
                // Nobody rides pressed against the balustrade.
                sideways = q.l > 0.0f ? -0.3f : 0.3f;
            }
            return e.forward * (e.beltSpeed + speed * r.pace) + e.right * sideways;
        }
        if (lane == 0) {
            return pref;  // not boarding (e.g. walking past the foot of the escalator)
        }
        // Approach: line up on the lane's centre line, then walk onto the steps.
        const glm::vec2 aim = e.entry + e.right * laneL + e.forward * std::min(q.s + 1.5f, 0.5f);
        glm::vec2 to = aim - pos;
        const float dist = std::sqrt(glm::dot(to, to));
        to = dist > 1e-4f ? to / dist : e.forward;
        if (q.s <= -kBoardWindow) {
            return to * speed;
        }
        // At the foot: slow to about the step speed (so the belt does not carry people
        // into the rider ahead), step on only when lined up with the lane (so nobody
        // slips past a waiting stander into the other lane), and standers only once the
        // person ahead in their lane is their boarding gap away.
        glm::vec2 out = to * std::min(speed, e.beltSpeed + 0.35f);
        const float sideways = std::clamp((laneL - q.l) * 2.0f, -0.4f, 0.4f);
        const float offLane = std::abs(q.l - laneL);
        // Squeezers pushed to the very front on the wrong side step on anyway and
        // shuffle across on the steps; otherwise the two lines could lock each other.
        const glm::vec2 v = agents_.velocity[i];
        const bool pressed = r.squeeze && q.s > -0.6f && glm::dot(v, v) < 0.15f * 0.15f;
        bool blocked = offLane > 0.3f && !pressed;
        if (!blocked && offLane > 0.12f) {
            out = e.right * sideways + e.forward * 0.15f;  // shuffle forward while side-stepping
        }
        if (stands && !blocked) {
            hash_.queryPoints(pos, r.boardGap + 0.5f, [&](std::uint32_t j, glm::vec2 other, float otherHeight) {
                if (j == i || blocked || std::abs(otherHeight - height) > 1.5f) {
                    return;
                }
                const Local o = toLocal(e, other);
                blocked = std::abs(o.l - laneL) < 0.45f && o.s > q.s && o.s - q.s < r.boardGap;
            });
        }
        if (blocked) {
            out = e.right * sideways;
        }
        return out;
    }
    return pref;
}

const Escalator* Simulation::queueingFor(std::uint32_t i, bool& atFoot) const {
    atFoot = false;
    if (agents_.escalatorLane[i] == 0) {
        return nullptr;
    }
    const std::uint16_t own = agents_.escalatorIndex[i];
    if (own >= escalators_.size()) {
        return nullptr;
    }
    const Escalator& e = escalators_[own];
    const Local q = toLocal(e, agents_.position[i]);
    if (inApproach(e, q, agents_.height[i])) {
        atFoot = q.s > -kBoardWindow;
        return &e;
    }
    return nullptr;
}

bool Simulation::onEscalator(glm::vec2 position, float height, float clear) const {
    for (const Escalator& e : escalators_) {
        const Local q = toLocal(e, position);
        if (onBelt(e, q, height) || inExit(e, q, height, clear)) {
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
