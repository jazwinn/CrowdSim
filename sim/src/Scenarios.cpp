#include "crowdsim/sim/Scenarios.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::sim {

namespace {

using geometry::kSurfaceObstacle;
using geometry::kSurfaceWalkable;
using geometry::PrimitiveDesc;
using geometry::PrimitiveKind;

constexpr std::uint32_t kFloorColor = 0xff6a625cu;  // ABGR
constexpr std::uint32_t kWallColor = 0xffb8b0a8u;
constexpr std::uint32_t kStairColor = 0xff7d8a96u;
constexpr std::uint32_t kPillarColor = 0xff9aa2aau;
constexpr std::uint32_t kFurnitureColor = 0xff4a6a8au;

void addBox(Simulation& sim, std::string name, glm::vec3 position, glm::vec3 size, std::uint8_t flags,
            std::uint32_t color) {
    PrimitiveDesc d;
    d.kind = PrimitiveKind::Box;
    d.position = position;
    d.size = size;
    d.flags = flags;
    d.color = color;
    sim.addStaticObject(StaticObject{std::move(name), d, {}, {}, {}});
}

void addGround(Simulation& sim, float halfX, float halfZ) {
    addBox(sim, "Ground", {0.0f, -0.2f, 0.0f}, {2.0f * halfX, 0.2f, 2.0f * halfZ}, kSurfaceWalkable, kFloorColor);
}

// --- Tiny behaviour-tree builder ---------------------------------------------------------
BtParamValue num(float v) {
    BtParamValue p;
    p.kind = BtParamKind::Number;
    p.number = v;
    return p;
}
BtParamValue text(std::string v) {
    BtParamValue p;
    p.kind = BtParamKind::Text;
    p.text = std::move(v);
    return p;
}
BtNodeDesc node(std::string type, std::string id, std::map<std::string, BtParamValue> params = {},
                std::vector<BtNodeDesc> children = {}) {
    return BtNodeDesc{std::move(type), std::move(id), std::move(params), std::move(children)};
}

AgentType makeType(const char* name, const char* tree, glm::vec3 color, float speedMin, float speedMax,
                   float personalSpace) {
    AgentType t;
    t.name = name;
    t.behaviour = tree;
    t.params.color = color;
    t.params.colorJitter = 0.1f;
    t.params.preferredSpeedMin = speedMin;
    t.params.preferredSpeedMax = speedMax;
    t.personalSpace = personalSpace;
    return t;
}

void buildPlaza(Simulation& sim, std::uint32_t agents) {
    defineStandardAgentTypes(sim);
    addGround(sim, 60.0f, 60.0f);
    addBox(sim, "Fountain", {0.0f, 0.0f, 0.0f}, {8.0f, 1.0f, 8.0f}, kSurfaceObstacle, kWallColor);
    for (auto [x, z] : {std::pair{-9.0f, 0.0f}, std::pair{9.0f, 0.0f}, std::pair{0.0f, -9.0f}, std::pair{0.0f, 9.0f}}) {
        const bool alongX = z != 0.0f;
        addBox(sim, "Bench", {x, 0.0f, z}, {alongX ? 3.0f : 0.8f, 0.5f, alongX ? 0.8f : 3.0f}, kSurfaceObstacle,
               kFurnitureColor);
    }
    // Four kiosks with a service window each; lines extend away from the building.
    struct Kiosk {
        const char* name;
        float x;
        float z;
        float dir;
    };
    const Kiosk kiosks[] = {{"Kiosk A", -25.0f, 25.0f, -1.0f},
                            {"Kiosk B", 25.0f, 25.0f, -1.0f},
                            {"Kiosk C", -25.0f, -25.0f, 1.0f},
                            {"Kiosk D", 25.0f, -25.0f, 1.0f}};
    for (const Kiosk& k : kiosks) {
        addBox(sim, std::string(k.name) + " building", {k.x, 0.0f, k.z - k.dir * 1.0f}, {4.0f, 3.0f, 2.0f},
               kSurfaceObstacle, kPillarColor);
        Goal g{.name = k.name, .area = {{k.x, k.z + k.dir * 1.5f}, {1.5f, 0.8f}}};
        g.capacity = 4;
        g.serviceTime = 3.0f;
        g.queueDirection = {0.0f, k.dir};
        sim.addGoal(g);
    }
    const SceneId east = sim.addGoal(Goal{.name = "East exit", .area = {{57.0f, 0.0f}, {2.0f, 25.0f}}});
    const SceneId west = sim.addGoal(Goal{.name = "West exit", .area = {{-57.0f, 0.0f}, {2.0f, 25.0f}}});

    Spawner south;
    south.name = "South gate";
    south.area = {{0.0f, -55.0f}, {20.0f, 3.0f}};
    south.rate = std::max(1.0f, static_cast<float>(agents) / 120.0f);
    south.total = agents / 2;
    south.goal = east;
    south.agentMix = {{"commuter", 0.55f}, {"wanderer", 0.25f}, {"queuer", 0.06f}, {"follower", 0.14f}};
    south.groupSizeMin = 1;
    south.groupSizeMax = 3;
    sim.addSpawner(south);

    Spawner north = south;
    north.name = "North gate";
    north.area = {{0.0f, 55.0f}, {20.0f, 3.0f}};
    north.total = agents - agents / 2;
    north.goal = west;
    sim.addSpawner(north);
}

void buildBuilding(Simulation& sim, std::uint32_t agents) {
    // Ground floor slab 60 x 40 m, top at y = 0.
    addBox(sim, "Ground floor", {0.0f, -0.2f, 0.0f}, {60.0f, 0.2f, 40.0f}, kSurfaceWalkable, kFloorColor);
    // Upper floor covers the northern half (z 0..20), top at y = 4.
    addBox(sim, "Upper floor", {0.0f, 3.8f, 10.0f}, {60.0f, 0.2f, 20.0f}, kSurfaceWalkable, kFloorColor);

    // Outer walls.
    addBox(sim, "Wall N", {0.0f, 0.0f, 20.15f}, {60.6f, 8.0f, 0.3f}, kSurfaceObstacle, kWallColor);
    addBox(sim, "Wall S", {0.0f, 0.0f, -20.15f}, {60.6f, 8.0f, 0.3f}, kSurfaceObstacle, kWallColor);
    addBox(sim, "Wall W", {-30.15f, 0.0f, 0.0f}, {0.3f, 8.0f, 40.6f}, kSurfaceObstacle, kWallColor);
    addBox(sim, "Wall E", {30.15f, 0.0f, 0.0f}, {0.3f, 8.0f, 40.6f}, kSurfaceObstacle, kWallColor);

    // Stairs (8 m wide, 20 steps of 0.2 m) rising north to the upper floor edge at z = 0.
    PrimitiveDesc stairs;
    stairs.kind = PrimitiveKind::Stairs;
    stairs.position = {0.0f, 0.0f, -4.0f};
    stairs.size = {8.0f, 4.0f, 8.0f};
    stairs.steps = 20;
    stairs.flags = kSurfaceWalkable;
    stairs.color = kStairColor;
    sim.addStaticObject(StaticObject{"Stairs", stairs, {}, {}, {}});

    // Ramp (6 m wide, 16 m long, ~14 degrees) in the east.
    PrimitiveDesc ramp;
    ramp.kind = PrimitiveKind::Ramp;
    ramp.position = {25.0f, 0.0f, -8.0f};
    ramp.size = {6.0f, 4.0f, 16.0f};
    ramp.flags = kSurfaceWalkable;
    ramp.color = kStairColor;
    sim.addStaticObject(StaticObject{"Ramp", ramp, {}, {}, {}});

    // Railing along the upper floor edge, open where stairs, ramp and ladder arrive.
    const float railY = 4.0f;
    for (auto [x0, x1] :
         {std::pair{-29.7f, -28.0f}, std::pair{-26.0f, -4.0f}, std::pair{4.0f, 22.0f}, std::pair{28.0f, 29.7f}}) {
        addBox(sim, "Railing", {0.5f * (x0 + x1), railY, 0.1f}, {x1 - x0, 1.1f, 0.2f}, kSurfaceObstacle, kWallColor);
    }

    // Upper floor partition with three doorways (x = -20, 0, 20; 3 m wide).
    for (auto [x0, x1] :
         {std::pair{-30.0f, -21.5f}, std::pair{-18.5f, -1.5f}, std::pair{1.5f, 18.5f}, std::pair{21.5f, 30.0f}}) {
        addBox(sim, "Partition", {0.5f * (x0 + x1), railY, 10.0f}, {x1 - x0, 3.0f, 0.3f}, kSurfaceObstacle, kWallColor);
    }

    // Pillars under the upper floor and benches on the ground floor.
    for (float x : {-20.0f, -10.0f, 10.0f, 20.0f}) {
        for (float z : {5.0f, 15.0f}) {
            addBox(sim, "Pillar", {x, 0.0f, z}, {0.6f, 3.8f, 0.6f}, kSurfaceObstacle, kPillarColor);
        }
    }
    for (float x : {-12.0f, 12.0f}) {
        addBox(sim, "Bench", {x, 0.0f, -10.0f}, {3.0f, 0.5f, 0.8f}, kSurfaceObstacle, kFurnitureColor);
    }

    // Ladder in the west corner: a one-way off-mesh link up to the upper floor.
    sim.addOffMeshLink(OffMeshLink{{-27.0f, 0.0f, -1.5f}, {-27.0f, 4.0f, 1.5f}, 0.6f, false});

    const std::uint32_t goal = sim.addGoal(Goal{.name = "Upper exit", .area = {{0.0f, 17.0f}, {26.0f, 2.0f}, 4.0f}});
    Spawner s;
    s.name = "Ground entrance";
    s.area = {{0.0f, -16.0f}, {20.0f, 3.0f}, 0.0f};
    s.rate = std::max(1.0f, static_cast<float>(agents) / 60.0f);
    s.total = agents;
    s.goal = goal;
    sim.addSpawner(s);
}

}  // namespace

std::optional<ScenarioKind> parseScenario(std::string_view name) {
    for (ScenarioKind k : {ScenarioKind::Stream, ScenarioKind::Counterflow, ScenarioKind::Benchmark,
                           ScenarioKind::Building, ScenarioKind::Plaza}) {
        if (scenarioName(k) == name) {
            return k;
        }
    }
    return std::nullopt;
}

std::string_view scenarioName(ScenarioKind kind) {
    switch (kind) {
        case ScenarioKind::Stream:
            return "stream";
        case ScenarioKind::Counterflow:
            return "counterflow";
        case ScenarioKind::Benchmark:
            return "benchmark";
        case ScenarioKind::Building:
            return "building";
        case ScenarioKind::Plaza:
            return "plaza";
    }
    return "unknown";
}

void buildScenario(Simulation& sim, ScenarioKind kind, std::uint32_t agents) {
    sim.clearScene();

    switch (kind) {
        case ScenarioKind::Stream: {
            // All agents spawn within ~40 s; the walk takes ~90 s, so the full
            // population is alive at once around t = 40..85 s.
            addGround(sim, 40.0f, 75.0f);
            const std::uint32_t goal = sim.addGoal(Goal{.name = "Exit", .area = {{0.0f, 60.0f}, {30.0f, 3.0f}}});
            Spawner s;
            s.name = "Entrance";
            s.area = {{0.0f, -60.0f}, {30.0f, 6.0f}};
            s.rate = std::max(1.0f, static_cast<float>(agents) / 40.0f);
            s.total = agents;
            s.goal = goal;
            sim.addSpawner(s);
            break;
        }
        case ScenarioKind::Counterflow:
        case ScenarioKind::Benchmark: {
            const bool burst = kind == ScenarioKind::Benchmark;
            // Benchmark: ~1.5 m^2 per agent so even 50k agents fit in the two spawn areas.
            const float halfWidth = burst ? std::max(60.0f, 0.0075f * static_cast<float>(agents)) : 30.0f;
            const float spawnDepth = burst ? 25.0f : 6.0f;
            const float rate = burst ? static_cast<float>(agents) : std::max(1.0f, static_cast<float>(agents) / 80.0f);
            addGround(sim, halfWidth + 10.0f, 80.0f);
            const std::uint32_t north = sim.addGoal(Goal{.name = "North", .area = {{0.0f, 70.0f}, {halfWidth, 3.0f}}});
            const std::uint32_t south = sim.addGoal(Goal{.name = "South", .area = {{0.0f, -70.0f}, {halfWidth, 3.0f}}});

            Spawner a;
            a.name = "South entrance";
            a.area = {{0.0f, -55.0f + spawnDepth * 0.5f}, {halfWidth, spawnDepth}};
            a.rate = rate;
            a.total = agents / 2;
            a.goal = north;
            a.agent.color = {0.9f, 0.55f, 0.25f};
            sim.addSpawner(a);

            Spawner b = a;
            b.name = "North entrance";
            b.area = {{0.0f, 55.0f - spawnDepth * 0.5f}, {halfWidth, spawnDepth}};
            b.total = agents - agents / 2;
            b.goal = south;
            b.agent.color = {0.3f, 0.6f, 0.95f};
            b.agent.agentType = 1;
            sim.addSpawner(b);
            break;
        }
        case ScenarioKind::Building:
            buildBuilding(sim, agents);
            break;
        case ScenarioKind::Plaza:
            buildPlaza(sim, agents);
            break;
    }
}

void defineStandardAgentTypes(Simulation& sim) {
    // Content files (loaded first) win: only define what is missing.
    auto defineTree = [&sim](CompiledTree tree) {
        if (sim.findBehaviorTree(tree.name) == nullptr) {
            sim.setBehaviorTree(std::move(tree));
        }
    };
    auto defineType = [&sim](const AgentType& type) {
        if (sim.findAgentType(type.name) == nullptr) {
            sim.defineAgentType(type);
        }
    };
    defineTree(compileBehaviorTree(node("Sequence", "root", {},
                                        {node("SetState", "label", {{"name", text("walking")}}),
                                         node("MoveToGoal", "walk"), node("Despawn", "leave")}),
                                   "commuter"));
    defineTree(
        compileBehaviorTree(node("Sequence", "root", {},
                                 {node("SetState", "label", {{"name", text("wandering")}}),
                                  node("SetSpeed", "stroll", {{"multiplier", num(0.75f)}}),
                                  node("Repeat", "loop", {{"count", num(3.0f)}},
                                       {node("Sequence", "visit", {},
                                             {node("Wander", "wander", {{"radius", num(14.0f)}}),
                                              node("Wait", "look", {{"seconds", num(2.0f)}, {"jitter", num(3.0f)}})})}),
                                  node("SetState", "label2", {{"name", text("leaving")}}),
                                  node("SetSpeed", "normal", {{"multiplier", num(1.0f)}}), node("MoveToGoal", "exit"),
                                  node("Despawn", "leave")}),
                            "wanderer"));
    defineTree(compileBehaviorTree(node("Sequence", "root", {},
                                        {node("SetState", "label", {{"name", text("queueing")}}),
                                         node("WeightedRandom", "pick", {{"weights", text("1,1,1,1")}},
                                              {node("Queue", "kioskA", {{"goal", text("Kiosk A")}}),
                                               node("Queue", "kioskB", {{"goal", text("Kiosk B")}}),
                                               node("Queue", "kioskC", {{"goal", text("Kiosk C")}}),
                                               node("Queue", "kioskD", {{"goal", text("Kiosk D")}})}),
                                         node("SetState", "label2", {{"name", text("leaving")}}),
                                         node("MoveToGoal", "exit"), node("Despawn", "leave")}),
                                   "queuer"));
    defineTree(compileBehaviorTree(node("Selector", "root", {},
                                        {node("Sequence", "inGroup", {},
                                              {node("SetState", "label", {{"name", text("following")}}),
                                               node("FollowGroup", "follow", {{"distance", num(1.2f)}})}),
                                         node("Sequence", "lead", {},
                                              {node("SetState", "label2", {{"name", text("leading")}}),
                                               node("SetSpeed", "pace", {{"multiplier", num(0.85f)}}),
                                               node("MoveToGoal", "walk"), node("Despawn", "leave")})}),
                                   "follower"));
    defineTree(
        compileBehaviorTree(node("Selector", "root", {},
                                 {node("Sequence", "escape", {},
                                       {node("SetState", "label", {{"name", text("fleeing")}}),
                                        node("SetSpeed", "run", {{"multiplier", num(1.4f)}}),
                                        node("FleeFrom", "flee", {{"tag", text("danger")}, {"radius", num(12.0f)}})}),
                                  node("Sequence", "evacuate", {},
                                       {node("SetState", "label2", {{"name", text("evacuating")}}),
                                        node("AvoidArea", "avoid", {{"tag", text("danger")}}),
                                        node("MoveToGoal", "exit"), node("Despawn", "leave")})}),
                            "evacuee"));

    defineType(makeType("commuter", "commuter", {0.85f, 0.55f, 0.25f}, 1.15f, 1.55f, 0.06f));
    defineType(makeType("wanderer", "wanderer", {0.29f, 0.64f, 0.87f}, 0.9f, 1.3f, 0.1f));
    defineType(makeType("queuer", "queuer", {0.36f, 0.75f, 0.42f}, 1.0f, 1.4f, 0.08f));
    defineType(makeType("follower", "follower", {0.63f, 0.42f, 0.84f}, 1.0f, 1.35f, 0.04f));
    defineType(makeType("evacuee", "evacuee", {0.88f, 0.31f, 0.24f}, 1.6f, 2.1f, 0.02f));
}

}  // namespace crowdsim::sim
