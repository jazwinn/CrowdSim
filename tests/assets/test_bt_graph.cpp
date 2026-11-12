// M7: behaviour-tree graph model behind the node editor.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "EngineContext.hpp"
#include "crowdsim/assets/BehaviorJson.hpp"
#include "crowdsim/assets/BtGraph.hpp"

using namespace crowdsim;
using assets::BtGraph;
using commands::makeCommand;
using Json = nlohmann::json;

namespace {

/// What a user does in the editor: add nodes at positions, link them, edit parameters.
BtGraph buildInEditor() {
    BtGraph g;
    g.name = "edited_patrol";
    const int root = g.addNode("Sequence", 0, 0);
    const int label = g.addNode("SetState", 260, -200);
    const int pause = g.addNode("Wait", 260, -50);
    const int walk = g.addNode("MoveToGoal", 260, 100);
    const int leave = g.addNode("Despawn", 260, 250);
    for (int child : {label, pause, walk, leave}) {
        REQUIRE(g.link(root, child, {}) != 0);
    }
    g.findNode(label)->params["name"] = "edited";
    g.findNode(pause)->params["seconds"] = 1.5;
    return g;
}

}  // namespace

TEST_CASE("BtGraph linking rules: leaves, decorator capacity, single parent, no cycles") {
    BtGraph g;
    const int seq = g.addNode("Sequence", 0, 0);
    const int inv = g.addNode("Inverter", 200, 0);
    const int a = g.addNode("Wait", 400, 0);
    const int b = g.addNode("Despawn", 400, 100);
    std::string why;
    CHECK_FALSE(g.canLink(a, b, {}, &why));  // Wait is a leaf
    CHECK(why.find("no children") != std::string::npos);
    CHECK(g.link(seq, inv, {}) != 0);
    CHECK(g.link(inv, a, {}) != 0);
    CHECK_FALSE(g.canLink(inv, b, {}, &why));    // decorators take one child
    CHECK_FALSE(g.canLink(seq, a, {}, &why));    // already has a parent
    CHECK_FALSE(g.canLink(inv, seq, {}, &why));  // cycle
    CHECK(g.link(seq, b, {}) != 0);
    CHECK(g.roots() == std::vector<int>{seq});
    CHECK(g.validate({}).empty());

    // Problems are reported per node.
    g.findNode(a)->params["seconds"] = "soon";
    g.findNode(b)->params["bogus"] = 1;
    const int lonely = g.addNode("Selector", 0, 300);
    const auto issues = g.validate({});
    auto has = [&](int node, const char* text) {
        return std::any_of(issues.begin(), issues.end(), [&](const assets::BtGraphIssue& i) {
            return i.node == node && i.message.find(text) != std::string::npos;
        });
    };
    CHECK(has(a, "must be a number"));
    CHECK(has(b, "unknown parameter"));
    CHECK(has(lonely, "at least 1"));
    CHECK(has(0, "unconnected"));
    CHECK_THROWS_AS((void)g.toJson(), std::invalid_argument);
}

TEST_CASE("BtGraph orders children top to bottom and lays out loaded trees in JSON order") {
    BtGraph g = buildInEditor();
    Json doc = g.toJson();
    const Json& kids = doc["root"]["children"];
    REQUIRE(kids.size() == 4);
    CHECK(kids[0]["type"] == "SetState");
    CHECK(kids[3]["type"] == "Despawn");
    // Dragging the Despawn node above the others makes it the first child.
    g.findNode(g.children(g.roots()[0]).back())->y = -500;
    CHECK(g.toJson()["root"]["children"][0]["type"] == "Despawn");

    // A file without an editor block gets an auto layout that keeps its order.
    Json plain = doc;
    plain.erase("editor");
    const BtGraph loaded = BtGraph::fromJson(plain);
    const Json again = loaded.toJson();
    CHECK(again["root"] == doc["root"]);
    CHECK(again.contains("editor"));
}

TEST_CASE("M7: a tree built in the editor drives agents and round-trips to identical JSON") {
    const std::filesystem::path content = std::filesystem::temp_directory_path() / "crowdsim_m7_content";
    std::filesystem::remove_all(content);
    std::filesystem::create_directories(content);

    app::AppConfig config;
    config.log.console = false;
    config.log.level = spdlog::level::warn;
    config.loadContent = false;
    config.contentDir = content.string();
    config.agents = 200;
    app::EngineContext engine(config);

    const Json doc = buildInEditor().toJson();
    // Graph -> JSON -> graph -> JSON is identical (byte for byte).
    CHECK(BtGraph::fromJson(doc).toJson().dump() == doc.dump());

    // Apply (hot reload) and drive agents with it.
    REQUIRE(engine.execute(makeCommand("SetBehaviorTree", {{"tree", doc}})).ok);
    REQUIRE(engine
                .execute(
                    makeCommand("DefineAgentType", {{"type", {{"name", "patroller"}, {"behaviour", "edited_patrol"}}}}))
                .ok);
    const sim::SceneId spawner = engine.simulation().spawnerIds().front();
    REQUIRE(
        engine
            .execute(makeCommand(
                "UpdateEntity", {{"id", spawner}, {"patch", {{"agentMix", {{{"type", "patroller"}, {"weight", 1}}}}}}}))
            .ok);
    engine.execute(makeCommand("Step", {{"steps", 240}}));
    engine.runPendingSteps(240);
    const auto& states = engine.simulation().stateNames();
    CHECK(std::find(states.begin(), states.end(), "edited") != states.end());
    engine.execute(makeCommand("Step", {{"steps", 4000}}));
    engine.runPendingSteps(4000);
    CHECK(engine.simulation().stats().despawned > 50);  // agents finished the edited tree

    // Save to content/behaviours and open it again: the identical document comes back.
    REQUIRE(engine.execute(makeCommand("SaveBehaviorTree", {{"tree", doc}})).ok);
    CHECK(std::filesystem::exists(content / "behaviours" / "edited_patrol.bt.json"));
    const commands::CommandOutcome opened = engine.execute(makeCommand("GetBehaviorTree", {{"name", "edited_patrol"}}));
    REQUIRE(opened.ok);
    CHECK(opened.result["source"] == "file");
    CHECK(opened.result["tree"].dump() == doc.dump());
    CHECK(BtGraph::fromJson(opened.result["tree"]).toJson().dump() == doc.dump());
    std::filesystem::remove_all(content);
}
