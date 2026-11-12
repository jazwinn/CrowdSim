#pragma once

/// @file NativeWindow.hpp
/// OS window handles passed from platform/ to render/ so neither module has to
/// know about the other.

namespace crowdsim::core {

enum class NativeWindowKind {
    Default,  ///< Win32 HWND, Cocoa NSWindow, or X11 Window
    Wayland,  ///< wl_surface + wl_display
};

struct NativeWindow {
    void* window = nullptr;   ///< HWND / NSWindow* / X11 Window (as pointer) / wl_surface*
    void* display = nullptr;  ///< X11 Display* / wl_display*; null elsewhere
    NativeWindowKind kind = NativeWindowKind::Default;
};

}  // namespace crowdsim::core
