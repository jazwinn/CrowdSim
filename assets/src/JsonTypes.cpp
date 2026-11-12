#include "crowdsim/assets/JsonTypes.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <stdexcept>

#include "crowdsim/assets/AssetLibrary.hpp"

namespace crowdsim::assets {

namespace {

constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kRadToDeg = 57.29577951308232f;

Json vec(glm::vec3 v) {
    return Json::array({roundFloat(v.x), roundFloat(v.y), roundFloat(v.z)});
}
Json vec(glm::vec2 v) {
    return Json::array({roundFloat(v.x), roundFloat(v.y)});
}

glm::vec3 vec3From(const Json& j) {
    if (!j.is_array() || j.size() != 3) {
        throw std::invalid_argument("expected [x, y, z], got " + j.dump());
    }
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

glm::vec2 vec2From(const Json& j) {
    if (!j.is_array() || j.size() != 2) {
        throw std::invalid_argument("expected [x, z], got " + j.dump());
    }
    return {j[0].get<float>(), j[1].get<float>()};
}

std::string colorString(std::uint32_t abgr) {
    return std::format("#{:02x}{:02x}{:02x}", abgr & 0xffu, (abgr >> 8) & 0xffu, (abgr >> 16) & 0xffu);
}

std::uint32_t colorFrom(const Json& j) {
    const std::string s = j.get<std::string>();
    std::uint32_t rgb = 0;
    const char* first = s.data() + 1;
    const char* last = s.data() + s.size();
    const auto [ptr, ec] = s.size() == 7 && s[0] == '#' ? std::from_chars(first, last, rgb, 16)
                                                        : std::from_chars_result{first, std::errc::invalid_argument};
    if (ec != std::errc{} || ptr != last) {
        throw std::invalid_argument("expected colour \"#rrggbb\", got \"" + s + "\"");
    }
    const std::uint32_t r = (rgb >> 16) & 0xffu;
    const std::uint32_t g = (rgb >> 8) & 0xffu;
    const std::uint32_t b = rgb & 0xffu;
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

Json rgb(glm::vec3 c) {
    auto ch = [](float v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return std::format("#{:02x}{:02x}{:02x}", ch(c.r), ch(c.g), ch(c.b));
}

glm::vec3 rgbFrom(const Json& j) {
    const std::uint32_t abgr = colorFrom(j);
    return {static_cast<float>(abgr & 0xffu) / 255.0f, static_cast<float>((abgr >> 8) & 0xffu) / 255.0f,
            static_cast<float>((abgr >> 16) & 0xffu) / 255.0f};
}

Json flagsToTags(std::uint8_t flags) {
    Json tags = Json::array();
    if ((flags & geometry::kSurfaceWalkable) != 0) {
        tags.push_back("walkable");
    }
    if ((flags & geometry::kSurfaceObstacle) != 0) {
        tags.push_back("obstacle");
    }
    return tags;
}

std::uint8_t tagsToFlags(const Json& tags) {
    std::uint8_t flags = 0;
    for (const auto& t : tags) {
        const std::string tag = t.get<std::string>();
        if (tag == "walkable") {
            flags |= geometry::kSurfaceWalkable;
        } else if (tag == "obstacle") {
            flags |= geometry::kSurfaceObstacle;
        }
        // Other tags (area:<name>, stairs, door...) are kept for scripts/metrics in later milestones.
    }
    return flags;
}

template <typename T>
T value(const Json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    return it == j.end() || it->is_null() ? fallback : it->get<T>();
}

/// Spawners may name their goal ("goal": "Exit") instead of giving its id.
void resolveGoalReference(const sim::Simulation& sim, Json& spawner) {
    if (!spawner.contains("goal") || !spawner["goal"].is_string()) {
        return;
    }
    const std::string name = spawner["goal"].get<std::string>();
    for (sim::SceneId id : sim.goalIds()) {
        if (sim.findGoal(id)->name == name) {
            spawner["goal"] = id;
            return;
        }
    }
    throw std::invalid_argument("no goal named '" + name + "'");
}

}  // namespace

double roundFloat(float v) {
    if (!std::isfinite(v)) {
        return static_cast<double>(v);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return std::strtod(buf, nullptr);
}

const char* entityKindName(EntityKind kind) {
    switch (kind) {
        case EntityKind::Object:
            return "object";
        case EntityKind::Link:
            return "link";
        case EntityKind::Goal:
            return "goal";
        case EntityKind::Spawner:
            return "spawner";
        case EntityKind::None:
            return "none";
    }
    return "none";
}

EntityKind entityKindOf(const sim::Simulation& sim, sim::SceneId id) {
    if (sim.findStaticObject(id) != nullptr)
        return EntityKind::Object;
    if (sim.findOffMeshLink(id) != nullptr)
        return EntityKind::Link;
    if (sim.findGoal(id) != nullptr)
        return EntityKind::Goal;
    if (sim.findSpawner(id) != nullptr)
        return EntityKind::Spawner;
    return EntityKind::None;
}

Json toJson(const sim::StaticObject& o) {
    const geometry::PrimitiveDesc& p = o.primitive;
    Json j;
    j["name"] = o.name;
    j["type"] = std::string(geometry::primitiveKindName(p.kind));
    j["position"] = vec(p.position);
    j["rotationY"] = roundFloat(p.rotationY * kRadToDeg);
    if (p.kind == geometry::PrimitiveKind::Mesh) {
        j["asset"] = o.meshAsset;
        j["scale"] = roundFloat(p.size.x);
    } else {
        j["size"] = vec(p.size);
        if (p.kind == geometry::PrimitiveKind::Stairs) {
            j["steps"] = p.steps;
        }
        j["color"] = colorString(p.color);
    }
    j["tags"] = flagsToTags(p.flags);
    return j;
}

sim::StaticObject staticObjectFromJson(const Json& j, AssetLibrary* assets) {
    sim::StaticObject o;
    o.name = value<std::string>(j, "name", "Object");
    const std::string type = value<std::string>(j, "type", "box");
    const auto kind = geometry::parsePrimitiveKind(type);
    if (!kind) {
        throw std::invalid_argument("unknown object type '" + type + "' (box, ramp, stairs, plane, mesh)");
    }
    geometry::PrimitiveDesc& p = o.primitive;
    p.kind = *kind;
    p.position = j.contains("position") ? vec3From(j["position"]) : glm::vec3(0.0f);
    p.rotationY = value<float>(j, "rotationY", 0.0f) * kDegToRad;
    if (p.kind == geometry::PrimitiveKind::Mesh) {
        o.meshAsset = value<std::string>(j, "asset", "");
        if (o.meshAsset.empty()) {
            throw std::invalid_argument("mesh object needs an \"asset\" path");
        }
        const float scale = value<float>(j, "scale", 1.0f);
        p.size = glm::vec3(scale);
        if (assets == nullptr) {
            throw std::invalid_argument("mesh objects need an asset library");
        }
        o.mesh = assets->loadMesh(o.meshAsset).mesh;
    } else {
        p.size = j.contains("size") ? vec3From(j["size"]) : glm::vec3(1.0f);
        if (p.size.x <= 0.0f || p.size.z <= 0.0f || p.size.y < 0.0f) {
            throw std::invalid_argument("object size must be positive");
        }
        p.steps = value<std::uint32_t>(j, "steps", 10);
        if (j.contains("color")) {
            p.color = colorFrom(j["color"]);
        }
    }
    p.flags = j.contains("tags") ? tagsToFlags(j["tags"]) : geometry::kSurfaceWalkable;
    return o;
}

Json toJson(const sim::OffMeshLink& l) {
    return Json{{"start", vec(l.start)},
                {"end", vec(l.end)},
                {"radius", roundFloat(l.radius)},
                {"bidirectional", l.bidirectional},
                {"extraCost", roundFloat(l.extraCost)}};
}

sim::OffMeshLink offMeshLinkFromJson(const Json& j) {
    sim::OffMeshLink l;
    l.start = vec3From(j.at("start"));
    l.end = vec3From(j.at("end"));
    l.radius = value<float>(j, "radius", 0.6f);
    l.bidirectional = value<bool>(j, "bidirectional", true);
    l.extraCost = value<float>(j, "extraCost", 25.0f);
    return l;
}

Json toJson(const sim::Area& a) {
    return Json{{"center", vec(a.center)}, {"halfExtents", vec(a.halfExtents)}, {"elevation", roundFloat(a.elevation)}};
}

sim::Area areaFromJson(const Json& j) {
    sim::Area a;
    a.center = vec2From(j.at("center"));
    a.halfExtents = vec2From(j.at("halfExtents"));
    a.elevation = value<float>(j, "elevation", 0.0f);
    if (a.halfExtents.x <= 0.0f || a.halfExtents.y <= 0.0f) {
        throw std::invalid_argument("area halfExtents must be positive");
    }
    return a;
}

Json toJson(const sim::Goal& g) {
    return Json{{"name", g.name}, {"area", toJson(g.area)}};
}

sim::Goal goalFromJson(const Json& j) {
    sim::Goal g;
    g.name = value<std::string>(j, "name", "Goal");
    g.area = areaFromJson(j.at("area"));
    return g;
}

Json toJson(const sim::AgentParams& p) {
    return Json{{"radius", Json::array({roundFloat(p.radiusMin), roundFloat(p.radiusMax)})},
                {"preferredSpeed", Json::array({roundFloat(p.preferredSpeedMin), roundFloat(p.preferredSpeedMax)})},
                {"maxSpeedFactor", roundFloat(p.maxSpeedFactor)},
                {"maxAcceleration", roundFloat(p.maxAcceleration)},
                {"color", rgb(p.color)},
                {"colorJitter", roundFloat(p.colorJitter)},
                {"agentType", p.agentType}};
}

sim::AgentParams agentParamsFromJson(const Json& j) {
    sim::AgentParams p;
    if (j.contains("radius")) {
        const glm::vec2 r = vec2From(j["radius"]);
        p.radiusMin = r.x;
        p.radiusMax = r.y;
    }
    if (j.contains("preferredSpeed")) {
        const glm::vec2 s = vec2From(j["preferredSpeed"]);
        p.preferredSpeedMin = s.x;
        p.preferredSpeedMax = s.y;
    }
    p.maxSpeedFactor = value<float>(j, "maxSpeedFactor", p.maxSpeedFactor);
    p.maxAcceleration = value<float>(j, "maxAcceleration", p.maxAcceleration);
    if (j.contains("color")) {
        p.color = rgbFrom(j["color"]);
    }
    p.colorJitter = value<float>(j, "colorJitter", p.colorJitter);
    p.agentType = value<std::uint16_t>(j, "agentType", p.agentType);
    if (p.radiusMin <= 0.0f || p.radiusMax < p.radiusMin || p.preferredSpeedMin <= 0.0f ||
        p.preferredSpeedMax < p.preferredSpeedMin) {
        throw std::invalid_argument("agent radius/preferredSpeed ranges must be positive and ordered [min, max]");
    }
    return p;
}

Json toJson(const sim::Spawner& s) {
    Json j{{"name", s.name},
           {"area", toJson(s.area)},
           {"rate", roundFloat(s.rate)},
           {"total", s.total},
           {"startTime", roundFloat(s.startTime)},
           {"goal", s.goal},
           {"agent", toJson(s.agent)}};
    j["stopTime"] = std::isfinite(s.stopTime) ? Json(roundFloat(s.stopTime)) : Json(nullptr);
    return j;
}

sim::Spawner spawnerFromJson(const Json& j) {
    sim::Spawner s;
    s.name = value<std::string>(j, "name", "Spawner");
    s.area = areaFromJson(j.at("area"));
    s.rate = value<float>(j, "rate", 10.0f);
    s.total = value<std::uint32_t>(j, "total", 0);
    s.startTime = value<float>(j, "startTime", 0.0f);
    s.stopTime = value<float>(j, "stopTime", std::numeric_limits<float>::infinity());
    s.goal = value<sim::SceneId>(j, "goal", sim::kNoSceneId);
    if (j.contains("agent")) {
        s.agent = agentParamsFromJson(j["agent"]);
    }
    if (s.rate < 0.0f) {
        throw std::invalid_argument("spawner rate must be >= 0");
    }
    return s;
}

Json toJson(const sim::NavBuildSettings& n) {
    return Json{{"cellSize", roundFloat(n.cellSize)},
                {"cellHeight", roundFloat(n.cellHeight)},
                {"agentHeight", roundFloat(n.agentHeight)},
                {"agentRadius", roundFloat(n.agentRadius)},
                {"agentMaxClimb", roundFloat(n.agentMaxClimb)},
                {"agentMaxSlope", roundFloat(n.agentMaxSlopeDegrees)},
                {"tileSize", n.tileSizeCells}};
}

sim::NavBuildSettings navSettingsFromJson(const Json& j) {
    sim::NavBuildSettings n;
    n.cellSize = value<float>(j, "cellSize", n.cellSize);
    n.cellHeight = value<float>(j, "cellHeight", n.cellHeight);
    n.agentHeight = value<float>(j, "agentHeight", n.agentHeight);
    n.agentRadius = value<float>(j, "agentRadius", n.agentRadius);
    n.agentMaxClimb = value<float>(j, "agentMaxClimb", n.agentMaxClimb);
    n.agentMaxSlopeDegrees = value<float>(j, "agentMaxSlope", n.agentMaxSlopeDegrees);
    n.tileSizeCells = value<int>(j, "tileSize", n.tileSizeCells);
    if (n.cellSize <= 0.01f || n.cellHeight <= 0.01f || n.tileSizeCells < 16) {
        throw std::invalid_argument("navigation cellSize/cellHeight too small or tileSize < 16");
    }
    return n;
}

Json entityToJson(const sim::Simulation& sim, sim::SceneId id) {
    Json j;
    switch (entityKindOf(sim, id)) {
        case EntityKind::Object:
            j = toJson(*sim.findStaticObject(id));
            break;
        case EntityKind::Link:
            j = toJson(*sim.findOffMeshLink(id));
            break;
        case EntityKind::Goal:
            j = toJson(*sim.findGoal(id));
            break;
        case EntityKind::Spawner:
            j = toJson(*sim.findSpawner(id));
            break;
        case EntityKind::None:
            return nullptr;
    }
    Json out{{"kind", entityKindName(entityKindOf(sim, id))}, {"id", id}};
    out.update(j);
    return out;
}

sim::SceneId addEntityFromJson(sim::Simulation& sim, const Json& j, AssetLibrary* assets) {
    const std::string kind = value<std::string>(j, "kind", "object");
    const sim::SceneId id = value<sim::SceneId>(j, "id", sim::kNoSceneId);
    sim::SceneId created = sim::kNoSceneId;
    if (kind == "object") {
        created = sim.addStaticObject(staticObjectFromJson(j, assets), id);
    } else if (kind == "link") {
        created = sim.addOffMeshLink(offMeshLinkFromJson(j), id);
    } else if (kind == "goal") {
        created = sim.addGoal(goalFromJson(j), id);
    } else if (kind == "spawner") {
        Json resolved = j;
        resolveGoalReference(sim, resolved);
        created = sim.addSpawner(spawnerFromJson(resolved), id);
    } else {
        throw std::invalid_argument("unknown entity kind '" + kind + "' (object, link, goal, spawner)");
    }
    if (created == sim::kNoSceneId) {
        throw std::invalid_argument(std::format("id {} is already in use", id));
    }
    return created;
}

void patchEntity(sim::Simulation& sim, sim::SceneId id, const Json& patch, AssetLibrary* assets) {
    Json current = entityToJson(sim, id);
    if (current.is_null()) {
        throw std::invalid_argument(std::format("no scene entity with id {}", id));
    }
    Json p = patch;
    p.erase("id");
    p.erase("kind");
    current.merge_patch(p);
    switch (entityKindOf(sim, id)) {
        case EntityKind::Object:
            sim.updateStaticObject(id, staticObjectFromJson(current, assets));
            break;
        case EntityKind::Link:
            sim.updateOffMeshLink(id, offMeshLinkFromJson(current));
            break;
        case EntityKind::Goal:
            sim.updateGoal(id, goalFromJson(current));
            break;
        case EntityKind::Spawner:
            resolveGoalReference(sim, current);
            sim.updateSpawner(id, spawnerFromJson(current));
            break;
        case EntityKind::None:
            break;
    }
}

}  // namespace crowdsim::assets
