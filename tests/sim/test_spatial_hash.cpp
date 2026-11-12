#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <vector>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Random.hpp"
#include "crowdsim/sim/SpatialHash.hpp"

using namespace crowdsim;

TEST_CASE("SpatialHash query returns a superset of brute-force neighbours, each once") {
    core::JobSystem jobs(4);
    core::Pcg32 rng(5);
    std::vector<glm::vec2> points(3000);
    for (auto& p : points) {
        p = {rng.range(-50.0f, 50.0f), rng.range(-50.0f, 50.0f)};
    }
    sim::SpatialHash hash;
    const float radius = 2.5f;
    hash.build(points, radius, jobs);

    for (std::size_t q = 0; q < 200; ++q) {
        const glm::vec2 c = points[q * 7];
        std::vector<std::uint32_t> found;
        hash.query(c, radius, [&](std::uint32_t i) { found.push_back(i); });

        const std::set<std::uint32_t> unique(found.begin(), found.end());
        REQUIRE(unique.size() == found.size());  // no duplicates even with bucket collisions

        for (std::uint32_t i = 0; i < points.size(); ++i) {
            const glm::vec2 d = points[i] - c;
            if (d.x * d.x + d.y * d.y < radius * radius) {
                REQUIRE(unique.count(i) == 1);
            }
        }
    }
}

TEST_CASE("SpatialHash build order is independent of thread count") {
    core::Pcg32 rng(9);
    std::vector<glm::vec2> points(5000);
    for (auto& p : points) {
        p = {rng.range(-80.0f, 80.0f), rng.range(-80.0f, 80.0f)};
    }
    auto collect = [&](std::uint32_t threads) {
        core::JobSystem jobs(threads);
        sim::SpatialHash hash;
        hash.build(points, 2.0f, jobs);
        std::vector<std::uint32_t> order;
        hash.query({0.0f, 0.0f}, 4.0f, [&](std::uint32_t i) { order.push_back(i); });
        return order;
    };
    CHECK(collect(1) == collect(8));
}

TEST_CASE("SpatialHash handles empty input") {
    core::JobSystem jobs(1);
    sim::SpatialHash hash;
    hash.build({}, 1.0f, jobs);
    int visits = 0;
    hash.query({0.0f, 0.0f}, 1.0f, [&](std::uint32_t) { ++visits; });
    CHECK(visits == 0);
}
