#include "crowdsim/ui/MetricsPanel.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "crowdsim/commands/Command.hpp"
#include "crowdsim/metrics/Metrics.hpp"
#include "crowdsim/ui/Theme.hpp"

namespace crowdsim::ui {

using Json = nlohmann::json;
using commands::makeCommand;

void MetricsPanel::draw(const metrics::MetricsCollector& m, const float gatePoint[2], std::vector<Json>& commands) {
    const Json s = m.summary();

    // --- Summary ------------------------------------------------------------------------
    if (s.contains("alive")) {
        ImGui::Text("Alive %u   arrived %llu   stuck %zu", s["alive"].get<unsigned>(),
                    static_cast<unsigned long long>(s["arrived"].get<std::uint64_t>()),
                    s["stuck"]["count"].get<std::size_t>());
        ImGui::Text("Speed mean %.2f  p90 %.2f m/s   density peak %.1f  p90 %.1f /m2", s["speed"]["mean"].get<double>(),
                    s["speed"]["p90"].get<double>(), s["density"]["peak"].get<double>(),
                    s["density"]["p90"].get<double>());
    } else {
        ImGui::TextDisabled("No samples yet (one every 0.5 s of simulated time).");
    }
    const Json& ttg = s["timeToGoal"];
    if (ttg["count"].get<std::size_t>() > 0) {
        ImGui::Text("Time to goal: %zu arrivals, mean %.1f s, p50 %.1f, p90 %.1f, max %.1f",
                    ttg["count"].get<std::size_t>(), ttg["mean"].get<double>(), ttg["p50"].get<double>(),
                    ttg["p90"].get<double>(), ttg["max"].get<double>());
    }

    // --- Recording / export -------------------------------------------------------------
    const bool recording = s["recording"]["active"].get<bool>();
    if (ImGui::Button(recording ? "Stop recording" : "Record trajectories")) {
        commands.push_back(recording ? makeCommand("StopRecording")
                                     : makeCommand("StartRecording", {{"hz", recordHz_}}));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("Hz", &recordHz_, 0.5f, 0.5f, 30.0f, "%.1f");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu rows", s["recording"]["rows"].get<std::size_t>());
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##export", exportPath_, sizeof(exportPath_));
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) {
        commands.push_back(makeCommand("ExportRecording", {{"path", std::string(exportPath_)}}));
    }
    ImGui::SameLine();
    if (ImGui::Button("Export metrics")) {
        commands.push_back(makeCommand("ExportMetrics"));
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        commands.push_back(makeCommand("ResetMetrics"));
    }

    // --- Gates --------------------------------------------------------------------------
    if (crowdsim::ui::sectionToggle("Gates", nullptr, true)) {
        if (ImGui::BeginTable("gates", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Gate");
            ImGui::TableSetupColumn("Total");
            ImGui::TableSetupColumn("/s");
            ImGui::TableSetupColumn("/m/s");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const Json& g : s["gates"]) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g["name"].get<std::string>().c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(g["total"].get<std::uint64_t>()));
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", g["flowPerSecond"].get<double>());
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", g["flowPerMetrePerSecond"].get<double>());
                ImGui::TableNextColumn();
                ImGui::PushID(g["name"].get<std::string>().c_str());
                if (ImGui::SmallButton("x")) {
                    commands.push_back(makeCommand("RemoveGate", {{"name", g["name"]}}));
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputText("##gname", gateName_, sizeof(gateName_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat2("A##g", gateA_, 0.25f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat2("B##g", gateB_, 0.25f);
        ImGui::SameLine();
        if (ImGui::Button("At view")) {
            gateA_[0] = gatePoint[0];
            gateA_[1] = gatePoint[1] - 3.0f;
            gateB_[0] = gatePoint[0];
            gateB_[1] = gatePoint[1] + 3.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add gate")) {
            commands.push_back(makeCommand(
                "AddGate",
                {{"name", std::string(gateName_)}, {"a", {gateA_[0], gateA_[1]}}, {"b", {gateB_[0], gateB_[1]}}}));
        }
    }

    // --- Plots --------------------------------------------------------------------------
    const auto& series = m.series();
    if (series.empty()) {
        return;
    }
    std::vector<double> t(series.size());
    std::vector<double> alive(series.size());
    std::vector<double> arrived(series.size());
    std::vector<double> mean(series.size());
    std::vector<double> p90(series.size());
    std::vector<double> peak(series.size());
    for (std::size_t i = 0; i < series.size(); ++i) {
        t[i] = series[i].time;
        alive[i] = series[i].alive;
        arrived[i] = static_cast<double>(series[i].arrived);
        mean[i] = series[i].meanSpeed;
        p90[i] = series[i].p90Speed;
        peak[i] = series[i].peakDensity;
    }
    const int n = static_cast<int>(series.size());
    const ImVec2 size(-1.0f, 150.0f);
    if (ImPlot::BeginPlot("Crowd", size)) {
        ImPlot::SetupAxes("time (s)", "agents", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::PlotLine("alive", t.data(), alive.data(), n);
        ImPlot::PlotLine("arrived", t.data(), arrived.data(), n);
        ImPlot::EndPlot();
    }
    if (ImPlot::BeginPlot("Speed and density", size)) {
        ImPlot::SetupAxes("time (s)", "m/s | agents/m2", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::PlotLine("mean speed", t.data(), mean.data(), n);
        ImPlot::PlotLine("p90 speed", t.data(), p90.data(), n);
        ImPlot::PlotLine("peak density", t.data(), peak.data(), n);
        ImPlot::EndPlot();
    }
    if (!m.gates().empty() && ImPlot::BeginPlot("Gate crossings", size)) {
        ImPlot::SetupAxes("time (s)", "crossings", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        std::vector<double> totals(series.size());
        for (std::size_t g = 0; g < m.gates().size(); ++g) {
            for (std::size_t i = 0; i < series.size(); ++i) {
                totals[i] = g < series[i].gateTotals.size() ? static_cast<double>(series[i].gateTotals[g]) : 0.0;
            }
            ImPlot::PlotLine(m.gates()[g].name.c_str(), t.data(), totals.data(), n);
        }
        ImPlot::EndPlot();
    }
    const auto& ttgValues = m.timeToGoal();
    if (!ttgValues.empty() && ImPlot::BeginPlot("Time to goal", size)) {
        ImPlot::SetupAxes("seconds", "agents", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::PlotHistogram("arrivals", ttgValues.data(), static_cast<int>(ttgValues.size()), 20);
        ImPlot::EndPlot();
    }
}

}  // namespace crowdsim::ui
