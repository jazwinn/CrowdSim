/// Headless entry point: no window, no GPU. Runs the simulation as fast as
/// possible for batch experiments, CI and AI-driven runs. Everything is driven
/// through commands, exactly like the GUI: --commands executes a JSON array of
/// commands (Step / RunFor advance the simulation in order), --replay re-runs a
/// command log deterministically. Results go to stdout as JSON lines; logs go
/// to stderr. (The MCP stdio transport arrives in M8.)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "AppConfig.hpp"
#include "EngineContext.hpp"
#include "McpInstructions.hpp"
#include "crowdsim/assets/SceneSerializer.hpp"
#include "crowdsim/mcp/McpServer.hpp"
#include "crowdsim/mcp/Transports.hpp"

namespace {

using namespace crowdsim;
using Json = nlohmann::json;

Json readJsonFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read '" + path + "'");
    }
    return Json::parse(in);
}

void writeJsonFile(const std::string& path, const Json& j) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write '" + path + "'");
    }
    out << j.dump(2) << '\n';
}

void printLine(const Json& j) {
    std::printf("%s\n", j.dump().c_str());
    std::fflush(stdout);
}

void dumpRemaining(sim::Simulation& sim) {
    const sim::AgentPool& a = sim.agents();
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(a.size(), 50); ++i) {
        const std::uint32_t dense = sim.navMesh().denseIndex(a.navPoly[i]);
        const bool reachable = dense != sim::kNoPoly && sim.flowFields()[a.goalIndex[i]].reachable(dense);
        std::fprintf(stderr,
                     "agent %u pos=(%.2f, %.2f, %.2f) vel=(%.2f, %.2f) pref=(%.2f, %.2f) orca=(%.2f, %.2f) poly=%u "
                     "dense=%u reachable=%d link=%d\n",
                     a.id[i], static_cast<double>(a.position[i].x), static_cast<double>(a.height[i]),
                     static_cast<double>(a.position[i].y), static_cast<double>(a.velocity[i].x),
                     static_cast<double>(a.velocity[i].y), static_cast<double>(a.preferredVelocity[i].x),
                     static_cast<double>(a.preferredVelocity[i].y), static_cast<double>(a.newVelocity[i].x),
                     static_cast<double>(a.newVelocity[i].y), a.navPoly[i], dense, reachable ? 1 : 0, a.linkActive[i]);
        if (const auto d = sim.agentDebug(a.id[i])) {
            std::fprintf(stderr,
                         "    type=%s tree=%s state=%s goal=%s mode=%d target=(%.2f, %.2f, %.2f) queue=%d "
                         "group=%u corridor=%d/%d\n",
                         d->typeName.c_str(), d->treeName.c_str(), d->stateName.c_str(), d->goalName.c_str(),
                         static_cast<int>(d->moveMode), static_cast<double>(d->moveTarget.x),
                         static_cast<double>(d->moveTarget.y), static_cast<double>(d->moveTarget.z), d->queueStage,
                         d->groupId, static_cast<int>(a.corridorStatus[i]), static_cast<int>(a.corridorLength[i]));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const app::ParseResult parsed = app::parseCommandLine(args, /*gui=*/false);
    if (parsed.error) {
        std::fprintf(stderr, "error: %s\n\n%s", parsed.error->c_str(), app::usage("crowdsim_headless", false).c_str());
        return 2;
    }
    const app::AppConfig& config = parsed.config;
    if (config.showHelp) {
        std::printf("%s", app::usage("crowdsim_headless", false).c_str());
        return 0;
    }
    if (config.showVersion) {
        std::printf("crowdsim_headless %s\n", CROWDSIM_VERSION);
        return 0;
    }

    try {
        app::EngineContext engine(config);
        sim::Simulation& sim = engine.simulation();

        if (config.listCommands) {
            Json list = Json::array();
            for (const commands::CommandSpec& spec : engine.dispatcher().registry().all()) {
                list.push_back(Json{{"name", spec.name},
                                    {"description", spec.description},
                                    {"params", spec.paramsSchema},
                                    {"logged", spec.logged}});
            }
            printLine(list);
            return 0;
        }

        if (!config.replayFile.empty()) {
            const Json log = readJsonFile(config.replayFile);
            const auto t0 = Clock::now();
            const std::uint64_t hash = commands::replayLog(log, engine.dispatcher(), engine.commandContext());
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            printLine(Json{{"replay", config.replayFile},
                           {"steps", sim.stepIndex()},
                           {"entries", log["entries"].size()},
                           {"ms", ms},
                           {"state_hash", std::format("{:016x}", hash)}});
            return 0;
        }

        // MCP server: stdio (one client, until stdin closes) or HTTP (until killed).
        if (config.mcpStdio || !config.mcpHttp.empty()) {
            mcp::McpServices services = engine.mcpServices();
            mcp::McpServer server("crowdsim", CROWDSIM_VERSION, app::kMcpInstructions);
            mcp::registerCrowdSimTools(server, services);
            if (config.mcpStdio) {
                engine.log().info("MCP: serving on stdio ({} tools)", server.tools().size());
                mcp::runStdio(server, std::cin, std::cout);
                return 0;
            }
            mcp::McpRequestQueue queue;
            mcp::HttpTransport http(queue, config.mcpToken);
            const auto [host, port] = mcp::parseHostPort(config.mcpHttp);
            http.start(host, port);
            engine.log().info("MCP: listening on http://{}:{}/mcp ({} tools)", host, http.port(),
                              server.tools().size());
            for (;;) {
                queue.waitForWork(std::chrono::milliseconds(200));
                queue.process(server);
            }
        }

        // Command batch: each result is printed as one JSON line.
        if (!config.commandsFile.empty()) {
            const Json batch = readJsonFile(config.commandsFile);
            if (!batch.is_array()) {
                throw std::runtime_error("--commands file must contain a JSON array of commands");
            }
            for (const Json& cmd : batch) {
                const commands::CommandOutcome o = engine.execute(cmd);
                engine.syncScene();
                const std::uint64_t ran = engine.runPendingSteps(~0ull);
                printLine(Json{{"cmd", o.command},
                               {"ok", o.ok},
                               {"result", o.result},
                               {"error", o.error},
                               {"steps_run", ran},
                               {"sim_time", sim.simTimeSeconds()}});
            }
        }

        // Wall-clock time is measured here, in the app, for reporting only.
        double totalMs = 0.0;
        double maxStepMs = 0.0;
        std::uint32_t peakAgents = 0;
        sim::StepProfile phases;  // sums
        for (std::uint64_t i = 0; i < config.steps; ++i) {
            const auto t0 = Clock::now();
            sim.step();
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            totalMs += ms;
            maxStepMs = std::max(maxStepMs, ms);
            peakAgents = std::max(peakAgents, sim.agents().size());
            const sim::StepProfile& p = sim.lastStepProfile();
            phases.navigation += p.navigation;
            phases.hash += p.hash;
            phases.spawn += p.spawn;
            phases.decisions += p.decisions;
            phases.resolve += p.resolve;
            phases.preferred += p.preferred;
            phases.avoidance += p.avoidance;
            phases.integrate += p.integrate;
            phases.despawn += p.despawn;
            phases.observers += p.observers;
        }
        if (config.profile && config.steps > 0) {
            const auto avg = [&](float sum) {
                return std::round(sum / static_cast<double>(config.steps) * 1000.0) / 1000.0;
            };
            printLine(Json{{"profile_ms_per_step",
                            {{"navigation", avg(phases.navigation)},
                             {"hash", avg(phases.hash)},
                             {"spawn", avg(phases.spawn)},
                             {"decisions", avg(phases.decisions)},
                             {"resolve", avg(phases.resolve)},
                             {"preferred", avg(phases.preferred)},
                             {"avoidance", avg(phases.avoidance)},
                             {"integrate", avg(phases.integrate)},
                             {"despawn", avg(phases.despawn)},
                             {"observers", avg(phases.observers)}}},
                           {"agents_peak", peakAgents}});
        }
        const sim::SimStats stats = sim.stats();
        if (config.dumpRemaining) {
            dumpRemaining(sim);
        }
        if (!config.exportScene.empty()) {
            assets::saveSceneFile(sim, engine.sceneName(), engine.assets().resolve(config.exportScene));
        }
        if (!config.saveLogFile.empty()) {
            writeJsonFile(config.saveLogFile, engine.dispatcher().logToJson(engine.commandContext()));
        }
        const double avgMs = config.steps > 0 ? totalMs / static_cast<double>(config.steps) : 0.0;
        engine.log().info("Ran {} steps ({:.1f} s simulated): avg {:.3f} ms/step, max {:.3f} ms", sim.stepIndex(),
                          sim.simTimeSeconds(), avgMs, maxStepMs);

        // Machine-readable result on stdout.
        std::printf(
            "{\"steps\":%llu,\"sim_time\":%.6f,\"alive\":%u,\"peak_alive\":%u,\"spawned\":%llu,\"arrived\":%llu,"
            "\"avg_speed\":%.4f,\"avg_step_ms\":%.4f,\"max_step_ms\":%.4f,\"state_hash\":\"%016llx\"}\n",
            static_cast<unsigned long long>(sim.stepIndex()), sim.simTimeSeconds(), stats.alive, peakAgents,
            static_cast<unsigned long long>(stats.spawned), static_cast<unsigned long long>(stats.arrived),
            static_cast<double>(stats.averageSpeed), avgMs, maxStepMs,
            static_cast<unsigned long long>(sim.stateHash()));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
