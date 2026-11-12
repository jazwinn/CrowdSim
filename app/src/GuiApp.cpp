#include "GuiApp.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "EngineContext.hpp"
#include "crowdsim/core/FixedStepClock.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/core/TripleBuffer.hpp"
#include "crowdsim/platform/Window.hpp"
#include "crowdsim/render/Camera.hpp"
#include "crowdsim/render/ImGuiRenderer.hpp"
#include "crowdsim/render/Renderer.hpp"
#include "crowdsim/render/SceneRenderer.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"
#include "crowdsim/ui/EditorShell.hpp"

namespace crowdsim::app {

namespace {

constexpr std::array<sim::ScenarioKind, 4> kScenarios = {sim::ScenarioKind::Stream, sim::ScenarioKind::Counterflow,
                                                         sim::ScenarioKind::Benchmark, sim::ScenarioKind::Building};

/// RAII owner of the ImGui context and its SDL3 platform backend. ImGui keeps
/// a "current context" pointer internally; we create exactly one and destroy
/// it deterministically here.
class ImGuiSession {
public:
    ImGuiSession(SDL_Window* window, float contentScale) {
        IMGUI_CHECKVERSION();
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        io.IniFilename = "imgui.ini";
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.ScaleAllSizes(contentScale);
        style.FontScaleDpi = contentScale;
        ImGui_ImplSDL3_InitForOther(window);
    }
    ~ImGuiSession() {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(context_);
    }
    ImGuiSession(const ImGuiSession&) = delete;
    ImGuiSession& operator=(const ImGuiSession&) = delete;

private:
    ImGuiContext* context_ = nullptr;
};

std::string timestampedScreenshotPath() {
    std::filesystem::create_directories("screenshots");
    const auto now = std::chrono::system_clock::now();
    return std::format("screenshots/crowdsim_{:%Y%m%d_%H%M%S}.png", std::chrono::floor<std::chrono::seconds>(now));
}

render::Camera defaultCamera(const AppConfig& config) {
    render::Camera camera;
    camera.target = {0.0f, 0.0f, 0.0f};
    camera.yaw = 0.7f;
    camera.pitch = 0.62f;
    camera.distance = 150.0f;
    if (config.camera) {
        constexpr float kDegToRad = 0.017453292f;
        const auto& c = *config.camera;
        camera.target = {c[0], 0.0f, c[1]};
        camera.distance = c[2];
        camera.yaw = c[3] * kDegToRad;
        camera.pitch = c[4] * kDegToRad;
    }
    return camera;
}

/// Orbit / pan / zoom from ImGui's mouse state while the viewport is hovered.
void updateCamera(render::Camera& camera, bool viewportHovered) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!viewportHovered) {
        return;
    }
    constexpr float kOrbitSpeed = 0.006f;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        camera.yaw += io.MouseDelta.x * kOrbitSpeed;
        camera.pitch = std::clamp(camera.pitch + io.MouseDelta.y * kOrbitSpeed, 0.05f, 1.55f);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        // Pan in the ground plane, scaled with distance so it feels constant on screen.
        const float k = camera.distance * 0.0015f;
        const float s = std::sin(camera.yaw);
        const float c = std::cos(camera.yaw);
        // Screen-right in world XZ is (cos yaw, -sin yaw); screen-up maps to towards the target.
        camera.target.x += (-io.MouseDelta.x * c + io.MouseDelta.y * s) * k;
        camera.target.z += (io.MouseDelta.x * s + io.MouseDelta.y * c) * k;
    }
    if (io.MouseWheel != 0.0f) {
        camera.distance = std::clamp(camera.distance * std::pow(0.88f, io.MouseWheel), 2.0f, 1500.0f);
    }
}

/// World-space ray through a framebuffer pixel of the 3D viewport.
struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

Ray viewportRay(const render::Camera& camera, const ui::PixelRect& viewport, float px, float py) {
    const glm::vec3 eye = camera.eye();
    const glm::vec3 forward = glm::normalize(camera.target - eye);
    // Left-handed, +Y up (matches bx::mtxLookAt): right = up x forward.
    const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), forward));
    const glm::vec3 up = glm::cross(forward, right);
    const float ndcX = 2.0f * (px - viewport.x) / viewport.width - 1.0f;
    const float ndcY = 1.0f - 2.0f * (py - viewport.y) / viewport.height;
    const float tanHalf = std::tan(glm::radians(camera.fovYDegrees) * 0.5f);
    const float aspect = viewport.width / viewport.height;
    return {eye, glm::normalize(forward + right * (ndcX * tanHalf * aspect) + up * (ndcY * tanHalf))};
}

}  // namespace

int runGui(EngineContext& engine) {
    const AppConfig& config = engine.config();
    spdlog::logger& log = engine.log();
    sim::Simulation& simulation = engine.simulation();

    platform::Window window(platform::WindowConfig{"CrowdSim", config.width, config.height});
    platform::PixelSize fb = window.pixelSize();

    render::Renderer renderer(log, render::RendererConfig{
                                       .window = window.nativeHandle(),
                                       .width = fb.width,
                                       .height = fb.height,
                                       .backend = config.renderer,
                                       .vsync = config.vsync,
                                   });
    SDL_SetWindowTitle(window.sdlWindow(), std::format("CrowdSim - {}", renderer.backendName()).c_str());

    ImGuiSession imguiSession(window.sdlWindow(), window.displayScale());
    render::ImGuiRenderer imguiRenderer(renderer, render::kViewImGui);
    render::SceneRenderer sceneRenderer;
    ui::OverlayToggles initialOverlays;
    {
        const std::string& list = config.overlays;
        auto has = [&](std::string_view name) { return list.find(name) != std::string::npos; };
        initialOverlays.navmesh = has("navmesh");
        initialOverlays.flowField = has("flow");
        initialOverlays.velocities = has("velocities");
        initialOverlays.ground = !has("nogrid");
        initialOverlays.areas = !has("noareas");
        initialOverlays.links = !has("nolinks");
    }
    ui::EditorShell shell(initialOverlays);
    render::Camera camera = defaultCamera(config);

    // Sim -> render hand-off. Producer and consumer share this thread today; the
    // triple buffer keeps the contract ready for a dedicated simulation thread.
    core::TripleBuffer<snapshot::RenderSnapshot> snapshots;
    simulation.writeSnapshot(snapshots.writeSlot());
    snapshots.publish();

    std::vector<std::string_view> scenarioNames;
    for (sim::ScenarioKind kind : kScenarios) {
        scenarioNames.push_back(sim::scenarioName(kind));
    }
    int scenarioIndex = static_cast<int>(
        std::distance(kScenarios.begin(), std::find(kScenarios.begin(), kScenarios.end(), config.scenario)));
    std::uint32_t scenarioAgents = config.agents;

    core::FixedStepClock clock(config.sim.stepHz);
    bool playing = true;
    float simSpeed = config.simSpeed;
    float frameMs = 0.0f;
    float simStepMs = 0.0f;
    float renderMs = 0.0f;
    render::SceneStats sceneStats;
    std::uint64_t frameIndex = 0;
    // Screenshots complete a couple of frames after the request, so --screenshot
    // requests early enough for the capture to land before --frames exits.
    constexpr std::uint64_t kScreenshotLatencyFrames = 3;
    std::vector<std::string> logLines;
    ui::PixelRect viewport{0.0f, 0.0f, static_cast<float>(fb.width), static_cast<float>(fb.height)};

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    const auto loopStart = last;
    // Wall-clock frame times of the most recent frames, for the exit report.
    std::array<float, 600> recentFrameMs{};
    bool running = true;

    while (running) {
        CS_PROFILE_FRAME();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                 event.window.windowID == SDL_GetWindowID(window.sdlWindow()))) {
                running = false;
            }
        }
        if (SDL_GetWindowFlags(window.sdlWindow()) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            last = Clock::now();
            continue;
        }

        const platform::PixelSize size = window.pixelSize();
        if (size.width != fb.width || size.height != fb.height) {
            fb = size;
            renderer.resize(fb.width, fb.height);
        }

        // Wall-clock time drives the accumulator; the simulation only sees fixed steps.
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        frameMs = 0.9f * frameMs + 0.1f * static_cast<float>(dt * 1000.0);
        if (frameIndex > 0) {
            recentFrameMs[frameIndex % recentFrameMs.size()] = static_cast<float>(dt * 1000.0);
        }

        if (playing) {
            const std::uint32_t steps = clock.advance(dt * static_cast<double>(simSpeed));
            if (steps > 0) {
                const auto simStart = Clock::now();
                for (std::uint32_t i = 0; i < steps; ++i) {
                    simulation.step();
                }
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - simStart).count();
                simStepMs = 0.9f * simStepMs + 0.1f * static_cast<float>(ms / steps);
                simulation.writeSnapshot(snapshots.writeSlot());
                snapshots.publish();
            }
        }
        snapshots.acquire();
        const snapshot::RenderSnapshot& snap = snapshots.readSlot();
        const sim::SimStats stats = simulation.stats();

        // UI
        logLines = engine.logService().tail(500);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        // Cursor picking against the collision world (Jolt raycast).
        bool hasCursor = false;
        glm::vec3 cursorWorld(0.0f);
        {
            const ImGuiIO& io = ImGui::GetIO();
            const float px = io.MousePos.x * io.DisplayFramebufferScale.x;
            const float py = io.MousePos.y * io.DisplayFramebufferScale.y;
            if (px >= viewport.x && py >= viewport.y && px < viewport.x + viewport.width &&
                py < viewport.y + viewport.height) {
                const Ray ray = viewportRay(camera, viewport, px, py);
                if (const auto hit = engine.physics().raycast(ray.origin, ray.direction, 5000.0f)) {
                    hasCursor = true;
                    cursorWorld = hit->position;
                }
            }
        }
        const sim::NavMesh& nav = simulation.navMesh();

        const ui::EditorRequests req = shell.draw(ui::EditorFrameData{
            .playing = playing,
            .stepIndex = simulation.stepIndex(),
            .simTimeSeconds = simulation.simTimeSeconds(),
            .stepHz = config.sim.stepHz,
            .stateHash = simulation.stateHash(),
            .simSpeed = simSpeed,
            .seed = config.sim.seed,
            .scenarioNames = scenarioNames,
            .scenarioIndex = scenarioIndex,
            .scenarioAgents = scenarioAgents,
            .agentsAlive = stats.alive,
            .agentsSpawned = stats.spawned,
            .agentsArrived = stats.arrived,
            .averageSpeed = stats.averageSpeed,
            .staticObjects = static_cast<std::uint32_t>(simulation.staticMesh().triangleCount() > 0
                                                            ? simulation.registry().view<sim::StaticObject>().size()
                                                            : 0),
            .navPolygons = nav.polyCount(),
            .navTiles = nav.tileCount(),
            .goals = static_cast<std::uint32_t>(simulation.flowFields().size()),
            .hasCursorWorld = hasCursor,
            .cursorWorld = {cursorWorld.x, cursorWorld.y, cursorWorld.z},
            .frameMs = frameMs,
            .simStepMs = simStepMs,
            .renderSubmitMs = renderMs,
            .rendererName = renderer.backendName(),
            .agentsDrawn = sceneStats.agentsDrawn,
            .drawCalls = sceneStats.drawCalls,
            .jobThreads = engine.jobs().threadCount(),
            .framebufferWidth = fb.width,
            .framebufferHeight = fb.height,
            .logLines = logLines,
        });
        updateCamera(camera, req.viewportHovered);
        ImGui::Render();

        // Apply UI requests (these become commands in M3).
        if (req.togglePlay) {
            playing = !playing;
            clock.reset();
        }
        if (req.stepOnce && !playing) {
            simulation.step();
            simulation.writeSnapshot(snapshots.writeSlot());
            snapshots.publish();
        }
        if (req.reset) {
            simulation.reset();
            clock.reset();
            simulation.writeSnapshot(snapshots.writeSlot());
            snapshots.publish();
        }
        if (req.rebuildScenario) {
            scenarioIndex = std::clamp(req.scenarioIndex, 0, static_cast<int>(kScenarios.size()) - 1);
            scenarioAgents = req.scenarioAgents;
            sim::buildScenario(simulation, kScenarios[static_cast<std::size_t>(scenarioIndex)], scenarioAgents);
            engine.syncScene();
            log.info("Loaded scenario '{}' with {} agents", scenarioNames[static_cast<std::size_t>(scenarioIndex)],
                     scenarioAgents);
            clock.reset();
            simulation.writeSnapshot(snapshots.writeSlot());
            snapshots.publish();
        }
        if (req.frameAll) {
            camera = defaultCamera(config);
        }
        simSpeed = req.simSpeed;
        sceneRenderer.overlays = render::Overlays{
            .ground = req.overlays.ground,
            .areas = req.overlays.areas,
            .navmesh = req.overlays.navmesh,
            .flowField = req.overlays.flowField,
            .flowGoal = req.overlays.flowGoal,
            .links = req.overlays.links,
            .velocities = req.overlays.velocities,
        };
        if (req.viewport.width >= 1.0f && req.viewport.height >= 1.0f) {
            viewport = req.viewport;
        }
        if (req.quit) {
            running = false;
        }
        if (req.screenshot) {
            renderer.requestScreenshot(timestampedScreenshotPath());
        }

        // Render
        const auto renderStart = Clock::now();
        renderer.beginFrame();
        const float alpha = playing ? clock.alpha() : 1.0f;
        sceneStats = sceneRenderer.render(
            snap, alpha, camera,
            render::ViewRect{static_cast<std::uint16_t>(viewport.x), static_cast<std::uint16_t>(viewport.y),
                             static_cast<std::uint16_t>(viewport.width), static_cast<std::uint16_t>(viewport.height)});
        imguiRenderer.render(ImGui::GetDrawData());
        renderMs =
            0.9f * renderMs +
            0.1f * static_cast<float>(std::chrono::duration<double, std::milli>(Clock::now() - renderStart).count());
        renderer.endFrame();

        ++frameIndex;
        if (config.frames > 0) {
            if (!config.screenshotPath.empty() &&
                frameIndex ==
                    std::max<std::uint64_t>(
                        1, config.frames > kScreenshotLatencyFrames ? config.frames - kScreenshotLatencyFrames : 1)) {
                renderer.requestScreenshot(config.screenshotPath);
            }
            if (frameIndex >= config.frames) {
                running = false;
            }
        }
    }

    const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - loopStart).count();
    log.info("Exiting after {} frames ({:.2f} ms/frame avg), {} sim steps, {} agents alive", frameIndex,
             frameIndex > 0 ? totalMs / static_cast<double>(frameIndex) : 0.0, simulation.stepIndex(),
             simulation.agents().size());
    // Steady-state report over the last frames (ignores start-up), e.g. at full population.
    const std::size_t recent = std::min<std::size_t>(recentFrameMs.size(), frameIndex > 1 ? frameIndex - 1 : 0);
    if (recent > 0) {
        std::vector<float> sorted(recentFrameMs.begin(), recentFrameMs.end());
        if (recent < sorted.size()) {
            sorted.clear();
            for (std::uint64_t f = 1; f < frameIndex; ++f) {
                sorted.push_back(recentFrameMs[f % recentFrameMs.size()]);
            }
        }
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (float v : sorted) {
            sum += v;
        }
        const float p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
        log.info("Last {} frames: avg {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms", sorted.size(),
                 sum / static_cast<double>(sorted.size()), p99, sorted.back());
    }
    return 0;
}

}  // namespace crowdsim::app
