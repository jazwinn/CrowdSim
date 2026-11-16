// Ad attention (metrics/AdAttention): an ad beside a corridor is seen, glanced at and noticed
// by people walking past; a wall in between hides it; people on their phones do not read
// it; results do not depend on the thread count.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/metrics/AdAttention.hpp"
#include "crowdsim/physics/PhysicsWorld.hpp"
#include "crowdsim/sim/BehaviorTree.hpp"
#include "crowdsim/sim/Simulation.hpp"

using namespace crowdsim;

namespace {

struct Options {
    bool wall = false;        ///< an opaque wall between the corridor and the ad
    float phone = 0.0f;       ///< SetAttention phone
    float interest = 3.0f;    ///< SetAttention interest
    std::uint32_t threads = 4;
};

/// 20 people walk 40 m along a corridor past a 3 m x 1.5 m ad on its side, 2.2 m away.
nlohmann::json walkPast(const Options& o) {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{o.threads};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
    auto box = [&](const char* name, glm::vec3 pos, glm::vec3 size, std::vector<std::string> tags) {
        geometry::PrimitiveDesc d;
        d.position = pos;
        d.size = size;
        sim.addStaticObject(sim::StaticObject{name, d, {}, {}, std::move(tags)});
    };
    box("floor", {0.0f, -0.2f, 20.0f}, {4.0f, 0.2f, 44.0f}, {"walkable"});
    box("ad", {2.2f, 1.0f, 20.0f}, {0.05f, 1.5f, 3.0f}, {"obstacle", "ad", "ad:salience=1"});
    if (o.wall) {
        box("wall", {1.9f, 0.0f, 20.0f}, {0.2f, 3.0f, 30.0f}, {"obstacle"});
    }

    sim::BtNodeDesc attention{"SetAttention", "attention", {}, {}};
    attention.params["phone"].number = o.phone;
    attention.params["interest"].number = o.interest;
    sim::BtNodeDesc root{"Sequence", "root", {}, {attention, sim::BtNodeDesc{"MoveToGoal", "go", {}, {}},
                                                  sim::BtNodeDesc{"Despawn", "leave", {}, {}}}};
    sim.setBehaviorTree(sim::compileBehaviorTree(root, "passer"));
    sim::AgentType t;
    t.name = "passer";
    t.behaviour = "passer";
    sim.defineAgentType(t);
    sim::Goal goal;
    goal.name = "end";
    goal.area = {{0.0f, 39.0f}, {1.5f, 1.0f}, 0.0f};
    const sim::SceneId goalId = sim.addGoal(goal);
    sim::Spawner s;
    s.name = "start";
    s.area = {{0.0f, 1.0f}, {1.0f, 0.5f}, 0.0f};
    s.rate = 1.0f;
    s.total = 20;
    s.goal = goalId;
    s.agentMix = {{"passer", 1.0f}};
    sim.addSpawner(s);
    REQUIRE(sim.updateNavigation());

    physics::JoltRuntime runtime;
    auto world = physics::createJoltPhysicsWorld(runtime, logs.logger());
    for (sim::SceneId id : sim.staticObjectIds()) {
        (void)world->addStaticMesh(sim.objectMesh(id));
    }
    metrics::AdAttention ads(sim, jobs, *world);
    for (int step = 0; step < 30 * 90; ++step) {
        sim.step();
    }
    REQUIRE(ads.ads().size() == 1);
    return ads.summary()["ads"][0];
}

}  // namespace

TEST_CASE("People walking past an ad see it, glance at it and notice it") {
    const nlohmann::json ad = walkPast({});
    CAPTURE(ad.dump());
    CHECK(ad["ots"] == 20);
    CHECK(ad["viewable"].get<int>() >= 15);  // it is right beside them for several seconds
    CHECK(ad["glances"].get<int>() > 0);
    CHECK(ad["noticed"].get<int>() > 0);
    CHECK(ad["dwellSeconds"].get<double>() > 0.0);
    CHECK(ad["meanViewingAngle"].get<double>() > 0.0);
}

TEST_CASE("An ad behind a wall has an opportunity to see but is never seen") {
    const nlohmann::json ad = walkPast({.wall = true});
    CAPTURE(ad.dump());
    CHECK(ad["ots"] == 20);
    CHECK(ad["viewable"] == 0);
    CHECK(ad["dwellSeconds"].get<double>() == 0.0);
    CHECK(ad["noticed"] == 0);
}

TEST_CASE("People on their phones do not read ads, and the uninterested do not glance") {
    const nlohmann::json phones = walkPast({.phone = 1.0f});
    CHECK(phones["dwellSeconds"].get<double>() == 0.0);
    CHECK(phones["noticed"] == 0);
    CHECK(phones["glances"] == 0);
    const nlohmann::json bored = walkPast({.interest = 0.0f});
    CHECK(bored["glances"] == 0);
}

TEST_CASE("Ad attention is the same whatever the thread count") {
    CHECK(walkPast({.threads = 1}).dump() == walkPast({.threads = 4}).dump());
}

TEST_CASE("SetAttention validates its parameters") {
    sim::BtNodeDesc ok{"SetAttention", "a", {}, {}};
    ok.params["phone"].number = 0.3f;
    CHECK_NOTHROW((void)sim::compileBehaviorTree(ok, "ok"));
    sim::BtNodeDesc tooMuch{"SetAttention", "a", {}, {}};
    tooMuch.params["phone"].number = 1.5f;
    CHECK_THROWS((void)sim::compileBehaviorTree(tooMuch, "bad"));
}
