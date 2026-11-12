/// Decision layer of the Simulation: behaviour-tree ticking (parallel, staggered,
/// low rate), the serial resolution of shared state (queues, arrivals, despawns),
/// path corridors for point targets, and the tree / agent-type registries.
/// See ADR 0007.

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

#include <spdlog/logger.h>
#include <glm/geometric.hpp>

#include "Funnel.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::sim {

namespace {

constexpr float kGoalVerticalTolerance = 1.5f;
constexpr float kRetargetDistance = 0.5f;  ///< replan a corridor when the target moved this far
constexpr std::uint32_t kPortalLookahead = 8;

std::uint64_t bit(std::uint16_t node) {
    return node < 64 ? (1ull << node) : 0ull;
}

float distanceToArea(glm::vec2 p, const Area& a) {
    const glm::vec2 d = glm::max(glm::abs(p - a.center) - a.halfExtents, glm::vec2(0.0f));
    return std::sqrt(glm::dot(d, d));
}

}  // namespace

// ------------------------------------------------------------------------- registries

std::uint16_t Simulation::internName(std::vector<std::string>& table, std::string_view name) {
    if (name.empty()) {
        return 0;
    }
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i] == name) {
            return static_cast<std::uint16_t>(i);
        }
    }
    table.emplace_back(name);
    return static_cast<std::uint16_t>(table.size() - 1);
}

void Simulation::rebuildTreeLookups() {
    std::unordered_map<std::string, std::int32_t> goalByName;
    for (std::uint32_t g = 0; g < goals_.size(); ++g) {
        goalByName.emplace(registry_.get<Goal>(goals_[g]).name, static_cast<std::int32_t>(g));
    }
    treeLookups_.assign(trees_.size(), {});
    for (std::size_t t = 0; t < trees_.size(); ++t) {
        const CompiledTree& tree = trees_[t];
        TreeLookup& lookup = treeLookups_[t];
        lookup.stateOf.assign(tree.strings.size(), 0);
        lookup.tagOf.assign(tree.strings.size(), 0);
        lookup.goalOf.assign(tree.strings.size(), -1);
        for (const BtNode& n : tree.nodes) {
            const std::string& text = tree.strings[n.text];
            switch (n.type) {
                case BtNodeType::SetState:
                    lookup.stateOf[n.text] = internName(stateNames_, text);
                    break;
                case BtNodeType::FleeFrom:
                case BtNodeType::AvoidArea:
                case BtNodeType::IsAreaTagged:
                    lookup.tagOf[n.text] = internName(tagNames_, text);
                    break;
                case BtNodeType::MoveToGoal:
                case BtNodeType::Queue:
                case BtNodeType::IsNearGoal:
                    if (!text.empty()) {
                        const auto it = goalByName.find(text);
                        lookup.goalOf[n.text] = it == goalByName.end() ? -2 : it->second;  // -2: unknown goal
                    }
                    break;
                default:
                    break;
            }
        }
    }
}

void Simulation::rebuildTaggedVolumes() {
    taggedVolumes_.clear();
    for (entt::entity e : staticObjects_) {
        const StaticObject& o = registry_.get<StaticObject>(e);
        if (o.tags.empty()) {
            continue;
        }
        const geometry::Bounds b = objectMesh(registry_.get<SceneIdComponent>(e).id).bounds();
        if (!b.valid()) {
            continue;
        }
        for (const std::string& tag : o.tags) {
            taggedVolumes_.push_back({internName(tagNames_, tag), b.min, b.max});
        }
    }
    rebuildTreeLookups();
}

std::uint16_t Simulation::resolveTree(const std::string& name) const {
    for (std::size_t t = 0; t < trees_.size(); ++t) {
        if (trees_[t].name == name) {
            return static_cast<std::uint16_t>(t);
        }
    }
    return 0;  // the built-in commuter tree
}

std::uint16_t Simulation::setBehaviorTree(CompiledTree tree) {
    std::uint16_t index = 0;
    bool replaced = false;
    for (std::size_t t = 0; t < trees_.size(); ++t) {
        if (trees_[t].name == tree.name) {
            tree.generation = trees_[t].generation + 1;  // agents restart from the root
            trees_[t] = std::move(tree);
            index = static_cast<std::uint16_t>(t);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        tree.generation = 1;
        trees_.push_back(std::move(tree));
        index = static_cast<std::uint16_t>(trees_.size() - 1);
        // Agent types may already reference this tree by name.
        for (std::uint16_t a = 0; a < agentTypes_.size(); ++a) {
            if (agentTypes_[a].behaviour == trees_[index].name) {
                for (std::uint32_t i = 0; i < agents_.size(); ++i) {
                    if (agents_.agentType[i] == a) {
                        agents_.tree[i] = index;
                    }
                }
            }
        }
    }
    rebuildTreeLookups();
    services_.log.info("Behaviour tree '{}' {} (generation {}, {} nodes)", trees_[index].name,
                       replaced ? "reloaded" : "added", trees_[index].generation, trees_[index].nodes.size());
    return index;
}

const CompiledTree* Simulation::findBehaviorTree(std::string_view name) const {
    for (const CompiledTree& t : trees_) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

std::uint16_t Simulation::defineAgentType(const AgentType& type) {
    for (std::size_t t = 0; t < agentTypes_.size(); ++t) {
        if (agentTypes_[t].name == type.name) {
            const bool behaviourChanged = agentTypes_[t].behaviour != type.behaviour;
            agentTypes_[t] = type;
            if (behaviourChanged) {
                const std::uint16_t tree = resolveTree(type.behaviour);
                for (std::uint32_t i = 0; i < agents_.size(); ++i) {
                    if (agents_.agentType[i] == t) {
                        agents_.tree[i] = tree;
                        agents_.bt[i].reset(trees_[tree].generation);
                    }
                }
            }
            return static_cast<std::uint16_t>(t);
        }
    }
    agentTypes_.push_back(type);
    return static_cast<std::uint16_t>(agentTypes_.size() - 1);
}

const AgentType* Simulation::findAgentType(std::string_view name) const {
    for (const AgentType& t : agentTypes_) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

std::optional<AgentDebugInfo> Simulation::agentDebug(std::uint32_t agentId) const {
    const std::uint32_t i = agentIndexOf(agentId);
    if (i == kNoAgent) {
        return std::nullopt;
    }
    AgentDebugInfo d;
    d.id = agentId;
    d.index = i;
    d.typeName = agentTypes_[agents_.agentType[i]].name;
    d.tree = &trees_[agents_.tree[i]];
    d.treeName = d.tree->name;
    d.stateName = agents_.stateId[i] < stateNames_.size() ? stateNames_[agents_.stateId[i]] : "?";
    d.goalName = agents_.goalIndex[i] < goals_.size() ? registry_.get<Goal>(goals_[agents_.goalIndex[i]]).name : "";
    d.bt = agents_.bt[i];
    d.moveMode = agents_.moveMode[i];
    d.moveTarget = agents_.moveTarget[i];
    d.position = {agents_.position[i].x, agents_.height[i], agents_.position[i].y};
    d.speed = glm::length(agents_.velocity[i]);
    d.speedMultiplier = agents_.speedMultiplier[i];
    d.queueStage = agents_.queueStage[i];
    d.groupId = agents_.groupId[i];
    return d;
}

// ------------------------------------------------------------------------- ticking

void Simulation::tickBehaviors() {
    CS_PROFILE_ZONE();
    const auto stride =
        static_cast<std::uint64_t>(std::max(1.0, std::round(config_.stepHz / std::max(0.1, config_.decisionHz))));
    services_.jobs.parallelFor(agents_.size(), 128, [&](core::IndexRange r, std::uint32_t thread) {
        for (std::uint32_t i = r.begin; i < r.end; ++i) {
            // Stagger: each agent ticks on a fixed phase of the stride (by id, so it is stable).
            if ((agents_.id[i] + stepIndex_) % stride != 0 || agents_.despawnRequested[i] != 0 ||
                agents_.linkActive[i] != 0) {
                continue;
            }
            const CompiledTree& tree = trees_[agents_.tree[i]];
            BtState& st = agents_.bt[i];
            if (st.generation != tree.generation) {
                st.reset(tree.generation);  // hot-reloaded tree: start over from the root
            }
            st.visited = 0;
            st.running = 0;
            st.lastStatus = tickNode(tree, 0, i, thread);
        }
    });
    runCustomNodes();
}

void Simulation::runCustomNodes() {
    std::vector<CustomRequest> requests;
    for (std::vector<CustomRequest>& perThread : customRequests_) {
        requests.insert(requests.end(), perThread.begin(), perThread.end());
        perThread.clear();
    }
    if (requests.empty() || customRunner_ == nullptr) {
        return;
    }
    // Deterministic order regardless of which thread ticked which agent.
    std::sort(requests.begin(), requests.end(), [](const CustomRequest& a, const CustomRequest& b) {
        return std::tie(a.tree, a.node, a.agent) < std::tie(b.tree, b.node, b.agent);
    });
    std::vector<std::uint32_t> agents(requests.size());
    std::vector<CustomResult> results(requests.size(), CustomResult::Running);
    std::vector<CustomNodeBatch> batches;
    for (std::size_t k = 0; k < requests.size();) {
        std::size_t end = k;
        while (end < requests.size() && requests[end].tree == requests[k].tree &&
               requests[end].node == requests[k].node) {
            agents[end] = requests[end].agent;
            ++end;
        }
        const CompiledTree& tree = trees_[requests[k].tree];
        CustomNodeBatch batch;
        batch.customIndex = tree.nodes[requests[k].node].customIndex;
        batch.tree = &tree;
        batch.node = requests[k].node;
        batch.agents = std::span<const std::uint32_t>(agents.data() + k, end - k);
        batch.results = std::span<CustomResult>(results.data() + k, end - k);
        batches.push_back(batch);
        k = end;
    }
    customRunner_->runCustomNodes(*this, batches);
    for (std::size_t k = 0; k < requests.size(); ++k) {
        const CompiledTree& tree = trees_[requests[k].tree];
        const BtNode& n = tree.nodes[requests[k].node];
        std::uint8_t& m = agents_.bt[requests[k].agent].memory[n.memSlot];
        m = results[k] == CustomResult::Success ? 3 : (results[k] == CustomResult::Failure ? 4 : 2);
    }
}

std::uint16_t Simulation::defineCustomNode(const CustomNodeSpec& spec) {
    for (std::size_t k = 0; k < customNodes_.size(); ++k) {
        if (customNodes_[k].name == spec.name) {
            customNodes_[k] = spec;
            return static_cast<std::uint16_t>(k);
        }
    }
    customNodes_.push_back(spec);
    return static_cast<std::uint16_t>(customNodes_.size() - 1);
}

void Simulation::scriptMoveTo(std::uint32_t index, glm::vec3 target) {
    if (index < agents_.size()) {
        setPointTarget(index, target);
    }
}

void Simulation::scriptMoveToGoal(std::uint32_t index) {
    if (index < agents_.size()) {
        agents_.moveMode[index] = MoveMode::Goal;
    }
}

void Simulation::scriptStop(std::uint32_t index) {
    if (index < agents_.size()) {
        agents_.moveMode[index] = MoveMode::Stop;
    }
}

void Simulation::scriptSetSpeed(std::uint32_t index, float multiplier) {
    if (index < agents_.size()) {
        agents_.speedMultiplier[index] = std::clamp(multiplier, 0.0f, 3.0f);
    }
}

void Simulation::scriptSetState(std::uint32_t index, std::string_view state) {
    if (index < agents_.size()) {
        agents_.stateId[index] = internName(stateNames_, state);
    }
}

float Simulation::scriptRandom(std::uint32_t index) {
    return index < agents_.size() ? agents_.rng[index].nextFloat() : 0.0f;
}

std::uint32_t Simulation::countAgentsNear(glm::vec3 p, float radius) const {
    std::uint32_t count = 0;
    const float r2 = radius * radius;
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        const glm::vec2 d = agents_.position[i] - glm::vec2(p.x, p.z);
        if (glm::dot(d, d) <= r2 && std::abs(agents_.height[i] - p.y) < 1.0f) {
            ++count;
        }
    }
    return count;
}

void Simulation::resetSubtree(const CompiledTree& tree, std::uint16_t node, BtState& state) const {
    for (std::uint16_t k = node; k < tree.nodes[node].subtreeEnd; ++k) {
        const BtNode& n = tree.nodes[k];
        if (n.memSlot != kBtNoSlot) {
            state.memory[n.memSlot] = 0;
        }
        if (n.timerSlot != kBtNoSlot) {
            state.timers[n.timerSlot] = 0.0f;
        }
    }
}

void Simulation::setPointTarget(std::uint32_t i, glm::vec3 target) {
    agents_.moveMode[i] = MoveMode::Point;
    const glm::vec3 d = target - agents_.corridorTarget[i];
    if (agents_.corridorStatus[i] == 2 || glm::dot(d, d) > kRetargetDistance * kRetargetDistance) {
        agents_.corridorStatus[i] = 0;  // plan again in the locomotion phase
    }
    agents_.moveTarget[i] = target;
}

BtStatus Simulation::tickNode(const CompiledTree& tree, std::uint16_t index, std::uint32_t i, std::uint32_t thread) {
    const BtNode& n = tree.nodes[index];
    BtState& st = agents_.bt[i];
    st.visited |= bit(index);
    const auto now = static_cast<float>(simTimeSeconds());
    const TreeLookup& lookup = treeLookups_[agents_.tree[i]];
    const glm::vec2 pos = agents_.position[i];
    const glm::vec3 pos3(pos.x, agents_.height[i], pos.y);
    const auto kids = tree.children(n);

    auto goalFor = [&](std::uint16_t text) -> std::int32_t {
        const std::int32_t g = lookup.goalOf[text];
        if (g == -1) {
            return agents_.goalIndex[i] < goals_.size() ? agents_.goalIndex[i] : -2;
        }
        return g;
    };
    auto running = [&] {
        st.running |= bit(index);
        return BtStatus::Running;
    };
    auto arrivedAt = [&](glm::vec3 target, float tolerance) {
        const glm::vec2 d(target.x - pos.x, target.z - pos.y);
        return glm::dot(d, d) <= tolerance * tolerance && std::abs(target.y - pos3.y) < 1.2f;
    };

    switch (n.type) {
        // ----------------------------------------------------------------- composites
        case BtNodeType::Sequence:
        case BtNodeType::Selector: {
            const bool isSequence = n.type == BtNodeType::Sequence;
            std::uint8_t& current = st.memory[n.memSlot];
            for (std::uint8_t c = current; c < n.childCount; ++c) {
                const BtStatus s = tickNode(tree, kids[c], i, thread);
                if (s == BtStatus::Running) {
                    current = c;
                    return running();
                }
                if ((isSequence && s == BtStatus::Failure) || (!isSequence && s == BtStatus::Success)) {
                    current = 0;
                    return s;
                }
            }
            current = 0;
            return isSequence ? BtStatus::Success : BtStatus::Failure;
        }
        case BtNodeType::Parallel: {
            std::uint32_t successes = 0;
            std::uint32_t failures = 0;
            for (std::uint16_t child : kids) {
                const BtStatus s = tickNode(tree, child, i, thread);
                successes += s == BtStatus::Success ? 1u : 0u;
                failures += s == BtStatus::Failure ? 1u : 0u;
            }
            const auto needed = static_cast<std::uint32_t>(n.number[0]);
            if (successes >= needed || failures > n.childCount - needed) {
                for (std::uint16_t child : kids) {
                    resetSubtree(tree, child, st);
                }
                return successes >= needed ? BtStatus::Success : BtStatus::Failure;
            }
            return running();
        }
        case BtNodeType::RandomSelector:
        case BtNodeType::WeightedRandom: {
            std::uint8_t& chosen = st.memory[n.memSlot];  // child + 1, 0 = none yet
            if (chosen == 0) {
                if (n.type == BtNodeType::RandomSelector) {
                    chosen = static_cast<std::uint8_t>(agents_.rng[i].below(n.childCount) + 1);
                } else {
                    float total = 0.0f;
                    for (std::uint8_t c = 0; c < n.childCount; ++c) {
                        total += tree.floatPool[n.weights + c];
                    }
                    float pick = agents_.rng[i].nextFloat() * total;
                    chosen = n.childCount;
                    for (std::uint8_t c = 0; c < n.childCount; ++c) {
                        pick -= tree.floatPool[n.weights + c];
                        if (pick < 0.0f) {
                            chosen = static_cast<std::uint8_t>(c + 1);
                            break;
                        }
                    }
                }
            }
            const BtStatus s = tickNode(tree, kids[chosen - 1], i, thread);
            if (s != BtStatus::Running) {
                chosen = 0;
                return s;
            }
            return running();
        }
        // ----------------------------------------------------------------- decorators
        case BtNodeType::Inverter: {
            const BtStatus s = tickNode(tree, kids[0], i, thread);
            if (s == BtStatus::Running) {
                return running();
            }
            return s == BtStatus::Success ? BtStatus::Failure : BtStatus::Success;
        }
        case BtNodeType::Succeeder: {
            const BtStatus s = tickNode(tree, kids[0], i, thread);
            return s == BtStatus::Running ? running() : BtStatus::Success;
        }
        case BtNodeType::Repeat: {
            std::uint8_t& count = st.memory[n.memSlot];
            const BtStatus s = tickNode(tree, kids[0], i, thread);
            if (s == BtStatus::Running) {
                return running();
            }
            if (s == BtStatus::Failure) {
                count = 0;
                return BtStatus::Failure;
            }
            const auto limit = static_cast<std::uint8_t>(n.number[0]);
            count = static_cast<std::uint8_t>(std::min(255, count + 1));
            if (limit > 0 && count >= limit) {
                count = 0;
                return BtStatus::Success;
            }
            return running();  // run the child again on the next tick
        }
        case BtNodeType::Cooldown: {
            float& availableAt = st.timers[n.timerSlot];
            if (availableAt > 0.0f && now < availableAt) {
                return BtStatus::Failure;
            }
            const BtStatus s = tickNode(tree, kids[0], i, thread);
            if (s == BtStatus::Running) {
                return running();
            }
            availableAt = now + n.number[0] + 1e-3f;
            return s;
        }
        case BtNodeType::Timeout: {
            float& startedAt = st.timers[n.timerSlot];
            if (startedAt <= 0.0f) {
                startedAt = now + 1e-3f;
            }
            const BtStatus s = tickNode(tree, kids[0], i, thread);
            if (s != BtStatus::Running) {
                startedAt = 0.0f;
                return s;
            }
            if (now - startedAt >= n.number[0]) {
                startedAt = 0.0f;
                resetSubtree(tree, kids[0], st);
                return BtStatus::Failure;
            }
            return running();
        }
        // ----------------------------------------------------------------- actions
        case BtNodeType::MoveToGoal: {
            const std::int32_t g = goalFor(n.text);
            if (g < 0) {
                return BtStatus::Failure;
            }
            if (static_cast<std::uint32_t>(g) != agents_.goalIndex[i]) {
                agents_.goalIndex[i] = static_cast<std::uint16_t>(g);
                const Area& area = goalAreas_[static_cast<std::size_t>(g)];
                const glm::vec2 lane = area.clamp(pos, 0.5f);
                glm::vec3 target(lane.x, area.elevation, lane.y);
                if (const auto nearest = navMesh_.findNearest(target, {1.0f, 2.0f, 1.0f}, thread)) {
                    target = nearest->point;
                }
                agents_.goalTarget[i] = target;
                agents_.arrivedOnce[i] = 0;
            }
            agents_.moveMode[i] = MoveMode::Goal;
            if (goalAreas_[static_cast<std::size_t>(g)].contains3d(pos3, kGoalVerticalTolerance)) {
                if (agents_.arrivedOnce[i] == 0) {
                    agents_.arrivedOnce[i] = 1;
                    agents_.arrivalPending[i] = 1;
                }
                return BtStatus::Success;
            }
            return running();
        }
        case BtNodeType::MoveToPoint: {
            glm::vec3 target;
            if (n.hasVec) {
                target = n.vec;
            } else if (st.hasTarget != 0) {
                target = st.target;
            } else {
                return BtStatus::Failure;
            }
            setPointTarget(i, target);
            if (agents_.corridorStatus[i] == 2) {
                agents_.moveMode[i] = MoveMode::Stop;
                return BtStatus::Failure;
            }
            if (arrivedAt(target, n.number[0])) {
                agents_.moveMode[i] = MoveMode::Stop;
                return BtStatus::Success;
            }
            return running();
        }
        case BtNodeType::Wander: {
            if (st.hasTarget == 0) {
                bool found = false;
                for (int attempt = 0; attempt < 4 && !found; ++attempt) {
                    const float angle = agents_.rng[i].nextFloat() * 2.0f * std::numbers::pi_v<float>;
                    const float dist = std::sqrt(agents_.rng[i].nextFloat()) * n.number[0];
                    const glm::vec3 probe(pos.x + std::cos(angle) * dist, pos3.y, pos.y + std::sin(angle) * dist);
                    const auto nearest = navMesh_.findNearest(probe, {1.0f, 1.5f, 1.0f}, thread);
                    if (nearest && navMesh_.connected(nearest->ref, agents_.navPoly[i])) {
                        st.target = nearest->point;
                        st.hasTarget = 1;
                        found = true;
                    }
                }
                if (!found) {
                    return BtStatus::Failure;
                }
            }
            setPointTarget(i, st.target);
            if (agents_.corridorStatus[i] == 2 || arrivedAt(st.target, 0.6f)) {
                const bool reached = agents_.corridorStatus[i] != 2;
                st.hasTarget = 0;
                agents_.moveMode[i] = MoveMode::Stop;
                return reached ? BtStatus::Success : BtStatus::Failure;
            }
            return running();
        }
        case BtNodeType::Wait: {
            float& until = st.timers[n.timerSlot];
            if (until <= 0.0f) {
                until = now + n.number[0] + agents_.rng[i].nextFloat() * n.number[1] + 1e-3f;
            }
            agents_.moveMode[i] = MoveMode::Stop;
            if (now >= until) {
                until = 0.0f;
                return BtStatus::Success;
            }
            return running();
        }
        case BtNodeType::Queue: {
            const std::int32_t g = goalFor(n.text);
            if (g < 0) {
                return BtStatus::Failure;
            }
            std::uint8_t& stage = agents_.queueStage[i];
            if (agents_.queueGoal[i] != g || stage == 0) {
                agents_.queueGoal[i] = static_cast<std::int16_t>(g);
                agents_.queueTicket[i] = 0xFFFFFFFFu;  // assigned in resolveDecisions()
                stage = 1;
                st.timers[n.timerSlot] = 0.0f;
            }
            const Goal& goal = goalsCache_[static_cast<std::size_t>(g)];
            switch (stage) {
                case 1:  // in line: walk to the assigned slot and wait there
                    if (agents_.queueTicket[i] == 0xFFFFFFFFu) {
                        agents_.moveMode[i] = MoveMode::Stop;
                    } else if (arrivedAt(agents_.queueSlot[i], 0.35f)) {
                        agents_.moveMode[i] = MoveMode::Stop;
                    } else {
                        setPointTarget(i, agents_.queueSlot[i]);
                    }
                    return running();
                case 2: {  // admitted: walk into the goal area
                    if (agents_.goalIndex[i] != static_cast<std::uint32_t>(g)) {
                        agents_.goalIndex[i] = static_cast<std::uint16_t>(g);
                        agents_.goalTarget[i] = goal.area.center3d();
                    }
                    agents_.moveMode[i] = MoveMode::Goal;
                    if (goal.area.contains3d(pos3, kGoalVerticalTolerance)) {
                        stage = 3;
                        st.timers[n.timerSlot] = now + goal.serviceTime + 1e-3f;
                        agents_.moveMode[i] = MoveMode::Stop;
                    }
                    return running();
                }
                case 3:  // being served
                    agents_.moveMode[i] = MoveMode::Stop;
                    if (now >= st.timers[n.timerSlot]) {
                        stage = 4;  // freed in resolveDecisions()
                        st.timers[n.timerSlot] = 0.0f;
                        agents_.arrivalPending[i] = 1;
                        return BtStatus::Success;
                    }
                    return running();
                default:
                    return BtStatus::Success;
            }
        }
        case BtNodeType::FollowGroup: {
            const std::uint32_t leaderId = agents_.groupId[i];
            if (leaderId == kNoAgent || leaderId == agents_.id[i]) {
                return BtStatus::Failure;
            }
            const std::uint32_t leader = agentIndexOf(leaderId);
            if (leader == kNoAgent || agents_.despawnRequested[leader] != 0) {
                return BtStatus::Failure;  // leader gone: fall back to own behaviour
            }
            const glm::vec2 lp = agents_.position[leader];
            const glm::vec2 lv = agents_.velocity[leader];
            const float speed = glm::length(lv);
            const glm::vec2 back = speed > 0.1f ? -lv / speed : glm::normalize(pos - lp + glm::vec2(1e-3f, 0.0f));
            // Spread followers sideways by id so they do not all aim at the same spot.
            const glm::vec2 side(-back.y, back.x);
            const float lateral = (static_cast<float>(agents_.id[i] % 3) - 1.0f) * 0.7f;
            const glm::vec2 slot = lp + back * n.number[0] + side * lateral;
            setPointTarget(i, {slot.x, agents_.height[leader], slot.y});
            agents_.speedMultiplier[i] = glm::length(slot - pos) > 2.0f ? 1.25f : 1.0f;
            if (arrivedAt({slot.x, agents_.height[leader], slot.y}, 0.4f)) {
                agents_.moveMode[i] = MoveMode::Stop;
            }
            return running();
        }
        case BtNodeType::FleeFrom: {
            const std::uint16_t tag = lookup.tagOf[n.text];
            const float radius = n.number[0];
            const TaggedVolume* nearest = nullptr;
            float best = radius;
            for (const TaggedVolume& v : taggedVolumes_) {
                if (v.tag != tag || tag == 0) {
                    continue;
                }
                const glm::vec2 c(std::clamp(pos.x, v.min.x, v.max.x), std::clamp(pos.y, v.min.z, v.max.z));
                const float d = glm::length(pos - c);
                if (d < best) {
                    best = d;
                    nearest = &v;
                }
            }
            if (nearest == nullptr) {
                return BtStatus::Failure;
            }
            const glm::vec2 centre(0.5f * (nearest->min.x + nearest->max.x), 0.5f * (nearest->min.z + nearest->max.z));
            glm::vec2 away = pos - centre;
            const float len = glm::length(away);
            away = len > 1e-3f ? away / len : glm::vec2(1.0f, 0.0f);
            const glm::vec3 probe(pos.x + away.x * radius, pos3.y, pos.y + away.y * radius);
            if (const auto spot = navMesh_.findNearest(probe, {radius * 0.5f, 2.0f, radius * 0.5f}, thread)) {
                setPointTarget(i, spot->point);
            }
            return running();
        }
        case BtNodeType::AvoidArea:
            agents_.avoidTag[i] = lookup.tagOf[n.text];
            return BtStatus::Success;
        case BtNodeType::SetSpeed:
            agents_.speedMultiplier[i] = std::max(0.0f, n.number[0]);
            return BtStatus::Success;
        case BtNodeType::SetState:
            agents_.stateId[i] = lookup.stateOf[n.text];
            return BtStatus::Success;
        case BtNodeType::Despawn:
            agents_.despawnRequested[i] = 1;
            agents_.moveMode[i] = MoveMode::Stop;
            return BtStatus::Success;
        // ----------------------------------------------------------------- conditions
        case BtNodeType::IsNearGoal: {
            const std::int32_t g = goalFor(n.text);
            if (g < 0) {
                return BtStatus::Failure;
            }
            const Area& area = goalAreas_[static_cast<std::size_t>(g)];
            const bool near = distanceToArea(pos, area) <= n.number[0] && std::abs(pos3.y - area.elevation) < 2.0f;
            return near ? BtStatus::Success : BtStatus::Failure;
        }
        case BtNodeType::IsCrowded: {
            const float radiusSq = n.number[1] * n.number[1];
            std::uint32_t count = 0;
            hash_.query(pos, n.number[1], [&](std::uint32_t j) {
                if (j != i) {
                    const glm::vec2 d = agents_.position[j] - pos;
                    count += glm::dot(d, d) < radiusSq && std::abs(agents_.height[j] - pos3.y) < 1.0f ? 1u : 0u;
                }
            });
            return static_cast<float>(count) >= n.number[0] ? BtStatus::Success : BtStatus::Failure;
        }
        case BtNodeType::IsAreaTagged: {
            const std::uint16_t tag = lookup.tagOf[n.text];
            for (const TaggedVolume& v : taggedVolumes_) {
                if (v.tag == tag && tag != 0 && pos.x >= v.min.x && pos.x <= v.max.x && pos.y >= v.min.z &&
                    pos.y <= v.max.z && pos3.y >= v.min.y - 0.5f && pos3.y <= v.max.y + 2.0f) {
                    return BtStatus::Success;
                }
            }
            return BtStatus::Failure;
        }
        case BtNodeType::RandomChance:
            return agents_.rng[i].nextFloat() < n.number[0] ? BtStatus::Success : BtStatus::Failure;
        case BtNodeType::Custom: {
            // Script node: request a scripted tick (run serially after this phase) and
            // report its result on the next decision tick. Memory: 0 idle, 1 requested,
            // 2 running, 3 succeeded, 4 failed.
            std::uint8_t& m = st.memory[n.memSlot];
            if (m == 3 || m == 4) {
                const bool ok = m == 3;
                m = 0;
                return ok ? BtStatus::Success : BtStatus::Failure;
            }
            if (customRunner_ == nullptr || n.customIndex >= customNodes_.size()) {
                return BtStatus::Failure;
            }
            m = 1;
            customRequests_[thread].push_back({i, agents_.tree[i], index});
            return running();
        }
    }
    return BtStatus::Failure;
}

// ------------------------------------------------------------------------- serial phase

void Simulation::resolveDecisions() {
    CS_PROFILE_ZONE();
    // Arrivals (counted serially: goal counters are shared).
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        if (agents_.arrivalPending[i] != 0) {
            agents_.arrivalPending[i] = 0;
            if (agents_.goalIndex[i] < goals_.size()) {
                registry_.get<Goal>(goals_[agents_.goalIndex[i]]).arrivals += 1;
            }
            ++arrivedTotal_;
            events_.push_back(
                {SimEventType::AgentArrived, agents_.id[i], static_cast<std::int32_t>(agents_.goalIndex[i])});
        }
    }

    // Queues: tickets in agent order, admission by capacity, line positions by ticket.
    std::vector<std::tuple<std::int16_t, std::uint32_t, std::uint32_t>> waiting;  // (goal, ticket, agent)
    std::vector<std::uint32_t> admitted(goals_.size(), 0);
    for (std::uint32_t i = 0; i < agents_.size(); ++i) {
        const std::int16_t g = agents_.queueGoal[i];
        if (g < 0 || static_cast<std::size_t>(g) >= goals_.size()) {
            continue;
        }
        std::uint8_t& stage = agents_.queueStage[i];
        if (stage == 4) {
            stage = 0;
            agents_.queueGoal[i] = -1;
            continue;
        }
        if (stage == 2 || stage == 3) {
            ++admitted[static_cast<std::size_t>(g)];
        } else if (stage == 1) {
            if (agents_.queueTicket[i] == 0xFFFFFFFFu) {
                agents_.queueTicket[i] = registry_.get<Goal>(goals_[static_cast<std::size_t>(g)]).nextTicket++;
            }
            waiting.emplace_back(g, agents_.queueTicket[i], i);
        }
    }
    std::sort(waiting.begin(), waiting.end());
    std::size_t w = 0;
    while (w < waiting.size()) {
        const std::int16_t g = std::get<0>(waiting[w]);
        const Goal& goal = registry_.get<Goal>(goals_[static_cast<std::size_t>(g)]);
        const glm::vec2 dir =
            glm::length(goal.queueDirection) > 1e-3f ? glm::normalize(goal.queueDirection) : glm::vec2(0.0f, -1.0f);
        const float extent = std::abs(dir.x) * goal.area.halfExtents.x + std::abs(dir.y) * goal.area.halfExtents.y;
        const glm::vec2 origin = goal.area.center + dir * (extent + goal.queueSpacing);
        std::uint32_t inLine = 0;
        for (; w < waiting.size() && std::get<0>(waiting[w]) == g; ++w) {
            const std::uint32_t i = std::get<2>(waiting[w]);
            const bool slotFree = goal.capacity == 0 || admitted[static_cast<std::size_t>(g)] < goal.capacity;
            if (slotFree && inLine == 0) {
                agents_.queueStage[i] = 2;
                ++admitted[static_cast<std::size_t>(g)];
                continue;
            }
            const glm::vec2 slot = origin + dir * (goal.queueSpacing * static_cast<float>(inLine));
            agents_.queueSlot[i] = {slot.x, goal.area.elevation, slot.y};
            ++inLine;
        }
    }
}

void Simulation::processDespawns() {
    CS_PROFILE_ZONE();
    for (std::uint32_t i = agents_.size(); i-- > 0;) {
        if (agents_.despawnRequested[i] != 0) {
            ++despawnedTotal_;
            events_.push_back({SimEventType::AgentDespawned, agents_.id[i], -1});
            removeAgentAt(i);
        }
    }
}

// ------------------------------------------------------------------------- path corridors

bool Simulation::planCorridor(std::uint32_t i, std::uint32_t thread) {
    const glm::vec3 target = agents_.moveTarget[i];
    agents_.corridorTarget[i] = target;
    agents_.corridorLength[i] = 0;
    const auto end = navMesh_.findNearest(target, {1.5f, 2.0f, 1.5f}, thread);
    if (!end || agents_.navPoly[i] == 0) {
        agents_.corridorStatus[i] = 2;
        return false;
    }
    const float start[3] = {agents_.position[i].x, agents_.height[i], agents_.position[i].y};
    const float goal[3] = {end->point.x, end->point.y, end->point.z};
    std::array<dtPolyRef, kCorridorCapacity> path{};
    int count = 0;
    const dtStatus st = navMesh_.query(thread).findPath(agents_.navPoly[i], end->ref, start, goal, &navMesh_.filter(),
                                                        path.data(), &count, static_cast<int>(path.size()));
    // A partial path that cannot leave the current polygon makes no progress; fail
    // instead of replanning forever (target on an unreachable island).
    if (dtStatusFailed(st) || count == 0 ||
        (dtStatusDetail(st, DT_PARTIAL_RESULT) && count == 1 && path[0] == agents_.navPoly[i])) {
        agents_.corridorStatus[i] = 2;
        return false;
    }
    std::copy_n(path.begin(), count, agents_.corridor[i].begin());
    agents_.corridorLength[i] = static_cast<std::uint8_t>(count);
    agents_.corridorStatus[i] = 1;
    return true;
}

glm::vec3 Simulation::corridorSteeringTarget(std::uint32_t i, std::uint32_t thread) {
    const glm::vec3 pos3(agents_.position[i].x, agents_.height[i], agents_.position[i].y);
    if (agents_.corridorStatus[i] == 0 && !planCorridor(i, thread)) {
        return pos3;
    }
    if (agents_.corridorStatus[i] == 2) {
        return pos3;
    }
    // Advance the corridor to the polygon the agent stands on (replan once if it left it).
    auto advance = [&]() {
        auto& c = agents_.corridor[i];
        const std::uint8_t len = agents_.corridorLength[i];
        for (std::uint8_t k = 0; k < len; ++k) {
            if (c[k] == agents_.navPoly[i]) {
                if (k > 0) {
                    std::copy(c.begin() + k, c.begin() + len, c.begin());
                    agents_.corridorLength[i] = static_cast<std::uint8_t>(len - k);
                }
                return true;
            }
        }
        return false;
    };
    if (!advance() && (!planCorridor(i, thread) || !advance())) {
        return pos3;
    }
    const auto& c = agents_.corridor[i];
    const std::uint32_t len = agents_.corridorLength[i];
    if (len <= 1) {
        // Same polygon as the target (or a partial path ending here): go straight; a
        // partial path is replanned once its end is reached.
        if (len == 1) {
            const auto targetPoly = navMesh_.findNearest(agents_.moveTarget[i], {1.5f, 2.0f, 1.5f}, thread);
            if (targetPoly && targetPoly->ref != c[0]) {
                agents_.corridorStatus[i] = 0;
            }
        }
        return agents_.moveTarget[i];
    }
    std::array<glm::vec3, kPortalLookahead> lefts{};
    std::array<glm::vec3, kPortalLookahead> rights{};
    std::uint32_t count = 0;
    for (std::uint32_t k = 0; k + 1 < len && count < kPortalLookahead; ++k) {
        if (!navMesh_.portal(c[k], c[k + 1], lefts[count], rights[count], thread)) {
            break;
        }
        funnel::shrinkPortal(lefts[count], rights[count]);
        ++count;
    }
    const glm::vec3 end = count + 1 == len ? agents_.moveTarget[i] : 0.5f * (lefts[count - 1] + rights[count - 1]);
    return count == 0 ? agents_.moveTarget[i] : funnel::steeringTarget(pos3, lefts.data(), rights.data(), count, end);
}

}  // namespace crowdsim::sim
