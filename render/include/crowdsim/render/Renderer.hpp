#pragma once

/// @file Renderer.hpp
/// Owns the bgfx device for one window. Rendering reads simulation snapshots
/// (from M1) and never touches simulation state directly.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "crowdsim/core/NativeWindow.hpp"

namespace spdlog {
class logger;
}

namespace crowdsim::render {

/// bgfx view ids, in submission order.
inline constexpr std::uint16_t kViewBackground = 0;  ///< full-framebuffer clear
inline constexpr std::uint16_t kViewShadow = 1;      ///< directional-light shadow map (depth only)
inline constexpr std::uint16_t kViewScene = 2;       ///< 3D scene, restricted to the viewport rect
inline constexpr std::uint16_t kViewCapture = 3;     ///< copies of the scene for video recording
inline constexpr std::uint16_t kViewImGui = 255;

struct RendererConfig {
    core::NativeWindow window;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string backend = "auto";  ///< auto | d3d11 | d3d12 | vulkan | metal | opengl | noop
    bool vsync = true;
    std::uint32_t clearColor = 0x262a30ff;  ///< RGBA
};

class Renderer {
public:
    /// Initialises bgfx. Throws std::runtime_error on failure.
    Renderer(spdlog::logger& log, const RendererConfig& config);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Call when the drawable size changes.
    void resize(std::uint32_t width, std::uint32_t height);

    /// Prepare views for a new frame.
    void beginFrame();

    /// Submit the frame to the GPU. Returns bgfx's frame number (for read-backs).
    std::uint32_t endFrame();

    /// Save the next completed frame as a PNG at `path`.
    void requestScreenshot(const std::string& path);

    [[nodiscard]] std::string_view backendName() const;
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct Callback;
    spdlog::logger& log_;
    std::unique_ptr<Callback> callback_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t resetFlags_;
    std::uint32_t clearColor_;
};

}  // namespace crowdsim::render
