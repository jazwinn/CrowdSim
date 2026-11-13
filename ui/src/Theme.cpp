#include "crowdsim/ui/Theme.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace crowdsim::ui {

namespace {

// Palette: graphite panels that recede behind the 3D view, with one accent taken
// from wayfinding signage (exit green) for selection and the active state.
constexpr ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a};
}
constexpr ImVec4 kPanel = rgb(32, 35, 40);       // window background
constexpr ImVec4 kChrome = rgb(25, 27, 31);      // title bars, menu bar, tab strip
constexpr ImVec4 kWell = rgb(21, 23, 27);        // input wells (recessed)
constexpr ImVec4 kRaised = rgb(44, 48, 55);      // buttons, headers
constexpr ImVec4 kRaisedHover = rgb(54, 59, 67);
constexpr ImVec4 kRaisedActive = rgb(64, 70, 79);
constexpr ImVec4 kHairline = rgb(52, 56, 63);
constexpr ImVec4 kText = rgb(226, 229, 233);
constexpr ImVec4 kTextDim = rgb(138, 146, 156);
constexpr ImVec4 kAccent = rgb(63, 178, 127);    // exit-sign green
constexpr ImVec4 kAccentSoft = rgb(63, 178, 127, 0.22f);
constexpr ImVec4 kAccentHover = rgb(63, 178, 127, 0.34f);
constexpr ImVec4 kWarning = rgb(224, 163, 58);
constexpr ImVec4 kError = rgb(229, 100, 90);

// Categorical colours: the accent first, then hues that stay apart on the dark panels.
constexpr ImVec4 kSeries[] = {kAccent,           rgb(98, 151, 219),  kWarning,
                              rgb(176, 132, 214), kError,             rgb(120, 200, 205),
                              rgb(214, 196, 92),  rgb(222, 126, 176)};

constexpr float kBaseFontSize = 15.0f;

ImFont* tryLoad(std::initializer_list<const char*> candidates, float size) {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, size, &cfg); font != nullptr) {
                return font;
            }
        }
    }
    return nullptr;
}

}  // namespace

namespace palette {
ImVec4 text() {
    return kText;
}
ImVec4 textDim() {
    return kTextDim;
}
ImVec4 accent() {
    return kAccent;
}
ImVec4 warning() {
    return kWarning;
}
ImVec4 error() {
    return kError;
}
ImVec4 series(std::size_t i) {
    return kSeries[i % std::size(kSeries)];
}
}  // namespace palette

void applyTheme(float dpiScale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();

    // Geometry: frames and buttons get a small radius, structural surfaces none,
    // so controls read as touchable and panels as part of the frame.
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 5);
    s.CellPadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 14;
    s.ScrollbarSize = 11;
    s.GrabMinSize = 10;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.TabBarBorderSize = 1;
    s.TabBarOverlineSize = 2;
    s.WindowRounding = 0;
    s.ChildRounding = 4;
    s.FrameRounding = 4;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 3;
    s.TabRounding = 4;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(0, 6);
    s.DockingSeparatorSize = 2;
    s.WindowMenuButtonPosition = ImGuiDir_None;  // the dock-node arrow added noise to every tab strip

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_WindowBg] = kPanel;
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(38, 42, 48, 0.98f);
    c[ImGuiCol_Border] = kHairline;
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kWell;
    c[ImGuiCol_FrameBgHovered] = rgb(29, 32, 37);
    c[ImGuiCol_FrameBgActive] = rgb(33, 37, 43);
    c[ImGuiCol_TitleBg] = kChrome;
    c[ImGuiCol_TitleBgActive] = kChrome;
    c[ImGuiCol_TitleBgCollapsed] = kChrome;
    c[ImGuiCol_MenuBarBg] = kChrome;
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(70, 76, 85);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(90, 97, 107);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(110, 118, 129);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = rgb(92, 204, 152);
    c[ImGuiCol_Button] = kRaised;
    c[ImGuiCol_ButtonHovered] = kRaisedHover;
    c[ImGuiCol_ButtonActive] = kRaisedActive;
    c[ImGuiCol_Header] = kAccentSoft;  // selected list rows
    c[ImGuiCol_HeaderHovered] = rgb(255, 255, 255, 0.06f);
    c[ImGuiCol_HeaderActive] = kAccentHover;
    c[ImGuiCol_Separator] = kHairline;
    c[ImGuiCol_SeparatorHovered] = kAccent;
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = kAccentSoft;
    c[ImGuiCol_ResizeGripActive] = kAccent;
    c[ImGuiCol_InputTextCursor] = kText;
    c[ImGuiCol_Tab] = kChrome;
    c[ImGuiCol_TabHovered] = kRaisedHover;
    c[ImGuiCol_TabSelected] = kPanel;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = kChrome;
    c[ImGuiCol_TabDimmedSelected] = kPanel;
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview] = kAccentHover;
    c[ImGuiCol_DockingEmptyBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PlotLines] = kAccent;
    c[ImGuiCol_PlotLinesHovered] = kWarning;
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_PlotHistogramHovered] = kWarning;
    c[ImGuiCol_TableHeaderBg] = kChrome;
    c[ImGuiCol_TableBorderStrong] = kHairline;
    c[ImGuiCol_TableBorderLight] = rgb(44, 48, 54);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
    c[ImGuiCol_TextLink] = kAccent;
    c[ImGuiCol_TextSelectedBg] = kAccentHover;
    c[ImGuiCol_TreeLines] = kHairline;
    c[ImGuiCol_DragDropTarget] = kAccent;
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_NavWindowingHighlight] = rgb(255, 255, 255, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0, 0, 0, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.45f);

    s.FontSizeBase = kBaseFontSize;
    s.ScaleAllSizes(dpiScale);
    s.FontScaleDpi = dpiScale;
}

void applyPlotTheme() {
    if (ImPlot::GetCurrentContext() == nullptr) {
        return;
    }
    ImPlotStyle& p = ImPlot::GetStyle();
    p.Colors[ImPlotCol_FrameBg] = rgb(0, 0, 0, 0);
    p.Colors[ImPlotCol_PlotBg] = kWell;
    p.Colors[ImPlotCol_PlotBorder] = rgb(0, 0, 0, 0);
    p.Colors[ImPlotCol_LegendBg] = rgb(32, 35, 40, 0.85f);
    p.Colors[ImPlotCol_LegendBorder] = kHairline;
    p.Colors[ImPlotCol_LegendText] = kText;
    p.Colors[ImPlotCol_TitleText] = kText;
    p.Colors[ImPlotCol_InlayText] = kText;
    p.Colors[ImPlotCol_AxisText] = kTextDim;
    p.Colors[ImPlotCol_AxisGrid] = rgb(255, 255, 255, 0.06f);
    p.Colors[ImPlotCol_AxisTick] = rgb(255, 255, 255, 0.10f);
    p.Colormap = ImPlot::AddColormap("CrowdSim", kSeries, static_cast<int>(std::size(kSeries)));
    p.PlotPadding = ImVec2(6, 6);
    p.LegendPadding = ImVec2(6, 4);
    p.LegendInnerPadding = ImVec2(4, 2);
}

ThemeFonts loadFonts() {
    ThemeFonts f;
    f.body = tryLoad({"C:/Windows/Fonts/segoeui.ttf", "/System/Library/Fonts/SFNS.ttf",
                      "/System/Library/Fonts/Supplemental/Arial.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                      "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"},
                     kBaseFontSize);
    f.strong = tryLoad({"C:/Windows/Fonts/seguisb.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                        "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
                        "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
                       kBaseFontSize);
    f.mono = tryLoad({"C:/Windows/Fonts/CascadiaMono.ttf", "C:/Windows/Fonts/consola.ttf",
                      "/System/Library/Fonts/SFNSMono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                      "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf"},
                     kBaseFontSize);
    if (f.body == nullptr) {
        f.body = ImGui::GetIO().Fonts->AddFontDefault();
    }
    ImGui::GetIO().FontDefault = f.body;
    if (f.strong == nullptr) {
        f.strong = f.body;
    }
    if (f.mono == nullptr) {
        f.mono = f.body;
    }
    return f;
}

std::string humanize(std::string_view key) {
    std::string out;
    for (std::size_t i = 0; i < key.size(); ++i) {
        const char ch = key[i];
        if (ch == '_') {
            out += ' ';
            continue;
        }
        const bool upper = std::isupper(static_cast<unsigned char>(ch)) != 0;
        if (upper && i > 0 && out.back() != ' ') {
            out += ' ';
        }
        out += i == 0 ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))
                      : (upper && i + 1 < key.size() ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
                                                     : ch);
    }
    return out;
}

float propertyColumn() {
    const float w = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.0f;
    const float em = ImGui::GetFontSize();
    return std::clamp(w * 0.40f, em * 6.0f, em * 11.0f);
}

void propertyLabel(const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float valueX = style.WindowPadding.x + propertyColumn();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float clipRight = ImGui::GetWindowPos().x + valueX - style.ItemInnerSpacing.x;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    const bool clipped = ImGui::CalcTextSize(label).x > clipRight - start.x;
    if (clipped) {
        ImGui::PushClipRect(start, ImVec2(clipRight, start.y + ImGui::GetFrameHeight()), true);
    }
    ImGui::TextUnformatted(label);
    if (clipped) {
        ImGui::PopClipRect();
        ImGui::SetItemTooltip("%s", label);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(valueX);
    ImGui::SetNextItemWidth(-1.0f);
}

bool beginReadout(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) {
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, propertyColumn() - ImGui::GetStyle().CellPadding.x);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void readout(const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(kTextDim, "%s", label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

void endReadout() {
    ImGui::EndTable();
}

bool sectionToggle(const char* title, ImFont* strong, bool defaultOpen) {
    ImGui::PushFont(strong, 0.0f);
    const bool open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                   (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    ImGui::PopFont();
    return open;
}

void sectionHeading(const char* text, const ThemeFonts& fonts) {
    ImGui::Spacing();
    ImGui::PushFont(fonts.strong, 0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 2.0f), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 2.0f),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
}

}  // namespace crowdsim::ui
