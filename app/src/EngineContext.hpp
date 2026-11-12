#pragma once

/// @file EngineContext.hpp
/// The composition root's service bundle. Built once in main(), owned on the
/// stack there, and handed by reference to everything else. There are no
/// singletons: if a module needs a service, it receives it from here.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "AppConfig.hpp"
#include "ContentWatcher.hpp"
#include "crowdsim/assets/AssetLibrary.hpp"
#include "crowdsim/commands/CommandDispatcher.hpp"
#include "crowdsim/core/JobSystem.hpp"
#include "crowdsim/core/Logging.hpp"
#include "crowdsim/mcp/CrowdSimTools.hpp"
#include "crowdsim/physics/PhysicsWorld.hpp"
#include "crowdsim/script/ScriptHost.hpp"
#include "crowdsim/sim/Simulation.hpp"

namespace crowdsim::app {

/// A raycast hit on a scene entity.
struct EntityHit {
    sim::SceneId id = sim::kNoSceneId;
    glm::vec3 position{0.0f};
    float distance = 0.0f;
};

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
    [[nodiscard]] assets::AssetLibrary& assets() noexcept { return assets_; }
    [[nodiscard]] commands::CommandDispatcher& dispatcher() noexcept { return dispatcher_; }
    [[nodiscard]] commands::CommandContext& commandContext() noexcept { return commandContext_; }
    [[nodiscard]] commands::SimControl& control() noexcept { return control_; }
    [[nodiscard]] const std::string& sceneName() const noexcept { return sceneName_; }
    [[nodiscard]] script::ScriptHost& scripts() noexcept { return scriptHost_; }

    /// Execute a command now and log a warning if it fails.
    commands::CommandOutcome execute(const commands::Json& command);

    /// Bring navigation and collision up to date with the simulation's scene.
    /// Call after scene edits (cheap when nothing changed).
    void syncScene();

    /// Closest scene entity under a ray: static objects via physics, goals and
    /// spawners via their floor rectangles.
    [[nodiscard]] std::optional<EntityHit> pick(glm::vec3 origin, glm::vec3 direction) const;

    /// Apply new or modified behaviour / agent-type files from the content dir
    /// as commands (hot reload). Cheap to call every frame; scans twice a second.
    /// Returns the number of files applied.
    std::size_t reloadContent(bool force = false);

    /// MCP tool services bound to this engine (call from the engine thread only).
    [[nodiscard]] mcp::McpServices mcpServices();

    /// Run requested steps (Step / RunFor), at most `maxSteps` now. Returns steps run.
    std::uint64_t runPendingSteps(std::uint64_t maxSteps);

private:
    // Declaration order is construction order: logging first, commands last.
    AppConfig config_;
    core::LogService logService_;
    core::JobSystem jobs_;
    physics::JoltRuntime joltRuntime_;
    std::unique_ptr<physics::IPhysicsWorld> physics_;
    sim::Simulation simulation_;
    assets::AssetLibrary assets_;
    commands::SimControl control_;
    std::string sceneName_ = "untitled";
    commands::CommandContext commandContext_;
    commands::CommandRegistry registry_;
    commands::CommandDispatcher dispatcher_;
    script::ScriptHost scriptHost_;  ///< Lua domain; detaches from the simulation before it is destroyed
    ContentWatcher contentWatcher_;

    std::uint64_t physicsVersion_ = ~0ull;
    std::unordered_map<std::uint32_t, sim::SceneId> bodyToEntity_;
};

}  // namespace crowdsim::app
