#include <doctest/doctest.h>

#include <spdlog/spdlog.h>

#include "crowdsim/core/Logging.hpp"

using crowdsim::core::LogConfig;
using crowdsim::core::LogService;

TEST_CASE("LogService keeps recent lines in its ring buffer") {
    LogConfig config;
    config.console = false;
    config.ringCapacity = 3;
    LogService logs(config);

    for (int i = 0; i < 5; ++i) {
        logs.logger().info("line {}", i);
    }
    const auto lines = logs.tail();
    REQUIRE(lines.size() == 3);
    CHECK(lines.front().find("line 2") != std::string::npos);
    CHECK(lines.back().find("line 4") != std::string::npos);
    CHECK(logs.tail(1).size() == 1);
}

TEST_CASE("LogService does not register its logger globally") {
    LogConfig config;
    config.name = "crowdsim-test-unregistered";
    config.console = false;
    LogService logs(config);
    CHECK(spdlog::get(config.name) == nullptr);
}
