/// Character baking and per-frame gait cost (Google Benchmark).
///
///   crowdsim_bench --benchmark_filter=Gait|Bake

#include <benchmark/benchmark.h>

#include <vector>

#include "crowdsim/animation/Gait.hpp"
#include "crowdsim/animation/ProceduralCharacter.hpp"

using namespace crowdsim;

namespace {

void BakeCharacter(benchmark::State& state) {
    for (auto _ : state) {
        const snapshot::CharacterAsset c = animation::buildProceduralCharacter();
        benchmark::DoNotOptimize(c.boneTexture.data());
    }
}
BENCHMARK(BakeCharacter)->Unit(benchmark::kMillisecond);

/// One frame of gait updates for N agents at mixed speeds (single thread).
void GaitUpdate(benchmark::State& state) {
    const snapshot::CharacterAsset c = animation::buildProceduralCharacter();
    const auto n = static_cast<std::uint32_t>(state.range(0));
    std::vector<animation::GaitState> states(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        states[i] = animation::initialGaitState(i);
    }
    float sink = 0.0f;
    for (auto _ : state) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const float speed = static_cast<float>(i % 32) * 0.1f;
            const animation::GaitSample g = animation::updateGait(c, states[i], speed, 0.2f, 1.0f, 1.0f / 60.0f);
            sink += g.blend;
        }
    }
    benchmark::DoNotOptimize(sink);
    state.counters["agents/s"] =
        benchmark::Counter(static_cast<double>(n), benchmark::Counter::kIsIterationInvariantRate);
}
BENCHMARK(GaitUpdate)->Arg(5000)->Arg(50000)->Unit(benchmark::kMicrosecond);

}  // namespace
