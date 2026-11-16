// Bodies in packed crowds (BodyParams): contact forces add up along chains of people,
// pushing raises the pressure, slopes are measured, people topple and lie still, and
// it all stays deterministic. Off by default.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <optional>

#include <glm/geometric.hpp>

#include "crowdsim/assets/JsonTypes.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/sim/BehaviorTree.hpp"
#include "crowdsim/sim/Simulation.hpp"

using namespace crowdsim;

namespace {

struct Crowd {
    float frontPressure = 0.0f;  ///< highest pressure within 2 m of the dead end, N/m
    float backPressure = 0.0f;   ///< highest pressure on the last 2 m of the crowd, N/m
    float frontDensity = 0.0f;   ///< people per m2 within 2 m of the dead end
    std::uint64_t falls = 0;
    std::uint32_t fallen = 0;
    bool fallenStill = true;     ///< everyone lying down had zero velocity
    std::uint64_t hash = 0;
};

/// 60 people walk into a 2 m wide dead-end corridor and keep pressing towards its end.
Crowd deadEnd(std::optional<sim::BodyParams> bodies, float push, std::uint32_t threads = 4, int seconds = 30) {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{threads};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
    if (bodies) {
        sim.setBodyParams(*bodies);
    }
    geometry::PrimitiveDesc floor;
    floor.position = {0.0f, -0.2f, 6.0f};
    floor.size = {2.0f, 0.2f, 16.0f};  // z -2 .. 14
    sim.addStaticObject(sim::StaticObject{"corridor", floor, {}, {}, {}});

    sim::BtNodeDesc root{"Sequence", "root", {}, {}};
    sim::BtNodeDesc pushing{"SetPushing", "push", {}, {}};
    pushing.params["force"].number = push;
    sim::BtNodeDesc wait{"Wait", "stay", {}, {}};
    wait.params["seconds"].number = 1000.0f;
    root.children = {pushing, sim::BtNodeDesc{"MoveToGoal", "go", {}, {}}, wait};
    sim.setBehaviorTree(sim::compileBehaviorTree(root, "press"));
    sim::AgentType t;
    t.name = "presser";
    t.behaviour = "press";
    t.params.radiusMin = 0.22f;
    t.params.radiusMax = 0.24f;
    sim.defineAgentType(t);

    sim::Goal goal;
    goal.name = "end";
    goal.area = {{0.0f, 13.6f}, {0.8f, 0.3f}, 0.0f};
    const sim::SceneId goalId = sim.addGoal(goal);
    sim::Spawner s;
    s.name = "start";
    s.area = {{0.0f, -1.0f}, {0.7f, 0.7f}, 0.0f};
    s.rate = 4.0f;
    s.total = 60;
    s.goal = goalId;
    s.agentMix = {{"presser", 1.0f}};
    sim.addSpawner(s);

    Crowd out;
    for (int step = 0; step < 30 * seconds; ++step) {
        sim.step();
        const sim::AgentPool& a = sim.agents();
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            if (a.fallen[i] != 0 && glm::length(a.velocity[i]) > 0.0f) {
                out.fallenStill = false;
            }
        }
    }
    const sim::AgentPool& a = sim.agents();
    float back = 1e9f;
    for (std::uint32_t i = 0; i < a.size(); ++i) {
        back = std::min(back, a.position[i].y);
    }
    std::uint32_t front = 0;
    for (std::uint32_t i = 0; i < a.size(); ++i) {
        if (a.position[i].y > 12.0f) {
            out.frontPressure = std::max(out.frontPressure, a.pressure[i]);
            ++front;
        }
        if (a.position[i].y < back + 2.0f) {
            out.backPressure = std::max(out.backPressure, a.pressure[i]);
        }
        out.fallen += a.fallen[i];
    }
    out.frontDensity = static_cast<float>(front) / (2.0f * 2.0f);
    out.falls = sim.stats().falls;
    out.hash = sim.stateHash();
    return out;
}

sim::BodyParams noFalls() {
    sim::BodyParams b;
    b.enabled = true;
    b.fallForce = 1e9f;
    b.tripForce = 1e9f;
    return b;
}

}  // namespace

TEST_CASE("Bodies are off by default: no contact pressure, no falls") {
    const Crowd c = deadEnd(std::nullopt, 300.0f);
    CHECK(c.frontPressure == 0.0f);
    CHECK(c.falls == 0);
}

TEST_CASE("Pushing into a dead end packs people past what avoidance allows and loads the front") {
    const Crowd polite = deadEnd(std::nullopt, 300.0f);
    const Crowd packed = deadEnd(noFalls(), 300.0f);
    CAPTURE(polite.frontDensity);
    CAPTURE(packed.frontDensity);
    CAPTURE(packed.frontPressure);
    CAPTURE(packed.backPressure);
    // (Density over the corridor's full 2 m x 2 m end; the walkable part is narrower.)
    CHECK(packed.frontDensity > 2.0f * polite.frontDensity);
    // Forces add up along the chain: those against the end carry everyone behind them.
    CHECK(packed.frontPressure > 300.0f);
    CHECK(packed.frontPressure > 2.0f * packed.backPressure);
}

TEST_CASE("Harder pushing raises the pressure at the front") {
    const Crowd gentle = deadEnd(noFalls(), 40.0f);
    const Crowd hard = deadEnd(noFalls(), 400.0f);
    CAPTURE(gentle.frontPressure);
    CAPTURE(hard.frontPressure);
    CHECK(hard.frontPressure > 1.5f * gentle.frontPressure);
}

TEST_CASE("People topple under large net pushes, lie still, and not at all when they can stand it") {
    sim::BodyParams fragile;
    fragile.enabled = true;
    fragile.fallForce = 60.0f;
    fragile.fallRate = 2.0f;
    fragile.getUpSeconds = 1000.0f;
    const Crowd falling = deadEnd(fragile, 400.0f);
    CAPTURE(falling.falls);
    CHECK(falling.falls > 0);
    CHECK(falling.fallen > 0);
    CHECK(falling.fallenStill);

    const Crowd standing = deadEnd(noFalls(), 400.0f);
    CHECK(standing.falls == 0);
}

TEST_CASE("Bodies are deterministic across thread counts") {
    sim::BodyParams b;
    b.enabled = true;
    b.fallForce = 150.0f;  // with falls, which use each agent's own random numbers
    const Crowd one = deadEnd(b, 300.0f, 1, 20);
    const Crowd four = deadEnd(b, 300.0f, 4, 20);
    CHECK(one.hash == four.hash);
    CHECK(one.falls == four.falls);
}

TEST_CASE("The navmesh measures slopes; flat floors have none") {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{2};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
    geometry::PrimitiveDesc flat;
    flat.position = {0.0f, -0.2f, -6.0f};
    flat.size = {4.0f, 0.2f, 8.0f};
    sim.addStaticObject(sim::StaticObject{"flat", flat, {}, {}, {}});
    geometry::PrimitiveDesc ramp;
    ramp.kind = geometry::PrimitiveKind::Ramp;
    ramp.position = {0.0f, 0.0f, 8.0f};
    ramp.size = {4.0f, 2.0f, 20.0f};  // rises 10% towards +z
    sim.addStaticObject(sim::StaticObject{"ramp", ramp, {}, {}, {}});
    REQUIRE(sim.updateNavigation());
    const sim::NavMesh& nav = sim.navMesh();
    auto gradientAt = [&](glm::vec3 p) {
        const auto hit = nav.findNearest(p, {0.5f, 2.0f, 0.5f});
        REQUIRE(hit.has_value());
        return nav.polyGradient(nav.denseIndex(hit->ref));
    };
    const glm::vec2 onRamp = gradientAt({0.0f, 1.0f, 8.0f});
    const glm::vec2 onFlat = gradientAt({0.0f, 0.0f, -6.0f});
    CAPTURE(onRamp.x);
    CAPTURE(onRamp.y);
    CHECK(onRamp.y == doctest::Approx(0.1f).epsilon(0.25));
    CHECK(std::abs(onRamp.x) < 0.02f);
    CHECK(glm::length(onFlat) < 0.01f);
}

TEST_CASE("Body settings round-trip through JSON and reject nonsense") {
    sim::BodyParams b;
    b.enabled = true;
    b.stiffness = 7000.0f;
    b.core = 0.6f;
    b.fallForce = 900.0f;
    const sim::BodyParams back = assets::bodyParamsFromJson(assets::toJson(b));
    CHECK(back.enabled);
    CHECK(back.stiffness == doctest::Approx(7000.0f));
    CHECK(back.core == doctest::Approx(0.6f));
    CHECK(back.fallForce == doctest::Approx(900.0f));
    // A "bodies" section switches them on unless it says otherwise.
    CHECK(assets::bodyParamsFromJson(nlohmann::json::object()).enabled);
    CHECK_THROWS((void)assets::bodyParamsFromJson(nlohmann::json{{"core", 1.2}}));
    CHECK_THROWS((void)assets::bodyParamsFromJson(nlohmann::json{{"stiffness", 0}}));
}

TEST_CASE("SetPushing and IsBlocked compile and validate") {
    sim::BtNodeDesc push{"SetPushing", "p", {}, {}};
    push.params["force"].number = 250.0f;
    CHECK_NOTHROW((void)sim::compileBehaviorTree(push, "push"));
    sim::BtNodeDesc negative{"SetPushing", "p", {}, {}};
    negative.params["force"].number = -1.0f;
    CHECK_THROWS((void)sim::compileBehaviorTree(negative, "bad"));
    sim::BtNodeDesc blocked{"IsBlocked", "b", {}, {}};
    blocked.params["seconds"].number = 5.0f;
    CHECK_NOTHROW((void)sim::compileBehaviorTree(blocked, "blocked"));
}
