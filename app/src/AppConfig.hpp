#pragma once

/// @file AppConfig.hpp
/// Command-line configuration shared by the GUI and headless executables.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "crowdsim/core/Logging.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::app {

struct AppConfig {
    core::LogConfig log;
    sim::SimConfig sim;
    std::uint32_t threads = 0;      ///< 0 = hardware concurrency
    std::uint64_t steps = 300;      ///< headless: number of sim steps to run
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
