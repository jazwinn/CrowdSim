/// Headless entry point: no window, no GPU. Runs the simulation as fast as
/// possible for batch experiments, CI and AI-driven runs (MCP stdio arrives in M8).

#include <chrono>
#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

#include "AppConfig.hpp"
#include "EngineContext.hpp"

int main(int argc, char** argv) {
    using namespace crowdsim;

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
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < parsed.config.steps; ++i) {
            sim.step();
        }
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;

        engine.log().info("Ran {} steps ({:.2f} s simulated) in {:.3f} ms", sim.stepIndex(), sim.simTimeSeconds(),
                          elapsed.count());
        // Machine-readable result on stdout.
        std::printf("{\"steps\":%llu,\"sim_time\":%.6f,\"state_hash\":\"%016llx\"}\n",
                    static_cast<unsigned long long>(sim.stepIndex()), sim.simTimeSeconds(),
                    static_cast<unsigned long long>(sim.stateHash()));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
