// M9: metrics collector (gates, heatmaps, time to goal, stuck detection, recording)
// and the bottleneck done-criterion through MCP.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "EngineContext.hpp"
#include "crowdsim/mcp/CrowdSimTools.hpp"
#include "crowdsim/mcp/McpServer.hpp"

using namespace crowdsim;
using commands::makeCommand;
using Json = nlohmann::json;

namespace {

app::AppConfig quiet(std::uint32_t agents = 300) {
    app::AppConfig c;
    c.log.console = false;
    c.log.level = spdlog::level::warn;
    c.loadContent = false;
    c.agents = agents;
    return c;
}

void run(app::EngineContext& e, double seconds) {
    e.execute(makeCommand("RunFor", {{"seconds", seconds}}));
    e.runPendingSteps(~0ull);
}

}  // namespace

TEST_CASE("Gates count directional crossings; time to goal matches arrivals; heatmap covers the walk") {
    app::EngineContext e(quiet(300));  // stream: agents walk from z ~ -60 towards +60
    // a -> b along +x: walking +z goes from its right (negative) side to its left = forward.
    REQUIRE(e.execute(makeCommand("AddGate", {{"name", "mid"}, {"a", {-30, 0}}, {"b", {30, 0}}})).ok);
    REQUIRE(e.execute(makeCommand("AddGate", {{"name", "flipped"}, {"a", {30, 10}}, {"b", {-30, 10}}})).ok);
    run(e, 150);
    const Json m = e.execute(makeCommand("GetMetrics")).result;
    const Json& mid = m["gates"][0];
    const Json& flipped = m["gates"][1];
    CHECK(mid["total"].get<int>() >= 290);  // (almost) everyone crossed once
    CHECK(mid["forward"].get<int>() + mid["backward"].get<int>() == mid["total"].get<int>());
    // Reversing a gate's endpoints swaps the directions.
    CHECK(mid["forward"].get<int>() > mid["backward"].get<int>());
    CHECK(flipped["backward"].get<int>() > flipped["forward"].get<int>());
    CHECK(m["timeToGoal"]["count"].get<std::uint64_t>() == e.simulation().stats().arrived);
    CHECK(m["timeToGoal"]["mean"].get<double>() > 20.0);

    const Json h = e.execute(makeCommand("GetHeatmap", {{"kind", "density"}, {"maxCells", 400}})).result;
    CHECK(h["width"].get<int>() * h["height"].get<int>() <= 400);
    CHECK(h["max"].get<double>() > 0.0);
    const Json speed = e.execute(makeCommand("GetHeatmap", {{"kind", "speed"}})).result;
    CHECK(speed["max"].get<double>() > 0.8);  // people walk at ~1.2 m/s
    CHECK(speed["max"].get<double>() < 3.0);

    // Undo of AddGate removes it again.
    REQUIRE(e.execute(makeCommand("RemoveGate", {{"name", "flipped"}})).ok);
    CHECK(e.metrics().gates().size() == 1);
    // Resetting the simulation restarts the metrics.
    e.execute(makeCommand("ResetSimulation"));
    run(e, 1);
    CHECK(e.metrics().gates()[0].forward + e.metrics().gates()[0].backward == 0);
}

TEST_CASE("Removing a gate keeps the other gates' history") {
    app::EngineContext e(quiet(200));
    REQUIRE(e.execute(makeCommand("AddGate", {{"name", "early"}, {"a", {-30, -40}}, {"b", {30, -40}}})).ok);
    REQUIRE(e.execute(makeCommand("AddGate", {{"name", "late"}, {"a", {-30, 0}}, {"b", {30, 0}}})).ok);
    run(e, 60);
    std::vector<std::uint64_t> lateBefore;
    for (const metrics::Sample& s : e.metrics().series()) {
        lateBefore.push_back(s.gateTotals.at(1));
    }
    REQUIRE(lateBefore.back() > 0);
    REQUIRE(e.metrics().series().back().gateTotals[0] != lateBefore.back());  // the two differ

    REQUIRE(e.execute(makeCommand("RemoveGate", {{"name", "early"}})).ok);
    const auto& series = e.metrics().series();
    REQUIRE(series.size() == lateBefore.size());
    for (std::size_t i = 0; i < series.size(); ++i) {
        REQUIRE(series[i].gateTotals.size() == 1);
        CHECK(series[i].gateTotals[0] == lateBefore[i]);
    }
}

TEST_CASE("Agents that cannot reach their goal are reported as stuck") {
    app::EngineContext e(quiet());
    // Two separate floors: the spawner on one, the goal on the other (no path).
    REQUIRE(e.execute(makeCommand("CreateScene", {{"name", "islands"}})).ok);
    for (const double x : {-15.0, 15.0}) {
        REQUIRE(e.execute(makeCommand("PlaceObject", {{"object",
                                                       {{"name", "Floor"},
                                                        {"type", "box"},
                                                        {"position", {x, -0.2, 0}},
                                                        {"size", {10, 0.2, 10}},
                                                        {"tags", {"walkable"}}}}}))
                    .ok);
    }
    REQUIRE(
        e.execute(makeCommand("CreateGoal",
                              {{"goal", {{"name", "Far"}, {"area", {{"center", {15, 0}}, {"halfExtents", {2, 2}}}}}}}))
            .ok);
    REQUIRE(e.execute(makeCommand("CreateSpawner", {{"spawner",
                                                     {{"name", "Here"},
                                                      {"area", {{"center", {-15, 0}}, {"halfExtents", {3, 3}}}},
                                                      {"rate", 20},
                                                      {"total", 20},
                                                      {"goal", "Far"}}}}))
                .ok);
    run(e, 25);
    const Json m = e.execute(makeCommand("GetMetrics")).result;
    CHECK(m["stuck"]["count"].get<int>() >= 10);
    CHECK(e.simulation().stats().arrived == 0);
}

TEST_CASE("M9: a bottleneck run exports trajectories and a density heatmap through MCP") {
    app::EngineContext e(quiet());
    mcp::McpServices services = e.mcpServices();
    mcp::McpServer server("crowdsim", "test", "");
    mcp::registerCrowdSimTools(server, services);
    auto tool = [&](const char* name, Json args) {
        const Json r = server.handleNow(Json{{"jsonrpc", "2.0"},
                                             {"id", 1},
                                             {"method", "tools/call"},
                                             {"params", {{"name", name}, {"arguments", args}}}});
        REQUIRE_MESSAGE(r["result"]["isError"] == false, r.dump());
        return r["result"]["structuredContent"];
    };
    tool("scene_load", {{"path", "scenes/corridor_bottleneck.scene.json"}});
    tool("gate_add", {{"name", "doorway"}, {"a", {0.4, -2.5}}, {"b", {0.4, 2.5}}});
    tool("recording_start", {{"hz", 4}});
    const Json run = tool("sim_run_for", {{"seconds", 60}});
    const Json& gate = run["metrics"]["gates"][0];
    CHECK(gate["total"].get<int>() > 150);
    CHECK(gate["flowPerMetrePerSecond"].get<double>() > 0.8);  // a realistic specific flow
    CHECK(gate["flowPerMetrePerSecond"].get<double>() < 3.0);

    // Density heatmap: the queue in front of the doorway (x < 0) is denser than the far side.
    const Json h = tool("heatmap_get", {{"kind", "density"}, {"maxCells", 600}});
    REQUIRE(h["width"].get<int>() > 0);
    const double cell = h["cellSize"].get<double>();
    const double originX = h["origin"][0].get<double>();
    double upstream = 0.0;
    double downstream = 0.0;
    for (const Json& row : h["values"]) {
        for (std::size_t x = 0; x < row.size(); ++x) {
            const double cx = originX + (static_cast<double>(x) + 0.5) * cell;
            const double v = row[x].get<double>();
            if (cx > -12.0 && cx < -1.0) {
                upstream = std::max(upstream, v);
            } else if (cx > 6.0 && cx < 30.0) {
                downstream = std::max(downstream, v);
            }
        }
    }
    CHECK(upstream > 1.0);
    CHECK(upstream > 2.0 * downstream);

    // Trajectories export as CSV.
    const auto file = std::filesystem::temp_directory_path() / "crowdsim_m9_trajectories.csv";
    std::filesystem::remove(file);
    const Json exported = tool("recording_export", {{"path", file.string()}});
    const auto rows = exported["rows"].get<std::size_t>();
    CHECK(rows > 50000);
    std::ifstream in(file);
    std::string header;
    std::getline(in, header);
    CHECK(header == "agent_id,type,time,x,y,z,vx,vz,state");
    std::size_t lines = 0;
    for (std::string line; std::getline(in, line);) {
        ++lines;
    }
    CHECK(lines == rows);
    in.close();
    std::filesystem::remove(file);
}
