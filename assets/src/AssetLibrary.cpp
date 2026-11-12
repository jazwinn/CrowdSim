#include "crowdsim/assets/AssetLibrary.hpp"

#include <spdlog/logger.h>

#include <stdexcept>

namespace crowdsim::assets {

AssetLibrary::AssetLibrary(spdlog::logger& log, std::filesystem::path contentRoot)
    : log_(log), root_(std::filesystem::absolute(std::move(contentRoot)).lexically_normal()) {}

std::filesystem::path AssetLibrary::resolve(const std::string& path) const {
    const std::filesystem::path p(path);
    return p.is_absolute() ? p : (root_ / p).lexically_normal();
}

std::string AssetLibrary::relativize(const std::filesystem::path& path) const {
    const std::filesystem::path abs = std::filesystem::absolute(path).lexically_normal();
    const std::filesystem::path rel = abs.lexically_relative(root_);
    if (!rel.empty() && rel.native()[0] != '.') {
        return rel.generic_string();
    }
    return abs.generic_string();
}

const ImportedMesh& AssetLibrary::loadMesh(const std::string& path) {
    const std::string key = relativize(resolve(path));
    if (const auto it = meshes_.find(key); it != meshes_.end()) {
        return it->second;
    }
    ImportedMesh imported = importGltf(resolve(path));
    log_.info("Imported '{}': {} triangles ({} walkable, {} obstacle)", key, imported.mesh->triangleCount(),
              imported.walkableTriangles, imported.obstacleTriangles);
    return meshes_.emplace(key, std::move(imported)).first->second;
}

}  // namespace crowdsim::assets
