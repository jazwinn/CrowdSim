#include "GuiApp.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "EngineContext.hpp"
#include "crowdsim/core/FixedStepClock.hpp"
#include "crowdsim/core/Profile.hpp"
#include "crowdsim/platform/Window.hpp"
#include "crowdsim/render/ImGuiRenderer.hpp"
#include "crowdsim/render/Renderer.hpp"
#include "crowdsim/ui/EditorShell.hpp"

namespace crowdsim::app {

namespace {

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
                                   });
    SDL_SetWindowTitle(window.sdlWindow(), std::format("CrowdSim - {}", renderer.backendName()).c_str());

    ImGuiSession imguiSession(window.sdlWindow(), window.displayScale());
    render::ImGuiRenderer imguiRenderer(renderer, render::kViewImGui);
    ui::EditorShell shell;

    core::FixedStepClock clock(config.sim.stepHz);
    bool playing = true;
    float simSpeed = 1.0f;
    float frameMs = 0.0f;
    float simStepMs = 0.0f;
    std::uint64_t frameIndex = 0;
    // Screenshots complete a couple of frames after the request, so --screenshot
    // requests early enough for the capture to land before --frames exits.
    constexpr std::uint64_t kScreenshotLatencyFrames = 3;
    std::vector<std::string> logLines;

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    const auto loopStart = last;
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

        if (playing) {
            const std::uint32_t steps = clock.advance(dt * static_cast<double>(simSpeed));
            if (steps > 0) {
                const auto simStart = Clock::now();
                for (std::uint32_t i = 0; i < steps; ++i) {
                    simulation.step();
                }
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - simStart).count();
                simStepMs = static_cast<float>(ms / steps);
            }
        }

        // UI
        logLines = engine.logService().tail(500);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ui::EditorRequests req = shell.draw(ui::EditorFrameData{
            .playing = playing,
            .stepIndex = simulation.stepIndex(),
            .simTimeSeconds = simulation.simTimeSeconds(),
            .stepHz = config.sim.stepHz,
            .stateHash = simulation.stateHash(),
            .simSpeed = simSpeed,
            .seed = config.sim.seed,
            .frameMs = frameMs,
            .simStepMs = simStepMs,
            .rendererName = renderer.backendName(),
            .jobThreads = engine.jobs().threadCount(),
            .framebufferWidth = fb.width,
            .framebufferHeight = fb.height,
            .logLines = logLines,
        });
        ImGui::Render();

        // Apply UI requests (these become commands in M3).
        if (req.togglePlay) {
            playing = !playing;
            clock.reset();
        }
        if (req.stepOnce && !playing) {
            simulation.step();
        }
        if (req.reset) {
            simulation.reset();
            clock.reset();
        }
        simSpeed = req.simSpeed;
        if (req.quit) {
            running = false;
        }
        if (req.screenshot) {
            renderer.requestScreenshot(timestampedScreenshotPath());
        }

        // Render
        renderer.beginFrame();
        imguiRenderer.render(ImGui::GetDrawData());
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
    log.info("Exiting after {} frames ({:.2f} ms/frame avg), {} sim steps", frameIndex,
             frameIndex > 0 ? totalMs / static_cast<double>(frameIndex) : 0.0, simulation.stepIndex());
    return 0;
}

}  // namespace crowdsim::app
