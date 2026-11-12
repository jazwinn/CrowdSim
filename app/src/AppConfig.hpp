#pragma once

/// @file AppConfig.hpp
/// Command-line configuration shared by the GUI and headless executables.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "crowdsim/core/Logging.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::app {

struct AppConfig {
    core::LogConfig log;
    sim::SimConfig sim;
    sim::ScenarioKind scenario = sim::ScenarioKind::Stream;
    std::uint32_t agents = 5000;                    ///< total agents the scenario spawns
    std::string contentDir = CROWDSIM_CONTENT_DIR;  ///< root for scenes, models, logs
    std::string scenePath;                          ///< scene file to load instead of a built-in scenario
    std::string scriptPath;                         ///< Lua script to run after the scene loads (scenario script)
    std::string commandsFile;                       ///< headless: JSON array of commands to execute
    std::string replayFile;                         ///< headless: command log to replay
    std::string saveLogFile;                        ///< headless: write the command log here at exit
    std::string exportScene;                        ///< headless: save the scene here at exit
    bool vsync = true;                              ///< gui: present with vsync
    float simSpeed = 1.0f;                          ///< gui: initial simulation speed multiplier
    std::uint32_t selectId = 0;                     ///< gui: entity to select at startup (screenshots)
    std::uint32_t selectAgentId = 0xFFFFFFFFu;      ///< gui: agent to inspect at startup (screenshots)
    std::string editBehaviour;                      ///< gui: behaviour tree to open in the node editor
    std::string overlays;  ///< gui: comma-separated overlays to enable (navmesh,flow,velocities,...)
    /// gui: initial camera (target x, target z, distance, yaw deg, pitch deg); empty = default
    std::optional<std::array<float, 5>> camera;
    std::optional<std::array<float, 2>> lodDistances;  ///< gui: character LOD near / far distances (m)
    std::uint32_t threads = 0;                         ///< 0 = hardware concurrency
    std::uint64_t steps = 300;                         ///< headless: number of sim steps to run
    bool dumpRemaining = false;                        ///< headless: print the agents still alive at the end
    bool listCommands = false;                         ///< headless: print every command with its JSON schema and exit
    bool loadContent = true;        ///< load content/behaviours and content/agents (and hot reload them)
    bool mcpStdio = false;          ///< headless: serve MCP on stdin/stdout
    bool profile = false;           ///< headless: print the average time of each step phase
    std::string mcpHttp;            ///< serve MCP over HTTP on host:port (e.g. 127.0.0.1:8765)
    std::string mcpToken;           ///< optional bearer token required by the HTTP transport
    std::uint64_t frames = 0;       ///< gui: exit after N frames (0 = run until closed)
    std::string screenshotPath;     ///< gui: save a PNG of the last frame before exit
    std::string renderer = "auto";  ///< gui: auto | d3d11 | d3d12 | vulkan | metal | opengl | noop
    int width = 1600;
    int height = 900;
    bool showHelp = false;
    bool showVersion = false;
};

/// Result of parsing; `error` is set (and `config` meaningless) on failure.
struct ParseResult {
    AppConfig config;
    std::optional<std::string> error;
};

/// Parse `argv[1..]`. Unknown flags are errors so typos never silently change a run.
[[nodiscard]] ParseResult parseCommandLine(std::span<const std::string_view> args, bool gui);

[[nodiscard]] std::string usage(std::string_view exeName, bool gui);

}  // namespace crowdsim::app
