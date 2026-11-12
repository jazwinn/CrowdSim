/// Headless entry point: no window, no GPU. Runs the simulation as fast as
/// possible for batch experiments, CI and AI-driven runs (MCP stdio arrives in M8).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

#include "AppConfig.hpp"
#include "EngineContext.hpp"

int main(int argc, char** argv) {
    using namespace crowdsim;
    using Clock = std::chrono::steady_clock;

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    const app::ParseResult parsed = app::parseCommandLine(args, /*gui=*/false);
    if (parsed.error) {
        std::fprintf(stderr, "error: %s\n\n%s", parsed.error->c_str(), app::usage("crowdsim_headless", false).c_str());
        return 2;
    }
    if (parsed.config.showHelp) {
        std::printf("%s", app::usage("crowdsim_headless", false).c_str());
        return 0;
    }
    if (parsed.config.showVersion) {
        std::printf("crowdsim_headless %s\n", CROWDSIM_VERSION);
        return 0;
    }

    try {
        app::EngineContext engine(parsed.config);
        sim::Simulation& sim = engine.simulation();

        // Wall-clock time is measured here, in the app, for reporting only.
        double totalMs = 0.0;
        double maxStepMs = 0.0;
        std::uint32_t peakAgents = 0;
        for (std::uint64_t i = 0; i < parsed.config.steps; ++i) {
            const auto t0 = Clock::now();
            sim.step();
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            totalMs += ms;
            maxStepMs = std::max(maxStepMs, ms);
            peakAgents = std::max(peakAgents, sim.agents().size());
        }
        const sim::SimStats stats = sim.stats();
        if (parsed.config.dumpRemaining) {
            const sim::AgentPool& a = sim.agents();
            for (std::uint32_t i = 0; i < std::min<std::uint32_t>(a.size(), 50); ++i) {
                const std::uint32_t dense = sim.navMesh().denseIndex(a.navPoly[i]);
                const bool reachable = dense != sim::kNoPoly && sim.flowFields()[a.goalIndex[i]].reachable(dense);
                std::fprintf(stderr,
                             "agent %u pos=(%.2f, %.2f, %.2f) vel=(%.2f, %.2f) pref=(%.2f, %.2f) orca=(%.2f, %.2f) "
                             "poly=%u dense=%u "
                             "reachable=%d link=%d\n",
                             a.id[i], static_cast<double>(a.position[i].x), static_cast<double>(a.height[i]),
                             static_cast<double>(a.position[i].y), static_cast<double>(a.velocity[i].x),
                             static_cast<double>(a.velocity[i].y), static_cast<double>(a.preferredVelocity[i].x),
                             static_cast<double>(a.preferredVelocity[i].y), static_cast<double>(a.newVelocity[i].x),
                             static_cast<double>(a.newVelocity[i].y), a.navPoly[i], dense, reachable ? 1 : 0,
                             a.linkActive[i]);
            }
        }
        const double avgMs = parsed.config.steps > 0 ? totalMs / static_cast<double>(parsed.config.steps) : 0.0;

        engine.log().info("Ran {} steps ({:.1f} s simulated) in {:.1f} ms: avg {:.3f} ms/step, max {:.3f} ms",
                          sim.stepIndex(), sim.simTimeSeconds(), totalMs, avgMs, maxStepMs);
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
