#include <doctest/doctest.h>

#include <array>

#include "AppConfig.hpp"

using crowdsim::app::parseCommandLine;

TEST_CASE("Command line parses common and headless options") {
    constexpr std::array<std::string_view, 8> args{"--seed", "99", "--steps",     "10",
                                                   "--hz",   "60", "--log-level", "debug"};
    const auto r = parseCommandLine(args, false);
    REQUIRE_FALSE(r.error);
    CHECK(r.config.sim.seed == 99);
    CHECK(r.config.steps == 10);
    CHECK(r.config.sim.stepHz == doctest::Approx(60.0));
    CHECK(r.config.log.level == spdlog::level::debug);
}

TEST_CASE("Command line rejects unknown and malformed arguments") {
    constexpr std::array<std::string_view, 1> unknown{"--nope"};
    CHECK(parseCommandLine(unknown, false).error);

    constexpr std::array<std::string_view, 2> badNumber{"--seed", "abc"};
    CHECK(parseCommandLine(badNumber, false).error);

    constexpr std::array<std::string_view, 1> missing{"--seed"};
    CHECK(parseCommandLine(missing, false).error);

    // GUI-only flags are rejected by the headless parser.
    constexpr std::array<std::string_view, 2> guiFlag{"--frames", "3"};
    CHECK(parseCommandLine(guiFlag, false).error);
    CHECK_FALSE(parseCommandLine(guiFlag, true).error);
}

TEST_CASE("Command line parses window size") {
    constexpr std::array<std::string_view, 2> size{"--size", "800x600"};
    const auto r = parseCommandLine(size, true);
    REQUIRE_FALSE(r.error);
    CHECK(r.config.width == 800);
    CHECK(r.config.height == 600);
}
