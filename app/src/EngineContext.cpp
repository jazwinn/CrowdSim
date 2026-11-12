#include "EngineContext.hpp"

namespace crowdsim::app {

EngineContext::EngineContext(const AppConfig& config)
    : config_(config),
      logService_(config.log),
      jobs_(config.threads),
      simulation_(sim::SimServices{logService_.logger(), jobs_}, config.sim) {
    log().info("CrowdSim {} | {} job threads | sim {} Hz | seed {}", CROWDSIM_VERSION, jobs_.threadCount(),
               config.sim.stepHz, config.sim.seed);
    sim::buildScenario(simulation_, config.scenario, config.agents);
    log().info("Scenario '{}' with {} agents", sim::scenarioName(config.scenario), config.agents);
}

}  // namespace crowdsim::app
