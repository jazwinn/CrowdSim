// M8: MCP server core, tool schemas and the request queue.

#include <doctest/doctest.h>

#include <set>
#include <thread>

#include "EngineContext.hpp"
#include "crowdsim/mcp/CrowdSimTools.hpp"
#include "crowdsim/mcp/McpServer.hpp"

using namespace crowdsim;
using mcp::Json;

namespace {

struct Fixture {
    app::AppConfig config = [] {
        app::AppConfig c;
        c.log.console = false;
        c.log.level = spdlog::level::warn;
        c.loadContent = false;
        c.agents = 50;
        return c;
    }();
    app::EngineContext engine{config};
    mcp::McpServices services = engine.mcpServices();
    mcp::McpServer server{"crowdsim", "test", "instructions"};
    Fixture() { mcp::registerCrowdSimTools(server, services); }

    Json call(const std::string& method, Json params = Json::object(), int id = 1) {
        return server.handleNow(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    }
    Json tool(const std::string& name, Json args = Json::object()) {
        return call("tools/call", Json{{"name", name}, {"arguments", args}})["result"];
    }
};

}  // namespace

TEST_CASE("MCP initialize negotiates the protocol and announces tools and resources") {
    Fixture f;
    const Json r = f.call("initialize", {{"protocolVersion", "2025-03-26"}, {"capabilities", Json::object()}});
    CHECK(r["jsonrpc"] == "2.0");
    CHECK(r["id"] == 1);
    CHECK(r["result"]["protocolVersion"] == "2025-03-26");
    CHECK(r["result"]["serverInfo"]["name"] == "crowdsim");
    CHECK(r["result"]["capabilities"].contains("tools"));
    CHECK(r["result"]["capabilities"].contains("resources"));
    CHECK(f.call("initialize", {{"protocolVersion", "1999-01-01"}})["result"]["protocolVersion"] ==
          mcp::kProtocolVersion);
    // Notifications get no response; unknown methods a JSON-RPC error.
    CHECK(f.server.handleNow(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null());
    CHECK(f.call("no/such/method")["error"]["code"] == -32601);
    CHECK(f.server.handleNow(Json::array())["error"]["code"] == -32600);
    CHECK(f.call("ping")["result"].is_object());
}

TEST_CASE("Every MCP tool has a description and a well-formed input schema") {
    Fixture f;
    const Json list = f.call("tools/list")["result"]["tools"];
    REQUIRE(list.size() >= 35);
    std::set<std::string> names;
    for (const Json& t : list) {
        CAPTURE(t.dump());
        const std::string name = t.at("name").get<std::string>();
        CHECK(names.insert(name).second);
        CHECK(t.at("description").get<std::string>().size() > 10);
        const Json& s = t.at("inputSchema");
        CHECK(s.at("type") == "object");
        CHECK(s.at("properties").is_object());
        for (const Json& r : s.value("required", Json::array())) {
            CHECK(s["properties"].contains(r.get<std::string>()));
        }
    }
    for (const char* required : {"scene_load",        "scene_save",      "scene_describe", "object_place",
                                 "object_update",     "object_delete",   "gltf_import",    "navmesh_rebuild",
                                 "spawner_create",    "spawner_update",  "goal_create",    "goal_update",
                                 "agent_type_define", "agent_type_list", "agents_spawn",   "agents_query",
                                 "bt_list",           "bt_get",          "bt_set",         "bt_validate",
                                 "bt_node_types",     "script_list",     "script_get",     "script_write",
                                 "script_reload",     "script_eval",     "sim_play",       "sim_pause",
                                 "sim_step",          "sim_run_for",     "sim_reset",      "sim_set_params",
                                 "metrics_get",       "log_tail",        "errors_get"}) {
        CHECK_MESSAGE(names.count(required) == 1, required);
    }
}

TEST_CASE("MCP tools report failures as tool errors and run commands") {
    Fixture f;
    CHECK(f.call("tools/call", {{"name", "nope"}})["error"]["code"] == -32602);
    const Json bad = f.tool("object_delete", {{"id", 999999}});
    CHECK(bad["isError"] == true);
    const Json placed = f.tool(
        "object_place",
        {{"object",
          {{"name", "Box"}, {"type", "box"}, {"position", {0, 0, 0}}, {"size", {1, 1, 1}}, {"tags", {"obstacle"}}}}});
    CHECK(placed["isError"] == false);
    CHECK(placed["structuredContent"]["id"].is_number());
    const Json run = f.tool("sim_run_for", {{"seconds", 2}});
    CHECK(run["structuredContent"]["steps"] == 60);
    CHECK(run["structuredContent"]["metrics"]["time"] == 2);
    const Json q = f.tool("agents_query", {{"limit", 3}});
    CHECK(q["structuredContent"]["listed"].get<int>() <= 3);
    const Json eval = f.tool("script_eval", {{"source", "print('hi') return 6 * 7"}});
    CHECK(eval["structuredContent"]["value"] == 42);
}

TEST_CASE("MCP resources list and read the scene, trees and docs") {
    Fixture f;
    const Json list = f.call("resources/list")["result"]["resources"];
    std::set<std::string> uris;
    for (const Json& r : list) {
        uris.insert(r["uri"].get<std::string>());
    }
    CHECK(uris.count("crowdsim://scene") == 1);
    CHECK(uris.count("crowdsim://bt/commuter") == 1);
    CHECK(uris.count("crowdsim://docs/formats.md") == 1);
    const Json scene = f.call("resources/read", {{"uri", "crowdsim://scene"}})["result"]["contents"][0];
    CHECK(Json::parse(scene["text"].get<std::string>())["format"] == "crowdsim.scene");
    CHECK(f.call("resources/read", {{"uri", "crowdsim://docs/../../secret"}})["error"]["code"] == -32002);
}

TEST_CASE("base64 and the cross-thread request queue") {
    CHECK(mcp::base64Encode("") == "");
    CHECK(mcp::base64Encode("f") == "Zg==");
    CHECK(mcp::base64Encode("fo") == "Zm8=");
    CHECK(mcp::base64Encode("foobar") == "Zm9vYmFy");

    Fixture f;
    mcp::McpRequestQueue queue;
    Json reply;
    std::thread client([&] {
        auto taken = queue.submit(Json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}});
        reply = taken.get().get();
    });
    while (queue.process(f.server) == 0) {
        queue.waitForWork(std::chrono::milliseconds(10));
    }
    client.join();
    CHECK(reply["id"] == 7);
    CHECK(reply["result"].is_object());
}
