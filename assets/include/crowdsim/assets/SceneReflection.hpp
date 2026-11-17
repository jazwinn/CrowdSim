#pragma once

/// @file SceneReflection.hpp
/// Reflection descriptors for the scene description types (see reflect/Reflect.hpp): the
/// one place that says how each is saved in *.scene.json (docs/formats.md), how it is
/// validated and how the editor shows it. Agent type files (*.agent.json) are described here too. Keys and units match the file format, which is
/// why some differ from the member names (rotationY is saved in degrees, flags as tags).

#include <stdexcept>
#include <string>
#include <vector>

#include "crowdsim/geometry/Primitives.hpp"
#include "crowdsim/reflect/Reflect.hpp"
#include "crowdsim/sim/Environment.hpp"
#include "crowdsim/sim/NavMesh.hpp"
#include "crowdsim/sim/Simulation.hpp"

CROWDSIM_REFLECT(crowdsim::sim::Area,
    field("center", &T::center).required().unit("m").tip("Centre of the area on the floor (x, z)"),
    field("halfExtents", &T::halfExtents).required().above(0).unit("m").tip("Half its width (x) and depth (z)"),
    field("elevation", &T::elevation).unit("m").tip("Height of the floor the area lies on"))

CROWDSIM_REFLECT(crowdsim::sim::Goal,
    field("name", &T::name),
    field("area", &T::area).required(),
    field("capacity", &T::capacity).tip("People served at once (Queue node); 0 = unlimited"),
    field("serviceTime", &T::serviceTime).atLeast(0).unit("s").tip("How long each person is served"),
    field("queueDirection", &T::queueDirection).tip("Direction the waiting line extends from the area (x, z)"),
    field("queueSpacing", &T::queueSpacing).above(0.1).unit("m").tip("Space between people in the line"))

CROWDSIM_REFLECT(crowdsim::sim::OffMeshLink,
    field("start", &T::start).required().unit("m"),
    field("end", &T::end).required().unit("m"),
    field("radius", &T::radius).above(0).unit("m").tip("How close to an end people must be to take the link"),
    field("bidirectional", &T::bidirectional).tip("Usable both ways (off: start to end only)"),
    field("extraCost", &T::extraCost).atLeast(0).unit("m").tip(
        "Extra path cost, so routes prefer the link only when it saves a real detour"))

CROWDSIM_REFLECT(crowdsim::sim::AgentParams,
    range("radius", &T::radiusMin, &T::radiusMax).above(0).unit("m").tip("Body radius, drawn per person"),
    range("preferredSpeed", &T::preferredSpeedMin, &T::preferredSpeedMax).above(0).unit("m/s").label("Speed").tip(
        "Walking speed, drawn per person"),
    field("maxSpeedFactor", &T::maxSpeedFactor).above(0).label("Top speed x").tip("Top speed = preferred speed x this"),
    range("mass", &T::massMin, &T::massMax).above(0).unit("kg").tip(
        "Body mass: heavier people give way less in avoidance and contact"),
    field("maxAcceleration", &T::maxAcceleration).above(0).unit("m/s2").label("Acceleration"),
    field("color", &T::color).color(),
    field("colorJitter", &T::colorJitter).between(0, 1).tip("Random variation of the colour per person"),
    field("agentType", &T::agentType).widget("agentType").tip("Agent type (behaviour) when the spawner has no agent mix"))

CROWDSIM_REFLECT(crowdsim::sim::AgentMixEntry,
    field("type", &T::type).required().widget("agentTypeName"),
    field("weight", &T::weight).atLeast(0).tip("Share of the spawned people, relative to the other entries"))

CROWDSIM_REFLECT(crowdsim::sim::Spawner,
    field("name", &T::name),
    field("area", &T::area).required(),
    field("rate", &T::rate).atLeast(0).unit("/s").tip("People spawned per second"),
    field("total", &T::total).tip("People spawned in all; 0 = no limit"),
    field("startTime", &T::startTime).atLeast(0).unit("s"),
    field("stopTime", &T::stopTime).infinityAsNull().unit("s").tip("When spawning stops; not set = never"),
    field("goal", &T::goal).widget("goal").tip("Goal the spawned people walk to"),
    field("agent", &T::agent).tip("Body parameters when the agent mix is empty"),
    field("agentMix", &T::agentMix).widget("agentMix").tip("Agent types to spawn, by weight"),
    range("groupSize", &T::groupSizeMin, &T::groupSizeMax).between(1, 12).tip(
        "People spawned together as a group that walks together"))

CROWDSIM_REFLECT(crowdsim::sim::NavBuildSettings,
    field("cellSize", &T::cellSize).above(0.01).unit("m").tip("Voxel size across: smaller is finer and slower to build"),
    field("cellHeight", &T::cellHeight).above(0.01).unit("m"),
    field("agentHeight", &T::agentHeight).above(0).unit("m"),
    field("agentRadius", &T::agentRadius).atLeast(0).unit("m").tip("Walkable space is shrunk by this much from walls"),
    field("agentMaxClimb", &T::agentMaxClimb).atLeast(0).unit("m").tip("Highest step people walk up"),
    field("agentMaxSlope", &T::agentMaxSlopeDegrees).between(0, 90).unit("deg"),
    field("tileSize", &T::tileSizeCells).atLeast(16).tip("Navmesh tile size in cells"))

CROWDSIM_REFLECT(crowdsim::sim::BodyParams,
    field("enabled", &T::enabled),
    field("stiffness", &T::stiffness).above(0).unit("N/m").tip("Force per metre two bodies are pressed into each other"),
    field("damping", &T::damping).atLeast(0).unit("N s/m"),
    field("core", &T::core).atLeast(0).tip("Incompressible share of the radius (below 1)"),
    field("slopeLean", &T::slopeLean).atLeast(0).tip("Share of the downhill weight packed people pass on"),
    field("fallForce", &T::fallForce).atLeast(0).unit("N").tip("Net push above which standing people may topple"),
    field("fallRate", &T::fallRate).atLeast(0).unit("/s").tip("Falls per second at twice the fall force"),
    field("tripForce", &T::tripForce).atLeast(0).unit("N").tip("Push towards someone lying in the way that trips people"),
    field("tripRate", &T::tripRate).atLeast(0).unit("/s"),
    field("getUpSeconds", &T::getUpSeconds).atLeast(0).unit("s").tip("Shortest time on the ground"),
    field("getUpPressure", &T::getUpPressure).atLeast(0).unit("N/m").tip("Fallen people get up only below this pressure"),
    check<T>([](const T& b) {
        if (b.core >= 1.0f) {
            throw crowdsim::reflect::FieldError("core", "must be below 1");
        }
    }))

// An agent type file (*.agent.json) holds the body parameters inline, beside the type's own
// fields; `agentType` (a spawner's default type) means nothing there.
CROWDSIM_REFLECT(crowdsim::sim::AgentType,
    field("name", &T::name).tip("Referred to by spawners' agent mixes"),
    field("behaviour", &T::behaviour).widget("behaviour").tip("Behaviour tree the agents run"),
    field("params", &T::params).flatten("agentType"),
    field("personalSpace", &T::personalSpace).atLeast(0).unit("m").tip("Space kept around the body in avoidance"),
    field("height", &T::height).above(0.5).unit("m").tip("Body height (drawing, eye height)"),
    field("tags", &T::tags).tip("Free tags for scripts and metrics"),
    check<T>([](const T& t) {
        if (t.name.empty()) {
            throw crowdsim::reflect::FieldError("name", "an agent type needs a name");
        }
    }))

CROWDSIM_REFLECT(crowdsim::sim::Group,
    field("name", &T::name))

namespace crowdsim::assets::detail {

/// Surface flags and free tags are saved together as one list of tags.
inline std::vector<std::string> objectTags(const sim::StaticObject& o) {
    std::vector<std::string> tags;
    if ((o.primitive.flags & geometry::kSurfaceWalkable) != 0) {
        tags.emplace_back("walkable");
    }
    if ((o.primitive.flags & geometry::kSurfaceObstacle) != 0) {
        tags.emplace_back("obstacle");
    }
    tags.insert(tags.end(), o.tags.begin(), o.tags.end());
    return tags;
}

inline void setObjectTags(sim::StaticObject& o, const std::vector<std::string>& tags) {
    o.primitive.flags = 0;
    o.tags.clear();
    for (const std::string& t : tags) {
        if (t == "walkable") {
            o.primitive.flags |= geometry::kSurfaceWalkable;
        } else if (t == "obstacle") {
            o.primitive.flags |= geometry::kSurfaceObstacle;
        } else {
            o.tags.push_back(t);
        }
    }
}

inline bool isMesh(const sim::StaticObject& o) {
    return o.primitive.kind == geometry::PrimitiveKind::Mesh;
}

}  // namespace crowdsim::assets::detail

CROWDSIM_REFLECT(crowdsim::sim::StaticObject,
    field("name", &T::name),
    property<T>(
        "type", [](const T& o) { return o.primitive.kind; },
        [](T& o, crowdsim::geometry::PrimitiveKind k) { o.primitive.kind = k; })
        .lowercase()
        .tip("Box, ramp, stairs, plane, or an imported mesh"),
    property<T>(
        "position", [](const T& o) { return o.primitive.position; },
        [](T& o, glm::vec3 p) { o.primitive.position = p; })
        .unit("m")
        .tip("Centre of the footprint, at the bottom"),
    property<T>(
        "rotationY", [](const T& o) { return o.primitive.rotationY; },
        [](T& o, float r) { o.primitive.rotationY = r; })
        .degrees()
        .speed(1.0)
        .tip("Turn about the vertical axis"),
    field("asset", &T::meshAsset).when(crowdsim::assets::detail::isMesh).hidden(),
    property<T>(
        "scale", [](const T& o) { return o.primitive.size.x; },
        [](T& o, float s) { o.primitive.size = glm::vec3(s); })
        .above(0)
        .when(crowdsim::assets::detail::isMesh),
    property<T>(
        "size", [](const T& o) { return o.primitive.size; }, [](T& o, glm::vec3 s) { o.primitive.size = s; })
        .atLeast(0)
        .unit("m")
        .tip("Width (x), height (y) and depth (z)")
        .when([](const T& o) { return !crowdsim::assets::detail::isMesh(o); }),
    property<T>(
        "steps", [](const T& o) { return o.primitive.steps; }, [](T& o, std::uint32_t n) { o.primitive.steps = n; })
        .atLeast(1)
        .when([](const T& o) { return o.primitive.kind == crowdsim::geometry::PrimitiveKind::Stairs; }),
    property<T>(
        "color", [](const T& o) { return o.primitive.color; }, [](T& o, std::uint32_t c) { o.primitive.color = c; })
        .color()
        .when([](const T& o) { return !crowdsim::assets::detail::isMesh(o); }),
    property<T>("tags", crowdsim::assets::detail::objectTags, crowdsim::assets::detail::setObjectTags)
        .tip("walkable, obstacle, and tags that configure mechanics or metrics (escalator, emergency, attention...)"),
    check<T>([](const T& o) {
        if (crowdsim::assets::detail::isMesh(o)) {
            if (o.meshAsset.empty()) {
                throw crowdsim::reflect::FieldError("asset", "a mesh object needs an asset path");
            }
        } else if (o.primitive.size.x <= 0.0f || o.primitive.size.z <= 0.0f) {
            throw crowdsim::reflect::FieldError("size", "width and depth must be above 0");
        }
    }))
