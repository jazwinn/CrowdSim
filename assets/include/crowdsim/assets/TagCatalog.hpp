#pragma once

/// @file TagCatalog.hpp
/// What object tags mean, read from content (content/tags/*.tags.json, hot reloaded), so
/// the editor can offer the known tags with explanations and value ranges instead of a
/// blank text field. Describing a tag never needs a recompile: the engine and scripts read
/// tags as text; the catalogue only documents them. Tags missing from it still work.
///
///   {"format": "crowdsim.tags", "version": 1, "tags": [
///     {"name": "escalator", "group": "Escalators", "description": "Moving stairs or walkway..."},
///     {"name": "escalator:speed", "group": "Escalators", "kind": "number", "requires": "escalator",
///      "min": 0.1, "max": 2, "default": 0.5, "unit": "m/s", "description": "..."},
///     {"name": "area:", "group": "Regions", "kind": "text", "description": "..."}]}
///
/// - "flag" (default): the tag as it is ("walkable").
/// - "number": written "name=value" ("attention:salience=0.6"), with a range for the editor.
/// - "text": the name is a prefix followed by free text ("area:" -> "area:alley").
/// - "requires": a tag that must be present for this one to mean anything (added with it).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "crowdsim/reflect/Reflect.hpp"

namespace crowdsim::assets {

enum class TagKind : std::uint8_t { Flag, Number, Text };

struct TagSpec {
    std::string name;
    std::string group = "Other";
    std::string description;
    TagKind kind = TagKind::Flag;
    std::string requiredTag;  ///< tag this one refines, added with it (empty: none)
    float min = 0.0f;         ///< number tags
    float max = 1.0f;
    float defaultValue = 0.0f;
    std::string unit;
};

/// A tag on an object, matched against the catalogue.
struct TagMatch {
    const TagSpec* spec = nullptr;  ///< null: not in the catalogue
    std::string value;              ///< number or text part ("0.6", "alley"); empty for flags
};

class TagCatalog {
public:
    /// Replace the tags that came from `source` (a file name) with those in `doc`.
    /// Throws std::invalid_argument (with the field path) for an invalid document.
    void define(const std::string& source, const nlohmann::json& doc);
    void remove(const std::string& source);

    /// Every known tag, grouped (groups and tags in name order), the same tag from a later
    /// source replacing an earlier one.
    [[nodiscard]] std::vector<const TagSpec*> all() const;
    [[nodiscard]] TagMatch match(std::string_view tag) const;
    /// The tag text for `spec` with `value` ("name", "name=value" or "name" + text).
    [[nodiscard]] static std::string compose(const TagSpec& spec, std::string_view value);
    [[nodiscard]] bool empty() const noexcept { return sources_.empty(); }
    /// Bumped on every change (UI caches).
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

private:
    std::map<std::string, std::vector<TagSpec>> sources_;  ///< by source, ordered
    std::uint64_t version_ = 0;
};

}  // namespace crowdsim::assets

// clang-format off
CROWDSIM_REFLECT(crowdsim::assets::TagSpec,
    field("name", &T::name).required(),
    field("group", &T::group),
    field("description", &T::description),
    field("kind", &T::kind).lowercase(),
    field("requires", &T::requiredTag),
    field("min", &T::min).when([](const T& t) { return t.kind == crowdsim::assets::TagKind::Number; }),
    field("max", &T::max).when([](const T& t) { return t.kind == crowdsim::assets::TagKind::Number; }),
    field("default", &T::defaultValue).when([](const T& t) { return t.kind == crowdsim::assets::TagKind::Number; }),
    field("unit", &T::unit).when([](const T& t) { return t.kind == crowdsim::assets::TagKind::Number; }),
    check<T>([](const T& t) {
        if (t.name.empty()) {
            throw crowdsim::reflect::FieldError("name", "must not be empty");
        }
        if (t.kind == crowdsim::assets::TagKind::Number && t.max < t.min) {
            throw crowdsim::reflect::FieldError("max", "must be at least min");
        }
    }))
// clang-format on
