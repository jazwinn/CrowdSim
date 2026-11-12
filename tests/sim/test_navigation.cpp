#include <doctest/doctest.h>

#include <cmath>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/geometry/Primitives.hpp"
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
