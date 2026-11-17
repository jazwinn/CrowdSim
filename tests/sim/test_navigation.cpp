#include <doctest/doctest.h>

#include <cmath>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/sim/BehaviorTree.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/sim/Simulation.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

using namespace crowdsim;

namespace {

struct World {
    core::LogService logs{core::LogConfig{.console = false}};
    core::JobSystem jobs{4};
    sim::Simulation sim{{logs.logger(), jobs}, sim::SimConfig{}};
};

}  // namespace

TEST_CASE("Primitives produce outward-facing, flagged triangles") {
    geometry::PrimitiveDesc box;
    box.size = {2.0f, 1.0f, 3.0f};
    const geometry::TriangleMesh m = geometry::buildPrimitive(box);
    CHECK(m.triangleCount() == 12);
    CHECK(m.triangleFlags.size() == 12);
    const geometry::Bounds b = m.bounds();
    CHECK(b.min.y == doctest::Approx(0.0f));
    CHECK(b.max.y == doctest::Approx(1.0f));
    CHECK(b.max.x == doctest::Approx(1.0f));
    CHECK(b.max.z == doctest::Approx(1.5f));

    geometry::PrimitiveDesc stairs;
    stairs.kind = geometry::PrimitiveKind::Stairs;
    stairs.size = {2.0f, 2.0f, 4.0f};
    stairs.steps = 10;
    const auto s = geometry::buildPrimitive(stairs);
    CHECK(s.bounds().max.y == doctest::Approx(2.0f));
}

TEST_CASE("Navmesh is built for the building and connects both floors") {
    World w;
    sim::buildScenario(w.sim, sim::ScenarioKind::Building, 10);
    REQUIRE(w.sim.updateNavigation());
    const sim::NavMesh& nav = w.sim.navMesh();
    REQUIRE(nav.valid());
    CHECK(nav.polyCount() > 50);
    CHECK(!nav.walls().empty());

    // Ground floor and upper floor points are both on the mesh.
    const auto ground = nav.findNearest({0.0f, 0.0f, -16.0f}, {0.5f, 1.0f, 0.5f});
    const auto upper = nav.findNearest({0.0f, 4.0f, 17.0f}, {0.5f, 1.0f, 0.5f});
    REQUIRE(ground);
    REQUIRE(upper);
    CHECK(ground->point.y == doctest::Approx(0.0f).epsilon(0.2));
    CHECK(upper->point.y == doctest::Approx(4.0f).epsilon(0.05));
    // Nothing walkable inside a pillar (0.6 m wide at x = -10, z = 5): the nearest
    // walkable point is at least half the pillar width plus the agent radius away.
    const auto pillar = nav.findNearest({-10.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f});
    REQUIRE(pillar);
    CHECK(std::hypot(pillar->point.x + 10.0f, pillar->point.z - 5.0f) > 0.5f);

    // The goal (upper floor) is reachable from the ground floor via the flow field.
    const sim::FlowField& ff = w.sim.flowFields().at(0);
    CHECK(ff.reachable(nav.denseIndex(ground->ref)));
    CHECK(ff.cost[nav.denseIndex(ground->ref)] > 20.0f);
}

TEST_CASE("Agents stay on the navmesh (no wall penetration) and all reach the upper floor") {
    World w;
    sim::buildScenario(w.sim, sim::ScenarioKind::Building, 400);
    int checks = 0;
    for (int step = 0; step < 30 * 240; ++step) {
        w.sim.step();
        if (step % 15 != 0) {
            continue;
        }
        const sim::AgentPool& a = w.sim.agents();
        for (std::uint32_t i = 0; i < a.size(); ++i) {
            if (a.linkActive[i] != 0) {
                continue;  // on the ladder: between polygons by design
            }
            const glm::vec3 p(a.position[i].x, a.height[i], a.position[i].y);
            const auto closest = w.sim.navMesh().closestPointOnPoly(a.navPoly[i], p);
            REQUIRE(closest.has_value());
            const float dx = closest->x - p.x;
            const float dz = closest->z - p.z;
            REQUIRE(std::sqrt(dx * dx + dz * dz) < 0.01f);
            REQUIRE(std::abs(closest->y - p.y) < 0.25f);
            ++checks;
        }
    }
    CHECK(checks > 1000);
    CHECK(w.sim.stats().spawned == 400);
    CHECK(w.sim.stats().arrived == 400);
}

TEST_CASE("Static scene snapshot carries meshes, navmesh and flow arrows") {
    World w;
    sim::buildScenario(w.sim, sim::ScenarioKind::Building, 10);
    w.sim.step();
    snapshot::RenderSnapshot snap;
    w.sim.writeSnapshot(snap);
    REQUIRE(snap.staticScene);
    CHECK(snap.staticScene->version == w.sim.staticVersion());
    CHECK(!snap.staticScene->meshVertices.empty());
    CHECK(snap.staticScene->meshVertices.size() % 3 == 0);
    CHECK(!snap.staticScene->navTriangles.empty());
    CHECK(snap.staticScene->offMeshLinks.size() == 2);
    REQUIRE(snap.staticScene->flowArrows.size() == 1);
    CHECK(!snap.staticScene->flowArrows[0].empty());

    // Unchanged scene: the same immutable object is shared by the next snapshot.
    snapshot::RenderSnapshot snap2;
    w.sim.step();
    w.sim.writeSnapshot(snap2);
    CHECK(snap2.staticScene.get() == snap.staticScene.get());
}

TEST_CASE("Emergency routes: only agents told to use them route through them") {
    // A hall with two ways out: a short fire exit corridor tagged "emergency", and a long
    // everyday corridor round the north side.
    struct Result {
        std::uint32_t arrived = 0;
        bool usedFireExit = false;
    };
    auto run = [](bool useEmergency) {
        World w;
        auto floor = [&](float x0, float x1, float z0, float z1, std::vector<std::string> tags) {
            geometry::PrimitiveDesc d;
            d.position = {(x0 + x1) / 2, -0.2f, (z0 + z1) / 2};
            d.size = {x1 - x0, 0.2f, z1 - z0};
            w.sim.addStaticObject(sim::StaticObject{"floor", d, {}, {}, std::move(tags)});
        };
        floor(-10.0f, 10.0f, -5.0f, 5.0f, {});            // hall
        floor(10.0f, 20.0f, -1.0f, 1.0f, {"emergency"});  // fire exit corridor
        floor(-10.0f, 26.0f, 5.0f, 8.0f, {});             // everyday corridor
        floor(20.0f, 26.0f, -5.0f, 5.0f, {});             // outside

        sim::BtNodeDesc use{"UseEmergencyRoutes", "use", {}, {}};
        use.params["use"].number = useEmergency ? 1.0f : 0.0f;
        sim::BtNodeDesc root{
            "Sequence",
            "root",
            {},
            {use, sim::BtNodeDesc{"MoveToGoal", "go", {}, {}}, sim::BtNodeDesc{"Despawn", "leave", {}, {}}}};
        w.sim.setBehaviorTree(sim::compileBehaviorTree(root, "leaver"));
        sim::AgentType t;
        t.name = "leaver";
        t.behaviour = "leaver";
        w.sim.defineAgentType(t);
        sim::Goal goal;
        goal.name = "outside";
        goal.area = {{23.0f, -2.0f}, {2.0f, 2.0f}, 0.0f};
        const sim::SceneId goalId = w.sim.addGoal(goal);
        sim::Spawner s;
        s.name = "hall";
        s.area = {{-5.0f, 0.0f}, {2.0f, 2.0f}, 0.0f};
        s.rate = 5.0f;
        s.total = 5;
        s.goal = goalId;
        s.agentMix = {{"leaver", 1.0f}};
        w.sim.addSpawner(s);
        REQUIRE(w.sim.updateNavigation());
        REQUIRE(w.sim.emergencyFlowFields().size() == 1);
        Result r;
        for (int step = 0; step < 30 * 90; ++step) {
            w.sim.step();
            const auto& a = w.sim.agents();
            for (std::uint32_t i = 0; i < a.size(); ++i) {
                r.usedFireExit = r.usedFireExit || (a.position[i].x > 12.0f && a.position[i].x < 18.0f &&
                                                    std::abs(a.position[i].y) < 1.5f);
            }
        }
        r.arrived = static_cast<std::uint32_t>(w.sim.stats().arrived);
        return r;
    };
    const Result evacuees = run(true);
    CHECK(evacuees.arrived == 5);
    CHECK(evacuees.usedFireExit);
    const Result shoppers = run(false);
    CHECK(shoppers.arrived == 5);
    CHECK_FALSE(shoppers.usedFireExit);

    sim::BtNodeDesc negative{"UseEmergencyRoutes", "use", {}, {}};
    negative.params["use"].number = -1.0f;
    CHECK_THROWS((void)sim::compileBehaviorTree(negative, "bad"));
}

TEST_CASE("MoveToGoal with no goal named goes to the spawner's goal, after visiting others") {
    World w;
    geometry::PrimitiveDesc floor;
    floor.position = {0.0f, -0.2f, 0.0f};
    floor.size = {30.0f, 0.2f, 10.0f};
    w.sim.addStaticObject(sim::StaticObject{"floor", floor, {}, {}, {}});
    sim::BtNodeDesc visit{"MoveToGoal", "visit", {}, {}};
    visit.params["goal"].kind = sim::BtParamKind::Text;
    visit.params["goal"].text = "Shop";
    sim::BtNodeDesc root{
        "Sequence",
        "root",
        {},
        {visit, sim::BtNodeDesc{"MoveToGoal", "home", {}, {}}, sim::BtNodeDesc{"Despawn", "leave", {}, {}}}};
    w.sim.setBehaviorTree(sim::compileBehaviorTree(root, "visitor"));
    sim::AgentType t;
    t.name = "visitor";
    t.behaviour = "visitor";
    w.sim.defineAgentType(t);
    sim::Goal shop;
    shop.name = "Shop";
    shop.area = {{0.0f, 0.0f}, {1.0f, 1.0f}, 0.0f};
    (void)w.sim.addGoal(shop);
    sim::Goal exit;
    exit.name = "Exit";
    exit.area = {{12.0f, 0.0f}, {1.0f, 3.0f}, 0.0f};
    const sim::SceneId exitId = w.sim.addGoal(exit);
    sim::Spawner s;
    s.name = "in";
    s.area = {{-12.0f, 0.0f}, {0.5f, 0.5f}, 0.0f};
    s.rate = 5.0f;
    s.total = 1;
    s.goal = exitId;
    s.agentMix = {{"visitor", 1.0f}};
    w.sim.addSpawner(s);
    float furthest = -100.0f;
    for (int step = 0; step < 30 * 40 && (w.sim.stats().spawned == 0 || w.sim.agents().size() > 0); ++step) {
        w.sim.step();
        if (w.sim.agents().size() == 1) {
            furthest = std::max(furthest, w.sim.agents().position[0].x);
        }
    }
    CHECK(w.sim.agents().size() == 0);
    CHECK(furthest > 10.0f);  // left the shop for the exit before despawning
}
