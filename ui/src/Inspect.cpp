#include "crowdsim/ui/Inspect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace crowdsim::ui::inspect_detail {

namespace {

bool committed() {
    return ImGui::IsItemDeactivatedAfterEdit();
}

/// Drag limits in shown units (ImGui clamps only when min < max).
void limits(const reflect::Meta& m, float& lo, float& hi) {
    lo = std::isfinite(m.min) ? static_cast<float>(m.min) : -std::numeric_limits<float>::max();
    hi = std::isfinite(m.max) ? static_cast<float>(m.max) : std::numeric_limits<float>::max();
}

float speed(const reflect::Meta& m) {
    if (m.speed > 0.0) {
        return static_cast<float>(m.speed);
    }
    if (std::isfinite(m.min) && std::isfinite(m.max)) {
        return static_cast<float>(std::max(0.001, (m.max - m.min) / 200.0));
    }
    return 0.05f;
}

/// Keep exclusive lower limits out of reach of the drag (above(0) cannot be dragged to 0).
float floor(const reflect::Meta& m, float lo) {
    return m.minExclusive ? lo + std::max(1e-3f, std::abs(lo) * 1e-3f) : lo;
}

}  // namespace

void row(const char* label, const reflect::Meta&) {
    propertyLabel(label);
}

void tooltip(const reflect::Meta& meta) {
    if (meta.tooltip != nullptr) {
        ImGui::SetItemTooltip("%s", meta.tooltip);
    }
}

std::string format(const reflect::Meta& meta, bool integer) {
    std::string f = integer ? "%d" : "%.3g";
    if (meta.unit != nullptr) {
        f += " ";
        f += meta.unit;
    }
    return f;
}

bool widget(bool& v, const reflect::Meta&) {
    return ImGui::Checkbox("##v", &v);
}

bool widget(std::string& v, const reflect::Meta&) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", v.c_str());
    ImGui::InputText("##v", buf, sizeof(buf));
    v = buf;
    return committed();
}

bool widget(std::vector<std::string>& v, const reflect::Meta&) {
    std::string joined;
    for (const std::string& t : v) {
        joined += (joined.empty() ? "" : ", ") + t;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", joined.c_str());
    ImGui::InputTextWithHint("##v", "comma, separated", buf, sizeof(buf));
    if (!committed()) {
        return false;
    }
    v.clear();
    std::string token;
    for (const char* c = buf;; ++c) {
        if (*c == ',' || *c == '\0') {
            token.erase(0, token.find_first_not_of(' '));
            token.erase(token.find_last_not_of(' ') + 1);
            if (!token.empty()) {
                v.push_back(token);
            }
            token.clear();
            if (*c == '\0') {
                break;
            }
        } else {
            token += *c;
        }
    }
    return true;
}

namespace {

template <int N>
bool vecWidget(float* v, const reflect::Meta& meta) {
    const auto k = static_cast<float>(meta.scale);
    float shown[N];
    for (int i = 0; i < N; ++i) {
        shown[i] = v[i] * k;
    }
    float lo = 0.0f;
    float hi = 0.0f;
    limits(meta, lo, hi);
    const std::string f = format(meta);
    ImGui::DragScalarN("##v", ImGuiDataType_Float, shown, N, speed(meta), &lo, &hi, f.c_str(),
                       meta.bounded() ? ImGuiSliderFlags_AlwaysClamp : 0);
    for (int i = 0; i < N; ++i) {
        v[i] = shown[i] / k;
    }
    return committed();
}

}  // namespace

bool widget(glm::vec2& v, const reflect::Meta& meta) {
    return vecWidget<2>(&v.x, meta);
}

bool widget(glm::vec3& v, const reflect::Meta& meta) {
    if (meta.color) {
        ImGui::ColorEdit3("##v", &v.x, ImGuiColorEditFlags_DisplayHex);
        return committed();
    }
    return vecWidget<3>(&v.x, meta);
}

bool widget(float& v, const reflect::Meta& meta) {
    if (meta.infinityAsNull && std::isinf(v)) {
        if (ImGui::Button("Never (click to set)", ImVec2(-1.0f, 0.0f))) {
            v = 60.0f;
            return true;
        }
        return false;
    }
    const auto k = static_cast<float>(meta.scale);
    float shown = v * k;
    float lo = 0.0f;
    float hi = 0.0f;
    limits(meta, lo, hi);
    lo = floor(meta, lo);
    const std::string f = format(meta);
    if (meta.infinityAsNull) {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Never").x -
                                ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
    }
    ImGui::DragFloat("##v", &shown, speed(meta), lo, hi, f.c_str(), meta.bounded() ? ImGuiSliderFlags_AlwaysClamp : 0);
    v = shown / k;
    bool done = committed();
    if (meta.infinityAsNull) {
        ImGui::SameLine();
        if (ImGui::Button("Never")) {
            v = std::numeric_limits<float>::infinity();
            done = true;
        }
    }
    return done;
}

bool widget(double& v, const reflect::Meta& meta) {
    auto f = static_cast<float>(v);
    const bool done = widget(f, meta);
    v = f;
    return done;
}

bool widgetInteger(std::int64_t& v, const reflect::Meta& meta) {
    const std::int64_t lo = std::isfinite(meta.min) ? static_cast<std::int64_t>(std::ceil(meta.min + (meta.minExclusive ? 0.5 : 0.0)))
                                                    : std::numeric_limits<std::int32_t>::min();
    const std::int64_t hi =
        std::isfinite(meta.max) ? static_cast<std::int64_t>(meta.max) : std::numeric_limits<std::int32_t>::max();
    const std::string f = format(meta, true);
    std::int64_t shown = v;
    ImGui::DragScalar("##v", ImGuiDataType_S64, &shown, 0.25f, &lo, &hi, "%lld", ImGuiSliderFlags_AlwaysClamp);
    (void)f;
    v = shown;
    return committed();
}

bool widgetColor(std::uint32_t& abgr) {
    float c[3] = {static_cast<float>(abgr & 0xffu) / 255.0f, static_cast<float>((abgr >> 8) & 0xffu) / 255.0f,
                  static_cast<float>((abgr >> 16) & 0xffu) / 255.0f};
    ImGui::ColorEdit3("##v", c, ImGuiColorEditFlags_DisplayHex);
    auto ch = [](float x) { return static_cast<std::uint32_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
    abgr = (abgr & 0xff000000u) | (ch(c[2]) << 16) | (ch(c[1]) << 8) | ch(c[0]);
    return committed();
}

bool widget(reflect::Range<float>& v, const reflect::Meta& meta) {
    float pair[2] = {v.lo, v.hi};
    const bool done = vecWidget<2>(pair, meta);
    v = {pair[0], std::max(pair[0], pair[1])};
    return done;
}

bool widgetRange(std::int64_t& lo, std::int64_t& hi, const reflect::Meta& meta) {
    const std::int64_t min = std::isfinite(meta.min) ? static_cast<std::int64_t>(std::ceil(meta.min)) : 0;
    const std::int64_t max =
        std::isfinite(meta.max) ? static_cast<std::int64_t>(meta.max) : std::numeric_limits<std::int32_t>::max();
    std::int64_t pair[2] = {lo, hi};
    ImGui::DragScalarN("##v", ImGuiDataType_S64, pair, 2, 0.1f, &min, &max, "%lld", ImGuiSliderFlags_AlwaysClamp);
    lo = pair[0];
    hi = std::max(pair[0], pair[1]);
    return committed();
}

}  // namespace crowdsim::ui::inspect_detail
