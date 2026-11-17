#include "crowdsim/assets/JsonTypes.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/assets/SceneReflection.hpp"

// Field by field, the scene types are described once in SceneReflection.hpp; this file adds
// what is not about one value: names a document may leave out, mesh loading, goal references
// by name, and the entity envelope (kind, id, group, visibility).

namespace crowdsim::assets {

namespace {

template <typename T>
T value(const Json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    return it == j.end() || it->is_null() ? fallback : it->get<T>();
}

/// Read a described type over `base`.
template <typename T>
T read(const Json& j, T base = {}) {
    reflect::fromJson(j, base);
    return base;
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
    return reflect::roundFloat(v);
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
        case EntityKind::Group:
            return "group";
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
    if (sim.findGroup(id) != nullptr)
        return EntityKind::Group;
    return EntityKind::None;
}

Json toJson(const sim::StaticObject& o) {
    return reflect::toJson(o);
}

sim::StaticObject staticObjectFromJson(const Json& j, AssetLibrary* assets) {
    sim::StaticObject o;
    o.name = "Object";
    reflect::fromJson(j, o);
    if (o.primitive.kind == geometry::PrimitiveKind::Mesh) {
        if (assets == nullptr) {
            throw std::invalid_argument("mesh objects need an asset library");
        }
        o.mesh = assets->loadMesh(o.meshAsset).mesh;
    }
    return o;
}

Json toJson(const sim::OffMeshLink& l) {
    return reflect::toJson(l);
}

sim::OffMeshLink offMeshLinkFromJson(const Json& j) {
    return read<sim::OffMeshLink>(j);
}

Json toJson(const sim::Area& a) {
    return reflect::toJson(a);
}

sim::Area areaFromJson(const Json& j) {
    return read<sim::Area>(j);
}

Json toJson(const sim::Goal& g) {
    return reflect::toJson(g);
}

sim::Goal goalFromJson(const Json& j) {
    sim::Goal g;
    g.name = "Goal";
    return read(j, g);
}

Json toJson(const sim::AgentParams& p) {
    return reflect::toJson(p);
}

sim::AgentParams agentParamsFromJson(const Json& j) {
    return read<sim::AgentParams>(j);
}

Json toJson(const sim::Spawner& s) {
    return reflect::toJson(s);
}

sim::Spawner spawnerFromJson(const Json& j) {
    sim::Spawner s;
    s.name = "Spawner";
    return read(j, s);
}

Json toJson(const sim::NavBuildSettings& n) {
    return reflect::toJson(n);
}

sim::NavBuildSettings navSettingsFromJson(const Json& j) {
    return read<sim::NavBuildSettings>(j);
}

Json toJson(const sim::BodyParams& b) {
    return reflect::toJson(b);
}

sim::BodyParams bodyParamsFromJson(const Json& j, sim::BodyParams b) {
    if (!j.is_object()) {
        throw std::invalid_argument("bodies must be an object");
    }
    b.enabled = true;  // a "bodies" section turns them on unless it says otherwise
    return read(j, b);
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
        case EntityKind::Group:
            j = reflect::toJson(*sim.findGroup(id));
            break;
        case EntityKind::None:
            return nullptr;
    }
    Json out{{"kind", entityKindName(entityKindOf(sim, id))}, {"id", id}};
    out.update(j);
    if (const sim::SceneId parent = sim.parentOf(id); parent != sim::kNoSceneId) {
        out["parent"] = parent;
    }
    if (sim.isHidden(id)) {
        out["hidden"] = true;
    }
    return out;
}

namespace {

/// "parent" of an entity document: a group id, or kNoSceneId when absent or null.
sim::SceneId parentField(const Json& j) {
    if (!j.contains("parent") || j["parent"].is_null()) {
        return sim::kNoSceneId;
    }
    if (!j["parent"].is_number_integer()) {
        throw std::invalid_argument("parent must be a group id");
    }
    return j["parent"].get<sim::SceneId>();
}

void applyParent(sim::Simulation& sim, sim::SceneId id, sim::SceneId parent) {
    if (!sim.setParent(id, parent)) {
        throw std::invalid_argument(
            std::format("cannot put {} in {}: not a group, or it would contain itself", id, parent));
    }
}

}  // namespace

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
    } else if (kind == "group") {
        created = sim.addGroup(read(j, sim::Group{"Group"}), id);
    } else {
        throw std::invalid_argument("unknown entity kind '" + kind + "' (object, link, goal, spawner, group)");
    }
    if (created == sim::kNoSceneId) {
        throw std::invalid_argument(std::format("id {} is already in use", id));
    }
    if (const sim::SceneId parent = parentField(j); parent != sim::kNoSceneId) {
        if (!sim.setParent(created, parent)) {
            sim.removeEntity(created);
            applyParent(sim, created, parent);  // throws with the reason
        }
    }
    if (j.value("hidden", false)) {
        sim.setHidden(created, true);
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
    if (p.contains("parent")) {
        applyParent(sim, id, parentField(p));
        p.erase("parent");
    }
    if (p.contains("hidden")) {
        if (!p["hidden"].is_null() && !p["hidden"].is_boolean()) {
            throw std::invalid_argument("hidden must be true or false");
        }
        sim.setHidden(id, p["hidden"].is_boolean() && p["hidden"].get<bool>());
        p.erase("hidden");
    }
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
        case EntityKind::Group:
            sim.updateGroup(id, read(current, sim::Group{"Group"}));
            break;
        case EntityKind::None:
            break;
    }
}

}  // namespace crowdsim::assets
