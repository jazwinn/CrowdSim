#include "crowdsim/render/Renderer.hpp"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bimg/bimg.h>
#include <bx/file.h>
#include <spdlog/logger.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace crowdsim::render {

namespace {

bgfx::RendererType::Enum parseBackend(std::string_view name) {
    if (name == "d3d11")
        return bgfx::RendererType::Direct3D11;
    if (name == "d3d12")
        return bgfx::RendererType::Direct3D12;
    if (name == "vulkan")
        return bgfx::RendererType::Vulkan;
    if (name == "metal")
        return bgfx::RendererType::Metal;
    if (name == "opengl")
        return bgfx::RendererType::OpenGL;
    if (name == "noop")
        return bgfx::RendererType::Noop;
    if (name == "auto")
        return bgfx::RendererType::Count;  // bgfx picks the platform default
    throw std::runtime_error("unknown renderer backend '" + std::string(name) + "'");
}

}  // namespace

/// Routes bgfx diagnostics into our logger and writes screenshots as PNG.
struct Renderer::Callback final : public bgfx::CallbackI {
    explicit Callback(spdlog::logger& logger) : log(logger) {}

    void fatal(const char* filePath, std::uint16_t line, bgfx::Fatal::Enum code, const char* str) override {
        log.critical("bgfx fatal {:#x} at {}:{}: {}", static_cast<int>(code), filePath, line, str);
        if (code != bgfx::Fatal::DebugCheck) {
            std::abort();
        }
    }

    void traceVargs(const char* filePath, std::uint16_t line, const char* format, va_list args) override {
        char buffer[1024];
        const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
        if (n <= 0) {
            return;
        }
        std::string_view msg(buffer, static_cast<std::size_t>(std::min<int>(n, sizeof(buffer) - 1)));
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
            msg.remove_suffix(1);
        }
        log.trace("bgfx {}:{}: {}", filePath, line, msg);
    }

    void profilerBegin(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerBeginLiteral(const char*, std::uint32_t, const char*, std::uint16_t) override {}
    void profilerEnd() override {}
    std::uint32_t cacheReadSize(std::uint64_t) override { return 0; }
    bool cacheRead(std::uint64_t, void*, std::uint32_t) override { return false; }
    void cacheWrite(std::uint64_t, const void*, std::uint32_t) override {}

    void screenShot(const char* filePath, std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
                    const void* data, std::uint32_t /*size*/, bool yflip) override {
        // bgfx always delivers screenshots as BGRA8.
        bx::FileWriter writer;
        bx::Error err;
        if (!bx::open(&writer, filePath, false, &err)) {
            log.error("screenshot: cannot open '{}' for writing", filePath);
            return;
        }
        bimg::imageWritePng(&writer, width, height, pitch, data, bimg::TextureFormat::BGRA8, yflip, &err);
        bx::close(&writer);
        if (err.isOk()) {
            log.info("screenshot saved: {} ({}x{})", filePath, width, height);
        } else {
            log.error("screenshot: failed to encode '{}'", filePath);
        }
    }

    void captureBegin(std::uint32_t, std::uint32_t, std::uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, std::uint32_t) override {}

    spdlog::logger& log;
};

Renderer::Renderer(spdlog::logger& log, const RendererConfig& config)
    : log_(log),
      callback_(std::make_unique<Callback>(log)),
      width_(config.width),
      height_(config.height),
      resetFlags_(config.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE),
      clearColor_(config.clearColor) {
    // Calling renderFrame() before init selects bgfx's single-threaded mode: the
    // render thread is ours. The simulation runs on job threads, not here.
    bgfx::renderFrame();

    bgfx::Init init;
    init.type = parseBackend(config.backend);
    init.platformData.nwh = config.window.window;
    init.platformData.ndt = config.window.display;
    init.platformData.type = config.window.kind == core::NativeWindowKind::Wayland
                                 ? bgfx::NativeWindowHandleType::Wayland
                                 : bgfx::NativeWindowHandleType::Default;
    init.resolution.width = width_;
    init.resolution.height = height_;
    init.resolution.reset = resetFlags_;
    init.callback = callback_.get();

    if (!bgfx::init(init)) {
        throw std::runtime_error("bgfx::init failed for backend '" + config.backend + "'");
    }
    log_.info("Renderer: {} ({}x{})", backendName(), width_, height_);

    bgfx::setViewClear(kViewBackground, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clearColor_, 1.0f, 0);
    bgfx::setViewName(kViewBackground, "Background");
    bgfx::setViewClear(kViewScene, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clearColor_, 1.0f, 0);
    bgfx::setViewName(kViewScene, "Scene");
    // Submission order matters for ground decals; opaque geometry relies on the depth buffer.
    bgfx::setViewMode(kViewScene, bgfx::ViewMode::Sequential);
    bgfx::setViewName(kViewCapture, "Capture");
}

Renderer::~Renderer() {
    bgfx::shutdown();
}

void Renderer::resize(std::uint32_t width, std::uint32_t height) {
    if (width == width_ && height == height_) {
        return;
    }
    width_ = width;
    height_ = height;
    bgfx::reset(width_, height_, resetFlags_);
}

void Renderer::beginFrame() {
    bgfx::setViewRect(kViewBackground, 0, 0, static_cast<std::uint16_t>(width_), static_cast<std::uint16_t>(height_));
    bgfx::touch(kViewBackground);  // clear even when nothing is submitted
    bgfx::touch(kViewScene);
}

std::uint32_t Renderer::endFrame() {
    return bgfx::frame();
}

void Renderer::requestScreenshot(const std::string& path) {
    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path.c_str());
}

std::string_view Renderer::backendName() const {
    return bgfx::getRendererName(bgfx::getRendererType());
}

}  // namespace crowdsim::render
