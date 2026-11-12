#pragma once

/// @file LuaJson.hpp
/// Lua values <-> JSON, used wherever scripts talk to commands.
/// Tables whose keys are exactly 1..n become arrays, other tables objects
/// (keys as strings, so key order is the sorted JSON order, not Lua's hash
/// order); an empty table becomes an empty object. Integral numbers that fit
/// become JSON integers. Functions, userdata and threads are rejected.

#include <sol/sol.hpp>

#include <nlohmann/json.hpp>

namespace crowdsim::script {

[[nodiscard]] nlohmann::json luaToJson(const sol::object& value, int depth = 0);
[[nodiscard]] sol::object jsonToLua(sol::state_view lua, const nlohmann::json& value);

}  // namespace crowdsim::script
