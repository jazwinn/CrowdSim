#pragma once

/// @file AssetLibrary.hpp
/// Loads and caches assets referenced by scenes (imported meshes). Paths are
/// stored in scenes relative to the content root, so scene files stay portable.

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "crowdsim/geometry/TriangleMesh.hpp"

namespace spdlog {
class logger;
}

namespace crowdsim::assets {

/// Result of importing a glTF file.
struct ImportedMesh {
    std::shared_ptr<const geometry::TriangleMesh> mesh;  ///< CrowdSim axes (+Y up, left-handed)
    std::uint32_t walkableTriangles = 0;
    std::uint32_t obstacleTriangles = 0;
    std::vector<std::string> nodeTags;  ///< "node: tag, tag" lines, for reporting
};

class AssetLibrary {
public:
    AssetLibrary(spdlog::logger& log, std::filesystem::path contentRoot);

    /// Absolute path for a content-relative (or absolute) path.
    [[nodiscard]] std::filesystem::path resolve(const std::string& path) const;
    /// Content-relative path with forward slashes when inside the root, else absolute.
    [[nodiscard]] std::string relativize(const std::filesystem::path& path) const;
    [[nodiscard]] const std::filesystem::path& contentRoot() const noexcept { return root_; }

    /// Load (or fetch from cache) a glTF mesh. Throws std::runtime_error on failure.
    const ImportedMesh& loadMesh(const std::string& path);

    /// Forget cached meshes (hot reload).
    void clearCache() { meshes_.clear(); }

private:
    spdlog::logger& log_;
    std::filesystem::path root_;
    std::map<std::string, ImportedMesh> meshes_;  // ordered: deterministic iteration
};

/// Import a glTF 2.0 file (.gltf or .glb) into one triangle mesh in CrowdSim axes.
/// Surface flags come from tags on nodes (inherited by children):
///   - node name containing "obstacle", "wall", "pillar", "column", "blocker" -> obstacle
///   - node name containing "nowalk", "ceiling", "roof"                       -> not walkable
///   - extras {"crowdsim": {"tags": ["obstacle" | "walkable" | "nowalk", ...]}} override names
///   - otherwise walkable (Recast's slope test still applies)
/// Throws std::runtime_error on failure.
[[nodiscard]] ImportedMesh importGltf(const std::filesystem::path& file);

}  // namespace crowdsim::assets
