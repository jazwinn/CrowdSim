#pragma once

/// @file BehaviorTree.hpp
/// Behaviour tree assets for the decision layer. A tree description (parsed
/// from *.bt.json by assets/) is compiled into a flat, cache-friendly node
/// array shared by every agent using it. Each agent keeps only a fixed-size
/// BtState (composite memory, timers, a small blackboard, debug trace): no
/// per-agent allocation, no maps in the hot path. See ADR 0007.

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

namespace crowdsim::sim {

enum class BtStatus : std::uint8_t { Success, Failure, Running };

enum class BtNodeType : std::uint8_t {
    // Composites
    Sequence,
    Selector,
    Parallel,
    RandomSelector,
    WeightedRandom,
    // Decorators
    Inverter,
    Succeeder,
    Repeat,
    Cooldown,
    Timeout,
    // Actions
    MoveToGoal,
    MoveToPoint,
    Wander,
    Wait,
    Queue,
    FollowGroup,
    FleeFrom,
    AvoidArea,
    SetSpeed,
    SetState,
    SetEscalatorStyle,
    SetEscalatorBoarding,
    SetQueueManners,
    SetPushing,
    UseEmergencyRoutes,
    Despawn,
    // Conditions
    IsNearGoal,
    IsCrowded,
    IsBlocked,
    IsAreaTagged,
    RandomChance,
    // Script-defined (M6)
    Custom,
};

enum class BtCategory : std::uint8_t { Composite, Decorator, Action, Condition };
enum class BtParamKind : std::uint8_t { Number, Text, Vec3 };

struct BtParamSpec {
    const char* name;
    BtParamKind kind;
    float defaultNumber;
    const char* defaultText;
    const char* description;
};

struct BtNodeTypeInfo {
    BtNodeType type;
    const char* name;
    BtCategory category;
    std::uint8_t minChildren;
    std::uint8_t maxChildren;
    std::uint8_t paramCount;
    std::array<BtParamSpec, 3> params;
    const char* description;
};

/// Every built-in node type with its parameter schema (used by validation, the
/// node editor and MCP's bt_node_types).
[[nodiscard]] std::span<const BtNodeTypeInfo> btNodeTypes();
[[nodiscard]] const BtNodeTypeInfo* findBtNodeType(std::string_view name);
[[nodiscard]] const BtNodeTypeInfo& btNodeTypeInfo(BtNodeType type);

// --- Description (input to the compiler) ----------------------------------------

struct BtParamValue {
    float number = 0.0f;
    std::string text;
    glm::vec3 vec{0.0f};
    BtParamKind kind = BtParamKind::Number;
};

/// Parameter of a script-defined node type.
struct CustomParamSpec {
    std::string name;
    BtParamKind kind = BtParamKind::Number;
    float defaultNumber = 0.0f;
    std::string defaultText;
    glm::vec3 defaultVec{0.0f};
    std::string description;
};

/// A script-defined (Lua) leaf node type. Its tick runs in the scripting layer,
/// batched per node instance after the parallel decision tick (see ScriptHooks.hpp).
struct CustomNodeSpec {
    std::string name;
    BtCategory category = BtCategory::Action;  ///< Action or Condition
    std::vector<CustomParamSpec> params;
    std::string description;
};

struct BtNodeDesc {
    std::string type;
    std::string id;  ///< stable node id (editor / debugger); generated if empty
    std::map<std::string, BtParamValue> params;
    std::vector<BtNodeDesc> children;
};

// --- Compiled tree ----------------------------------------------------------------

inline constexpr std::uint32_t kBtMaxNodes = 64;
inline constexpr std::uint32_t kBtMaxMemory = 24;
inline constexpr std::uint32_t kBtMaxTimers = 8;
inline constexpr std::uint8_t kBtNoSlot = 0xFF;

struct BtNode {
    BtNodeType type = BtNodeType::Sequence;
    std::uint8_t childCount = 0;
    std::uint8_t memSlot = kBtNoSlot;    ///< composite / decorator memory
    std::uint8_t timerSlot = kBtNoSlot;  ///< Wait / Cooldown / Timeout
    std::uint16_t firstChild = 0;        ///< index into CompiledTree::childList
    std::uint16_t subtreeEnd = 0;        ///< one past the last node of this subtree (pre-order)
    std::uint16_t text = 0;              ///< index into CompiledTree::strings (0 = "")
    float number[2] = {0.0f, 0.0f};
    glm::vec3 vec{0.0f};
    bool hasVec = false;
    std::uint16_t weights = 0;       ///< WeightedRandom: offset into floatPool
    std::uint16_t customIndex = 0;   ///< Custom: index of its CustomNodeSpec
    std::uint16_t customParams = 0;  ///< Custom: index into CompiledTree::customParams
};

struct CompiledTree {
    std::string name;
    std::uint32_t generation = 0;  ///< bumped on hot reload; agents restart from the root
    std::vector<BtNode> nodes;     ///< pre-order; nodes[0] is the root
    std::vector<std::uint16_t> childList;
    std::vector<std::string> strings{""};
    std::vector<float> floatPool;
    std::vector<std::string> nodeIds;                               ///< per node, for the debugger / node editor
    std::vector<std::string> nodeTypes;                             ///< per node type name
    std::vector<std::map<std::string, BtParamValue>> customParams;  ///< resolved params of Custom nodes
    std::uint8_t memSlots = 0;
    std::uint8_t timerSlots = 0;

    [[nodiscard]] std::span<const std::uint16_t> children(const BtNode& n) const {
        return {childList.data() + n.firstChild, n.childCount};
    }
};

/// Validate and compile. Throws std::invalid_argument with the offending node
/// path on unknown types, wrong child counts, bad parameters or size limits.
/// `customNodes` are the script-defined node types (a node's customIndex is its position).
[[nodiscard]] CompiledTree compileBehaviorTree(const BtNodeDesc& root, const std::string& name,
                                               std::span<const CustomNodeSpec> customNodes = {});

/// Reconstruct a description from a compiled tree (node ids, types, parameters
/// and structure; editor layout is not part of the compiled form).
[[nodiscard]] BtNodeDesc decompileBehaviorTree(const CompiledTree& tree);

/// Per-agent decision state. Fixed size, lives in the AgentPool.
struct BtState {
    std::uint32_t generation = 0;  ///< tree generation the state belongs to
    std::array<std::uint8_t, kBtMaxMemory> memory{};
    std::array<float, kBtMaxTimers> timers{};
    // Blackboard
    glm::vec3 target{0.0f};
    std::uint8_t hasTarget = 0;
    // Debug trace of the last tick: bit i = node i was visited / is running.
    std::uint64_t visited = 0;
    std::uint64_t running = 0;
    BtStatus lastStatus = BtStatus::Running;

    void reset(std::uint32_t gen) {
        *this = BtState{};
        generation = gen;
    }
};

}  // namespace crowdsim::sim
