#include "crowdsim/core/Logging.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace crowdsim::core {

LogService::LogService(const LogConfig& config)
    : ring_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(config.ringCapacity)) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(ring_);
    if (config.console) {
        // stderr so that stdout stays clean for the MCP stdio transport.
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }
    if (!config.filePath.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.filePath, true));
    }

    // Constructed directly: deliberately NOT registered in spdlog's global registry.
    logger_ = std::make_shared<spdlog::logger>(config.name, sinks.begin(), sinks.end());
    logger_->set_level(config.level);
    logger_->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");
    logger_->flush_on(spdlog::level::warn);
}

std::vector<std::string> LogService::tail(std::size_t limit) const {
    return ring_->last_formatted(limit);
}

}  // namespace crowdsim::core
