#pragma once

/// @file SceneSections.hpp
/// Top-level sections of a *.scene.json owned by something other than the simulation, such
/// as an observer's settings ("attention"). The owner registers a section; saving a scene
/// writes it and loading one reads it back, so a study's settings travel with its scene.
/// Passed explicitly to saveScene / loadScene (no global registry, ADR 0002).

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace crowdsim::assets {

class SceneSections {
public:
    struct Section {
        std::string key;  ///< top-level key in the scene document
        /// The section's content, or null to leave it out (e.g. all defaults).
        std::function<nlohmann::json()> save;
        /// Check the section (null when the scene has none: back to defaults) and return how
        /// to apply it. Throws std::invalid_argument when it is invalid; nothing has changed then.
        std::function<std::function<void()>(const nlohmann::json* section)> prepare;
    };

    /// Throws std::invalid_argument for a key the scene format already uses, or a duplicate.
    void add(Section section);
    [[nodiscard]] const std::vector<Section>& sections() const noexcept { return sections_; }
    /// Every section back to its defaults (a new or built-in scene has none).
    void resetToDefaults() const;

private:
    std::vector<Section> sections_;
};

}  // namespace crowdsim::assets
