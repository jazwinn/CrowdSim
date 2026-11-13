// Agent mass: a heavier agent gives way less and pushes a lighter one aside;
// equal masses avoid each other exactly as before.

#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include <glm/geometric.hpp>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/sim/BehaviorTree.hpp"
#include "crowdsim/sim/Orca.hpp"
#include "crowdsim/sim/Simulation.hpp"

using namespace crowdsim;

TEST_CASE("Mass responsibility: equal shares, lighter gives way, pairs add up to one") {
    using sim::orca::massResponsibility;
    CHECK(massResponsibility(75.0f, 75.0f) == doctest::Approx(0.5f));
    CHECK(massResponsibility(50.0f, 100.0f) == doctest::Approx(0.75f));
    CHECK(massResponsibility(100.0f, 50.0f) == doctest::Approx(0.25f));
    CHECK(massResponsibility(40.0f, 120.0f) + massResponsibility(120.0f, 40.0f) == doctest::Approx(1.0f));
    CHECK(massResponsibility(0.0f, 80.0f) == doctest::Approx(0.5f));  // invalid input: as usual
}

namespace {

struct Outcome {
    float bystanderMoved = 0.0f;  ///< how far the standing agent was displaced, m
    float walkerDetour = 0.0f;    ///< largest sideways deviation of the walker, m
};

/// A corridor: a bystander stands at z = 0 on the walker's straight line from z = -6
/// to a goal at z = 10. Returns how much each one had to move.
Outcome passBy(float walkerMass, float bystanderMass) {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{4};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
    geometry::PrimitiveDesc floor;
    floor.position = {0.0f, -0.2f, 2.0f};
    floor.size = {8.0f, 0.2f, 24.0f};
    sim.addStaticObject(sim::StaticObject{"floor", floor, {}, {}, {}});

    sim::BtNodeDesc wait{"Wait", "w", {}, {}};
    wait.params["seconds"].number = 1000.0f;
    sim.setBehaviorTree(sim::compileBehaviorTree(wait, "stand"));
    sim.setBehaviorTree(sim::compileBehaviorTree(sim::BtNodeDesc{"MoveToGoal", "g", {}, {}}, "walk"));
    auto type = [&](const char* name, const char* tree, float mass) {
        sim::AgentType t;
        t.name = name;
        t.behaviour = tree;
        t.params.radiusMin = t.params.radiusMax = 0.25f;
        t.params.preferredSpeedMin = t.params.preferredSpeedMax = 1.3f;
        t.params.massMin = t.params.massMax = mass;
        sim.defineAgentType(t);
    };
    type("bystander", "stand", bystanderMass);
    type("walker", "walk", walkerMass);

    sim::Goal goal;
    goal.name = "end";
    goal.area = {{0.0f, 11.0f}, {0.5f, 0.5f}, 0.0f};
    const sim::SceneId goalId = sim.addGoal(goal);
    auto spawn = [&](const char* typeName, float z) {
        sim::Spawner s;
        s.name = typeName;
        s.area = {{0.0f, z}, {0.02f, 0.02f}, 0.0f};
        s.rate = 30.0f;
        s.total = 1;
        s.goal = goalId;
        s.agentMix = {{typeName, 1.0f}};
        sim.addSpawner(s);
    };
    spawn("bystander", 0.0f);
    spawn("walker", -6.0f);

    Outcome out;
    glm::vec2 bystanderStart{0.0f};
    bool started = false;
    for (int step = 0; step < 30 * 12; ++step) {
        sim.step();
        const sim::AgentPool& a = sim.agents();
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            const bool walker = sim.agentTypes()[a.agentType[i]].name == "walker";
            if (walker) {
                out.walkerDetour = std::max(out.walkerDetour, std::abs(a.position[i].x));
            } else if (!started) {
                // Measure from 1.5 s in: by then its tree has it standing still, and the
                // walker (4 m/s away at 1.3 m/s) has not reached it yet.
                if (step == 45) {
                    bystanderStart = a.position[i];
                    started = true;
                }
            } else {
                out.bystanderMoved = std::max(out.bystanderMoved, glm::length(a.position[i] - bystanderStart));
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("A heavier walker pushes a lighter bystander aside instead of going round") {
    const Outcome equal = passBy(75.0f, 75.0f);
    const Outcome heavy = passBy(110.0f, 50.0f);
    const Outcome light = passBy(50.0f, 110.0f);
    CAPTURE(equal.bystanderMoved);
    CAPTURE(equal.walkerDetour);
    CAPTURE(heavy.bystanderMoved);
    CAPTURE(heavy.walkerDetour);
    // Equal masses split the side-step about evenly.
    CHECK(std::abs(equal.bystanderMoved - equal.walkerDetour) < 0.1f);
    // The light bystander is shoved further than an equal one, and the heavy walker
    // swerves less; the bystander now does most of the moving.
    CHECK(heavy.bystanderMoved > equal.bystanderMoved + 0.05f);
    CHECK(heavy.walkerDetour < equal.walkerDetour - 0.05f);
    CHECK(heavy.bystanderMoved > 1.5f * heavy.walkerDetour);
    // Facing someone heavier, a light walker goes round and barely moves them.
    CHECK(light.walkerDetour > heavy.walkerDetour);
    CHECK(light.bystanderMoved < heavy.bystanderMoved);
}
