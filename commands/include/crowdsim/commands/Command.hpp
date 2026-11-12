#pragma once

/// @file Command.hpp
/// Commands are the only way application state changes. A command is plain
/// JSON — {"cmd": "PlaceObject", "params": {...}} — so GUI actions, Lua
/// scripts, MCP tools and replay logs all speak the same language. Each command
/// type registers a handler that validates and applies it and returns:
///   - a result (e.g. the id of a created entity),
///   - an inverse command for undo (null if not undoable),
///   - optionally a canonical form to record in the log (e.g. LoadScene is
///     logged as RestoreScene with the loaded content, so logs are self-contained).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace spdlog {
class logger;
}

namespace crowdsim::sim {
class Simulation;
}

namespace crowdsim::assets {
class AssetLibrary;
}

namespace crowdsim::commands {

using Json = nlohmann::json;

/// Play state owned by the app; commands like Play / Step / RunFor edit it and
/// the app's main loop acts on it.
struct SimControl {
    bool playing = false;
    float speed = 1.0f;
    std::uint64_t pendingSteps = 0;  ///< steps requested by Step / RunFor, run by the main loop
};

/// What handlers may touch. Built by the composition root.
struct CommandContext {
    sim::Simulation& sim;
    assets::AssetLibrary& assets;
    SimControl& control;
    spdlog::logger& log;
    std::string& sceneName;
    bool replaying = false;  ///< true during log replay: skip external side effects (file writes)
};

struct CommandOutput {
    Json result = Json::object();
    Json undo = nullptr;   ///< inverse command, or null
    Json logAs = nullptr;  ///< canonical command for the log, or null to log the input
    std::string label;     ///< short human description for history panels
};

/// Throws (std::exception) to report invalid parameters or failures.
using CommandFn = std::function<CommandOutput(const Json& params, CommandContext& ctx)>;

struct CommandSpec {
    std::string name;
    std::string description;
    Json paramsSchema = Json::object();  ///< JSON Schema of "params" (used by MCP tools/list)
    CommandFn apply;
    bool logged = true;  ///< recorded in the command log (false for pure play controls / file saves)
};

/// {"cmd": name, "params": params}
[[nodiscard]] Json makeCommand(const std::string& name, Json params = Json::object());

class CommandRegistry {
public:
    void add(CommandSpec spec);
    [[nodiscard]] const CommandSpec* find(const std::string& name) const;
    [[nodiscard]] const std::vector<CommandSpec>& all() const noexcept { return specs_; }

private:
    std::vector<CommandSpec> specs_;
};

/// Register the built-in scene, navigation and simulation commands.
void registerBuiltinCommands(CommandRegistry& registry);

}  // namespace crowdsim::commands
