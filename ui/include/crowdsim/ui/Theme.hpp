#pragma once

/// @file Theme.hpp
/// The editor's look: palette, spacing, fonts, plus the small layout helpers the
/// panels share (property rows, read-outs, section headings) so every panel
/// lines up the same way.

#include <string>
#include <string_view>

struct ImFont;
struct ImVec4;

namespace crowdsim::ui {

/// Fonts loaded from the system (null members fall back to ImGui's default font).
struct ThemeFonts {
    ImFont* body = nullptr;    ///< proportional UI text
    ImFont* strong = nullptr;  ///< semibold: headings, entity names
    ImFont* mono = nullptr;    ///< code, log lines, hashes
};

/// Semantic colours used outside the ImGui style table (status text, badges).
namespace palette {
ImVec4 text();
ImVec4 textDim();
ImVec4 accent();
ImVec4 warning();
ImVec4 error();
}  // namespace palette

/// Style ImGui (and ImPlot, when its context exists) for the editor; `dpiScale`
/// scales spacing and font size.
void applyTheme(float dpiScale);
/// Style the current ImPlot context to match.
void applyPlotTheme();
/// Load UI fonts from well-known system locations (Windows, macOS, Linux).
ThemeFonts loadFonts();

/// "maxSpeedFactor" -> "Max speed factor", "rotationY" -> "Rotation Y".
std::string humanize(std::string_view key);

/// Property row: draws `label` dimmed on the left and positions the next widget
/// in the value column, filling the remaining width. Use "##id" widget labels.
void propertyLabel(const char* label);
/// Width of the label column in the current window.
float propertyColumn();

/// Two-column read-out table (dim label, value). Wrap rows in begin/end.
bool beginReadout(const char* id);
void readout(const char* label, const char* fmt, ...);
void endReadout();

/// Collapsible section title (semibold, no frame). The body is not indented;
/// there is no TreePop to call.
bool sectionToggle(const char* title, ImFont* strong, bool defaultOpen = false);

/// Quiet section heading with a hairline under it.
void sectionHeading(const char* text, const ThemeFonts& fonts);

}  // namespace crowdsim::ui
