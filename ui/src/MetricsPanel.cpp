#include "crowdsim/ui/MetricsPanel.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string>

#include "crowdsim/commands/Command.hpp"
#include "crowdsim/metrics/Metrics.hpp"

namespace crowdsim::ui {

using Json = nlohmann::json;
using commands::makeCommand;

namespace {

/// Plot with the panel's conventions: no ImPlot title (a section heading names it),
/// legend in a row above the plot, both axes fitted to the data.
bool beginPlot(const char* id, const char* xLabel, const char* yLabel) {
    const ImVec2 size(-1.0f, ImGui::GetFontSize() * 10.0f);
    if (!ImPlot::BeginPlot(id, size, ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        return false;
    }
    ImPlot::SetupAxes(xLabel, yLabel, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
    // Legend entries open a colour popup when clicked (see MetricsPanel::legendMenu).
    // They stay buttons, because ImPlot only tracks legend hovering for buttons; the
    // visibility they would toggle is re-applied from the panel every frame instead.
    ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
    // ImPlot 1.0 looks up anti-aliased line UVs with the pre-1.93 ImGui atlas layout,
    // so its textured lines sample glyphs instead of the line ramp. Draw series without
    // anti-aliasing until ImPlot catches up; endPlot() restores the flag.
    ImPlot::GetPlotDrawList()->Flags &= ~ImDrawListFlags_AntiAliasedLines;
    return true;
}

void endPlot() {
    if (ImGui::GetStyle().AntiAliasedLines) {
        ImPlot::GetPlotDrawList()->Flags |= ImDrawListFlags_AntiAliasedLines;
    }
    ImPlot::EndPlot();
}

/// Series lines 2 px wide (1 px disappears against the plot well on high-DPI screens).
ImPlotSpec lineSpec() {
    ImPlotSpec spec;
    spec.LineWeight = 2.0f * ImGui::GetStyle().FontScaleDpi;
    return spec;
}

}  // namespace

namespace {

constexpr const char* kSettingsType = "CrowdSimCharts";

}  // namespace

// --- Persistence in imgui.ini ------------------------------------------------------------
// [CrowdSimCharts][Series]
// colour=0.247,0.698,0.498,1|speed/mean
// hidden=crowd/arrived

MetricsPanel::MetricsPanel() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    ImGuiSettingsHandler h;
    h.TypeName = kSettingsType;
    h.TypeHash = ImHashStr(kSettingsType);
    h.UserData = this;
    h.ClearAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler) {
        auto* self = static_cast<MetricsPanel*>(handler->UserData);
        self->seriesColors_.clear();
        self->hiddenSeries_.clear();
    };
    h.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, const char*) -> void* {
        return handler->UserData;  // one section; any non-null entry pointer
    };
    h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
        static_cast<MetricsPanel*>(entry)->readSetting(line);
    };
    h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        const std::string body = static_cast<const MetricsPanel*>(handler->UserData)->writeSettings();
        if (!body.empty()) {
            buf->appendf("[%s][Series]\n%s\n", kSettingsType, body.c_str());
        }
    };
    ImGui::AddSettingsHandler(&h);
}

MetricsPanel::~MetricsPanel() {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::RemoveSettingsHandler(kSettingsType);
    }
}

void MetricsPanel::readSetting(const char* line) {
    const std::string s(line);
    if (s.starts_with("colour=")) {
        const std::size_t bar = s.find('|');
        if (bar == std::string::npos) {
            return;
        }
        std::array<float, 4> c{0.0f, 0.0f, 0.0f, 1.0f};
        const char* p = s.data() + 7;
        const char* end = s.data() + bar;
        for (float& v : c) {
            const auto r = std::from_chars(p, end, v);
            if (r.ec != std::errc{}) {
                return;
            }
            p = r.ptr < end ? r.ptr + 1 : end;  // skip the comma
        }
        seriesColors_[s.substr(bar + 1)] = c;
    } else if (s.starts_with("hidden=") && s.size() > 7) {
        hiddenSeries_.insert(s.substr(7));
    }
}

std::string MetricsPanel::writeSettings() const {
    std::string out;
    char buf[64];
    for (const auto& [key, c] : seriesColors_) {
        std::snprintf(buf, sizeof(buf), "colour=%.3f,%.3f,%.3f,%.3f|", static_cast<double>(c[0]),
                      static_cast<double>(c[1]), static_cast<double>(c[2]), static_cast<double>(c[3]));
        out += buf + key + "\n";
    }
    for (const std::string& key : hiddenSeries_) {
        out += "hidden=" + key + "\n";
    }
    return out;
}

void MetricsPanel::settingsChanged() const {
    ImGui::MarkIniSettingsDirty();
}

void MetricsPanel::line(const char* plot, const char* label, const double* x, const double* y, int n) {
    const std::string key = std::string(plot) + "/" + label;
    ImPlotSpec spec = lineSpec();
    if (const auto it = seriesColors_.find(key); it != seriesColors_.end()) {
        spec.LineColor = ImVec4(it->second[0], it->second[1], it->second[2], it->second[3]);
    }
    ImPlot::HideNextItem(hiddenSeries_.contains(key), ImPlotCond_Always);
    ImPlot::PlotLine(label, x, y, n, spec);
    legendMenu(plot, label);
}

void MetricsPanel::legendMenu(const char* plot, const char* label) {
    const std::string key = std::string(plot) + "/" + label;
    if (ImPlot::IsLegendEntryHovered(label)) {
        ImGui::SetTooltip("Click to change the colour or hide '%s'", label);
    }
    if (!ImPlot::BeginLegendPopup(label, ImGuiMouseButton_Left)) {
        return;
    }
    ImGui::TextUnformatted(label);
    ImGui::Separator();
    const auto it = seriesColors_.find(key);
    const ImVec4 current = it != seriesColors_.end()
                               ? ImVec4(it->second[0], it->second[1], it->second[2], it->second[3])
                               : ImPlot::GetLastItemColor();
    float rgba[4] = {current.x, current.y, current.z, current.w};
    if (ImGui::ColorPicker4("##colour", rgba,
                            ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoSidePreview |
                                ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_DisplayHex)) {
        seriesColors_[key] = {rgba[0], rgba[1], rgba[2], 1.0f};
        settingsChanged();
    }
    bool shown = !hiddenSeries_.contains(key);
    if (ImGui::Checkbox("Show line", &shown)) {
        if (shown) {
            hiddenSeries_.erase(key);
        } else {
            hiddenSeries_.insert(key);
        }
        settingsChanged();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(it == seriesColors_.end());
    if (ImGui::Button("Default colour")) {
        seriesColors_.erase(key);
        settingsChanged();
    }
    ImGui::EndDisabled();
    ImPlot::EndLegendPopup();
}

void MetricsPanel::draw(const metrics::MetricsCollector& m, const float gatePoint[2], const ThemeFonts& fonts,
                        std::vector<Json>& commands) {
    const Json s = m.summary();
    const Json& ttg = s["timeToGoal"];

    // --- Now: the latest sample -----------------------------------------------------------
    sectionHeading("Now", fonts);
    if (!s.contains("alive")) {
        ImGui::TextDisabled("No samples yet. One is taken every 0.5 s of simulated time.");
    } else if (beginReadout("now")) {
        readout("Alive", "%u", s["alive"].get<unsigned>());
        readout("Arrived", "%llu", static_cast<unsigned long long>(s["arrived"].get<std::uint64_t>()));
        const std::size_t stuck = s["stuck"]["count"].get<std::size_t>();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(palette::textDim(), "Stuck");
        ImGui::SetItemTooltip("Agents that have barely moved for several seconds");
        ImGui::TableNextColumn();
        ImGui::TextColored(stuck > 0 ? palette::warning() : palette::text(), "%zu", stuck);
        readout("Mean speed", "%.2f m/s", s["speed"]["mean"].get<double>());
        readout("Speed, p90", "%.2f m/s", s["speed"]["p90"].get<double>());
        readout("Peak density", "%.1f /m2", s["density"]["peak"].get<double>());
        readout("Density, p90", "%.1f /m2", s["density"]["p90"].get<double>());
        if (ttg["count"].get<std::size_t>() > 0) {
            readout("Time to goal", "%.1f s mean", ttg["mean"].get<double>());
            readout("Time to goal, p90", "%.1f s", ttg["p90"].get<double>());
        }
        if (s.contains("pressure")) {
            // Scene bodies: crowd pressure and falls.
            const Json& pr = s["pressure"];
            const double peakNow = pr["peak"].get<double>();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(palette::textDim(), "Peak pressure");
            ImGui::SetItemTooltip(
                "Highest crowd pressure on any body now (N/m); about %.0f N/m is dangerous "
                "when sustained",
                pr["criticalLevel"].get<double>());
            ImGui::TableNextColumn();
            ImGui::TextColored(peakNow >= pr["criticalLevel"].get<double>() ? palette::warning() : palette::text(),
                               "%.0f N/m", peakNow);
            readout("Above critical now", "%u", pr["critical"].get<unsigned>());
            readout("Critically exposed", "%llu",
                    static_cast<unsigned long long>(pr["criticallyExposed"].get<std::uint64_t>()));
            ImGui::SetItemTooltip("People who have spent %.0f s or more above the critical pressure",
                                  pr["criticalSeconds"].get<double>());
            readout("On the ground", "%u", s["falls"]["onGround"].get<unsigned>());
            readout("Falls", "%llu", static_cast<unsigned long long>(s["falls"]["total"].get<std::uint64_t>()));
        }
        endReadout();
    }

    // --- Charts ---------------------------------------------------------------------------
    const auto& series = m.series();
    if (!series.empty()) {
        std::vector<double> t(series.size());
        std::vector<double> alive(series.size());
        std::vector<double> arrived(series.size());
        std::vector<double> mean(series.size());
        std::vector<double> p90(series.size());
        std::vector<double> peak(series.size());
        std::vector<double> pressurePeak(series.size());
        std::vector<double> pressureP90(series.size());
        std::vector<double> onGround(series.size());
        bool anyPressure = false;
        for (std::size_t i = 0; i < series.size(); ++i) {
            pressurePeak[i] = series[i].peakPressure;
            pressureP90[i] = series[i].p90Pressure;
            onGround[i] = series[i].fallen;
            anyPressure = anyPressure || series[i].peakPressure > 0.0f || series[i].fallen > 0;
            t[i] = series[i].time;
            alive[i] = series[i].alive;
            arrived[i] = static_cast<double>(series[i].arrived);
            mean[i] = series[i].meanSpeed;
            p90[i] = series[i].p90Speed;
            peak[i] = series[i].peakDensity;
        }
        const int n = static_cast<int>(series.size());

        sectionHeading("Crowd", fonts);
        if (beginPlot("##crowd", "time (s)", "agents")) {
            line("crowd", "alive", t.data(), alive.data(), n);
            line("crowd", "arrived", t.data(), arrived.data(), n);
            endPlot();
        }

        // Speed and density have different units, so density gets its own (right) axis.
        sectionHeading("Speed and density", fonts);
        if (beginPlot("##speed", "time (s)", "m/s")) {
            ImPlot::SetupAxis(ImAxis_Y2, "peak agents/m2", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
            line("speed", "mean", t.data(), mean.data(), n);
            line("speed", "p90", t.data(), p90.data(), n);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            line("speed", "density", t.data(), peak.data(), n);
            endPlot();
        }

        if (anyPressure) {
            // People on the ground share the chart on a right axis: counts, not N/m.
            sectionHeading("Pressure and falls", fonts);
            if (beginPlot("##pressure", "time (s)", "N/m")) {
                ImPlot::SetupAxis(ImAxis_Y2, "on the ground", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
                line("pressure", "peak", t.data(), pressurePeak.data(), n);
                line("pressure", "p90", t.data(), pressureP90.data(), n);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                line("pressure", "on the ground", t.data(), onGround.data(), n);
                endPlot();
            }
        }

        if (!m.gates().empty()) {
            sectionHeading("Gate crossings", fonts);
            if (beginPlot("##gates", "time (s)", "crossings")) {
                std::vector<double> totals(series.size());
                for (std::size_t g = 0; g < m.gates().size(); ++g) {
                    for (std::size_t i = 0; i < series.size(); ++i) {
                        totals[i] =
                            g < series[i].gateTotals.size() ? static_cast<double>(series[i].gateTotals[g]) : 0.0;
                    }
                    line("gates", m.gates()[g].name.c_str(), t.data(), totals.data(), n);
                }
                endPlot();
            }
        }
    }
    const auto& ttgValues = m.timeToGoal();
    if (!ttgValues.empty()) {
        sectionHeading("Time to goal", fonts);
        if (beginPlot("##ttg", "seconds", "agents")) {
            ImPlotSpec spec;
            if (const auto it = seriesColors_.find("ttg/arrivals"); it != seriesColors_.end()) {
                spec.FillColor = ImVec4(it->second[0], it->second[1], it->second[2], it->second[3]);
                spec.LineColor = spec.FillColor;
            }
            ImPlot::HideNextItem(hiddenSeries_.contains("ttg/arrivals"), ImPlotCond_Always);
            ImPlot::PlotHistogram("arrivals", ttgValues.data(), static_cast<int>(ttgValues.size()), 20, 1.0,
                                  ImPlotRange(), spec);
            legendMenu("ttg", "arrivals");
            endPlot();
        }
    }

    // --- Gates: counting lines ------------------------------------------------------------
    sectionHeading("Gates", fonts);
    ImGui::PushStyleColor(ImGuiCol_Text, palette::textDim());
    ImGui::TextWrapped("A gate counts agents crossing the line from A to B, in total and per second.");
    ImGui::PopStyleColor();
    if (!s["gates"].empty() &&
        ImGui::BeginTable("gates", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Gate", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Per s", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Per m", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (const Json& g : s["gates"]) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(g["name"].get<std::string>().c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(g["total"].get<std::uint64_t>()));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", g["flowPerSecond"].get<double>());
            ImGui::SetItemTooltip("Crossings per second");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", g["flowPerMetrePerSecond"].get<double>());
            ImGui::SetItemTooltip("Crossings per second, per metre of gate width");
            ImGui::TableNextColumn();
            ImGui::PushID(g["name"].get<std::string>().c_str());
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) {
                commands.push_back(makeCommand("RemoveGate", {{"name", g["name"]}}));
            }
            ImGui::SetItemTooltip("Remove this gate");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (sectionToggle("New gate", nullptr, s["gates"].empty())) {
        propertyLabel("Name");
        ImGui::InputText("##gname", gateName_, sizeof(gateName_));
        propertyLabel("End A (x, z)");
        ImGui::DragFloat2("##ga", gateA_, 0.25f, 0.0f, 0.0f, "%.2f");
        propertyLabel("End B (x, z)");
        ImGui::DragFloat2("##gb", gateB_, 0.25f, 0.0f, 0.0f, "%.2f");
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Centre on view", ImVec2(half, 0))) {
            gateA_[0] = gatePoint[0];
            gateA_[1] = gatePoint[1] - 3.0f;
            gateB_[0] = gatePoint[0];
            gateB_[1] = gatePoint[1] + 3.0f;
        }
        ImGui::SetItemTooltip("Place a 6 m gate across the point the camera looks at");
        ImGui::SameLine();
        if (ImGui::Button("Add gate", ImVec2(-1.0f, 0))) {
            commands.push_back(makeCommand(
                "AddGate",
                {{"name", std::string(gateName_)}, {"a", {gateA_[0], gateA_[1]}}, {"b", {gateB_[0], gateB_[1]}}}));
        }
    }

    // --- Recording and export -------------------------------------------------------------
    sectionHeading("Recording", fonts);
    const bool recording = s["recording"]["active"].get<bool>();
    const std::size_t rows = s["recording"]["rows"].get<std::size_t>();
    propertyLabel("Sample rate");
    ImGui::BeginDisabled(recording);
    ImGui::DragFloat("##hz", &recordHz_, 0.5f, 0.5f, 30.0f, "%.1f Hz");
    ImGui::EndDisabled();
    if (recording) {
        ImGui::TextColored(palette::accent(), "Recording trajectories: %zu rows", rows);
    } else {
        ImGui::TextDisabled(rows > 0 ? "Stopped: %zu rows kept" : "Not recording", rows);
    }
    if (ImGui::Button(recording ? "Stop recording" : "Record trajectories", ImVec2(-1.0f, 0))) {
        commands.push_back(recording ? makeCommand("StopRecording")
                                     : makeCommand("StartRecording", {{"hz", recordHz_}}));
    }
    propertyLabel("CSV file");
    ImGui::InputText("##export", exportPath_, sizeof(exportPath_));
    const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    ImGui::BeginDisabled(rows == 0);
    if (ImGui::Button("Export CSV", ImVec2(third, 0))) {
        commands.push_back(makeCommand("ExportRecording", {{"path", std::string(exportPath_)}}));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Write the recorded trajectories to the CSV file");
    ImGui::SameLine();
    if (ImGui::Button("Export series", ImVec2(third, 0))) {
        commands.push_back(makeCommand("ExportMetrics"));
    }
    ImGui::SetItemTooltip("Write the metrics time series to recordings/metrics.csv");
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(-1.0f, 0))) {
        commands.push_back(makeCommand("ResetMetrics"));
    }
    ImGui::SetItemTooltip("Discard samples, heatmaps, gate counts and recorded rows (gates stay)");
}

}  // namespace crowdsim::ui
