#include "crowdsim/script/ScriptCommands.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/script/ScriptHost.hpp"

namespace crowdsim::script {

namespace {

using commands::CommandContext;
using commands::CommandOutput;
using commands::makeCommand;

Json str(const char* description) {
    return Json{{"type", "string"}, {"description", description}};
}
Json obj(Json properties, Json required = Json::array()) {
    return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}

constexpr std::size_t kMaxSourceBytes = 1u << 20;

std::string readScript(const CommandContext& ctx, const std::string& path) {
    std::ifstream in(ctx.assets.resolve(path), std::ios::binary);
    if (!in) {
        throw std::invalid_argument("cannot read script '" + path + "'");
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

Json evalJson(const EvalResult& r) {
    return Json{{"ok", r.ok}, {"output", r.output}, {"value", r.value}, {"error", r.error}};
}

}  // namespace

void registerScriptCommands(commands::CommandRegistry& r, ScriptHost& host) {
    r.add({"LoadScript",
           "Run (or hot-reload) a Lua script; its registrations replace earlier ones by name. Give the source inline "
           "or a content-relative path. Logged with the source, so logs replay without the file.",
           obj({{"name", str("script name (e.g. info_kiosk)")},
                {"source", str("Lua source")},
                {"path", str("content-relative path, e.g. scripts/info_kiosk.lua")}}),
           [&host](const Json& p, CommandContext& ctx) {
               std::string source;
               std::string name = p.value("name", std::string());
               if (p.contains("source")) {
                   source = p["source"].get<std::string>();
               } else if (p.contains("path")) {
                   const std::string path = p["path"].get<std::string>();
                   source = readScript(ctx, path);
                   if (name.empty()) {
                       const auto slash = path.find_last_of("/\\");
                       const auto stem = path.substr(slash == std::string::npos ? 0 : slash + 1);
                       name = stem.substr(0, stem.rfind('.'));
                   }
               } else {
                   throw std::invalid_argument("LoadScript needs \"source\" or \"path\"");
               }
               if (name.empty()) {
                   throw std::invalid_argument("LoadScript needs a \"name\"");
               }
               if (source.size() > kMaxSourceBytes) {
                   throw std::invalid_argument("script too large (1 MiB max)");
               }
               const EvalResult result = host.load(name, source);
               CommandOutput out;
               out.result = evalJson(result);
               out.result["name"] = name;
               out.logAs = makeCommand("LoadScript", Json{{"name", name}, {"source", source}});
               out.label = "Script '" + name + "'";
               return out;
           }});

    r.add({"ReloadScript", "Re-run content/scripts/<name>.lua.", obj({{"name", str("script name")}}, {"name"}),
           [&host](const Json& p, CommandContext& ctx) {
               const std::string name = p.at("name").get<std::string>();
               const std::string source = readScript(ctx, "scripts/" + name + ".lua");
               const EvalResult result = host.load(name, source);
               CommandOutput out;
               out.result = evalJson(result);
               out.logAs = makeCommand("LoadScript", Json{{"name", name}, {"source", source}});
               out.label = "Reload '" + name + "'";
               return out;
           }});

    r.add({"SaveScript",
           "Write content/scripts/<name>.lua and run it (the editor's Save). Logged as LoadScript with the "
           "source, so logs replay without the file; nothing is written while replaying.",
           obj({{"name", str("script name: letters, digits, _ and -")}, {"source", str("Lua source")}},
               {"name", "source"}),
           [&host](const Json& p, CommandContext& ctx) {
               const std::string name = p.at("name").get<std::string>();
               const std::string source = p.at("source").get<std::string>();
               const bool validName = !name.empty() && name.size() <= 64 &&
                                      std::all_of(name.begin(), name.end(), [](unsigned char c) {
                                          return std::isalnum(c) != 0 || c == '_' || c == '-';
                                      });
               if (!validName) {
                   throw std::invalid_argument("script names use letters, digits, _ and - (got '" + name + "')");
               }
               if (source.size() > kMaxSourceBytes) {
                   throw std::invalid_argument("script too large (1 MiB max)");
               }
               CommandOutput out;
               const std::string path = "scripts/" + name + ".lua";
               if (!ctx.replaying) {
                   const std::filesystem::path file = ctx.assets.resolve(path);
                   std::filesystem::create_directories(file.parent_path());
                   std::ofstream f(file, std::ios::binary);
                   if (!f || !(f << source)) {
                       throw std::invalid_argument("cannot write '" + path + "'");
                   }
               }
               const EvalResult result = host.load(name, source);
               out.result = evalJson(result);
               out.result["name"] = name;
               out.result["path"] = path;
               out.logAs = makeCommand("LoadScript", Json{{"name", name}, {"source", source}});
               out.label = "Save script '" + name + "'";
               return out;
           }});

    r.add({"EvalLua", "Evaluate Lua in the scripting environment; returns printed output and the result.",
           obj({{"source", str("Lua code")}}, {"source"}), [&host](const Json& p, CommandContext&) {
               const std::string source = p.at("source").get<std::string>();
               if (source.size() > kMaxSourceBytes) {
                   throw std::invalid_argument("snippet too large (1 MiB max)");
               }
               const EvalResult result = host.eval(source);
               if (!result.ok) {
                   throw std::runtime_error(result.error);
               }
               CommandOutput out;
               out.result = evalJson(result);
               out.label = "Lua";
               return out;
           }});

    r.add({"SpawnEntity", "Place an instance of a Lua-registered entity type. Returns its id.",
           obj({{"type", str("entity type name")},
                {"position", Json{{"type", "array"}, {"description", "[x, y, z]"}}},
                {"rotationY", Json{{"type", "number"}}},
                {"name", str("object name (default: the type name)")}},
               {"type", "position"}),
           [&host, &r](const Json& p, CommandContext& ctx) {
               const std::string type = p.at("type").get<std::string>();
               const Json object = host.entityObject(type, p);
               const commands::CommandSpec* place = r.find("PlaceObject");
               CommandOutput out = place->apply(Json{{"object", object}}, ctx);
               const auto id = out.result.value("id", 0u);
               host.entitySpawned(type, id);
               // Logged as SpawnEntity (not the PlaceObject it expands to) so on_spawn runs again on
               // replay; ids are assigned deterministically, so the replayed entity gets the same id.
               out.logAs = nullptr;
               out.label = "Spawn " + type;
               return out;
           }});

    r.add({"ListScripts", "Loaded scripts with their status.", obj({}),
           [&host](const Json&, CommandContext&) {
               CommandOutput out;
               Json list = Json::array();
               for (const ScriptInfo& s : host.scripts()) {
                   list.push_back(Json{{"name", s.name}, {"ok", s.ok}, {"error", s.error}, {"bytes", s.source.size()}});
               }
               out.result = Json{{"scripts", list}};
               return out;
           },
           false});

    r.add({"GetScript", "Source of a loaded script.", obj({{"name", str("script name")}}, {"name"}),
           [&host](const Json& p, CommandContext&) {
               const std::string name = p.at("name").get<std::string>();
               for (const ScriptInfo& s : host.scripts()) {
                   if (s.name == name) {
                       CommandOutput out;
                       out.result = Json{{"name", s.name}, {"source", s.source}, {"ok", s.ok}, {"error", s.error}};
                       return out;
                   }
               }
               throw std::invalid_argument("no script '" + name + "' is loaded");
           },
           false});

    r.add({"ScriptErrors", "Recent Lua errors (script loads, node ticks, entity callbacks, events).", obj({}),
           [&host](const Json&, CommandContext&) {
               CommandOutput out;
               Json list = Json::array();
               for (const ScriptError& e : host.errors()) {
                   list.push_back(Json{{"where", e.where}, {"message", e.message}, {"step", e.step}});
               }
               out.result = Json{{"errors", list}};
               return out;
           },
           false});

    r.add({"ListEntityTypes", "Entity types registered from Lua.", obj({}),
           [&host](const Json&, CommandContext&) {
               CommandOutput out;
               out.result = Json{{"entityTypes", host.entityTypesJson()}};
               return out;
           },
           false});
}

}  // namespace crowdsim::script
