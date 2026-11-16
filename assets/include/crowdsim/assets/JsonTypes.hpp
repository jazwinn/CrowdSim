#pragma once

/// @file JsonTypes.hpp
/// JSON forms of scene entities, shared by scene files, the command layer and
/// MCP. Human/AI friendly: angles in degrees, colours as "#rrggbb", floats
/// rounded to 6 significant digits so files are stable under save/load.

#include <string>

#include <nlohmann/json.hpp>

#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::assets {

class AssetLibrary;

using Json = nlohmann::json;

/// Entity kinds as they appear in JSON ("kind" field and command names).
enum class EntityKind { Object, Link, Goal, Spawner, None };
[[nodiscard]] const char* entityKindName(EntityKind kind);
[[nodiscard]] EntityKind entityKindOf(const sim::Simulation& sim, sim::SceneId id);

[[nodiscard]] Json toJson(const sim::StaticObject& o);
/// `assets` resolves "asset" paths of mesh objects (required for type "mesh").
[[nodiscard]] sim::StaticObject staticObjectFromJson(const Json& j, AssetLibrary* assets);

[[nodiscard]] Json toJson(const sim::OffMeshLink& l);
[[nodiscard]] sim::OffMeshLink offMeshLinkFromJson(const Json& j);

[[nodiscard]] Json toJson(const sim::Area& a);
[[nodiscard]] sim::Area areaFromJson(const Json& j);

[[nodiscard]] Json toJson(const sim::Goal& g);
[[nodiscard]] sim::Goal goalFromJson(const Json& j);

[[nodiscard]] Json toJson(const sim::AgentParams& p);
[[nodiscard]] sim::AgentParams agentParamsFromJson(const Json& j);

[[nodiscard]] Json toJson(const sim::Spawner& s);
[[nodiscard]] sim::Spawner spawnerFromJson(const Json& j);

[[nodiscard]] Json toJson(const sim::NavBuildSettings& n);
[[nodiscard]] sim::NavBuildSettings navSettingsFromJson(const Json& j);

[[nodiscard]] Json toJson(const sim::BodyParams& b);
/// Missing fields keep the values in `base`; a present "bodies" section is enabled unless it says otherwise.
[[nodiscard]] sim::BodyParams bodyParamsFromJson(const Json& j, sim::BodyParams base = {});

/// {"kind": ..., "id": ..., <entity fields>} for any scene entity; null if unknown.
[[nodiscard]] Json entityToJson(const sim::Simulation& sim, sim::SceneId id);

/// Add an entity from its entityToJson form (uses "id" if present). Returns its id.
sim::SceneId addEntityFromJson(sim::Simulation& sim, const Json& j, AssetLibrary* assets);

/// Merge `patch` (JSON merge-patch semantics) into entity `id` and apply it.
/// Throws std::invalid_argument for unknown ids or invalid fields.
void patchEntity(sim::Simulation& sim, sim::SceneId id, const Json& patch, AssetLibrary* assets);

/// Rounding used for all floats written to JSON.
[[nodiscard]] double roundFloat(float v);

}  // namespace crowdsim::assets
