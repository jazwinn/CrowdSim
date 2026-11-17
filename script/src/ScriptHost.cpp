#include "crowdsim/script/ScriptHost.hpp"

#include <sol/sol.hpp>

extern "C" {
#include <luajit.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

#include "LuaJson.hpp"
#include "crowdsim/assets/JsonTypes.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/core/Random.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::script {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kHookInterval = 1000;      ///< instructions between budget checks
constexpr std::size_t kMaxErrors = 200;  ///< errors kept for the UI / MCP
constexpr char kHostRegistryKey = 'H';   ///< address used as the registry key (immutable)

sim::CustomResult parseResult(const sol::object& o) {
    switch (o.get_type()) {
        case sol::type::boolean:
            return o.as<bool>() ? sim::CustomResult::Success : sim::CustomResult::Failure;
        case sol::type::string: {
            const std::string s = o.as<std::string>();
            if (s == "success" || s == "ok" || s == "done") {
                return sim::CustomResult::Success;
            }
            if (s == "failure" || s == "fail" || s == "failed") {
                return sim::CustomResult::Failure;
            }
            return sim::CustomResult::Running;
        }
        default:
            return sim::CustomResult::Running;
    }
}

sim::BtParamKind kindFromName(const std::string& t) {
    if (t == "number" || t == "float" || t == "int") {
        return sim::BtParamKind::Number;
    }
    if (t == "string" || t == "text") {
        return sim::BtParamKind::Text;
    }
    if (t == "vec3" || t == "point") {
        return sim::BtParamKind::Vec3;
    }
    throw std::invalid_argument("parameter type must be number, string or vec3, got '" + t + "'");
}

}  // namespace

/// Agent access handed to script node ticks: `ctx:position(a)`, `ctx:move_to(a, x, y, z)`, ...
/// `a` is an agent handle from the `agents` array (an agent pool index, valid for this call).
class NodeContext {
public:
    NodeContext(sim::Simulation& sim, sol::table nodeParams) : params(std::move(nodeParams)), sim_(sim) {}

    sol::table params;

    [[nodiscard]] std::tuple<float, float, float> position(std::uint32_t a) const {
        check(a);
        const auto& p = sim_.agents();
        return {p.position[a].x, p.height[a], p.position[a].y};
    }
    [[nodiscard]] std::tuple<float, float> velocity(std::uint32_t a) const {
        check(a);
        const glm::vec2 v = sim_.agents().velocity[a];
        return {v.x, v.y};
    }
    [[nodiscard]] float speed(std::uint32_t a) const {
        check(a);
        const glm::vec2 v = sim_.agents().velocity[a];
        return std::sqrt(v.x * v.x + v.y * v.y);
    }
    [[nodiscard]] std::uint32_t id(std::uint32_t a) const {
        check(a);
        return sim_.agents().id[a];
    }
    [[nodiscard]] std::string state(std::uint32_t a) const {
        check(a);
        const auto s = sim_.agents().stateId[a];
        return s < sim_.stateNames().size() ? sim_.stateNames()[s] : std::string();
    }
    [[nodiscard]] std::string type(std::uint32_t a) const {
        check(a);
        const auto t = sim_.agents().agentType[a];
        return t < sim_.agentTypes().size() ? sim_.agentTypes()[t].name : std::string();
    }
    [[nodiscard]] float random(std::uint32_t a) {
        check(a);
        return sim_.scriptRandom(a);
    }
    [[nodiscard]] double time() const { return sim_.simTimeSeconds(); }
    [[nodiscard]] std::uint32_t countNear(std::uint32_t a, float radius) const {
        const auto [x, y, z] = position(a);
        return sim_.countAgentsNear({x, y, z}, radius);
    }
    void moveTo(std::uint32_t a, float x, float y, float z) {
        check(a);
        sim_.scriptMoveTo(a, {x, y, z});
    }
    void moveToGoal(std::uint32_t a) {
        check(a);
        sim_.scriptMoveToGoal(a);
    }
    void stop(std::uint32_t a) {
        check(a);
        sim_.scriptStop(a);
    }
    void setSpeed(std::uint32_t a, float multiplier) {
        check(a);
        sim_.scriptSetSpeed(a, multiplier);
    }
    void setState(std::uint32_t a, const std::string& s) {
        check(a);
        sim_.scriptSetState(a, s);
    }

private:
    void check(std::uint32_t a) const {
        if (a >= sim_.agents().size()) {
            throw std::out_of_range("invalid agent handle " + std::to_string(a));
        }
    }
    sim::Simulation& sim_;
};

struct EntityType {
    std::string name;
    std::string script;
    Json object;  ///< PlaceObject template (name/type/size/color/tags)
    double tickHz = 2.0;
    sol::protected_function onSpawn;
    sol::protected_function onTick;
};

/// Counting allocator: refuses allocations beyond the memory budget, so even a
/// single huge allocation (string.rep) fails with "not enough memory".
struct MemoryBudget {
    std::size_t used = 0;
    std::size_t limit = 0;

    static void* allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
        auto* m = static_cast<MemoryBudget*>(ud);
        const std::size_t old = ptr != nullptr ? osize : 0;
        if (nsize == 0) {
            m->used -= old;
            std::free(ptr);
            return nullptr;
        }
        if (m->limit != 0 && nsize > old && m->used - old + nsize > m->limit) {
            return nullptr;
        }
        void* p = std::realloc(ptr, nsize);
        if (p != nullptr) {
            m->used = m->used - old + nsize;
        }
        return p;
    }
};

/// A state using the counting allocator when this LuaJIT build supports custom
/// allocators (GC64), else the default one (the hook then enforces the limit).
sol::state makeState(MemoryBudget& memory) {
    if (lua_State* probe = lua_newstate(&MemoryBudget::allocate, &memory)) {
        lua_close(probe);
        memory.used = 0;
        return sol::state(sol::default_at_panic, &MemoryBudget::allocate, &memory);
    }
    return sol::state();
}

struct ScriptHost::Impl {
    // The allocator budget and the Lua state come first: every Lua reference below
    // is released before the state, and the state before its allocator.
    MemoryBudget memory;
    sol::state lua;

    commands::CommandRegistry& registry;
    commands::CommandContext& ctx;
    spdlog::logger& log;
    ScriptLimits limits;

    // Budget of the outermost call into Lua (re-entrant calls share it).
    int depth = 0;
    std::uint64_t instructions = 0;
    Clock::time_point deadline{};
    std::string current = "<host>";

    std::string printBuffer;
    std::vector<ScriptInfo> scripts;
    std::deque<ScriptError> errors;
    std::map<std::string, std::string> lastErrorBy;  ///< de-duplicates repeated callback errors

    std::map<std::string, sol::protected_function> nodeTicks;  ///< by node type name
    std::map<std::string, EntityType> entityTypes;
    struct Handler {
        std::string owner;  ///< script that registered it (replaced when that script reloads)
        sol::protected_function fn;
    };
    std::map<std::string, std::vector<Handler>> handlers;  ///< events.on
    core::Pcg32 rng;

    Impl(commands::CommandRegistry& r, commands::CommandContext& c, spdlog::logger& l, ScriptLimits lim)
        : memory{0, lim.memoryBytes},
          lua(makeState(memory)),
          registry(r),
          ctx(c),
          log(l),
          limits(lim),
          rng(c.sim.config().seed ^ 0x5C21u) {}

    /// World-seeded uniform [0, 1) for math.random / sim.random.
    double uniform() { return static_cast<double>(rng.next()) * (1.0 / 4294967296.0); }

    // ------------------------------------------------------------------ budget + calls

    static void hook(lua_State* L, lua_Debug* /*ar*/) {
        lua_pushlightuserdata(L, const_cast<char*>(&kHostRegistryKey));
        lua_rawget(L, LUA_REGISTRYINDEX);
        auto* self = static_cast<Impl*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        if (self == nullptr || self->depth == 0) {
            return;
        }
        self->instructions += kHookInterval;
        if (self->instructions > self->limits.instructionsPerCall) {
            luaL_error(L, "instruction limit exceeded (%d million)",
                       static_cast<int>(self->limits.instructionsPerCall / 1'000'000));
        }
        if (Clock::now() > self->deadline) {
            luaL_error(L, "time limit exceeded (%d ms)", static_cast<int>(self->limits.secondsPerCall * 1000.0));
        }
        const auto kb = static_cast<std::size_t>(lua_gc(L, LUA_GCCOUNT, 0));
        if (kb * 1024u > self->limits.memoryBytes) {
            lua_gc(L, LUA_GCCOLLECT, 0);
            if (static_cast<std::size_t>(lua_gc(L, LUA_GCCOUNT, 0)) * 1024u > self->limits.memoryBytes) {
                luaL_error(L, "memory limit exceeded (%d MB)", static_cast<int>(self->limits.memoryBytes >> 20));
            }
        }
    }

    struct CallScope {
        Impl& impl;
        explicit CallScope(Impl& i) : impl(i) {
            if (impl.depth++ == 0) {
                impl.instructions = 0;
                impl.deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                   std::chrono::duration<double>(impl.limits.secondsPerCall));
            }
        }
        ~CallScope() { --impl.depth; }
        CallScope(const CallScope&) = delete;
        CallScope& operator=(const CallScope&) = delete;
    };

    template <typename... Args>
    sol::protected_function_result call(const sol::protected_function& f, Args&&... args) {
        CallScope scope(*this);
        return f(std::forward<Args>(args)...);
    }

    void recordError(const std::string& where, const std::string& message, bool dedupe) {
        if (dedupe) {
            auto& last = lastErrorBy[where];
            if (last == message) {
                return;
            }
            last = message;
        }
        log.error("Lua error in {}: {}", where, message);
        errors.push_back({where, message, ctx.sim.stepIndex()});
        while (errors.size() > kMaxErrors) {
            errors.pop_front();
        }
    }

    // ------------------------------------------------------------------ commands

    Json command(const std::string& name, const Json& params) {
        const commands::CommandSpec* spec = registry.find(name);
        if (spec == nullptr) {
            throw std::invalid_argument("unknown command '" + name + "'");
        }
        return spec->apply(params.is_null() ? Json::object() : params, ctx).result;
    }

    sol::object commandLua(const std::string& name, sol::object params) {
        return jsonToLua(lua, command(name, luaToJson(params)));
    }

    std::uint32_t idFrom(const Json& result) const {
        return result.contains("id") ? result["id"].get<std::uint32_t>() : 0u;
    }

    // ------------------------------------------------------------------ entities

    sol::object entityTable(std::uint32_t id) {
        Json e = assets::entityToJson(ctx.sim, id);
        if (e.is_null()) {
            return sol::make_object(lua, sol::lua_nil);
        }
        for (const Json& tag : e.value("tags", Json::array())) {
            const std::string t = tag.get<std::string>();
            if (t.rfind("type:", 0) == 0) {
                e["type"] = t.substr(5);
            }
        }
        return jsonToLua(lua, e);
    }

    std::vector<std::uint32_t> entitiesOfType(const std::string& type) const {
        std::vector<std::uint32_t> out;
        const std::string tag = "type:" + type;
        for (sim::SceneId id : ctx.sim.staticObjectIds()) {
            const sim::StaticObject* o = ctx.sim.findStaticObject(id);
            if (o != nullptr && (type.empty() || std::find(o->tags.begin(), o->tags.end(), tag) != o->tags.end())) {
                out.push_back(id);
            }
        }
        return out;
    }

    void registerEntityType(const sol::table& def) {
        EntityType t;
        t.name = def.get_or<std::string>("name", "");
        if (t.name.empty() || t.name.find(':') != std::string::npos) {
            throw std::invalid_argument("entities.register_type needs a name without ':'");
        }
        t.script = current;
        const sol::object object = def.get<sol::object>("object");
        t.object = object.valid() && object.get_type() == sol::type::table ? luaToJson(object) : Json::object();
        // Shorthand fields on the definition itself.
        for (const char* key : {"type", "size", "color", "steps", "scale", "asset"}) {
            const sol::object v = def.get<sol::object>(key);
            if (v.valid() && v.get_type() != sol::type::lua_nil) {
                t.object[key == std::string("type") ? "type" : key] = luaToJson(v);
            }
        }
        if (!t.object.contains("type")) {
            t.object["type"] = "box";
        }
        Json tags = t.object.value("tags", Json::array());
        const sol::object extraTags = def.get<sol::object>("tags");
        if (extraTags.valid() && extraTags.get_type() == sol::type::table) {
            for (const Json& tag : luaToJson(extraTags)) {
                tags.push_back(tag);
            }
        }
        if (std::find(tags.begin(), tags.end(), Json("obstacle")) == tags.end() &&
            std::find(tags.begin(), tags.end(), Json("walkable")) == tags.end()) {
            tags.push_back("obstacle");
        }
        tags.push_back("type:" + t.name);
        t.object["tags"] = tags;
        t.tickHz = std::clamp(def.get_or("tick_hz", 2.0), 0.1, 30.0);
        t.onSpawn = def.get_or<sol::protected_function>("on_spawn", sol::lua_nil);
        t.onTick = def.get_or<sol::protected_function>("on_tick", sol::lua_nil);
        log.info("Lua: entity type '{}' registered by {}", t.name, current);
        entityTypes[t.name] = std::move(t);
    }

    // ------------------------------------------------------------------ BT nodes

    void registerNode(const sol::table& def) {
        sim::CustomNodeSpec spec;
        spec.name = def.get_or<std::string>("name", "");
        if (spec.name.empty() || sim::findBtNodeType(spec.name) != nullptr) {
            throw std::invalid_argument("bt.register_node needs a name that is not a built-in node type");
        }
        const std::string kind = def.get_or<std::string>("kind", "action");
        if (kind != "action" && kind != "condition") {
            throw std::invalid_argument("bt.register_node kind must be \"action\" or \"condition\"");
        }
        spec.category = kind == "condition" ? sim::BtCategory::Condition : sim::BtCategory::Action;
        spec.description = def.get_or<std::string>("description", "");
        const sol::protected_function tick = def.get_or<sol::protected_function>("tick", sol::lua_nil);
        if (!tick.valid() || tick.get_type() != sol::type::function) {
            throw std::invalid_argument("bt.register_node needs a tick = function(ctx, agents) ... end");
        }
        // params: { name = {type=, default=, description=} } (sorted by name) or an array of {name=, ...}.
        const sol::object params = def.get<sol::object>("params");
        if (params.valid() && params.get_type() == sol::type::table) {
            const Json p = luaToJson(params);
            auto add = [&](const std::string& name, const Json& d) {
                sim::CustomParamSpec ps;
                ps.name = name;
                ps.kind = kindFromName(d.value("type", std::string("number")));
                ps.description = d.value("description", std::string());
                if (d.contains("default")) {
                    const Json& v = d["default"];
                    if (ps.kind == sim::BtParamKind::Number && v.is_number()) {
                        ps.defaultNumber = v.get<float>();
                    } else if (ps.kind == sim::BtParamKind::Text && v.is_string()) {
                        ps.defaultText = v.get<std::string>();
                    } else if (ps.kind == sim::BtParamKind::Vec3 && v.is_array() && v.size() == 3) {
                        ps.defaultVec = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
                    } else {
                        throw std::invalid_argument("default of parameter '" + name + "' has the wrong type");
                    }
                }
                spec.params.push_back(std::move(ps));
            };
            if (p.is_array()) {
                for (const Json& d : p) {
                    add(d.at("name").get<std::string>(), d);
                }
            } else {
                for (auto it = p.begin(); it != p.end(); ++it) {
                    add(it.key(), it.value());
                }
            }
        }
        ctx.sim.defineCustomNode(spec);
        nodeTicks[spec.name] = tick;
        log.info("Lua: BT node '{}' ({}, {} params) registered by {}", spec.name, kind, spec.params.size(), current);
    }

    sol::table paramsTable(const sim::CompiledTree& tree, std::uint16_t node) {
        sol::table t = lua.create_table();
        const sim::BtNode& n = tree.nodes[node];
        if (n.customParams < tree.customParams.size()) {
            for (const auto& [key, v] : tree.customParams[n.customParams]) {
                switch (v.kind) {
                    case sim::BtParamKind::Number:
                        t[key] = v.number;
                        break;
                    case sim::BtParamKind::Text:
                        t[key] = v.text;
                        break;
                    case sim::BtParamKind::Vec3:
                        t[key] = lua.create_table_with(1, v.vec.x, 2, v.vec.y, 3, v.vec.z);
                        break;
                }
            }
        }
        return t;
    }

    // ------------------------------------------------------------------ setup

    void setupSandbox() {
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::bit32);
        lua_State* L = lua.lua_state();
        luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);  // hooks must see every instruction
        lua_pushlightuserdata(L, const_cast<char*>(&kHostRegistryKey));
        lua_pushlightuserdata(L, this);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_sethook(L, &Impl::hook, LUA_MASKCOUNT, kHookInterval);

        for (const char* name : {"dofile", "loadfile", "require", "module", "newproxy", "getfenv", "setfenv", "gcinfo",
                                 "collectgarbage", "load", "loadstring"}) {
            lua[name] = sol::lua_nil;
        }
        lua["string"]["dump"] = sol::lua_nil;
        lua.set_function("collectgarbage", [this](sol::optional<std::string> opt) -> double {
            if (opt && *opt != "count") {
                throw std::invalid_argument("collectgarbage: only \"count\" is allowed");
            }
            return static_cast<double>(lua_gc(lua.lua_state(), LUA_GCCOUNT, 0));
        });
        // Text-only load (no bytecode, no reader functions) in the shared environment;
        // like stock Lua it returns nil plus the error message on failure.
        auto safeLoad = [this](const std::string& chunk,
                               sol::optional<std::string> name) -> std::tuple<sol::object, sol::object> {
            sol::load_result r = lua.load(chunk, name.value_or("=load"), sol::load_mode::text);
            if (!r.valid()) {
                const sol::error e = r;
                return {sol::make_object(lua, sol::lua_nil), sol::make_object(lua, std::string(e.what()))};
            }
            return {sol::make_object(lua, r.get<sol::protected_function>()), sol::make_object(lua, sol::lua_nil)};
        };
        lua.set_function("load", safeLoad);
        lua.set_function("loadstring", safeLoad);
        lua.set_function("print", [this](sol::variadic_args args) {
            std::string line;
            const auto tostring = lua.get<sol::protected_function>("tostring");
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i > 0) {
                    line += '\t';
                }
                const sol::protected_function_result r = tostring(args[i]);
                line += r.valid() ? r.get<std::string>() : std::string("?");
            }
            printBuffer += line + '\n';
            log.info("[lua:{}] {}", current, line);
        });
        sol::table math = lua.get<sol::table>("math");
        math.set_function("random", [this](sol::optional<double> a, sol::optional<double> b) -> double {
            const double r = uniform();
            if (!a) {
                return r;
            }
            const double lo = b ? *a : 1.0;
            const double hi = b ? *b : *a;
            return std::floor(lo + r * (hi - lo + 1.0));
        });
        math.set_function("randomseed", [this](double seed) { rng = core::Pcg32(static_cast<std::uint64_t>(seed)); });
    }

    void setupApi() {
        sol::table logT = lua.create_named_table("log");
        logT.set_function("info", [this](const std::string& m) { log.info("[lua:{}] {}", current, m); });
        logT.set_function("warn", [this](const std::string& m) { log.warn("[lua:{}] {}", current, m); });
        logT.set_function("error", [this](const std::string& m) { log.error("[lua:{}] {}", current, m); });

        // scene.* — everything through commands.
        sol::table scene = lua.create_named_table("scene");
        scene.set_function("command", [this](const std::string& n, sol::object p) { return commandLua(n, p); });
        scene.set_function("place", [this](sol::object object) {
            return idFrom(command("PlaceObject", Json{{"object", luaToJson(object)}}));
        });
        scene.set_function("create_goal", [this](sol::object goal) {
            return idFrom(command("CreateGoal", Json{{"goal", luaToJson(goal)}}));
        });
        scene.set_function("create_spawner", [this](sol::object spawner) {
            return idFrom(command("CreateSpawner", Json{{"spawner", luaToJson(spawner)}}));
        });
        scene.set_function("update", [this](std::uint32_t id, sol::object patch) {
            command("UpdateEntity", Json{{"id", id}, {"patch", luaToJson(patch)}});
        });
        scene.set_function("delete", [this](std::uint32_t id) { command("DeleteEntity", Json{{"id", id}}); });
        scene.set_function("get", [this](std::uint32_t id) { return entityTable(id); });
        scene.set_function("find", [this](const std::string& name) -> sol::object {
            for (std::uint32_t id : entitiesOfType("")) {
                if (ctx.sim.findStaticObject(id)->name == name) {
                    return sol::make_object(lua, id);
                }
            }
            for (sim::SceneId id : ctx.sim.goalIds()) {
                if (ctx.sim.findGoal(id)->name == name) {
                    return sol::make_object(lua, id);
                }
            }
            for (sim::SceneId id : ctx.sim.spawnerIds()) {
                if (ctx.sim.findSpawner(id)->name == name) {
                    return sol::make_object(lua, id);
                }
            }
            return sol::make_object(lua, sol::lua_nil);
        });
        scene.set_function("objects", [this]() {
            sol::table t = lua.create_table();
            int i = 1;
            for (std::uint32_t id : entitiesOfType("")) {
                t[i++] = entityTable(id);
            }
            return t;
        });

        // entities.*
        sol::table entities = lua.create_named_table("entities");
        entities.set_function("register_type", [this](const sol::table& def) { registerEntityType(def); });
        entities.set_function("spawn", [this](const std::string& type, sol::object placement) {
            Json p =
                placement.valid() && placement.get_type() == sol::type::table ? luaToJson(placement) : Json::object();
            p["type"] = type;
            return idFrom(command("SpawnEntity", p));
        });
        entities.set_function("list", [this](sol::optional<std::string> type) {
            sol::table t = lua.create_table();
            int i = 1;
            for (std::uint32_t id : entitiesOfType(type.value_or(""))) {
                t[i++] = entityTable(id);
            }
            return t;
        });

        // agents.*
        sol::table agents = lua.create_named_table("agents");
        agents.set_function("register_type", [this](sol::object def) {
            Json t = luaToJson(def);
            t["format"] = "crowdsim.agent";
            t["version"] = 1;
            command("DefineAgentType", Json{{"type", t}});
        });
        agents.set_function("count", [this]() { return ctx.sim.agents().size(); });
        agents.set_function(
            "count_near", [this](float x, float y, float z, float r) { return ctx.sim.countAgentsNear({x, y, z}, r); });
        // Crowd conditions inside an axis-aligned box (x, z; optionally only between floor
        // heights y0 and y1, for one storey of a building): how many people, how dense, the
        // pressure on them and how many are on the ground.
        agents.set_function("region", [this](float x0, float z0, float x1, float z1, sol::optional<float> y0,
                                             sol::optional<float> y1) {
            const float minY = std::min(y0.value_or(-1e9f), y1.value_or(1e9f));
            const float maxY = std::max(y0.value_or(-1e9f), y1.value_or(1e9f));
            const sim::AgentPool& a = ctx.sim.agents();
            const float minX = std::min(x0, x1);
            const float maxX = std::max(x0, x1);
            const float minZ = std::min(z0, z1);
            const float maxZ = std::max(z0, z1);
            std::uint32_t count = 0;
            std::uint32_t fallen = 0;
            float peak = 0.0f;
            double sum = 0.0;
            for (std::uint32_t i = 0; i < a.size(); ++i) {
                const glm::vec2 p = a.position[i];
                if (p.x < minX || p.x > maxX || p.y < minZ || p.y > maxZ || a.height[i] < minY || a.height[i] > maxY) {
                    continue;
                }
                ++count;
                fallen += a.fallen[i];
                peak = std::max(peak, a.pressure[i]);
                sum += a.pressure[i];
            }
            const float area = std::max(1e-3f, (maxX - minX) * (maxZ - minZ));
            return jsonToLua(lua, Json{{"count", count},
                                       {"density", static_cast<float>(count) / area},
                                       {"peak_pressure", peak},
                                       {"mean_pressure", count > 0 ? sum / count : 0.0},
                                       {"fallen", fallen}});
        });
        agents.set_function("get", [this](std::uint32_t id) -> sol::object {
            const auto d = ctx.sim.agentDebug(id);
            if (!d) {
                return sol::make_object(lua, sol::lua_nil);
            }
            return jsonToLua(lua, Json{{"id", d->id},
                                       {"type", d->typeName},
                                       {"behaviour", d->treeName},
                                       {"state", d->stateName},
                                       {"goal", d->goalName},
                                       {"position", {d->position.x, d->position.y, d->position.z}},
                                       {"speed", d->speed}});
        });

        // bt.*
        sol::table bt = lua.create_named_table("bt");
        bt.set_function("register_node", [this](const sol::table& def) { registerNode(def); });
        bt.set_function("set", [this](sol::object tree) {
            Json t = luaToJson(tree);
            if (!t.contains("format")) {
                t["format"] = "crowdsim.bt";
            }
            command("SetBehaviorTree", Json{{"tree", t}});
        });
        bt.set_function("node_types", [this]() { return commandLua("ListBehaviorNodeTypes", sol::lua_nil); });

        // sim.*
        sol::table simT = lua.create_named_table("sim");
        simT.set_function("time", [this]() { return ctx.sim.simTimeSeconds(); });
        simT.set_function("step_index", [this]() { return static_cast<double>(ctx.sim.stepIndex()); });
        simT.set_function("seed", [this]() { return static_cast<double>(ctx.sim.config().seed); });
        simT.set_function("random", [this]() { return uniform(); });
        simT.set_function("play", [this]() { command("Play", Json::object()); });
        simT.set_function("pause", [this]() { command("Pause", Json::object()); });
        simT.set_function("step", [this](sol::optional<int> n) { command("Step", Json{{"steps", n.value_or(1)}}); });
        simT.set_function("command", [this](const std::string& n, sol::object p) { return commandLua(n, p); });

        // metrics.* (M9 extends this)
        sol::table metrics = lua.create_named_table("metrics");
        metrics.set_function("get", [this]() {
            const sim::SimStats s = ctx.sim.stats();
            return jsonToLua(lua, Json{{"alive", s.alive},
                                       {"spawned", s.spawned},
                                       {"arrived", s.arrived},
                                       {"despawned", s.despawned},
                                       {"average_speed", s.averageSpeed},
                                       {"fallen", s.fallen},
                                       {"falls", s.falls},
                                       {"peak_pressure", s.peakPressure},
                                       {"time", ctx.sim.simTimeSeconds()}});
        });

        // events.on(name, fn)
        sol::table events = lua.create_named_table("events");
        events.set_function("on", [this](const std::string& name, sol::protected_function fn) {
            static constexpr const char* kNames[] = {"agent_spawned", "agent_arrived", "agent_despawned", "step"};
            if (std::find_if(std::begin(kNames), std::end(kNames), [&](const char* n) { return name == n; }) ==
                std::end(kNames)) {
                throw std::invalid_argument("unknown event '" + name +
                                            "' (agent_spawned, agent_arrived, agent_despawned, step)");
            }
            handlers[name].push_back({current, std::move(fn)});
        });
        events.set_function("clear", [this](sol::optional<std::string> name) {
            if (name) {
                handlers.erase(*name);
            } else {
                handlers.clear();
            }
        });

        // A getter rather than &NodeContext::params: Clang cannot instantiate sol2's
        // data-member binding for a sol::table member (GCC and MSVC can).
        lua.new_usertype<NodeContext>("NodeContext", sol::no_constructor, "params",
                                      sol::readonly_property([](const NodeContext& c) { return c.params; }), "position",
                                      &NodeContext::position, "velocity", &NodeContext::velocity, "speed",
                                      &NodeContext::speed, "id", &NodeContext::id, "state", &NodeContext::state, "type",
                                      &NodeContext::type, "random", &NodeContext::random, "time", &NodeContext::time,
                                      "count_near", &NodeContext::countNear, "move_to", &NodeContext::moveTo,
                                      "move_to_goal", &NodeContext::moveToGoal, "stop", &NodeContext::stop, "set_speed",
                                      &NodeContext::setSpeed, "set_state", &NodeContext::setState);
    }

    EvalResult run(const std::string& chunkName, const std::string& source) {
        EvalResult out;
        printBuffer.clear();
        sol::load_result loaded = lua.load(source, "@" + chunkName, sol::load_mode::text);
        if (!loaded.valid()) {
            const sol::error e = loaded;
            out.ok = false;
            out.error = e.what();
            return out;
        }
        const sol::protected_function fn = loaded.get<sol::protected_function>();
        const sol::protected_function_result r = call(fn);
        out.output = printBuffer;
        printBuffer.clear();
        if (!r.valid()) {
            const sol::error e = r;
            out.ok = false;
            out.error = e.what();
            return out;
        }
        try {
            if (r.return_count() == 1) {
                out.value = luaToJson(r.get<sol::object>(0));
            } else if (r.return_count() > 1) {
                out.value = Json::array();
                for (int i = 0; i < r.return_count(); ++i) {
                    out.value.push_back(luaToJson(r.get<sol::object>(i)));
                }
            }
        } catch (const std::exception& e) {
            out.value = std::string("<") + e.what() + ">";
        }
        return out;
    }
};

ScriptHost::ScriptHost(commands::CommandRegistry& registry, commands::CommandContext& ctx, spdlog::logger& log,
                       ScriptLimits limits)
    : impl_(std::make_unique<Impl>(registry, ctx, log, limits)) {
    impl_->setupSandbox();
    impl_->setupApi();
    ctx.sim.setCustomNodeRunner(this);
    ctx.sim.addStepObserver(this);
}

ScriptHost::~ScriptHost() {
    impl_->ctx.sim.setCustomNodeRunner(nullptr);
    impl_->ctx.sim.removeStepObserver(this);
}

EvalResult ScriptHost::load(const std::string& name, const std::string& source) {
    Impl& im = *impl_;
    const std::string previous = im.current;
    im.current = name;
    // Hot reload: drop the event handlers this script registered last time.
    for (auto& [event, list] : im.handlers) {
        std::erase_if(list, [&](const Impl::Handler& h) { return h.owner == name; });
    }
    EvalResult r = im.run(name, source);
    im.current = previous;
    auto it = std::find_if(im.scripts.begin(), im.scripts.end(), [&](const ScriptInfo& s) { return s.name == name; });
    if (it == im.scripts.end()) {
        im.scripts.push_back({name, source, r.ok, r.error});
    } else {
        *it = ScriptInfo{name, source, r.ok, r.error};
    }
    if (!r.ok) {
        im.recordError(name, r.error, false);
        throw std::runtime_error(r.error);
    }
    im.log.info("Lua: script '{}' loaded", name);
    return r;
}

EvalResult ScriptHost::eval(const std::string& source) {
    Impl& im = *impl_;
    const std::string previous = im.current;
    im.current = "console";
    EvalResult r = im.run("console", source);
    im.current = previous;
    if (!r.ok) {
        im.recordError("console", r.error, false);
    }
    return r;
}

const std::vector<ScriptInfo>& ScriptHost::scripts() const noexcept {
    return impl_->scripts;
}

std::vector<ScriptError> ScriptHost::errors() const {
    return {impl_->errors.begin(), impl_->errors.end()};
}

Json ScriptHost::entityTypesJson() const {
    Json list = Json::array();
    for (const auto& [name, t] : impl_->entityTypes) {
        list.push_back(Json{{"name", name},
                            {"script", t.script},
                            {"object", t.object},
                            {"tickHz", t.tickHz},
                            {"onSpawn", t.onSpawn.valid()},
                            {"onTick", t.onTick.valid()}});
    }
    return list;
}

Json ScriptHost::entityObject(const std::string& type, const Json& placement) const {
    const auto it = impl_->entityTypes.find(type);
    if (it == impl_->entityTypes.end()) {
        throw std::invalid_argument("unknown entity type '" + type + "' (register it with entities.register_type)");
    }
    Json o = it->second.object;
    o["name"] = placement.value("name", o.value("name", type));
    if (placement.contains("position")) {
        o["position"] = placement["position"];
    }
    if (placement.contains("rotationY")) {
        o["rotationY"] = placement["rotationY"];
    }
    return o;
}

void ScriptHost::entitySpawned(const std::string& type, std::uint32_t id) {
    Impl& im = *impl_;
    const auto it = im.entityTypes.find(type);
    if (it == im.entityTypes.end() || !it->second.onSpawn.valid()) {
        return;
    }
    const sol::protected_function_result r = im.call(it->second.onSpawn, im.entityTable(id));
    if (!r.valid()) {
        const sol::error e = r;
        im.recordError(type + ".on_spawn", e.what(), true);
    }
}

std::vector<Json> ScriptHost::logPrelude() const {
    std::vector<Json> out;
    for (const ScriptInfo& s : impl_->scripts) {
        if (s.ok) {
            out.push_back(commands::makeCommand("LoadScript", Json{{"name", s.name}, {"source", s.source}}));
        }
    }
    return out;
}

void ScriptHost::runCustomNodes(sim::Simulation& sim, std::span<const sim::CustomNodeBatch> batches) {
    Impl& im = *impl_;
    for (const sim::CustomNodeBatch& batch : batches) {
        const std::string& name = sim.customNodeSpecs()[batch.customIndex].name;
        const auto tick = im.nodeTicks.find(name);
        if (tick == im.nodeTicks.end()) {
            std::fill(batch.results.begin(), batch.results.end(), sim::CustomResult::Failure);
            continue;
        }
        NodeContext context(sim, im.paramsTable(*batch.tree, batch.node));
        sol::table agents = im.lua.create_table(static_cast<int>(batch.agents.size()), 0);
        for (std::size_t k = 0; k < batch.agents.size(); ++k) {
            agents[k + 1] = batch.agents[k];
        }
        const std::string previous = im.current;
        im.current = "node " + name;
        const sol::protected_function_result r = im.call(tick->second, std::ref(context), agents);
        im.current = previous;
        if (!r.valid()) {
            const sol::error e = r;
            im.recordError("node " + name, e.what(), true);
            std::fill(batch.results.begin(), batch.results.end(), sim::CustomResult::Failure);
            continue;
        }
        const sol::object ret = r.get<sol::object>();
        if (ret.get_type() == sol::type::table) {
            const sol::table t = ret.as<sol::table>();
            for (std::size_t k = 0; k < batch.results.size(); ++k) {
                batch.results[k] = parseResult(t.get<sol::object>(k + 1));
            }
        } else {
            std::fill(batch.results.begin(), batch.results.end(), parseResult(ret));
        }
    }
}

void ScriptHost::afterStep(sim::Simulation& sim) {
    Impl& im = *impl_;
    if (!im.handlers.empty()) {
        auto fire = [&](const char* name, auto&&... args) {
            const auto it = im.handlers.find(name);
            if (it == im.handlers.end()) {
                return;
            }
            for (const Impl::Handler& h : it->second) {
                const sol::protected_function_result r = im.call(h.fn, args...);
                if (!r.valid()) {
                    const sol::error e = r;
                    im.recordError(std::string("events.") + name, e.what(), true);
                }
            }
        };
        for (const sim::SimEvent& e : sim.stepEvents()) {
            switch (e.type) {
                case sim::SimEventType::AgentSpawned:
                    fire("agent_spawned", e.agentId);
                    break;
                case sim::SimEventType::AgentArrived: {
                    const auto goals = sim.goalIds();
                    std::string goal;
                    if (e.goalIndex >= 0 && static_cast<std::size_t>(e.goalIndex) < goals.size()) {
                        goal = sim.findGoal(goals[static_cast<std::size_t>(e.goalIndex)])->name;
                    }
                    fire("agent_arrived", e.agentId, goal);
                    break;
                }
                case sim::SimEventType::AgentDespawned:
                    fire("agent_despawned", e.agentId);
                    break;
            }
        }
        fire("step", static_cast<double>(sim.stepIndex()), sim.simTimeSeconds());
    }
    for (auto& [name, type] : im.entityTypes) {
        if (!type.onTick.valid()) {
            continue;
        }
        const auto stride =
            static_cast<std::uint64_t>(std::max(1.0, std::round(sim.config().stepHz / std::max(0.1, type.tickHz))));
        if (sim.stepIndex() % stride != 0) {
            continue;
        }
        const double dt = static_cast<double>(stride) * sim.stepSeconds();
        for (std::uint32_t id : im.entitiesOfType(name)) {
            const sol::protected_function_result r = im.call(type.onTick, im.entityTable(id), dt);
            if (!r.valid()) {
                const sol::error e = r;
                im.recordError(name + ".on_tick", e.what(), true);
            }
        }
    }
}

}  // namespace crowdsim::script
