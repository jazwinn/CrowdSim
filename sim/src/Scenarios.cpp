#include "crowdsim/sim/Scenarios.hpp"

#include <algorithm>
#include <string>

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
    sim.addStaticObject(StaticObject{std::move(name), d});
}

void addGround(Simulation& sim, float halfX, float halfZ) {
    addBox(sim, "Ground", {0.0f, -0.2f, 0.0f}, {2.0f * halfX, 0.2f, 2.0f * halfZ}, kSurfaceWalkable, kFloorColor);
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
    sim.addStaticObject(StaticObject{"Stairs", stairs});

    // Ramp (6 m wide, 16 m long, ~14 degrees) in the east.
    PrimitiveDesc ramp;
    ramp.kind = PrimitiveKind::Ramp;
    ramp.position = {25.0f, 0.0f, -8.0f};
    ramp.size = {6.0f, 4.0f, 16.0f};
    ramp.flags = kSurfaceWalkable;
    ramp.color = kStairColor;
    sim.addStaticObject(StaticObject{"Ramp", ramp});

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
    for (ScenarioKind k :
         {ScenarioKind::Stream, ScenarioKind::Counterflow, ScenarioKind::Benchmark, ScenarioKind::Building}) {
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
    }
}

}  // namespace crowdsim::sim
