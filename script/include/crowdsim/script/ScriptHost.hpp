#pragma once

/// @file ScriptHost.hpp
/// The Lua scripting domain: one sandboxed LuaJIT state (owned here, injected by
/// the composition root, never global) exposing the CrowdSim API documented in
/// docs/scripting.md.
///
/// * Sandbox: base, string, table, math and bit only. No os, io, debug, ffi,
///   jit, package/require, dofile/loadfile, string.dump or bytecode loading.
///   The JIT is off so the instruction hook covers all code; each call into Lua
///   gets an instruction, wall-time and memory budget.
/// * Determinism: math.random is a world-seeded PCG stream; script BT nodes run
///   serially in agent order (see sim/ScriptHooks.hpp). Commands issued from
///   Lua are applied directly and not logged: replaying the LoadScript /
///   EvalLua command that ran the script regenerates them.
/// * Errors never escape: they are logged and kept in errors().

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/sim/ScriptHooks.hpp"

namespace spdlog {
class logger;
}
namespace crowdsim::commands {
class CommandRegistry;
struct CommandContext;
}  // namespace crowdsim::commands

namespace crowdsim::script {

using Json = nlohmann::json;

struct ScriptLimits {
    std::uint64_t instructionsPerCall = 20'000'000;
    double secondsPerCall = 0.5;
    std::size_t memoryBytes = 256u * 1024u * 1024u;
};

struct ScriptInfo {
    std::string name;
    std::string source;
    bool ok = true;
    std::string error;
};

struct ScriptError {
    std::string where;  ///< script or node/entity callback
    std::string message;
    std::uint64_t step = 0;
};

struct EvalResult {
    bool ok = true;
    std::string output;  ///< everything print()ed
    Json value;          ///< returned value(s) converted to JSON
    std::string error;
};

class ScriptHost final : public sim::CustomNodeRunner, public sim::StepObserver {
public:
    ScriptHost(commands::CommandRegistry& registry, commands::CommandContext& ctx, spdlog::logger& log,
               ScriptLimits limits = {});
    ~ScriptHost() override;
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    /// Run (or re-run: hot reload) a script. Registrations made by a previous
    /// version are replaced by name. Throws std::runtime_error with the Lua error.
    EvalResult load(const std::string& name, const std::string& source);
    /// Evaluate a snippet in the shared environment (console, MCP script_eval).
    EvalResult eval(const std::string& source);

    [[nodiscard]] const std::vector<ScriptInfo>& scripts() const noexcept;
    [[nodiscard]] std::vector<ScriptError> errors() const;
    [[nodiscard]] Json entityTypesJson() const;

    /// Object JSON (PlaceObject "object") for one instance of a scripted entity type.
    [[nodiscard]] Json entityObject(const std::string& type, const Json& placement) const;
    /// Run the type's on_spawn callback for a freshly placed entity.
    void entitySpawned(const std::string& type, std::uint32_t id);

    /// Commands that recreate the scripting state (LoadScript per loaded script),
    /// placed at the start of command logs.
    [[nodiscard]] std::vector<Json> logPrelude() const;

    // sim hooks
    void runCustomNodes(sim::Simulation& sim, std::span<const sim::CustomNodeBatch> batches) override;
    void afterStep(sim::Simulation& sim) override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace crowdsim::script
