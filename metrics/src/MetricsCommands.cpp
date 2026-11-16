#include <algorithm>
#include <stdexcept>
#include <string>

#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/commands/Command.hpp"
#include "crowdsim/metrics/Metrics.hpp"

namespace crowdsim::metrics {

namespace {

using commands::CommandContext;
using commands::CommandOutput;
using commands::makeCommand;

Json str(const char* d) {
    return Json{{"type", "string"}, {"description", d}};
}
Json num(const char* d) {
    return Json{{"type", "number"}, {"description", d}};
}
Json obj(Json props, Json required = Json::array()) {
    return Json{{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}};
}
Json point(const char* d) {
    return Json{{"type", "array"}, {"description", d}};
}

Json gateJson(const Gate& g) {
    return Json{{"name", g.name}, {"a", {g.a.x, g.a.y}}, {"b", {g.b.x, g.b.y}}, {"elevation", g.elevation}};
}

}  // namespace

void registerMetricsCommands(commands::CommandRegistry& r, MetricsCollector& m) {
    r.add({"AddGate",
           "Add (or replace) a counting gate: a line segment on the floor; agents crossing it are counted per "
           "direction and as a flow rate.",
           obj({{"name", str("gate name")},
                {"a", point("[x, z] first endpoint")},
                {"b", point("[x, z] second endpoint")},
                {"elevation", num("floor height (m)")}},
               {"name", "a", "b"}),
           [&m](const Json& p, CommandContext&) {
               Gate g;
               g.name = p.at("name").get<std::string>();
               g.a = {p.at("a").at(0).get<float>(), p.at("a").at(1).get<float>()};
               g.b = {p.at("b").at(0).get<float>(), p.at("b").at(1).get<float>()};
               g.elevation = p.value("elevation", 0.0f);
               CommandOutput out;
               const auto& gates = m.gates();
               const auto old =
                   std::find_if(gates.begin(), gates.end(), [&](const Gate& x) { return x.name == g.name; });
               out.undo = old != gates.end() ? makeCommand("AddGate", gateJson(*old))
                                             : makeCommand("RemoveGate", Json{{"name", g.name}});
               out.label = "Gate '" + g.name + "'";
               m.addGate(std::move(g));
               return out;
           }});
    r.add({"RemoveGate", "Remove a counting gate.", obj({{"name", str("gate name")}}, {"name"}),
           [&m](const Json& p, CommandContext&) {
               const std::string name = p.at("name").get<std::string>();
               CommandOutput out;
               for (const Gate& g : m.gates()) {
                   if (g.name == name) {
                       out.undo = makeCommand("AddGate", gateJson(g));
                   }
               }
               if (!m.removeGate(name)) {
                   throw std::invalid_argument("no gate named '" + name + "'");
               }
               return out;
           }});
    r.add({"ListGates", "Counting gates with their crossing counts.", obj({}),
           [&m](const Json&, CommandContext&) {
               CommandOutput out;
               out.result = Json{{"gates", m.summary()["gates"]}};
               return out;
           },
           false});
    r.add({"GetMetrics",
           "Metrics summary: speed and density statistics, gate counts and flow rates, time-to-goal distribution, "
           "stuck agents, recording state.",
           obj({}),
           [&m](const Json&, CommandContext&) {
               CommandOutput out;
               out.result = m.summary();
               return out;
           },
           false});
    r.add({"GetHeatmap",
           "Density (agents/m², time-averaged) or speed (m/s) heatmap over the scene, downsampled to at most "
           "maxCells cells.",
           obj({{"kind", str("density | speed | pressure")}, {"maxCells", num("cell budget (default 2500)")}}),
           [&m](const Json& p, CommandContext&) {
               CommandOutput out;
               out.result =
                   m.heatmapJson(p.value("kind", std::string("density")), p.value<std::size_t>("maxCells", 2500));
               return out;
           },
           false});
    r.add({"StartRecording", "Record agent trajectories (id, type, time, position, velocity, state) at `hz` samples/s.",
           obj({{"hz", num("samples per second (default 6)")}}), [&m](const Json& p, CommandContext&) {
               m.startRecording(p.value("hz", 6.0));
               return CommandOutput{};
           }});
    r.add({"StopRecording", "Stop recording trajectories (the data is kept until exported or reset).", obj({}),
           [&m](const Json&, CommandContext&) {
               m.stopRecording();
               CommandOutput out;
               out.result = Json{{"rows", m.recordedRows()}};
               return out;
           }});
    r.add({"ExportRecording",
           "Write recorded trajectories as CSV (content-relative path, default recordings/trajectories.csv).",
           obj({{"path", str("content-relative CSV path")}}),
           [&m](const Json& p, CommandContext& ctx) {
               CommandOutput out;
               if (!ctx.replaying) {
                   const auto file = ctx.assets.resolve(p.value("path", std::string("recordings/trajectories.csv")));
                   const std::size_t rows = m.exportTrajectories(file);
                   out.result = Json{{"path", ctx.assets.relativize(file)}, {"rows", rows}};
               }
               return out;
           },
           false});
    r.add({"ExportMetrics", "Write the metrics time series as CSV (default recordings/metrics.csv).",
           obj({{"path", str("content-relative CSV path")}}),
           [&m](const Json& p, CommandContext& ctx) {
               CommandOutput out;
               if (!ctx.replaying) {
                   const auto file = ctx.assets.resolve(p.value("path", std::string("recordings/metrics.csv")));
                   out.result = Json{{"path", ctx.assets.relativize(file)}, {"samples", m.exportSeries(file)}};
               }
               return out;
           },
           false});
    r.add({"ResetMetrics", "Clear accumulated metrics, heatmaps and recordings (gates are kept).", obj({}),
           [&m](const Json&, CommandContext&) {
               m.reset();
               return CommandOutput{};
           }});
}

}  // namespace crowdsim::metrics
