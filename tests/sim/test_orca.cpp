#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <glm/geometric.hpp>

#include "crowdsim/sim/Orca.hpp"

using namespace crowdsim::sim;

namespace {

orca::AgentState agent(glm::vec2 pos, glm::vec2 vel, glm::vec2 pref, float maxSpeed = 1.5f) {
    return orca::AgentState{pos, vel, pref, 0.3f, maxSpeed, 0};
}

}  // namespace

TEST_CASE("ORCA with no neighbours returns the preferred velocity") {
    const auto v = orca::computeNewVelocity(agent({0, 0}, {0, 0}, {1.0f, 0.5f}), {}, {}, {});
    CHECK(v.x == doctest::Approx(1.0f));
    CHECK(v.y == doctest::Approx(0.5f));
}

TEST_CASE("ORCA clamps the preferred velocity to max speed") {
    const auto v = orca::computeNewVelocity(agent({0, 0}, {0, 0}, {10.0f, 0.0f}, 2.0f), {}, {}, {});
    CHECK(glm::length(v) == doctest::Approx(2.0f));
}

TEST_CASE("ORCA ignores a distant agent moving away") {
    const std::array<orca::Neighbor, 1> n{orca::Neighbor{{20.0f, 0.0f}, {1.0f, 0.0f}, 0.3f, 1}};
    const auto v = orca::computeNewVelocity(agent({0, 0}, {1, 0}, {1.0f, 0.0f}), n, {}, {});
    CHECK(v.x == doctest::Approx(1.0f));
    CHECK(v.y == doctest::Approx(0.0f));
}

TEST_CASE("Two agents walking head-on never overlap") {
    // Simulate both agents with ORCA and simple integration.
    glm::vec2 pa{-5.0f, 0.01f}, pb{5.0f, -0.01f};
    glm::vec2 va{0.0f, 0.0f}, vb{0.0f, 0.0f};
    const float dt = 1.0f / 30.0f;
    float minDist = 1e9f;
    for (int step = 0; step < 600; ++step) {
        const glm::vec2 prefA = glm::normalize(glm::vec2(5.0f, 0.0f) - pa) * 1.3f;
        const glm::vec2 prefB = glm::normalize(glm::vec2(-5.0f, 0.0f) - pb) * 1.3f;
        const std::array<orca::Neighbor, 1> nA{orca::Neighbor{pb, vb, 0.3f, 1}};
        const std::array<orca::Neighbor, 1> nB{orca::Neighbor{pa, va, 0.3f, 0}};
        const orca::Params params{2.0f, dt};
        const glm::vec2 newA = orca::computeNewVelocity({pa, va, prefA, 0.3f, 1.8f, 0}, nA, {}, params);
        const glm::vec2 newB = orca::computeNewVelocity({pb, vb, prefB, 0.3f, 1.8f, 1}, nB, {}, params);
        va = newA;
        vb = newB;
        pa += va * dt;
        pb += vb * dt;
        minDist = std::min(minDist, glm::length(pa - pb));
        if (glm::length(pa - glm::vec2(5.0f, 0.0f)) < 0.2f && glm::length(pb - glm::vec2(-5.0f, 0.0f)) < 0.2f) {
            break;
        }
    }
    CHECK(minDist >= 0.6f - 1e-3f);                      // combined radius
    CHECK(pa.x == doctest::Approx(5.0f).epsilon(0.05));  // both reached their goals
    CHECK(pb.x == doctest::Approx(-5.0f).epsilon(0.05));
}

TEST_CASE("ORCA separates exactly overlapping agents deterministically") {
    const std::array<orca::Neighbor, 1> n{orca::Neighbor{{0.0f, 0.0f}, {0.0f, 0.0f}, 0.3f, 5}};
    const auto v1 = orca::computeNewVelocity({{0, 0}, {0, 0}, {0, 0}, 0.3f, 1.5f, 2}, n, {}, {});
    const auto v2 = orca::computeNewVelocity({{0, 0}, {0, 0}, {0, 0}, 0.3f, 1.5f, 2}, n, {}, {});
    CHECK(glm::length(v1) > 0.1f);  // pushed apart
    CHECK(v1 == v2);
    CHECK(std::isfinite(v1.x));
    CHECK(std::isfinite(v1.y));
}

TEST_CASE("ORCA wall constraint stops an agent walking into a wall") {
    // Wall along x = 1; agent at the origin wants to walk +x at 1.5 m/s.
    const std::array<orca::Wall, 1> walls{orca::Wall{{1.0f, -5.0f}, {1.0f, 5.0f}}};
    const orca::Params params{2.0f, 1.0f / 30.0f, 0.5f};
    const auto v = orca::computeNewVelocity(agent({0, 0}, {0, 0}, {1.5f, 0.0f}), {}, walls, params);
    // May approach at most (distance - clearance) / obstacleTimeHorizon = (1 - 0.05) / 0.5 = 1.9 m/s,
    // so the 1.5 m/s preference is still allowed this far out.
    CHECK(v.x == doctest::Approx(1.5f));

    // Simulate: the agent must never get closer than its radius.
    glm::vec2 p{0.0f, 0.0f};
    for (int i = 0; i < 300; ++i) {
        const auto vel = orca::computeNewVelocity(agent(p, {0, 0}, {1.5f, 0.2f}), {}, walls, params);
        p += vel * (1.0f / 30.0f);
        REQUIRE(p.x <= 1.0f - 0.05f + 1e-3f);  // centre never closer than the clearance
    }
}
