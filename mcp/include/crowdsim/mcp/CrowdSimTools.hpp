#pragma once

/// @file CrowdSimTools.hpp
/// The CrowdSim MCP tools and resources (see docs/ai-integration.md). Each tool
/// maps onto a command (so it is validated, logged and undoable like a GUI
/// action) or onto a read-only query. Results are kept small for LLM contexts:
/// summaries, samples and caps instead of raw dumps.

#include <filesystem>
#include <functional>
#include <future>
#include <string>

#include "crowdsim/commands/CommandDispatcher.hpp"
#include "crowdsim/mcp/McpServer.hpp"

namespace crowdsim::sim {
class Simulation;
}
namespace crowdsim::core {
class LogService;
}

namespace crowdsim::mcp {

/// What the tools need from the engine (references must outlive the server).
struct McpServices {
    std::function<commands::CommandOutcome(const Json& command)> execute;
    /// Run requested steps (Step / RunFor) now; returns steps run.
    std::function<std::uint64_t(std::uint64_t maxSteps)> runSteps;
    sim::Simulation& sim;
    core::LogService& logs;
    std::filesystem::path contentRoot;
    std::filesystem::path docsDir;
    std::function<std::string()> sceneName;
    /// GUI only: render a PNG of the viewport (base64) — null in headless mode.
    std::function<std::shared_future<ToolResult>(const Json& args)> screenshot;
};

void registerCrowdSimTools(McpServer& server, McpServices& services);

/// Metrics summary used by metrics_get and sim_run_for.
[[nodiscard]] Json metricsSummary(const sim::Simulation& sim);

}  // namespace crowdsim::mcp
