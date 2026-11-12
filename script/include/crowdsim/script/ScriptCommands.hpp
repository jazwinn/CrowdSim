#pragma once

/// @file ScriptCommands.hpp
/// Commands that drive the scripting domain:
///   LoadScript {name, source | path}  run / hot-reload a script (logged with its source)
///   ReloadScript {name}               re-run content/scripts/<name>.lua
///   EvalLua {source}                  evaluate a snippet (logged; console, MCP script_eval)
///   SpawnEntity {type, position, rotationY?, name?}  place a scripted entity (undoable)
///   ListScripts, GetScript {name}, ScriptErrors, ListEntityTypes   (queries, not logged)

namespace crowdsim::commands {
class CommandRegistry;
}

namespace crowdsim::script {

class ScriptHost;

void registerScriptCommands(commands::CommandRegistry& registry, ScriptHost& host);

}  // namespace crowdsim::script
