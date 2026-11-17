#include "crowdsim/assets/TagCatalog.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <stdexcept>

namespace crowdsim::assets {

void TagCatalog::define(const std::string& source, const nlohmann::json& doc) {
    if (!doc.is_object() || doc.value("format", std::string("crowdsim.tags")) != "crowdsim.tags") {
        throw std::invalid_argument("not a crowdsim.tags document");
    }
    const auto it = doc.find("tags");
    if (it == doc.end() || !it->is_array()) {
        throw std::invalid_argument("tags: expected a list");
    }
    std::vector<TagSpec> specs;
    specs.reserve(it->size());
    for (std::size_t i = 0; i < it->size(); ++i) {
        TagSpec spec;
        try {
            reflect::fromJson((*it)[i], spec);
        } catch (const reflect::FieldError& e) {
            throw std::invalid_argument(
                std::format("tags.{}{}{}: {}", i, e.path().empty() ? "" : ".", e.path(), e.reason()));
        }
        specs.push_back(std::move(spec));
    }
    sources_[source] = std::move(specs);
    ++version_;
}

void TagCatalog::remove(const std::string& source) {
    if (sources_.erase(source) > 0) {
        ++version_;
    }
}

std::vector<const TagSpec*> TagCatalog::all() const {
    std::map<std::string, const TagSpec*> byName;  // later sources replace earlier ones
    for (const auto& [source, specs] : sources_) {
        for (const TagSpec& s : specs) {
            byName[s.name] = &s;
        }
    }
    std::vector<const TagSpec*> out;
    out.reserve(byName.size());
    for (const auto& [name, spec] : byName) {
        out.push_back(spec);
    }
    std::stable_sort(out.begin(), out.end(), [](const TagSpec* a, const TagSpec* b) { return a->group < b->group; });
    return out;
}

TagMatch TagCatalog::match(std::string_view tag) const {
    TagMatch best;
    std::size_t bestLength = 0;
    for (const TagSpec* s : all()) {
        const std::string_view name = s->name;
        switch (s->kind) {
            case TagKind::Flag:
                if (tag == name) {
                    return {s, {}};
                }
                break;
            case TagKind::Number:
                if (tag.size() > name.size() && tag.starts_with(name) && tag[name.size()] == '=' &&
                    name.size() >= bestLength) {
                    best = {s, std::string(tag.substr(name.size() + 1))};
                    bestLength = name.size();
                }
                break;
            case TagKind::Text:
                // The longest matching prefix wins ("type:kiosk:" over "type:").
                if (tag.starts_with(name) && name.size() >= bestLength) {
                    best = {s, std::string(tag.substr(name.size()))};
                    bestLength = name.size();
                }
                break;
        }
    }
    return best;
}

std::string TagCatalog::compose(const TagSpec& spec, std::string_view value) {
    switch (spec.kind) {
        case TagKind::Flag:
            return spec.name;
        case TagKind::Number:
            return spec.name + "=" + std::string(value);
        case TagKind::Text:
            return spec.name + std::string(value);
    }
    return spec.name;
}

}  // namespace crowdsim::assets
