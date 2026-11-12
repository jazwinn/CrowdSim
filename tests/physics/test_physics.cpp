#include <doctest/doctest.h>

#include "crowdsim/core/Logging.hpp"
#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/physics/PhysicsWorld.hpp"

using namespace crowdsim;

TEST_CASE("Jolt raycasts hit static floors at the right height, top floor first") {
    core::LogService logs(core::LogConfig{.console = false});
    physics::JoltRuntime runtime;
    auto world = physics::createJoltPhysicsWorld(runtime, logs.logger());

    geometry::TriangleMesh mesh;
    geometry::PrimitiveDesc ground;
    ground.position = {0.0f, -0.2f, 0.0f};
    ground.size = {20.0f, 0.2f, 20.0f};
    mesh.append(geometry::buildPrimitive(ground));
    geometry::PrimitiveDesc upper = ground;
    upper.position = {0.0f, 3.8f, 5.0f};
    upper.size = {20.0f, 0.2f, 10.0f};
    mesh.append(geometry::buildPrimitive(upper));

    REQUIRE(world->addStaticMesh(mesh).valid());
    CHECK(world->bodyCount() == 1);

    // Straight down over the upper floor: hits it at y = 4.
    auto hit = world->raycast({0.0f, 10.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    REQUIRE(hit);
    CHECK(hit->position.y == doctest::Approx(4.0f).epsilon(1e-3));
    CHECK(hit->distance == doctest::Approx(6.0f).epsilon(1e-3));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(1e-3));

    // Over the double-height half: hits the ground at y = 0.
    hit = world->raycast({0.0f, 10.0f, -5.0f}, {0.0f, -2.0f, 0.0f}, 100.0f);
    REQUIRE(hit);
    CHECK(hit->position.y == doctest::Approx(0.0f).epsilon(1e-3));

    // Too short, and pointing away: no hit.
    CHECK_FALSE(world->raycast({0.0f, 10.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, 5.0f));
    CHECK_FALSE(world->raycast({0.0f, 10.0f, -5.0f}, {0.0f, 1.0f, 0.0f}, 100.0f));

    world->clear();
    CHECK(world->bodyCount() == 0);
    CHECK_FALSE(world->raycast({0.0f, 10.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, 100.0f));
}
