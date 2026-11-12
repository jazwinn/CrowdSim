#pragma once

/// @file EngineContext.hpp
/// The composition root's service bundle. Built once in main(), owned on the
/// stack there, and handed by reference to everything else. There are no
/// singletons: if a module needs a service, it receives it from here.

#include <cstdint>
#include <memory>

#include "AppConfig.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/physics/PhysicsWorld.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::app {

class EngineContext {
public:
    explicit EngineContext(const AppConfig& config);

    EngineContext(const EngineContext&) = delete;
    EngineContext& operator=(const EngineContext&) = delete;

    [[nodiscard]] const AppConfig& config() const noexcept { return config_; }
    [[nodiscard]] core::LogService& logService() noexcept { return logService_; }
    [[nodiscard]] spdlog::logger& log() noexcept { return logService_.logger(); }
    [[nodiscard]] core::JobSystem& jobs() noexcept { return jobs_; }
    [[nodiscard]] sim::Simulation& simulation() noexcept { return simulation_; }
    [[nodiscard]] physics::IPhysicsWorld& physics() noexcept { return *physics_; }

    /// Bring navigation and collision up to date with the simulation's scene.
    /// Call after scene edits (cheap when nothing changed).
    void syncScene();

private:
    // Declaration order is construction order: logging first, simulation last.
    AppConfig config_;
    core::LogService logService_;
    core::JobSystem jobs_;
    physics::JoltRuntime joltRuntime_;
    std::unique_ptr<physics::IPhysicsWorld> physics_;
    sim::Simulation simulation_;
    std::uint64_t physicsVersion_ = ~0ull;
};

}  // namespace crowdsim::app
