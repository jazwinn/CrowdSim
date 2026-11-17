#include "crowdsim/ui/AttentionPanel.hpp"

#include "crowdsim/ui/Inspect.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include <imgui.h>

#include "crowdsim/commands/Command.hpp"
#include "crowdsim/metrics/AttentionModel.hpp"

namespace crowdsim::ui {

namespace {

using Json = nlohmann::json;
using commands::makeCommand;

struct Column {
    const char* title;
    const char* key;  ///< field of the target report
    const char* tooltip;
};

// The table's columns, in the order shown; tooltips say what each one means. The rest of an
// target's figures are in the details below the table.
constexpr Column kColumns[] = {
    {"Target", "name", "The target (an object tagged `attention`). Click to select it and see its details."},
    {"OTS", "ots", "Opportunity to see: people who came within the target's zone, whichever way they faced."},
    {"View", "viewabilityRate",
     "Viewability: share of OTS who had at least half the target unobstructed inside their field of view for a "
     "continuous second (the industry standard)."},
    {"Noticed", "noticeRate", "Share of OTS who fixated on it long enough to read it, at a readable angle and size."},
    {"Dwell", "meanDwellPerViewer",
     "Mean time the target was in a viewer's central vision, over people who looked at it at all (s). Longer dwell "
     "goes with better recall."},
};

double number(const Json& target, const char* key) {
    const Json& v = target[key];
    return v.is_number() ? v.get<double>() : 0.0;
}

}  // namespace

std::uint32_t AttentionPanel::draw(const metrics::AttentionModel& targets, double simTime, std::uint32_t selected,
                                   const ThemeFonts& fonts, std::vector<Json>& commands) {
    // The report sums every viewer's record: refresh it twice per simulated second, and
    // whenever the simulation was reset.
    if (reportTime_ < 0.0 || simTime < reportTime_ || simTime - reportTime_ >= 0.5) {
        report_ = targets.summary();
        reportTime_ = simTime;
    }
    std::uint32_t clicked = 0;
    const Json& list = report_["targets"];
    if (list.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("No targets in this scene.");
        ImGui::PushStyleColor(ImGuiCol_Text, palette::textDim());
        ImGui::TextWrapped(
            "Tag a thin box `attention` (optionally `attention:salience=0..1` for how eye-catching "
            "it is and `attention:range=<m>` for its zone) and every person's gaze is measured against it.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        drawSettings(targets, fonts, commands);
        return clicked;
    }

    // --- Headline -------------------------------------------------------------------------
    sectionHeading("Overview", fonts);
    std::size_t best = 0;
    std::uint64_t ots = 0, noticed = 0;
    for (std::size_t k = 0; k < list.size(); ++k) {
        ots += list[k]["ots"].get<std::uint64_t>();
        noticed += list[k]["noticed"].get<std::uint64_t>();
        if (number(list[k], "noticeRate") > number(list[best], "noticeRate")) {
            best = k;
        }
    }
    if (beginReadout("attentionOverview")) {
        readout("Targets", "%zu", list.size());
        readout("People seen", "%llu", static_cast<unsigned long long>(report_["agentsSeen"].get<std::uint64_t>()));
        readout("Noticed overall", "%.1f%%", ots > 0 ? 100.0 * static_cast<double>(noticed) / ots : 0.0);
        ImGui::SetItemTooltip("Noticings over opportunities to see, all targets together");
        readout("Most noticed", "%s", list[best]["name"].get<std::string>().c_str());
        endReadout();
    }

    const Json& params = report_["params"];
    const double readableAngle = params.value("readableAngle", 60.0);
    const double minSize = params.value("minAngularSize", 1.0);
    // Hard to read: mostly seen too steeply, or too small in view.
    auto problems = [&](const Json& target) {
        std::string why;
        if (number(target, "meanViewingAngle") > readableAngle) {
            why += std::format("Mostly seen at a steep angle ({:.0f} deg on average): hard to read.",
                               number(target, "meanViewingAngle"));
        }
        if (number(target, "meanAngularSize") < minSize) {
            why += std::string(why.empty() ? "" : "\n") +
                   std::format("Mostly small in view ({:.1f}% of the field on average): hard to read.",
                               number(target, "meanAngularSize"));
        }
        return why;
    };

    // --- Table ----------------------------------------------------------------------------
    sectionHeading("Per target", fonts);
    constexpr int kCols = static_cast<int>(std::size(kColumns));
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH;
    const float rowH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().CellPadding.y * 2.0f;
    // Up to about half the panel, so the selected target's details below stay in view.
    const float rows = std::clamp(std::floor(ImGui::GetContentRegionAvail().y * 0.5f / rowH), 4.0f, 12.0f);
    const float height = rowH * (std::min(static_cast<float>(list.size()), rows) + 1.0f) + 2.0f;
    if (ImGui::BeginTable("targets", kCols, flags, ImVec2(0.0f, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (int c = 0; c < kCols; ++c) {
            ImGuiTableColumnFlags f =
                c == 0 ? ImGuiTableColumnFlags_WidthStretch
                       : ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending;
            if (c == 3) {
                f |= ImGuiTableColumnFlags_DefaultSort;
            }
            ImGui::TableSetupColumn(kColumns[c].title, f);
        }
        // Headers with tooltips.
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < kCols; ++c) {
            ImGui::TableSetColumnIndex(c);
            ImGui::TableHeader(kColumns[c].title);
            ImGui::SetItemTooltip("%s", kColumns[c].tooltip);
        }

        // Rows in the chosen order.
        std::vector<std::size_t> order(list.size());
        for (std::size_t k = 0; k < order.size(); ++k) {
            order[k] = k;
        }
        if (const ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort != nullptr && sort->SpecsCount > 0) {
            const int col = sort->Specs[0].ColumnIndex;
            const bool down = sort->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
                if (col == 0) {
                    const auto& a = list[x]["name"].get_ref<const std::string&>();
                    const auto& b = list[y]["name"].get_ref<const std::string&>();
                    return down ? b < a : a < b;
                }
                const double a = number(list[x], kColumns[col].key);
                const double b = number(list[y], kColumns[col].key);
                return down ? b < a : a < b;
            });
        }
        double maxNoticed = 0.0;
        for (const Json& target : list) {
            maxNoticed = std::max(maxNoticed, number(target, "noticeRate"));
        }
        const float figure = ImGui::CalcTextSize("100.0%").x;
        for (std::size_t k : order) {
            const Json& target = list[k];
            const auto id = target["id"].get<std::uint32_t>();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(id));
            const std::string name = target["name"].get<std::string>();
            const std::string why = problems(target);
            if (ImGui::Selectable(name.c_str(), selected == id, ImGuiSelectableFlags_SpanAllColumns)) {
                clicked = id;
            }
            if (!why.empty()) {
                ImGui::SetItemTooltip("%s", why.c_str());
                ImGui::SameLine();
                ImGui::TextColored(palette::warning(), "!");
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(target["ots"].get<std::uint64_t>()));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%", number(target, "viewabilityRate"));
            ImGui::TableNextColumn();
            {
                // A thin bar under the figure, scaled to the most-noticed target, for a quick read.
                const double v = number(target, "noticeRate");
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetTextLineHeight();
                const float frac = maxNoticed > 0.0 ? static_cast<float>(v / maxNoticed) : 0.0f;
                ImVec4 accent = palette::accent();
                accent.w = 0.45f;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(p.x, p.y + h * 0.86f), ImVec2(p.x + figure * frac, p.y + h), ImGui::GetColorU32(accent));
                ImGui::Text("%.1f%%", v);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.1f s", number(target, "meanDwellPerViewer"));
        }
        ImGui::EndTable();
    }

    // --- Details of the selected target ---------------------------------------------------------
    const Json* chosen = nullptr;
    for (const Json& target : list) {
        if (target["id"].get<std::uint32_t>() == selected) {
            chosen = &target;
        }
    }
    if (chosen == nullptr) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, palette::textDim());
        ImGui::TextWrapped("Select a target, here or in the scene, for its details.");
        ImGui::PopStyleColor();
    } else {
        const Json& target = *chosen;
        sectionHeading(target["name"].get<std::string>().c_str(), fonts);
        if (beginReadout("targetDetails")) {
            readout("Opportunity to see", "%llu people",
                    static_cast<unsigned long long>(target["ots"].get<std::uint64_t>()));
            readout("Viewable", "%llu (%.1f%%)",
                    static_cast<unsigned long long>(target["viewable"].get<std::uint64_t>()),
                    number(target, "viewabilityRate"));
            readout("Noticed", "%llu (%.1f%%)", static_cast<unsigned long long>(target["noticed"].get<std::uint64_t>()),
                    number(target, "noticeRate"));
            readout("Glances drawn", "%llu", static_cast<unsigned long long>(target["glances"].get<std::uint64_t>()));
            ImGui::SetItemTooltip("Head turns towards the target, drawn from the corner of the eye");
            readout("Dwell per viewer", "%.2f s mean, %.2f s median", number(target, "meanDwellPerViewer"),
                    number(target, "medianDwellPerViewer"));
            readout("Dwell per OTS", "%.2f s", number(target, "meanDwellPerOts"));
            ImGui::SetItemTooltip("Total dwell over everyone who had the opportunity to see it");
            readout("Viewing angle", "%.0f deg mean, %.0f%% steeper than %.0f", number(target, "meanViewingAngle"),
                    number(target, "steepShare"), readableAngle);
            readout("Size in view", "%.2f%% mean, %.0f%% below %.0f%%", number(target, "meanAngularSize"),
                    number(target, "smallShare"), minSize);
            readout("Salience", "%.2f", number(target, "salience"));
            ImGui::SetItemTooltip("How eye-catching it is (tag attention:salience)");
            endReadout();
        }
    }
    ImGui::Spacing();

    // --- Actions --------------------------------------------------------------------------
    if (ImGui::Button("Export CSV")) {
        commands.push_back(makeCommand("ExportAttentionMetrics"));
    }
    ImGui::SetItemTooltip(
        "recordings/attention.csv (per target) and recordings/attention_viewers.csv (per person and target)");
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        commands.push_back(makeCommand("ResetAttentionMetrics"));
        reportTime_ = -1.0;
    }
    ImGui::SetItemTooltip("Start counting again from now");
    ImGui::Spacing();
    drawSettings(targets, fonts, commands);
    return clicked;
}

void AttentionPanel::drawSettings(const metrics::AttentionModel& model, const ThemeFonts& fonts,
                                  std::vector<Json>& commands) {
    if (!sectionToggle("Settings", fonts.strong)) {
        return;
    }
    // A working copy while a widget is being edited, so a drag carries on from where it is.
    if (!ImGui::IsAnyItemActive()) {
        settings_ = model.params;
    }
    ImGui::PushID("attentionSettings");
    if (inspect(settings_)) {
        const Json now = reflect::toJson(settings_);
        const Json live = reflect::toJson(model.params);
        Json changed = Json::object();
        for (const auto& [key, v] : now.items()) {
            if (live[key] != v) {
                changed[key] = v;
            }
        }
        if (!changed.empty()) {
            commands.push_back(makeCommand("SetAttentionParams", {{"params", changed}}));
            reportTime_ = -1.0;
        }
    }
    ImGui::PopID();
    ImGui::PushStyleColor(ImGuiCol_Text, palette::textDim());
    ImGui::TextWrapped("Saved with the scene. Changing a setting starts the count again.");
    ImGui::PopStyleColor();
}

}  // namespace crowdsim::ui
