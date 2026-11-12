#include "crowdsim/assets/SceneSerializer.hpp"

#include <fstream>
#include <stdexcept>

#include "crowdsim/assets/AssetLibrary.hpp"
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
