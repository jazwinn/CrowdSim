#pragma once

/// @file SceneSerializer.hpp
/// *.scene.json read/write. Format (version 1):
///
///   {
///     "format": "crowdsim.scene", "version": 1, "name": "...",
///     "navigation": { "cellSize": 0.2, ... },
///     "nextId": 42,
///     "objects":  [ {"id": 1, "name": "Floor", "type": "box", "position": [x,y,z], "rotationY": deg,
///                    "size": [w,h,d], "color": "#rrggbb", "tags": ["walkable"]}, ...
///                   {"id": 7, "type": "mesh", "asset": "models/hall.gltf", "scale": 1, ...} ],
///     "links":    [ {"id": 9, "start": [..], "end": [..], "radius": 0.6, "bidirectional": false,
///                    "extraCost": 25} ],
///     "goals":    [ {"id": 3, "name": "Exit", "area": {"center": [x,z], "halfExtents": [x,z],
///                    "elevation": 0}} ],
///     "spawners": [ {"id": 4, "name": "Entrance", "area": {...}, "rate": 20, "total": 500,
///                    "startTime": 0, "stopTime": null, "goal": 3, "agent": {...}} ],
///     "attention": {...}   // and other sections registered in SceneSections (optional)
///   }
///
/// Entities are written in creation order, so save -> load -> save is byte-identical.

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace crowdsim::sim {
class Simulation;
}

namespace crowdsim::assets {

class AssetLibrary;
class SceneSections;

inline constexpr int kSceneFormatVersion = 1;

/// `sections` adds the sections other owners registered (observer settings); optional.
[[nodiscard]] nlohmann::json saveScene(const sim::Simulation& sim, const std::string& name,
                                       const SceneSections* sections = nullptr);

/// Replace the simulation's scene with `scene` (also resets the simulation), and the
/// registered `sections` with the scene's (defaults where it has none).
/// Throws std::invalid_argument / nlohmann::json::exception on malformed input; an invalid
/// section is reported before anything changes. Returns the scene name.
std::string loadScene(const nlohmann::json& scene, sim::Simulation& sim, AssetLibrary& assets,
                      const SceneSections* sections = nullptr);

void saveSceneFile(const sim::Simulation& sim, const std::string& name, const std::filesystem::path& file,
                   const SceneSections* sections = nullptr);
std::string loadSceneFile(const std::filesystem::path& file, sim::Simulation& sim, AssetLibrary& assets,
                          const SceneSections* sections = nullptr);

/// Upgrade older scene documents in place (migration hook; v1 is current).
void migrateScene(nlohmann::json& scene);

}  // namespace crowdsim::assets
