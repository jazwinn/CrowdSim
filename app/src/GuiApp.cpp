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
#include <fstream>
#include <string>
#include <vector>

#include "EngineContext.hpp"
#include "crowdsim/animation/ProceduralCharacter.hpp"
#include "crowdsim/core/FixedStepClock.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/core/TripleBuffer.hpp"
#include "crowdsim/platform/Window.hpp"
#include "crowdsim/render/Camera.hpp"
#include "crowdsim/render/ImGuiRenderer.hpp"
#include "crowdsim/render/Renderer.hpp"
#include "crowdsim/render/SceneRenderer.hpp"
#include "crowdsim/sim/Scenarios.hpp"
#include "crowdsim/snapshot/CharacterAsset.hpp"
#include "crowdsim/snapshot/RenderSnapshot.hpp"
#include "crowdsim/ui/EditorShell.hpp"

namespace crowdsim::app {

namespace {

using commands::Json;
using commands::makeCommand;

constexpr std::array<sim::ScenarioKind, 5> kScenarios = {sim::ScenarioKind::Stream, sim::ScenarioKind::Counterflow,
                                                         sim::ScenarioKind::Benchmark, sim::ScenarioKind::Building,
                                                         sim::ScenarioKind::Plaza};

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

std::string timestamp() {
    return std::format("{:%Y%m%d_%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
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
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        camera.yaw += io.MouseDelta.x * kOrbitSpeed;
        camera.pitch = std::clamp(camera.pitch + io.MouseDelta.y * kOrbitSpeed, 0.05f, 1.55f);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        // Pan in the ground plane, scaled with distance so it feels constant on screen.
        const float k = camera.distance * 0.0015f;
        const float s = std::sin(camera.yaw);
        const float c = std::cos(camera.yaw);
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

/// Outline of the selected entity: triangle edges of objects, rectangles for areas.
std::vector<render::DebugLine> selectionLines(const sim::Simulation& sim, sim::SceneId id) {
    std::vector<render::DebugLine> lines;
    constexpr std::uint32_t kColor = 0xff20d0ffu;  // amber (ABGR)
    if (sim.findStaticObject(id) != nullptr) {
        const geometry::TriangleMesh mesh = sim.objectMesh(id);
        for (std::uint32_t t = 0; t < mesh.triangleCount() && lines.size() < 20000; ++t) {
            for (int k = 0; k < 3; ++k) {
                lines.push_back({mesh.positions[mesh.indices[3 * t + k]],
                                 mesh.positions[mesh.indices[3 * t + (k + 1) % 3]], kColor});
            }
        }
        return lines;
    }
    const sim::Area* area = nullptr;
    if (const sim::Goal* g = sim.findGoal(id)) {
        area = &g->area;
    } else if (const sim::Spawner* s = sim.findSpawner(id)) {
        area = &s->area;
    }
    if (area != nullptr) {
        const float y = area->elevation + 0.05f;
        const glm::vec2 c = area->center;
        const glm::vec2 h = area->halfExtents;
        const glm::vec3 p[4] = {
            {c.x - h.x, y, c.y - h.y}, {c.x + h.x, y, c.y - h.y}, {c.x + h.x, y, c.y + h.y}, {c.x - h.x, y, c.y + h.y}};
        for (int k = 0; k < 4; ++k) {
            lines.push_back({p[k], p[(k + 1) % 4], kColor});
        }
    }
    if (const sim::OffMeshLink* l = sim.findOffMeshLink(id)) {
        lines.push_back({l->start, l->end, kColor});
    }
    return lines;
}

/// Agent under a ray: the closest agent whose capsule (radius, 1.8 m tall) the ray
/// passes through. Returns {id, distance along the ray}.
std::optional<std::pair<std::uint32_t, float>> pickAgent(const sim::Simulation& sim, const Ray& ray) {
    const sim::AgentPool& a = sim.agents();
    std::optional<std::pair<std::uint32_t, float>> best;
    for (std::uint32_t i = 0; i < a.size(); ++i) {
        const glm::vec3 c(a.position[i].x, a.height[i] + 0.9f, a.position[i].y);
        const float t = glm::dot(c - ray.origin, ray.direction);
        if (t <= 0.0f || (best && t >= best->second)) {
            continue;
        }
        const glm::vec3 p = ray.origin + ray.direction * t;
        const float dx = p.x - c.x;
        const float dz = p.z - c.z;
        const float r = a.radius[i] + 0.1f;
        if (dx * dx + dz * dz <= r * r && std::abs(p.y - c.y) <= 0.9f) {
            best = std::pair{a.id[i], t};
        }
    }
    return best;
}

/// Ring around the selected agent plus a line to where it is heading.
void appendAgentHighlight(const sim::Simulation& sim, std::uint32_t agentId, std::vector<render::DebugLine>& lines) {
    const auto info = sim.agentDebug(agentId);
    if (!info) {
        return;
    }
    constexpr std::uint32_t kRing = 0xff20d0ffu;
    constexpr std::uint32_t kTarget = 0xff60ff60u;
    const glm::vec3 p = info->position + glm::vec3(0.0f, 0.05f, 0.0f);
    constexpr int kSegments = 24;
    const float r = 0.55f;
    for (int k = 0; k < kSegments; ++k) {
        const float a0 = 6.2831853f * static_cast<float>(k) / kSegments;
        const float a1 = 6.2831853f * static_cast<float>(k + 1) / kSegments;
        lines.push_back({p + glm::vec3(std::cos(a0) * r, 0.0f, std::sin(a0) * r),
                         p + glm::vec3(std::cos(a1) * r, 0.0f, std::sin(a1) * r), kRing});
    }
    if (info->moveMode == sim::MoveMode::Point) {
        lines.push_back({p, info->moveTarget + glm::vec3(0.0f, 0.05f, 0.0f), kTarget});
    }
}

std::vector<std::string> listContentFiles(const assets::AssetLibrary& assets, const char* dir, const char* suffix) {
    std::vector<std::string> files;
    std::error_code ec;
    const auto root = assets.contentRoot() / dir;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.ends_with(suffix)) {
            files.push_back(std::string(dir) + "/" + name);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool replacesScene(const std::string& command) {
    return command == "CreateScene" || command == "LoadScene" || command == "LoadBuiltinScenario" ||
           command == "ResetSimulation";
}

bool createsEntity(const std::string& command) {
    return command == "PlaceObject" || command == "ImportGltf" || command == "CreateGoal" ||
           command == "CreateSpawner" || command == "CreateOffMeshLink" || command == "AddEntity";
}

}  // namespace

int runGui(EngineContext& engine) {
    const AppConfig& config = engine.config();
    spdlog::logger& log = engine.log();
    sim::Simulation& simulation = engine.simulation();
    commands::SimControl& control = engine.control();
    control.playing = true;
    control.speed = config.simSpeed;

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
    sceneRenderer.setJobSystem(&engine.jobs());
    {
        // Procedural character, baked with ozz into a vertex animation texture.
        const auto bakeStart = std::chrono::steady_clock::now();
        auto character = std::make_shared<const snapshot::CharacterAsset>(animation::buildProceduralCharacter());
        engine.log().info(
            "Character '{}' baked: {} joints, {} clips, {}x{} VAT, LOD meshes {} / {} triangles in {:.1f} ms",
            character->name, character->jointCount, character->clips.size(), character->textureWidth,
            character->textureHeight, character->lods[0].indices.size() / 3, character->lods[1].indices.size() / 3,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bakeStart).count());
        sceneRenderer.setCharacter(std::move(character));
    }
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
        initialOverlays.shadows = !has("noshadows");
        initialOverlays.animatedCharacters = !has("capsules");
        if (config.lodDistances) {
            initialOverlays.lodNearDistance = (*config.lodDistances)[0];
            initialOverlays.lodFarDistance = (*config.lodDistances)[1];
        }
    }
    ui::EditorShell shell(initialOverlays);
    shell.sceneEditor().select(config.selectId);
    if (config.selectAgentId != ui::SceneEditor::kNoAgentSelected) {
        shell.sceneEditor().selectAgent(config.selectAgentId);
        shell.sceneEditor().followAgent = true;
    }
    render::Camera camera = defaultCamera(config);

    // Sim -> render hand-off. Producer and consumer share this thread today; the
    // triple buffer keeps the contract ready for a dedicated simulation thread.
    core::TripleBuffer<snapshot::RenderSnapshot> snapshots;
    auto publishSnapshot = [&] {
        simulation.writeSnapshot(snapshots.writeSlot());
        snapshots.publish();
    };
    publishSnapshot();

    std::vector<std::string_view> scenarioNames;
    for (sim::ScenarioKind kind : kScenarios) {
        scenarioNames.push_back(sim::scenarioName(kind));
    }
    int scenarioIndex = static_cast<int>(
        std::distance(kScenarios.begin(), std::find(kScenarios.begin(), kScenarios.end(), config.scenario)));
    std::uint32_t scenarioAgents = config.agents;
    std::vector<std::string> sceneFiles = listContentFiles(engine.assets(), "scenes", ".scene.json");

    core::FixedStepClock clock(config.sim.stepHz);
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

    // Selection highlight cache.
    sim::SceneId highlightId = sim::kNoSceneId;
    std::uint64_t highlightVersion = ~0ull;
    std::vector<render::DebugLine> highlight;

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    const auto loopStart = last;
    std::array<float, 600> recentFrameMs{};
    bool running = true;
    ImVec2 clickStart(0.0f, 0.0f);

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

        // --- Hot reload edited behaviour / agent-type files ----------------------------
        engine.reloadContent();

        // --- Simulate ---------------------------------------------------------------
        {
            std::uint32_t steps = control.playing ? clock.advance(dt * static_cast<double>(control.speed)) : 0;
            const auto simStart = Clock::now();
            for (std::uint32_t i = 0; i < steps; ++i) {
                simulation.step();
            }
            const std::uint64_t requested = engine.runPendingSteps(2000);  // Step / RunFor
            steps += static_cast<std::uint32_t>(requested);
            if (steps > 0) {
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - simStart).count();
                simStepMs = 0.9f * simStepMs + 0.1f * static_cast<float>(ms / steps);
                publishSnapshot();
            }
        }
        snapshots.acquire();
        const snapshot::RenderSnapshot& snap = snapshots.readSlot();
        const sim::SimStats stats = simulation.stats();

        // --- UI -----------------------------------------------------------------------
        logLines = engine.logService().tail(500);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiIO& io = ImGui::GetIO();
        const float fbScaleX = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
        const float fbScaleY = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;

        // Cursor picking against the collision world (Jolt raycast).
        bool hasCursor = false;
        glm::vec3 cursorWorld(0.0f);
        const float mouseX = io.MousePos.x * fbScaleX;
        const float mouseY = io.MousePos.y * fbScaleY;
        const bool mouseInViewport = mouseX >= viewport.x && mouseY >= viewport.y &&
                                     mouseX < viewport.x + viewport.width && mouseY < viewport.y + viewport.height;
        if (mouseInViewport) {
            const Ray ray = viewportRay(camera, viewport, mouseX, mouseY);
            if (const auto hit = engine.physics().raycast(ray.origin, ray.direction, 5000.0f)) {
                hasCursor = true;
                cursorWorld = hit->position;
            }
        }

        float view[16];
        float proj[16];
        render::cameraMatrices(camera, viewport.width / std::max(1.0f, viewport.height), view, proj);
        ui::SceneEditorInput editorInput{simulation, engine.dispatcher(), view, proj};
        editorInput.viewportX = viewport.x / fbScaleX;
        editorInput.viewportY = viewport.y / fbScaleY;
        editorInput.viewportW = viewport.width / fbScaleX;
        editorInput.viewportH = viewport.height / fbScaleY;
        editorInput.hasCursorWorld = hasCursor;
        editorInput.cursorWorld[0] = cursorWorld.x;
        editorInput.cursorWorld[1] = cursorWorld.y;
        editorInput.cursorWorld[2] = cursorWorld.z;
        editorInput.cameraTarget[0] = camera.target.x;
        editorInput.cameraTarget[1] = camera.target.y;
        editorInput.cameraTarget[2] = camera.target.z;

        const sim::NavMesh& nav = simulation.navMesh();
        ui::EditorRequests req = shell.draw(ui::EditorFrameData{
            .playing = control.playing,
            .stepIndex = simulation.stepIndex(),
            .simTimeSeconds = simulation.simTimeSeconds(),
            .stepHz = config.sim.stepHz,
            .stateHash = simulation.stateHash(),
            .simSpeed = control.speed,
            .seed = simulation.config().seed,
            .scenarioNames = scenarioNames,
            .scenarioIndex = scenarioIndex,
            .scenarioAgents = scenarioAgents,
            .agentsAlive = stats.alive,
            .agentsSpawned = stats.spawned,
            .agentsArrived = stats.arrived,
            .averageSpeed = stats.averageSpeed,
            .staticObjects = static_cast<std::uint32_t>(simulation.staticObjectIds().size()),
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
            .agentsNear = sceneStats.agentsNear,
            .agentsMid = sceneStats.agentsMid,
            .agentsFar = sceneStats.agentsFar,
            .agentsCulled = sceneStats.agentsCulled,
            .shadowCasters = sceneStats.shadowCasters,
            .crowdPrepareMs = sceneStats.prepareMs,
            .jobThreads = engine.jobs().threadCount(),
            .framebufferWidth = fb.width,
            .framebufferHeight = fb.height,
            .logLines = logLines,
            .editor = &editorInput,
            .sceneFiles = sceneFiles,
            .sceneName = engine.sceneName(),
        });
        updateCamera(camera, req.viewportHovered);

        // Click (press + release without dragging) in the viewport selects what is under the cursor.
        if (req.viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            clickStart = io.MousePos;
        }
        if (req.viewportHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && mouseInViewport) {
            const float moved = std::abs(io.MousePos.x - clickStart.x) + std::abs(io.MousePos.y - clickStart.y);
            if (moved < 4.0f) {
                const Ray ray = viewportRay(camera, viewport, mouseX, mouseY);
                const auto hit = engine.pick(ray.origin, ray.direction);
                const auto agent = pickAgent(simulation, ray);
                if (agent && (!hit || agent->second < hit->distance + 0.3f)) {
                    shell.sceneEditor().selectAgent(agent->first);
                } else {
                    shell.sceneEditor().select(hit ? hit->id : sim::kNoSceneId);
                }
            }
        }
        ImGui::Render();

        // --- Turn UI requests into commands and apply them between steps ---------------
        if (req.togglePlay) {
            req.commands.push_back(makeCommand(control.playing ? "Pause" : "Play"));
            clock.reset();
        }
        if (req.stepOnce && !control.playing) {
            req.commands.push_back(makeCommand("Step", {{"steps", 1}}));
        }
        if (req.reset) {
            req.commands.push_back(makeCommand("ResetSimulation"));
        }
        if (req.rebuildScenario) {
            scenarioIndex = std::clamp(req.scenarioIndex, 0, static_cast<int>(kScenarios.size()) - 1);
            scenarioAgents = req.scenarioAgents;
            req.commands.push_back(makeCommand(
                "LoadBuiltinScenario", {{"name", std::string(scenarioNames[static_cast<std::size_t>(scenarioIndex)])},
                                        {"agents", scenarioAgents}}));
        }
        if (req.simSpeed != control.speed) {
            req.commands.push_back(makeCommand("SetSimSpeed", {{"speed", req.simSpeed}}));
        }
        bool sceneReplaced = false;
        for (Json& cmd : req.commands) {
            engine.dispatcher().submit(std::move(cmd));
        }
        for (const commands::CommandOutcome& o : engine.dispatcher().flush(engine.commandContext())) {
            if (!o.ok) {
                continue;
            }
            if (replacesScene(o.command)) {
                sceneReplaced = true;
            }
            if (createsEntity(o.command) && o.result.contains("id")) {
                shell.sceneEditor().select(o.result["id"].get<sim::SceneId>());
            }
            if (o.command == "SaveScene") {
                sceneFiles = listContentFiles(engine.assets(), "scenes", ".scene.json");
            }
        }
        if (sceneReplaced) {
            engine.dispatcher().restartLog(engine.commandContext());
            clock.reset();
        }
        if (req.saveCommandLog) {
            const auto file =
                engine.assets().resolve("logs/" + engine.sceneName() + "_" + timestamp() + ".cmdlog.json");
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file) << engine.dispatcher().logToJson(engine.commandContext()).dump(2) << '\n';
            log.info("Command log saved to {}", file.string());
        }
        // Navmesh/collision rebuilds are deferred while a gizmo drag is in progress.
        if (!shell.sceneEditor().gizmoActive()) {
            engine.syncScene();
        }
        if (!req.commands.empty() || sceneReplaced) {
            publishSnapshot();
            snapshots.acquire();
        }
        if (req.frameAll) {
            camera = defaultCamera(config);
        }
        sceneRenderer.overlays = render::Overlays{
            .ground = req.overlays.ground,
            .areas = req.overlays.areas,
            .navmesh = req.overlays.navmesh,
            .flowField = req.overlays.flowField,
            .flowGoal = req.overlays.flowGoal,
            .links = req.overlays.links,
            .velocities = req.overlays.velocities,
        };
        sceneRenderer.settings.animatedCharacters = req.overlays.animatedCharacters;
        sceneRenderer.settings.shadows = req.overlays.shadows;
        sceneRenderer.settings.lodNearDistance = req.overlays.lodNearDistance;
        sceneRenderer.settings.lodFarDistance = req.overlays.lodFarDistance;
        if (req.viewport.width >= 1.0f && req.viewport.height >= 1.0f) {
            viewport = req.viewport;
        }
        if (req.quit) {
            running = false;
        }
        if (req.screenshot) {
            const auto file = engine.assets().resolve("../screenshots/crowdsim_" + timestamp() + ".png");
            std::filesystem::create_directories(file.parent_path());
            renderer.requestScreenshot(file.string());
        }

        // Selection outline (rebuilt when the selection or the scene changes).
        const sim::SceneId selected = shell.sceneEditor().selected();
        if (selected != highlightId || simulation.staticVersion() != highlightVersion || !req.commands.empty()) {
            highlight = selectionLines(simulation, selected);
            highlightId = selected;
            highlightVersion = simulation.staticVersion();
        }
        sceneRenderer.extraLines = highlight;
        if (const std::uint32_t agentId = shell.sceneEditor().selectedAgent();
            agentId != ui::SceneEditor::kNoAgentSelected) {
            appendAgentHighlight(simulation, agentId, sceneRenderer.extraLines);
            if (shell.sceneEditor().followAgent) {
                if (const auto info = simulation.agentDebug(agentId)) {
                    camera.target = glm::mix(camera.target, info->position, 0.15f);
                }
            }
        }

        // --- Render -------------------------------------------------------------------
        const auto renderStart = Clock::now();
        renderer.beginFrame();
        const float alpha = control.playing ? clock.alpha() : 1.0f;
        sceneStats = sceneRenderer.render(
            snapshots.readSlot(), alpha, camera,
            render::ViewRect{static_cast<std::uint16_t>(viewport.x), static_cast<std::uint16_t>(viewport.y),
                             static_cast<std::uint16_t>(viewport.width), static_cast<std::uint16_t>(viewport.height)},
            static_cast<float>(dt) * (control.playing ? control.speed : 0.0f));
        imguiRenderer.render(ImGui::GetDrawData());
        renderMs =
            0.9f * renderMs +
            0.1f * static_cast<float>(std::chrono::duration<double, std::milli>(Clock::now() - renderStart).count());
        renderer.endFrame();
        (void)snap;

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
    log.info(
        "Last frame: {} near, {} mid, {} far, {} culled characters, {} shadow casters (shadows {}), "
        "crowd prepare {:.3f} ms, {} draw calls",
        sceneStats.agentsNear, sceneStats.agentsMid, sceneStats.agentsFar, sceneStats.agentsCulled,
        sceneStats.shadowCasters, sceneRenderer.shadowsSupported() ? "supported" : "unsupported", sceneStats.prepareMs,
        sceneStats.drawCalls);
    // Steady-state report over the last frames (ignores start-up), e.g. at full population.
    const std::size_t recent = std::min<std::size_t>(recentFrameMs.size(), frameIndex > 1 ? frameIndex - 1 : 0);
    if (recent > 0) {
        std::vector<float> sorted;
        for (std::uint64_t f = frameIndex - recent; f < frameIndex; ++f) {
            sorted.push_back(recentFrameMs[f % recentFrameMs.size()]);
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
