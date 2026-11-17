#include "crowdsim/sim/BehaviorTree.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept>

namespace crowdsim::sim {

namespace {

using K = BtParamKind;
constexpr BtParamSpec kNone{"", K::Number, 0.0f, "", ""};

// Composite children limit: kBtMaxNodes bounds the tree anyway.
constexpr std::uint8_t kMany = 32;

constexpr std::array<BtNodeTypeInfo, 33> kNodeTypes = {{
    {BtNodeType::Sequence,
     "Sequence",
     BtCategory::Composite,
     1,
     kMany,
     0,
     {kNone, kNone, kNone},
     "Run children in order; fail on the first failure, succeed when all succeed. Resumes a running child."},
    {BtNodeType::Selector,
     "Selector",
     BtCategory::Composite,
     1,
     kMany,
     0,
     {kNone, kNone, kNone},
     "Run children in order until one succeeds; fail if all fail. Resumes a running child."},
    {BtNodeType::Parallel,
     "Parallel",
     BtCategory::Composite,
     1,
     kMany,
     1,
     {BtParamSpec{"successThreshold", K::Number, 1.0f, "", "children that must succeed"}, kNone, kNone},
     "Tick all children every time; succeed once `successThreshold` succeed, fail when that becomes impossible."},
    {BtNodeType::RandomSelector,
     "RandomSelector",
     BtCategory::Composite,
     1,
     kMany,
     0,
     {kNone, kNone, kNone},
     "Pick one child uniformly at random and run it to completion."},
    {BtNodeType::WeightedRandom,
     "WeightedRandom",
     BtCategory::Composite,
     1,
     kMany,
     1,
     {BtParamSpec{"weights", K::Text, 0.0f, "", "comma-separated weight per child, e.g. \"3,1\""}, kNone, kNone},
     "Pick one child with the given weights and run it to completion."},
    {BtNodeType::Inverter,
     "Inverter",
     BtCategory::Decorator,
     1,
     1,
     0,
     {kNone, kNone, kNone},
     "Swap success and failure of the child."},
    {BtNodeType::Succeeder,
     "Succeeder",
     BtCategory::Decorator,
     1,
     1,
     0,
     {kNone, kNone, kNone},
     "Always succeed once the child finishes."},
    {BtNodeType::Repeat,
     "Repeat",
     BtCategory::Decorator,
     1,
     1,
     1,
     {BtParamSpec{"count", K::Number, 0.0f, "", "times to run the child; 0 = forever"}, kNone, kNone},
     "Run the child repeatedly while it succeeds; fail as soon as it fails."},
    {BtNodeType::Cooldown,
     "Cooldown",
     BtCategory::Decorator,
     1,
     1,
     1,
     {BtParamSpec{"seconds", K::Number, 5.0f, "", "minimum time between runs"}, kNone, kNone},
     "Fail while cooling down; after the child finishes, block it for `seconds`."},
    {BtNodeType::Timeout,
     "Timeout",
     BtCategory::Decorator,
     1,
     1,
     1,
     {BtParamSpec{"seconds", K::Number, 10.0f, "", "maximum running time"}, kNone, kNone},
     "Fail if the child is still running after `seconds`."},
    {BtNodeType::MoveToGoal,
     "MoveToGoal",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"goal", K::Text, 0.0f, "", "goal name; empty = the goal its spawner gave it"}, kNone, kNone},
     "Walk to a goal along its flow field; succeed when inside the goal area."},
    {BtNodeType::MoveToPoint,
     "MoveToPoint",
     BtCategory::Action,
     0,
     0,
     2,
     {BtParamSpec{"point", K::Vec3, 0.0f, "", "target; omit to use the blackboard target"},
      BtParamSpec{"tolerance", K::Number, 0.5f, "", "arrival distance (m)"}, kNone},
     "Walk to a point along a navmesh path; succeed on arrival, fail if unreachable."},
    {BtNodeType::Wander,
     "Wander",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"radius", K::Number, 8.0f, "", "how far from the current position"}, kNone, kNone},
     "Pick a random reachable point within `radius` and walk to it; succeed on arrival."},
    {BtNodeType::Wait,
     "Wait",
     BtCategory::Action,
     0,
     0,
     2,
     {BtParamSpec{"seconds", K::Number, 2.0f, "", "duration"},
      BtParamSpec{"jitter", K::Number, 0.0f, "", "random extra seconds in [0, jitter)"}, kNone},
     "Stand still for `seconds`."},
    {BtNodeType::Queue,
     "Queue",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"goal", K::Text, 0.0f, "", "goal to queue at; empty = the goal its spawner gave it"}, kNone, kNone},
     "Join the goal's queue, wait for a free slot (goal capacity), get served for the goal's serviceTime."},
    {BtNodeType::FollowGroup,
     "FollowGroup",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"distance", K::Number, 1.2f, "", "preferred distance behind the leader"}, kNone, kNone},
     "Follow the group leader (keeps running). Fails for the leader itself or ungrouped agents."},
    {BtNodeType::FleeFrom,
     "FleeFrom",
     BtCategory::Action,
     0,
     0,
     2,
     {BtParamSpec{"tag", K::Text, 0.0f, "danger", "object tag to flee from"},
      BtParamSpec{"radius", K::Number, 10.0f, "", "danger radius (m)"}, kNone},
     "Run away from the nearest object with `tag` within `radius`; fail when there is none."},
    {BtNodeType::AvoidArea,
     "AvoidArea",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"tag", K::Text, 0.0f, "", "object tag whose area to keep out of; empty clears"}, kNone, kNone},
     "Make locomotion steer around objects with `tag` from now on."},
    {BtNodeType::SetSpeed,
     "SetSpeed",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"multiplier", K::Number, 1.0f, "", "multiplies the preferred speed"}, kNone, kNone},
     "Change walking speed."},
    {BtNodeType::SetState,
     "SetState",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"name", K::Text, 0.0f, "walking", "state label (debug colours, metrics)"}, kNone, kNone},
     "Set the agent's state label."},
    {BtNodeType::SetEscalatorStyle,
     "SetEscalatorStyle",
     BtCategory::Action,
     0,
     0,
     3,
     {BtParamSpec{"side", K::Text, 0.0f, "either",
                  "left, right, either (stay on your side; near the middle the shorter line) or shorter"},
      BtParamSpec{"walk", K::Number, 0.0f, "", "own pace on the steps, x preferred speed; 0 = stand still"},
      BtParamSpec{"clear", K::Number, 2.0f, "", "m to walk straight on after stepping off before turning"}},
     "Choose how the agent rides escalators from now on: which side, standing or walking."},
    {BtNodeType::SetEscalatorBoarding,
     "SetEscalatorBoarding",
     BtCategory::Action,
     0,
     0,
     2,
     {BtParamSpec{"gap", K::Number, 0.9f, "", "m standers leave to the person ahead before stepping on"},
      BtParamSpec{"squeeze", K::Number, 1.0f, "",
                  "1 = when pushed to the front on the wrong side, step on (or switch lanes) anyway"},
      kNone},
     "Choose how the agent steps onto escalators from now on."},
    {BtNodeType::SetQueueManners,
     "SetQueueManners",
     BtCategory::Action,
     0,
     0,
     2,
     {BtParamSpec{"gap", K::Number, 0.6f, "", "m (centre to centre) to stop behind the person ahead; 0 = off"},
      BtParamSpec{"ease", K::Number, 0.5f, "", "m over which to slow down before that"}, kNone},
     "From now on, slow to a stop behind whoever is ahead in your line instead of pressing into them."},
    {BtNodeType::SetPushing,
     "SetPushing",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"force", K::Number, 100.0f, "",
                  "N pressed towards where you are going once bodies touch (0 = only shuffle along)"},
      kNone, kNone},
     "From now on, push into a packed crowd this hard (scene bodies; the default is 100 N)."},
    {BtNodeType::UseEmergencyRoutes,
     "UseEmergencyRoutes",
     BtCategory::Action,
     0,
     0,
     1,
     {BtParamSpec{"use", K::Number, 1.0f, "", "1 = also take emergency routes, 0 = everyday routes only"}, kNone,
      kNone},
     "From now on, find the way to goals through emergency routes too (objects tagged emergency: fire stairs, exit "
     "doors); everyone else keeps out of them."},
    {BtNodeType::SetAttention,
     "SetAttention",
     BtCategory::Action,
     0,
     0,
     2,
     {BtParamSpec{"phone", K::Number, 0.0f, "", "share of the time spent looking at a phone, 0..1"},
      BtParamSpec{"interest", K::Number, 1.0f, "",
                  "inclination to look around at attention targets, x the usual (0 = never)"},
      kNone},
     "From now on, spend this share of the time on the phone and look around this much (attention model; does not "
     "change movement)."},
    {BtNodeType::Despawn,
     "Despawn",
     BtCategory::Action,
     0,
     0,
     0,
     {kNone, kNone, kNone},
     "Remove the agent from the simulation."},
    {BtNodeType::IsNearGoal,
     "IsNearGoal",
     BtCategory::Condition,
     0,
     0,
     2,
     {BtParamSpec{"distance", K::Number, 3.0f, "", "distance to the goal area (m)"},
      BtParamSpec{"goal", K::Text, 0.0f, "", "goal name; empty = the spawner's goal"}, kNone},
     "Succeed if within `distance` of the goal area."},
    {BtNodeType::IsCrowded,
     "IsCrowded",
     BtCategory::Condition,
     0,
     0,
     2,
     {BtParamSpec{"threshold", K::Number, 6.0f, "", "neighbour count"},
      BtParamSpec{"radius", K::Number, 2.0f, "", "radius (m)"}, kNone},
     "Succeed if at least `threshold` agents are within `radius`."},
    {BtNodeType::IsBlocked,
     "IsBlocked",
     BtCategory::Condition,
     0,
     0,
     1,
     {BtParamSpec{"seconds", K::Number, 5.0f, "", "how long the agent has wanted to move but hardly moved"}, kNone,
      kNone},
     "Succeed if the agent has been held up (moving at under a quarter of its wish) for `seconds`."},
    {BtNodeType::IsAreaTagged,
     "IsAreaTagged",
     BtCategory::Condition,
     0,
     0,
     1,
     {BtParamSpec{"tag", K::Text, 0.0f, "", "object tag"}, kNone, kNone},
     "Succeed if the agent stands inside the footprint of an object with `tag`."},
    {BtNodeType::RandomChance,
     "RandomChance",
     BtCategory::Condition,
     0,
     0,
     1,
     {BtParamSpec{"probability", K::Number, 0.5f, "", "chance of success per evaluation"}, kNone, kNone},
     "Succeed with the given probability."},
    {BtNodeType::Custom,
     "Custom",
     BtCategory::Action,
     0,
     0,
     0,
     {kNone, kNone, kNone},
     "Script-defined node (registered from Lua)."},
}};

std::vector<float> parseWeights(const std::string& text) {
    std::vector<float> w;
    const char* p = text.data();
    const char* end = text.data() + text.size();
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',')) {
            ++p;
        }
        if (p >= end) {
            break;
        }
        float v = 0.0f;
        const auto [next, ec] = std::from_chars(p, end, v);
        if (ec != std::errc{}) {
            throw std::invalid_argument("weights must be numbers, got \"" + text + "\"");
        }
        w.push_back(v);
        p = next;
    }
    return w;
}

class Compiler {
public:
    Compiler(CompiledTree& tree, std::span<const CustomNodeSpec> custom) : t_(tree), custom_(custom) {}

    std::uint16_t compile(const BtNodeDesc& d, const std::string& path) {
        if (t_.nodes.size() >= kBtMaxNodes) {
            throw std::invalid_argument(std::format("{}: tree exceeds {} nodes", path, kBtMaxNodes));
        }
        const BtNodeTypeInfo* info = findBtNodeType(d.type);
        int customIndex = -1;
        if (info == nullptr || info->type == BtNodeType::Custom) {
            for (std::size_t c = 0; c < custom_.size() && customIndex < 0; ++c) {
                if (custom_[c].name == d.type) {
                    customIndex = static_cast<int>(c);
                }
            }
            if (customIndex < 0) {
                throw std::invalid_argument(std::format("{}: unknown node type '{}'", path, d.type));
            }
            info = &btNodeTypeInfo(BtNodeType::Custom);
        }
        const auto index = static_cast<std::uint16_t>(t_.nodes.size());
        t_.nodes.emplace_back();
        t_.nodeIds.push_back(d.id.empty() ? std::format("n{}", index) : d.id);
        t_.nodeTypes.push_back(d.type);

        const auto n = d.children.size();
        if (customIndex < 0 && (n < info->minChildren || n > info->maxChildren)) {
            throw std::invalid_argument(std::format("{}: {} takes {}..{} children, has {}", path, info->name,
                                                    info->minChildren, info->maxChildren, n));
        }

        BtNode node;
        node.type = info->type;
        node.customIndex = static_cast<std::uint16_t>(std::max(0, customIndex));

        // Parameters: defaults, overridden by the description; unknown names are errors
        // (custom nodes validate their own parameters when they are registered).
        if (customIndex < 0) {
            for (const auto& [key, value] : d.params) {
                bool known = false;
                for (std::uint8_t i = 0; i < info->paramCount; ++i) {
                    known = known || key == info->params[i].name;
                }
                if (!known) {
                    throw std::invalid_argument(std::format("{}: {} has no parameter '{}'", path, info->name, key));
                }
            }
        }
        int numberIndex = 0;
        for (std::uint8_t i = 0; i < info->paramCount; ++i) {
            const BtParamSpec& spec = info->params[i];
            const auto it = d.params.find(spec.name);
            switch (spec.kind) {
                case BtParamKind::Number: {
                    float v = spec.defaultNumber;
                    if (it != d.params.end()) {
                        if (it->second.kind != BtParamKind::Number) {
                            throw std::invalid_argument(std::format("{}: '{}' must be a number", path, spec.name));
                        }
                        v = it->second.number;
                    }
                    if (numberIndex < 2) {
                        node.number[numberIndex++] = v;
                    }
                    break;
                }
                case BtParamKind::Text: {
                    std::string v = spec.defaultText;
                    if (it != d.params.end()) {
                        if (it->second.kind != BtParamKind::Text) {
                            throw std::invalid_argument(std::format("{}: '{}' must be a string", path, spec.name));
                        }
                        v = it->second.text;
                    }
                    node.text = intern(v);
                    break;
                }
                case BtParamKind::Vec3:
                    if (it != d.params.end()) {
                        if (it->second.kind != BtParamKind::Vec3) {
                            throw std::invalid_argument(std::format("{}: '{}' must be [x, y, z]", path, spec.name));
                        }
                        node.vec = it->second.vec;
                        node.hasVec = true;
                    }
                    break;
            }
        }
        if (node.type == BtNodeType::SetEscalatorStyle) {
            const std::string& side = t_.strings[node.text];
            if (side != "left" && side != "right" && side != "either" && side != "shorter") {
                throw std::invalid_argument(std::format(
                    "{}: SetEscalatorStyle side must be left, right, either or shorter, got '{}'", path, side));
            }
        }
        if ((node.type == BtNodeType::SetEscalatorStyle || node.type == BtNodeType::SetEscalatorBoarding ||
             node.type == BtNodeType::SetQueueManners || node.type == BtNodeType::SetPushing ||
             node.type == BtNodeType::UseEmergencyRoutes || node.type == BtNodeType::SetAttention ||
             node.type == BtNodeType::IsBlocked) &&
            (node.number[0] < 0.0f || node.number[1] < 0.0f)) {
            throw std::invalid_argument(std::format("{}: {} parameters must not be negative", path, info->name));
        }
        if (node.type == BtNodeType::SetAttention && node.number[0] > 1.0f) {
            throw std::invalid_argument(std::format("{}: SetAttention phone is a share of the time, 0..1", path));
        }
        // Script nodes: leaves whose parameters are checked against the registered spec
        // and stored (with defaults filled in) for the script to read.
        if (customIndex >= 0) {
            const CustomNodeSpec& spec = custom_[static_cast<std::size_t>(customIndex)];
            if (!d.children.empty()) {
                throw std::invalid_argument(std::format("{}: script node {} takes no children", path, spec.name));
            }
            std::map<std::string, BtParamValue> values;
            for (const auto& [key, value] : d.params) {
                const auto it = std::find_if(spec.params.begin(), spec.params.end(),
                                             [&](const CustomParamSpec& p) { return p.name == key; });
                if (it == spec.params.end()) {
                    throw std::invalid_argument(std::format("{}: {} has no parameter '{}'", path, spec.name, key));
                }
                if (value.kind != it->kind) {
                    throw std::invalid_argument(std::format("{}: '{}' has the wrong type", path, key));
                }
            }
            for (const CustomParamSpec& p : spec.params) {
                const auto it = d.params.find(p.name);
                if (it != d.params.end()) {
                    values.emplace(p.name, it->second);
                } else {
                    BtParamValue v;
                    v.kind = p.kind;
                    v.number = p.defaultNumber;
                    v.text = p.defaultText;
                    v.vec = p.defaultVec;
                    values.emplace(p.name, std::move(v));
                }
            }
            node.text = intern(spec.name);
            node.customParams = static_cast<std::uint16_t>(t_.customParams.size());
            t_.customParams.push_back(std::move(values));
        }

        // Value checks.
        switch (node.type) {
            case BtNodeType::Parallel:
                if (node.number[0] < 1.0f || node.number[0] > static_cast<float>(n)) {
                    throw std::invalid_argument(std::format("{}: successThreshold must be 1..{}", path, n));
                }
                break;
            case BtNodeType::Repeat:
                if (node.number[0] < 0.0f || node.number[0] > 255.0f) {
                    throw std::invalid_argument(std::format("{}: count must be 0..255", path));
                }
                break;
            case BtNodeType::RandomChance:
                if (node.number[0] < 0.0f || node.number[0] > 1.0f) {
                    throw std::invalid_argument(std::format("{}: probability must be 0..1", path));
                }
                break;
            case BtNodeType::WeightedRandom: {
                const std::vector<float> w = parseWeights(t_.strings[node.text]);
                if (w.size() != n) {
                    throw std::invalid_argument(
                        std::format("{}: weights has {} values for {} children", path, w.size(), n));
                }
                if (std::any_of(w.begin(), w.end(), [](float x) { return x < 0.0f; })) {
                    throw std::invalid_argument(std::format("{}: weights must be >= 0", path));
                }
                node.weights = static_cast<std::uint16_t>(t_.floatPool.size());
                t_.floatPool.insert(t_.floatPool.end(), w.begin(), w.end());
                break;
            }
            case BtNodeType::Wait:
            case BtNodeType::Cooldown:
            case BtNodeType::Timeout:
            case BtNodeType::Wander:
                if (node.number[0] < 0.0f) {
                    throw std::invalid_argument(std::format("{}: value must be >= 0", path));
                }
                break;
            default:
                break;
        }

        // Memory and timer slots.
        switch (node.type) {
            case BtNodeType::Sequence:
            case BtNodeType::Selector:
            case BtNodeType::RandomSelector:
            case BtNodeType::WeightedRandom:
            case BtNodeType::Repeat:
            case BtNodeType::Custom:
                node.memSlot = allocate(t_.memSlots, kBtMaxMemory, path, "memory");
                break;
            case BtNodeType::Wait:
            case BtNodeType::Cooldown:
            case BtNodeType::Timeout:
            case BtNodeType::Queue:
                node.timerSlot = allocate(t_.timerSlots, kBtMaxTimers, path, "timer");
                break;
            default:
                break;
        }

        std::vector<std::uint16_t> kids;
        for (std::size_t c = 0; c < n; ++c) {
            kids.push_back(compile(d.children[c], std::format("{}/{}[{}]", path, d.children[c].type, c)));
        }
        node.firstChild = static_cast<std::uint16_t>(t_.childList.size());
        node.childCount = static_cast<std::uint8_t>(n);
        node.subtreeEnd = static_cast<std::uint16_t>(t_.nodes.size());
        t_.childList.insert(t_.childList.end(), kids.begin(), kids.end());
        t_.nodes[index] = node;
        return index;
    }

private:
    std::uint16_t intern(const std::string& s) {
        const auto it = std::find(t_.strings.begin(), t_.strings.end(), s);
        if (it != t_.strings.end()) {
            return static_cast<std::uint16_t>(it - t_.strings.begin());
        }
        t_.strings.push_back(s);
        return static_cast<std::uint16_t>(t_.strings.size() - 1);
    }

    static std::uint8_t allocate(std::uint8_t& counter, std::uint32_t limit, const std::string& path,
                                 const char* what) {
        if (counter >= limit) {
            throw std::invalid_argument(std::format("{}: tree uses more than {} {} slots", path, limit, what));
        }
        return counter++;
    }

    CompiledTree& t_;
    std::span<const CustomNodeSpec> custom_;
};

}  // namespace

std::span<const BtNodeTypeInfo> btNodeTypes() {
    return kNodeTypes;
}

const BtNodeTypeInfo* findBtNodeType(std::string_view name) {
    for (const BtNodeTypeInfo& info : kNodeTypes) {
        if (name == info.name) {
            return &info;
        }
    }
    return nullptr;
}

const BtNodeTypeInfo& btNodeTypeInfo(BtNodeType type) {
    for (const BtNodeTypeInfo& info : kNodeTypes) {
        if (info.type == type) {
            return info;
        }
    }
    return kNodeTypes.back();
}

namespace {

BtNodeDesc decompileNode(const CompiledTree& tree, std::uint16_t index) {
    const BtNode& n = tree.nodes[index];
    BtNodeDesc d;
    d.type = tree.nodeTypes[index];
    d.id = tree.nodeIds[index];
    if (n.type == BtNodeType::Custom && n.customParams < tree.customParams.size()) {
        d.params = tree.customParams[n.customParams];
    }
    if (n.type != BtNodeType::Custom) {
        const BtNodeTypeInfo& info = btNodeTypeInfo(n.type);
        int numberIndex = 0;
        for (std::uint8_t p = 0; p < info.paramCount; ++p) {
            const BtParamSpec& spec = info.params[p];
            BtParamValue v;
            v.kind = spec.kind;
            switch (spec.kind) {
                case BtParamKind::Number:
                    v.number = numberIndex < 2 ? n.number[numberIndex] : 0.0f;
                    ++numberIndex;
                    break;
                case BtParamKind::Text:
                    v.text = tree.strings[n.text];
                    break;
                case BtParamKind::Vec3:
                    if (!n.hasVec) {
                        continue;  // optional point parameter left out
                    }
                    v.vec = n.vec;
                    break;
            }
            d.params.emplace(spec.name, std::move(v));
        }
    }
    for (std::uint16_t child : tree.children(n)) {
        d.children.push_back(decompileNode(tree, child));
    }
    return d;
}

}  // namespace

BtNodeDesc decompileBehaviorTree(const CompiledTree& tree) {
    return tree.nodes.empty() ? BtNodeDesc{} : decompileNode(tree, 0);
}

CompiledTree compileBehaviorTree(const BtNodeDesc& root, const std::string& name,
                                 std::span<const CustomNodeSpec> customNodes) {
    CompiledTree tree;
    tree.name = name;
    Compiler compiler(tree, customNodes);
    compiler.compile(root, root.type);
    return tree;
}

}  // namespace crowdsim::sim
