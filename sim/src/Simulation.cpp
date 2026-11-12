#include "crowdsim/sim/Simulation.hpp"

#include <spdlog/logger.h>

#include "crowdsim/core/Hash.hpp"
#include "crowdsim/core/Profile.hpp"

namespace crowdsim::sim {

Simulation::Simulation(SimServices services, const SimConfig& config)
    : services_(services), config_(config), stepSeconds_(1.0 / config.stepHz), rng_(config.seed) {
    services_.log.debug("Simulation created (seed={}, {} Hz)", config_.seed, config_.stepHz);
}

void Simulation::step() {
    CS_PROFILE_ZONE();
    // M0: the world is empty; consuming one RNG value per step makes the
    // determinism test meaningful before real systems exist.
    (void)rng_.next();
    ++stepIndex_;
}

void Simulation::reset() {
    registry_.clear();
    rng_.reseed(config_.seed);
    stepIndex_ = 0;
}

std::uint64_t Simulation::stateHash() const noexcept {
    core::Fnv1a64 hash;
    hash.add(stepIndex_);
    hash.add(rng_.state());
    return hash.value();
}

}  // namespace crowdsim::sim
