#include "crowdsim/assets/SceneSections.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace crowdsim::assets {

void SceneSections::add(Section section) {
    static constexpr std::array<std::string_view, 13> kReserved = {
        "format", "version", "name",   "navigation", "bodies", "nextId",  "behaviours",
        "agentTypes", "groups", "objects", "links", "goals", "spawners"};
    if (section.key.empty() || std::find(kReserved.begin(), kReserved.end(), section.key) != kReserved.end()) {
        throw std::invalid_argument("scene section key '" + section.key + "' is reserved");
    }
    if (std::any_of(sections_.begin(), sections_.end(), [&](const Section& s) { return s.key == section.key; })) {
        throw std::invalid_argument("scene section '" + section.key + "' is already registered");
    }
    sections_.push_back(std::move(section));
}

void SceneSections::resetToDefaults() const {
    for (const Section& s : sections_) {
        s.prepare(nullptr)();
    }
}

}  // namespace crowdsim::assets
