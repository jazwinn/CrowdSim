#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>

#include <spdlog/logger.h>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/assets/BehaviorJson.hpp"
#include "crowdsim/assets/JsonTypes.hpp"
#include "crowdsim/assets/SceneSerializer.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::commands {

namespace {

using assets::EntityKind;

// --- Small schema helpers (JSON Schema subset used for MCP tool descriptions) ---------
Json obj(Json properties, std::vector<std::string> required = {}) {
    Json s{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) {
        s["required"] = required;
    }
    return s;
}
Json num(const char* d) {
    return Json{{"type", "number"}, {"description", d}};
}
Json integer(const char* d) {
    return Json{{"type", "integer"}, {"description", d}};
}
Json str(const char* d) {
    return Json{{"type", "string"}, {"description", d}};
}
Json any(const char* d) {
    return Json{{"type", "object"}, {"description", d}};
}
Json vec3(const char* d) {
    return Json{
        {"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", d}};
}

sim::SceneId requireId(const Json& params) {
    if (!params.contains("id") || !params["id"].is_number_integer()) {
        throw std::invalid_argument("missing integer \"id\"");
    }
    return params["id"].get<sim::SceneId>();
}

Json restoreSceneCommand(CommandContext& ctx) {
    return makeCommand("RestoreScene",
                       Json{{"scene", assets::saveScene(ctx.sim, ctx.sceneName)}, {"name", ctx.sceneName}});
}

/// Add an entity of `kind` from params[key] (entity JSON) and return output with undo.
CommandOutput addEntity(const Json& params, CommandContext& ctx, const char* key, const char* kind,
                        const char* commandName) {
    if (!params.contains(key) || !params[key].is_object()) {
        throw std::invalid_argument(std::format("missing object \"{}\"", key));
    }
    Json entity = params[key];
    entity["kind"] = kind;
    const sim::SceneId id = assets::addEntityFromJson(ctx.sim, entity, &ctx.assets);
    entity["id"] = id;
    entity.erase("kind");

    CommandOutput out;
    out.result = Json{{"id", id}};
    out.undo = makeCommand("DeleteEntity", Json{{"id", id}});
    out.logAs = makeCommand(commandName, Json{{key, entity}});  // with the assigned id
    out.label = std::format("Create {} '{}'", kind, entity.value("name", std::string(kind)));
    return out;
}

CommandOutput updateEntity(sim::SceneId id, const Json& patch, CommandContext& ctx) {
    Json before = assets::entityToJson(ctx.sim, id);
    if (before.is_null()) {
        throw std::invalid_argument(std::format("no scene entity with id {}", id));
    }
    assets::patchEntity(ctx.sim, id, patch, &ctx.assets);
    before.erase("id");
    before.erase("kind");
    CommandOutput out;
    out.result = Json{{"id", id}};
    out.undo = makeCommand("UpdateEntity", Json{{"id", id}, {"patch", before}});
    out.label = std::format("Edit '{}'", before.value("name", std::to_string(id)));
    return out;
}

Json locomotionToJson(const sim::LocomotionParams& p) {
    return Json{{"neighborDistance", assets::roundFloat(p.neighborDistance)},
                {"maxNeighbors", p.maxNeighbors},
                {"timeHorizon", assets::roundFloat(p.timeHorizon)},
                {"obstacleTimeHorizon", assets::roundFloat(p.obstacleTimeHorizon)},
                {"personalSpace", assets::roundFloat(p.personalSpace)},
                {"maxTurnRate", assets::roundFloat(p.maxTurnRate)},
                {"levelSeparation", assets::roundFloat(p.levelSeparation)}};
}

sim::LocomotionParams locomotionFromJson(const Json& j, sim::LocomotionParams p) {
    p.neighborDistance = j.value("neighborDistance", p.neighborDistance);
    p.maxNeighbors = j.value("maxNeighbors", p.maxNeighbors);
    p.timeHorizon = j.value("timeHorizon", p.timeHorizon);
    p.obstacleTimeHorizon = j.value("obstacleTimeHorizon", p.obstacleTimeHorizon);
    p.personalSpace = j.value("personalSpace", p.personalSpace);
    p.maxTurnRate = j.value("maxTurnRate", p.maxTurnRate);
    p.levelSeparation = j.value("levelSeparation", p.levelSeparation);
    if (p.neighborDistance <= 0.1f || p.timeHorizon <= 0.0f || p.obstacleTimeHorizon <= 0.0f) {
        throw std::invalid_argument("locomotion distances and horizons must be positive");
    }
    return p;
}

}  // namespace

void registerBuiltinCommands(CommandRegistry& r) {
    // ----------------------------------------------------------------------- scenes
    r.add({"CreateScene", "Clear everything and start an empty scene.", obj({{"name", str("scene name")}}),
           [](const Json& p, CommandContext& ctx) {
               CommandOutput out;
               out.undo = restoreSceneCommand(ctx);
               ctx.sim.clearScene();
               ctx.sceneName = p.value("name", std::string("untitled"));
               out.label = "New scene";
               return out;
           }});

    r.add({"RestoreScene", "Replace the scene with an inline scene document (scene.json content).",
           obj({{"scene", any("crowdsim.scene document")}, {"name", str("scene name")}}, {"scene"}),
           [](const Json& p, CommandContext& ctx) {
               CommandOutput out;
               out.undo = restoreSceneCommand(ctx);
               const std::string name = assets::loadScene(p.at("scene"), ctx.sim, ctx.assets);
               ctx.sceneName = p.value("name", name);
               out.label = "Restore scene";
               return out;
           }});

    r.add({"LoadScene", "Load a *.scene.json file (path relative to the content root).",
           obj({{"path", str("scene file")}}, {"path"}), [](const Json& p, CommandContext& ctx) {
               CommandOutput out;
               out.undo = restoreSceneCommand(ctx);
               const std::string path = p.at("path").get<std::string>();
               ctx.sceneName = assets::loadSceneFile(ctx.assets.resolve(path), ctx.sim, ctx.assets);
               // Log the content, not the path, so logs replay even if the file changes.
               out.logAs = makeCommand(
                   "RestoreScene", Json{{"scene", assets::saveScene(ctx.sim, ctx.sceneName)}, {"name", ctx.sceneName}});
               out.result = Json{{"name", ctx.sceneName}};
               out.label = "Load " + path;
               return out;
           }});

    r.add({"SaveScene", "Save the scene to a *.scene.json file (path relative to the content root).",
           obj({{"path", str("scene file")}, {"name", str("scene name to store")}}, {"path"}),
           [](const Json& p, CommandContext& ctx) {
               CommandOutput out;
               if (p.contains("name")) {
                   ctx.sceneName = p["name"].get<std::string>();
               }
               if (!ctx.replaying) {
                   const auto file = ctx.assets.resolve(p.at("path").get<std::string>());
                   assets::saveSceneFile(ctx.sim, ctx.sceneName, file);
                   out.result = Json{{"path", ctx.assets.relativize(file)}};
                   ctx.log.info("Saved scene '{}' to {}", ctx.sceneName, file.string());
               }
               return out;
           },
           /*logged=*/false});

    r.add({"LoadBuiltinScenario",
           "Replace the scene with a built-in scenario (stream, counterflow, benchmark, building).",
           obj({{"name", str("scenario name")}, {"agents", integer("total agents")}}, {"name"}),
           [](const Json& p, CommandContext& ctx) {
               const std::string name = p.at("name").get<std::string>();
               const auto kind = sim::parseScenario(name);
               if (!kind) {
                   throw std::invalid_argument("unknown scenario '" + name + "'");
               }
               CommandOutput out;
               out.undo = restoreSceneCommand(ctx);
               sim::buildScenario(ctx.sim, *kind, p.value<std::uint32_t>("agents", 2000));
               ctx.sceneName = name;
               out.logAs = makeCommand(
                   "RestoreScene", Json{{"scene", assets::saveScene(ctx.sim, ctx.sceneName)}, {"name", ctx.sceneName}});
               out.label = "Scenario " + name;
               return out;
           }});

    // ----------------------------------------------------------------------- entities
    const Json objectProps =
        obj({{"name", str("display name")},
             {"type", str("box | ramp | stairs | plane | mesh")},
             {"position", vec3("centre of the footprint at its lowest point")},
             {"rotationY", num("degrees about +Y")},
             {"size", vec3("width, height, depth (m)")},
             {"steps", integer("stairs only")},
             {"color", str("#rrggbb")},
             {"tags", Json{{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "walkable | obstacle"}}},
             {"asset", str("glTF path (type mesh)")},
             {"scale", num("uniform scale (type mesh)")}});
    r.add({"PlaceObject", "Add a static object (primitive or imported mesh). Returns its id.",
           obj({{"object", objectProps}}, {"object"}),
           [](const Json& p, CommandContext& ctx) { return addEntity(p, ctx, "object", "object", "PlaceObject"); }});

    r.add({"ImportGltf", "Import a glTF file as a static mesh object. Node names/extras set walkable/obstacle tags.",
           obj({{"path", str("glTF file relative to the content root")},
                {"name", str("object name")},
                {"position", vec3("placement")},
                {"rotationY", num("degrees")},
                {"scale", num("uniform scale")}},
               {"path"}),
           [](const Json& p, CommandContext& ctx) {
               const std::string path = p.at("path").get<std::string>();
               const assets::ImportedMesh& mesh = ctx.assets.loadMesh(path);
               Json object{{"name", p.value("name", path)},
                           {"type", "mesh"},
                           {"asset", ctx.assets.relativize(ctx.assets.resolve(path))},
                           {"position", p.value("position", Json::array({0, 0, 0}))},
                           {"rotationY", p.value("rotationY", 0.0)},
                           {"scale", p.value("scale", 1.0)},
                           {"tags", Json::array()}};
               CommandOutput out = addEntity(Json{{"object", object}}, ctx, "object", "object", "PlaceObject");
               out.result["triangles"] = mesh.mesh->triangleCount();
               out.result["walkableTriangles"] = mesh.walkableTriangles;
               out.result["obstacleTriangles"] = mesh.obstacleTriangles;
               out.result["nodeTags"] = mesh.nodeTags;
               out.label = "Import " + path;
               return out;
           }});

    r.add(
        {"CreateSpawner", "Add a spawner. Returns its id.",
         obj({{"spawner",
               any("{name, area:{center:[x,z], halfExtents:[x,z], elevation}, rate, total, startTime, "
                   "stopTime, goal:<goal id or name>, agent:{radius:[min,max], preferredSpeed:[min,max], ...}}")}},
             {"spawner"}),
         [](const Json& p, CommandContext& ctx) { return addEntity(p, ctx, "spawner", "spawner", "CreateSpawner"); }});

    r.add({"CreateGoal", "Add a goal area. Returns its id.",
           obj({{"goal", any("{name, area:{center:[x,z], halfExtents:[x,z], elevation}}")}}, {"goal"}),
           [](const Json& p, CommandContext& ctx) { return addEntity(p, ctx, "goal", "goal", "CreateGoal"); }});

    r.add({"CreateOffMeshLink", "Add an off-mesh link (ladder, escalator). Returns its id.",
           obj({{"link", any("{start:[x,y,z], end:[x,y,z], radius, bidirectional, extraCost}")}}, {"link"}),
           [](const Json& p, CommandContext& ctx) { return addEntity(p, ctx, "link", "link", "CreateOffMeshLink"); }});

    r.add({"AddEntity", "Add any entity from its full JSON form {kind, id, ...} (used by undo).",
           obj({{"entity", any("entity JSON with kind")}}, {"entity"}), [](const Json& p, CommandContext& ctx) {
               const Json& e = p.at("entity");
               const sim::SceneId id = assets::addEntityFromJson(ctx.sim, e, &ctx.assets);
               CommandOutput out;
               out.result = Json{{"id", id}};
               out.undo = makeCommand("DeleteEntity", Json{{"id", id}});
               Json logged = e;
               logged["id"] = id;
               out.logAs = makeCommand("AddEntity", Json{{"entity", logged}});
               out.label = std::format("Add '{}'", e.value("name", std::to_string(id)));
               return out;
           }});

    r.add({"UpdateEntity", "Merge-patch any scene entity's fields (e.g. {\"id\": 5, \"patch\": {\"rate\": 30}}).",
           obj({{"id", integer("entity id")}, {"patch", any("fields to change")}}, {"id", "patch"}),
           [](const Json& p, CommandContext& ctx) { return updateEntity(requireId(p), p.at("patch"), ctx); }});

    r.add({"TransformObject", "Move / rotate / resize an object.",
           obj({{"id", integer("object id")},
                {"position", vec3("new position")},
                {"rotationY", num("degrees")},
                {"size", vec3("new size (primitives)")},
                {"scale", num("new scale (meshes)")}},
               {"id"}),
           [](const Json& p, CommandContext& ctx) {
               Json patch = Json::object();
               for (const char* key : {"position", "rotationY", "size", "scale"}) {
                   if (p.contains(key)) {
                       patch[key] = p[key];
                   }
               }
               CommandOutput out = updateEntity(requireId(p), patch, ctx);
               out.label = "Transform " + out.label.substr(5);
               return out;
           }});

    r.add({"SetSpawnerConfig", "Change a spawner's settings (merge-patch).",
           obj({{"id", integer("spawner id")}, {"config", any("spawner fields")}}, {"id", "config"}),
           [](const Json& p, CommandContext& ctx) {
               if (assets::entityKindOf(ctx.sim, requireId(p)) != EntityKind::Spawner) {
                   throw std::invalid_argument("id is not a spawner");
               }
               return updateEntity(requireId(p), p.at("config"), ctx);
           }});

    r.add({"SetGoalConfig", "Change a goal's settings (merge-patch).",
           obj({{"id", integer("goal id")}, {"config", any("goal fields")}}, {"id", "config"}),
           [](const Json& p, CommandContext& ctx) {
               if (assets::entityKindOf(ctx.sim, requireId(p)) != EntityKind::Goal) {
                   throw std::invalid_argument("id is not a goal");
               }
               return updateEntity(requireId(p), p.at("config"), ctx);
           }});

    r.add({"DeleteEntity", "Delete any scene entity (object, spawner, goal, link) by id.",
           obj({{"id", integer("entity id")}}, {"id"}), [](const Json& p, CommandContext& ctx) {
               const sim::SceneId id = requireId(p);
               Json before = assets::entityToJson(ctx.sim, id);
               if (before.is_null() || !ctx.sim.removeEntity(id)) {
                   throw std::invalid_argument(std::format("no scene entity with id {}", id));
               }
               CommandOutput out;
               out.undo = makeCommand("AddEntity", Json{{"entity", before}});
               out.label = std::format("Delete '{}'", before.value("name", std::to_string(id)));
               return out;
           }});

    // ----------------------------------------------------------------------- behaviours
    r.add({"SetBehaviorTree",
           "Add or replace (hot reload) a behaviour tree from its *.bt.json content; agents using it restart "
           "from the root.",
           obj({{"tree", any("crowdsim.bt document: {name, root: {type, id, params, children}}")}}, {"tree"}),
           [](const Json& p, CommandContext& ctx) {
               sim::CompiledTree tree = assets::compileBehaviorJson(p.at("tree"), ctx.sim.customNodeSpecs());
               CommandOutput out;
               if (const sim::CompiledTree* old = ctx.sim.findBehaviorTree(tree.name)) {
                   out.undo = makeCommand("SetBehaviorTree", Json{{"tree", assets::behaviorToJson(*old)}});
               }
               const std::string name = tree.name;
               const std::size_t nodes = tree.nodes.size();
               ctx.sim.setBehaviorTree(std::move(tree));
               out.result =
                   Json{{"name", name}, {"nodes", nodes}, {"generation", ctx.sim.findBehaviorTree(name)->generation}};
               out.label = "Behaviour '" + name + "'";
               return out;
           }});

    r.add({"ValidateBehaviorTree", "Check a behaviour tree document without applying it.",
           obj({{"tree", any("crowdsim.bt document")}}, {"tree"}),
           [](const Json& p, CommandContext& ctx) {
               CommandOutput out;
               try {
                   const sim::CompiledTree tree = assets::compileBehaviorJson(p.at("tree"), ctx.sim.customNodeSpecs());
                   out.result = Json{{"valid", true},
                                     {"name", tree.name},
                                     {"nodes", tree.nodes.size()},
                                     {"memorySlots", tree.memSlots},
                                     {"timerSlots", tree.timerSlots}};
               } catch (const std::exception& e) {
                   out.result = Json{{"valid", false}, {"error", e.what()}};
               }
               return out;
           },
           false});

    r.add({"ListBehaviorNodeTypes", "List behaviour tree node types with their parameter schemas.", obj({}),
           [](const Json&, CommandContext& ctx) {
               CommandOutput out;
               out.result = Json{{"nodeTypes", assets::btNodeTypesJson(ctx.sim.customNodeSpecs())}};
               return out;
           },
           false});

    r.add({"GetBehaviorTree",
           "A behaviour tree as a *.bt.json document. Returns the content/behaviours file (with its editor "
           "layout) when it matches the tree in use, else the live tree.",
           obj({{"name", str("behaviour tree name")}}, {"name"}),
           [](const Json& p, CommandContext& ctx) {
               const std::string name = p.at("name").get<std::string>();
               const sim::CompiledTree* live = ctx.sim.findBehaviorTree(name);
               const auto file = ctx.assets.resolve("behaviours/" + name + ".bt.json");
               CommandOutput out;
               std::error_code ec;
               if (std::filesystem::exists(file, ec)) {
                   try {
                       std::ifstream in(file, std::ios::binary);
                       const Json doc = Json::parse(in);
                       const sim::CompiledTree fromFile = assets::compileBehaviorJson(doc, ctx.sim.customNodeSpecs());
                       if (live == nullptr || assets::behaviorToJson(fromFile) == assets::behaviorToJson(*live)) {
                           out.result = Json{{"tree", doc}, {"source", "file"}};
                           return out;
                       }
                   } catch (const std::exception& e) {
                       ctx.log.warn("GetBehaviorTree: {} is not usable: {}", file.string(), e.what());
                   }
               }
               if (live == nullptr) {
                   throw std::invalid_argument("no behaviour tree named '" + name + "'");
               }
               out.result = Json{{"tree", assets::behaviorToJson(*live)}, {"source", "live"}};
               return out;
           },
           false});

    r.add({"SaveBehaviorTree",
           "Validate a *.bt.json document and write it to content/behaviours/<name>.bt.json (keeps the editor "
           "layout). Does not apply it; the file watcher or SetBehaviorTree does.",
           obj({{"tree", any("crowdsim.bt document")}}, {"tree"}),
           [](const Json& p, CommandContext& ctx) {
               const Json& doc = p.at("tree");
               const sim::CompiledTree tree = assets::compileBehaviorJson(doc, ctx.sim.customNodeSpecs());
               CommandOutput out;
               if (!ctx.replaying) {
                   const auto file = ctx.assets.resolve("behaviours/" + tree.name + ".bt.json");
                   std::filesystem::create_directories(file.parent_path());
                   std::ofstream(file, std::ios::binary) << doc.dump(2) << '\n';
                   out.result = Json{{"path", ctx.assets.relativize(file)}};
                   ctx.log.info("Saved behaviour '{}' to {}", tree.name, file.string());
               }
               return out;
           },
           false});

    r.add({"ListAgentTypes", "Agent types with their behaviour and parameters.", obj({}),
           [](const Json&, CommandContext& ctx) {
               CommandOutput out;
               Json list = Json::array();
               for (const sim::AgentType& t : ctx.sim.agentTypes()) {
                   Json j = assets::toJson(t);
                   j.erase("format");
                   j.erase("version");
                   list.push_back(j);
               }
               out.result = Json{{"agentTypes", list}};
               return out;
           },
           false});

    r.add({"ListBehaviorTrees", "Behaviour trees in use (name, node count, generation, agent count).", obj({}),
           [](const Json&, CommandContext& ctx) {
               CommandOutput out;
               std::vector<std::uint32_t> users(ctx.sim.behaviorTrees().size(), 0);
               const sim::AgentPool& a = ctx.sim.agents();
               for (std::uint32_t i = 0; i < a.size(); ++i) {
                   if (a.tree[i] < users.size()) {
                       ++users[a.tree[i]];
                   }
               }
               Json list = Json::array();
               for (std::size_t t = 0; t < ctx.sim.behaviorTrees().size(); ++t) {
                   const sim::CompiledTree& tree = ctx.sim.behaviorTrees()[t];
                   list.push_back(Json{{"name", tree.name},
                                       {"nodes", tree.nodes.size()},
                                       {"generation", tree.generation},
                                       {"agents", users[t]}});
               }
               out.result = Json{{"behaviours", list}};
               return out;
           },
           false});

    r.add({"QueryAgents",
           "Agents matching optional filters (type, state, behaviour, area {center:[x,z], halfExtents:[x,z]}). "
           "Returns the total count and an evenly spaced sample of at most `limit` agents.",
           obj({{"type", str("agent type")},
                {"state", str("state label")},
                {"behaviour", str("behaviour tree")},
                {"area", any("{center:[x,z], halfExtents:[x,z]}")},
                {"limit", integer("max agents listed (default 50, max 500)")}}),
           [](const Json& p, CommandContext& ctx) {
               const sim::AgentPool& a = ctx.sim.agents();
               const auto& types = ctx.sim.agentTypes();
               const auto& states = ctx.sim.stateNames();
               const auto& trees = ctx.sim.behaviorTrees();
               const std::string type = p.value("type", std::string());
               const std::string state = p.value("state", std::string());
               const std::string behaviour = p.value("behaviour", std::string());
               const bool hasArea = p.contains("area");
               glm::vec2 lo(0.0f);
               glm::vec2 hi(0.0f);
               if (hasArea) {
                   const glm::vec2 c(p["area"].at("center").at(0).get<float>(), p["area"]["center"].at(1).get<float>());
                   const glm::vec2 h(p["area"].at("halfExtents").at(0).get<float>(),
                                     p["area"]["halfExtents"].at(1).get<float>());
                   lo = c - h;
                   hi = c + h;
               }
               std::vector<std::uint32_t> match;
               for (std::uint32_t i = 0; i < a.size(); ++i) {
                   if (!type.empty() && (a.agentType[i] >= types.size() || types[a.agentType[i]].name != type)) {
                       continue;
                   }
                   if (!state.empty() && (a.stateId[i] >= states.size() || states[a.stateId[i]] != state)) {
                       continue;
                   }
                   if (!behaviour.empty() && (a.tree[i] >= trees.size() || trees[a.tree[i]].name != behaviour)) {
                       continue;
                   }
                   const glm::vec2 q = a.position[i];
                   if (hasArea && (q.x < lo.x || q.x > hi.x || q.y < lo.y || q.y > hi.y)) {
                       continue;
                   }
                   match.push_back(i);
               }
               const std::size_t limit = std::clamp<std::size_t>(p.value<std::size_t>("limit", 50), 0, 500);
               Json list = Json::array();
               const std::size_t n = std::min(limit, match.size());
               for (std::size_t k = 0; k < n; ++k) {
                   const std::uint32_t i = match[k * match.size() / std::max<std::size_t>(1, n)];
                   const auto d = ctx.sim.agentDebug(a.id[i]);
                   list.push_back(
                       Json{{"id", a.id[i]},
                            {"type", d->typeName},
                            {"state", d->stateName},
                            {"behaviour", d->treeName},
                            {"goal", d->goalName},
                            {"position",
                             {std::round(d->position.x * 100.0f) / 100.0f, std::round(d->position.y * 100.0f) / 100.0f,
                              std::round(d->position.z * 100.0f) / 100.0f}},
                            {"speed", std::round(d->speed * 100.0f) / 100.0f}});
               }
               CommandOutput out;
               out.result = Json{{"total", match.size()}, {"listed", list.size()}, {"agents", list}};
               return out;
           },
           false});

    r.add({"SpawnAgents",
           "Spawn `count` agents in an area now (adds a one-shot spawner that emits them within a second). "
           "Returns the spawner id.",
           obj({{"count", integer("number of agents")},
                {"area", any("{center:[x,z], halfExtents:[x,z], elevation}")},
                {"goal", any("goal id or name")},
                {"type", str("agent type (default: default)")},
                {"name", str("spawner name")}},
               {"count", "area", "goal"}),
           [](const Json& p, CommandContext& ctx) {
               const auto count = p.at("count").get<std::uint32_t>();
               if (count == 0 || count > 100000) {
                   throw std::invalid_argument("count must be 1..100000");
               }
               Json spawner{{"name", p.value("name", std::string("Burst"))},
                            {"area", p.at("area")},
                            {"rate", std::max(50.0, static_cast<double>(count) * 2.0)},
                            {"total", count},
                            {"startTime", ctx.sim.simTimeSeconds()},
                            {"goal", p.at("goal")}};
               if (p.contains("type")) {
                   spawner["agentMix"] = Json::array({Json{{"type", p["type"]}, {"weight", 1}}});
               }
               return addEntity(Json{{"spawner", spawner}}, ctx, "spawner", "spawner", "CreateSpawner");
           }});

    r.add({"DefineAgentType", "Add or replace an agent type from its *.agent.json content.",
           obj({{"type", any("crowdsim.agent document: {name, behaviour, radius, preferredSpeed, color, ...}")}},
               {"type"}),
           [](const Json& p, CommandContext& ctx) {
               const sim::AgentType type = assets::agentTypeFromJson(p.at("type"));
               CommandOutput out;
               if (const sim::AgentType* old = ctx.sim.findAgentType(type.name)) {
                   out.undo = makeCommand("DefineAgentType", Json{{"type", assets::toJson(*old)}});
               }
               ctx.sim.defineAgentType(type);
               out.result = Json{{"name", type.name}};
               out.label = "Agent type '" + type.name + "'";
               return out;
           }});

    r.add({"SetAgentTypeBehaviour", "Switch an agent type to another behaviour tree (running agents restart).",
           obj({{"type", str("agent type name")}, {"behaviour", str("behaviour tree name")}}, {"type", "behaviour"}),
           [](const Json& p, CommandContext& ctx) {
               const std::string name = p.at("type").get<std::string>();
               const sim::AgentType* old = ctx.sim.findAgentType(name);
               if (old == nullptr) {
                   throw std::invalid_argument("unknown agent type '" + name + "'");
               }
               sim::AgentType t = *old;
               CommandOutput out;
               out.undo = makeCommand("SetAgentTypeBehaviour", Json{{"type", name}, {"behaviour", t.behaviour}});
               t.behaviour = p.at("behaviour").get<std::string>();
               ctx.sim.defineAgentType(t);
               out.label = "Behaviour of '" + name + "'";
               return out;
           }});

    // ----------------------------------------------------------------------- navigation
    r.add({"RebuildNavmesh", "Rebuild the navmesh and flow fields now (otherwise done lazily).", obj({}),
           [](const Json&, CommandContext& ctx) {
               ctx.sim.updateNavigation();
               CommandOutput out;
               out.result = Json{{"polygons", ctx.sim.navMesh().polyCount()},
                                 {"tiles", ctx.sim.navMesh().tileCount()},
                                 {"walls", ctx.sim.navMesh().walls().size()}};
               return out;
           }});

    r.add({"SetNavigationSettings", "Change navmesh build settings (cellSize, agentRadius, agentMaxClimb, ...).",
           obj({{"settings", any("navigation settings")}}, {"settings"}), [](const Json& p, CommandContext& ctx) {
               Json before = assets::toJson(ctx.sim.config().navigation);
               Json merged = before;
               merged.merge_patch(p.at("settings"));
               ctx.sim.setNavigationSettings(assets::navSettingsFromJson(merged));
               CommandOutput out;
               out.undo = makeCommand("SetNavigationSettings", Json{{"settings", before}});
               out.label = "Navigation settings";
               return out;
           }});

    // ----------------------------------------------------------------------- simulation
    r.add({"SetSimParams", "Change locomotion parameters and/or the seed (seed applies at the next reset).",
           obj({{"seed", integer("world seed")},
                {"locomotion", any("neighborDistance, maxNeighbors, timeHorizon, ...")}}),
           [](const Json& p, CommandContext& ctx) {
               const Json before{{"seed", ctx.sim.config().seed},
                                 {"locomotion", locomotionToJson(ctx.sim.config().locomotion)}};
               if (p.contains("locomotion")) {
                   ctx.sim.setLocomotionParams(locomotionFromJson(p["locomotion"], ctx.sim.config().locomotion));
               }
               if (p.contains("seed")) {
                   ctx.sim.setSeed(p["seed"].get<std::uint64_t>());
               }
               CommandOutput out;
               out.undo = makeCommand("SetSimParams", before);
               out.label = "Simulation parameters";
               return out;
           }});

    r.add({"ResetSimulation", "Remove all agents and return to t = 0 (the scene is kept).", obj({}),
           [](const Json&, CommandContext& ctx) {
               ctx.sim.reset();
               ctx.control.pendingSteps = 0;
               return CommandOutput{};
           }});

    r.add({"Play", "Run the simulation in real time (GUI).", obj({}),
           [](const Json&, CommandContext& ctx) {
               ctx.control.playing = true;
               return CommandOutput{};
           },
           false});
    r.add({"Pause", "Pause the simulation.", obj({}),
           [](const Json&, CommandContext& ctx) {
               ctx.control.playing = false;
               return CommandOutput{};
           },
           false});
    r.add({"SetSimSpeed", "Real-time multiplier for playback.", obj({{"speed", num("0..16")}}, {"speed"}),
           [](const Json& p, CommandContext& ctx) {
               ctx.control.speed = std::clamp(p.at("speed").get<float>(), 0.0f, 16.0f);
               return CommandOutput{};
           },
           false});
    r.add({"Step", "Advance a number of fixed steps (default 1).", obj({{"steps", integer("steps")}}),
           [](const Json& p, CommandContext& ctx) {
               ctx.control.pendingSteps += p.value<std::uint64_t>("steps", 1);
               return CommandOutput{};
           },
           false});
    r.add({"RunFor", "Advance by simulated seconds.", obj({{"seconds", num("simulated seconds")}}, {"seconds"}),
           [](const Json& p, CommandContext& ctx) {
               const double seconds = p.at("seconds").get<double>();
               if (seconds < 0.0) {
                   throw std::invalid_argument("seconds must be >= 0");
               }
               ctx.control.pendingSteps += static_cast<std::uint64_t>(std::llround(seconds / ctx.sim.stepSeconds()));
               return CommandOutput{};
           },
           false});
}

}  // namespace crowdsim::commands
