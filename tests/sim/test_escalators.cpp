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
        setStyle.params["walk"].number = style.starts_with("walk") ? 0.45f : 0.0f;
        sim::BtNodeDesc root{
            "Sequence",
            "root",
            {},
            {setStyle, sim::BtNodeDesc{"MoveToGoal", "walk", {}, {}}, sim::BtNodeDesc{"Despawn", "leave", {}, {}}}};
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
    float seconds = 0.0f;       ///< time spent on the moving steps
    float meanLateral = 0.0f;   ///< mean x while riding (left is -x when facing +z)
    float maxGaitSpeed = 0.0f;  ///< largest walking speed the animation saw while riding
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

TEST_CASE("Queue manners: a follower stops behind the person ahead instead of pressing past") {
    // A corridor; one person stands still at z = 0, another walks from z = -6 to a goal
    // at z = 12 straight behind them. Polite: stops about `gap` behind. Not: goes round.
    auto run = [](float gap) {
        core::LogService logs{core::LogConfig{.console = false}};
        core::JobSystem jobs{4};
        sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
        geometry::PrimitiveDesc floor;
        floor.position = {0.0f, -0.2f, 3.0f};
        floor.size = {4.0f, 0.2f, 24.0f};
        sim.addStaticObject(sim::StaticObject{"floor", floor, {}, {}, {}});
        sim::BtNodeDesc manners{"SetQueueManners", "m", {}, {}};
        manners.params["gap"].number = gap;
        sim.setBehaviorTree(sim::compileBehaviorTree(
            sim::BtNodeDesc{"Sequence", "r", {}, {manners, sim::BtNodeDesc{"MoveToGoal", "g", {}, {}}}}, "follow"));
        sim::BtNodeDesc wait{"Wait", "w", {}, {}};
        wait.params["seconds"].number = 1000.0f;
        sim.setBehaviorTree(sim::compileBehaviorTree(wait, "stand"));
        for (const char* name : {"follow", "stand"}) {
            sim::AgentType t;
            t.name = name;
            t.behaviour = name;
            t.params.radiusMin = t.params.radiusMax = 0.25f;
            t.params.preferredSpeedMin = t.params.preferredSpeedMax = 1.3f;
            sim.defineAgentType(t);
        }
        sim::Goal goal;
        goal.name = "end";
        goal.area = {{0.0f, 13.0f}, {1.5f, 1.0f}, 0.0f};
        const sim::SceneId goalId = sim.addGoal(goal);
        auto spawn = [&](const char* type, float z) {
            sim::Spawner s;
            s.name = type;
            s.area = {{0.0f, z}, {0.05f, 0.05f}, 0.0f};
            s.rate = 30.0f;
            s.total = 1;
            s.goal = goalId;
            s.agentMix = {{type, 1.0f}};
            sim.addSpawner(s);
        };
        spawn("stand", 0.0f);
        spawn("follow", -6.0f);
        for (int step = 0; step < 30 * 15; ++step) {
            sim.step();
        }
        const sim::AgentPool& a = sim.agents();
        float followerZ = 99.0f;
        float standerZ = 0.0f;
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            (sim.agentTypes()[a.agentType[i]].name == "follow" ? followerZ : standerZ) = a.position[i].y;
        }
        return followerZ - standerZ;  // follower already arrived (despawned or past): large
    };
    const float polite = run(1.5f);
    CHECK(polite < -1.2f);  // still behind, about a gap away
    CHECK(polite > -1.9f);
    CHECK(run(0.0f) > 0.5f);  // without manners: walked round and on
}

TEST_CASE("Behaviour nodes for escalators and queues compile with their parameters") {
    sim::BtNodeDesc style{"SetEscalatorStyle", "s", {}, {}};
    style.params["side"].kind = sim::BtParamKind::Text;
    style.params["side"].text = "shorter";
    style.params["walk"].number = 0.6f;
    style.params["clear"].number = 3.0f;
    sim::BtNodeDesc boarding{"SetEscalatorBoarding", "b", {}, {}};
    boarding.params["gap"].number = 1.2f;
    boarding.params["squeeze"].number = 0.0f;
    sim::BtNodeDesc manners{"SetQueueManners", "m", {}, {}};
    manners.params["gap"].number = 0.7f;
    const sim::CompiledTree t =
        sim::compileBehaviorTree(sim::BtNodeDesc{"Sequence", "r", {}, {style, boarding, manners}}, "ok");
    CHECK(t.nodes[1].number[0] == doctest::Approx(0.6f));
    CHECK(t.nodes[1].number[1] == doctest::Approx(3.0f));
    CHECK(t.nodes[2].number[0] == doctest::Approx(1.2f));
    CHECK(t.nodes[3].number[1] == doctest::Approx(0.5f));  // ease default

    sim::BtNodeDesc negative{"SetQueueManners", "m", {}, {}};
    negative.params["gap"].number = -1.0f;
    CHECK_THROWS_AS((void)sim::compileBehaviorTree(negative, "bad"), std::invalid_argument);
}

TEST_CASE("SetEscalatorStyle rejects an unknown side") {
    sim::BtNodeDesc bad{"SetEscalatorStyle", "style", {}, {}};
    bad.params["side"].kind = sim::BtParamKind::Text;
    bad.params["side"].text = "middle";
    CHECK_THROWS_AS((void)sim::compileBehaviorTree(bad, "bad"), std::invalid_argument);
}

TEST_CASE("Flow fields never route people against the steps") {
    World w("stand-left");  // one up escalator; goal "top" on the upper landing
    sim::Goal bottom;
    bottom.name = "bottom";
    bottom.area = {{0.0f, -kRun / 2 - 9.0f}, {3.0f, 1.5f}, 0.0f};
    (void)w.sim.addGoal(bottom);
    REQUIRE(w.sim.updateNavigation());
    const sim::NavMesh& nav = w.sim.navMesh();
    auto dense = [&](glm::vec3 p) {
        const auto hit = nav.findNearest(p, {1.0f, 1.0f, 1.0f});
        REQUIRE(hit.has_value());
        return nav.denseIndex(hit->ref);
    };
    const std::uint32_t lower = dense({0.0f, 0.0f, -kRun / 2 - 4.0f});
    const std::uint32_t upper = dense({0.0f, kRise, kRun / 2 + 4.0f});
    const auto& fields = w.sim.flowFields();
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].reachable(lower));   // up the escalator: fine
    CHECK(!fields[1].reachable(upper));  // down the up escalator: no route
}

TEST_CASE("Riders stepping off an up escalator do not board the down escalator beside it") {
    // An up and a down escalator side by side; the goal is on the upper landing beyond the
    // head of the down one, so riders walk across its boarding approach.
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{4};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
    auto box = [&](glm::vec3 pos, glm::vec3 size) {
        geometry::PrimitiveDesc d;
        d.position = pos;
        d.size = size;
        sim.addStaticObject(sim::StaticObject{"floor", d, {}, {}, {}});
    };
    box({0.0f, -0.2f, -kRun / 2 - 6.0f}, {12.0f, 0.2f, 12.0f});
    box({0.0f, kRise - 0.2f, kRun / 2 + 6.0f}, {12.0f, 0.2f, 12.0f});
    for (const float x : {-1.5f, 1.5f}) {
        geometry::PrimitiveDesc esc;
        esc.kind = geometry::PrimitiveKind::Stairs;
        esc.position = {x, 0.0f, 0.0f};
        esc.size = {2.4f, kRise, kRun};
        esc.steps = 30;
        std::vector<std::string> tags{"escalator"};
        if (x > 0.0f) {
            tags.emplace_back("escalator:down");
        }
        sim.addStaticObject(sim::StaticObject{"escalator", esc, {}, {}, tags});
    }
    sim::BtNodeDesc root{"Sequence",
                         "root",
                         {},
                         {sim::BtNodeDesc{"MoveToGoal", "go", {}, {}}, sim::BtNodeDesc{"Despawn", "leave", {}, {}}}};
    sim.setBehaviorTree(sim::compileBehaviorTree(root, "up"));
    sim::AgentType t;
    t.name = "up";
    t.behaviour = "up";
    t.params.radiusMin = t.params.radiusMax = 0.24f;
    sim.defineAgentType(t);
    sim::Goal goal;
    goal.name = "beyond";
    goal.area = {{4.5f, kRun / 2 + 2.5f}, {1.0f, 1.0f}, kRise};
    const sim::SceneId goalId = sim.addGoal(goal);
    sim::Spawner s;
    s.name = "bottom";
    s.area = {{-1.5f, -kRun / 2 - 9.0f}, {1.0f, 0.5f}, 0.0f};
    s.rate = 1.0f;
    s.total = 8;
    s.goal = goalId;
    s.agentMix = {{"up", 1.0f}};
    sim.addSpawner(s);

    bool rodeDown = false;
    for (int step = 0; step < 30 * 120; ++step) {
        sim.step();
        const auto& a = sim.agents();
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            // On the down escalator's steps (x > 0, between the landings).
            rodeDown = rodeDown || (a.position[i].x > 0.4f && std::abs(a.position[i].y) < kRun / 2 - 0.5f);
        }
    }
    CHECK_FALSE(rodeDown);
    CHECK(sim.stats().arrived == 8);
}

TEST_CASE("A box tagged escalator is a moving walkway: carries riders and is one-way") {
    // A 20 m walkway (travelator) along +z beside a plain floor strip, landings at both ends.
    auto run = [](bool towardsFar, float& seconds, bool& rodeAgainst) {
        core::LogService logs{core::LogConfig{.console = false}};
        core::JobSystem jobs{4};
        sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
        auto box = [&](glm::vec3 pos, glm::vec3 size, std::vector<std::string> tags) {
            geometry::PrimitiveDesc d;
            d.position = pos;
            d.size = size;
            sim.addStaticObject(sim::StaticObject{"box", d, {}, {}, std::move(tags)});
        };
        box({0.0f, -0.2f, -16.0f}, {8.0f, 0.2f, 12.0f}, {"walkable"});  // near landing
        box({0.0f, -0.2f, 16.0f}, {8.0f, 0.2f, 12.0f}, {"walkable"});   // far landing
        box({2.4f, -0.2f, 0.0f}, {3.2f, 0.2f, 20.0f}, {"walkable"});    // walkway beside it
        box({-1.4f, -0.1f, 0.0f}, {2.4f, 0.1f, 20.0f},                  // the travelator
            {"walkable", "escalator", "escalator:speed=0.75"});
        box({0.6f, 0.0f, 0.0f}, {0.2f, 1.0f, 20.0f}, {"obstacle"});  // balustrade between

        sim::BtNodeDesc stand{"SetEscalatorStyle", "style", {}, {}};
        stand.params["side"].kind = sim::BtParamKind::Text;
        stand.params["side"].text = "either";
        stand.params["walk"].number = 0.0f;
        sim::BtNodeDesc root{
            "Sequence",
            "root",
            {},
            {stand, sim::BtNodeDesc{"MoveToGoal", "go", {}, {}}, sim::BtNodeDesc{"Despawn", "leave", {}, {}}}};
        sim.setBehaviorTree(sim::compileBehaviorTree(root, "stander"));
        sim::AgentType t;
        t.name = "stander";
        t.behaviour = "stander";
        t.params.radiusMin = t.params.radiusMax = 0.24f;
        t.params.preferredSpeedMin = t.params.preferredSpeedMax = 1.3f;
        sim.defineAgentType(t);
        const float from = towardsFar ? -13.0f : 13.0f;
        sim::Goal goal;
        goal.name = "end";
        goal.area = {{0.0f, -from}, {3.0f, 1.0f}, 0.0f};
        const sim::SceneId goalId = sim.addGoal(goal);
        sim::Spawner s;
        s.name = "start";
        s.area = {{-1.4f, from}, {0.3f, 0.3f}, 0.0f};
        s.rate = 5.0f;
        s.total = 1;
        s.goal = goalId;
        s.agentMix = {{"stander", 1.0f}};
        sim.addSpawner(s);
        seconds = 0.0f;
        rodeAgainst = false;
        for (int step = 0; step < 30 * 120 && sim.stats().arrived == 0; ++step) {
            sim.step();
            const auto& a = sim.agents();
            if (a.size() == 1 && a.position[0].x < 0.0f && std::abs(a.position[0].y) < 9.0f) {
                seconds += static_cast<float>(sim.stepSeconds());  // on the travelator
                rodeAgainst = rodeAgainst || !towardsFar;
            }
        }
        CHECK(sim.stats().arrived == 1);
    };
    float seconds = 0.0f;
    bool against = false;
    run(true, seconds, against);
    // Standing still, carried 18 m (the middle stretch) at 0.75 m/s: about 24 s.
    CHECK(seconds == doctest::Approx(18.0f / 0.75f).epsilon(0.15));
    run(false, seconds, against);
    CHECK_FALSE(against);  // the other way round they walk beside it, not against the belt
}
