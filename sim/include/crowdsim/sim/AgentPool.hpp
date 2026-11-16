#pragma once

/// @file AgentPool.hpp
/// Structure-of-arrays storage for agents. Every hot loop (neighbour search,
/// ORCA, integration, snapshot, behaviour ticks) walks these contiguous columns
/// by index, which keeps them cache-friendly, trivially parallel and SIMD-ready.
/// Removal is swap-and-pop, which is deterministic given a deterministic
/// removal order. See ADR 0004 (storage) and ADR 0007 (decision layer).

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "crowdsim/core/Random.hpp"
#include "crowdsim/sim/BehaviorTree.hpp"

namespace crowdsim::sim {

/// How the locomotion layer should move an agent (written by the decision layer).
enum class MoveMode : std::uint8_t {
    Stop,   ///< stand still (avoidance still applies)
    Goal,   ///< follow the flow field of `goalIndex`
    Point,  ///< follow a navmesh path corridor to `moveTarget`
};

inline constexpr std::uint32_t kCorridorCapacity = 16;
inline constexpr std::uint32_t kNoAgent = 0xFFFFFFFFu;

/// Everything needed to create one agent.
/// How an agent rides escalators. Behaviour, set from its tree by SetEscalatorStyle and
/// SetEscalatorBoarding; the defaults apply to agents that never run those nodes.
struct EscalatorRider {
    std::int8_t side = 0;       ///< -1 left, +1 right, 0 either (own side, then shorter line), 2 shorter line
    float pace = 0.45f;         ///< own walking on the steps, x preferred speed; 0 = stand still
    float clearLanding = 2.0f;  ///< m walked straight on after stepping off before turning
    float boardGap = 0.9f;      ///< m standers leave to the person ahead before stepping on
    bool squeeze = true;        ///< pushed to the front on the wrong side: step on (or switch lanes) anyway
};

/// Queue manners (SetQueueManners): slow to a stop `gap` m behind whoever is ahead in
/// your line instead of pressing into them. gap 0 = off (the default).
struct QueueManners {
    float gap = 0.0f;   ///< m, centre to centre
    float ease = 0.5f;  ///< m over which to slow down before that
};

/// How hard an agent presses forward when bodies touch (SetPushing), newtons. The
/// default is the light pressure of someone moving with a packed crowd.
inline constexpr float kDefaultPush = 100.0f;

struct AgentInit {
    std::uint32_t id = 0;
    glm::vec2 position{0.0f};
    float heading = 0.0f;
    float radius = 0.3f;
    float preferredSpeed = 1.3f;
    float maxSpeed = 1.8f;
    float maxAcceleration = 3.0f;
    float personalSpace = 0.06f;
    float mass = 75.0f;  ///< kg (avoidance priority, see AgentParams::massMin)
    glm::vec3 color{1.0f};
    float height = 0.0f;         ///< world Y (floor height under the agent)
    std::uint32_t navPoly = 0;   ///< Detour polygon ref, 0 = not on the navmesh
    glm::vec3 goalTarget{0.0f};  ///< point inside the goal area (on the navmesh)
    std::uint16_t goalIndex = 0;
    std::uint16_t agentType = 0;  ///< index into the simulation's agent types
    std::uint16_t tree = 0;       ///< index into the simulation's behaviour trees
    float spawnTime = 0.0f;
    std::uint64_t rngSeed = 0;
    std::uint32_t groupId = kNoAgent;  ///< id of the group's leader (== id for the leader)
};

class AgentPool {
public:
    // --- Identity / locomotion state. Positions are on the ground plane: x = X, y = Z. ---
    std::vector<std::uint32_t> id;
    std::vector<glm::vec2> position;
    std::vector<glm::vec2> prevPosition;
    std::vector<glm::vec2> velocity;
    std::vector<glm::vec2> preferredVelocity;
    std::vector<glm::vec2> newVelocity;
    std::vector<float> heading;
    std::vector<float> prevHeading;
    std::vector<float> radius;
    std::vector<float> preferredSpeed;
    std::vector<float> maxSpeed;
    std::vector<float> maxAcceleration;
    std::vector<float> personalSpace;
    std::vector<float> mass;  ///< kg
    std::vector<glm::vec3> color;
    std::vector<float> height;
    std::vector<float> prevHeight;
    std::vector<std::uint32_t> navPoly;
    std::vector<glm::vec3> goalTarget;
    std::vector<std::uint16_t> goalIndex;
    std::vector<std::uint16_t> agentType;
    std::vector<float> spawnTime;
    // Off-mesh link traversal (ladders, escalators): active while linkActive != 0.
    std::vector<std::uint8_t> linkActive;
    std::vector<float> linkProgress;
    std::vector<glm::vec3> linkStart;
    std::vector<glm::vec3> linkEnd;
    std::vector<std::uint32_t> linkEndPoly;
    // Escalator lane while boarding or riding: -1 left, +1 right, 0 none; and which
    // escalator (index into Simulation::escalators()) that lane is on.
    std::vector<std::int8_t> escalatorLane;
    std::vector<std::uint16_t> escalatorIndex;
    // Behaviour settings from the agent's tree (see EscalatorRider, QueueManners).
    std::vector<EscalatorRider> rider;
    std::vector<QueueManners> manners;
    std::vector<float> push;  ///< N pressed forward when touching others (SetPushing)

    // --- Bodies in dense crowds (only when the scene enables BodyParams) ---
    std::vector<float> pressure;         ///< N/m: compressive force on the body per metre of its width
    std::vector<glm::vec2> bodyForce;    ///< N: contact forces + slope lean, this step
    std::vector<glm::vec2> wallForce;    ///< N: reaction of walls at the last move
    std::vector<glm::vec2> separation;   ///< m/s: undoes overlap of incompressible cores
    std::vector<std::uint8_t> inContact; ///< touching another body this step
    std::vector<std::uint8_t> fallen;    ///< 1 = on the ground
    std::vector<float> fallenFor;        ///< s on the ground
    std::vector<std::uint8_t> pileLayer; ///< 1 = on the floor, 2+ = lying on others
    std::vector<std::uint8_t> bodyRequest; ///< parallel phase -> serial: 1 fall, 2 get up
    std::vector<float> slowTime;         ///< s spent wanting to move but barely moving (IsBlocked)

    // --- Decision layer ---
    std::vector<std::uint16_t> tree;
    std::vector<BtState> bt;
    std::vector<core::Pcg32> rng;
    std::vector<MoveMode> moveMode;
    std::vector<glm::vec3> moveTarget;
    std::vector<float> speedMultiplier;
    std::vector<std::uint16_t> stateId;
    std::vector<std::uint16_t> avoidTag;  ///< tag id to steer around, 0 = none
    // Path corridor for MoveMode::Point (cached Detour path, replanned when needed).
    std::vector<std::array<std::uint32_t, kCorridorCapacity>> corridor;
    std::vector<std::uint8_t> corridorLength;
    std::vector<std::uint8_t> corridorStatus;  ///< 0 = needs plan, 1 = valid, 2 = unreachable
    std::vector<glm::vec3> corridorTarget;
    // Groups
    std::vector<std::uint32_t> groupId;
    // Queueing at goals (resolved in the serial decision phase)
    std::vector<std::int16_t> queueGoal;   ///< goal index, -1 = not queueing
    std::vector<std::uint8_t> queueStage;  ///< 0 none, 1 waiting, 2 admitted, 3 being served, 4 done
    std::vector<std::uint32_t> queueTicket;
    std::vector<glm::vec3> queueSlot;  ///< where to wait while in line
    // Requests for the serial phase
    std::vector<std::uint8_t> arrivalPending;  ///< reached its goal this tick (count it)
    std::vector<std::uint8_t> arrivedOnce;     ///< already counted for the current goal
    std::vector<std::uint8_t> despawnRequested;

    [[nodiscard]] std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(id.size()); }
    [[nodiscard]] bool empty() const noexcept { return id.empty(); }

    void reserve(std::size_t n) {
        forEachColumn([n](auto& column) { column.reserve(n); });
    }

    void clear() {
        forEachColumn([](auto& column) { column.clear(); });
    }

    void add(const AgentInit& a) {
        id.push_back(a.id);
        position.push_back(a.position);
        prevPosition.push_back(a.position);
        velocity.push_back(glm::vec2(0.0f));
        preferredVelocity.push_back(glm::vec2(0.0f));
        newVelocity.push_back(glm::vec2(0.0f));
        heading.push_back(a.heading);
        prevHeading.push_back(a.heading);
        radius.push_back(a.radius);
        preferredSpeed.push_back(a.preferredSpeed);
        maxSpeed.push_back(a.maxSpeed);
        maxAcceleration.push_back(a.maxAcceleration);
        personalSpace.push_back(a.personalSpace);
        mass.push_back(a.mass);
        color.push_back(a.color);
        height.push_back(a.height);
        prevHeight.push_back(a.height);
        navPoly.push_back(a.navPoly);
        goalTarget.push_back(a.goalTarget);
        goalIndex.push_back(a.goalIndex);
        agentType.push_back(a.agentType);
        spawnTime.push_back(a.spawnTime);
        linkActive.push_back(0);
        linkProgress.push_back(0.0f);
        linkStart.push_back(glm::vec3(0.0f));
        linkEnd.push_back(glm::vec3(0.0f));
        linkEndPoly.push_back(0);
        escalatorLane.push_back(0);
        escalatorIndex.push_back(0);
        rider.push_back(EscalatorRider{});
        manners.push_back(QueueManners{});
        push.push_back(kDefaultPush);
        pressure.push_back(0.0f);
        bodyForce.push_back(glm::vec2(0.0f));
        wallForce.push_back(glm::vec2(0.0f));
        separation.push_back(glm::vec2(0.0f));
        inContact.push_back(0);
        fallen.push_back(0);
        fallenFor.push_back(0.0f);
        pileLayer.push_back(0);
        bodyRequest.push_back(0);
        slowTime.push_back(0.0f);

        tree.push_back(a.tree);
        bt.push_back(BtState{});
        rng.push_back(core::Pcg32(a.rngSeed, a.id));
        moveMode.push_back(MoveMode::Goal);
        moveTarget.push_back(a.goalTarget);
        speedMultiplier.push_back(1.0f);
        stateId.push_back(0);
        avoidTag.push_back(0);
        corridor.push_back({});
        corridorLength.push_back(0);
        corridorStatus.push_back(0);
        corridorTarget.push_back(glm::vec3(0.0f));
        groupId.push_back(a.groupId);
        queueGoal.push_back(-1);
        queueStage.push_back(0);
        queueTicket.push_back(0xFFFFFFFFu);
        queueSlot.push_back(glm::vec3(0.0f));
        arrivalPending.push_back(0);
        arrivedOnce.push_back(0);
        despawnRequested.push_back(0);
    }

    /// Reorder all columns: new agent k is old agent order[k] (order is a permutation).
    void permute(std::span<const std::uint32_t> order) {
        forEachColumn([order](auto& column) {
            std::remove_reference_t<decltype(column)> sorted;
            sorted.reserve(column.size());
            for (const std::uint32_t k : order) {
                sorted.push_back(column[k]);
            }
            column.swap(sorted);
        });
    }

    /// Remove agent `i` by moving the last agent into its slot.
    void swapRemove(std::uint32_t i) {
        forEachColumn([i](auto& column) {
            column[i] = column.back();
            column.pop_back();
        });
    }

private:
    template <typename F>
    void forEachColumn(F&& f) {
        f(id);
        f(position);
        f(prevPosition);
        f(velocity);
        f(preferredVelocity);
        f(newVelocity);
        f(heading);
        f(prevHeading);
        f(radius);
        f(preferredSpeed);
        f(maxSpeed);
        f(maxAcceleration);
        f(personalSpace);
        f(mass);
        f(color);
        f(height);
        f(prevHeight);
        f(navPoly);
        f(goalTarget);
        f(goalIndex);
        f(agentType);
        f(spawnTime);
        f(linkActive);
        f(linkProgress);
        f(linkStart);
        f(linkEnd);
        f(linkEndPoly);
        f(escalatorLane);
        f(escalatorIndex);
        f(rider);
        f(manners);
        f(push);
        f(pressure);
        f(bodyForce);
        f(wallForce);
        f(separation);
        f(inContact);
        f(fallen);
        f(fallenFor);
        f(pileLayer);
        f(bodyRequest);
        f(slowTime);
        f(tree);
        f(bt);
        f(rng);
        f(moveMode);
        f(moveTarget);
        f(speedMultiplier);
        f(stateId);
        f(avoidTag);
        f(corridor);
        f(corridorLength);
        f(corridorStatus);
        f(corridorTarget);
        f(groupId);
        f(queueGoal);
        f(queueStage);
        f(queueTicket);
        f(queueSlot);
        f(arrivalPending);
        f(arrivedOnce);
        f(despawnRequested);
    }
};

}  // namespace crowdsim::sim
