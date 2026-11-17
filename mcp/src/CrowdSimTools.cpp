#include "crowdsim/mcp/CrowdSimTools.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "crowdsim/assets/JsonTypes.hpp"
#include "crowdsim/assets/SceneSerializer.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::mcp {

namespace {

using commands::makeCommand;

Json str(const char* d) {
    return Json{{"type", "string"}, {"description", d}};
}
Json num(const char* d) {
    return Json{{"type", "number"}, {"description", d}};
}
Json integer(const char* d) {
    return Json{{"type", "integer"}, {"description", d}};
}
Json object(const char* d) {
    return Json{{"type", "object"}, {"description", d}};
}
Json schema(Json props = Json::object(), Json required = Json::array()) {
    return Json{{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}};
}

constexpr std::size_t kMaxText = 8 * 1024;  ///< script output / log text returned to the model

std::string clip(std::string s) {
    if (s.size() > kMaxText) {
        s = s.substr(s.size() - kMaxText);
        s = "[... truncated]\n" + s;
    }
    return s;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

Json metricsSummary(const sim::Simulation& sim, const std::function<commands::CommandOutcome(const Json&)>& execute) {
    const sim::SimStats s = sim.stats();
    Json goals = Json::array();
    for (sim::SceneId id : sim.goalIds()) {
        const sim::Goal* g = sim.findGoal(id);
        goals.push_back(Json{{"id", id}, {"name", g->name}, {"arrivals", g->arrivals}});
    }
    std::map<std::string, std::uint32_t> byType;
    std::map<std::string, std::uint32_t> byState;
    const sim::AgentPool& a = sim.agents();
    for (std::uint32_t i = 0; i < a.size(); ++i) {
        if (a.agentType[i] < sim.agentTypes().size()) {
            ++byType[sim.agentTypes()[a.agentType[i]].name];
        }
        if (a.stateId[i] < sim.stateNames().size()) {
            ++byState[sim.stateNames()[a.stateId[i]]];
        }
    }
    Json out{{"time", sim.simTimeSeconds()},
             {"step", sim.stepIndex()},
             {"alive", s.alive},
             {"spawned", s.spawned},
             {"arrived", s.arrived},
             {"despawned", s.despawned},
             {"averageSpeed", std::round(s.averageSpeed * 1000.0f) / 1000.0f},
             {"goals", goals},
             {"aliveByType", byType},
             {"aliveByState", byState}};
    if (execute) {
        const commands::CommandOutcome analysis = execute(makeCommand("GetMetrics"));
        if (analysis.ok) {
            for (const char* key : {"speed", "density", "gates", "timeToGoal", "stuck"}) {
                if (analysis.result.contains(key)) {
                    out[key] = analysis.result[key];
                }
            }
        }
    }
    return out;
}

void registerCrowdSimTools(McpServer& server, McpServices& svc) {
    // A tool that runs one command; `build` turns tool arguments into command params.
    auto command = [&server, &svc](const char* tool, const char* description, Json inputSchema, const char* cmd,
                                   std::function<Json(const Json&)> build = nullptr) {
        server.addTool({tool, description, std::move(inputSchema),
                        [&svc, cmd, build](const Json& args) {
                            const commands::CommandOutcome o =
                                svc.execute(makeCommand(cmd, build ? build(args) : args));
                            if (!o.ok) {
                                return ToolResult::error(o.error);
                            }
                            return ToolResult::json(o.result.is_null() ? Json::object() : o.result);
                        },
                        nullptr});
    };
    auto patchArgs = [](const Json& a) { return Json{{"id", a.at("id")}, {"patch", a.at("patch")}}; };

    // --- Scene ----------------------------------------------------------------------------
    command("scene_new", "Start an empty scene (removes all objects, goals, spawners, agents).",
            schema({{"name", str("scene name")}}), "CreateScene");
    command("scene_load", "Load a *.scene.json from the content directory, e.g. scenes/corridor_bottleneck.scene.json.",
            schema({{"path", str("content-relative path")}}, {"path"}), "LoadScene");
    command("scene_save", "Save the scene to a *.scene.json in the content directory.",
            schema({{"path", str("content-relative path, e.g. scenes/my.scene.json")}, {"name", str("scene name")}},
                   {"path"}),
            "SaveScene");
    server.addTool(
        {"scene_describe",
         "Structured summary of the scene: groups, objects, goals (with arrivals), spawners, links, navmesh, "
         "behaviours and agent types. Entities in a group carry its id as `parent`. Lists are capped at 100 "
         "entries.",
         schema(),
         [&svc](const Json&) {
             const sim::Simulation& sim = svc.sim;
             auto list = [&](const std::vector<sim::SceneId>& ids) {
                 Json out = Json::array();
                 for (std::size_t k = 0; k < ids.size() && k < 100; ++k) {
                     out.push_back(assets::entityToJson(sim, ids[k]));
                 }
                 return out;
             };
             Json behaviours = Json::array();
             for (const sim::CompiledTree& t : sim.behaviorTrees()) {
                 behaviours.push_back(t.name);
             }
             Json types = Json::array();
             for (const sim::AgentType& t : sim.agentTypes()) {
                 types.push_back(Json{{"name", t.name}, {"behaviour", t.behaviour}});
             }
             return ToolResult::json(
                 Json{{"name", svc.sceneName ? svc.sceneName() : std::string()},
                      {"time", sim.simTimeSeconds()},
                      {"agentsAlive", sim.agents().size()},
                      {"navmesh", {{"polygons", sim.navMesh().polyCount()}, {"tiles", sim.navMesh().tileCount()}}},
                      {"groups", list(sim.groupIds())},
                      {"objectCount", sim.staticObjectIds().size()},
                      {"objects", list(sim.staticObjectIds())},
                      {"goals", list(sim.goalIds())},
                      {"spawners", list(sim.spawnerIds())},
                      {"links", list(sim.offMeshLinkIds())},
                      {"behaviours", behaviours},
                      {"agentTypes", types}});
         },
         nullptr});
    command("object_place",
            "Place a static object. object: {name, type: box|ramp|stairs|plane|mesh, position:[x,y,z], size:[x,y,z], "
            "rotationY (deg), color '#rrggbb', tags: ['walkable'|'obstacle', ...], steps (stairs), asset (mesh)}. "
            "position is the footprint centre at its lowest point. Returns its id.",
            schema({{"object", object("object fields (see description)")}}, {"object"}), "PlaceObject");
    command("group_create",
            "Add a group: a folder of scene entities, e.g. one level of a building. group: {name, parent (group "
            "id, optional)}. Returns its id.",
            schema({{"group", object("group fields")}}, {"group"}), "CreateGroup");
    command("entity_set_parent",
            "Put an entity (object, goal, spawner, link or group) in a group; parent 0 takes it out of any group.",
            schema({{"id", integer("entity id")}, {"parent", integer("group id, 0 = none")}}, {"id", "parent"}),
            "SetParent");
    command("entity_set_hidden",
            "Hide or show an entity in the editor view; hiding a group hides everything in it (e.g. a whole "
            "level). Display only: the simulation is unaffected.",
            schema({{"id", integer("entity id")}, {"hidden", Json{{"type", "boolean"}}}}, {"id", "hidden"}),
            "SetHidden");
    command("object_update", "Merge-patch any entity (object, goal, spawner, link, group) by id; `parent` moves "
            "it into a group (null = none).",
            schema({{"id", integer("entity id")}, {"patch", object("fields to change")}}, {"id", "patch"}),
            "UpdateEntity", patchArgs);
    command("object_delete", "Delete any entity by id (a deleted group's contents move up to its parent).", schema({{"id", integer("entity id")}}, {"id"}),
            "DeleteEntity");
    command("gltf_import",
            "Import a glTF/GLB (content-relative path) as a mesh object; node names/extras mark walkable/obstacle.",
            schema({{"path", str("content-relative path")},
                    {"position", Json{{"type", "array"}, {"description", "[x, y, z]"}}},
                    {"rotationY", num("degrees")},
                    {"scale", num("uniform scale")},
                    {"name", str("object name")}},
                   {"path"}),
            "ImportGltf");
    command("navmesh_rebuild", "Rebuild the navmesh and flow fields now (normally automatic).", schema(),
            "RebuildNavmesh");
    command("link_create",
            "Add an off-mesh link (ladder/escalator): {start:[x,y,z], end:[x,y,z], radius, bidirectional, extraCost}.",
            schema({{"link", object("link fields")}}, {"link"}), "CreateOffMeshLink");

    // --- Crowd ----------------------------------------------------------------------------
    command("goal_create",
            "Add a goal area: {name, area:{center:[x,z], halfExtents:[x,z], elevation}, capacity, serviceTime, "
            "queueDirection:[x,z]}. Returns its id.",
            schema({{"goal", object("goal fields")}}, {"goal"}), "CreateGoal");
    command("goal_update", "Merge-patch a goal.",
            schema({{"id", integer("goal id")}, {"patch", object("fields")}}, {"id", "patch"}), "UpdateEntity",
            patchArgs);
    command("spawner_create",
            "Add a spawner: {name, area:{center:[x,z], halfExtents:[x,z], elevation}, rate (agents/s), total, "
            "startTime, goal (id or name), agentMix:[{type, weight}], groupSize:[min,max], agent:{...}}.",
            schema({{"spawner", object("spawner fields")}}, {"spawner"}), "CreateSpawner");
    command("spawner_update", "Merge-patch a spawner.",
            schema({{"id", integer("spawner id")}, {"patch", object("fields")}}, {"id", "patch"}), "UpdateEntity",
            patchArgs);
    command("agent_type_define",
            "Define or replace an agent type: {name, behaviour, radius:[min,max], preferredSpeed:[min,max], "
            "maxSpeedFactor, maxAcceleration, personalSpace, height, color, colorJitter, tags}.",
            schema({{"type", object("agent type fields")}}, {"type"}), "DefineAgentType");
    command("agent_type_list", "All agent types with their parameters.", schema(), "ListAgentTypes");
    command("agent_type_save", "Write an agent type to its content file (agents/*.agent.json).",
            schema({{"name", str("agent type name")}}, {"name"}), "SaveAgentType");
    command("agents_spawn", "Spawn agents in an area now (one-shot spawner). Returns the spawner id.",
            schema({{"count", integer("agents")},
                    {"area", object("{center:[x,z], halfExtents:[x,z], elevation}")},
                    {"goal", Json{{"description", "goal id or name"}}},
                    {"type", str("agent type")}},
                   {"count", "area", "goal"}),
            "SpawnAgents");
    command("agents_query",
            "Count agents matching filters and list an evenly spaced sample (default 50, max 500): id, type, state, "
            "behaviour, goal, position, speed.",
            schema({{"type", str("agent type")},
                    {"state", str("state label")},
                    {"behaviour", str("behaviour tree")},
                    {"area", object("{center:[x,z], halfExtents:[x,z]}")},
                    {"limit", integer("max listed")}}),
            "QueryAgents");

    // --- Behaviour ------------------------------------------------------------------------
    command("bt_list", "Behaviour trees in use with node counts and how many agents run them.", schema(),
            "ListBehaviorTrees");
    command("bt_get", "A behaviour tree as *.bt.json.", schema({{"name", str("tree name")}}, {"name"}),
            "GetBehaviorTree");
    command("bt_set",
            "Add or replace (hot reload) a behaviour tree from a full *.bt.json document "
            "{format:'crowdsim.bt', version:1, name, root:{type, id, params, children}}. Running agents restart it.",
            schema({{"tree", object("crowdsim.bt document")}}, {"tree"}), "SetBehaviorTree");
    command("bt_validate", "Check a behaviour tree document without applying it.",
            schema({{"tree", object("crowdsim.bt document")}}, {"tree"}), "ValidateBehaviorTree");
    command("bt_node_types", "Node types (built-in and Lua) with parameter schemas and child limits.", schema(),
            "ListBehaviorNodeTypes");

    // --- Scripting ------------------------------------------------------------------------
    command("script_list", "Loaded Lua scripts and their status.", schema(), "ListScripts");
    command("script_get", "Source of a loaded Lua script.", schema({{"name", str("script name")}}, {"name"}),
            "GetScript");
    server.addTool(
        {"script_write",
         "Write content/scripts/<name>.lua and run it (registers nodes / entity types; replaces the "
         "previous version). See resource crowdsim://docs/scripting.md for the API.",
         schema({{"name", str("script name: letters, digits, _")}, {"source", str("Lua source")}}, {"name", "source"}),
         [&svc](const Json& args) {
             const std::string name = args.at("name").get<std::string>();
             if (name.empty() ||
                 name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
                     std::string::npos) {
                 return ToolResult::error("script names may only use letters, digits and _");
             }
             const std::string source = args.at("source").get<std::string>();
             const auto file = svc.contentRoot / "scripts" / (name + ".lua");
             std::filesystem::create_directories(file.parent_path());
             std::ofstream(file, std::ios::binary) << source;
             const commands::CommandOutcome o =
                 svc.execute(makeCommand("LoadScript", {{"name", name}, {"source", source}}));
             if (!o.ok) {
                 return ToolResult::error("written to scripts/" + name + ".lua but failed: " + o.error);
             }
             Json r = o.result;
             r["output"] = clip(r.value("output", std::string()));
             r["path"] = "scripts/" + name + ".lua";
             return ToolResult::json(r);
         },
         nullptr});
    command("script_reload", "Re-run content/scripts/<name>.lua.", schema({{"name", str("script name")}}, {"name"}),
            "ReloadScript");
    server.addTool({"script_eval",
                    "Evaluate Lua in the sandbox (instruction/time/memory limited); returns printed output (last 8 KB) "
                    "and the returned value.",
                    schema({{"source", str("Lua code")}}, {"source"}),
                    [&svc](const Json& args) {
                        const commands::CommandOutcome o = svc.execute(makeCommand("EvalLua", args));
                        if (!o.ok) {
                            return ToolResult::error(o.error);
                        }
                        Json r = o.result;
                        r["output"] = clip(r.value("output", std::string()));
                        return ToolResult::json(r);
                    },
                    nullptr});

    // --- Simulation -------------------------------------------------------------------------
    command("sim_play", "Run in real time (GUI).", schema(), "Play");
    command("sim_pause", "Pause real-time playback.", schema(), "Pause");
    server.addTool({"sim_step", "Advance a number of fixed steps (30 per simulated second by default) now.",
                    schema({{"steps", integer("steps (default 1, max 100000)")}}),
                    [&svc](const Json& args) {
                        const auto steps = std::clamp<std::uint64_t>(args.value<std::uint64_t>("steps", 1), 1, 100000);
                        const commands::CommandOutcome o = svc.execute(makeCommand("Step", {{"steps", steps}}));
                        if (!o.ok) {
                            return ToolResult::error(o.error);
                        }
                        const std::uint64_t ran = svc.runSteps(steps);
                        return ToolResult::json(Json{{"steps", ran}, {"time", svc.sim.simTimeSeconds()}});
                    },
                    nullptr});
    server.addTool({"sim_run_for",
                    "Advance the simulation by simulated seconds as fast as possible (headless-friendly) and return a "
                    "metrics summary: alive/spawned/arrived, per-goal arrivals, agents by type and state.",
                    schema({{"seconds", num("simulated seconds (max 3600)")}}, {"seconds"}),
                    [&svc](const Json& args) {
                        const double seconds = std::clamp(args.at("seconds").get<double>(), 0.0, 3600.0);
                        const commands::CommandOutcome o = svc.execute(makeCommand("RunFor", {{"seconds", seconds}}));
                        if (!o.ok) {
                            return ToolResult::error(o.error);
                        }
                        const auto start = std::chrono::steady_clock::now();
                        const std::uint64_t ran = svc.runSteps(~0ull);
                        const double ms =
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                        return ToolResult::json(Json{{"steps", ran},
                                                     {"wallMs", std::round(ms)},
                                                     {"metrics", metricsSummary(svc.sim, svc.execute)}});
                    },
                    nullptr});
    command("sim_reset", "Remove all agents and return to t = 0 (the scene is kept).", schema(), "ResetSimulation");
    command("sim_set_params",
            "Set the seed, locomotion parameters and/or body physics for packed crowds: {seed, locomotion:{...}, "
            "bodies:{enabled, stiffness, core, slopeLean, fallForce, tripForce, ...}}.",
            schema({{"seed", integer("world seed")},
                    {"locomotion", object("locomotion parameters")},
                    {"bodies", object("body contact, pressure and falls in packed crowds")}}),
            "SetSimParams");

    // --- Analysis ---------------------------------------------------------------------------
    command("attention_get",
            "How well each attention target (objects tagged `attention`: ads, signs, exits...) is seen: opportunity to see, viewability, noticing, dwell, "
            "glances, viewing angle and angular size, from each person's simulated gaze. Run the simulation first.",
            schema(), "GetAttentionMetrics");
    command("attention_set",
            "Change attention model settings (only the keys given; attention_get lists them under params). "
            "Clears the attention record.",
            schema({{"params", object("settings to change")}}, {"params"}), "SetAttentionParams");
    server.addTool({"metrics_get",
                    "Metrics summary: counts, per-goal arrivals, agents by type/state, speed and density "
                    "statistics, gate counts and flow rates, time-to-goal distribution, stuck agents.",
                    schema(), [&svc](const Json&) { return ToolResult::json(metricsSummary(svc.sim, svc.execute)); },
                    nullptr});
    server.addTool({"heatmap_get",
                    "Density (agents/m², time-averaged since the last reset) or speed (m/s) heatmap over the scene, "
                    "downsampled to at most maxCells cells (default 900): values[z][x] with origin and cellSize.",
                    schema({{"kind", str("density | speed | pressure")}, {"maxCells", integer("cell budget, 16..40000")}}),
                    [&svc](const Json& args) {
                        Json p{{"kind", args.value("kind", std::string("density"))},
                               {"maxCells", args.value<std::size_t>("maxCells", 900)}};
                        const commands::CommandOutcome o = svc.execute(makeCommand("GetHeatmap", p));
                        return o.ok ? ToolResult::json(o.result) : ToolResult::error(o.error);
                    },
                    nullptr});
    command("gate_add",
            "Add a counting gate (line segment on the floor, a/b as [x, z]); its crossings and flow rate appear in "
            "metrics_get.",
            schema({{"name", str("gate name")},
                    {"a", Json{{"type", "array"}, {"description", "[x, z]"}}},
                    {"b", Json{{"type", "array"}, {"description", "[x, z]"}}},
                    {"elevation", num("floor height")}},
                   {"name", "a", "b"}),
            "AddGate");
    command("recording_start", "Start recording agent trajectories at `hz` samples per second (default 6).",
            schema({{"hz", num("samples per second")}}), "StartRecording");
    command("recording_stop", "Stop recording trajectories; returns the number of rows.", schema(), "StopRecording");
    command("recording_export",
            "Write recorded trajectories (agent_id,type,time,x,y,z,vx,vz,state) as CSV to a content-relative path "
            "(default recordings/trajectories.csv).",
            schema({{"path", str("content-relative CSV path")}}), "ExportRecording");
    command("metrics_export", "Write the metrics time series as CSV (default recordings/metrics.csv).",
            schema({{"path", str("content-relative CSV path")}}), "ExportMetrics");

    // --- Logs -------------------------------------------------------------------------------
    server.addTool({"log_tail", "Most recent log lines (default 50, max 300), optionally only those containing text.",
                    schema({{"lines", integer("number of lines")}, {"contains", str("filter text")}}),
                    [&svc](const Json& args) {
                        const auto n = std::clamp<std::size_t>(args.value<std::size_t>("lines", 50), 1, 300);
                        const std::string filter = args.value("contains", std::string());
                        std::vector<std::string> lines = svc.logs.tail(0);
                        std::vector<std::string> out;
                        for (auto it = lines.rbegin(); it != lines.rend() && out.size() < n; ++it) {
                            if (filter.empty() || it->find(filter) != std::string::npos) {
                                out.push_back(*it);
                            }
                        }
                        std::reverse(out.begin(), out.end());
                        return ToolResult::json(Json{{"lines", out}});
                    },
                    nullptr});
    server.addTool({"errors_get", "Recent warnings/errors from the log and recent Lua errors.", schema(),
                    [&svc](const Json&) {
                        Json log = Json::array();
                        for (const std::string& line : svc.logs.tail(0)) {
                            if (line.find("[error]") != std::string::npos ||
                                line.find("[warning]") != std::string::npos ||
                                line.find("[critical]") != std::string::npos) {
                                log.push_back(line);
                            }
                        }
                        while (log.size() > 50) {
                            log.erase(log.begin());
                        }
                        const commands::CommandOutcome scripts = svc.execute(makeCommand("ScriptErrors"));
                        return ToolResult::json(
                            Json{{"log", log}, {"lua", scripts.ok ? scripts.result["errors"] : Json::array()}});
                    },
                    nullptr});

    // --- Vision (GUI) -------------------------------------------------------------------------
    if (svc.screenshot) {
        server.addTool({"screenshot",
                        "PNG of the editor window (3D view and panels; GUI mode only). Optional camera: "
                        "{target:[x,z], distance, yaw, pitch} (degrees) moves the view first; omitted = current view.",
                        schema({{"camera", object("{target:[x,z], distance, yaw, pitch}")}}), nullptr,
                        [&svc](const Json& args) { return svc.screenshot(args); }});
    }

    if (svc.recordStart && svc.recordStop) {
        server.addTool({"video_start",
                        "Start recording the 3D view, without editor UI, to an MJPEG .avi in content/recordings "
                        "(GUI mode only). Optional: fps (30), seconds (stop automatically; 0 = until "
                        "video_stop), name (file name without extension). The video has the viewport's size and "
                        "follows real time, so run the simulation (sim_play) while it records.",
                        schema({{"fps", num("frames per second, 1..120")},
                                {"seconds", num("stop after this long; 0 = until video_stop")},
                                {"name", str("file name without extension")}}),
                        [&svc](const Json& args) { return svc.recordStart(args); }});
        server.addTool({"video_stop", "Stop the video recording and finish the file; returns its path and length.",
                        schema({}), [&svc](const Json& args) { return svc.recordStop(args); }});
    }

    // --- Resources ----------------------------------------------------------------------------
    server.setResources(
        {[&svc]() {
             std::vector<Resource> out;
             out.push_back({"crowdsim://scene", "Current scene", "The scene as *.scene.json", "application/json"});
             for (const sim::CompiledTree& t : svc.sim.behaviorTrees()) {
                 out.push_back({"crowdsim://bt/" + t.name, "Behaviour " + t.name, "*.bt.json", "application/json"});
             }
             for (const sim::AgentType& t : svc.sim.agentTypes()) {
                 out.push_back(
                     {"crowdsim://agent-type/" + t.name, "Agent type " + t.name, "*.agent.json", "application/json"});
             }
             const commands::CommandOutcome scripts = svc.execute(makeCommand("ListScripts"));
             for (const Json& s : scripts.result.value("scripts", Json::array())) {
                 const std::string name = s["name"].get<std::string>();
                 out.push_back({"crowdsim://script/" + name, "Script " + name, "Lua source", "text/x-lua"});
             }
             std::error_code ec;
             for (const auto& e : std::filesystem::directory_iterator(svc.docsDir, ec)) {
                 if (e.path().extension() == ".md") {
                     const std::string file = e.path().filename().string();
                     out.push_back({"crowdsim://docs/" + file, file, "CrowdSim documentation", "text/markdown"});
                 }
             }
             return out;
         },
         [&svc](const std::string& uri) -> std::optional<std::pair<std::string, std::string>> {
             auto after = [&](const char* prefix) -> std::optional<std::string> {
                 const std::string p(prefix);
                 return uri.rfind(p, 0) == 0 ? std::optional<std::string>(uri.substr(p.size())) : std::nullopt;
             };
             if (uri == "crowdsim://scene") {
                 return std::pair{std::string("application/json"),
                                  assets::saveScene(svc.sim, svc.sceneName ? svc.sceneName() : "scene").dump(2)};
             }
             if (const auto name = after("crowdsim://bt/")) {
                 const auto o = svc.execute(makeCommand("GetBehaviorTree", {{"name", *name}}));
                 return o.ok ? std::optional(std::pair{std::string("application/json"), o.result["tree"].dump(2)})
                             : std::nullopt;
             }
             if (const auto name = after("crowdsim://agent-type/")) {
                 const auto o = svc.execute(makeCommand("ListAgentTypes"));
                 for (const Json& t : o.result.value("agentTypes", Json::array())) {
                     if (t.value("name", std::string()) == *name) {
                         return std::pair{std::string("application/json"), t.dump(2)};
                     }
                 }
                 return std::nullopt;
             }
             if (const auto name = after("crowdsim://script/")) {
                 const auto o = svc.execute(makeCommand("GetScript", {{"name", *name}}));
                 return o.ok
                            ? std::optional(std::pair{std::string("text/x-lua"), o.result["source"].get<std::string>()})
                            : std::nullopt;
             }
             if (const auto file = after("crowdsim://docs/")) {
                 const auto path = svc.docsDir / *file;
                 if (file->find("..") != std::string::npos || !std::filesystem::exists(path)) {
                     return std::nullopt;
                 }
                 return std::pair{std::string("text/markdown"), readFile(path)};
             }
             return std::nullopt;
         }});
}

}  // namespace crowdsim::mcp
