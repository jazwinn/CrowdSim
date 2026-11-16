#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/assets/SceneSerializer.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/sim/Simulation.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

using namespace crowdsim;
using commands::Json;
using commands::makeCommand;

namespace {

/// A full command-driven engine without GUI.
struct Engine {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{4};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{.seed = 5}};
    assets::AssetLibrary assets{logs.logger(), CROWDSIM_CONTENT_DIR};
    commands::SimControl control;
    std::string sceneName = "test";
    commands::CommandContext ctx{sim, assets, control, logs.logger(), sceneName};
    commands::CommandRegistry registry;
    commands::CommandDispatcher dispatcher{registry};

    Engine() { commands::registerBuiltinCommands(registry); }

    Json run(const std::string& cmd, Json params = Json::object()) {
        const commands::CommandOutcome o = dispatcher.execute(makeCommand(cmd, std::move(params)), ctx);
        INFO(cmd << ": " << o.error);
        REQUIRE(o.ok);
        return o.result;
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) {
            sim.step();
        }
    }
};

/// Build a small scene the way the editor would: one command per edit.
void buildByCommands(Engine& e) {
    e.run("CreateScene", {{"name", "edited"}});
    e.run("PlaceObject",
          {{"object", {{"name", "Floor"}, {"type", "box"}, {"position", {0, -0.2, 0}}, {"size", {40, 0.2, 30}}}}});
    const auto wall = e.run("PlaceObject", {{"object",
                                             {{"name", "Wall"},
                                              {"type", "box"},
                                              {"position", {0, 0, 0}},
                                              {"size", {0.3, 3, 20}},
                                              {"tags", {"obstacle"}}}}})["id"];
    e.run("TransformObject", {{"id", wall}, {"position", {2, 0, 0}}, {"rotationY", 15}});
    const auto goal = e.run(
        "CreateGoal", {{"goal", {{"name", "Exit"}, {"area", {{"center", {15, 0}}, {"halfExtents", {2, 10}}}}}}})["id"];
    const auto spawner = e.run("CreateSpawner", {{"spawner",
                                                  {{"name", "In"},
                                                   {"area", {{"center", {-15, 0}}, {"halfExtents", {2, 10}}}},
                                                   {"rate", 20},
                                                   {"total", 150},
                                                   {"goal", goal}}}})["id"];
    e.run("SetSpawnerConfig", {{"id", spawner}, {"config", {{"rate", 25}}}});
}

}  // namespace

TEST_CASE("Scene JSON round-trips byte-identically") {
    Engine e;
    sim::buildScenario(e.sim, sim::ScenarioKind::Building, 100);
    const std::string first = assets::saveScene(e.sim, "building").dump(2);
    assets::loadScene(Json::parse(first), e.sim, e.assets);
    const std::string second = assets::saveScene(e.sim, "building").dump(2);
    CHECK(first == second);
    CHECK(Json::parse(first)["objects"].size() == e.sim.staticObjectIds().size());
}

TEST_CASE("glTF import applies tag conventions and builds a navmesh") {
    Engine e;
    e.run("CreateScene");
    const Json r = e.run("ImportGltf", {{"path", "models/test_room.gltf"}, {"name", "Room"}});
    CHECK(r["triangles"].get<int>() == 2 + 12 + 12);
    CHECK(r["walkableTriangles"].get<int>() == 2);
    CHECK(r["obstacleTriangles"].get<int>() == 24);
    e.run("RebuildNavmesh");
    REQUIRE(e.sim.navMesh().valid());
    // glTF +Z is flipped to -Z: the bench at glTF z = +3 lies at z = -3 here, and is not walkable.
    const auto nearBench = e.sim.navMesh().findNearest({-5.0f, 0.0f, -3.0f}, {2.5f, 1.0f, 1.0f});
    REQUIRE(nearBench);
    CHECK(std::abs(nearBench->point.z + 3.0f) > 0.6f);
}

TEST_CASE("Undo / redo restore entity state exactly") {
    Engine e;
    buildByCommands(e);
    const std::string built = assets::saveScene(e.sim, "x").dump();
    const sim::SceneId wall = e.sim.staticObjectIds()[1];

    e.run("DeleteEntity", {{"id", wall}});
    CHECK(e.sim.findStaticObject(wall) == nullptr);
    REQUIRE(e.dispatcher.undo(e.ctx).ok);
    CHECK(e.sim.findStaticObject(wall) != nullptr);
    CHECK(assets::saveScene(e.sim, "x").dump() == built);
    REQUIRE(e.dispatcher.redo(e.ctx).ok);
    CHECK(e.sim.findStaticObject(wall) == nullptr);

    // Merged edits (gizmo drags) undo as one step.
    REQUIRE(e.dispatcher.undo(e.ctx).ok);  // back to the built scene
    const sim::SceneId floor = e.sim.staticObjectIds()[0];
    for (int i = 1; i <= 5; ++i) {
        Json cmd = makeCommand("TransformObject", {{"id", floor}, {"position", {i, -0.2, 0}}});
        cmd["merge"] = "drag";
        REQUIRE(e.dispatcher.execute(cmd, e.ctx).ok);
    }
    REQUIRE(e.dispatcher.undo(e.ctx).ok);
    CHECK(assets::saveScene(e.sim, "x").dump() == built);

    // Undoing CreateScene brings the whole previous scene back.
    e.run("CreateScene");
    CHECK(e.sim.staticObjectIds().empty());
    REQUIRE(e.dispatcher.undo(e.ctx).ok);
    CHECK(assets::saveScene(e.sim, "x").dump() == built);
}

TEST_CASE("Invalid commands fail cleanly without changing state") {
    Engine e;
    buildByCommands(e);
    const std::string before = assets::saveScene(e.sim, "x").dump();
    CHECK_FALSE(e.dispatcher.execute(makeCommand("NoSuchCommand"), e.ctx).ok);
    CHECK_FALSE(e.dispatcher.execute(makeCommand("DeleteEntity", {{"id", 9999}}), e.ctx).ok);
    CHECK_FALSE(e.dispatcher.execute(makeCommand("PlaceObject", {{"object", {{"type", "teapot"}}}}), e.ctx).ok);
    CHECK_FALSE(
        e.dispatcher.execute(makeCommand("PlaceObject", {{"object", {{"type", "box"}, {"size", {-1, 1, 1}}}}}), e.ctx)
            .ok);
    CHECK_FALSE(e.dispatcher.execute(Json("not an object"), e.ctx).ok);
    CHECK(assets::saveScene(e.sim, "x").dump() == before);
}

TEST_CASE("M3: a scene built by commands saves, reloads and replays identically") {
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "crowdsim_m3_test.scene.json";

    // 1. Edit -> run -> keep editing while running -> run more. Record the log.
    Engine a;
    a.dispatcher.restartLog(a.ctx);
    buildByCommands(a);
    a.steps(150);
    a.run("PlaceObject", {{"object",
                           {{"name", "Pillar"},
                            {"type", "box"},
                            {"position", {-5, 0, 4}},
                            {"size", {0.6, 3, 0.6}},
                            {"tags", {"obstacle"}}}}});
    a.steps(150);
    a.dispatcher.undo(a.ctx);  // undo is logged too
    a.steps(100);
    const std::uint64_t originalHash = a.sim.stateHash();
    const Json log = a.dispatcher.logToJson(a.ctx);
    a.run("SaveScene", {{"path", file.string()}});

    // 2. Save -> load -> save is identical.
    Engine b;
    b.run("LoadScene", {{"path", file.string()}});
    CHECK(assets::saveScene(b.sim, a.sceneName).dump(2) == assets::saveScene(a.sim, a.sceneName).dump(2));

    // 3. Replaying the log from scratch reproduces the exact simulation state.
    Engine c;
    const std::uint64_t replayHash = commands::replayLog(Json::parse(log.dump()), c.dispatcher, c.ctx);
    CHECK(replayHash == originalHash);
    CHECK(c.sim.stepIndex() == a.sim.stepIndex());
    CHECK(assets::saveScene(c.sim, "s").dump() == assets::saveScene(a.sim, "s").dump());
    std::filesystem::remove(file);
}

TEST_CASE("Groups: entities nest in groups, save and reload, and undo cleanly") {
    Engine e;
    buildByCommands(e);
    const sim::SceneId floor = e.sim.staticObjectIds()[0];
    const sim::SceneId wall = e.sim.staticObjectIds()[1];
    const sim::SceneId goal = e.sim.goalIds()[0];
    const auto level = e.run("CreateGroup", {{"group", {{"name", "Level 1"}}}})["id"].get<sim::SceneId>();
    const auto walls =
        e.run("CreateGroup", {{"group", {{"name", "Walls"}, {"parent", level}}}})["id"].get<sim::SceneId>();
    e.run("SetParent", {{"id", floor}, {"parent", level}});
    e.run("SetParent", {{"id", wall}, {"parent", walls}});
    e.run("UpdateEntity", {{"id", goal}, {"patch", {{"parent", level}}}});
    CHECK(e.sim.parentOf(walls) == level);
    CHECK(e.sim.parentOf(wall) == walls);
    CHECK(e.sim.childrenOf(level) == std::vector<sim::SceneId>{walls, floor, goal});

    // Only groups hold things, and no group may end up inside itself.
    CHECK_FALSE(e.dispatcher.execute(makeCommand("SetParent", {{"id", wall}, {"parent", floor}}), e.ctx).ok);
    CHECK_FALSE(e.dispatcher.execute(makeCommand("SetParent", {{"id", level}, {"parent", walls}}), e.ctx).ok);
    CHECK(e.sim.parentOf(level) == sim::kNoSceneId);

    // Saved with the groups (parents first) and reloaded the same, whatever the file order.
    const Json saved = assets::saveScene(e.sim, "x");
    REQUIRE(saved["groups"].size() == 2);
    CHECK(saved["groups"][0]["id"] == level);
    Json shuffled = saved;
    std::swap(shuffled["groups"][0], shuffled["groups"][1]);
    assets::loadScene(shuffled, e.sim, e.assets);
    CHECK(assets::saveScene(e.sim, "x").dump() == saved.dump());
    CHECK(e.sim.parentOf(wall) == walls);

    // Deleting a group keeps its contents (they move up); undo puts them back in it.
    e.run("DeleteEntity", {{"id", walls}});
    CHECK(e.sim.findGroup(walls) == nullptr);
    CHECK(e.sim.parentOf(wall) == level);
    REQUIRE(e.dispatcher.undo(e.ctx).ok);
    CHECK(e.sim.parentOf(wall) == walls);
    CHECK(assets::saveScene(e.sim, "x").dump() == saved.dump());

    // Taking something out of a group undoes back into it.
    e.run("SetParent", {{"id", goal}, {"parent", 0}});
    CHECK(e.sim.parentOf(goal) == sim::kNoSceneId);
    REQUIRE(e.dispatcher.undo(e.ctx).ok);
    CHECK(e.sim.parentOf(goal) == level);
}

TEST_CASE("Hiding: a hidden group hides everything in it, for display only") {
    Engine e;
    buildByCommands(e);
    const sim::SceneId floor = e.sim.staticObjectIds()[0];
    const sim::SceneId wall = e.sim.staticObjectIds()[1];
    const sim::SceneId goal = e.sim.goalIds()[0];
    const auto level = e.run("CreateGroup", {{"group", {{"name", "Level"}}}})["id"].get<sim::SceneId>();
    e.run("SetParent", {{"id", wall}, {"parent", level}});
    e.run("SetParent", {{"id", goal}, {"parent", level}});
    e.run("RebuildNavmesh");

    auto drawn = [&]() {
        snapshot::RenderSnapshot snap;
        e.sim.writeSnapshot(snap);
        return std::pair{snap.staticScene->meshVertices.size(), snap.areas.size()};
    };
    const auto [allVertices, allAreas] = drawn();
    const std::size_t wallVertices = 3 * e.sim.objectMesh(wall).triangleCount();

    // Same simulation with and without hiding.
    Engine plain;
    buildByCommands(plain);
    plain.run("RebuildNavmesh");

    e.run("SetHidden", {{"id", level}, {"hidden", true}});
    CHECK(e.sim.isHidden(level));
    CHECK_FALSE(e.sim.isHidden(wall));
    CHECK_FALSE(e.sim.isVisible(wall));  // hidden with its group
    CHECK(e.sim.isVisible(floor));
    const auto [vertices, areas] = drawn();
    CHECK(vertices == allVertices - wallVertices);
    CHECK(areas == allAreas - 1);  // the goal's marker

    e.steps(300);
    plain.steps(300);
    CHECK(e.sim.stateHash() == plain.sim.stateHash());

    // Saved, reloaded, and undone like any other edit.
    const Json saved = assets::saveScene(e.sim, "x");
    assets::loadScene(saved, e.sim, e.assets);
    CHECK(e.sim.isHidden(level));
    CHECK(assets::saveScene(e.sim, "x").dump() == saved.dump());
    e.run("SetHidden", {{"id", floor}, {"hidden", true}});
    CHECK_FALSE(e.sim.isVisible(floor));
    REQUIRE(e.dispatcher.undo(e.ctx).ok);
    CHECK(e.sim.isVisible(floor));
    CHECK_FALSE(e.dispatcher.execute(makeCommand("SetHidden", {{"id", 9999}, {"hidden", true}}), e.ctx).ok);
}
