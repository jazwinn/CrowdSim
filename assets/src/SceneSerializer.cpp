#include "crowdsim/assets/SceneSerializer.hpp"

#include <fstream>
#include <set>
#include <string>
#include <stdexcept>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/assets/BehaviorJson.hpp"
#include "crowdsim/assets/JsonTypes.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::assets {

nlohmann::json saveScene(const sim::Simulation& sim, const std::string& name) {
    Json j;
    j["format"] = "crowdsim.scene";
    j["version"] = kSceneFormatVersion;
    j["name"] = name;
    j["navigation"] = toJson(sim.config().navigation);
    j["nextId"] = sim.nextSceneId();
    auto list = [&](const std::vector<sim::SceneId>& ids) {
        Json arr = Json::array();
        for (sim::SceneId id : ids) {
            Json e = entityToJson(sim, id);
            e.erase("kind");  // implied by the array
            arr.push_back(std::move(e));
        }
        return arr;
    };
    // Agent types the spawners use, and their behaviour trees, are embedded so a
    // scene file (and every command log, which starts from one) is self-contained.
    // The built-in "default" type is never written.
    std::set<std::string> usedTypes;
    for (sim::SceneId id : sim.spawnerIds()) {
        for (const sim::AgentMixEntry& m : sim.findSpawner(id)->agentMix) {
            if (m.type != "default") {
                usedTypes.insert(m.type);
            }
        }
    }
    std::set<std::string> usedTrees;
    Json types = Json::array();
    for (const sim::AgentType& t : sim.agentTypes()) {
        if (usedTypes.contains(t.name)) {
            Json tj = toJson(t);
            tj.erase("format");
            tj.erase("version");
            types.push_back(tj);
            usedTrees.insert(t.behaviour);
        }
    }
    Json behaviours = Json::array();
    for (const sim::CompiledTree& tree : sim.behaviorTrees()) {
        if (usedTrees.contains(tree.name)) {
            behaviours.push_back(behaviorToJson(tree));
        }
    }
    if (!behaviours.empty()) {
        j["behaviours"] = behaviours;
    }
    if (!types.empty()) {
        j["agentTypes"] = types;
    }
    j["objects"] = list(sim.staticObjectIds());
    j["links"] = list(sim.offMeshLinkIds());
    j["goals"] = list(sim.goalIds());
    j["spawners"] = list(sim.spawnerIds());
    return j;
}

void migrateScene(nlohmann::json& scene) {
    const int version = scene.value("version", 0);
    if (version > kSceneFormatVersion) {
        throw std::invalid_argument("scene format version " + std::to_string(version) +
                                    " is newer than this build supports (" + std::to_string(kSceneFormatVersion) + ")");
    }
    // version 0 -> 1: no changes needed beyond stamping the version.
    scene["version"] = kSceneFormatVersion;
}

std::string loadScene(const nlohmann::json& input, sim::Simulation& sim, AssetLibrary& assets) {
    Json scene = input;
    if (scene.value("format", "crowdsim.scene") != "crowdsim.scene") {
        throw std::invalid_argument("not a crowdsim.scene document");
    }
    migrateScene(scene);

    sim.clearScene();
    if (scene.contains("navigation")) {
        sim.setNavigationSettings(navSettingsFromJson(scene["navigation"]));
    }
    // Definitions first (spawners reference agent types, types reference trees).
    for (const Json& tree : scene.value("behaviours", Json::array())) {
        sim.setBehaviorTree(compileBehaviorJson(tree));
    }
    for (const Json& type : scene.value("agentTypes", Json::array())) {
        sim.defineAgentType(agentTypeFromJson(type));
    }
    auto addAll = [&](const char* key, const char* kind) {
        if (!scene.contains(key)) {
            return;
        }
        for (Json e : scene[key]) {
            e["kind"] = kind;
            addEntityFromJson(sim, e, &assets);
        }
    };
    // Goals before spawners so spawner goal references resolve; order within a kind is preserved.
    addAll("objects", "object");
    addAll("links", "link");
    addAll("goals", "goal");
    addAll("spawners", "spawner");
    sim.reserveSceneIds(scene.value<sim::SceneId>("nextId", sim::kNoSceneId));
    return scene.value("name", std::string("untitled"));
}

void saveSceneFile(const sim::Simulation& sim, const std::string& name, const std::filesystem::path& file) {
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write '" + file.string() + "'");
    }
    out << saveScene(sim, name).dump(2) << '\n';
}

std::string loadSceneFile(const std::filesystem::path& file, sim::Simulation& sim, AssetLibrary& assets) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read '" + file.string() + "'");
    }
    return loadScene(nlohmann::json::parse(in), sim, assets);
}

}  // namespace crowdsim::assets
