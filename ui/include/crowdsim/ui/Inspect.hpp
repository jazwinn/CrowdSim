#pragma once

/// @file Inspect.hpp
/// Property widgets for any described type (reflect/Reflect.hpp): one row per field, with
/// the field's label, unit, limits and tooltip, so a new type or field shows up in the
/// editor without editor code.
///
/// Edits change `value` while they happen (a drag moves the value every frame); `inspect`
/// returns true on the frame an edit is committed (a drag released, text confirmed, a box
/// ticked), which is when the caller turns the change into a command.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "crowdsim/reflect/Reflect.hpp"
#include "crowdsim/ui/Theme.hpp"

namespace crowdsim::ui {

/// Widgets the editor supplies for fields marked `.widget(name)` (pick a goal, an agent
/// type, ...). Draws the field's JSON value with its label and returns whether an edit was
/// committed, or nullopt (drawing nothing) for a widget it does not supply, which then gets
/// the default widget.
using CustomWidget =
    std::function<std::optional<bool>(std::string_view widget, const char* label, reflect::Json& value)>;

namespace inspect_detail {

/// A std::vector of described types (shown as a list of groups).
template <typename V>
inline constexpr bool isReflectedList = false;
template <reflect::Reflected T>
inline constexpr bool isReflectedList<std::vector<T>> = true;

/// Label column plus tooltip for one field row.
void row(const char* label, const reflect::Meta& meta);
void tooltip(const reflect::Meta& meta);
/// printf format for a value shown with `meta`'s unit ("%.3g m").
std::string format(const reflect::Meta& meta, bool integer = false);

bool widget(bool& v, const reflect::Meta& meta);
bool widget(std::string& v, const reflect::Meta& meta);
bool widget(std::vector<std::string>& v, const reflect::Meta& meta);
bool widget(glm::vec2& v, const reflect::Meta& meta);
bool widget(glm::vec3& v, const reflect::Meta& meta);
bool widget(float& v, const reflect::Meta& meta);
bool widget(double& v, const reflect::Meta& meta);
bool widgetInteger(std::int64_t& v, const reflect::Meta& meta);
bool widgetColor(std::uint32_t& abgr);
bool widget(reflect::Range<float>& v, const reflect::Meta& meta);
bool widgetRange(std::int64_t& lo, std::int64_t& hi, const reflect::Meta& meta);

template <typename V>
    requires(std::is_integral_v<V> && !std::is_same_v<V, bool>)
bool widget(V& v, const reflect::Meta& meta) {
    if (meta.color) {
        auto c = static_cast<std::uint32_t>(v);
        const bool done = widgetColor(c);
        v = static_cast<V>(c);
        return done;
    }
    auto wide = static_cast<std::int64_t>(v);
    const bool done = widgetInteger(wide, meta);
    v = static_cast<V>(wide);
    return done;
}

template <typename V>
    requires(std::is_integral_v<V>)
bool widget(reflect::Range<V>& v, const reflect::Meta& meta) {
    auto lo = static_cast<std::int64_t>(v.lo);
    auto hi = static_cast<std::int64_t>(v.hi);
    const bool done = widgetRange(lo, hi, meta);
    v = {static_cast<V>(lo), static_cast<V>(hi)};
    return done;
}

template <typename E>
    requires std::is_enum_v<E>
bool widget(E& v, const reflect::Meta&) {
    bool done = false;
    const std::string current = humanize(magic_enum::enum_name(v));
    if (ImGui::BeginCombo("##v", current.c_str())) {
        for (const auto& [value, name] : magic_enum::enum_entries<E>()) {
            if (ImGui::Selectable(humanize(name).c_str(), value == v) && value != v) {
                v = value;
                done = true;
            }
        }
        ImGui::EndCombo();
    }
    return done;
}

}  // namespace inspect_detail

template <reflect::Reflected T>
bool inspect(T& value, const CustomWidget& custom = {}, const char* omit = nullptr);

namespace inspect_detail {

template <reflect::Reflected T>
bool widget(T& v, const reflect::Meta&, const CustomWidget& custom) {
    return inspect(v, custom);
}

template <reflect::Reflected T>
bool widget(std::vector<T>& list, const reflect::Meta&, const CustomWidget& custom) {
    bool done = false;
    std::size_t remove = list.size();
    for (std::size_t i = 0; i < list.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::TreeNodeEx("##item", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "Item %zu",
                              i + 1)) {
            done = inspect(list[i], custom) || done;
            if (ImGui::SmallButton("Remove")) {
                remove = i;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove < list.size()) {
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(remove));
        done = true;
    }
    if (ImGui::SmallButton("Add")) {
        list.push_back(list.empty() ? T{} : list.back());
        done = true;
    }
    return done;
}

/// One field: an editor-supplied widget, a group for nested types, or a property row.
template <typename F, typename C>
bool field(const F& f, C& owner, const CustomWidget& custom) {
    using V = typename F::Value;
    const reflect::Meta& meta = f.meta;
    if (meta.hidden || !f.active(owner)) {
        return false;
    }
    const std::string label = meta.label != nullptr ? std::string(meta.label) : humanize(f.key);
    ImGui::PushID(f.key);
    V v = f.get(owner);
    bool done = false;
    std::optional<bool> handled;
    if (meta.widget != nullptr && custom) {
        const reflect::Json before = reflect::Codec<V>::encode(v, meta);
        reflect::Json j = before;
        handled = custom(meta.widget, label.c_str(), j);
        if (handled) {
            tooltip(meta);
            done = *handled;
            if (j != before) {  // in progress (a drag) or committed
                try {
                    reflect::readValue(j, v, meta);
                } catch (const std::exception&) {
                    done = false;  // the widget produced something the field cannot hold
                }
            }
        }
    }
    if (handled) {
        // drawn by the editor's widget
    } else if constexpr (reflect::Reflected<V>) {
        if (meta.flatten) {
            done = inspect(v, custom, meta.omit);  // inline, as it is saved
        } else {
            ImGui::Spacing();
            const bool open = ImGui::TreeNodeEx("##group", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                                "%s", label.c_str());
            tooltip(meta);
            if (open) {
                done = widget(v, meta, custom);
                ImGui::TreePop();
            }
        }
    } else if constexpr (isReflectedList<V>) {
        ImGui::Spacing();
        const bool open = ImGui::TreeNodeEx("##group", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                            "%s", label.c_str());
        tooltip(meta);
        if (open) {
            done = widget(v, meta, custom);
            ImGui::TreePop();
        }
    } else {
        row(label.c_str(), meta);
        done = widget(v, meta);
        tooltip(meta);
    }
    f.set(owner, std::move(v));
    ImGui::PopID();
    return done;
}

}  // namespace inspect_detail

template <reflect::Reflected T>
bool inspect(T& value, const CustomWidget& custom, const char* omit) {
    bool done = false;
    reflect::forEachField<T>([&](const auto& f) {
        if (omit == nullptr || std::string_view(f.key) != omit) {
            done = inspect_detail::field(f, value, custom) || done;
        }
    });
    return done;
}

}  // namespace crowdsim::ui
