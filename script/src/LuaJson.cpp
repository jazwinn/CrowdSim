#include "LuaJson.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace crowdsim::script {

using Json = nlohmann::json;

namespace {

constexpr int kMaxDepth = 64;

Json numberToJson(double d) {
    if (std::isfinite(d) && std::floor(d) == d && std::abs(d) < 9.0e15) {
        return Json(static_cast<std::int64_t>(d));
    }
    if (!std::isfinite(d)) {
        throw std::invalid_argument("cannot convert inf/nan to JSON");
    }
    return Json(d);
}

}  // namespace

Json luaToJson(const sol::object& value, int depth) {
    if (depth > kMaxDepth) {
        throw std::invalid_argument("table nesting too deep (cycle?)");
    }
    switch (value.get_type()) {
        case sol::type::lua_nil:
        case sol::type::none:
            return nullptr;
        case sol::type::boolean:
            return value.as<bool>();
        case sol::type::number:
            return numberToJson(value.as<double>());
        case sol::type::string:
            return value.as<std::string>();
        case sol::type::table: {
            const sol::table t = value.as<sol::table>();
            std::size_t count = 0;
            bool sequence = true;
            for (const auto& kv : t) {
                ++count;
                const sol::object& k = kv.first;
                if (k.get_type() != sol::type::number) {
                    sequence = false;
                } else {
                    const double d = k.as<double>();
                    if (std::floor(d) != d || d < 1.0) {
                        sequence = false;
                    }
                }
            }
            if (count == 0) {
                return Json::object();
            }
            if (sequence) {
                Json arr = Json::array();
                for (std::size_t i = 1; i <= count; ++i) {
                    const sol::object v = t.get<sol::object>(i);
                    if (v.get_type() == sol::type::lua_nil) {
                        sequence = false;  // holes: not a sequence after all
                        break;
                    }
                    arr.push_back(luaToJson(v, depth + 1));
                }
                if (sequence) {
                    return arr;
                }
            }
            Json obj = Json::object();
            for (const auto& kv : t) {
                const sol::object& k = kv.first;
                std::string key;
                if (k.get_type() == sol::type::string) {
                    key = k.as<std::string>();
                } else if (k.get_type() == sol::type::number) {
                    key = numberToJson(k.as<double>()).dump();
                } else {
                    throw std::invalid_argument("table keys must be strings or numbers");
                }
                obj[key] = luaToJson(kv.second, depth + 1);
            }
            return obj;
        }
        default:
            throw std::invalid_argument(std::string("cannot convert a Lua ") +
                                        sol::type_name(value.lua_state(), value.get_type()) + " to JSON");
    }
}

sol::object jsonToLua(sol::state_view lua, const Json& value) {
    switch (value.type()) {
        case Json::value_t::null:
        case Json::value_t::discarded:
            return sol::make_object(lua, sol::lua_nil);
        case Json::value_t::boolean:
            return sol::make_object(lua, value.get<bool>());
        case Json::value_t::number_integer:
        case Json::value_t::number_unsigned:
            return sol::make_object(lua, value.get<double>());
        case Json::value_t::number_float:
            return sol::make_object(lua, value.get<double>());
        case Json::value_t::string:
            return sol::make_object(lua, value.get<std::string>());
        case Json::value_t::array: {
            sol::table t = lua.create_table(static_cast<int>(value.size()), 0);
            int i = 1;
            for (const Json& v : value) {
                t[i++] = jsonToLua(lua, v);
            }
            return t;
        }
        case Json::value_t::object: {
            sol::table t = lua.create_table(0, static_cast<int>(value.size()));
            for (auto it = value.begin(); it != value.end(); ++it) {
                t[it.key()] = jsonToLua(lua, it.value());
            }
            return t;
        }
        case Json::value_t::binary:
            break;
    }
    return sol::make_object(lua, sol::lua_nil);
}

}  // namespace crowdsim::script
