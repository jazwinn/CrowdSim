// Escalators: stairs tagged "escalator" carry agents; the SetEscalatorStyle behaviour
// tree node sets who stands or walks and on which side.

#include <doctest/doctest.h>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/sim/BehaviorTree.hpp"
#include "crowdsim/sim/Simulation.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

using namespace crowdsim;

namespace {

constexpr float kRise = 6.0f;
constexpr float kRun = 10.4f;  // 30 degrees
constexpr float kBelt = 0.5f;  // m/s along the slope

struct World {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{4};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};

    /// `style` is "stand-left", "walk-right", ...: a tree that sets it, then walks to the goal.
    explicit World(const std::string& style) {
        auto box = [&](glm::vec3 pos, glm::vec3 size) {
            geometry::PrimitiveDesc d;
            d.position = pos;
            d.size = size;
            sim.addStaticObject(sim::StaticObject{"floor", d, {}, {}, {}});
        };
        box({0.0f, -0.2f, -kRun / 2 - 6.0f}, {8.0f, 0.2f, 12.0f});        // lower landing
        box({0.0f, kRise - 0.2f, kRun / 2 + 6.0f}, {8.0f, 0.2f, 12.0f});  // upper landing
        geometry::PrimitiveDesc esc;
        esc.kind = geometry::PrimitiveKind::Stairs;
        esc.size = {2.4f, kRise, kRun};
        esc.steps = 30;
        sim.addStaticObject(sim::StaticObject{"escalator", esc, {}, {}, {"escalator", "escalator:speed=0.5"}});

        const std::size_t dash = style.find('-');
        sim::BtNodeDesc setStyle{"SetEscalatorStyle", "style", {}, {}};
        setStyle.params["side"].kind = sim::BtParamKind::Text;
        setStyle.params["side"].text = style.substr(dash + 1);
        setStyle.params["walk"].number = style.starts_with("walk") ? 1.0f : 0.0f;
        sim::BtNodeDesc root{"Sequence", "root", {}, {setStyle, sim::BtNodeDesc{"MoveToGoal", "walk", {}, {}},
                                                      sim::BtNodeDesc{"Despawn", "leave", {}, {}}}};
        sim.setBehaviorTree(sim::compileBehaviorTree(root, style));

        sim::AgentType stander;
        stander.name = style;
        stander.behaviour = style;
        stander.params.radiusMin = stander.params.radiusMax = 0.24f;
        stander.params.preferredSpeedMin = stander.params.preferredSpeedMax = 1.3f;
        stander.personalSpace = 0.03f;
        sim.defineAgentType(stander);

        sim::Goal goal;
        goal.name = "top";
        goal.area = {{0.0f, kRun / 2 + 9.0f}, {3.0f, 1.5f}, kRise};
        const sim::SceneId goalId = sim.addGoal(goal);
        sim::Spawner s;
        s.name = "bottom";
        s.area = {{0.0f, -kRun / 2 - 9.0f}, {0.5f, 0.5f}, 0.0f};
        s.rate = 5.0f;
        s.total = 1;
        s.goal = goalId;
        s.agentMix = {{style, 1.0f}};
        sim.addSpawner(s);
    }
};

struct Ride {
    float seconds = 0.0f;        ///< time spent on the moving steps
    float meanLateral = 0.0f;    ///< mean x while riding (left is -x when facing +z)
    float maxGaitSpeed = 0.0f;   ///< largest walking speed the animation saw while riding
    bool reachedTop = false;
};

Ride ride(const std::string& type) {
    World w(type);
    Ride r;
    int samples = 0;
    snapshot::RenderSnapshot snap;
    for (int step = 0; step < 30 * 90 && !r.reachedTop; ++step) {
        w.sim.step();
        const auto& a = w.sim.agents();
        if (a.size() == 0) {
            r.reachedTop = w.sim.stats().arrived == 1;
            continue;
        }
        const glm::vec2 p = a.position[0];
        // Middle of the escalator only, away from stepping on and off.
        if (p.y > -kRun / 2 + 1.0f && p.y < kRun / 2 - 1.0f) {
            r.seconds += static_cast<float>(w.sim.stepSeconds());
            r.meanLateral += p.x;
            ++samples;
            w.sim.writeSnapshot(snap);
            r.maxGaitSpeed = std::max(r.maxGaitSpeed, snap.agents[0].speed);
        }
        r.reachedTop = r.reachedTop || w.sim.stats().arrived == 1;
    }
    r.meanLateral /= static_cast<float>(std::max(samples, 1));
    return r;
}

}  // namespace

TEST_CASE("Escalator objects are found and carry standers at belt speed in their lane") {
    World w("stand-left");
    REQUIRE(w.sim.updateNavigation());
    REQUIRE(w.sim.escalators().size() == 1);
    const sim::Escalator& e = w.sim.escalators()[0];
    CHECK(e.forward.y == doctest::Approx(1.0f));
    CHECK(e.right.x == doctest::Approx(1.0f));  // +x is on the right when facing +z
    CHECK(e.entryHeight == doctest::Approx(0.0f));
    CHECK(e.exitHeight == doctest::Approx(kRise));
    const float horizontal = kBelt * kRun / std::sqrt(kRun * kRun + kRise * kRise);
    CHECK(e.beltSpeed == doctest::Approx(horizontal));

    const Ride left = ride("stand-left");
    REQUIRE(left.reachedTop);
    // The middle (kRun - 2 m) takes (kRun - 2) / belt speed when standing still.
    CHECK(left.seconds == doctest::Approx((kRun - 2.0f) / horizontal).epsilon(0.1));
    CHECK(left.meanLateral < -0.15f);
    CHECK(left.maxGaitSpeed < 0.2f);  // carried, not walking

    const Ride right = ride("stand-right");
    REQUIRE(right.reachedTop);
    CHECK(right.meanLateral > 0.15f);
}

TEST_CASE("Walkers climb the moving steps faster than standers") {
    const Ride stander = ride("stand-left");
    const Ride walker = ride("walk-right");
    REQUIRE(walker.reachedTop);
    CHECK(walker.seconds < stander.seconds * 0.7f);
    CHECK(walker.meanLateral > 0.15f);
    CHECK(walker.maxGaitSpeed > 0.3f);
}

TEST_CASE("SetEscalatorStyle rejects an unknown side") {
    sim::BtNodeDesc bad{"SetEscalatorStyle", "style", {}, {}};
    bad.params["side"].kind = sim::BtParamKind::Text;
    bad.params["side"].text = "middle";
    CHECK_THROWS_AS((void)sim::compileBehaviorTree(bad, "bad"), std::invalid_argument);
}
