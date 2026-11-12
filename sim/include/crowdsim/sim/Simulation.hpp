#pragma once

/// @file Simulation.hpp
/// The simulation world. M0: owns the ECS registry, the world RNG and the
/// fixed-step counter. Agents, navigation and behaviour arrive in later milestones.

#include <cstdint>

#include <entt/entity/registry.hpp>

#include "crowdsim/core/Random.hpp"

namespace spdlog {
class logger;
}

namespace crowdsim::core {
class JobSystem;
}

namespace crowdsim::sim {

struct SimConfig {
    std::uint64_t seed = 1;
    double stepHz = 30.0;
};

/// Services the simulation borrows from the composition root.
struct SimServices {
    spdlog::logger& log;
    core::JobSystem& jobs;
};

class Simulation {
public:
    Simulation(SimServices services, const SimConfig& config);

    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    /// Advance exactly one fixed step. No wall-clock time is read in here.
    void step();

    /// Return to the initial state with the configured seed.
    void reset();

    [[nodiscard]] std::uint64_t stepIndex() const noexcept { return stepIndex_; }
    [[nodiscard]] double simTimeSeconds() const noexcept { return static_cast<double>(stepIndex_) * stepSeconds_; }
    [[nodiscard]] double stepSeconds() const noexcept { return stepSeconds_; }

    /// Hash of all simulation state; equal hashes after equal inputs prove determinism.
    [[nodiscard]] std::uint64_t stateHash() const noexcept;

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }

private:
    SimServices services_;
    SimConfig config_;
    double stepSeconds_;
    std::uint64_t stepIndex_ = 0;
    core::Pcg32 rng_;
    entt::registry registry_;
};

}  // namespace crowdsim::sim
