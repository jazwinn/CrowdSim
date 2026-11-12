// M6: Lua scripting — sandbox, limits, error handling, script BT nodes, scripted
// entity types, hot reload, determinism and replay.

#include <doctest/doctest.h>

#include <string>

#include "EngineContext.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"

using namespace crowdsim;
using commands::Json;
using commands::makeCommand;

namespace {

app::AppConfig quietConfig(std::uint32_t threads = 4, std::uint32_t agents = 300) {
    app::AppConfig c;
    c.log.console = false;
    c.log.level = spdlog::level::warn;
    c.threads = threads;
    c.scenario = sim::ScenarioKind::Stream;
    c.agents = agents;
    c.loadContent = false;  // nothing from content/: everything below is defined at runtime
    return c;
}

commands::CommandOutcome lua(app::EngineContext& e, const std::string& source) {
    return e.execute(makeCommand("EvalLua", {{"source", source}}));
}

Json luaValue(app::EngineContext& e, const std::string& source) {
    const commands::CommandOutcome o = lua(e, source);
    REQUIRE_MESSAGE(o.ok, o.error);
    return o.result["value"];
}

void run(app::EngineContext& e, int steps) {
    e.execute(makeCommand("Step", {{"steps", steps}}));
    e.runPendingSteps(static_cast<std::uint64_t>(steps));
}

// A Lua node, an entity type and the tree/type that use them — all at runtime.
const char* kLingerScript = R"lua(
lingered = 0
beacon_ticks = 0
entities.register_type{
  name = "beacon", type = "box", size = {1, 2, 1}, color = "#ff8800", tags = {"beacon"}, tick_hz = 2,
  on_tick = function(e, dt) beacon_ticks = beacon_ticks + 1 end,
}
local waiting = {}
bt.register_node{
  name = "Linger", kind = "action",
  params = { seconds = { type = "number", default = 1.5 } },
  tick = function(ctx, agents)
    local out = {}
    for k, a in ipairs(agents) do
      local id = ctx:id(a)
      if not waiting[id] then
        waiting[id] = ctx:time() + ctx.params.seconds
        ctx:stop(a)
        ctx:set_state(a, "lingering")
        out[k] = "running"
      elseif ctx:time() >= waiting[id] then
        waiting[id] = nil
        lingered = lingered + 1
        out[k] = "success"
      else
        out[k] = "running"
      end
    end
    return out
  end,
}
)lua";

const char* kUseScript = R"lua(
bt.set{ name = "lingerer", root = { type = "Sequence", children = {
  { type = "SetState", params = { name = "walking" } },
  { type = "Linger", params = { seconds = 2 } },
  { type = "MoveToGoal" },
  { type = "Despawn" } } } }
agents.register_type{ name = "lingerer", behaviour = "lingerer", color = "#ff00ff" }
local spawner = scene.find("Entrance")
scene.update(spawner, { agentMix = { { type = "lingerer", weight = 1 } } })
return entities.spawn("beacon", { position = { 0, 0, 30 } })
)lua";

}  // namespace

TEST_CASE("Lua sandbox: no os/io/debug/ffi/jit/require/dofile/loadfile/string.dump, text-only load") {
    app::EngineContext e(quietConfig());
    const Json v = luaValue(e,
                            "return os == nil, io == nil, debug == nil, ffi == nil, jit == nil, require == nil, "
                            "dofile == nil, loadfile == nil, string.dump == nil, package == nil");
    REQUIRE(v.is_array());
    for (const Json& b : v) {
        CHECK(b == true);
    }
    const Json bytecode = luaValue(e, "return load(string.char(27) .. 'LJ')");
    CHECK(bytecode[0].is_null());
    CHECK(bytecode[1].get<std::string>().find("binary") != std::string::npos);
    CHECK(luaValue(e, "return load('return 6 * 7')()") == 42);
    // math.random is world-seeded: the same seed gives the same sequence.
    app::EngineContext e2(quietConfig());
    CHECK(luaValue(e, "return math.random()") == luaValue(e2, "return math.random()"));
}

TEST_CASE("Lua limits: runaway loops and memory bombs are stopped, errors are reported, the host survives") {
    app::EngineContext e(quietConfig());
    commands::CommandOutcome o = lua(e, "while true do end");
    CHECK_FALSE(o.ok);
    CHECK(o.error.find("limit exceeded") != std::string::npos);
    o = lua(e, "local s = string.rep('x', 1024 * 1024 * 1024); return #s");
    CHECK_FALSE(o.ok);
    CHECK(o.error.find("memory") != std::string::npos);
    o = lua(e, "error('boom')");
    CHECK_FALSE(o.ok);
    CHECK(o.error.find("boom") != std::string::npos);
    o = lua(e, "scene.command('NoSuchCommand', {})");
    CHECK(o.error.find("unknown command") != std::string::npos);
    const commands::CommandOutcome errors = e.execute(makeCommand("ScriptErrors"));
    CHECK(errors.result["errors"].size() >= 4);
    CHECK(luaValue(e, "return 1 + 1") == 2);  // still usable
}

TEST_CASE("M6: a BT node and an object type defined in Lua at runtime drive a running simulation") {
    app::EngineContext e(quietConfig());
    run(e, 60);  // the simulation is already running with the built-in commuters
    REQUIRE(e.simulation().agents().size() > 0);

    commands::CommandOutcome o = e.execute(makeCommand("LoadScript", {{"name", "linger"}, {"source", kLingerScript}}));
    REQUIRE_MESSAGE(o.ok, o.error);
    o = lua(e, kUseScript);
    REQUIRE_MESSAGE(o.ok, o.error);
    const auto beacon = o.result["value"].get<std::uint32_t>();
    const sim::StaticObject* object = e.simulation().findStaticObject(beacon);
    REQUIRE(object != nullptr);
    CHECK(std::find(object->tags.begin(), object->tags.end(), "type:beacon") != object->tags.end());

    // New agents run the Lua node: they linger (state set from Lua), then walk on.
    bool sawLingering = false;
    for (int i = 0; i < 40 && !sawLingering; ++i) {
        run(e, 15);
        for (const std::string& s : e.simulation().stateNames()) {
            sawLingering = sawLingering || s == "lingering";
        }
    }
    CHECK(sawLingering);
    run(e, 900);
    CHECK(luaValue(e, "return lingered").get<int>() > 20);
    CHECK(luaValue(e, "return beacon_ticks").get<int>() > 10);  // entity on_tick at 2 Hz
    CHECK(e.execute(makeCommand("ScriptErrors")).result["errors"].empty());
}

TEST_CASE("Hot reload replaces a Lua node's behaviour for running agents") {
    app::EngineContext e(quietConfig());
    REQUIRE(e.execute(makeCommand("LoadScript", {{"name", "linger"}, {"source", kLingerScript}})).ok);
    REQUIRE(lua(e, kUseScript).ok);
    run(e, 300);
    CHECK(luaValue(e, "return lingered").get<int>() > 0);
    // New version: the node fails immediately; agents still finish their tree (Sequence fails, restarts...)
    // but nobody lingers any more.
    const std::string v2 = std::string(kLingerScript) + R"lua(
bt.register_node{ name = "Linger", kind = "action", params = { seconds = { type = "number", default = 1 } },
  tick = function(ctx, agents) local out = {} for k, a in ipairs(agents) do ctx:set_state(a, "reloaded") out[k] = true end return out end }
)lua";
    REQUIRE(e.execute(makeCommand("LoadScript", {{"name", "linger"}, {"source", v2}})).ok);
    run(e, 60);
    bool sawReloaded = false;
    for (const std::string& s : e.simulation().stateNames()) {
        sawReloaded = sawReloaded || s == "reloaded";
    }
    CHECK(sawReloaded);
}

TEST_CASE("Lua nodes are deterministic across thread counts and replay identically") {
    auto runOnce = [](std::uint32_t threads, Json* logOut) {
        app::EngineContext e(quietConfig(threads, 600));
        REQUIRE(e.execute(makeCommand("LoadScript", {{"name", "linger"}, {"source", kLingerScript}})).ok);
        REQUIRE(lua(e, kUseScript).ok);
        run(e, 600);
        if (logOut != nullptr) {
            *logOut = e.dispatcher().logToJson(e.commandContext());
        }
        return e.simulation().stateHash();
    };
    Json log;
    const std::uint64_t one = runOnce(1, &log);
    CHECK(one == runOnce(7, nullptr));

    // Replay in a fresh engine: the log carries the script sources.
    app::EngineContext fresh(quietConfig(3, 600));
    commands::replayLog(log, fresh.dispatcher(), fresh.commandContext());
    CHECK(fresh.simulation().stateHash() == one);
}
