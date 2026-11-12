// M4 decision layer: behaviour-tree compiler, node semantics, hot reload,
// queues and the plaza scenario.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "ContentWatcher.hpp"
#include "EngineContext.hpp"
#include "crowdsim/assets/BehaviorJson.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/sim/Simulation.hpp"

using namespace crowdsim;
using Json = nlohmann::json;

namespace {

Json tree(const std::string& name, Json root) {
    return Json{{"format", "crowdsim.bt"}, {"version", 1}, {"name", name}, {"root", std::move(root)}};
}

Json leaf(const std::string& type, Json params = Json::object()) {
    Json j{{"type", type}};
    if (!params.empty()) {
        j["params"] = std::move(params);
    }
    return j;
}

Json composite(const std::string& type, Json children, Json params = Json::object()) {
    Json j = leaf(type, std::move(params));
    // nlohmann turns a one-element brace list into that element: wrap it again.
    j["children"] = children.is_array() ? std::move(children) : Json::array({std::move(children)});
    return j;
}

/// Simulation fixture: the stream scenario, re-pointed to spawn `count` agents of
/// a test type that runs `bt`.
struct Fixture {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs;
    sim::Simulation sim;

    explicit Fixture(std::uint32_t threads = 4, std::uint64_t seed = 1)
        : jobs(threads), sim({logs.logger(), jobs}, sim::SimConfig{.seed = seed}) {}

    void streamWith(const Json& bt, std::uint32_t count) {
        sim::buildScenario(sim, sim::ScenarioKind::Stream, count);
        sim.setBehaviorTree(assets::compileBehaviorJson(bt));
        sim::AgentType type;
        type.name = "probe";
        type.behaviour = bt.at("name").get<std::string>();
        sim.defineAgentType(type);
        const sim::SceneId spawnerId = sim.spawnerIds().front();
        sim::Spawner s = *sim.findSpawner(spawnerId);
        s.agentMix = {{"probe", 1.0f}};
        s.rate = 200.0f;
        sim.updateSpawner(spawnerId, s);
    }

    void run(int steps) {
        for (int i = 0; i < steps; ++i) {
            sim.step();
        }
    }

    std::multiset<std::string> states() const {
        std::multiset<std::string> out;
        const sim::AgentPool& a = sim.agents();
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            out.insert(sim.agentDebug(a.id[i])->stateName);
        }
        return out;
    }
};

}  // namespace

TEST_CASE("Behaviour tree compiler rejects malformed trees with the node path") {
    const auto compile = [](const Json& doc) { (void)assets::compileBehaviorJson(doc); };
    CHECK_THROWS_WITH_AS(compile(tree("t", leaf("Teleport"))), doctest::Contains("Teleport"), std::invalid_argument);
    // Decorators take exactly one child, composites at least one, leaves none.
    CHECK_THROWS_AS(compile(tree("t", composite("Inverter", {leaf("Despawn"), leaf("Despawn")}))),
                    std::invalid_argument);
    CHECK_THROWS_AS(compile(tree("t", composite("Sequence", Json::array()))), std::invalid_argument);
    CHECK_THROWS_AS(compile(tree("t", composite("Wait", {leaf("Despawn")}))), std::invalid_argument);
    // Parameter of the wrong kind.
    CHECK_THROWS_AS(compile(tree("t", leaf("Wait", {{"seconds", "soon"}}))), std::invalid_argument);
    // Missing name / root.
    CHECK_THROWS_AS(compile(Json{{"format", "crowdsim.bt"}, {"root", leaf("Despawn")}}), std::invalid_argument);
    CHECK_THROWS_AS(compile(Json{{"name", "t"}}), std::invalid_argument);
    // Size limit.
    Json many = Json::array();
    for (int i = 0; i < 80; ++i) {
        many.push_back(leaf("SetState", {{"name", "s"}}));
    }
    CHECK_THROWS_AS(compile(tree("t", composite("Sequence", many))), std::invalid_argument);
}

TEST_CASE("Behaviour tree JSON survives compile -> decompile -> JSON") {
    const Json doc =
        tree("roundtrip", composite("Sequence", {leaf("SetState", {{"name", "a"}}),
                                                 composite("Repeat",
                                                           {composite("Sequence", {leaf("Wander", {{"radius", 5}}),
                                                                                   leaf("Wait", {{"seconds", 1.5}})})},
                                                           {{"count", 2}}),
                                                 leaf("Despawn")}));
    const sim::CompiledTree compiled = assets::compileBehaviorJson(doc);
    CHECK(compiled.nodes.size() == 7);
    const Json back = assets::behaviorToJson(compiled);
    const sim::CompiledTree again = assets::compileBehaviorJson(back);
    CHECK(again.nodes.size() == compiled.nodes.size());
    CHECK(again.nodeTypes == compiled.nodeTypes);
    CHECK(assets::behaviorToJson(again) == back);
}

TEST_CASE("Every script, behaviour and agent type in content/ loads") {
    // The engine loads content/scripts first; trees may use the Lua nodes they define.
    app::AppConfig config;
    config.log.console = false;
    config.log.level = spdlog::level::warn;
    config.agents = 10;
    app::EngineContext engine(config);
    CHECK(engine.execute(commands::makeCommand("ScriptErrors")).result["errors"].empty());
    CHECK(engine.simulation().findBehaviorTree("tourist") != nullptr);

    const std::filesystem::path root = CROWDSIM_CONTENT_DIR;
    int trees = 0;
    for (const auto& e : std::filesystem::directory_iterator(root / "behaviours")) {
        std::ifstream in(e.path());
        CHECK_NOTHROW(
            (void)assets::compileBehaviorJson(Json::parse(in), engine.simulation().customNodeSpecs()));
        ++trees;
    }
    int types = 0;
    for (const auto& e : std::filesystem::directory_iterator(root / "agents")) {
        std::ifstream in(e.path());
        CHECK_NOTHROW((void)assets::agentTypeFromJson(Json::parse(in)));
        ++types;
    }
    CHECK(trees >= 5);
    CHECK(types >= 5);
}

TEST_CASE("Selector, Inverter and RandomChance pick the expected branch") {
    Fixture f;
    // RandomChance 0 always fails, so the Selector falls through to the second branch;
    // Inverter(Succeeder) fails too.
    f.streamWith(
        tree("pick",
             composite(
                 "Selector",
                 {composite("Sequence",
                            {leaf("RandomChance", {{"probability", 0}}), leaf("SetState", {{"name", "wrong"}})}),
                  composite("Inverter", {composite("Succeeder", {leaf("SetState", {{"name", "skipped"}})})}),
                  composite("Sequence", {leaf("SetState", {{"name", "right"}}), leaf("Wait", {{"seconds", 1000}})})})),
        40);
    f.run(90);
    const auto states = f.states();
    REQUIRE(!states.empty());
    CHECK(states.count("right") == states.size());
}

TEST_CASE("Wait holds agents in place, then Despawn removes them") {
    Fixture f;
    f.streamWith(tree("pause", composite("Sequence", {leaf("SetState", {{"name", "waiting"}}),
                                                      leaf("Wait", {{"seconds", 2}}), leaf("Despawn")})),
                 30);
    f.run(15);  // 0.5 s: everyone spawned so far is waiting and has not moved
    REQUIRE(f.sim.agents().size() > 0);
    CHECK(f.states().count("waiting") == f.sim.agents().size());
    f.run(150);  // 5.5 s: all spawned (30 at 200/s) and all waits expired
    CHECK(f.sim.stats().spawned == 30);
    CHECK(f.sim.agents().size() == 0);
    CHECK(f.sim.stats().despawned == 30);
}

TEST_CASE("Hot reload restarts running agents on the new tree") {
    Fixture f;
    f.streamWith(
        tree("loop", composite("Sequence", {leaf("SetState", {{"name", "old"}}), leaf("Wait", {{"seconds", 1000}})})),
        50);
    f.run(60);
    REQUIRE(f.sim.agents().size() == 50);
    CHECK(f.states().count("old") == 50);
    const std::uint32_t before = f.sim.findBehaviorTree("loop")->generation;

    f.sim.setBehaviorTree(assets::compileBehaviorJson(
        tree("loop", composite("Sequence", {leaf("SetState", {{"name", "new"}}), leaf("Wait", {{"seconds", 1000}})}))));
    CHECK(f.sim.findBehaviorTree("loop")->generation == before + 1);
    f.run(10);  // > one decision period (3 steps at 10 Hz decisions / 30 Hz sim)
    CHECK(f.states().count("new") == 50);
}

TEST_CASE("Plaza: commuters, wanderers, queuers and groups coexist and all leave") {
    Fixture f(6, 7);
    sim::buildScenario(f.sim, sim::ScenarioKind::Plaza, 800);
    std::set<std::string> typesSeen;
    std::set<std::string> statesSeen;
    std::map<std::uint32_t, std::uint32_t> maxServed;  // goal index -> agents being served at once
    for (int step = 0; step < 18000 && (step < 100 || f.sim.agents().size() > 0); ++step) {
        f.sim.step();
        if (step % 30 != 0) {
            continue;
        }
        const sim::AgentPool& a = f.sim.agents();
        std::map<std::uint32_t, std::uint32_t> served;
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            const auto d = f.sim.agentDebug(a.id[i]);
            typesSeen.insert(d->typeName);
            statesSeen.insert(d->stateName);
            if (a.queueStage[i] == 2) {
                ++served[a.queueGoal[i]];
            }
        }
        for (const auto& [goal, n] : served) {
            maxServed[goal] = std::max(maxServed[goal], n);
        }
    }
    CHECK(typesSeen.count("commuter") == 1);
    CHECK(typesSeen.count("wanderer") == 1);
    CHECK(typesSeen.count("queuer") == 1);
    CHECK(typesSeen.count("follower") == 1);
    CHECK(statesSeen.count("queueing") == 1);
    CHECK(statesSeen.count("wandering") == 1);
    CHECK(statesSeen.count("following") == 1);
    CHECK(!maxServed.empty());
    for (const auto& [goal, n] : maxServed) {
        CHECK(n <= 4);  // kiosk capacity
    }
    CHECK(f.sim.stats().spawned == 800);
    CHECK(f.sim.agents().size() == 0);
}

TEST_CASE("Plaza is deterministic and independent of the thread count") {
    auto hash = [](std::uint32_t threads) {
        Fixture f(threads, 5);
        sim::buildScenario(f.sim, sim::ScenarioKind::Plaza, 1200);
        f.run(900);
        return f.sim.stateHash();
    };
    const std::uint64_t one = hash(1);
    CHECK(one == hash(1));
    CHECK(one == hash(8));
}

TEST_CASE("ContentWatcher reports new and modified files once") {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "crowdsim_watch_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "behaviours");
    std::filesystem::create_directories(root / "agents");
    const auto write = [&](const std::filesystem::path& file, const Json& doc) { std::ofstream(file) << doc.dump(); };
    write(root / "behaviours" / "a.bt.json", tree("a", leaf("Despawn")));
    write(root / "agents" / "a.agent.json", Json{{"format", "crowdsim.agent"}, {"name", "a"}, {"behaviour", "a"}});
    write(root / "behaviours" / "notes.txt", Json::object());

    app::ContentWatcher watcher(root);
    auto changes = watcher.poll(true);
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].command["cmd"] == "SetBehaviorTree");  // trees before types
    CHECK(changes[1].command["cmd"] == "DefineAgentType");
    CHECK(watcher.poll(true).empty());

    write(root / "behaviours" / "a.bt.json", tree("a", leaf("Wait", {{"seconds", 1}})));
    std::filesystem::last_write_time(root / "behaviours" / "a.bt.json",
                                     std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    changes = watcher.poll(true);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].command["params"]["tree"]["root"]["type"] == "Wait");

    std::ofstream(root / "behaviours" / "broken.bt.json") << "{ nope";
    changes = watcher.poll(true);
    REQUIRE(changes.size() == 1);
    CHECK(!changes[0].error.empty());
    std::filesystem::remove_all(root);
}
