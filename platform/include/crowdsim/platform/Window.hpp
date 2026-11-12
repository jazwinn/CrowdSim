#pragma once

/// @file Window.hpp
/// RAII wrapper over SDL3 video initialisation and one application window.

#include <cstdint>
#include <string>

#include "crowdsim/core/NativeWindow.hpp"

struct SDL_Window;

namespace crowdsim::platform {

struct WindowConfig {
    std::string title = "CrowdSim";
    int width = 1600;
    int height = 900;
};

struct PixelSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// Owns SDL's video subsystem and the main window. Construct exactly one, in
/// the composition root. Throws std::runtime_error on failure.
class Window {
public:
    explicit Window(const WindowConfig& config);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// Handles the renderer needs to create its swap chain.
    [[nodiscard]] core::NativeWindow nativeHandle() const;

    /// Drawable size in pixels (differs from window size on high-DPI displays).
    [[nodiscard]] PixelSize pixelSize() const;

    /// Content scale (1.0 on standard displays, 2.0 on Retina, etc.).
    [[nodiscard]] float displayScale() const;

    /// Raw SDL handle, for the ImGui SDL3 backend only.
    [[nodiscard]] SDL_Window* sdlWindow() const noexcept { return window_; }

private:
    SDL_Window* window_ = nullptr;
};

}  // namespace crowdsim::platform
