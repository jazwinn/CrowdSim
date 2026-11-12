#include "EngineContext.hpp"

#include "crowdsim/script/ScriptCommands.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>

namespace crowdsim::app {

EngineContext::EngineContext(const AppConfig& config)
    : config_(config),
      logService_(config.log),
      jobs_(config.threads),
      physics_(physics::createJoltPhysicsWorld(joltRuntime_, logService_.logger())),
      simulation_(sim::SimServices{logService_.logger(), jobs_}, config.sim),
      assets_(logService_.logger(), config.contentDir),
      commandContext_{simulation_, assets_, control_, logService_.logger(), sceneName_},
      dispatcher_(registry_),
      scriptHost_(registry_, commandContext_, logService_.logger()),
      contentWatcher_(assets_.contentRoot()) {
    commands::registerBuiltinCommands(registry_);
    script::registerScriptCommands(registry_, scriptHost_);
    // Command logs start by re-loading the scripts, then the scene snapshot.
    commandContext_.logPrelude = [this] { return scriptHost_.logPrelude(); };
    log().info("CrowdSim {} | {} job threads | sim {} Hz | seed {} | content {}", CROWDSIM_VERSION, jobs_.threadCount(),
               config.sim.stepHz, config.sim.seed, assets_.contentRoot().string());

    // Content library first: scenarios only define the built-in types that are missing.
    if (config.loadContent) {
        const std::size_t loaded = reloadContent(true);
        log().info("Loaded {} behaviour/agent definitions from {}", loaded, assets_.contentRoot().string());
    }

    // Initial scene: a scene file if given, else a built-in scenario. Through commands, like everything else.
    const commands::CommandOutcome outcome =
        config.scenePath.empty()
            ? execute(commands::makeCommand(
                  "LoadBuiltinScenario",
                  {{"name", std::string(sim::scenarioName(config.scenario))}, {"agents", config.agents}}))
            : execute(commands::makeCommand("LoadScene", {{"path", config.scenePath}}));
    if (!outcome.ok) {
        throw std::runtime_error("could not load the initial scene: " + outcome.error);
    }
    dispatcher_.clearHistory();
    dispatcher_.restartLog(commandContext_);
    log().info("Scene '{}' loaded", sceneName_);

    // Scenario script (after the log restart, so it is recorded like any other command).
    if (!config.scriptPath.empty()) {
        const commands::CommandOutcome run =
            execute(commands::makeCommand("LoadScript", {{"path", config.scriptPath}}));
        if (!run.ok) {
            throw std::runtime_error("script '" + config.scriptPath + "' failed: " + run.error);
        }
    }
    syncScene();
}

commands::CommandOutcome EngineContext::execute(const commands::Json& command) {
    return dispatcher_.execute(command, commandContext_);
}

void EngineContext::syncScene() {
    const auto start = std::chrono::steady_clock::now();
    const bool rebuilt = simulation_.updateNavigation();
    if (simulation_.staticVersion() != physicsVersion_) {
        // One body per object so viewport clicks can identify what was hit.
        physics_->clear();
        bodyToEntity_.clear();
        for (sim::SceneId id : simulation_.staticObjectIds()) {
            const physics::BodyId body = physics_->addStaticMesh(simulation_.objectMesh(id));
            if (body.valid()) {
                bodyToEntity_[body.value] = id;
            }
        }
        physicsVersion_ = simulation_.staticVersion();
    }
    if (rebuilt) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        log().info("Scene sync: navmesh + flow fields + collision in {:.1f} ms", ms);
    }
}

std::optional<EntityHit> EngineContext::pick(glm::vec3 origin, glm::vec3 direction) const {
    std::optional<EntityHit> best;
    if (const auto hit = physics_->raycast(origin, direction, 5000.0f)) {
        if (const auto it = bodyToEntity_.find(hit->body.value); it != bodyToEntity_.end()) {
            best = EntityHit{it->second, hit->position, hit->distance};
        }
    }
    // Goals and spawners: intersect the ray with their rectangle's floor plane.
    const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    const glm::vec3 d = direction / len;
    auto tryArea = [&](sim::SceneId id, const sim::Area& area) {
        if (std::abs(d.y) < 1e-4f) {
            return;
        }
        const float t = (area.elevation + 0.05f - origin.y) / d.y;
        if (t <= 0.0f || (best && t >= best->distance + 0.5f)) {
            return;  // behind the camera, or clearly behind a hit object
        }
        const glm::vec3 p = origin + d * t;
        if (area.contains({p.x, p.z})) {
            best = EntityHit{id, p, t};
        }
    };
    for (sim::SceneId id : simulation_.goalIds()) {
        tryArea(id, simulation_.findGoal(id)->area);
    }
    for (sim::SceneId id : simulation_.spawnerIds()) {
        tryArea(id, simulation_.findSpawner(id)->area);
    }
    return best;
}

std::size_t EngineContext::reloadContent(bool force) {
    if (!config_.loadContent) {
        return 0;
    }
    std::size_t applied = 0;
    for (const ContentChange& change : contentWatcher_.poll(force)) {
        if (!change.error.empty()) {
            log().warn("Content: {}: {}", change.file.filename().string(), change.error);
            continue;
        }
        const commands::CommandOutcome outcome = execute(change.command);
        if (outcome.ok) {
            ++applied;
            if (!force) {
                log().info("Hot reload: {}", change.file.filename().string());
            }
        }
    }
    return applied;
}

std::uint64_t EngineContext::runPendingSteps(std::uint64_t maxSteps) {
    std::uint64_t ran = 0;
    while (control_.pendingSteps > 0 && ran < maxSteps) {
        simulation_.step();
        --control_.pendingSteps;
        ++ran;
    }
    return ran;
}

}  // namespace crowdsim::app
