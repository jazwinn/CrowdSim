/// Simulation step cost at increasing agent counts (Google Benchmark).
///
///   crowdsim_bench --benchmark_filter=SimStep
///
/// Each benchmark builds the dense counterflow "benchmark" scenario, steps until
/// every agent has spawned, then times individual steps.

#include <benchmark/benchmark.h>

#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/sim/Simulation.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"

using namespace crowdsim;

namespace {

struct Fixture {
    explicit Fixture(std::uint32_t agents, std::uint32_t threads = 0)
        : logs(core::LogConfig{.console = false}), jobs(threads), sim({logs.logger(), jobs}, sim::SimConfig{}) {
        sim::buildScenario(sim, sim::ScenarioKind::Benchmark, agents);
        // Step until everyone is spawned (bounded) and the crowd has started moving.
        for (int i = 0; i < 600 && sim.stats().spawned < agents; ++i) {
            sim.step();
        }
        for (int i = 0; i < 30; ++i) {
            sim.step();
        }
    }
    core::LogService logs;
    core::JobSystem jobs;
    sim::Simulation sim;
};

void SimStep(benchmark::State& state) {
    Fixture f(static_cast<std::uint32_t>(state.range(0)));
    for (auto _ : state) {
        f.sim.step();
    }
    state.counters["agents"] = static_cast<double>(f.sim.agents().size());
    state.counters["agents/s"] =
        benchmark::Counter(static_cast<double>(f.sim.agents().size()), benchmark::Counter::kIsIterationInvariantRate);
}
BENCHMARK(SimStep)
    ->Arg(1000)
    ->Arg(5000)
    ->Arg(20000)
    ->Arg(50000)
    ->Iterations(300)  // 10 s of sim time: nobody reaches a goal, so the count stays fixed
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

void SimStepSingleThread(benchmark::State& state) {
    Fixture f(static_cast<std::uint32_t>(state.range(0)), 1);
    for (auto _ : state) {
        f.sim.step();
    }
    state.counters["agents"] = static_cast<double>(f.sim.agents().size());
}
BENCHMARK(SimStepSingleThread)->Arg(5000)->Iterations(300)->Unit(benchmark::kMillisecond)->UseRealTime();

/// M4: the plaza runs every agent through a behaviour tree (commuters, wanderers on
/// path corridors, kiosk queues, groups). Warmed up until most agents are in.
void PlazaStep(benchmark::State& state) {
    const auto agents = static_cast<std::uint32_t>(state.range(0));
    core::LogService logs(core::LogConfig{.console = false});
    core::JobSystem jobs(0);
    sim::Simulation sim({logs.logger(), jobs}, sim::SimConfig{});
    sim::buildScenario(sim, sim::ScenarioKind::Plaza, agents);
    for (int i = 0; i < 3600 && sim.stats().spawned < agents; ++i) {
        sim.step();
    }
    for (auto _ : state) {
        sim.step();
    }
    state.counters["agents"] = static_cast<double>(sim.agents().size());
}
BENCHMARK(PlazaStep)->Arg(5000)->Arg(20000)->Iterations(300)->Unit(benchmark::kMillisecond)->UseRealTime();

void WriteSnapshot(benchmark::State& state) {
    Fixture f(static_cast<std::uint32_t>(state.range(0)));
    snapshot::RenderSnapshot snap;
    for (auto _ : state) {
        f.sim.writeSnapshot(snap);
        benchmark::DoNotOptimize(snap.agents.data());
    }
}
BENCHMARK(WriteSnapshot)->Arg(5000)->Arg(50000)->Unit(benchmark::kMicrosecond)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
