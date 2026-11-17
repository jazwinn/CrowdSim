// Visual attention (metrics/AttentionModel): an ad (an attention target) beside a corridor is seen, glanced at and noticed
// by people walking past; a wall in between hides it; people on their phones do not read
// it; results do not depend on the thread count.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/assets/SceneSections.hpp"
#include "crowdsim/assets/SceneSerializer.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/metrics/AttentionModel.hpp"
#include "crowdsim/physics/PhysicsWorld.hpp"
#include "crowdsim/sim/BehaviorTree.hpp"
#include "crowdsim/sim/Simulation.hpp"

using namespace crowdsim;

namespace {

struct Options {
    bool wall = false;        ///< an opaque wall between the corridor and the ad
    float phone = 0.0f;       ///< SetAttention phone
    float interest = 3.0f;    ///< SetAttention interest
    bool legacyTag = false;   ///< tag the ad with the older `ad` tags
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
    box("ad", {2.2f, 1.0f, 20.0f}, {0.05f, 1.5f, 3.0f},
        o.legacyTag ? std::vector<std::string>{"obstacle", "ad", "ad:salience=1"}
                    : std::vector<std::string>{"obstacle", "attention", "attention:salience=1"});
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
    metrics::AttentionModel model(sim, jobs, *world);
    for (int step = 0; step < 30 * 90; ++step) {
        sim.step();
    }
    REQUIRE(model.targets().size() == 1);
    return model.summary()["targets"][0];
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

TEST_CASE("Attention is the same whatever the thread count") {
    CHECK(walkPast({.threads = 1}).dump() == walkPast({.threads = 4}).dump());
}

TEST_CASE("The older `ad` tags still make an attention target") {
    CHECK(walkPast({.legacyTag = true}).dump() == walkPast({}).dump());
}

TEST_CASE("SetAttention validates its parameters") {
    sim::BtNodeDesc ok{"SetAttention", "a", {}, {}};
    ok.params["phone"].number = 0.3f;
    CHECK_NOTHROW((void)sim::compileBehaviorTree(ok, "ok"));
    sim::BtNodeDesc tooMuch{"SetAttention", "a", {}, {}};
    tooMuch.params["phone"].number = 1.5f;
    CHECK_THROWS((void)sim::compileBehaviorTree(tooMuch, "bad"));
}

TEST_CASE("SetAttentionParams changes only the settings given, checks them and can be undone") {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{2};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
    assets::AssetLibrary library{logs.logger(), CROWDSIM_CONTENT_DIR};
    commands::SimControl control;
    std::string sceneName = "test";
    commands::CommandContext ctx{sim, library, control, logs.logger(), sceneName};
    commands::CommandRegistry registry;
    commands::CommandDispatcher dispatcher{registry};
    physics::JoltRuntime runtime;
    auto world = physics::createJoltPhysicsWorld(runtime, logs.logger());
    metrics::AttentionModel model(sim, jobs, *world);
    metrics::registerAttentionCommands(registry, model);

    auto set = [&](nlohmann::json params) {
        return dispatcher.execute(commands::makeCommand("SetAttentionParams", {{"params", std::move(params)}}), ctx);
    };
    REQUIRE(set({{"fieldHalfAngle", 45}}).ok);
    CHECK(model.params.fieldHalfAngle == 45.0f);
    CHECK(model.params.fovealHalfAngle == 7.5f);  // untouched
    const commands::CommandOutcome bad = set({{"fovealHalfAngle", 0}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("fovealHalfAngle") != std::string::npos);
    CHECK(model.params.fovealHalfAngle == 7.5f);  // nothing changed
    REQUIRE(dispatcher.undo(ctx).ok);
    CHECK(model.params.fieldHalfAngle == 60.0f);
}

TEST_CASE("Attention settings are saved with the scene and restored when it is loaded") {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{2};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
    assets::AssetLibrary library{logs.logger(), CROWDSIM_CONTENT_DIR};
    physics::JoltRuntime runtime;
    auto world = physics::createJoltPhysicsWorld(runtime, logs.logger());
    metrics::AttentionModel model(sim, jobs, *world);
    assets::SceneSections sections;
    metrics::registerAttentionSceneSection(sections, model);

    // At the defaults the scene has no section, so scenes that do not study attention are unchanged.
    CHECK_FALSE(assets::saveScene(sim, "s", &sections).contains("attention"));

    model.params.fieldHalfAngle = 45.0f;
    const nlohmann::json studied = assets::saveScene(sim, "s", &sections);
    REQUIRE(studied.contains("attention"));
    CHECK(studied["attention"]["fieldHalfAngle"] == 45.0);

    // A scene without the section brings the defaults back; the studied one restores them.
    (void)assets::loadScene(assets::saveScene(sim, "plain"), sim, library, &sections);
    CHECK(model.params.fieldHalfAngle == 60.0f);
    (void)assets::loadScene(studied, sim, library, &sections);
    CHECK(model.params.fieldHalfAngle == 45.0f);

    // An invalid setting is reported with its path and changes nothing.
    nlohmann::json bad = studied;
    bad["attention"]["fieldHalfAngle"] = 500;
    CHECK_THROWS_WITH((void)assets::loadScene(bad, sim, library, &sections),
                      "attention.fieldHalfAngle: must be at least 10 and at most 90");
    CHECK(model.params.fieldHalfAngle == 45.0f);

    CHECK_THROWS((void)sections.add({"objects", nullptr, nullptr}));  // the scene format's own keys
}
