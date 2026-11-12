#include "EngineContext.hpp"

#include <chrono>

namespace crowdsim::app {

EngineContext::EngineContext(const AppConfig& config)
    : config_(config),
      logService_(config.log),
      jobs_(config.threads),
      physics_(physics::createJoltPhysicsWorld(joltRuntime_, logService_.logger())),
      simulation_(sim::SimServices{logService_.logger(), jobs_}, config.sim) {
    log().info("CrowdSim {} | {} job threads | sim {} Hz | seed {}", CROWDSIM_VERSION, jobs_.threadCount(),
               config.sim.stepHz, config.sim.seed);
    sim::buildScenario(simulation_, config.scenario, config.agents);
    log().info("Scenario '{}' with {} agents", sim::scenarioName(config.scenario), config.agents);
    syncScene();
}

void EngineContext::syncScene() {
    const auto start = std::chrono::steady_clock::now();
    const bool rebuilt = simulation_.updateNavigation();
    if (simulation_.staticVersion() != physicsVersion_) {
        physics_->clear();
        physics_->addStaticMesh(simulation_.staticMesh());
        physicsVersion_ = simulation_.staticVersion();
    }
    if (rebuilt) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        log().info("Scene sync: navmesh + flow fields + collision in {:.1f} ms", ms);
    }
}

}  // namespace crowdsim::app
