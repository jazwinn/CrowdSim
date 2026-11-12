#pragma once

/// @file Logging.hpp
/// Logger construction. CrowdSim never uses spdlog's default/global logger or
/// registry: the composition root creates one logger and passes it (by
/// reference) to every module that needs it.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/ringbuffer_sink.h>

namespace crowdsim::core {

/// Settings for the application logger.
struct LogConfig {
    std::string name = "crowdsim";
    spdlog::level::level_enum level = spdlog::level::info;
    bool console = true;              ///< Log to stderr (stdout is reserved for MCP stdio).
    std::string filePath;             ///< Optional log file; empty = no file.
    std::size_t ringCapacity = 2048;  ///< Lines kept in memory for UI / log_tail.
};

/// Owns the application logger and the in-memory ring buffer that UI panels
/// and the MCP `log_tail` tool read from.
class LogService {
public:
    explicit LogService(const LogConfig& config);

    LogService(const LogService&) = delete;
    LogService& operator=(const LogService&) = delete;

    /// The logger to inject into modules.
    [[nodiscard]] spdlog::logger& logger() noexcept { return *logger_; }

    /// Most recent formatted lines, oldest first. `limit == 0` returns all retained lines.
    [[nodiscard]] std::vector<std::string> tail(std::size_t limit = 0) const;

private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> ring_;
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace crowdsim::core
