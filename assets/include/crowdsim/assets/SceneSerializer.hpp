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
///                    "startTime": 0, "stopTime": null, "goal": 3, "agent": {...}} ]
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

inline constexpr int kSceneFormatVersion = 1;

[[nodiscard]] nlohmann::json saveScene(const sim::Simulation& sim, const std::string& name);

/// Replace the simulation's scene with `scene` (also resets the simulation).
/// Throws std::invalid_argument / nlohmann::json::exception on malformed input.
/// Returns the scene name.
std::string loadScene(const nlohmann::json& scene, sim::Simulation& sim, AssetLibrary& assets);

void saveSceneFile(const sim::Simulation& sim, const std::string& name, const std::filesystem::path& file);
std::string loadSceneFile(const std::filesystem::path& file, sim::Simulation& sim, AssetLibrary& assets);

/// Upgrade older scene documents in place (migration hook; v1 is current).
void migrateScene(nlohmann::json& scene);

}  // namespace crowdsim::assets
