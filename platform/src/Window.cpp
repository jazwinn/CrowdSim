#include "crowdsim/platform/Window.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace crowdsim::platform {

Window::Window(const WindowConfig& config) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    window_ = SDL_CreateWindow(config.title.c_str(), config.width, config.height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        const std::string error = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL_CreateWindow failed: " + error);
    }
}

Window::~Window() {
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

core::NativeWindow Window::nativeHandle() const {
    core::NativeWindow handle;
    const SDL_PropertiesID props = SDL_GetWindowProperties(window_);
#if defined(SDL_PLATFORM_WIN32)
    handle.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(SDL_PLATFORM_MACOS)
    handle.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(SDL_PLATFORM_LINUX)
    const std::string_view driver = SDL_GetCurrentVideoDriver();
    if (driver == "wayland") {
        handle.kind = core::NativeWindowKind::Wayland;
        handle.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        handle.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        handle.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        const auto xid =
            static_cast<std::uintptr_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        handle.window = reinterpret_cast<void*>(xid);
    }
#else
#error "Unsupported platform"
#endif
    if (handle.window == nullptr) {
        throw std::runtime_error("could not obtain native window handle from SDL");
    }
    return handle;
}

PixelSize Window::pixelSize() const {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

float Window::displayScale() const {
    const float scale = SDL_GetWindowDisplayScale(window_);
    return scale > 0.0f ? scale : 1.0f;
}

}  // namespace crowdsim::platform
